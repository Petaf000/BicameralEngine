// gpu_gas_projection_test.cpp — 気体の圧力の投影(T-0531。CPU の模型は sim/gas_projection・07 §2.6)の GPU 版を CPU とビット一致で確かめ、測る。
// 今は解く部分(GpuImplicit::RecordSolve。一般の右辺の V サイクル・止める判定は式の誤差〔残差 × D〕が許容差 mg 以下)だけ:
// CPU の模型(StepGasProjection)が実際に解いた系(各刻み・外側の反復の解き直しも)を見る口(solveObserver)で受け、
// 同じ系(CPU の BuildImplicitGrid が作った段・右辺・初めの近似)を GPU で解き直して、解いた値と V の回数がビット一致するかを確かめる。
// 式を作る段・当てる段・外側の反復の GPU 版はまだ(T-0531 の引き継ぎ)。
// 場面: 閉じた箱(一辺 n。重さあり)の 熱い泡(+30 K・同じ圧力)/ 扉(x の半分が 2% 高い圧力・同じ温度)/ 強い爆発(真ん中 1 セル 5 倍の圧力・1500 K)。
//       セルの空き(T-0667): 孔のある壁(塞がった板に 1/2 と 1/256 の孔・扉と同じ圧力の差)/ 細孔の板(1 層を全部 1/256・下が 2% 高い)。
// 音速は最速(0)と 340 m/s。外側の反復は既定(上限 4・100 mg。ADR-0234)。
//
// T-0845(--t0845): 投影の 1 刻み(式を作る・段の重み・V 2 回・当てる・運ぶ)を通しで GPU で回し(GpuGasProjection)、CPU の模型と
//   --check-every 刻みごと(既定 毎刻み)に箱のセルと帳簿をビット一致で比べる。--measure で通しの時間と部分の内訳(暖機の後 16 刻みの最小)。
// T-0915(--t0915): 釣り合う家族の部品の GPU 版(GpuEquilibrium)の上の投影を、CPU の部品の上の道(GasProjectionOptions::equilibrium)と
//   32³ × 5 場面 × --ticks(既定 300)刻み毎刻みビット一致(箱・帳簿・残差の計器)・前の GPU の道(T-0845)の 300 刻みのハッシュ・
//   保存(16³・V 1・2・4)・決定性(16³・2 回 + 1 本のリストに 8 刻み・段ごとの Dispatch 2 段まで)で確かめる。T-0845 の試験も部品の上で回る。
// T-0843(--t0843): 本物の音速(340 m/s。C = V/c²)・V サイクル 2 回ちょうど(fixedCycles。判定を積まない)・外側の反復 0 回(上限 1)で、
//   同じ場面を一辺 32・64(--edge で 1 つだけ)の 3 刻み回し、V 2 回の解き・V を積まない形・計画の表を毎回作り直す形を測る(CPU とビット一致)。
// 引数: [--edge n](既定 16)[--ticks n](既定 3)[--measure](GPU の時間を表に出す)[--breakdown](解きを固定費・V・止めた後の回に分ける)
//       [--stages](場面ごとに 1 回、V 1 回の段ごとの時間。T-0587)[--global-tail](ImTail を前の形で。T-0587)
// [--inner-tolerance mg][--inner-cycles n](内側の止め方を変えて比べる。ADR-0254)と GPU の試験の共通の引数(--warp・--queue・--no-gbv)。
// T-0900(--t0900): 測り M-1。投影の 1 刻みの ns/セル・VRAM の差(作った前後)・V 2/4/8 回の費用を一辺 32・64・128 で(値は比べない)。
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
#include "gas_projection_scenes.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_memory.h"
#include "gpu_test_options.h"
#include "sim/gas_projection.h"
#include "sim/gas_reference.h"
#include "sim/gpu_gas_projection.h"
#include "sim/gpu_implicit.h"
#include "sim/world_physics.h"
#include "standard_world_physics.h"

using namespace bicameral;

namespace {

    using namespace gas_scenes;  // 試験の場面(gas_projection_scenes.h。T-0853 で移した)

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
        bool t0843 = false;         // V 2 回固定・外側 0・音速 340 だけ(T-0843)
        bool replan = false;        // 解きのたびに計画の表を作り直す(T-0843。系の形が刻みごとに変わる時の固定費)
        bool shapeFromCost = true;  // T-0845 の計測: V の記録の形を 1 刻み目の数から選ぶ(--default-shape で既定の形)
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
        double planMilliseconds = 0;                    // T-0843: 同じ解きを計画の表を毎回作り直して
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

            if (replay.replan)
                gpu.InvalidatePlan();

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

