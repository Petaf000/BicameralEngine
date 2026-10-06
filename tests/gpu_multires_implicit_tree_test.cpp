// gpu_multires_implicit_tree_test.cpp — 木につないだ陰解法(方式②。T-0119 の multires_implicit_conduction.cpp が作る系)を GPU の
// sim::GpuImplicit で解き、CPU の StepImplicit と毎刻みビット一致するかを確かめる(T-0127)。
// 系(未知数・境のセル・面・多重格子の段)はまだ CPU が作る(MultiresNest::captureImplicitGrid で刻みごとに写す)。GPU で作って
// 伝導の段から呼ぶのは T-0129・T-0132。ここで確かめるのは「木の本物の系(刻みの初めの温度・端数の枠の無い粗い側・境のセルの長い行)
// を GPU の段がそのまま解けること」と、その費用。
// 場面(CPU の multires_implicit_tree_test と同じ):
//   - 熱い点: レベル 6 の根の 1 セルだけ 1500 K(Δk 4)
//   - 鎖: レベル 0〜6・いちばん細かいブロックが 1500 K・端数の枠 16(粗い側に端数の枠が無いブロックもある)
//   - たくさんの要求: 深さ 26 段・木箱が燃える(相変化と反応で熱容量が変わる。Δk ≤ 8 を陰解法)
// 確かめること: 毎刻み、セルのエネルギー・端数・V サイクルの回数・安全網の数が CPU と一致。CPU が木の中で解いた費用(nest.implicitCost)とも一致。
// 計測(release のハードウェアだけ。--measure-only なら計測だけ): 刻みごとの GPU の ms(同じ系を写し直して REPEATS 回解いた最小)。
//   V サイクルの上限(木の既定 64 と 16)・ImTail の境(GpuImplicitTuning)を変えて比べる。
#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/multires_conduction.hlsli"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu_test_options.h"
#include "multires_activity_scene.h"
#include "multires_conduction_scene.h"
#include "sim/gpu_implicit.h"
#include "sim/implicit_conduction.h"
#include "sim/multires_nest.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::multires;

namespace {

#ifdef NDEBUG
    constexpr bool RELEASE = true;
#else
    constexpr bool RELEASE = false;
#endif

    // --- 場面: 刻みごとの系(解く前)と、CPU が木の中で解いた費用 ---

    struct TreeTick {
        sim::ImplicitGrid grid;
        sim::ImplicitCost cost;
    };

    struct TreeScene {
        std::string name;
        std::vector<TreeTick> ticks;
        sim::ImplicitOptions options;
    };

    // 木の陰解法と同じ選択(multires_implicit_conduction.cpp の MakeImplicitOptions。ADR-0019)
    sim::ImplicitOptions TreeImplicitOptions(const sim::MultiresStepOptions& options) {
        return {
            .method = sim::ImplicitMethod::Multigrid, .cycles = options.implicitMaxCycles, .toleranceMillikelvin = 1};
    }

    // 案 a: 方式①は分けない + 基準より細かい所は全部②(D-434)
    sim::MultiresStepOptions TreeOptions(const sim::BakedReactionTable& table) {
        sim::MultiresStepOptions options = test::SubcycleTestOptions(table, 0);
        options.implicitConduction = true;

        return options;
    }

    // 刻んだ後の系を写す(陰解法の面が無い刻みは足さない)
    void Capture(const sim::MultiresNest& nest, TreeScene& scene) {
        if (nest.implicitGrid.cells.empty())
            return;

        scene.ticks.push_back({.grid = nest.implicitGrid, .cost = nest.implicitCost});
    }

    TreeScene HotPointScene(const sim::BakedReactionTable& table, uint64_t ticks) {
        constexpr int32_t HOT_LEVEL = 6;
        sim::MultiresCapacity capacity = test::MakeMultiresCapacity(table, 1, 0, 0);
        capacity.rootLevel = HOT_LEVEL;
        sim::MultiresNest nest = sim::MakeMultiresNest(capacity);
        std::vector<reaction::RxCell> cells(MR_BLOCK_CELLS, test::MakeConductionAir(table, 300000));
        cells[MrCellIndex(3, 4, 5)] = test::MakeConductionAir(table, 1500000);
        sim::PlaceRootBlock(nest, 0, 0, 0, cells);
        nest.captureImplicitGrid = true;

        const sim::MultiresStepOptions options = TreeOptions(table);
        TreeScene scene{.name = "熱い点", .ticks = {}, .options = TreeImplicitOptions(options)};
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            sim::StepNest(nest, table, test::CONDUCTION_SEED, tick, options);
            Capture(nest, scene);
        }

