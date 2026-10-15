// gpu_gas_projection_test.cpp — 気体の圧力の投影(T-0531。CPU の模型は sim/gas_projection・07 §2.6)の GPU 版を CPU とビット一致で確かめ、測る。
// 今は解く部分(GpuImplicit::RecordSolve。一般の右辺の V サイクル・止める判定は式の誤差〔残差 × D〕が許容差 mg 以下)だけ:
// CPU の模型(StepGasProjection)が実際に解いた系(各刻み・外側の反復の解き直しも)を見る口(solveObserver)で受け、
// 同じ系(CPU の BuildImplicitGrid が作った段・右辺・初めの近似)を GPU で解き直して、解いた値と V の回数がビット一致するかを確かめる。
// 式を作る段・当てる段・外側の反復の GPU 版はまだ(T-0531 の引き継ぎ)。
// 場面: 閉じた箱(一辺 n。重さあり)の 熱い泡(+30 K・同じ圧力)/ 扉(x の半分が 2% 高い圧力・同じ温度)/ 強い爆発(真ん中 1 セル 5 倍の圧力・1500 K)。
// 音速は最速(0)と 340 m/s。外側の反復は既定(上限 4・100 mg。ADR-0234)。
//
// 引数: [--edge n](既定 16)[--ticks n](既定 3)[--measure](GPU の時間を表に出す)[--breakdown](解きを固定費・V・止めた後の回に分ける)
//       [--stages](場面ごとに 1 回、V 1 回の段ごとの時間。T-0587)[--global-tail](ImTail を前の形で。T-0587)
// [--inner-tolerance mg][--inner-cycles n](内側の止め方を変えて比べる。ADR-0254)と GPU の試験の共通の引数(--warp・--queue・--no-gbv)。
// ctest は一辺 16・3 刻み(gpu_gas_projection)。計測は `job.py run -Preset release -Exe gpu_gas_projection_test -- --no-gbv --measure --edge 32`。
// 合わなければ失敗(どの場面・何回目の解き・何番のセルか)。
#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "sim/gas_projection.h"
#include "sim/gas_reference.h"
#include "sim/gpu_implicit.h"
#include "sim/world_physics.h"
#include "standard_world_physics.h"

using namespace bicameral;

namespace {

    // --- 試験の空気(gas_projection_test と同じ。N2 79% + O2 21% と煙の印)---
    constexpr uint32_t SPECIES_N2 = 0;
    constexpr uint32_t SPECIES_SMOKE = 2;
    constexpr std::array<uint32_t, 3> AIR_FRACTIONS_Q32 = {3393024163u, 901943132u, 0u};
    constexpr uint32_t AIR_TEMPERATURE = 293150;                   // mK
    constexpr uint64_t SURFACE_PRESSURE = 101325ull * 1000000ull;  // µPa
    constexpr uint32_t REAL_SOUND = 340000;                        // mm/s
    constexpr uint32_t BUBBLE_HEAT = 30000;                        // mK
    constexpr uint32_t BLAST_TEMPERATURE = 1500000;                // mK

    sim::GasConfig MakeAirConfig(uint32_t edge) {
        sim::GasConfig config;
        config.sizeX = edge;
        config.sizeY = edge;
        config.sizeZ = edge;
        config.speciesCount = 3;
        config.species[0] = {.molarMass = 28013, .heatCapacity = 20810, .formationEnergy = 0};  // N2
        config.species[1] = {.molarMass = 31999, .heatCapacity = 21064, .formationEnergy = 0};  // O2
        config.species[2] = {.molarMass = 28013, .heatCapacity = 20810, .formationEnergy = 0};  // 煙の印
        sim::ApplyWorldPhysics(sim::StandardWorldPhysics(), config);

        return config;
    }

    sim::GasBox MakeAirBox(uint32_t edge) {
        const sim::GasConfig config = MakeAirConfig(edge);
        return sim::MakeGasBox(
            config, sim::MakeHydrostaticReference(config, AIR_FRACTIONS_Q32, AIR_TEMPERATURE, SURFACE_PRESSURE));
    }