    // T-0843: V 2 回固定(判定を積まない形)で解いて CPU(fixedCycles)と比べ、計測の時は V を積まない形と、
    // 計画の表を解きのたびに作り直す形も測る
    void CompareFixedSolve(Replay& replay, const sim::GasPressureSolve& solve) {
        sim::GpuImplicitRecordShape shape = replay.previous ? sim::GpuImplicit::ShapeFrom(*replay.previous)
                                                            : sim::GpuImplicitRecordShape{};
        shape.fixedCycles = true;
        const uint32_t repeats = replay.measure ? MEASURE_REPEATS : 1;
        std::vector<int64_t> values;
        sim::GpuImplicitCost cost;
        const auto solved = SolveOnGpu(replay, solve, replay.solver, shape, repeats, values, cost);
        std::optional<std::string> difference = solved ? Difference(solve, values, cost) : solved.error();
        if (difference) {
            replay.failure = std::format("解き {}(V 固定): {}", replay.solves, *difference);
            return;
        }

        if (replay.measure) {
            sim::ImplicitOptions none = replay.solver;
            none.cycles = 0;
            std::vector<int64_t> scratch;
            sim::GpuImplicitCost scratchCost;
            const auto base = SolveOnGpu(replay, solve, none, shape, repeats, scratch, scratchCost);
            replay.replan = true;
            const auto planned = SolveOnGpu(replay, solve, replay.solver, shape, repeats, scratch, scratchCost);
            replay.replan = false;
            if (!base || !planned) {
                replay.failure = std::format("解き {}(V 固定の分け): {}", replay.solves,
                                             base ? planned.error() : base.error());
                return;
            }

            replay.fixedMilliseconds += *base;
            replay.planMilliseconds += *planned;
        }

        if (replay.stages && !replay.staged && replay.previous) {
            replay.staged = true;
            if (auto profiled = StageProfile(replay, solve, shape); !profiled) {
                replay.failure = std::format("解き {}(段ごと): {}", replay.solves, profiled.error());
                return;
            }
        }

        if (!replay.firstCost)
            replay.firstCost = cost;

        replay.previous = cost;
        replay.solves += 1;
        replay.cycles += cost.cycles;
        replay.milliseconds += *solved;
        replay.defaultMilliseconds += *solved;
    }

