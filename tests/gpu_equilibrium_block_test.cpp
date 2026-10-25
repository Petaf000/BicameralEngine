// gpu_equilibrium_block_test.cpp — 成分を持つ節の釣り合いの GPU 版(sim/gpu_equilibrium_block。T-0970・ADR-0422)を CPU リファレンス
// (sim/equilibrium_block の SolveEquilibriumBlocks)とビット一致で確かめ、1 節の費用を測る。
// 1. 支えの場面(sim/support_law の 1 つの系 Coupled。k = 3): 1 セルの張り出し 4 m(0.5 m)・張り出し 4 m(0.25 m)・片持ち梁 16 × 4 m(0.5 m)・
//    両端を固定した梁 8 × 1 m(0.5 m)・アーチ(0.5 / 0.25 m)・洞窟の天井(1 / 0.5 / 0.25 m)・掘った直後(16 × 8 m の下を掘り、前の答えから)。
//    CPU の SupportLaw::Step と同じ系を GPU で 1 刻みずつ解き(1 本のリストに --ticks 刻み。答えは次の刻みの温めた始まり)、毎刻み節 × 3 の値を比べる。
//    値が同じなので、帳簿(Σ セルの上向きの力 = 固定した面の力 − 重さ)と面の力も CPU と同じ(support_law_test が CPU で確かめる)。
// 2. 成分の数 k = 1・2・3・6 の箱(一辺 8。軸の面の k × k の結合・下の面を固定・区分的に一定の延ばし方)を 5 刻み、同じく比べる。
//    違うレベルの節が混ざる系(T-1104・ADR-0462): equilibrium_block_test の箱(k = 1・3。真ん中 2 × 2 × 2 をレベル 1)を V 2 回・
//    V 4 回 + 共役勾配・V 2 回 + 刻みをまたぐ共役勾配(D-649)で 12 刻み、節と面の並びを逆にした系も。支えの違う大きさのセルの場面
//    (support_mixed_test の場面。0.5 m と 0.25 m。支えの既定 = V 2 回 + 刻みをまたぐ共役勾配)を --ticks 刻み。
// 3. 決定性(R3): 色の一覧・行・子・固定した面の順を逆に積んだ GPU と 2 回目の GPU が、最後の値のハッシュで一致。
// 4. 費用(--measure): k ごとの箱(一辺 32。64 は k = 1・3)の 1 回の解き(V 2 回)の時間(暖機の後 8 回の最小)・段を作る時間・VRAM・Dispatch の数。
//    支えの洞窟 0.25 m の 1 回の解きも。値は比べない(1 と 2 で比べてある)。
// 引数: [--ticks n](支えの場面の刻み。既定 30)[--measure] と GPU の試験の共通の引数(--warp・--queue・--no-gbv)。
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "equilibrium_block_scenes.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "sim/equilibrium_block.h"
#include "sim/gpu_equilibrium_block.h"
#include "sim/support_law.h"
#include "support_scenes.h"

using namespace bicameral;

namespace {

    constexpr uint32_t MEASURE_WARMUP = 2;
    constexpr uint32_t MEASURE_ROUNDS = 8;

    struct Context {
        ID3D12Device5* device = nullptr;
        gpu::ImmediateQueue* queue = nullptr;
        gpu::DebugRing* ring = nullptr;
        uint64_t frequency = 0;
    };

    uint64_t HashValues(std::span<const int64_t> values) {
        uint64_t hash = 14695981039346656037ull;
        for (const int64_t value : values) {
            hash ^= (uint64_t)value;
            hash *= 1099511628211ull;
        }

        return hash;
    }

