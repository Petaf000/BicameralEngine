// gpu_multires.cpp — 多重解像度の入れ子の GPU 版(gpu_multires.h)。
// 細かくする・粗くする・引き戻すは Work Graph の再帰(1 レベル = 1 グループ)、刻むのは Compute(1 スレッド = 1 セル)。
// 結び付けは shaders/sim/multires_bindings.hlsli と同じ順(u0 見出し・u1 セル・u2 端数・u3 数える欄・u4 u5 外のバッファ、b0、
// デバッグのリング、t0〜t3 表)。
#include "sim/gpu_multires.h"

#include "core/log.h"
#include "gpu/resources.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        constexpr uint32_t ROOT_CONSTANT_COUNT = 16;
        constexpr uint32_t EXTERNAL_VIEW_FIRST = 4;  // u4・u5
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
            .uavCount = 6, .rootConstantCount = ROOT_CONSTANT_COUNT, .debugRing = true, .srvCount = 4};
        constexpr uint32_t STEP_THREADS_PER_GROUP = 64;  // multires_step.hlsl の numthreads

        enum Entry : uint8_t { EntryRefine, EntryCoarsen, EntryPullBack, EntryRemoveShadow };

        // multires_graph.hlsl のレコード
        struct RefineRecord {
            uint32_t parentSlot;
            uint32_t childSlot;
            uint32_t levelsLeft;
            uint32_t kind;
        };

        struct ChainRecord {
            uint32_t slot;
            uint32_t levelsLeft;
        };

        template <typename T>
        ComPtr<ID3D12Resource> CreateFilledUpload(ID3D12Device* device, std::span<const T> data) {
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

        bool WriteUpload(ID3D12Resource* upload, std::span<const std::byte> bytes) {
            void* mapped = nullptr;
            const D3D12_RANGE noRead{.Begin = 0, .End = 0};
            if (FAILED(upload->Map(0, &noRead, &mapped)))
                return false;

            std::memcpy(mapped, bytes.data(), bytes.size());
            upload->Unmap(0, nullptr);

            return true;
        }

        void UavBarrier(ID3D12GraphicsCommandList10* list) {
            const D3D12_RESOURCE_BARRIER barrier = gpu::UavBarrier(nullptr);
            list->ResourceBarrier(1, &barrier);
        }

    }  // namespace

    std::expected<GpuMultires, std::string> GpuMultires::Create(ID3D12Device5* device, const BakedReactionTable& table,
                                                                uint32_t blockCapacity, uint32_t fractionCapacity) {
        GpuMultires result;
        result.m_blockCapacity = blockCapacity;
        result.m_fractionCapacity = fractionCapacity;
        result.m_constants.blockCount = blockCapacity;

        if (auto pipelines = result.CreatePipelines(device); !pipelines)
            return std::unexpected(pipelines.error());

        if (auto buffers = result.CreateBuffers(device, table); !buffers)
            return std::unexpected(buffers.error());

        return result;
    }

    std::expected<void, std::string> GpuMultires::CreatePipelines(ID3D12Device5* device) {
        m_rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!m_rootSignature)
            return std::unexpected("多重解像度のルート署名を作れない");

        const auto step = gpu::LoadShader("sim/multires_step.cso");
        if (!step)
            return std::unexpected(step.error());

        m_stepPipeline = gpu::CreateComputePipeline(device, m_rootSignature.Get(), *step);
        if (!m_stepPipeline)
            return std::unexpected("多重解像度の刻みのパイプラインを作れない");

        const auto library = gpu::LoadShader("sim/multires_graph.cso");
        if (!library)
            return std::unexpected(library.error());

        auto graph = gpu::WorkGraph::Create(device, m_rootSignature.Get(), *library, L"Multires");
        if (!graph)
            return std::unexpected(graph.error());

        m_graph = std::make_unique<gpu::WorkGraph>(std::move(*graph));
        m_entries = {m_graph->EntrypointIndex(L"RefineNode"), m_graph->EntrypointIndex(L"CoarsenNode"),
                     m_graph->EntrypointIndex(L"PullBackNode"), m_graph->EntrypointIndex(L"RemoveShadowNode")};
        if (std::ranges::contains(m_entries, UINT32_MAX))
            return std::unexpected("multires_graph.cso に入口のノードが足りない");

        return {};
    }

    std::expected<void, std::string> GpuMultires::CreateBuffers(ID3D12Device5* device,
                                                                const BakedReactionTable& table) {
        const std::array<uint64_t, BUFFER_COUNT> sizes = {
            uint64_t{m_blockCapacity} * sizeof(MrBlock), uint64_t{m_blockCapacity} * MR_BLOCK_CELLS * sizeof(RxCell),
            uint64_t{m_fractionCapacity} * MR_BLOCK_CELLS * sizeof(MrFraction),
            uint64_t{MR_COUNTER_COUNT} * sizeof(uint32_t)};
        for (uint32_t i = 0; i < BUFFER_COUNT; ++i) {
            m_buffers[i] = gpu::CreateBuffer(device, sizes[i], gpu::BufferKind::UnorderedAccess);
            m_uploads[i] = gpu::CreateBuffer(device, sizes[i], gpu::BufferKind::Upload);
            m_readbacks[i] = gpu::CreateBuffer(device, sizes[i], gpu::BufferKind::Readback);
            if (!m_buffers[i] || !m_uploads[i] || !m_readbacks[i])
                return std::unexpected("多重解像度のバッファを作れない");
        }

        m_tables = {
            CreateFilledUpload(device, std::span(table.species)), CreateFilledUpload(device, std::span(table.rules)),
            CreateFilledUpload(device, std::span(table.ruleIndex)), CreateFilledUpload(device, std::span(table.rates))};
        if (!std::ranges::all_of(m_tables, [](const auto& buffer) { return buffer != nullptr; }))
            return std::unexpected("反応の表のバッファを作れない");

        const D3D12_QUERY_HEAP_DESC queryDesc{
            .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = MAX_TIMESTAMPS, .NodeMask = 0};
        if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_timestamps))))
            return std::unexpected("タイムスタンプのヒープを作れない");

        m_timestampReadback = gpu::CreateBuffer(device, uint64_t{MAX_TIMESTAMPS} * sizeof(uint64_t),
                                                gpu::BufferKind::Readback);
        if (!m_timestampReadback)
            return std::unexpected("タイムスタンプの読み戻しを作れない");

        return {};
    }

    bool GpuMultires::RecordUpload(ID3D12GraphicsCommandList10* list, const MultiresNest& nest) {
        if (nest.blocks.size() != m_blockCapacity ||
            nest.fractions.size() != size_t{m_fractionCapacity} * MR_BLOCK_CELLS)
            return false;

        const std::array<std::span<const std::byte>, BUFFER_COUNT> sources = {
            std::as_bytes(std::span(nest.blocks)), std::as_bytes(std::span(nest.cells)),
            std::as_bytes(std::span(nest.fractions)), std::as_bytes(std::span(nest.counters))};
        std::array<D3D12_RESOURCE_BARRIER, BUFFER_COUNT> barriers{};
        for (uint32_t i = 0; i < BUFFER_COUNT; ++i) {
            if (!WriteUpload(m_uploads[i].Get(), sources[i]))
                return false;

            // COMMON から暗黙に COPY_DEST へ昇格する。写した後は UAV へ
            list->CopyResource(m_buffers[i].Get(), m_uploads[i].Get());
            barriers[i] = gpu::Transition(m_buffers[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        list->ResourceBarrier(BUFFER_COUNT, barriers.data());

        return true;
    }

    // --- 結び付けと起動 ---

    void GpuMultires::BindRoot(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing) const {
        for (uint32_t i = 0; i < BUFFER_COUNT; ++i)
            list->SetComputeRootUnorderedAccessView(i, m_buffers[i]->GetGPUVirtualAddress());

        const D3D12_GPU_VIRTUAL_ADDRESS standIn = m_buffers[1]->GetGPUVirtualAddress();
        for (uint32_t i = 0; i < m_externalViews.size(); ++i) {
            const D3D12_GPU_VIRTUAL_ADDRESS view = m_externalViews[i] != 0 ? m_externalViews[i] : standIn;
            list->SetComputeRootUnorderedAccessView(EXTERNAL_VIEW_FIRST + i, view);
        }

        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT, &m_constants, 0);
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing);
        for (uint32_t i = 0; i < TABLE_COUNT; ++i)
            list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(i), m_tables[i]->GetGPUVirtualAddress());
    }

    void GpuMultires::SetGraphProgram(ID3D12GraphicsCommandList10* list) {
        list->SetComputeRootSignature(m_rootSignature.Get());
        m_graph->SetProgram(list, !m_graphInitialized);
        m_graphInitialized = true;
    }

    void GpuMultires::DispatchGraph(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                    uint32_t entry, const void* record, uint32_t recordBytes) {
        SetGraphProgram(list);
        BindRoot(list, debugRing);
        gpu::WorkGraph::DispatchFromCpu(list, m_entries[entry], record, 1, recordBytes);
        UavBarrier(list);
    }

    // --- 操作 ---

    void GpuMultires::RecordRefine(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                   uint32_t parentSlot, uint32_t firstChildSlot, uint32_t levelCount, uint32_t kind,
                                   const MultiresPoint& point) {
        const std::array<int64_t, 3> coordinates = {point.x, point.y, point.z};
        for (size_t axis = 0; axis < coordinates.size(); ++axis) {
            const auto value = static_cast<uint64_t>(coordinates[axis]);
            m_constants.point[axis * 2] = static_cast<uint32_t>(value);
            m_constants.point[(axis * 2) + 1] = static_cast<uint32_t>(value >> 32);
        }

        m_constants.pointLevel = point.level;
        const RefineRecord record{
            .parentSlot = parentSlot, .childSlot = firstChildSlot, .levelsLeft = levelCount, .kind = kind};
        DispatchGraph(list, debugRing, EntryRefine, &record, sizeof(record));
    }

    void GpuMultires::RecordCoarsen(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                    uint32_t deepestSlot, uint32_t levelCount) {
        const ChainRecord record{.slot = deepestSlot, .levelsLeft = levelCount};
        DispatchGraph(list, debugRing, EntryCoarsen, &record, sizeof(record));
    }

    void GpuMultires::RecordRemoveShadow(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                         uint32_t firstSlot, uint32_t levelCount) {
        const ChainRecord record{.slot = firstSlot, .levelsLeft = levelCount};
        DispatchGraph(list, debugRing, EntryRemoveShadow, &record, sizeof(record));
    }

    void GpuMultires::RecordPullBack(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                     uint32_t firstShadowSlot, uint32_t levelCount) {
        const ChainRecord record{.slot = firstShadowSlot, .levelsLeft = levelCount};
        DispatchGraph(list, debugRing, EntryPullBack, &record, sizeof(record));
    }

    void GpuMultires::RecordStep(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                 uint64_t worldSeed, uint64_t tick) {
        m_constants.seedLow = static_cast<uint32_t>(worldSeed);
        m_constants.seedHigh = static_cast<uint32_t>(worldSeed >> 32);
        m_constants.tickLow = static_cast<uint32_t>(tick);
        m_constants.tickHigh = static_cast<uint32_t>(tick >> 32);

        list->SetComputeRootSignature(m_rootSignature.Get());
        list->SetPipelineState(m_stepPipeline.Get());
        BindRoot(list, debugRing);
        list->Dispatch(m_blockCapacity * MR_BLOCK_CELLS / STEP_THREADS_PER_GROUP, 1, 1);
        UavBarrier(list);
    }

    // --- 外のバッファとパイプライン ---

    void GpuMultires::SetExternalViews(D3D12_GPU_VIRTUAL_ADDRESS first, D3D12_GPU_VIRTUAL_ADDRESS second) {
        m_externalViews = {first, second};
    }

    void GpuMultires::RecordExternalDispatch(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                             ID3D12PipelineState* pipeline, uint32_t groupCount,
                                             const std::array<uint32_t, 4>& external) {
        m_constants.external = external;

        list->SetComputeRootSignature(m_rootSignature.Get());
        list->SetPipelineState(pipeline);
        BindRoot(list, debugRing);
        list->Dispatch(groupCount, 1, 1);
        UavBarrier(list);
    }

    // --- 読み戻しと計測 ---

    void GpuMultires::RecordReadback(ID3D12GraphicsCommandList10* list) {
        for (uint32_t i = 0; i < BUFFER_COUNT; ++i)
            gpu::RecordCopyToReadback(list, m_buffers[i].Get(), m_readbacks[i].Get());

        if (m_timestampCount > 0) {
            list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, m_timestampCount,
                                   m_timestampReadback.Get(), 0);
        }
    }

    bool GpuMultires::Read(MultiresNest& nest) const {
        nest.blocks.resize(m_blockCapacity);
        nest.cells.resize(size_t{m_blockCapacity} * MR_BLOCK_CELLS);
        nest.fractions.resize(size_t{m_fractionCapacity} * MR_BLOCK_CELLS);

        return gpu::ReadBuffer(m_readbacks[0].Get(), std::as_writable_bytes(std::span(nest.blocks))) &&
               gpu::ReadBuffer(m_readbacks[1].Get(), std::as_writable_bytes(std::span(nest.cells))) &&
               gpu::ReadBuffer(m_readbacks[2].Get(), std::as_writable_bytes(std::span(nest.fractions))) &&
               gpu::ReadBuffer(m_readbacks[3].Get(), std::as_writable_bytes(std::span(nest.counters)));
    }

    void GpuMultires::RecordTimestamp(ID3D12GraphicsCommandList10* list, uint32_t index) {
        if (index >= MAX_TIMESTAMPS)
            return;

        list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
        m_timestampCount = std::max(m_timestampCount, index + 1);
    }

    std::vector<uint64_t> GpuMultires::ReadTimestamps(uint32_t count) const {
        std::vector<uint64_t> ticks(std::min(count, m_timestampCount));
        if (!gpu::ReadBuffer(m_timestampReadback.Get(), std::as_writable_bytes(std::span(ticks))))
            ticks.clear();

        return ticks;
    }

}  // namespace bicameral::sim
