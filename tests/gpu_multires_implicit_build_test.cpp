// gpu_multires_implicit_build_test.cpp — 木につないだ陰解法(方式②)の系のうち、未知数・境のセル・面・セルの面の一覧を GPU で作り
// (sim::GpuImplicitBuild。T-0129)、CPU の系(multires_implicit_conduction.cpp の MakeSystem)と毎刻みビット一致するか、
// その系を GPU の GpuImplicit で解いた結果が CPU の StepImplicit とビット一致するかを確かめる。
// 系を作る時の木は CPU の木の写し(MultiresNest::captureImplicitNest。陽解法の流れの後・変化を足す前)を GpuMultires に写して使う。
// 多重格子の段・重み・節の並びはまだ CPU が作る(T-0134)ので、GpuImplicit の段の形は CPU の系から作り、セル・面・面の一覧だけを
// GPU が作ったもので上書きする(CPU から写すセルのエネルギーと刻みの初めの温度は 0 にして、GPU が作った値で解いたことを確かめる)。
// 場面(gpu_multires_implicit_tree_test と同じ): 熱い点・鎖・たくさんの要求。
// 計測(release のハードウェアだけ。--measure-only なら計測だけ): 系を作る段の ms(暖機の後、同じ木で REPEATS 回作った最小)。
#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
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
#include "multires_activity_scene.h"
#include "multires_conduction_scene.h"
#include "sim/gpu_implicit.h"
#include "sim/gpu_implicit_build.h"
#include "sim/gpu_multires.h"
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

    constexpr uint64_t HOT_TICKS = 6;
    constexpr uint64_t CHAIN_TICKS = 12;
    constexpr uint64_t STRESS_TICKS = 4;
    constexpr uint32_t WARMUP_BUILDS = 20;
    constexpr uint32_t REPEATS = 5;

    // --- 場面: 刻みごとの系(解く前)と、それを作った時の木 ---

    struct BuildTick {
        sim::ImplicitGrid grid;
        std::shared_ptr<const sim::MultiresNest> nest;
        std::vector<uint8_t> frozen;
    };

    struct BuildScene {
        std::string name;
        std::vector<BuildTick> ticks;
        sim::MultiresStepOptions options;
    };

    // 案 a: 方式①は分けない + 基準より細かい所は全部②(D-434。gpu_multires_implicit_tree_test と同じ)
    sim::MultiresStepOptions TreeOptions(const sim::BakedReactionTable& table) {
        sim::MultiresStepOptions options = test::SubcycleTestOptions(table, 0);
        options.implicitConduction = true;

        return options;
    }

    sim::ImplicitOptions SolveOptions(const sim::MultiresStepOptions& options) {
        return {
            .method = sim::ImplicitMethod::Multigrid, .cycles = options.implicitMaxCycles, .toleranceMillikelvin = 1};
    }

    void Capture(const sim::MultiresNest& nest, BuildScene& scene) {
        if (nest.implicitGrid.cells.empty() || !nest.implicitNest)
            return;

        scene.ticks.push_back({.grid = nest.implicitGrid, .nest = nest.implicitNest, .frozen = nest.implicitFrozen});
    }

    void Watch(sim::MultiresNest& nest) {
        nest.captureImplicitGrid = true;
        nest.captureImplicitNest = true;
    }

    BuildScene HotPointScene(const sim::BakedReactionTable& table) {
        constexpr int32_t HOT_LEVEL = 6;
        sim::MultiresCapacity capacity = test::MakeMultiresCapacity(table, 1, 0, 0);
        capacity.rootLevel = HOT_LEVEL;
        sim::MultiresNest nest = sim::MakeMultiresNest(capacity);
        std::vector<reaction::RxCell> cells(MR_BLOCK_CELLS, test::MakeConductionAir(table, 300000));
        cells[MrCellIndex(3, 4, 5)] = test::MakeConductionAir(table, 1500000);
        sim::PlaceRootBlock(nest, 0, 0, 0, cells);
        Watch(nest);

        BuildScene scene{.name = "熱い点", .ticks = {}, .options = TreeOptions(table)};
        for (uint64_t tick = 0; tick < HOT_TICKS; ++tick) {
            sim::StepNest(nest, table, test::CONDUCTION_SEED, tick, scene.options);
            Capture(nest, scene);
        }

        return scene;
    }

    BuildScene ChainScene(const sim::BakedReactionTable& table) {
        sim::MultiresNest nest = test::MakeChainNest(table, 16);
        Watch(nest);

        BuildScene scene{.name = "鎖", .ticks = {}, .options = TreeOptions(table)};
        for (uint64_t tick = 0; tick < CHAIN_TICKS; ++tick) {
            sim::StepNest(nest, table, test::CONDUCTION_SEED, tick, scene.options);
            Capture(nest, scene);
        }

        return scene;
    }

    BuildScene StressScene(const sim::BakedReactionTable& table) {
        sim::MultiresNest nest = test::MakeActivityNest(table);
        Watch(nest);

        BuildScene scene{.name = "たくさんの要求", .ticks = {}, .options = TreeOptions(table)};
        for (uint64_t tick = 0; tick < STRESS_TICKS; ++tick) {
            const std::vector<MrRequest> requests = test::MakeStressRequests(nest, tick);
            test::BeginActivityTick(nest, tick, requests);
            test::EndActivityTick(nest, table, tick, false, scene.options);
            Capture(nest, scene);
        }

        return scene;
    }

    // --- CPU の系を GPU の並びに(gpu_implicit.cpp の MakeCellFaces・MakeCellImage と同じ)---

    sim::GpuImplicitSystem ExpectedSystem(const sim::ImplicitGrid& grid) {
        std::vector<std::vector<uint32_t>> perCell(grid.cells.size());
        for (uint32_t f = 0; f < grid.faces.size(); ++f) {
            perCell[grid.faces[f].fine].push_back(f * 2);
            perCell[grid.faces[f].coarse].push_back((f * 2) + 1);
        }

        sim::GpuImplicitSystem system;
        system.faces = static_cast<uint32_t>(grid.faces.size());
        for (size_t i = 0; i < grid.cells.size(); ++i) {
            const sim::ImplicitCell& cell = grid.cells[i];
            const auto start = static_cast<uint32_t>(system.lists.size());
            system.lists.insert(system.lists.end(), perCell[i].begin(), perCell[i].end());
            system.cells.push_back({.energy = cell.energy,
                                    .fraction = cell.fraction,
                                    .heatCapacity = cell.heatCapacity,
                                    .startTemperature = cell.startTemperature,
                                    .faceStart = start,
                                    .faceEnd = static_cast<uint32_t>(system.lists.size())});
        }

        for (const sim::ImplicitFace& face : grid.faces) {
            system.faceList.push_back({.coefficientHigh = face.coefficient.hi,
                                       .coefficientLow = face.coefficient.lo,
                                       .fine = face.fine,
                                       .coarse = face.coarse,
                                       .gap = face.gap,
                                       .coarseFraction = grid.cells[face.coarse].coarseFraction ? 1u : 0u});
        }

        return system;
    }

    bool SameCell(const ImGpuCell& a, const ImGpuCell& b) {
        return a.energy == b.energy && a.fraction == b.fraction && a.heatCapacity == b.heatCapacity &&
               a.startTemperature == b.startTemperature && a.faceStart == b.faceStart && a.faceEnd == b.faceEnd;
    }

    bool SameFace(const ImGpuFace& a, const ImGpuFace& b) {
        return a.coefficientHigh == b.coefficientHigh && a.coefficientLow == b.coefficientLow && a.fine == b.fine &&
               a.coarse == b.coarse && a.gap == b.gap && a.coarseFraction == b.coarseFraction;
    }

    std::expected<void, std::string> CompareSystem(const sim::GpuImplicitSystem& expected,
                                                   const sim::GpuImplicitSystem& gpu) {
        if (gpu.overflow)
            return std::unexpected("GPU の系が上限を超えた");

        if (gpu.cells.size() != expected.cells.size() || gpu.faces != expected.faces) {
            return std::unexpected(std::format("系の大きさが違う: セル {} / {}(未知数 {}・境 {})・面 {} / {}",
                                               expected.cells.size(), gpu.cells.size(), gpu.unknowns, gpu.boundary,
                                               expected.faces, gpu.faces));
        }

        for (size_t i = 0; i < expected.cells.size(); ++i) {
            if (!SameCell(expected.cells[i], gpu.cells[i])) {
                return std::unexpected(
                    std::format("セル {} が違う: エネルギー {} / {}・C {} / {}・面 [{}, {}) / [{}, {})", i,
                                expected.cells[i].energy, gpu.cells[i].energy, expected.cells[i].heatCapacity,
                                gpu.cells[i].heatCapacity, expected.cells[i].faceStart, expected.cells[i].faceEnd,
                                gpu.cells[i].faceStart, gpu.cells[i].faceEnd));
            }
        }

        for (size_t f = 0; f < expected.faceList.size(); ++f) {
            if (!SameFace(expected.faceList[f], gpu.faceList[f])) {
                return std::unexpected(std::format("面 {} が違う: 細かい {} / {}・粗い {} / {}・差 {} / {}", f,
                                                   expected.faceList[f].fine, gpu.faceList[f].fine,
                                                   expected.faceList[f].coarse, gpu.faceList[f].coarse,
                                                   expected.faceList[f].gap, gpu.faceList[f].gap));
            }
        }

        if (expected.lists != gpu.lists)
            return std::unexpected("セルの面の一覧が違う");

        return {};
    }

    std::expected<void, std::string> CompareSolved(const sim::ImplicitGrid& cpu, const sim::ImplicitCost& cpuCost,
                                                   const sim::ImplicitGrid& gpu, const sim::GpuImplicitCost& gpuCost) {
        for (size_t i = 0; i < cpu.cells.size(); ++i) {
            const sim::ImplicitCell& a = cpu.cells[i];
            const sim::ImplicitCell& b = gpu.cells[i];
            if (a.energy != b.energy || a.fraction != b.fraction) {
                return std::unexpected(std::format("解いたセル {}(レベル {})が食い違う: CPU {} + {} / GPU {} + {}", i,
                                                   a.level, a.energy, a.fraction, b.energy, b.fraction));
            }
        }

        if (!gpuCost.limitFinished)
            return std::unexpected("安全網が記録した回数の中で止まらない");

        if (cpuCost.cycles != gpuCost.cycles || cpuCost.limitedCells != gpuCost.limitedCells ||
            cpuCost.limitRounds != gpuCost.limitRounds ||
            cpuCost.worstExcessMillikelvin != gpuCost.worstExcessMillikelvin) {
            return std::unexpected(std::format("数が食い違う: V サイクル {} / {}・戻したセル {} / {}", cpuCost.cycles,
                                               gpuCost.cycles, cpuCost.limitedCells, gpuCost.limitedCells));
        }

        return {};
    }

    // --- GPU ---

    struct Context {
        ID3D12Device5* device = nullptr;
        gpu::ImmediateQueue* queue = nullptr;
        gpu::DebugRing* ring = nullptr;
        const sim::BakedReactionTable* table = nullptr;
        uint64_t frequency = 0;
    };

    // 1 本のリストを記録して投げる
    template <typename Record>
    std::expected<void, std::string> Execute(const Context& context, const Record& record) {
        ID3D12GraphicsCommandList10* list = context.queue->Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        context.ring->RecordBegin(list);
        if (!record(list))
            return std::unexpected("記録できない");

        context.ring->RecordReadbackAndReset(list);
        if (!context.queue->ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        const gpu::DebugRingContents debugOutput = context.ring->Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debugOutput.assertCount));

        return {};
    }

    sim::GpuImplicitBuildLimits SceneLimits(const BuildScene& scene) {
        sim::GpuImplicitBuildLimits limits{.unknowns = 1, .cells = 1};
        for (const BuildTick& tick : scene.ticks) {
            const auto cells = static_cast<uint32_t>(tick.grid.cells.size());
            limits.unknowns = std::max(limits.unknowns, cells);
            limits.cells = std::max(limits.cells, cells);
        }

        return limits;
    }

    // CPU から写すセルのエネルギー・刻みの初めの温度を 0 に(GPU が作った値で上書きされたことを確かめる)
    sim::ImplicitGrid Blank(const sim::ImplicitGrid& grid) {
        sim::ImplicitGrid blank = grid;
        for (sim::ImplicitCell& cell : blank.cells) {
            cell.energy = 0;
            cell.startTemperature = 0;
        }

        return blank;
    }

    // 1 刻み: 木を写して系を作り、読み戻して CPU と比べ、GpuImplicit へ写して解いて CPU と比べる
    std::expected<void, std::string> CheckTick(const Context& context, sim::GpuMultires& multires,
                                               sim::GpuImplicitBuild& build, const BuildScene& scene,
                                               const BuildTick& tick) {
        if (std::ranges::any_of(tick.frozen, [](uint8_t frozen) { return frozen != 0; }))
            return std::unexpected("凍った枠がある刻み(この試験は凍った印を GPU に写さない)");

        auto implicit = sim::GpuImplicit::Create(context.device, tick.grid);
        if (!implicit)
            return std::unexpected(implicit.error());

        const sim::ImplicitGrid blank = Blank(tick.grid);
        const sim::ImplicitOptions solveOptions = SolveOptions(scene.options);
        const auto record = [&](ID3D12GraphicsCommandList10* list) {
            const D3D12_GPU_VIRTUAL_ADDRESS ring = context.ring->GpuAddress();
            if (!multires.RecordUpload(list, *tick.nest))
                return false;

            build.RecordBuild(list, ring, multires, scene.options);
            build.RecordReadback(list);
            if (!implicit->RecordUpload(list, blank))
                return false;

            build.RecordCopyTo(list, *implicit);
            if (!implicit->RecordStep(list, ring, solveOptions))
                return false;

            implicit->RecordReadback(list);
            return true;
        };
        if (auto executed = Execute(context, record); !executed)
            return executed;

        auto system = build.Read();
        if (!system)
            return std::unexpected(system.error());

        if (auto same = CompareSystem(ExpectedSystem(tick.grid), *system); !same)
            return same;

        sim::ImplicitGrid cpu = tick.grid;
        const sim::ImplicitCost cpuCost = sim::StepImplicit(cpu, solveOptions);
        sim::ImplicitGrid gpu = tick.grid;
        sim::GpuImplicitCost gpuCost;
        if (!implicit->Read(gpu, gpuCost))
            return std::unexpected("解いた結果を読み戻せない");

        return CompareSolved(cpu, cpuCost, gpu, gpuCost);
    }

    std::expected<sim::GpuMultires, std::string> MakeMultires(const Context& context, const BuildScene& scene) {
        return sim::GpuMultires::Create(context.device, *context.table, scene.ticks.front().nest->capacity);
    }

    std::expected<void, std::string> CheckScene(const Context& context, const BuildScene& scene) {
        if (scene.ticks.empty())
            return std::unexpected(std::format("{}: 陰解法の刻みが無い", scene.name));

        auto multires = MakeMultires(context, scene);
        if (!multires)
            return std::unexpected(multires.error());

        auto build = sim::GpuImplicitBuild::Create(context.device, *multires, scene.ticks.front().nest->capacity,
                                                   SceneLimits(scene));
        if (!build)
            return std::unexpected(build.error());

        for (size_t i = 0; i < scene.ticks.size(); ++i) {
            if (auto checked = CheckTick(context, *multires, *build, scene, scene.ticks[i]); !checked)
                return std::unexpected(std::format("{} 刻み {}: {}", scene.name, i, checked.error()));
        }

        Log(Channel::Gpu, Level::Info, "  {}: {} 刻みの系(最後 {} セル・{} 面)が CPU とビット一致・解いた結果も一致",
            scene.name, scene.ticks.size(), scene.ticks.back().grid.cells.size(), scene.ticks.back().grid.faces.size());

        return {};
    }

    // 段ごとの内訳(段の間にタイムスタンプを打つので、合計は上の 1 回より少し大きい。REPEATS 回の段ごとの最小)
    std::expected<void, std::string> MeasurePasses(const Context& context, sim::GpuMultires& multires,
                                                   sim::GpuImplicitBuild& build, const BuildScene& scene,
                                                   const BuildTick& tick) {
        const uint32_t passes = sim::GpuImplicitBuild::StageCount();
        std::vector<double> best(passes, 0.0);
        build.StampPasses(true);
        for (uint32_t repeat = 0; repeat < REPEATS; ++repeat) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                if (!multires.RecordUpload(list, *tick.nest))
                    return false;

                build.RecordBuild(list, context.ring->GpuAddress(), multires, scene.options);
                build.RecordReadback(list);
                return true;
            };
            if (auto executed = Execute(context, record); !executed)
                return executed;

            const std::vector<uint64_t> stamps = build.ReadTimestamps(passes + 1);
            if (stamps.size() != passes + 1)
                return std::unexpected("タイムスタンプを読めない");

            for (uint32_t pass = 0; pass < passes; ++pass) {
                const double milliseconds = static_cast<double>(stamps[pass + 1] - stamps[pass]) * 1000.0 /
                                            static_cast<double>(context.frequency);
                best[pass] = repeat == 0 ? milliseconds : std::min(best[pass], milliseconds);
            }
        }

        build.StampPasses(false);
        std::string text;
        for (uint32_t pass = 0; pass < passes; ++pass)
            text += std::format(" {} {:.3f}", sim::GpuImplicitBuild::StageName(pass), best[pass]);

        Log(Channel::Gpu, Level::Info, "    段ごと(ms):{}", text);

        return {};
    }

    // 系を作る段の ms: 最後の刻みの木で、暖機の後 REPEATS 回作った最小
    std::expected<void, std::string> MeasureScene(const Context& context, const BuildScene& scene) {
        auto multires = MakeMultires(context, scene);
        if (!multires)
            return std::unexpected(multires.error());

        const BuildTick& tick = scene.ticks.back();
        auto build = sim::GpuImplicitBuild::Create(context.device, *multires, tick.nest->capacity, SceneLimits(scene));
        if (!build)
            return std::unexpected(build.error());

        double best = 0.0;
        for (uint32_t repeat = 0; repeat < REPEATS; ++repeat) {
            const auto record = [&](ID3D12GraphicsCommandList10* list) {
                const D3D12_GPU_VIRTUAL_ADDRESS ring = context.ring->GpuAddress();
                if (!multires->RecordUpload(list, *tick.nest))
                    return false;

                for (uint32_t i = 0; i < WARMUP_BUILDS; ++i)
                    build->RecordBuild(list, ring, *multires, scene.options);

                build->RecordTimestamp(list, 0);
                build->RecordBuild(list, ring, *multires, scene.options);
                build->RecordTimestamp(list, 1);
                build->RecordReadback(list);
                return true;
            };
            if (auto executed = Execute(context, record); !executed)
                return executed;

            const std::vector<uint64_t> stamps = build->ReadTimestamps(2);
            if (stamps.size() != 2)
                return std::unexpected("タイムスタンプを読めない");

            const double milliseconds = static_cast<double>(stamps[1] - stamps[0]) * 1000.0 /
                                        static_cast<double>(context.frequency);
            best = repeat == 0 ? milliseconds : std::min(best, milliseconds);
        }

        Log(Channel::Gpu, Level::Info, "  計測 {}: 系を作る段 {:.3f} ms(セル {}・面 {}・世界の枠 {})", scene.name, best,
            tick.grid.cells.size(), tick.grid.faces.size(), tick.nest->capacity.worldBlocks);

        return MeasurePasses(context, *multires, *build, scene, tick);
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
                "使い方: gpu_multires_implicit_build_test [--warp] [--queue direct|compute] [--measure-only]");
            return 2;
        }

        Log(Channel::Gpu, Level::Info, "gpu_multires_implicit_build_test: adapter {}, queue {}",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType));
        const auto table = sim::BakeReactionTable(sim::MakeCombustionTestTable());
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!table || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_multires_implicit_build_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        auto ring = gpu::DebugRing::Create(device->Get());
        if (!queue || !ring) {
            Log(Channel::Gpu, Level::Error,
                "gpu_multires_implicit_build_test: FAILED(キューかデバッグのリングを作れない)");
            return 1;
        }

        Context context{.device = device->Get(), .queue = &*queue, .ring = &*ring, .table = &*table, .frequency = 0};
        const std::vector<BuildScene> scenes = {HotPointScene(*table), ChainScene(*table), StressScene(*table)};

        if (RELEASE && options->adapter != gpu::AdapterKind::Warp &&
            SUCCEEDED(queue->Native()->GetTimestampFrequency(&context.frequency))) {
            for (const BuildScene& scene : scenes) {
                if (auto measured = MeasureScene(context, scene); !measured) {
                    Log(Channel::Gpu, Level::Error, "gpu_multires_implicit_build_test: FAILED(計測 {}: {})", scene.name,
                        measured.error());
                    return 1;
                }
            }
        }

        if (measureOnly)
            return 0;

        for (const BuildScene& scene : scenes) {
            if (auto checked = CheckScene(context, scene); !checked) {
                Log(Channel::Gpu, Level::Error, "gpu_multires_implicit_build_test: FAILED ({})", checked.error());
                return 1;
            }
        }

        if (!test::PassesValidation(*device, "gpu_multires_implicit_build_test"))
            return 1;

        Log(Channel::Gpu, Level::Info,
            "gpu_multires_implicit_build_test: OK(陰解法の系を GPU で作って CPU と毎刻みビット一致・解いた結果も一致)");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
