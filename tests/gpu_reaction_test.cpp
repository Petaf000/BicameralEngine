// gpu_reaction_test.cpp — 1 セルの反応の評価を GPU(shaders/sim/reaction_cells.hlsl)で走らせ、CPU リファレンスとビット一致を確かめる(T-0014)。
// いろいろな成分と温度のセル 4096 個(tests/reaction_test_cells.h。reaction_test と同じ列)を 50 刻みずつ 8 区間進め、
// 区間ごとに全部のセルを読み戻して CPU と比べる。最後の要約は reaction_test の「いろいろなセル」の要約と同じになる。
// 引数は gpu_test_options.h。
#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/com_ptr.h"
#include "gpu/debug_ring.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/resources.h"
#include "gpu_test_options.h"
#include "reaction_test_cells.h"
#include "sim/reaction_table.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::reaction;

namespace {

    constexpr uint32_t CELL_COUNT = 4096;
    constexpr uint32_t THREADS_PER_GROUP = 64;  // reaction_cells.hlsl の numthreads
    constexpr uint32_t TICKS_PER_SEGMENT = 50;
    constexpr uint32_t SEGMENT_COUNT = 8;
    constexpr int MAX_REPORTED_MISMATCHES = 5;

    // u0 セル / b0 定数 7 個 / デバッグのリング / t0〜t4 表と初めのセル
    constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
        .uavCount = 1, .rootConstantCount = 7, .debugRing = true, .srvCount = 5};

    struct ReactionPipeline {
        ComPtr<ID3D12RootSignature> rootSignature;
        ComPtr<ID3D12PipelineState> pipeline;
        std::array<ComPtr<ID3D12Resource>, 5>
            tables;  // 物質・規則・索引・速度・初めのセル(アップロードのヒープのまま読む)
        ComPtr<ID3D12Resource> cells;
        ComPtr<ID3D12Resource> readback;
    };

    template <typename T>
    ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, std::span<const T> data) {
        const auto bytes = std::as_bytes(data);
        ComPtr<ID3D12Resource> buffer = gpu::CreateBuffer(device, bytes.size(), gpu::BufferKind::Upload);
        void* mapped = nullptr;
        const D3D12_RANGE noRead{.Begin = 0, .End = 0};
        if (!buffer || FAILED(buffer->Map(0, &noRead, &mapped)))
            return nullptr;

        std::memcpy(mapped, bytes.data(), bytes.size());
        buffer->Unmap(0, nullptr);

        return buffer;
    }

    std::expected<ReactionPipeline, std::string> CreatePipeline(ID3D12Device5* device,
                                                                const sim::BakedReactionTable& table,
                                                                std::span<const RxCell> initialCells) {
        const auto bytecode = gpu::LoadShader("sim/reaction_cells.cso");
        if (!bytecode)
            return std::unexpected(bytecode.error());

        ReactionPipeline result;
        result.rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!result.rootSignature)
            return std::unexpected("ルート署名を作れない");

        result.pipeline = gpu::CreateComputePipeline(device, result.rootSignature.Get(), *bytecode);
        result.tables = {CreateUploadBuffer(device, std::span(table.species)),
                         CreateUploadBuffer(device, std::span(table.rules)),
                         CreateUploadBuffer(device, std::span(table.ruleIndex)),
                         CreateUploadBuffer(device, std::span(table.rates)), CreateUploadBuffer(device, initialCells)};
        const uint64_t cellBytes = initialCells.size_bytes();
        result.cells = gpu::CreateBuffer(device, cellBytes, gpu::BufferKind::UnorderedAccess);
        result.readback = gpu::CreateBuffer(device, cellBytes, gpu::BufferKind::Readback);
        const bool tablesCreated = std::ranges::all_of(result.tables,
                                                       [](const auto& buffer) { return buffer != nullptr; });
        if (!result.pipeline || !tablesCreated || !result.cells || !result.readback)
            return std::unexpected("パイプラインかバッファを作れない");

        return result;
    }

    // 1 区間(TICKS_PER_SEGMENT 刻み)を GPU で進め、全部のセルを読み戻す
    std::expected<std::vector<RxCell>, std::string> RunSegment(gpu::ImmediateQueue& queue,
                                                               const ReactionPipeline& pipeline,
                                                               gpu::DebugRing& debugRing, uint32_t cellCount,
                                                               uint32_t segment) {
        ID3D12GraphicsCommandList10* list = queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        const uint64_t tickBegin = uint64_t{segment} * TICKS_PER_SEGMENT;
        const std::array<uint32_t, 7> constants = {cellCount,
                                                   TICKS_PER_SEGMENT,
                                                   static_cast<uint32_t>(tickBegin),
                                                   static_cast<uint32_t>(tickBegin >> 32),
                                                   static_cast<uint32_t>(test::REACTION_TEST_SEED),
                                                   static_cast<uint32_t>(test::REACTION_TEST_SEED >> 32),
                                                   segment == 0 ? 1u : 0u};
        debugRing.RecordBegin(list);
        list->SetComputeRootSignature(pipeline.rootSignature.Get());
        list->SetPipelineState(pipeline.pipeline.Get());
        list->SetComputeRootUnorderedAccessView(0, pipeline.cells->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), 7, constants.data(), 0);
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing.GpuAddress());
        for (uint32_t i = 0; i < pipeline.tables.size(); ++i)
            list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(i), pipeline.tables[i]->GetGPUVirtualAddress());

        list->Dispatch(cellCount / THREADS_PER_GROUP, 1, 1);
        gpu::RecordCopyToReadback(list, pipeline.cells.Get(), pipeline.readback.Get());
        debugRing.RecordReadbackAndReset(list);
        if (!queue.ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        const gpu::DebugRingContents debugOutput = debugRing.Drain();
        if (debugOutput.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件", debugOutput.assertCount));

        std::vector<RxCell> cells(cellCount);
        if (!gpu::ReadBuffer(pipeline.readback.Get(), std::as_writable_bytes(std::span(cells))))
            return std::unexpected("読み戻せない");

        return cells;
    }

    // CPU の cells を 1 区間進め、GPU の結果と比べる。食い違ったセルの数を返す
    int CompareSegment(const sim::BakedReactionTable& table, std::vector<RxCell>& cpuCells,
                       std::span<const RxCell> gpuCells, uint32_t segment) {
        int mismatchCount = 0;
        for (uint32_t index = 0; index < cpuCells.size(); ++index) {
            const uint64_t tickBegin = uint64_t{segment} * TICKS_PER_SEGMENT;
            for (uint64_t tick = tickBegin; tick < tickBegin + TICKS_PER_SEGMENT; ++tick)
                cpuCells[index] = sim::EvaluateReactionCell(table, cpuCells[index], test::REACTION_TEST_SEED, tick,
                                                            index);

            const uint64_t cpuHash = sim::HashReactionCell(cpuCells[index]);
            const uint64_t gpuHash = sim::HashReactionCell(gpuCells[index]);
            if (cpuHash == gpuHash)
                continue;

            if (mismatchCount++ < MAX_REPORTED_MISMATCHES) {
                Log(Channel::Reaction, Level::Error,
                    "食い違い: 区間 {} セル {}: cpu {:016x}(エネルギー {})/ gpu {:016x}(エネルギー {})", segment, index,
                    cpuHash, cpuCells[index].energy, gpuHash, gpuCells[index].energy);
            }
        }

        return mismatchCount;
    }

    std::expected<uint64_t, std::string> RunAndCompare(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE queueType,
                                                       const sim::BakedReactionTable& table, uint32_t cellCount) {
        std::vector<RxCell> cpuCells(cellCount);
        for (uint32_t index = 0; index < cellCount; ++index)
            cpuCells[index] = test::MakeVariedReactionCell(table, index);

        auto queue = gpu::ImmediateQueue::Create(device, queueType);
        if (!queue)
            return std::unexpected(queue.error());

        const auto pipeline = CreatePipeline(device, table, cpuCells);
        if (!pipeline)
            return std::unexpected(pipeline.error());

        auto debugRing = gpu::DebugRing::Create(device);
        if (!debugRing)
            return std::unexpected(debugRing.error());

        for (uint32_t segment = 0; segment < SEGMENT_COUNT; ++segment) {
            const auto started = chr::steady_clock::now();
            const auto gpuCells = RunSegment(*queue, *pipeline, *debugRing, cellCount, segment);
            if (!gpuCells)
                return std::unexpected(gpuCells.error());

            const auto elapsed = chr::duration_cast<chr::milliseconds>(chr::steady_clock::now() - started);
            Log(Channel::Gpu, Level::Info, "  区間 {}: GPU {} ms", segment, elapsed.count());

            const int mismatches = CompareSegment(table, cpuCells, *gpuCells, segment);
            if (mismatches != 0)
                return std::unexpected(std::format("区間 {}(刻み {}〜)で {} セルが食い違う", segment,
                                                   segment * TICKS_PER_SEGMENT, mismatches));
        }

        uint64_t digest = 0;
        for (const RxCell& cell : cpuCells)
            digest = fx::FxHashCombine(digest, sim::HashReactionCell(cell));

        return digest;
    }

    int Run(std::span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_reaction_test [--warp] [--queue direct|compute]");
            return 2;
        }

        Log(Channel::Gpu, Level::Info, "gpu_reaction_test: adapter {}, queue {}",
            gpu::AdapterKindName(options->adapter), test::QueueTypeName(options->queueType));
        // WARP では GPU-based validation を切る: 大きなシェーダー(128bit の計算を展開した評価)の計装と JIT が 15 分を超えて終わらなかった。
        // debug layer は残す。ハードウェアでは GBV も有効のまま
        const bool warp = options->adapter == gpu::AdapterKind::Warp;
        gpu::DeviceOptions deviceOptions = gpu::DefaultDeviceOptions();
        deviceOptions.gpuBasedValidation = deviceOptions.gpuBasedValidation && !warp;
        const auto table = sim::BakeReactionTable(sim::MakeCombustionTestTable());
        const auto device = gpu::Device::Create(options->adapter, deviceOptions);
        if (!table || !device) {
            Log(Channel::Gpu, Level::Error, "gpu_reaction_test: FAILED(表かデバイスを作れない)");
            return 1;
        }

        const uint32_t cellCount = CELL_COUNT;
        const auto digest = RunAndCompare(device->Get(), options->queueType, *table, cellCount);
        if (!digest) {
            Log(Channel::Gpu, Level::Error, "gpu_reaction_test: FAILED ({})", digest.error());
            return 1;
        }

        if (!test::PassesValidation(*device, "gpu_reaction_test"))
            return 1;

        Log(Channel::Gpu, Level::Info,
            "gpu_reaction_test: OK(セル {} × {} 刻みで GPU と CPU がビット一致。要約 {:016x})", cellCount,
            TICKS_PER_SEGMENT * SEGMENT_COUNT, *digest);

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(std::span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();

    return exitCode;
}
