// gpu_field_gather_test.cpp — 集めて書く段(T-1252 段 2。ADR-0500 の 3・ADR-0532)の GPU(shaders/sim/multires_field_gather.hlsl)と
// CPU(GatherNestField)が、欄の頁の像(pageOf・空きのスタックの並び・使った高さ・頁の値)と作業の残り・報告の数までビット単位で同じかを確かめる。
// 場面(世界の木は CPU で作り、欄の置き場と作業の像を GPU へ写す。sim/gpu_multires_fields.h):
//   A 空きのスタックのある置き場(頁を持つ枠の頁を返した後)に、頁を持つ枠・持たない枠へ足す(12 値。空きの上から取り、尽きたら高さの上へ)
//   B A に頁の予算(使っている数 + 2)を付ける: 2 枠だけ頁を取り、残りは書かずに作業に残る(計器の誤り)
//   C 根 700 個(1 スレッドが枠 2 つの塊)に乱数で頁と足し込み(3 値)。プレフィックス和が塊をまたぐ
//   D 一様な枠へ足す: GPU の印の段が選ぶ枠(木の段が頁に広げる枠)が CPU が広げた枠と同じ
// 引数は gpu_test_options.h。
#include <algorithm>
#include <array>
#include <cstring>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <vector>

#include "common/multires_field_pages.hlsli"
#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/com_ptr.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/resources.h"
#include "gpu_test_options.h"
#include "multires_activity_scene.h"
#include "reaction_test_table.h"
#include "sim/gpu_multires_fields.h"
#include "sim/multires_fields.h"
#include "sim/multires_nest.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::reaction;
using namespace bicameral::sim;
using namespace bicameral::state_field;

namespace {

    constexpr uint32_t CONSERVED = SF_FLAG_CONSERVED | SF_FLAG_HASHED;
    constexpr uint32_t GATHER_THREADS = 512;  // multires_field_gather.hlsl の numthreads
    constexpr uint64_t SCENE_SEED = 0x6a7bu;