    // 同じ系を GPU で ticks 回解き、刻みごとの段 0 の値(節 × k)を返す
    std::expected<std::vector<std::vector<int64_t>>, std::string> SolveTicks(
        Context& context, const sim::EquilibriumBlockSystem& system, const sim::EquilibriumBlockGrid& grid,
        const sim::EquilibriumBlockOptions& solve, std::span<const int64_t> warm, uint32_t ticks, bool reverse) {
        auto created = sim::GpuEquilibriumBlock::Create(
            context.device, system, grid, {.solve = solve, .reverseOrder = reverse, .readbackSlots = ticks});
        if (!created)
            return std::unexpected(created.error());

        sim::GpuEquilibriumBlock& gpu = *created;
        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        const uint64_t ring = context.ring->GpuAddress();
        context.ring->RecordBegin(list);
        gpu.RecordBegin(list);
        if (!gpu.RecordUpload(list, system, grid, warm))
            return std::unexpected("段の形が合わない");

        gpu.RecordPrepare(list, ring);
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            gpu.RecordSolve(list, ring);
            gpu.RecordValuesReadback(list, tick);
        }

        gpu.RecordEnd(list);
        context.ring->RecordReadbackAndReset(list);
        if (!context.queue->ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        const gpu::DebugRingContents debug = context.ring->Drain();
        if (debug.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debug.assertCount));

