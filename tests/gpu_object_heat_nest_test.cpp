// gpu_object_heat_nest_test.cpp — 物の熱の GPU の段を多重解像度の世界(GpuMultires)のセルにつないだ形を、CPU リファレンスとビット一致で確かめる
// (T-1077・ADR-0451・ADR-0360)。CPU は tests/object_heat_nest_scene.h(object_heat_nest と同じ場面と口)。
// 1 刻み(GPU)= 世界の刻み(RecordStep。伝導つき)→ 囲むセルを写す(shaders/sim/object_heat_nest.hlsl の OhNestGather)
//   → 物の熱の段(shaders/sim/object_heat.hlsl。gpu_object_heat_stages.h の RecordTick。起きている物の一覧の形)
//   → 変わったセルの一様のブロックを頁に広げる(OhNestMarkExpand → TreeExpand → OhNestFill)→ 変わったセルを書き戻す(OhNestScatter。帳簿とつつき)。
// 確かめる: 0.5 m と 0.25 m の世界、物の囲むセルが頁を持つ根にある場面と一様の空気の根にある場面(頁に広げる。T-1077 ①)で、
//   30 刻みごとに 世界のセル(全部)・世界の帳簿・頁に広げた数と空きの頁の数・物のボクセル・囲むセル・対の端数・待ち・ブロックの状態が CPU とビット一致、
//   一様のブロックへ書けなかった数が 0。一様の空気の場面は頁に広げた数が 1 以上。
//   --measure: 0.5 m の世界を 300 刻み回した後の 1 刻みの GPU の時間(世界の刻み・写す・物の熱・広げて書き戻す)を出す。
// 引数は gpu_test_options.h と --measure。
#include <array>
#include <cstring>

#include "core/log.h"
#include "core/singleton.h"
#include "gpu_object_heat_stages.h"
#include "gpu_test_options.h"
#include "object_heat_nest_scene.h"
#include "sim/gpu_multires.h"

using namespace bicameral;
namespace rx = bicameral::reaction;

namespace {

    using namespace test::object_heat_gpu;   // 物の熱の GPU の段(gpu_object_heat_stages.h)
    using namespace test::object_heat_nest;  // 場面と CPU の口(object_heat_nest_scene.h)

    constexpr uint32_t COMPARE_TICKS = 120;
    constexpr uint32_t COMPARE_EVERY = 30;
    constexpr uint32_t MEASURE_WARM_TICKS = 300;
    constexpr uint32_t NEST_THREADS = 64;  // object_heat_nest.hlsl の NEST_GROUP_THREADS
    constexpr uint32_t STAMP_COUNT = 5;    // 刻みの初め・世界の刻み・写す・物の熱・広げて書き戻すの後

    struct Context {
        ID3D12Device5* device;
        D3D12_COMMAND_LIST_TYPE queueType;
        const sim::BakedReactionTable& table;
        const test::OhTables& tables;
    };

    // GPU の側の全部(世界の木・物の熱の世界・2 つの口のパイプライン)
    struct GpuNestScene {
        sim::GpuMultires nest;
        GpuWorld objects;
        Pipelines stages;
        ComPtr<ID3D12PipelineState> gather;
        ComPtr<ID3D12PipelineState> markExpand;
        ComPtr<ID3D12PipelineState> fill;
        ComPtr<ID3D12PipelineState> scatter;
        uint32_t cellCount = 0;
    };

    // 物の熱の世界の囲むセルと変わった段の並びに、口が使う後ろの部分を足す(object_heat_nest.hlsl の u4・u5 の並び)。
    // 物の熱の段は前の N 個だけを読むので、後ろは段に見えない
    test::OhWorld WithNestTails(const NestScene& scene) {
        test::OhWorld world = scene.world;
        const auto count = static_cast<uint32_t>(scene.refs.size());
        for (uint32_t c = 0; c < count; ++c)
            world.cells.push_back(scene.world.cells[c]);

        for (const NestCellRef& ref : scene.refs) {
            world.cellChangedPasses.push_back(ref.slot);
            world.cellChangedPasses.push_back(ref.index);
        }

        world.cellChangedPasses.push_back(0);  // 一様のブロックへ書けなかった数

        return world;
    }