    // u0 置き場・u1 作業 / b0 世界の枠の数 / デバッグのリング / t0 枠ごとの (頁, 種類)
    constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
        .uavCount = 2, .rootConstantCount = 1, .debugRing = true, .srvCount = 1};

    // --- 場面 ---------------------------------------------------------------------------------------

    struct GatherScene {
        MultiresNest nest;
        uint32_t field = MULTIRES_NO_FIELD;
        std::vector<uint32_t> roots;
        NestGatherWork work;
    };

    struct SceneShape {
        uint32_t pagedRoots = 0;
        uint32_t uniformRoots = 0;
        uint32_t components = 0;
    };

    uint64_t Pick(uint32_t index, uint32_t purpose, uint64_t span) {
        return fx::FxHash64(SCENE_SEED, 0, index, purpose) % span;
    }

    int64_t PickValue(uint32_t index, uint32_t purpose) {
        return static_cast<int64_t>(Pick(index, purpose, 2000001)) - 1000000;
    }

    GatherScene MakeScene(const BakedReactionTable& table, SceneShape shape) {
        const uint32_t roots = shape.pagedRoots + shape.uniformRoots;
        GatherScene scene{.nest = MakeMultiresNest(test::MakeMultiresCapacity(table, roots + 8, 0, 16))};
        scene.field = RegisterNestField(
            scene.nest, "carry",
            SfMakeOwnerDesc(SF_PLACE_CELL, SF_TYPE_I64, SF_PHASE_NONE, CONSERVED, shape.components, SF_OWNER_TEST));
        const std::vector<RxCell> varied = test::MakeMultiresRootCells(table);
        const std::vector<RxCell> air(MR_BLOCK_CELLS, test::MakeAirCell(table));
        for (uint32_t r = 0; r < roots; ++r) {
            const int64_t x = int64_t{r % 16} * 8;
            const int64_t y = int64_t{(r / 16) % 16} * 8;
            const int64_t z = int64_t{r / 256} * 8;
            scene.roots.push_back(PlaceRootBlock(scene.nest, x, y, z, r < shape.pagedRoots ? varied : air));
        }

        scene.work = MakeNestGatherWork(scene.nest, scene.field);

        return scene;
    }

    // 根 r の枠に欄の頁を作る(値はセル 3 つに)
    void FillRoot(GatherScene& scene, uint32_t r, uint32_t purpose) {
        const uint32_t components = scene.nest.fields[scene.field].desc.components;
        for (uint32_t k = 0; k < 3; ++k) {
            const auto index = static_cast<uint32_t>(Pick(r, purpose + k, MR_BLOCK_CELLS));
            StoreNestField(scene.nest, scene.field, scene.roots[r], index, (r + k) % components,
                           PickValue(r, purpose + 7 + k));
        }
    }

    // 根 r の枠の欄の頁を返す(全部 0 にして眠る判定の返す口で)
    void EmptyRoot(GatherScene& scene, uint32_t r) {
        const uint32_t slot = scene.roots[r];
        const uint32_t components = scene.nest.fields[scene.field].desc.components;
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            for (uint32_t c = 0; c < components; ++c) {
                if (LoadNestField(scene.nest, scene.field, slot, index, c) != 0)
                    StoreNestField(scene.nest, scene.field, slot, index, c, 0);
            }
        }

        nest_fields::ReleaseZeroPages(scene.nest, slot);
    }

    void AddRoot(GatherScene& scene, uint32_t r, uint32_t purpose, uint32_t count) {
        const uint32_t components = scene.work.components;
        for (uint32_t k = 0; k < count; ++k) {
            const auto index = static_cast<uint32_t>(Pick(r, purpose + (k * 3), MR_BLOCK_CELLS));
            const auto c = static_cast<uint32_t>(Pick(r, purpose + (k * 3) + 1, components));
            AddNestGatherWork(scene.work, scene.roots[r], index, c, PickValue(r, purpose + (k * 3) + 2));
        }
    }

    // A・B: 頁 0〜4 を作り 1・3 を返す(空き = [1, 3])→ 0・1・3・5・6 へ足す(同じセルへ 2 回の足し込みを含む)
    GatherScene MakeStackScene(const BakedReactionTable& table, uint32_t budgetAbove) {
        GatherScene scene = MakeScene(table, {.pagedRoots = 7, .uniformRoots = 1, .components = SF_MAX_COMPONENTS});
        for (uint32_t r = 0; r < 5; ++r)
            FillRoot(scene, r, 10);

        EmptyRoot(scene, 1);
        EmptyRoot(scene, 3);
        for (const uint32_t r : {6u, 0u, 3u, 5u, 1u})
            AddRoot(scene, r, 40, 5);

        AddNestGatherWork(scene.work, scene.roots[5], 17, 2, 11);
        AddNestGatherWork(scene.work, scene.roots[5], 17, 2, -4);
        if (budgetAbove > 0)
            SetNestFieldPageBudget(scene.nest, scene.field, UsedNestFieldPages(scene.nest, scene.field) + budgetAbove);

        return scene;
    }

    // C: 根 700 個。3 割に頁・そのうち 1 割を返す・4 割へ足す
    GatherScene MakeWideScene(const BakedReactionTable& table) {
        constexpr uint32_t ROOTS = 700;
        GatherScene scene = MakeScene(table, {.pagedRoots = ROOTS, .uniformRoots = 0, .components = 3});
        for (uint32_t r = 0; r < ROOTS; ++r) {
            if (Pick(r, 1, 10) < 3)
                FillRoot(scene, r, 20);
        }

        for (uint32_t r = ROOTS; r-- > 0;) {
            if (Pick(r, 1, 10) == 0)
                EmptyRoot(scene, r);
        }

        for (uint32_t r = 0; r < ROOTS; ++r) {
            if (Pick(r, 2, 10) < 4)
                AddRoot(scene, r, 60, 1 + static_cast<uint32_t>(Pick(r, 3, 4)));
        }

        return scene;
    }

    // D: 頁を持つ根 2 つ・一様な根 3 つ(2 つに足す)
    GatherScene MakeUniformScene(const BakedReactionTable& table) {
        GatherScene scene = MakeScene(table, {.pagedRoots = 2, .uniformRoots = 3, .components = 2});
        for (const uint32_t r : {0u, 2u, 4u})
            AddRoot(scene, r, 80, 3);

        return scene;
    }

    std::vector<uint32_t> BlockWords(const MultiresNest& nest) {
        std::vector<uint32_t> words(size_t{nest.capacity.worldBlocks} * 2);
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            words[size_t{slot} * 2] = nest.blocks[slot].page;
            words[(size_t{slot} * 2) + 1] = nest.blocks[slot].kind;
        }

        return words;
    }

    // --- GPU ----------------------------------------------------------------------------------------

    struct GatherPipelines {
        ComPtr<ID3D12RootSignature> rootSignature;
        std::array<ComPtr<ID3D12PipelineState>, 3> stages;  // 印・頁を取る・書く
    };

    std::expected<GatherPipelines, std::string> CreatePipelines(ID3D12Device* device) {
        GatherPipelines result;
        result.rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!result.rootSignature)
            return std::unexpected("ルート署名を作れない");

        const std::array<const char*, 3> names = {"sim/multires_field_gather_mark.cso",
                                                  "sim/multires_field_gather_assign.cso",
                                                  "sim/multires_field_gather_write.cso"};
        for (uint32_t i = 0; i < names.size(); ++i) {
            const auto bytecode = gpu::LoadShader(names[i]);
            if (!bytecode)
                return std::unexpected(bytecode.error());

            result.stages[i] = gpu::CreateComputePipeline(device, result.rootSignature.Get(), *bytecode);
            if (!result.stages[i])
                return std::unexpected(std::format("パイプライン {} を作れない", names[i]));
        }

        return result;
    }

    struct GatherImages {
        std::vector<uint32_t> store;
        std::vector<uint32_t> work;
    };

    struct GpuBuffer {
        ComPtr<ID3D12Resource> upload;
        ComPtr<ID3D12Resource> buffer;
        ComPtr<ID3D12Resource> readback;
    };

    GpuBuffer CreateGpuBuffer(ID3D12Device* device, std::span<const uint32_t> words) {
        const auto bytes = std::as_bytes(words);
        return {.upload = gpu::CreateFilledUpload(device, bytes),
                .buffer = gpu::CreateBuffer(device, bytes.size(), gpu::BufferKind::UnorderedAccess),
                .readback = gpu::CreateBuffer(device, bytes.size(), gpu::BufferKind::Readback)};
    }

    std::expected<GatherImages, std::string> RunGpu(ID3D12Device* device, gpu::ImmediateQueue& queue,
                                                    const GatherPipelines& pipelines, gpu::DebugRing& debugRing,
                                                    const GatherImages& input, std::span<const uint32_t> blockWords,
                                                    uint32_t worldBlocks) {
        const GpuBuffer store = CreateGpuBuffer(device, input.store);
        const GpuBuffer work = CreateGpuBuffer(device, input.work);
        const ComPtr<ID3D12Resource> blocks = gpu::CreateFilledUpload(device, std::as_bytes(blockWords));
        if (!store.upload || !store.buffer || !store.readback || !work.upload || !work.buffer || !work.readback ||
            !blocks)
            return std::unexpected("バッファを作れない");

        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        // --- 写す(COMMON から COPY_DEST へは暗黙に上がる)→ UAV ---
        list->CopyBufferRegion(store.buffer.Get(), 0, store.upload.Get(), 0, std::span(input.store).size_bytes());
        list->CopyBufferRegion(work.buffer.Get(), 0, work.upload.Get(), 0, std::span(input.work).size_bytes());
        const std::array<D3D12_RESOURCE_BARRIER, 2> toUav = {
            gpu::Transition(store.buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            gpu::Transition(work.buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        list->ResourceBarrier(static_cast<UINT>(toUav.size()), toUav.data());

        // --- 3 つの段 ---
        debugRing.RecordBegin(list);
        list->SetComputeRootSignature(pipelines.rootSignature.Get());
        list->SetComputeRootUnorderedAccessView(0, store.buffer->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(1, work.buffer->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), 1, &worldBlocks, 0);
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing.GpuAddress());
        list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(0), blocks->GetGPUVirtualAddress());
        for (uint32_t stage = 0; stage < pipelines.stages.size(); ++stage) {
            list->SetPipelineState(pipelines.stages[stage].Get());
            list->Dispatch(stage == 1 ? 1 : worldBlocks, 1, 1);
            const std::array<D3D12_RESOURCE_BARRIER, 2> uav = {gpu::UavBarrier(store.buffer.Get()),
                                                               gpu::UavBarrier(work.buffer.Get())};
            list->ResourceBarrier(static_cast<UINT>(uav.size()), uav.data());
        }

        gpu::RecordCopyToReadback(list, store.buffer.Get(), store.readback.Get());
        gpu::RecordCopyToReadback(list, work.buffer.Get(), work.readback.Get());
        debugRing.RecordReadbackAndReset(list);
        if (!queue.ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        const gpu::DebugRingContents debugOutput = debugRing.Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debugOutput.assertCount));

        GatherImages result{.store = std::vector<uint32_t>(input.store.size()),
                            .work = std::vector<uint32_t>(input.work.size())};
        if (!gpu::ReadBuffer(store.readback.Get(), std::as_writable_bytes(std::span(result.store))) ||
            !gpu::ReadBuffer(work.readback.Get(), std::as_writable_bytes(std::span(result.work))))
            return std::unexpected("読み戻せない");

        return result;
    }

    // --- 比べる -------------------------------------------------------------------------------------

    struct SceneRun {
        GatherScene cpu;
        GatherScene gpu;
        NestGatherReport cpuReport;
        NestGatherReport gpuReport;
        std::vector<uint32_t> results;  // GPU の枠ごとの段の結果
    };

    struct GpuContext {
        ID3D12Device* device = nullptr;
        gpu::ImmediateQueue* queue = nullptr;
        const GatherPipelines* pipelines = nullptr;
        gpu::DebugRing* debugRing = nullptr;
    };

    // 同じ場面を 2 つ作り、片方を CPU・片方を GPU で集める(容量は使った高さ + 足す枠の数 + 余り)
    template <typename Make>
    std::expected<SceneRun, std::string> RunScene(const GpuContext& context, Make make) {
        SceneRun run{.cpu = make(), .gpu = make()};
        const MultiresFieldStore& before = run.gpu.nest.fields[run.gpu.field].store;
        const auto top = static_cast<uint32_t>(before.pages.size() / (size_t{MR_BLOCK_CELLS} * before.components));
        const uint32_t capacity = top + static_cast<uint32_t>(run.gpu.work.slots.size()) + 4;
        const auto store = MakeGpuFieldImage(run.gpu.nest, run.gpu.field, capacity);
        if (!store)
            return std::unexpected(store.error());

        const GatherImages input{.store = *store, .work = MakeGpuGatherWorkImage(run.gpu.work)};
        const auto output = RunGpu(context.device, *context.queue, *context.pipelines, *context.debugRing, input,
                                   BlockWords(run.gpu.nest), run.gpu.nest.capacity.worldBlocks);
        if (!output)
            return std::unexpected(output.error());

        ApplyGpuFieldImage(run.gpu.nest, run.gpu.field, output->store);
        ApplyGpuGatherWorkImage(run.gpu.work, output->work, run.results);
        run.gpuReport = GpuFieldGatherReport(output->store);
        run.cpuReport = GatherNestField(run.cpu.nest, run.cpu.work);

        return run;
    }

    std::string CompareStores(const SceneRun& run) {
        const MultiresFieldStore& a = run.cpu.nest.fields[run.cpu.field].store;
        const MultiresFieldStore& b = run.gpu.nest.fields[run.gpu.field].store;
        if (a.pageOf != b.pageOf)
            return "pageOf(セルの頁 → 欄の頁)が違う";

        if (a.freePages != b.freePages)
            return std::format("空きのスタックが違う(CPU {} 個・GPU {} 個)", a.freePages.size(), b.freePages.size());

        if (a.pages.size() != b.pages.size())
            return std::format("使った高さが違う(CPU {}・GPU {} 値)", a.pages.size(), b.pages.size());

        if (a.pages != b.pages)
            return "欄の頁の値が違う";

        if (run.cpu.work.values != run.gpu.work.values)
            return "作業の残りが違う";

        const NestGatherReport& c = run.cpuReport;
        const NestGatherReport& g = run.gpuReport;
        if (c.blocks != g.blocks || c.pagesTaken != g.pagesTaken || c.pageShortage != g.pageShortage)
            return std::format("報告が違う(書いた枠 {}/{}・取った頁 {}/{}・尽きた {}/{})", c.blocks, g.blocks,
                               c.pagesTaken, g.pagesTaken, c.pageShortage, g.pageShortage);

        return {};
    }

    std::expected<void, std::string> CheckStackScene(const GpuContext& context, const BakedReactionTable& table,
                                                     uint32_t budgetAbove, std::string_view name) {
        const auto run = RunScene(context, [&] { return MakeStackScene(table, budgetAbove); });
        if (!run)
            return std::unexpected(run.error());

        if (const std::string error = CompareStores(*run); !error.empty())
            return std::unexpected(std::format("{}: {}", name, error));

        // 場面が道を通ったか(空きから取る・高さの上へ・予算で尽きる)
        const NestGatherReport& report = run->cpuReport;
        const bool expected = budgetAbove == 0
                                  ? report.pagesTaken == 4 && report.pageShortage == 0
                                  : report.pagesTaken == budgetAbove && report.pageShortage == 4 - budgetAbove;
        if (!expected || report.blocks != 1 + report.pagesTaken)
            return std::unexpected(std::format("{}: 場面が思った道を通っていない(書いた枠 {}・取った頁 {}・尽きた {})",
                                               name, report.blocks, report.pagesTaken, report.pageShortage));

        Log(Channel::Gpu, Level::Info, "  {}: 書いた枠 {}・取った頁 {}・尽きた {}・空きの残り {}", name, report.blocks,
            report.pagesTaken, report.pageShortage, run->cpu.nest.fields[run->cpu.field].store.freePages.size());

        return {};
    }

    std::expected<void, std::string> CheckWideScene(const GpuContext& context, const BakedReactionTable& table) {
        const auto run = RunScene(context, [&] { return MakeWideScene(table); });
        if (!run)
            return std::unexpected(run.error());

        if (const std::string error = CompareStores(*run); !error.empty())
            return std::unexpected(std::format("C: {}", error));

        const NestGatherReport& report = run->cpuReport;
        Log(Channel::Gpu, Level::Info, "  C: 書いた枠 {}・取った頁 {}(枠 {} 個・1 スレッドの塊 {})", report.blocks,
            report.pagesTaken, run->cpu.nest.capacity.worldBlocks,
            (run->cpu.nest.capacity.worldBlocks + GATHER_THREADS - 1) / GATHER_THREADS);
        if (report.pagesTaken == 0 || report.blocks == report.pagesTaken)
            return std::unexpected("C: 頁を取る枠と持つ枠の両方を通っていない");

        return {};
    }

    std::expected<void, std::string> CheckUniformScene(const GpuContext& context, const BakedReactionTable& table) {
        const GatherScene before = MakeUniformScene(table);
        const auto run = RunScene(context, [&] { return MakeUniformScene(table); });
        if (!run)
            return std::unexpected(run.error());

        // CPU が頁に広げた枠 = GPU の印の段が WANTED にした枠
        uint32_t expanded = 0;
        for (uint32_t slot = 0; slot < before.nest.capacity.worldBlocks; ++slot) {
            const bool cpuExpanded = MrIsUniform(before.nest.blocks[slot]) && !MrIsUniform(run->cpu.nest.blocks[slot]);
            const bool gpuWanted = run->results[slot] == MF_GATHER_WANTED;
            if (cpuExpanded != gpuWanted)
                return std::unexpected(
                    std::format("D: 枠 {} の広げる印が違う(CPU {}・GPU {})", slot, cpuExpanded, gpuWanted));

            expanded += cpuExpanded ? 1u : 0u;
        }

        if (expanded != 2 || run->cpuReport.expanded != 2 || run->gpuReport.expanded != 2)
            return std::unexpected(
                std::format("D: 広げた枠の数が違う(CPU {}・GPU {})", run->cpuReport.expanded, run->gpuReport.expanded));

        Log(Channel::Gpu, Level::Info, "  D: 触った一様な枠 {} つを木の段へ渡す印が CPU の広げた枠と同じ", expanded);

        return {};
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_field_gather_test [--warp] [--queue direct|compute]");
            return 2;
        }

        Log(Channel::Gpu, Level::Info, "gpu_field_gather_test: adapter {}, queue {}",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType));
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table) {
            Log(Channel::Gpu, Level::Error, "gpu_field_gather_test: FAILED(試験の表をベイクできない: {})",
                table.error());
            return 1;
        }

        const auto device = gpu::Device::Create(options->adapter, test::TestDeviceOptions(*options));
        if (!device) {
            Log(Channel::Gpu, Level::Error, "gpu_field_gather_test: FAILED(デバイスを作れない)");
            return 1;
        }

        auto queue = gpu::ImmediateQueue::Create(device->Get(), options->queueType);
        const auto pipelines = CreatePipelines(device->Get());
        auto debugRing = gpu::DebugRing::Create(device->Get());
        if (!queue || !pipelines || !debugRing) {
            Log(Channel::Gpu, Level::Error, "gpu_field_gather_test: FAILED(キュー・パイプライン・リングを作れない)");
            return 1;
        }

        const GpuContext context{
            .device = device->Get(), .queue = &*queue, .pipelines = &*pipelines, .debugRing = &*debugRing};
        const std::array<std::expected<void, std::string>, 4> checks = {
            CheckStackScene(context, *table, 0, "A"), CheckStackScene(context, *table, 2, "B"),
            CheckWideScene(context, *table), CheckUniformScene(context, *table)};
        bool failed = false;
        for (const auto& check : checks) {
            if (!check) {
                Log(Channel::Gpu, Level::Error, "gpu_field_gather_test: FAILED ({})", check.error());
                failed = true;
            }
        }

        if (failed || !test::PassesValidation(*device, "gpu_field_gather_test"))
            return 1;

        Log(Channel::Gpu, Level::Info, "gpu_field_gather_test: OK(集めて書く段の欄の頁の像が GPU と CPU でビット一致)");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