        return scene;
    }

    TreeScene ChainScene(const sim::BakedReactionTable& table, uint64_t ticks) {
        sim::MultiresNest nest = test::MakeChainNest(table, 16);
        nest.captureImplicitGrid = true;

        const sim::MultiresStepOptions options = TreeOptions(table);
        TreeScene scene{.name = "鎖", .ticks = {}, .options = TreeImplicitOptions(options)};
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            sim::StepNest(nest, table, test::CONDUCTION_SEED, tick, options);
            Capture(nest, scene);
        }

        return scene;
    }

    TreeScene StressScene(const sim::BakedReactionTable& table, uint64_t ticks) {
        sim::MultiresNest nest = test::MakeActivityNest(table);
        nest.captureImplicitGrid = true;

        const sim::MultiresStepOptions options = TreeOptions(table);
        TreeScene scene{.name = "たくさんの要求", .ticks = {}, .options = TreeImplicitOptions(options)};
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            const std::vector<MrRequest> requests = test::MakeStressRequests(nest, tick);
            test::BeginActivityTick(nest, tick, requests);
            test::EndActivityTick(nest, table, tick, false, options);
            Capture(nest, scene);
        }

        return scene;
    }

    // --- GPU で 1 回解く ---

    using Record = std::function<bool(ID3D12GraphicsCommandList10*)>;

    // 1 本のリストを記録して投げ、読み戻す
    std::expected<void, std::string> Execute(gpu::ImmediateQueue& queue, gpu::DebugRing& ring, sim::GpuImplicit& gpu,
                                             const Record& record) {
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        ring.RecordBegin(list);
        if (!record(list))
            return std::unexpected("記録できない");

        gpu.RecordReadback(list);
        ring.RecordReadbackAndReset(list);
        if (!queue.ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        const gpu::DebugRingContents debugOutput = ring.Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debugOutput.assertCount));

        return {};
    }

    struct Solved {
        sim::ImplicitGrid grid;
        sim::GpuImplicitCost cost;
        double stepMs = 0.0;  // repeats 回の最小(計測しない時は 0)
        uint32_t tailDepth = 0;
        uint32_t levelCount = 0;
        uint32_t dispatchesPerCycle = 0;
    };

    struct SolveSetup {
        ID3D12Device5* device = nullptr;
        gpu::ImmediateQueue* queue = nullptr;
        gpu::DebugRing* ring = nullptr;
        uint64_t frequency = 0;  // 0 なら測らない
    };

    constexpr uint32_t
        WARMUP_STEPS = 100;  // 測る前に同じ系で V 1 回の刻みを回す(GPU の時計を上げる。T-0117 と同じ考え)

    sim::ImplicitOptions WarmupOptions() {
        return {.method = sim::ImplicitMethod::Multigrid, .cycles = 1};
    }

    // 系を GPU に写して 1 刻み解く。測る時は暖機の後、同じ系を写し直して repeats 回解き、最小の GPU 時間を取る
    std::expected<Solved, std::string> SolveOnGpu(const SolveSetup& setup, const sim::ImplicitGrid& grid,
                                                  const sim::ImplicitOptions& options,
                                                  const sim::GpuImplicitTuning& tuning, uint32_t repeats) {
        auto gpu = sim::GpuImplicit::Create(setup.device, grid, tuning);
        if (!gpu)
            return std::unexpected(gpu.error());

        Solved solved{.grid = grid,
                      .cost = {},
                      .stepMs = 0.0,
                      .tailDepth = gpu->TailDepth(),
                      .levelCount = gpu->LevelCount(),
                      .dispatchesPerCycle = gpu->DispatchesPerCycle(options)};
        if (setup.frequency != 0) {
            const auto warm = [&](ID3D12GraphicsCommandList10* list) {
                bool recorded = gpu->RecordUpload(list, grid);
                for (uint32_t i = 0; i < WARMUP_STEPS && recorded; ++i)
                    recorded = gpu->RecordStep(list, setup.ring->GpuAddress(), WarmupOptions(), 0);

                return recorded;
            };
            if (auto executed = Execute(*setup.queue, *setup.ring, *gpu, warm); !executed)
                return std::unexpected(executed.error());
        }

        for (uint32_t repeat = 0; repeat < std::max(repeats, 1u); ++repeat) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                const bool uploaded = gpu->RecordUpload(list, grid);
                gpu->RecordTimestamp(list, 0);
                const bool stepped = uploaded && gpu->RecordStep(list, setup.ring->GpuAddress(), options);
                gpu->RecordTimestamp(list, 1);

                return stepped;
            };
            if (auto executed = Execute(*setup.queue, *setup.ring, *gpu, record); !executed)
                return std::unexpected(executed.error());

            const std::vector<uint64_t> stamps = gpu->ReadTimestamps(2);
            if (stamps.size() != 2 || !gpu->Read(solved.grid, solved.cost))
                return std::unexpected("読み戻せない");

            if (setup.frequency == 0)
                continue;

            const double milliseconds = static_cast<double>(stamps[1] - stamps[0]) * 1000.0 /
                                        static_cast<double>(setup.frequency);
            solved.stepMs = repeat == 0 ? milliseconds : std::min(solved.stepMs, milliseconds);
        }

        return solved;
    }

    // --- 比べる ---

    std::expected<void, std::string> CompareCost(const sim::ImplicitCost& expected, const sim::ImplicitCost& cpu) {
        if (expected.cycles == cpu.cycles && expected.limitedCells == cpu.limitedCells &&
            expected.limitRounds == cpu.limitRounds && expected.worstExcessMillikelvin == cpu.worstExcessMillikelvin)
            return {};

        return std::unexpected(
            std::format("CPU を解き直した数が木の中と違う(V サイクル {} / {})", expected.cycles, cpu.cycles));
    }

    std::expected<void, std::string> CompareGpu(const sim::ImplicitGrid& cpu, const sim::ImplicitCost& cpuCost,
                                                const Solved& gpu) {
        for (size_t i = 0; i < cpu.cells.size(); ++i) {
            const sim::ImplicitCell& a = cpu.cells[i];
            const sim::ImplicitCell& b = gpu.grid.cells[i];
            if (a.energy != b.energy || a.fraction != b.fraction) {
                return std::unexpected(std::format("セル {}(レベル {})が食い違う: CPU {} + {} / GPU {} + {}", i,
                                                   a.level, a.energy, a.fraction, b.energy, b.fraction));
            }
        }

        if (!gpu.cost.limitFinished)
            return std::unexpected("安全網が記録した回数の中で止まらない");

        if (cpuCost.cycles != gpu.cost.cycles || cpuCost.limitedCells != gpu.cost.limitedCells ||
            cpuCost.limitRounds != gpu.cost.limitRounds ||
            cpuCost.worstExcessMillikelvin != gpu.cost.worstExcessMillikelvin) {
            return std::unexpected(
                std::format("数が食い違う: V サイクル {} / {}・戻したセル {} / {}・安全網の回数 {} / {}",
                            cpuCost.cycles, gpu.cost.cycles, cpuCost.limitedCells, gpu.cost.limitedCells,
                            cpuCost.limitRounds, gpu.cost.limitRounds));
        }

        return {};
    }

    // 刻みごとに CPU で解き直し(木の中の費用と一致)、GPU で解いて比べる。測る時は刻みごとの ms を返す
    std::expected<std::vector<Solved>, std::string> RunScene(const SolveSetup& setup, const TreeScene& scene,
                                                             const sim::ImplicitOptions& options,
                                                             const sim::GpuImplicitTuning& tuning, uint32_t repeats) {
        std::vector<Solved> results;
        for (size_t tick = 0; tick < scene.ticks.size(); ++tick) {
            sim::ImplicitGrid cpu = scene.ticks[tick].grid;
            const sim::ImplicitCost cpuCost = sim::StepImplicit(cpu, options);
            if (options.cycles == scene.options.cycles) {
                if (auto same = CompareCost(scene.ticks[tick].cost, cpuCost); !same)
                    return std::unexpected(std::format("{} 刻み {}: {}", scene.name, tick, same.error()));
            }

            auto solved = SolveOnGpu(setup, scene.ticks[tick].grid, options, tuning, repeats);
            if (!solved)
                return std::unexpected(std::format("{} 刻み {}: {}", scene.name, tick, solved.error()));

            if (auto compared = CompareGpu(cpu, cpuCost, *solved); !compared)
                return std::unexpected(std::format("{} 刻み {}: {}", scene.name, tick, compared.error()));

            results.push_back(std::move(*solved));
        }

        return results;
    }

    // --- 場面を作る(CPU)---

    constexpr uint64_t HOT_TICKS = 12;
    // debug の HW は GPU-based validation で重い(刻みごとに Create する)ので鎖を短く
    constexpr uint64_t CHAIN_TICKS = RELEASE ? 36 : 24;
    constexpr uint64_t CHAIN_EARLY_TICKS = 12;
    constexpr uint64_t STRESS_TICKS = 8;

    std::vector<TreeScene> MakeScenes(const sim::BakedReactionTable& table) {
        std::vector<TreeScene> scenes;
        scenes.push_back(HotPointScene(table, HOT_TICKS));
        scenes.push_back(ChainScene(table, CHAIN_TICKS));
        scenes.push_back(StressScene(table, STRESS_TICKS));

        return scenes;
    }

    std::string ShapeText(const sim::ImplicitGrid& grid) {
        std::string text;
        for (const sim::ImplicitGridLevel& level : grid.levels) {
            uint32_t widest = 0;
            for (size_t i = 0; i + 1 < level.rowStarts.size(); ++i)
                widest = std::max(widest, level.rowStarts[i + 1] - level.rowStarts[i]);

            text += std::format("[{}, {}] ", level.levels.size(), widest);
        }

        return text;
    }

    // 段の分け方を変えても同じ値か(既定ではたくさんの要求の最も粗い段〔3396 節〕は ImTail に入らない。入れる形 = T-0120 の既定)
    constexpr sim::GpuImplicitTuning COARSEST_TAIL = {
        .tailMaxNodes = 1024, .tailMaxLinks = 32, .coarsestTailMaxNodes = 4096};

    std::expected<void, std::string> CheckAll(const SolveSetup& setup, const std::vector<TreeScene>& scenes) {
        for (size_t s = 0; s < scenes.size(); ++s) {
            const TreeScene& scene = scenes[s];
            if (scene.ticks.empty())
                return std::unexpected(std::format("{}: 陰解法の系が無い(場面が試験になっていない)", scene.name));

            const bool last = s + 1 == scenes.size();
            if (last && RELEASE) {
                if (auto other = RunScene(setup, scene, scene.options, COARSEST_TAIL, 1); !other)
                    return std::unexpected(std::format("最も粗い段を ImTail で回す形: {}", other.error()));
            }

            const auto results = RunScene(setup, scene, scene.options, {}, 1);
            if (!results)
                return std::unexpected(results.error());

            uint32_t maxCycles = 0;
            size_t maxCells = 0;
            for (size_t tick = 0; tick < results->size(); ++tick) {
                maxCycles = std::max(maxCycles, (*results)[tick].cost.cycles);
                maxCells = std::max(maxCells, scene.ticks[tick].grid.cells.size());
            }

            Log(Channel::Sim, Level::Info, "{}: {} 刻み一致(V サイクル最大 {}・系のセル最大 {}・最後の段 {})",
                scene.name, results->size(), maxCycles, maxCells, ShapeText(scene.ticks.back().grid));
        }

        return {};
    }

    // --- 計測 ---

    constexpr uint32_t REPEATS = 5;

    double Mean(std::span<const Solved> results) {
        double sum = 0.0;
        for (const Solved& solved : results)
            sum += solved.stepMs;

        return results.empty() ? 0.0 : sum / static_cast<double>(results.size());
    }

    std::string Series(std::span<const Solved> results) {
        std::string text;
        for (const Solved& solved : results)
            text += std::format("{:.2f}({}) ", solved.stepMs, solved.cost.cycles);

        return text;
    }

    struct MeasureCase {
        uint32_t maxCycles = 0;
        sim::GpuImplicitTuning tuning;
    };

    std::expected<void, std::string> MeasureScene(const SolveSetup& setup, const TreeScene& scene,
                                                  const MeasureCase& measure) {
        sim::ImplicitOptions options = scene.options;
        options.cycles = measure.maxCycles;
        const auto results = RunScene(setup, scene, options, measure.tuning, REPEATS);
        if (!results)
            return std::unexpected(results.error());

        const std::span<const Solved> all(*results);
        const size_t early = std::min<size_t>(all.size(), CHAIN_EARLY_TICKS);
        const Solved& last = all.back();
        Log(Channel::Sim, Level::Info,
            "計測 {}(上限 {}・ImTail 節 {} 隣 {} → 段 {} / {}・V 1 回 {} Dispatch)ms/刻み(V): 初めの {} 刻みの平均 "
            "{:.3f}・"
            "後の平均 {:.3f} / {}",
            scene.name, measure.maxCycles, measure.tuning.tailMaxNodes, measure.tuning.tailMaxLinks, last.tailDepth,
            last.levelCount, last.dispatchesPerCycle, early, Mean(all.first(early)), Mean(all.subspan(early)),
            Series(all));

        return {};
    }

    std::expected<void, std::string> MeasureAll(const SolveSetup& setup, const std::vector<TreeScene>& scenes) {
        const std::vector<MeasureCase> cases = {
            {.maxCycles = 64, .tuning = {}},
            {.maxCycles = 16, .tuning = {}},
            {.maxCycles = 16, .tuning = {.tailMaxNodes = 1024, .tailMaxLinks = 128}},
            {.maxCycles = 16, .tuning = {.tailMaxNodes = 2048, .tailMaxLinks = 256}},
            {.maxCycles = 16, .tuning = COARSEST_TAIL},
            {.maxCycles = 16, .tuning = {.tailMaxNodes = 256, .tailMaxLinks = 32}},
        };
        for (const MeasureCase& measure : cases) {
            for (const TreeScene& scene : scenes) {
                if (auto measured = MeasureScene(setup, scene, measure); !measured)
                    return measured;
            }
        }

        return {};
    }

    int Run(std::span<char*> arguments) {
        std::vector<char*> rest;
        bool measureOnly = false;
        for (char* argument : arguments) {
            if (std::string_view(argument) == "--measure-only")
                measureOnly = true;
            else
                rest.push_back(argument);
        }

        const auto options = test::ParseGpuTestOptions(rest);
        if (!options) {
            Log(Channel::Gpu, Level::Error,
                "使い方: gpu_multires_implicit_tree_test [--warp] [--queue direct|compute] [--measure-only]");
            return 2;
        }

        Log(Channel::Gpu, Level::Info, "gpu_multires_implicit_tree_test: adapter {}, queue {}",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType));
        const auto table = sim::BakeReactionTable(sim::MakeCombustionTestTable());
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!table || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_implicit_tree_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        if (!queue || !ring) {
            Log(Channel::Gpu, Level::Error,
                "gpu_multires_implicit_tree_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        const std::vector<TreeScene> scenes = MakeScenes(*table);
        SolveSetup setup{.device = device->Get(), .queue = &*queue, .ring = &*ring, .frequency = 0};

        if (RELEASE && options->adapter != gpu::AdapterKind::Warp) {
            SolveSetup measure = setup;
            if (FAILED(queue->Native()->GetTimestampFrequency(&measure.frequency)))
                measure.frequency = 0;

            if (auto measured = MeasureAll(measure, scenes); !measured) {
                Log(Channel::Gpu, Level::Error, "gpu_multires_implicit_tree_test: FAILED(計測: {})", measured.error());
                return 1;
            }
        }

        if (measureOnly)
            return 0;

        if (auto result = CheckAll(setup, scenes); !result) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_implicit_tree_test: FAILED ({})", result.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_multires_implicit_tree_test"))
            return 1;

        Log(Channel::Gpu, Level::Info,
            "gpu_multires_implicit_tree_test: OK(木の陰解法の系を GPU と CPU が毎刻みビット一致)");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