        std::vector<std::vector<int64_t>> result(ticks);
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            if (!gpu.ReadValues(result[tick], tick))
                return std::unexpected("読み戻せない");
        }

        return result;
    }

    // 最初に違う所(刻み・節・成分)。同じなら空
    std::string FirstDifference(std::span<const int64_t> cpu, std::span<const int64_t> gpu, uint32_t k, uint32_t tick) {
        if (cpu.size() != gpu.size())
            return std::format("{} 刻み目: 数が違う(CPU {}・GPU {})", tick + 1, cpu.size(), gpu.size());

        for (size_t i = 0; i < cpu.size(); ++i) {
            if (cpu[i] != gpu[i]) {
                return std::format("{} 刻み目: 節 {} の成分 {}(CPU {}・GPU {})", tick + 1, i / k, i % k, cpu[i],
                                   gpu[i]);
            }
        }

        return {};
    }

    // GPU(順のまま・逆順)を 1 回ずつ。CPU の刻みごとの値 cpuTicks と比べる
    std::expected<void, std::string> CompareRuns(Context& context, std::string_view name,
                                                 const sim::EquilibriumBlockSystem& system,
                                                 const sim::EquilibriumBlockGrid& grid,
                                                 const sim::EquilibriumBlockOptions& solve,
                                                 std::span<const int64_t> warm,
                                                 const std::vector<std::vector<int64_t>>& cpuTicks) {
        const auto ticks = (uint32_t)cpuTicks.size();
        uint64_t firstHash = 0;
        for (const bool reverse : {false, true}) {
            const auto gpuTicks = SolveTicks(context, system, grid, solve, warm, ticks, reverse);
            if (!gpuTicks)
                return std::unexpected(std::format("{}: {}", name, gpuTicks.error()));

            for (uint32_t tick = 0; tick < ticks; ++tick) {
                const std::string difference = FirstDifference(cpuTicks[tick], (*gpuTicks)[tick], system.components,
                                                               tick);
                if (!difference.empty())
                    return std::unexpected(std::format("{}{}: {}", name, reverse ? "(逆順)" : "", difference));
            }

            const uint64_t hash = HashValues(gpuTicks->back());
            if (reverse && hash != firstHash)
                return std::unexpected(std::format("{}: 逆順のハッシュが違う", name));

            firstHash = hash;
        }

        Log(Channel::Sim, Level::Info, "{}: 節 {}・段 {}・{} 刻み毎刻み CPU とビット一致(逆順も)・ハッシュ {:016x}",
            name, system.nodes.size(), grid.levels.size(), ticks, firstHash);

        return {};
    }

    // --- 1. 支えの場面 ---
    struct SupportCase {
        const char* name;
        sim::SupportScene scene;
    };

    std::vector<SupportCase> SupportCases() {
        return {
            {.name = "1 セルの張り出し 4 m(0.5 m)", .scene = test::Beam(8, 1, 500, false)},
            {.name = "張り出し 4 m(0.25 m)", .scene = test::Beam(16, 2, 250, false)},
            {.name = "片持ち梁 16 × 4 m(0.5 m)", .scene = test::Beam(32, 8, 500, false)},
            {.name = "両端を固定した梁 8 × 1 m(0.5 m)", .scene = test::Beam(16, 2, 500, true)},
            {.name = "アーチ 0.5 m", .scene = test::Arch(500)},
            {.name = "アーチ 0.25 m", .scene = test::Arch(250)},
            {.name = "洞窟の天井 1 m", .scene = test::Cave(1000)},
            {.name = "洞窟の天井 0.5 m", .scene = test::Cave(500)},
            {.name = "洞窟の天井 0.25 m", .scene = test::Cave(250)},
        };
    }

    // 違う大きさのセル(0.5 m と 0.25 m)が混ざる場面(support_mixed_test と同じ。T-1120・T-1104)。Coupled の 1 つの系がレベル 0 と 1 の節を持つ
    std::vector<SupportCase> MixedSupportCases() {
        const sim::SupportScene shelf = test::Beam(8, 1, 500, false);
        const sim::SupportScene cantilever = test::Beam(16, 2, 500, false);
        const sim::SupportScene clamped = test::Beam(16, 2, 500, true);

        return {
            {.name = "張り出し 4 m の根 1 m を 0.25 m", .scene = test::Marked(shelf, 0, 2, 0, 1)},
            {.name = "片持ち梁 8 × 1 m の根の半分を 0.25 m", .scene = test::Marked(cantilever, 0, 8, 0, 2)},
            {.name = "片持ち梁 8 × 1 m の上の段だけ 0.25 m", .scene = test::Marked(cantilever, 0, 16, 1, 2)},
            {.name = "両端を固定した梁 8 × 1 m の真ん中 3 m を 0.25 m", .scene = test::Marked(clamped, 5, 11, 0, 2)},
            {.name = "洞窟の天井(と空洞の縁)を 0.25 m", .scene = test::Marked(test::Cave(500), 14, 34, 6, 20)},
        };
    }

    // CPU の law を ticks 刻み回した値(刻みごと)
    std::vector<std::vector<int64_t>> CpuTicks(sim::SupportLaw& law, uint32_t ticks) {
        std::vector<std::vector<int64_t>> result;
        for (uint32_t tick = 0; tick < ticks; ++tick) {
            law.Step();
            result.push_back(law.BlockValues());
        }

        return result;
    }

    std::expected<void, std::string> CompareSupport(Context& context, std::string_view name, sim::SupportLaw& law,
                                                    const sim::SupportOptions& options, uint32_t ticks) {
        const std::vector<int64_t> warm = law.BlockValues();
        const auto cpuTicks = CpuTicks(law, ticks);

        return CompareRuns(context, name, law.BlockSystem(), law.BlockGrid(), options.blocks, warm, cpuTicks);
    }

    std::expected<void, std::string> TestMixedSupport(Context& context, const sim::SupportOptions& options,
                                                      uint32_t ticks) {
        for (const SupportCase& each : MixedSupportCases()) {
            sim::SupportLaw law(each.scene, options);
            const auto compared = CompareSupport(context, each.name, law, options, ticks);
            if (!compared)
                return compared;
        }

        return {};
    }

    std::expected<void, std::string> TestSupport(Context& context, uint32_t ticks) {
        const sim::SupportOptions options{.solve = sim::SupportSolve::Coupled};
        for (const SupportCase& each : SupportCases()) {
            sim::SupportLaw law(each.scene, options);
            const auto compared = CompareSupport(context, each.name, law, options, ticks);
            if (!compared)
                return compared;
        }

        // 掘った直後(support_law_test の TestDig と同じ場面: 32 × 16 セルを ticks 刻み → 下の 16 × 6 セルを掘り、前の答えから)
        constexpr uint32_t WIDTH = 32;
        constexpr uint32_t HEIGHT = 16;
        sim::SupportScene scene = test::Beam(WIDTH, HEIGHT, 500, false);
        for (uint32_t i = 0; i < WIDTH * HEIGHT; ++i)
            scene.clamps[i] = i < WIDTH ? sim::SUPPORT_CLAMP_BOTTOM : 0;

        sim::SupportLaw before(scene, options);
        for (uint32_t tick = 0; tick < ticks; ++tick)
            before.Step();

        for (uint32_t up = 4; up < 10; ++up) {
            for (uint32_t x = 8; x < 24; ++x)
                scene.solid[x + WIDTH * up] = 0;
        }

        sim::SupportLaw dug(scene, options);
        dug.CarryFrom(before);

        const auto compared = CompareSupport(context, "掘った直後(16 × 8 m の下 8 × 3 m)", dug, options, ticks);
        if (!compared)
            return compared;

        return TestMixedSupport(context, options, ticks);
    }

    // --- 2. k 成分の箱 ---
    sim::EquilibriumBlockSystem BoxSystem(uint32_t k, uint32_t edge) {
        constexpr int64_t UNIT = int64_t{1} << 32;
        sim::EquilibriumBlockSystem system;
        system.components = k;
        system.coefficientShift = 32;
        for (uint32_t octant = 0; octant < sim::EQ_OCTANTS; ++octant) {
            for (uint32_t c = 0; c < k; ++c)
                system.prolongation[octant][(c * sim::EQ_MAX_COMPONENTS) + c] = int64_t{1} << sim::EQ_PROLONG_SHIFT;
        }

        // 面の k × k: 対角 (k + 1) × 単位・非対角 1 × 単位(対角が優る正定値)
        sim::EquilibriumBlock coupling{};
        for (uint32_t row = 0; row < k; ++row) {
            for (uint32_t column = 0; column < k; ++column)
                coupling[(row * sim::EQ_MAX_COMPONENTS) + column] = row == column ? (int64_t)(k + 1) * UNIT : UNIT;
        }

        sim::EquilibriumBlock negative{};
        for (size_t i = 0; i < negative.size(); ++i)
            negative[i] = -coupling[i];

        const auto index = [edge](uint32_t x, uint32_t y, uint32_t z) {
            return x + (edge * (y + (edge * z)));
        };
        for (uint32_t z = 0; z < edge; ++z) {
            for (uint32_t y = 0; y < edge; ++y) {
                for (uint32_t x = 0; x < edge; ++x)
                    system.nodes.push_back({.level = 0, .x = x, .y = y, .z = z});
            }
        }

        for (uint32_t z = 0; z < edge; ++z) {
            for (uint32_t y = 0; y < edge; ++y) {
                for (uint32_t x = 0; x < edge; ++x) {
                    const uint32_t node = index(x, y, z);
                    const std::array<bool, 3> inside = {x + 1 < edge, y + 1 < edge, z + 1 < edge};
                    const std::array<uint32_t, 3> next = {node + 1, node + edge, node + (edge * edge)};
                    for (uint32_t axis = 0; axis < 3; ++axis) {
                        if (inside[axis]) {
                            system.faces.push_back({.low = node,
                                                    .high = next[axis],
                                                    .lowLow = coupling,
                                                    .lowHigh = negative,
                                                    .highHigh = coupling});
                        }
                    }

                    for (uint32_t c = 0; c < k; ++c) {
                        const int64_t weight = -(int64_t)((c + 1) * ((x + y + 1) % 5)) * (int64_t{1} << 20);
                        system.sources.push_back({.node = node, .component = c, .value = weight});
                    }

                    if (z != 0)
                        continue;

                    sim::EquilibriumBlockFixed fixed{.node = node, .block = coupling};
                    for (uint32_t c = 0; c < k; ++c)
                        fixed.value[c] = (int64_t)(c + 1) * 1000;

                    system.fixedFaces.push_back(fixed);
                }
            }
        }

        return system;
    }

    std::expected<void, std::string> TestComponents(Context& context) {
        constexpr uint32_t EDGE = 8;
        constexpr uint32_t TICKS = 5;
        // V 2 回だけ(前の既定)と、V 4 回 + 共役勾配(lineSearch。T-1085・ADR-0455。支えの今の既定)
        for (const bool conjugate : {false, true}) {
            for (const uint32_t k : {1u, 2u, 3u, 6u}) {
                const sim::EquilibriumBlockSystem system = BoxSystem(k, EDGE);
                const sim::EquilibriumBlockGrid grid = sim::BuildEquilibriumBlockGrid(system);
                const sim::EquilibriumBlockOptions solve{.cycles = conjugate ? 4u : 2u, .lineSearch = conjugate};
                sim::EquilibriumBlockState state;
                std::vector<std::vector<int64_t>> cpuTicks;
                for (uint32_t tick = 0; tick < TICKS; ++tick) {
                    sim::SolveEquilibriumBlocks(system, grid, solve, state);
                    cpuTicks.push_back(state.values);
                }

                const std::string name = std::format("箱 一辺 {}・k = {}{}", EDGE, k, conjugate ? "・共役勾配" : "");
                const auto compared = CompareRuns(context, name, system, grid, solve, {}, cpuTicks);
                if (!compared)
                    return compared;
            }
        }

        return {};
    }

    // 違うレベルの節が混ざる箱(T-1104)。CPU の刻みごとの値と比べる(GPU の逆順は CompareRuns。節と面の並びを逆にした系も)
    std::expected<void, std::string> TestMixed(Context& context) {
        constexpr uint32_t TICKS = 12;
        struct Variant {
            const char* name;
            sim::EquilibriumBlockOptions solve;
        };
        const std::array<Variant, 3> variants = {{
            {.name = "V 2 回", .solve = {}},
            {.name = "V 4 回 + 共役勾配", .solve = {.cycles = 4, .lineSearch = true}},
            {.name = "V 2 回 + 刻みをまたぐ共役勾配",
             .solve = {.cycles = 2, .lineSearch = true, .conjugateAcrossTicks = true}},
        }};
        for (const Variant& variant : variants) {
            for (const uint32_t k : {1u, 3u}) {
                for (const bool reverse : {false, true}) {
                    const sim::EquilibriumBlockSystem system = test::MixedSystem(k, true, reverse);
                    const sim::EquilibriumBlockGrid grid = sim::BuildEquilibriumBlockGrid(system);
                    sim::EquilibriumBlockState state;
                    std::vector<std::vector<int64_t>> cpuTicks;
                    for (uint32_t tick = 0; tick < TICKS; ++tick) {
                        sim::SolveEquilibriumBlocks(system, grid, variant.solve, state);
                        cpuTicks.push_back(state.values);
                    }

                    const std::string name = std::format("違うレベルの箱 k = {}・{}{}", k, variant.name,
                                                         reverse ? "・節と面を逆に" : "");
                    const auto compared = CompareRuns(context, name, system, grid, variant.solve, {}, cpuTicks);
                    if (!compared)
                        return compared;
                }
            }
        }

        return {};
    }

    // --- 4. 費用 ---
    struct Cost {
        double solveMs = 0;
        double prepareMs = 0;
    };

    std::expected<Cost, std::string> MeasureSolve(Context& context, const sim::EquilibriumBlockSystem& system,
                                                  const sim::EquilibriumBlockGrid& grid,
                                                  sim::GpuEquilibriumBlock& gpu) {
        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        const uint64_t ring = context.ring->GpuAddress();
        context.ring->RecordBegin(list);
        gpu.RecordBegin(list);
        if (!gpu.RecordUpload(list, system, grid, {}))
            return std::unexpected("段の形が合わない");

        gpu.RecordTimestamp(list, 0);
        gpu.RecordPrepare(list, ring);
        gpu.RecordTimestamp(list, 1);
        for (uint32_t round = 0; round < MEASURE_WARMUP + MEASURE_ROUNDS; ++round) {
            const uint32_t slot = 2 + (2 * round);
            gpu.RecordTimestamp(list, slot);
            gpu.RecordSolve(list, ring);
            gpu.RecordTimestamp(list, slot + 1);
        }

        const uint32_t stamps = 2 + (2 * (MEASURE_WARMUP + MEASURE_ROUNDS));
        gpu.RecordTimestampReadback(list, stamps);
        gpu.RecordEnd(list);
        context.ring->RecordReadbackAndReset(list);
        if (!context.queue->ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        (void)context.ring->Drain();
        const std::vector<uint64_t> ticks = gpu.ReadTimestamps(stamps);
        if (ticks.size() != stamps)
            return std::unexpected("時間を読めない");

        const auto toMs = [&](uint64_t begin, uint64_t end) {
            return (double)(end - begin) * 1000.0 / (double)context.frequency;
        };

        Cost cost{.prepareMs = toMs(ticks[0], ticks[1])};
        for (uint32_t round = MEASURE_WARMUP; round < MEASURE_WARMUP + MEASURE_ROUNDS; ++round) {
            const uint32_t slot = 2 + (2 * round);
            const double ms = toMs(ticks[slot], ticks[slot + 1]);
            cost.solveMs = round == MEASURE_WARMUP ? ms : std::min(cost.solveMs, ms);
        }

        return cost;
    }

    std::expected<void, std::string> MeasureOne(Context& context, std::string_view name,
                                                const sim::EquilibriumBlockSystem& system,
                                                const sim::EquilibriumBlockGrid& grid, bool quiet = false,
                                                const sim::EquilibriumBlockOptions& solve = {}) {
        auto gpu = sim::GpuEquilibriumBlock::Create(context.device, system, grid, {.solve = solve});
        if (!gpu)
            return std::unexpected(gpu.error());

        const auto cost = MeasureSolve(context, system, grid, *gpu);
        if (!cost)
            return std::unexpected(cost.error());

        if (quiet)
            return {};

        const double cells = (double)gpu->Cells();
        Log(Channel::Sim, Level::Info,
            "費用 {}: 節 {}(全部の段 {})・段 {}・隣 {}・1 回の解き(V {} 回{}){:.3f} ms = {:.1f} ns/節・Dispatch {}・"
            "段を作る {:.3f} ms = {:.1f} ns/節・VRAM {:.1f} MB = {:.0f} B/節",
            name, gpu->Cells(), gpu->Nodes(), gpu->Levels(), gpu->Links(), solve.cycles,
            solve.lineSearch ? " + 共役勾配" : "", cost->solveMs, cost->solveMs * 1.0e6 / cells,
            gpu->DispatchesPerSolve(), cost->prepareMs, cost->prepareMs * 1.0e6 / cells,
            (double)gpu->DeviceBytes() / 1.0e6, (double)gpu->DeviceBytes() / cells);

        return {};
    }

    std::expected<void, std::string> Measure(Context& context) {
        // 暖機(GPU の時計が上がるまで。最初の場面だけ遅く出た)
        for (uint32_t round = 0; round < 3; ++round) {
            const sim::EquilibriumBlockSystem system = BoxSystem(3, 32);
            const auto warmed = MeasureOne(context, "暖機", system, sim::BuildEquilibriumBlockGrid(system), true);
            if (!warmed)
                return warmed;
        }

        // 一辺 64 は k = 1・3 だけ(k = 6 は係数だけで 450 MB を超え、CPU の段の作りも重い)
        for (const uint32_t edge : {32u, 64u}) {
            for (const uint32_t k : {1u, 2u, 3u, 6u}) {
                if (edge == 64 && k != 1 && k != 3)
                    continue;

                const sim::EquilibriumBlockSystem system = BoxSystem(k, edge);
                const sim::EquilibriumBlockGrid grid = sim::BuildEquilibriumBlockGrid(system);
                const auto measured = MeasureOne(context, std::format("箱 一辺 {}・k = {}", edge, k), system, grid);
                if (!measured)
                    return measured;
            }
        }

        // 支えの洞窟: 前の前の既定(V 2 回)と今の既定(V 2 回 + 共役勾配・刻みをまたぐ。D-649)
        const sim::SupportOptions support{.solve = sim::SupportSolve::Coupled};
        for (const uint64_t cellMm : {500ull, 250ull}) {
            const sim::SupportLaw law(test::Cave(cellMm), support);
            for (const sim::EquilibriumBlockOptions& solve : {sim::EquilibriumBlockOptions{}, support.blocks}) {
                const auto measured = MeasureOne(context, std::format("支えの洞窟 {} mm・k = 3", cellMm),
                                                 law.BlockSystem(), law.BlockGrid(), false, solve);
                if (!measured)
                    return measured;
            }
        }

        // 違う大きさのセルが混ざる洞窟(0.5 m の天井と空洞の縁を 0.25 m。T-1104)と、違うレベルの箱(k = 3)
        const sim::SupportLaw mixedCave(test::Marked(test::Cave(500), 14, 34, 6, 20), support);
        for (const sim::EquilibriumBlockOptions& solve : {sim::EquilibriumBlockOptions{}, support.blocks}) {
            const auto measured = MeasureOne(context, "支えの洞窟 500 mm の天井を 250 mm・k = 3",
                                             mixedCave.BlockSystem(), mixedCave.BlockGrid(), false, solve);
            if (!measured)
                return measured;
        }

        const sim::EquilibriumBlockSystem mixedBox = test::MixedSystem(3, true, false);
        const auto measuredBox = MeasureOne(context, "違うレベルの箱 一辺 8・k = 3", mixedBox,
                                            sim::BuildEquilibriumBlockGrid(mixedBox));
        if (!measuredBox)
            return measuredBox;

        return {};
    }

    int Run(std::span<char*> arguments) {
        uint32_t ticks = 30;
        bool measure = false;
        std::vector<char*> rest = {arguments[0]};
        for (size_t index = 1; index < arguments.size(); ++index) {
            const std::string_view argument = arguments[index];
            if (argument == "--ticks" && index + 1 < arguments.size())
                ticks = std::max<uint32_t>((uint32_t)std::stoul(arguments[++index]), 1);
            else if (argument == "--measure")
                measure = true;
            else
                rest.push_back(arguments[index]);
        }

        const auto options = test::ParseGpuTestOptions(rest);
        if (!options) {
            Log(Channel::Gpu, Level::Error,
                "使い方: gpu_equilibrium_block_test [--warp] [--queue direct|compute] [--no-gbv] [--ticks n] "
                "[--measure]");
            return 2;
        }

        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!device) {
            Log(Channel::Gpu, Level::Error, "gpu_equilibrium_block_test: FAILED(デバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        uint64_t frequency = 0;
        if (!queue || !ring || FAILED(queue->Native()->GetTimestampFrequency(&frequency))) {
            Log(Channel::Gpu, Level::Error, "gpu_equilibrium_block_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        Context context{.device = device->Get(), .queue = &*queue, .ring = &*ring, .frequency = frequency};
        auto ran = TestComponents(context);
        if (ran)
            ran = TestMixed(context);

        if (ran)
            ran = TestSupport(context, ticks);

        if (ran && measure)
            ran = Measure(context);

        if (!ran) {
            Log(Channel::Gpu, Level::Error, "gpu_equilibrium_block_test: FAILED({})", ran.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_equilibrium_block_test"))
            return 1;

        Log(Channel::Gpu, Level::Info, "gpu_equilibrium_block_test: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