    // 既定の形と、前の解きの数から選んだ形(計測の時だけ)の両方で解き、どちらも CPU と比べる
    void CompareSolve(Replay& replay, const sim::GasPressureSolve& solve) {
        if (!replay.failure.empty())
            return;

        if (replay.t0843) {
            CompareFixedSolve(replay, solve);
            return;
        }

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
        double planMilliseconds = 0;    // T-0843(2 刻み目から): 計画の表を毎回作り直す形の合計
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

        if (replay.t0843) {  // T-0842 の版 1 の外側 0(音速は呼ぶ側が 340 m/s だけを渡す)
            options.fixedCycles = true;
            options.solver.cycles = 2;
            options.outerIterations = 1;
        }

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
            const double planBefore = replay.planMilliseconds;
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
            result.planMilliseconds += replay.planMilliseconds - planBefore;
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

    // T-0843 の表の行: | 一辺 | 場面 | V 2 回の解き ms/刻み | ns/セル | V を積まない形 ms | 計画の表を作り直す分 ms | 段の数 |
    void LogFixed(const Replay& replay, uint32_t edge, Scene scene, const SceneResult& result) {
        const auto ticks = (double)std::max(result.measuredTicks, 1u);
        const double solveMs = result.milliseconds / ticks;
        const double cells = (double)edge * edge * edge;
        const uint32_t levels = replay.firstCost ? replay.firstCost->levelCount : 0;
        Log(Channel::Sim, Level::Info,
            "| T-0843 | {}³ | {} | V {:.1f}/刻み | {:.3f} ms | {:.2f} ns/セル | V なし {:.3f} ms | 計画 +{:.3f} ms | "
            "段 {} |",
            edge, SceneName(scene), (double)result.measuredCycles / ticks, solveMs, solveMs * 1e6 / cells,
            result.fixedMilliseconds / ticks, (result.planMilliseconds - result.milliseconds) / ticks, levels);
    }

    std::expected<void, std::string> RunAll(Replay& replay, uint32_t edge, uint32_t ticks, bool measure) {
        constexpr std::array<uint32_t, 1> REAL_ONLY = {REAL_SOUND};
        const std::span<const uint32_t> speeds = replay.t0843 ? std::span<const uint32_t>(REAL_ONLY)
                                                              : std::span<const uint32_t>(SPEEDS);
        for (const Scene scene : SCENES) {
            for (const uint32_t speed : speeds) {
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

                if (replay.t0843) {
                    LogFixed(replay, edge, scene, *result);
                    continue;
                }

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

    // --- T-0845(--t0845): 投影の 1 刻みを通しで GPU に(式を作る・段の重み・V 2 回・当てる・運ぶ)。CPU の模型と毎刻みビット一致 ---

    // 段の形(段の数・段ごとの節・行・隣・親)が同じか(値は比べない。GPU は値を毎刻み作り直し、形が変わった刻みだけ写し直す)
    bool SameShape(const sim::ImplicitGrid& a, const sim::ImplicitGrid& b) {
        if (a.levels.size() != b.levels.size())
            return false;

        for (size_t depth = 0; depth < a.levels.size(); ++depth) {
            const sim::ImplicitGridLevel& left = a.levels[depth];
            const sim::ImplicitGridLevel& right = b.levels[depth];
            if (left.levels.size() != right.levels.size() || left.rowStarts != right.rowStarts ||
                left.neighbors != right.neighbors || left.parents != right.parents)
                return false;
        }

        return true;
    }

    // CPU の模型は釣り合う家族の部品の上の道(T-0853。前の道とビット一致は equilibrium_test)。GPU も部品の上(T-0915)
    // V の回数は世界のデータ(環境の balance_cycles.gas_pressure)から入れる口を通す(T-0955。CPU と GPU が同じ値を読む)
    sim::GasProjectionOptions FullOptions(uint32_t cycles = 2) {
        sim::GasProjectionOptions options;
        options.pressure.soundSpeedMmPerS = REAL_SOUND;
        options.fixedCycles = true;
        options.outerIterations = 1;
        options.equilibrium = true;
        sim::WorldPhysics world;
        world.balanceCycles.gasPressure = cycles;
        sim::ApplyBalanceCycles(world, options);

        return options;
    }

    sim::GpuGasProjectionOptions FullGpuOptions(const sim::GasProjectionOptions& options, bool meter = false) {
        sim::GpuGasProjectionOptions gpuOptions;
        gpuOptions.soundSpeedMmPerS = options.pressure.soundSpeedMmPerS;
        gpuOptions.solver = options.solver;
        gpuOptions.meterResiduals = meter;

        return gpuOptions;
    }

    // 保存: Σ 保存量 − 帳簿 が刻みの初めと同じ(equilibrium_test の Conserved と同じ)
    bool Conserved(const sim::GasBox& box, const sim::GasTotals& before) {
        const sim::GasTotals now = sim::SumGas(box);
        bool ok = now.amounts == before.amounts && now.energy == before.energy + box.ledger.energy;
        for (uint32_t axis = 0; axis < sim::GAS_AXES; ++axis)
            ok = ok && now.momentum[axis] == before.momentum[axis] + box.ledger.momentum[axis];

        return ok;
    }

    // GPU の箱を読み、CPU の箱とセル・帳簿を比べる(合わなければ最初に違うセル)
    std::expected<void, std::string> CompareBoxes(const sim::GpuGasProjection& projection, const sim::GasBox& box) {
        std::vector<sim::GasCell> cells;
        sim::GasLedger ledger;
        if (!projection.Read(cells, ledger))
            return std::unexpected("読み戻せない");

        sim::GasBox gpuBox = box;
        gpuBox.cells = cells;
        if (sim::HashGas(gpuBox) != sim::HashGas(box)) {
            for (size_t index = 0; index < cells.size(); ++index) {
                const sim::GasCell& gpuCell = cells[index];
                const sim::GasCell& cpuCell = box.cells[index];
                if (gpuCell.energy != cpuCell.energy || gpuCell.momentum != cpuCell.momentum ||
                    gpuCell.amounts != cpuCell.amounts) {
                    return std::unexpected(std::format("セル {} が違う(エネルギー GPU {} / CPU {}・運動量 z {} / {})",
                                                       index, gpuCell.energy, cpuCell.energy, gpuCell.momentum[2],
                                                       cpuCell.momentum[2]));
                }
            }
        }

        if (ledger.energy != box.ledger.energy || ledger.momentum != box.ledger.momentum ||
            ledger.amounts != box.ledger.amounts)
            return std::unexpected(
                std::format("帳簿が違う(運動量 z GPU {} / CPU {})", ledger.momentum[2], box.ledger.momentum[2]));

        return {};
    }

    struct FullRun {
        uint32_t ticks = 0;
        uint32_t checked = 0;
        uint32_t reshapes = 0;       // 段の形が変わって写し直した刻み
        uint32_t levels = 0;         // 刻み 0 の段の数
        uint32_t lists = 0;          // 投げたコマンドリストの数
        uint64_t hash = 0;           // 最後の刻みの CPU の箱のハッシュ(= GPU)
        uint64_t worstResidual = 0;  // 部品の残差の計器の最大(mg。GPU = CPU)
        bool conserved = true;       // CPU の箱で毎刻み 保存量 − 帳簿 が一定(GPU は CPU とビット一致)
        uint64_t deviceBytes = 0;    // VRAM(気体の箱 + 部品)
    };

    // 回し方(T-0915): V の回数・1 本のリストに積む刻みの数・V の記録の形(段ごとの Dispatch の段。0 = 既定)。値はどれでも同じはず
    struct FullVariant {
        uint32_t cycles = 2;
        uint32_t ticksPerList = 1;
        uint32_t dispatchLevels = 0;
        bool meter = true;  // 部品の残差の計器を積み、比べる時に CPU の計器と比べる
    };

    // CPU の模型と GPU を 1 刻みずつ進め、リストを投げるたび(checkEvery 刻みごとと最後)に箱と残差の計器を比べる。
    // 段の形が変わる刻みの前でリストを切る(形を写し直すのはリストの初めだけ)
    class FullScene {
    public:
        FullScene(Replay& replay, Scene scene, uint32_t edge, uint32_t ticks, uint32_t checkEvery,
                  const FullVariant& variant)
            : m_replay(replay),
              m_box(MakeScene(scene, edge)),
              m_ticks(ticks),
              m_checkEvery(checkEvery),
              m_variant(variant),
              m_options(FullOptions(variant.cycles)) {
            m_totals = sim::SumGas(m_box);
            m_observer = [this](const sim::GasPressureSolve& solve) {
                m_grid = solve.grid;
            };
            m_options.solveObserver = &m_observer;
        }

        std::expected<FullRun, std::string> Run() {
            for (uint32_t tick = 0; tick < m_ticks; ++tick) {
                if (auto stepped = Step(tick); !stepped)
                    return std::unexpected(std::format("刻み {}: {}", tick, stepped.error()));
            }

            m_run.hash = sim::HashGas(m_box);

            return m_run;
        }

    private:
        std::expected<void, std::string> Step(uint32_t tick) {
            const sim::GasBox start = m_box;
            const sim::GasProjectionCost cost = sim::StepGasProjection(m_box, m_state, m_options);
            if (!m_grid)
                return std::unexpected("CPU の模型が解かなかった");

            m_lastResidual = cost.worstResidual;
            m_run.worstResidual = std::max(m_run.worstResidual, cost.worstResidual);
            m_run.conserved = m_run.conserved && Conserved(m_box, m_totals);
            const bool reshape = m_projection && !SameShape(*m_shape, *m_grid);
            if (reshape && m_list != nullptr) {
                if (auto flushed = Flush(false); !flushed)
                    return flushed;
            }

            if (auto begun = Begin(start, reshape); !begun)
                return begun;

            m_grid.reset();
            if (!m_projection->RecordStep(m_list, m_replay.ring->GpuAddress()))
                return std::unexpected("記録できない(段が上限を超えた)");

            m_run.ticks += 1;
            m_pending += 1;
            const bool check = (tick + 1) % m_checkEvery == 0 || tick + 1 == m_ticks;
            if (check || m_pending >= m_variant.ticksPerList)
                return Flush(check);

            return {};
        }

        // リストを始める(最初は GPU を作って写す。形が変わった刻みは写し直す)
        std::expected<void, std::string> Begin(const sim::GasBox& start, bool reshape) {
            if (m_list != nullptr)
                return {};

            m_list = m_replay.queue->Begin();
            if (m_list == nullptr)
                return std::unexpected("コマンドリストを始められない");

            m_replay.ring->RecordBegin(m_list);
            if (!m_projection) {
                auto created = sim::GpuGasProjection::Create(m_replay.device, start, *m_grid,
                                                             FullGpuOptions(m_options, m_variant.meter));
                if (!created)
                    return std::unexpected(created.error());

                m_projection.emplace(std::move(*created));
                if (m_variant.dispatchLevels != 0)
                    m_projection->SetSolveShape({.dispatchLevels = m_variant.dispatchLevels});

                m_run.levels = (uint32_t)m_grid->levels.size();
                m_run.deviceBytes = m_projection->DeviceBytes();
                m_shape = *m_grid;

                return m_projection->RecordUpload(m_list, *m_grid) ? std::expected<void, std::string>{}
                                                                   : std::unexpected("写せない");
            }

            const bool recorded = m_projection->RecordBegin(m_list, reshape ? &*m_grid : nullptr);
            if (reshape) {
                m_shape = *m_grid;
                m_run.reshapes += 1;
            }

            return recorded ? std::expected<void, std::string>{} : std::unexpected("段の形を写せない");
        }

        // リストを投げて待つ。check なら箱と残差の計器を CPU と比べる
        std::expected<void, std::string> Flush(bool check) {
            if (check) {
                m_projection->RecordReadback(m_list);
                if (m_variant.meter)
                    m_projection->Equilibrium().RecordResidualReadback(m_list);
            }

            m_projection->RecordEnd(m_list);
            m_replay.ring->RecordReadbackAndReset(m_list);
            m_list = nullptr;
            m_pending = 0;
            m_run.lists += 1;
            if (!m_replay.queue->ExecuteAndWait())
                return std::unexpected("GPU での実行に失敗");

            const gpu::DebugRingContents debugOutput = m_replay.ring->Drain();
            if (debugOutput.assertCount > 0)
                return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debugOutput.assertCount));

            if (!check)
                return {};

            if (auto compared = CompareBoxes(*m_projection, m_box); !compared)
                return compared;

            // 残差の計器: GPU の語はリストの最後の解きの最大(CPU はその刻みの cost)
            uint64_t worst = 0;
            if (m_variant.meter) {
                if (!m_projection->Equilibrium().ReadWorstResidual(worst))
                    return std::unexpected("残差の計器を読めない");

                if (worst != m_lastResidual)
                    return std::unexpected(std::format("残差の計器 GPU {}・CPU {} mg", worst, m_lastResidual));
            }

            m_run.checked += 1;

            return {};
        }

        Replay& m_replay;
        sim::GasBox m_box;
        uint32_t m_ticks = 0;
        uint32_t m_checkEvery = 1;
        FullVariant m_variant;
        sim::GasProjectionOptions m_options;
        sim::GasTotals m_totals;
        sim::GasProjection m_state;
        sim::GasPressureSolveObserver m_observer;
        std::optional<sim::ImplicitGrid> m_grid;   // CPU の模型がこの刻みに解いた系
        std::optional<sim::ImplicitGrid> m_shape;  // GPU に写してある段の形
        std::optional<sim::GpuGasProjection> m_projection;
        ID3D12GraphicsCommandList10* m_list = nullptr;
        uint32_t m_pending = 0;       // 開いているリストに積んだ刻み
        uint64_t m_lastResidual = 0;  // 最後の刻みの CPU の残差の計器(mg)
        FullRun m_run;
    };

    std::expected<FullRun, std::string> RunFullScene(Replay& replay, Scene scene, uint32_t edge, uint32_t ticks,
                                                     uint32_t checkEvery, const FullVariant& variant = {}) {
        FullScene run(replay, scene, edge, ticks, checkEvery, variant);

        return run.Run();
    }

    // 時間: 刻み 0 の段の形のまま、暖機 MEASURE_WARMUP 刻みの後 MEASURE_REPEATS 刻みを 1 本のリストに積み、部分ごとの最小(ms)と通しの最小
    struct FullTiming {
        std::array<double, (size_t)sim::GpuGasProjectionPart::Count> parts{};
        double total = 0;
        double median = 0;
        uint64_t vramBytes = 0;         // 投影を作った前後のプロセスの VRAM の差(T-0900。桁を知る値)
        uint32_t levels = 0;            // 刻み 0 の段の数
        uint64_t deviceBytes = 0;       // VRAM(気体の箱 + 部品)
        uint64_t equilibriumBytes = 0;  // そのうち部品
    };

    std::expected<FullTiming, std::string> MeasureFullScene(Replay& replay, Scene scene, uint32_t edge,
                                                            sim::GasProjectionOptions options = FullOptions()) {
        const sim::GasBox box = MakeScene(scene, edge);
        sim::GasBox copy = box;
        sim::GasProjection state;
        std::optional<sim::ImplicitGrid> grid;
        const sim::GasPressureSolveObserver observer = [&grid](const sim::GasPressureSolve& solve) {
            grid = solve.grid;
        };
        options.solveObserver = &observer;
        sim::StepGasProjection(copy, state, options);
        if (!grid)
            return std::unexpected("CPU の模型が解かなかった");

        const uint64_t vramBefore = test::LocalVideoMemoryUsage(replay.device);
        auto projection = sim::GpuGasProjection::Create(replay.device, box, *grid, FullGpuOptions(options));
        const uint64_t vramAfter = test::LocalVideoMemoryUsage(replay.device);
        const auto levels = (uint32_t)grid->levels.size();
        if (!projection || !projection->EnableTiming(replay.device))
            return std::unexpected(projection ? "タイムスタンプを作れない" : projection.error());

        // --- 1 刻み目: 写して 1 刻み、V の数と段の形を読む(次の刻みからの記録の形を選ぶ。T-0843 と同じ ShapeFrom)---
        ID3D12GraphicsCommandList10* list = replay.queue->Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        replay.ring->RecordBegin(list);
        bool recorded = projection->RecordUpload(list, *grid) &&
                        projection->RecordStep(list, replay.ring->GpuAddress());
        projection->RecordCostReadback(list);
        projection->RecordEnd(list);
        replay.ring->RecordReadbackAndReset(list);
        sim::GpuImplicitCost cost;
        if (!recorded || !replay.queue->ExecuteAndWait() || !projection->ReadCost(cost))
            return std::unexpected("1 刻み目の記録か実行に失敗");

        projection->ReleaseStaging();  // 形の写しの元は写した後は要らない(T-0918。形が変わる刻みで作り直す)
        if (replay.shapeFromCost)
            projection->SetSolveShape(sim::GpuImplicit::ShapeFrom(cost));

        list = replay.queue->Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        replay.ring->RecordBegin(list);
        recorded = projection->RecordBegin(list, nullptr);
        for (uint32_t round = 0; round < MEASURE_WARMUP + MEASURE_REPEATS && recorded; ++round)
            recorded = projection->RecordStep(list, replay.ring->GpuAddress(), round >= MEASURE_WARMUP);

        projection->RecordTimingReadback(list);
        projection->RecordEnd(list);
        replay.ring->RecordReadbackAndReset(list);
        if (!recorded || !replay.queue->ExecuteAndWait())
            return std::unexpected("記録か実行に失敗");

        const auto steps = projection->ReadTiming();
        const uint64_t deviceBytes = projection->DeviceBytes();
        const uint64_t equilibriumBytes = projection->Equilibrium().DeviceBytes();
        if (steps.size() != MEASURE_REPEATS)
            return std::unexpected("時間を読めない");

        FullTiming timing{.deviceBytes = deviceBytes, .equilibriumBytes = equilibriumBytes};
        timing.vramBytes = test::VideoMemoryGrowth(vramBefore, vramAfter);
        timing.levels = levels;
        std::vector<double> totals;
        const auto toMs = [&replay](uint64_t ticks) {
            return (double)ticks * 1000.0 / (double)replay.frequency;
        };
        for (size_t step = 0; step < steps.size(); ++step) {
            double total = 0;
            for (size_t part = 0; part < timing.parts.size(); ++part) {
                const double ms = toMs(steps[step][part]);
                timing.parts[part] = step == 0 ? ms : std::min(timing.parts[part], ms);
                total += ms;
            }

            totals.push_back(total);
        }

        std::ranges::sort(totals);
        timing.total = totals.front();
        timing.median = totals[totals.size() / 2];

        return timing;
    }

    std::expected<void, std::string> RunFull(Replay& replay, uint32_t edge, uint32_t ticks, uint32_t checkEvery,
                                             bool measure) {
        const double cells = (double)edge * edge * edge;
        for (const Scene scene : SCENES) {
            if (ticks > 0) {
                const auto run = RunFullScene(replay, scene, edge, ticks, checkEvery);
                if (!run)
                    return std::unexpected(std::format("{}³ {}: {}", edge, SceneName(scene), run.error()));

                Log(Channel::Sim, Level::Info,
                    "| T-0845 一致 | {}³ | {} | {} 刻み | 比べた {} 回 | 段 {}・形の写し直し {} 刻み | ハッシュ "
                    "{:016x} |",
                    edge, SceneName(scene), run->ticks, run->checked, run->levels, run->reshapes, run->hash);
            }

            if (!measure)
                continue;

            const auto timing = MeasureFullScene(replay, scene, edge);
            if (!timing)
                return std::unexpected(std::format("{}³ {} の計測: {}", edge, SceneName(scene), timing.error()));

            const auto ns = [cells](double ms) {
                return ms * 1e6 / cells;
            };
            Log(Channel::Sim, Level::Info,
                "| T-0845 時間 | {}³ | {} | 通し {:.3f} ms(中央 {:.3f})| {:.2f} ns/セル | 式 {:.2f} | 段の重み {:.2f} "
                "| "
                "V 2 回 {:.2f} | 当てる {:.2f} | 運ぶ {:.2f} | バッファ {:.0f} B/セル(部品 {:.0f})| LOCAL の差 {:.0f} "
                "B/セル |",
                edge, SceneName(scene), timing->total, timing->median, ns(timing->total), ns(timing->parts[0]),
                ns(timing->parts[1]), ns(timing->parts[2]), ns(timing->parts[3]), ns(timing->parts[4]),
                (double)timing->deviceBytes / cells, (double)timing->equilibriumBytes / cells,
                (double)timing->vramBytes / cells);
        }

        return {};
    }

    // --- T-0915(--t0915): 釣り合う家族の部品の GPU 版。CPU の部品と毎刻みビット一致・前の GPU の道(T-0845)のハッシュ・保存・決定性 ---

    // 32³・300 刻み・V 2 の最後のハッシュ(前の GPU の道 = T-0845 の作り と CPU の前の道・部品の道。T-0853 のチケットの表。2026-10-21 に
    // 前の GPU の道でも同じ値を確かめた)。部品の GPU 版もこの値になること
    constexpr std::array<uint64_t, 5> T0845_HASHES_32 = {0xa4b6928b6f05e940ull, 0x884686f097eafbb2ull,
                                                         0x04811f98601a371dull, 0xd53f653207a2cfb6ull,
                                                         0x4b9e31f213adcd63ull};

    std::expected<void, std::string> RunT0915(Replay& replay, uint32_t ticks) {
        // --- 1. 5 場面 × ticks 刻み(32³・V 2):毎刻み CPU の部品と箱・帳簿・残差の計器がビット一致・保存・前の GPU の道のハッシュ ---
        for (size_t s = 0; s < SCENES.size(); ++s) {
            const Scene scene = SCENES[s];
            const auto run = RunFullScene(replay, scene, 32, ticks, 1);
            if (!run)
                return std::unexpected(std::format("32³ {}: {}", SceneName(scene), run.error()));

            const bool known = ticks == 300;
            if (known && run->hash != T0845_HASHES_32[s])
                return std::unexpected(std::format("32³ {}: 前の GPU の道のハッシュ {:016x} と違う {:016x}",
                                                   SceneName(scene), T0845_HASHES_32[s], run->hash));

            if (!run->conserved)
                return std::unexpected(std::format("32³ {}: 保存が崩れた", SceneName(scene)));

            Log(Channel::Sim, Level::Info,
                "| T-0915 一致 | 32³ | {} | {} 刻み毎刻み CPU の部品とビット一致 | 前の GPU の道 {} | 保存 0 | 段 "
                "{}・写し直し {} | "
                "残差の計器の最大 {} mg | ハッシュ {:016x} | VRAM {:.0f} B/セル |",
                SceneName(scene), run->ticks, known ? "一致" : "(300 刻みで比べる)", run->levels, run->reshapes,
                run->worstResidual, run->hash, (double)run->deviceBytes / (32.0 * 32.0 * 32.0));
        }

        // --- 2. 保存(V 1・2・4。16³ の爆発・孔のある壁 × 60 刻み。毎刻み CPU と一致)---
        for (const uint32_t cycles : {1u, 2u, 4u}) {
            for (const Scene scene : {Scene::Blast, Scene::Wall}) {
                const auto run = RunFullScene(replay, scene, 16, 60, 1, {.cycles = cycles});
                if (!run || !run->conserved)
                    return std::unexpected(
                        std::format("16³ {} V {}: {}", SceneName(scene), cycles, run ? "保存が崩れた" : run.error()));

                Log(Channel::Sim, Level::Info,
                    "| T-0915 保存 | 16³ | {} | V {} | 60 刻み毎刻み一致・保存 0 | {:016x} |", SceneName(scene), cycles,
                    run->hash);
            }
        }

        // --- 3. 決定性: 2 回の実行と積み方を変えた実行(1 本のリストに 8 刻み・段ごとの Dispatch を 2 段まで)でハッシュが同じ ---
        for (const Scene scene : SCENES) {
            const auto first = RunFullScene(replay, scene, 16, 120, 120, {.meter = false});
            const auto second = RunFullScene(replay, scene, 16, 120, 120, {.meter = false});
            const auto stacked = RunFullScene(replay, scene, 16, 120, 120,
                                              {.ticksPerList = 8, .dispatchLevels = 2, .meter = false});
            if (!first || !second || !stacked)
                return std::unexpected(std::format("16³ {} の決定性: {}", SceneName(scene),
                                                   !first    ? first.error()
                                                   : !second ? second.error()
                                                             : stacked.error()));

            if (first->hash != second->hash || first->hash != stacked->hash)
                return std::unexpected(std::format("16³ {} の決定性: {:016x} / {:016x} / {:016x}", SceneName(scene),
                                                   first->hash, second->hash, stacked->hash));

            Log(Channel::Sim, Level::Info,
                "| T-0915 決定性 | 16³ | {} | 120 刻み | 2 回 + 積み方を変えて(リスト {} 本)同じ {:016x} |",
                SceneName(scene), stacked->lists, first->hash);
        }

        return {};
    }

    // --- T-0900(--t0900。測り M-1): 今の陰の圧力の 1 セルの費用(ms・VRAM)と、V の回数を増やした時の 1 回あたりの費用 ---
    // 液体を同じ陰の圧力に入れた時の見込み(R1)は、費用の形が同じ(式・段・当てるはセルの値によらない)なので
    // 「V 1 回あたりの費用 × 要る V の回数」で出す。要る V の回数は CPU の gas_projection_test --t0900 が測る
    std::expected<void, std::string> RunCostStudy(Replay& replay, uint32_t edge) {
        const double cells = (double)edge * edge * edge;
        const auto ns = [cells](double ms) {
            return ms * 1e6 / cells;
        };
        for (const uint32_t cycles : {2u, 4u, 8u}) {
            for (const Scene scene : SCENES) {
                if (cycles != 2 && scene != Scene::Door)
                    continue;  // V の回数の比べは扉だけ(費用はセルの値によらない)

                sim::GasProjectionOptions options = FullOptions();
                options.solver.cycles = cycles;
                const auto timing = MeasureFullScene(replay, scene, edge, options);
                if (!timing)
                    return std::unexpected(
                        std::format("{}³ {} V {}: {}", edge, SceneName(scene), cycles, timing.error()));

                Log(Channel::Sim, Level::Info,
                    "| T-0900 費用 | {}³ | {} | V {} 回 | 段 {} | 通し {:.3f} ms(中央 {:.3f})| {:.2f} ns/セル | 式 "
                    "{:.2f} | "
                    "段の重み {:.2f} | V {:.2f} | 当てる {:.2f} | 運ぶ {:.2f} | VRAM の差 {:.1f} MiB = {:.0f} B/セル |",
                    edge, SceneName(scene), cycles, timing->levels, timing->total, timing->median, ns(timing->total),
                    ns(timing->parts[0]), ns(timing->parts[1]), ns(timing->parts[2]), ns(timing->parts[3]),
                    ns(timing->parts[4]), (double)timing->vramBytes / (1024.0 * 1024.0),
                    (double)timing->vramBytes / cells);
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
        bool t0843 = false;
        bool t0845 = false;
        bool t0900 = false;
        bool t0915 = false;
        bool defaultShape = false;
        uint32_t checkEvery = 1;
        bool edgeGiven = false;
        for (size_t index = 0; index < arguments.size(); ++index) {
            const std::string_view argument = arguments[index];
            if (argument == "--edge" && index + 1 < arguments.size()) {
                edge = (uint32_t)std::stoul(arguments[++index]);
                edgeGiven = true;
            } else if (argument == "--t0843")
                measure = t0843 = true;
            else if (argument == "--t0845")
                t0845 = true;
            else if (argument == "--t0900")
                t0900 = true;
            else if (argument == "--t0915")
                t0915 = true;
            else if (argument == "--default-shape")
                defaultShape = true;
            else if (argument == "--check-every" && index + 1 < arguments.size())
                checkEvery = std::max<uint32_t>((uint32_t)std::stoul(arguments[++index]), 1);
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
        if (!options || edge < 4 || (ticks == 0 && !t0845 && !t0900 && !t0915)) {
            Log(Channel::Gpu, Level::Error,
                "使い方: gpu_gas_projection_test [--warp] [--queue direct|compute] [--no-gbv] [--edge N] [--ticks N] "
                "[--measure] [--breakdown] [--stages] [--global-tail] [--inner-tolerance mg] [--inner-cycles n] "
                "[--t0843] [--t0845 [--check-every n]] [--t0900] [--t0915 [--ticks n]]");
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
                      .innerCycles = innerCycles,
                      .t0843 = t0843,
                      .shapeFromCost = !defaultShape};
        if (t0915) {
            const auto ran = RunT0915(replay, ticks);
            if (!ran) {
                Log(Channel::Gpu, Level::Error, "gpu_gas_projection_test: FAILED({})", ran.error());
                return 1;
            }

            if (!test::PassesValidation(*device, "gpu_gas_projection_test"))
                return 1;

            Log(Channel::Gpu, Level::Info, "gpu_gas_projection_test: OK");

            return 0;
        }

        std::vector<uint32_t> edges = {edge};
        if ((t0843 || t0845) && !edgeGiven)
            edges = {32, 64};

        if (t0900 && !edgeGiven)
            edges = {32, 64, 128};

        for (const uint32_t each : edges) {
            const auto ran = t0900   ? RunCostStudy(replay, each)
                             : t0845 ? RunFull(replay, each, ticks, checkEvery, measure)
                                     : RunAll(replay, each, ticks, measure);
            if (!ran) {
                Log(Channel::Gpu, Level::Error, "gpu_gas_projection_test: FAILED({})", ran.error());
                return 1;
            }
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