    // 同じ圧力のまま温度を変える(物質量を温度に反比例させる)
    void SetTemperature(sim::GasBox& box, uint32_t index, uint32_t temperature, uint64_t pressurePercent) {
        std::array<uint64_t, sim::GAS_MAX_SPECIES> amounts = box.cells[index].amounts;
        for (uint64_t& amount : amounts)
            amount = amount * pressurePercent * AIR_TEMPERATURE / (100 * (uint64_t)temperature);

        box.cells[index] = sim::MakeGasCell(box.config, amounts, temperature);
    }

    enum class Scene : uint8_t { Bubble, Door, Blast };

    constexpr std::array<Scene, 3> SCENES = {Scene::Bubble, Scene::Door, Scene::Blast};
    constexpr std::array<uint32_t, 2> SPEEDS = {0, REAL_SOUND};

    std::string_view SceneName(Scene scene) {
        switch (scene) {
            case Scene::Bubble: return "泡";
            case Scene::Door: return "扉";
            case Scene::Blast: return "爆発";
        }

        return "";
    }

    sim::GasBox MakeScene(Scene scene, uint32_t edge) {
        sim::GasBox box = MakeAirBox(edge);
        const auto center = (int32_t)(edge / 2);
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const auto x = (int32_t)(index % edge);
            const auto y = (int32_t)((index / edge) % edge);
            const auto z = (int32_t)(index / (edge * edge));
            if (scene == Scene::Bubble) {
                const int32_t dx = x - center;
                const int32_t dy = y - center;
                const int32_t dz = z - (center / 2);
                if ((dx * dx) + (dy * dy) + (dz * dz) <= 2)
                    SetTemperature(box, index, AIR_TEMPERATURE + BUBBLE_HEAT, 100);
            } else if (scene == Scene::Door) {
                if (x < center)
                    SetTemperature(box, index, AIR_TEMPERATURE, 102);
            } else if (x == center && y == center && z == center) {
                SetTemperature(box, index, BLAST_TEMPERATURE, 500);
                sim::GasCell& cell = box.cells[index];
                const uint64_t moved = cell.amounts[SPECIES_N2] / 2;
                cell.amounts[SPECIES_N2] -= moved;
                cell.amounts[SPECIES_SMOKE] += moved;
            }
        }