    std::expected<GpuNestScene, std::string> MakeGpuScene(const Context& context, gpu::ImmediateQueue& queue,
                                                          const NestScene& scene) {
        auto nest = sim::GpuMultires::Create(context.device, context.table, scene.nest.capacity);
        if (!nest)
            return std::unexpected(nest.error());

        auto stages = CreatePipelines(context.device);
        auto objects = CreateGpuWorld(context.device, context.table, context.tables, WithNestTails(scene), WORLD_SEED);
        const auto gatherShader = gpu::LoadShader("sim/object_heat_nest_gather.cso");
        const auto scatterShader = gpu::LoadShader("sim/object_heat_nest_scatter.cso");
        const auto markShader = gpu::LoadShader("sim/object_heat_nest_mark_expand.cso");
        const auto fillShader = gpu::LoadShader("sim/object_heat_nest_fill.cso");
        if (!stages || !objects || !gatherShader || !scatterShader || !markShader || !fillShader)
            return std::unexpected("物の熱の段か口を作れない");

        GpuNestScene result{.nest = std::move(*nest),
                            .objects = std::move(*objects),
                            .stages = std::move(*stages),
                            .gather = nullptr,
                            .markExpand = nullptr,
                            .fill = nullptr,
                            .scatter = nullptr,
                            .cellCount = static_cast<uint32_t>(scene.refs.size())};
        result.gather = gpu::CreateComputePipeline(context.device, result.nest.RootSignature(), *gatherShader);
        result.scatter = gpu::CreateComputePipeline(context.device, result.nest.RootSignature(), *scatterShader);
        result.markExpand = gpu::CreateComputePipeline(context.device, result.nest.RootSignature(), *markShader);
        result.fill = gpu::CreateComputePipeline(context.device, result.nest.RootSignature(), *fillShader);
        if (!result.gather || !result.scatter || !result.markExpand || !result.fill ||
            !UploadWorld(queue, result.objects))
            return std::unexpected("口のパイプラインか物の世界を作れない");

        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr || !result.nest.RecordUpload(list, scene.nest) || !queue.ExecuteAndWait())
            return std::unexpected("世界の木を写せない");

        return result;
    }

    // 1 刻み(GPU)を記録する。stamps なら段ごとに世界の木のタイムスタンプ(1〜4)
    void RecordNestTick(ID3D12GraphicsCommandList10* list, GpuNestScene& gpuScene, gpu::DebugRing& debugRing,
                        uint64_t tick, bool stamps) {
        const D3D12_GPU_VIRTUAL_ADDRESS ring = debugRing.GpuAddress();
        const uint32_t groups = (gpuScene.cellCount + NEST_THREADS - 1) / NEST_THREADS;
        const auto passBefore = static_cast<uint32_t>((tick - 1) * gpuScene.objects.passesPerTick);

        // --- 世界の刻み(CPU の StepNestScene と同じ: 伝導つき)---
        gpuScene.nest.RecordStep(list, ring, WORLD_SEED, tick, {.conduction = true});
        Barrier(list);
        if (stamps)
            gpuScene.nest.RecordTimestamp(list, 1);

        // --- 囲むセルを写す(変わったセルは物のブロックを起こす)---
        gpuScene.nest.SetExternalViews(gpuScene.objects.uavs[UavCells]->GetGPUVirtualAddress(),
                                       gpuScene.objects.uavs[UavCellChanged]->GetGPUVirtualAddress());
        gpuScene.nest.RecordExternalDispatch(list, ring, gpuScene.gather.Get(), groups,
                                             {gpuScene.cellCount, passBefore, 0, 0});
        if (stamps)
            gpuScene.nest.RecordTimestamp(list, 2);

        // --- 物の熱の段(自分のルート署名)---
        BindRoot(list, gpuScene.stages, gpuScene.objects, debugRing);
        RecordTick(list, gpuScene.stages, gpuScene.objects, false, tick, false);
        if (stamps)
            gpuScene.nest.RecordTimestamp(list, 3);

        // --- 変わったセルの一様のブロックを頁に広げる(印 → 枠の順に頁を配る → 一様の値で埋める。CPU の ExpandChangedBlocks)---
        const std::array<uint32_t, 4> cells = {gpuScene.cellCount, 0, 0, 0};
        gpuScene.nest.RecordExternalDispatch(list, ring, gpuScene.markExpand.Get(), groups, cells);
        gpuScene.nest.RecordExpandPass(list, ring);
        gpuScene.nest.RecordExternalDispatch(list, ring, gpuScene.fill.Get(), gpuScene.cellCount, cells);

        // --- 変わったセルを世界へ書き戻す(帳簿・つつく)---
        gpuScene.nest.RecordExternalDispatch(list, ring, gpuScene.scatter.Get(), groups, cells);
        if (stamps)
            gpuScene.nest.RecordTimestamp(list, 4);
    }

    std::expected<void, std::string> RunGpuTicks(gpu::ImmediateQueue& queue, GpuNestScene& gpuScene,
                                                 gpu::DebugRing& debugRing, uint64_t first, uint32_t count,
                                                 bool stamps) {
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        debugRing.RecordBegin(list);
        if (stamps)
            gpuScene.nest.RecordTimestamp(list, 0);

        for (uint32_t i = 0; i < count; ++i)
            RecordNestTick(list, gpuScene, debugRing, first + i, stamps);

        gpuScene.nest.RecordReadback(list);
        debugRing.RecordReadbackAndReset(list);
        if (!queue.ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        const gpu::DebugRingContents debugOutput = debugRing.Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debugOutput.assertCount));

        return {};
    }

    // 世界(セル全部・帳簿)と物の熱の世界を CPU と比べる。食い違いの数
    std::expected<size_t, std::string> CompareScene(const Context& context, gpu::ImmediateQueue& queue,
                                                    const GpuNestScene& gpuScene, const NestScene& cpu) {
        sim::MultiresNest read = cpu.nest;
        if (!gpuScene.nest.Read(read))
            return std::unexpected("世界の木を読み戻せない");

        const std::vector<rx::RxCell> cpuCells = sim::DenseCellImage(cpu.nest);
        const std::vector<rx::RxCell> gpuCells = sim::DenseCellImage(read);
        size_t different = CountDifferent<rx::RxCell>(cpuCells, gpuCells, "世界のセル");
        if (read.ledger != cpu.nest.ledger) {
            Log(Channel::Gpu, Level::Error, "世界の帳簿が食い違った");
            ++different;
        }

        for (const uint32_t counter :
             {multires::MR_COUNTER_EXPANDED, multires::MR_COUNTER_FREE_PAGES, multires::MR_COUNTER_PAGE_SHORTAGE}) {
            if (read.counters[counter] == cpu.nest.counters[counter])
                continue;

            Log(Channel::Gpu, Level::Error, "世界の数え {} が食い違った: cpu {} / gpu {}", counter,
                cpu.nest.counters[counter], read.counters[counter]);
            ++different;
        }

        const auto objects = CompareWorld(context.device, queue, gpuScene.objects, cpu.world);
        const auto words = ReadUav<uint32_t>(context.device, queue, gpuScene.objects.uavs[UavCellChanged].Get(),
                                             (3 * gpuScene.cellCount) + 1);
        if (!objects || !words)
            return std::unexpected("物の熱の世界を読み戻せない");

        const uint32_t unstored = words->back();
        if (unstored != 0)
            Log(Channel::Gpu, Level::Error, "一様のブロックへ書けなかったセル {}", unstored);

        return different + *objects + unstored;
    }

    std::expected<uint64_t, std::string> RunCompare(const Context& context, int32_t level, bool uniformAir) {
        NestScene cpu = MakeNestScene(context.table, level, uniformAir);
        const uint32_t expandedBefore = cpu.nest.counters[multires::MR_COUNTER_EXPANDED];
        auto queue = gpu::ImmediateQueue::Create(context.device, context.queueType);
        auto debugRing = gpu::DebugRing::Create(context.device);
        if (!queue || !debugRing)
            return std::unexpected("キューを作れない");

        auto gpuScene = MakeGpuScene(context, *queue, cpu);
        if (!gpuScene)
            return std::unexpected(gpuScene.error());

        for (uint32_t tick = 1; tick <= COMPARE_TICKS; tick += COMPARE_EVERY) {
            if (auto ran = RunGpuTicks(*queue, *gpuScene, *debugRing, tick, COMPARE_EVERY, false); !ran)
                return std::unexpected(ran.error());

            for (uint32_t t = tick; t < tick + COMPARE_EVERY; ++t) {
                if (!StepNestScene(context.table, context.tables, cpu, t))
                    return std::unexpected(std::format("CPU の刻み {} で書けなかった", t));
            }

            const auto different = CompareScene(context, *queue, *gpuScene, cpu);
            if (!different)
                return std::unexpected(different.error());

            if (*different != 0)
                return std::unexpected(std::format("刻み {} で GPU と CPU が食い違った", tick + COMPARE_EVERY - 1));
        }

        uint64_t digest = 0;
        for (const rx::RxCell& voxel : cpu.world.voxels)
            digest = fx::FxHashCombine(digest, sim::HashReactionCell(voxel));

        for (const rx::RxCell& cell : sim::DenseCellImage(cpu.nest))
            digest = fx::FxHashCombine(digest, sim::HashReactionCell(cell));

        const uint32_t expanded = cpu.nest.counters[multires::MR_COUNTER_EXPANDED] - expandedBefore;
        if (uniformAir && expanded == 0)
            return std::unexpected("一様の空気の根を頁に広げなかった");

        Log(Channel::Gpu, Level::Info,
            "レベル {} の世界({}): {} 刻み、世界のセル・帳簿・物の熱が GPU と CPU でビット一致(頁に広げた {}・"
            "回したブロック 受け渡し {}・反応 {})",
            level, uniformAir ? "一様の空気の根" : "頁を持つ根", COMPARE_TICKS, expanded, cpu.world.awakeExchangeBlocks,
            cpu.world.awakeReactBlocks);

        return digest;
    }

    // 0.5 m の世界を暖めた後の 1 刻みの GPU の時間(ms。世界の刻み・写す・物の熱・書き戻す)
    std::expected<void, std::string> Measure(const Context& context) {
        const NestScene cpu = MakeNestScene(context.table, 0);
        auto queue = gpu::ImmediateQueue::Create(context.device, context.queueType);
        auto debugRing = gpu::DebugRing::Create(context.device);
        if (!queue || !debugRing)
            return std::unexpected("キューを作れない");

        auto gpuScene = MakeGpuScene(context, *queue, cpu);
        if (!gpuScene)
            return std::unexpected(gpuScene.error());

        for (uint32_t done = 0; done < MEASURE_WARM_TICKS; done += COMPARE_EVERY) {
            if (auto ran = RunGpuTicks(*queue, *gpuScene, *debugRing, 1 + done, COMPARE_EVERY, false); !ran)
                return std::unexpected(ran.error());
        }

        std::array<double, STAMP_COUNT - 1> sums{};
        constexpr uint32_t REPEATS = 8;
        for (uint32_t r = 0; r < REPEATS; ++r) {
            if (auto ran = RunGpuTicks(*queue, *gpuScene, *debugRing, 1 + MEASURE_WARM_TICKS + r, 1, true); !ran)
                return std::unexpected(ran.error());

            uint64_t frequency = 0;
            const std::vector<uint64_t> stamps = gpuScene->nest.ReadTimestamps(STAMP_COUNT);
            if (stamps.size() != STAMP_COUNT || FAILED(queue->Native()->GetTimestampFrequency(&frequency)) ||
                frequency == 0)
                return std::unexpected("タイムスタンプを読めない");

            for (size_t s = 0; s + 1 < STAMP_COUNT; ++s)
                sums[s] += static_cast<double>(stamps[s + 1] - stamps[s]) * 1000.0 / static_cast<double>(frequency);
        }

        Log(Channel::Gpu, Level::Info,
            "計測(0.5 m の世界・物 5 つ・囲むセル 2・{} 刻みの後の 1 刻みの {} 回の平均): 世界の刻み {:.3f} ms・写す "
            "{:.3f} ms・"
            "物の熱 {:.3f} ms・広げて書き戻す {:.3f} ms",
            MEASURE_WARM_TICKS, REPEATS, sums[0] / REPEATS, sums[1] / REPEATS, sums[2] / REPEATS, sums[3] / REPEATS);

        return {};
    }

    int Run(std::span<char*> arguments) {
        std::vector<char*> rest;
        bool measure = false;
        for (char* argument : arguments) {
            if (std::string_view(argument) == "--measure")
                measure = true;
            else
                rest.push_back(argument);
        }

        const auto options = test::ParseGpuTestOptions(rest);
        if (!options) {
            Log(Channel::Gpu, Level::Error,
                "使い方: gpu_object_heat_nest_test [--warp] [--queue direct|compute] [--measure]");
            return 2;
        }

        const sim::BakedReactionTable table = MakeTable();
        const auto tables = test::MakeObjectHeatTables(table, DEFAULT_DIFFUSIVITY_MM2_PER_S);
        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!tables || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_object_heat_nest_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        const Context context{
            .device = device->Get(), .queueType = options->queueType, .table = table, .tables = *tables};
        uint64_t digest = 0;
        for (const bool uniformAir : {false, true}) {
            for (const int32_t level : {0, 1}) {
                const auto ran = RunCompare(context, level, uniformAir);
                if (!ran) {
                    Log(Channel::Gpu, Level::Error, "gpu_object_heat_nest_test: FAILED(レベル {}・{}: {})", level,
                        uniformAir ? "一様の空気" : "頁", ran.error());
                    return 1;
                }

                digest = fx::FxHashCombine(digest, *ran);
            }
        }

        if (measure) {
            if (auto measured = Measure(context); !measured) {
                Log(Channel::Gpu, Level::Error, "gpu_object_heat_nest_test: FAILED(計測: {})", measured.error());
                return 1;
            }
        }

        if (!test::PassesValidation(*device, "gpu_object_heat_nest_test"))
            return 1;

        Log(Channel::Gpu, Level::Info, "gpu_object_heat_nest_test: OK(要約 {:016x})", digest);

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