        return box;
    }

    // --- GPU で解き直す ---

    // 計測の時の同じ解きの繰り返し(1 本のリストに続けて積み、最小を採る。解きの間に CPU の模型が走る間 GPU が休んで
    // クロックが下がるので、1 回だけだと数倍遅く出た〔2026-10-15 の 1 回目〕)
    // 6 → 16(2 チャット目: ほかの仕事が GPU に割り込むと 32³・64³ の 1 回が数倍に出た。割り込みは短いので回を増やして最小を採る)
    constexpr uint32_t MEASURE_REPEATS = 16;
    // 計る回の前に同じ解きを計らずに積む回(GPU のクロックを上げる。6 回の最小でも 32³ の固定費が場面ごとに 0.07〜0.21 ms と
    // 3 倍揺れた〔2026-10-15 の 2 チャット目〕)
    constexpr uint32_t MEASURE_WARMUP = 12;

    struct Replay {
        ID3D12Device5* device = nullptr;
        gpu::ImmediateQueue* queue = nullptr;
        gpu::DebugRing* ring = nullptr;
        uint64_t frequency = 0;
        uint32_t toleranceMilligrams = 0;
        sim::ImplicitOptions solver;
        bool measure = false;
        bool breakdown = false;   // 解きの時間を固定費・V・止めた後の回に分ける(T-0531 の 2 チャット目)
        bool stages = false;      // 場面ごとに 1 回、V 1 回の解きの段ごとの時間を出す(T-0587)
        bool staged = false;      // この場面で段ごとの時間を出したか
        bool globalTail = false;  // ImTail を前の形(節の値を全体のメモリに置く)で比べる(T-0587)
        // 内側の解きの止め方を変えて比べる(0 なら GasProjectionOptions の既定。T-0531 の 2 チャット目)
        uint32_t innerToleranceMilligrams = 0;
        uint32_t innerCycles = 0;
        std::optional<sim::GpuImplicit> gpu;
        std::optional<sim::GpuImplicitCost> previous;  // 前の解きの GPU の数(記録の形を選ぶ。T-0154・T-0505)

        // --- 結果 ---
        std::string failure;
        uint32_t solves = 0;
        uint32_t cycles = 0;
        double milliseconds = 0;                        // 前の解きの数から選んだ記録の形(ShapeFrom)
        double defaultMilliseconds = 0;                 // 既定の形(V を上限 64 回まで段ごとに積む)
        double fixedMilliseconds = 0;                   // 分ける時: V を積まない(計画・初めの判定だけ)
        double exactMilliseconds = 0;                   // 分ける時: V を CPU が回した回数だけ積む(止めた後の回が無い)
        std::optional<sim::GpuImplicitCost> firstCost;  // 場面の初めの解きの段の形(表に出す)
    };

    // 1 回の解きを GPU で(repeats 回くり返して積み、最小の時間を返す。値は最後の回)
    std::expected<double, std::string> SolveOnGpu(Replay& replay, const sim::GasPressureSolve& solve,
                                                  const sim::ImplicitOptions& solver,
                                                  const sim::GpuImplicitRecordShape& shape, uint32_t repeats,
                                                  std::vector<int64_t>& values, sim::GpuImplicitCost& cost) {
        if (!replay.gpu) {
            auto created = sim::GpuImplicit::Create(replay.device, solve.grid);
            if (!created)
                return std::unexpected(created.error());

            replay.gpu.emplace(std::move(*created));
            replay.gpu->UseSharedTail(!replay.globalTail);
        }

        sim::GpuImplicit& gpu = *replay.gpu;
        const std::vector<int64_t> thresholds = sim::GasPressureThresholds(solve.grid, replay.toleranceMilligrams);
        ID3D12GraphicsCommandList10* list = replay.queue->Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        replay.ring->RecordBegin(list);
        bool recorded = gpu.RecordUpload(list, solve.grid);
        const uint32_t warmup = repeats > 1 ? MEASURE_WARMUP : 0;
        for (uint32_t round = 0; round < warmup + repeats && recorded; ++round) {
            const bool timed = round >= warmup;
            const uint32_t repeat = round - warmup;
            recorded = gpu.RecordUploadSolve(list, solve.rhs, solve.before, thresholds);
            if (timed)
                gpu.RecordTimestamp(list, 2 * repeat);

            recorded = recorded && gpu.RecordSolve(list, replay.ring->GpuAddress(), solver, shape);
            if (timed)
                gpu.RecordTimestamp(list, (2 * repeat) + 1);
        }

        if (!recorded)
            return std::unexpected("記録できない(系が上限を超えた)");

        gpu.RecordSolutionReadback(list, (uint32_t)solve.after.size());
        gpu.RecordRelease(list);
        replay.ring->RecordReadbackAndReset(list);
        if (!replay.queue->ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        const gpu::DebugRingContents debugOutput = replay.ring->Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debugOutput.assertCount));

        values.resize(solve.after.size());
        const std::vector<uint64_t> stamps = gpu.ReadTimestamps(2 * repeats);
        if (!gpu.ReadSolution(values, cost) || stamps.size() != 2 * repeats)
            return std::unexpected("読み戻せない");

        double best = 0;
        for (uint32_t repeat = 0; repeat < repeats; ++repeat) {
            const double ms = (double)(stamps[(2 * repeat) + 1] - stamps[2 * repeat]) * 1000.0 /
                              (double)replay.frequency;
            best = repeat == 0 ? ms : std::min(best, ms);
        }

        return best;
    }

    // 段ごとの時間(T-0587): V 1 回だけの解きを暖機の後 STAGE_ROUNDS 回積み、Dispatch ごと(バリアの後)のタイムスタンプの差の最小。
    // 1 行に段の順で「種類 段 色: µs」を並べ、V の段の合計と 1 スレッドの段(CycleEnd。同期の床)を出す
    constexpr uint32_t STAGE_ROUNDS = 8;
    constexpr uint32_t STAGE_STRIDE = 96;  // 1 回の解きに打てるタイムスタンプ(MAX_TIMESTAMPS ÷ 回)

    std::string StageLabel(uint32_t label) {
        const uint32_t pass = label & 0xFFu;
        const uint32_t depth = (label >> 8) & 0xFFu;
        const std::string_view name = sim::GpuImplicit::PassName(pass);
        if (name == "Smooth")
            return std::format("S{}{}", depth, (label >> 16) & 1u ? "b" : "r");

        if (name == "Restrict" || name == "Prolong" || name == "Tail")
            return std::format("{}{}", name.substr(0, 1), depth);

        return std::string(name);
    }

    std::expected<void, std::string> StageProfile(Replay& replay, const sim::GasPressureSolve& solve,
                                                  const sim::GpuImplicitRecordShape& shape) {
        sim::GpuImplicit& gpu = *replay.gpu;
        sim::ImplicitOptions one = replay.solver;
        one.cycles = 1;
        const std::vector<int64_t> thresholds = sim::GasPressureThresholds(solve.grid, replay.toleranceMilligrams);
        ID3D12GraphicsCommandList10* list = replay.queue->Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        replay.ring->RecordBegin(list);
        bool recorded = gpu.RecordUpload(list, solve.grid);
        for (uint32_t round = 0; round < MEASURE_WARMUP + STAGE_ROUNDS && recorded; ++round) {
            recorded = gpu.RecordUploadSolve(list, solve.rhs, solve.before, thresholds);
            if (round >= MEASURE_WARMUP)
                gpu.BeginStageStamps(list, (round - MEASURE_WARMUP) * STAGE_STRIDE);

            recorded = recorded && gpu.RecordSolve(list, replay.ring->GpuAddress(), one, shape);
            gpu.EndStageStamps();
        }

        const auto stages = (uint32_t)gpu.StageLabels().size();
        if (!recorded || stages + 1 > STAGE_STRIDE)
            return std::unexpected("段ごとの時間を記録できない");

        gpu.RecordSolutionReadback(list, (uint32_t)solve.after.size());
        gpu.RecordRelease(list);
        replay.ring->RecordReadbackAndReset(list);
        if (!replay.queue->ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        (void)replay.ring->Drain();
        const std::vector<uint64_t> stamps = gpu.ReadTimestamps(STAGE_ROUNDS * STAGE_STRIDE);
        if (stamps.size() < ((STAGE_ROUNDS - 1) * STAGE_STRIDE) + stages + 1)
            return std::unexpected("段ごとの時間を読み戻せない");

        // --- 段ごとの最小 ---
        std::string line;
        double total = 0;
        double floor = 1e9;
        for (uint32_t stage = 0; stage < stages; ++stage) {
            double best = 1e9;
            for (uint32_t round = 0; round < STAGE_ROUNDS; ++round) {
                const size_t at = ((size_t)round * STAGE_STRIDE) + stage;
                best = std::min(best, (double)(stamps[at + 1] - stamps[at]) * 1e6 / (double)replay.frequency);
            }

            const std::string label = StageLabel(gpu.StageLabels()[stage]);
            if (label == "CycleEnd")
                floor = std::min(floor, best);

            total += best;
            line += std::format(" {}:{:.1f}", label, best);
        }

        Log(Channel::Sim, Level::Info, "  段ごと(µs・V 1 回の解き・段 {}・合計 {:.1f}・1 スレッドの段 {:.1f}):{}",
            stages, total, floor, line);

        return {};
    }

    // GPU の解きが CPU と同じか(違えば理由)
    std::optional<std::string> Difference(const sim::GasPressureSolve& solve, const std::vector<int64_t>& values,
                                          const sim::GpuImplicitCost& cost) {
        if (cost.cycles != solve.cycles)
            return std::format("V の回数 GPU {}・CPU {}", cost.cycles, solve.cycles);

        const auto mismatch = std::ranges::mismatch(values, solve.after);
        if (mismatch.in1 == values.end())
            return std::nullopt;

        const auto cell = (size_t)(mismatch.in1 - values.begin());
        return std::format("セル {} の圧力 GPU {}・CPU {}(V {})", cell, *mismatch.in1, *mismatch.in2, solve.cycles);
    }

    // 解きの時間を分ける: V を積まない形(固定費)と、V を CPU が回した回数だけ積む形(止めた後の回の費用が無い)。
    // 後者は回数を前もって知っている時だけ作れる形(比べる用)。値は CPU と比べる
    bool Breakdown(Replay& replay, const sim::GasPressureSolve& solve) {
        const sim::GpuImplicitRecordShape shape = replay.previous ? sim::GpuImplicit::ShapeFrom(*replay.previous)
                                                                  : sim::GpuImplicitRecordShape{};
        sim::ImplicitOptions fixed = replay.solver;
        fixed.cycles = 0;
        sim::ImplicitOptions exact = replay.solver;
        exact.cycles = solve.cycles;
        std::vector<int64_t> values;
        sim::GpuImplicitCost cost;
        const auto fixedMs = SolveOnGpu(replay, solve, fixed, shape, MEASURE_REPEATS, values, cost);
        const auto exactMs = SolveOnGpu(replay, solve, exact, shape, MEASURE_REPEATS, values, cost);
        std::optional<std::string> difference = exactMs ? Difference(solve, values, cost) : exactMs.error();
        if (!fixedMs)
            difference = fixedMs.error();

        if (difference) {
            replay.failure = std::format("解き {}(回数だけ積む形): {}", replay.solves, *difference);
            return false;
        }

        replay.fixedMilliseconds += *fixedMs;
        replay.exactMilliseconds += *exactMs;

        return true;
    }

    // 既定の形と、前の解きの数から選んだ形(計測の時だけ)の両方で解き、どちらも CPU と比べる
    void CompareSolve(Replay& replay, const sim::GasPressureSolve& solve) {
        if (!replay.failure.empty())
            return;

        const uint32_t repeats = replay.measure ? MEASURE_REPEATS : 1;
        std::vector<int64_t> values;
        sim::GpuImplicitCost cost;
        const auto solved = SolveOnGpu(replay, solve, replay.solver, {}, repeats, values, cost);
        std::optional<std::string> difference = solved ? Difference(solve, values, cost) : solved.error();
        if (difference) {
            replay.failure = std::format("解き {}(既定の形): {}", replay.solves, *difference);
            return;
        }

        double shapedMs = *solved;
        if (replay.measure && replay.previous) {
            const auto shaped = SolveOnGpu(replay, solve, replay.solver, sim::GpuImplicit::ShapeFrom(*replay.previous),
                                           repeats, values, cost);
            difference = shaped ? Difference(solve, values, cost) : shaped.error();
            if (difference) {
                replay.failure = std::format("解き {}(前の数からの形): {}", replay.solves, *difference);
                return;
            }

            shapedMs = *shaped;
        }

        if (replay.breakdown && !Breakdown(replay, solve))
            return;

        if (replay.stages && !replay.staged && replay.previous && solve.cycles > 0) {
            replay.staged = true;
            if (auto profiled = StageProfile(replay, solve, sim::GpuImplicit::ShapeFrom(*replay.previous)); !profiled) {
                replay.failure = std::format("解き {}(段ごと): {}", replay.solves, profiled.error());
                return;
            }
        }

        if (!replay.firstCost)
            replay.firstCost = cost;

        replay.previous = cost;
        replay.solves += 1;
        replay.cycles += cost.cycles;
        replay.milliseconds += shapedMs;
        replay.defaultMilliseconds += *solved;
    }

    struct SceneResult {
        uint32_t solves = 0;
        uint32_t cycles = 0;
        double milliseconds = 0;         // 2 刻み目からの解きの GPU 時間の合計(前の数からの形)
        double defaultMilliseconds = 0;  // 同じ(既定の形)
        double worstTickMs = 0;          // 2 刻み目からの 1 刻みの解きの時間の最大
        uint32_t measuredTicks = 0;      // 2 刻み目から
        uint32_t outerIterations = 0;
        double fixedMilliseconds = 0;   // 分ける時(2 刻み目から): 固定費の合計
        double exactMilliseconds = 0;   // 同じ: 回数だけ積む形の合計
        uint32_t measuredCycles = 0;    // 2 刻み目からの V の数
        uint32_t measuredSolves = 0;    // 2 刻み目からの解きの数
        uint32_t maxCycles = 0;         // 1 回の解きの V の最大
        uint32_t outerUnconverged = 0;  // 外側の反復が上限で止めた刻み
        int64_t worstMismatch = 0;      // 採った状態の食い違いの最大(mg)
    };

    std::expected<SceneResult, std::string> RunScene(Replay& replay, Scene scene, uint32_t speed, uint32_t edge,
                                                     uint32_t ticks) {
        sim::GasBox box = MakeScene(scene, edge);
        sim::GasProjection state;
        sim::GasProjectionOptions options;
        options.pressure.soundSpeedMmPerS = speed;
        if (replay.innerToleranceMilligrams != 0)
            options.toleranceMilligrams = replay.innerToleranceMilligrams;

        if (replay.innerCycles != 0)
            options.solver.cycles = replay.innerCycles;

        replay.toleranceMilligrams = sim::GasInnerToleranceMilligrams(options);
        replay.solver = options.solver;
        const sim::GasPressureSolveObserver observer = [&replay](const sim::GasPressureSolve& solve) {
            CompareSolve(replay, solve);
        };
        options.solveObserver = &observer;

        SceneResult result;
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            const uint32_t solvesBefore = replay.solves;
            const uint32_t cyclesBefore = replay.cycles;
            const double msBefore = replay.milliseconds;
            const double defaultBefore = replay.defaultMilliseconds;
            const double fixedBefore = replay.fixedMilliseconds;
            const double exactBefore = replay.exactMilliseconds;
            const sim::GasProjectionCost cost = sim::StepGasProjection(box, state, options);
            if (!replay.failure.empty())
                return std::unexpected(std::format("刻み {}: {}", tick, replay.failure));

            result.solves += replay.solves - solvesBefore;
            result.cycles += replay.cycles - cyclesBefore;
            result.outerIterations += cost.outerIterations;
            result.maxCycles = std::max(result.maxCycles, cost.maxCycles);
            result.outerUnconverged += cost.outerUnconverged;
            result.worstMismatch = std::max(result.worstMismatch, cost.worstMismatch);
            if (tick == 0)
                continue;

            const double tickMs = replay.milliseconds - msBefore;
            result.milliseconds += tickMs;
            result.defaultMilliseconds += replay.defaultMilliseconds - defaultBefore;
            result.worstTickMs = std::max(result.worstTickMs, tickMs);
            result.measuredTicks += 1;
            result.fixedMilliseconds += replay.fixedMilliseconds - fixedBefore;
            result.exactMilliseconds += replay.exactMilliseconds - exactBefore;
            result.measuredCycles += replay.cycles - cyclesBefore;
            result.measuredSolves += replay.solves - solvesBefore;
        }

        return result;
    }

    // 分けた時間の行: 解き 1 回の固定費・V 1 回・止めた後の回(前の数からの形 − 回数だけ積む形)と段の形
    void LogBreakdown(const Replay& replay, const SceneResult& result) {
        const auto solves = (double)std::max(result.measuredSolves, 1u);
        const double fixedPerSolve = result.fixedMilliseconds / solves;
        const double perCycle = (result.exactMilliseconds - result.fixedMilliseconds) /
                                (double)std::max(result.measuredCycles, 1u);
        const double skippedPerSolve = (result.milliseconds - result.exactMilliseconds) / solves;
        std::string levels;
        if (replay.firstCost) {
            const sim::GpuImplicitCost& cost = *replay.firstCost;
            for (uint32_t depth = 0; depth < std::min(cost.levelCount, sim::GpuImplicitCost::KEPT_LEVELS); ++depth)
                levels += std::format("{}{}", depth == 0 ? "" : "・", cost.levelNodes[depth]);

            levels += std::format("(段 {}・下りが止まる段 {}・ImTail {})", cost.levelCount, cost.wantedTailDepth,
                                  cost.tailDepth);
        }

        Log(Channel::Sim, Level::Info,
            "  分ける: 固定費 {:.3f} ms/解き・V 1 回 {:.3f} ms・止めた後の回 {:.3f} ms/解き・段の節 {}", fixedPerSolve,
            perCycle, skippedPerSolve, levels);
    }

    std::expected<void, std::string> RunAll(Replay& replay, uint32_t edge, uint32_t ticks, bool measure) {
        for (const Scene scene : SCENES) {
            for (const uint32_t speed : SPEEDS) {
                replay.gpu.reset();
                replay.previous.reset();
                replay.firstCost.reset();
                replay.staged = false;
                const auto result = RunScene(replay, scene, speed, edge, ticks);
                if (!result)
                    return std::unexpected(
                        std::format("{}・音速 {} mm/s: {}", SceneName(scene), speed, result.error()));

                Log(Channel::Sim, Level::Info,
                    "{}³ {}・音速 {}: 解き {}(外側 {})・V {}(1 回の最大 {})が CPU とビット一致。外側が上限で止めた刻み "
                    "{}・"
                    "採った食い違いの最大 {} mg",
                    edge, SceneName(scene), speed == 0 ? "最速" : "340 m/s", result->solves, result->outerIterations,
                    result->cycles, result->maxCycles, result->outerUnconverged, result->worstMismatch);
                if (!measure || result->measuredTicks == 0)
                    continue;

                // 表の行: | 一辺 | 場面 | 音速 | 刻み | 解き/刻み | V/刻み | 解きの GPU ms/刻み 前の数からの形(平均 / 最大)| 既定の形(平均)|
                Log(Channel::Sim, Level::Info, "| {}³ | {} | {} | {} | {:.2f} | {:.2f} | {:.3f} / {:.3f} | {:.3f} |",
                    edge, SceneName(scene), speed == 0 ? "最速" : "340 m/s", ticks, (double)result->solves / ticks,
                    (double)result->cycles / ticks, result->milliseconds / result->measuredTicks, result->worstTickMs,
                    result->defaultMilliseconds / result->measuredTicks);
                if (replay.breakdown)
                    LogBreakdown(replay, *result);
            }
        }

        return {};
    }

    int Run(std::span<char*> arguments) {
        std::vector<char*> rest;
        uint32_t edge = 16;
        uint32_t ticks = 3;
        bool measure = false;
        bool breakdown = false;
        bool stages = false;
        bool globalTail = false;
        uint32_t innerTolerance = 0;
        uint32_t innerCycles = 0;
        for (size_t index = 0; index < arguments.size(); ++index) {
            const std::string_view argument = arguments[index];
            if (argument == "--edge" && index + 1 < arguments.size())
                edge = (uint32_t)std::stoul(arguments[++index]);
            else if (argument == "--ticks" && index + 1 < arguments.size())
                ticks = (uint32_t)std::stoul(arguments[++index]);
            else if (argument == "--measure")
                measure = true;
            else if (argument == "--breakdown")
                measure = breakdown = true;
            else if (argument == "--stages")
                measure = stages = true;
            else if (argument == "--global-tail")
                globalTail = true;
            else if (argument == "--inner-tolerance" && index + 1 < arguments.size())
                innerTolerance = (uint32_t)std::stoul(arguments[++index]);
            else if (argument == "--inner-cycles" && index + 1 < arguments.size())
                innerCycles = (uint32_t)std::stoul(arguments[++index]);
            else
                rest.push_back(arguments[index]);
        }

        const auto options = test::ParseGpuTestOptions(rest);
        if (!options || edge < 4 || ticks == 0) {
            Log(Channel::Gpu, Level::Error,
                "使い方: gpu_gas_projection_test [--warp] [--queue direct|compute] [--no-gbv] [--edge N] [--ticks N] "
                "[--measure] [--breakdown] [--stages] [--global-tail] [--inner-tolerance mg] [--inner-cycles n]");
            return 2;
        }

        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!device) {
            Log(Channel::Gpu, Level::Error, "gpu_gas_projection_test: FAILED(デバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        uint64_t frequency = 0;
        if (!queue || !ring || FAILED(queue->Native()->GetTimestampFrequency(&frequency))) {
            Log(Channel::Gpu, Level::Error, "gpu_gas_projection_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        Replay replay{.device = device->Get(),
                      .queue = &*queue,
                      .ring = &*ring,
                      .frequency = frequency,
                      .measure = measure,
                      .breakdown = breakdown,
                      .stages = stages,
                      .globalTail = globalTail,
                      .innerToleranceMilligrams = innerTolerance,
                      .innerCycles = innerCycles};
        if (auto ran = RunAll(replay, edge, ticks, measure); !ran) {
            Log(Channel::Gpu, Level::Error, "gpu_gas_projection_test: FAILED({})", ran.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_gas_projection_test"))
            return 1;

        Log(Channel::Gpu, Level::Info, "gpu_gas_projection_test: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
