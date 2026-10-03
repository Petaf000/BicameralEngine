// gpu_multires.cpp — 多重解像度の木の GPU 版(gpu_multires.h)。
// 要求の処理は Compute の段(multires_tree.hlsl)と Work Graph(細かくする鎖の再帰・粗くする要求)、影を作る・引き戻すは Work Graph の再帰
// (1 レベル = 1 グループ)、刻むのは Compute(1 スレッド = 1 セル)。
// 結び付けは shaders/sim/multires_bindings.hlsli と同じ順(u0 見出し・u1 セル・u2 端数・u3 数える欄・u4 u5 外のバッファ・
// u6〜u13 木の管理、b0、デバッグのリング、t0〜t3 表)。
#include "sim/gpu_multires.h"

#include "core/log.h"
#include "gpu/resources.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        constexpr uint32_t ROOT_CONSTANT_COUNT = 23;
        constexpr uint32_t EXTERNAL_VIEW_FIRST = 4;  // u4・u5
        constexpr uint32_t UAV_COUNT = 14;           // u0〜u13
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
            .uavCount = UAV_COUNT, .rootConstantCount = ROOT_CONSTANT_COUNT, .debugRing = true, .srvCount = 4};
        constexpr uint32_t STEP_THREADS_PER_GROUP = 64;  // multires_step.hlsl の numthreads
        constexpr uint32_t TREE_THREADS = 64;            // multires_tree.hlsl の TREE_THREADS

        enum Entry : uint8_t { EntryRefine, EntryCoarsenRequest, EntryPullBack, EntryRemoveShadow };

        // multires_tree.hlsl の段(呼ぶ順)
        enum TreePass : uint8_t { PassResolve, PassSettle, PassAllocate, PassRelease, PassClearIndex, PassFillIndex };
        constexpr std::array<const char*, 6> TREE_SHADERS = {
            "sim/multires_tree_resolve.cso", "sim/multires_tree_settle.cso",      "sim/multires_tree_allocate.cso",
            "sim/multires_tree_release.cso", "sim/multires_tree_clear_index.cso", "sim/multires_tree_fill_index.cso"};

        // Work Graph の GPU の入力(multires_bindings.hlsli の MR_GRAPH_INPUT_*)。見出しは D3D12_NODE_GPU_INPUT そのもの
        constexpr uint32_t GRAPH_INPUT_REFINE_HEADER = 0;
        constexpr uint32_t GRAPH_INPUT_COARSEN_HEADER = 32;
        constexpr uint32_t REFINE_RECORD_BYTES = 24;
        constexpr uint32_t GRAPH_INPUT_BYTES = 64 + ((REFINE_RECORD_BYTES + 4) * (MR_MAX_REQUESTS + 1));
        static_assert(offsetof(D3D12_NODE_GPU_INPUT, EntrypointIndex) == 0);
        static_assert(offsetof(D3D12_NODE_GPU_INPUT, NumRecords) == 4);
        static_assert(offsetof(D3D12_NODE_GPU_INPUT, Records) == 8);
        static_assert(sizeof(D3D12_NODE_GPU_INPUT) <= GRAPH_INPUT_COARSEN_HEADER);

        // multires_graph.hlsl・multires_bindings.hlsli のレコード
        struct RefineRecord {
            uint32_t request;
            uint32_t parentSlot;
            uint32_t childSlot;
            uint32_t levelsLeft;
            uint32_t kind;
            uint32_t depth;
        };
        static_assert(sizeof(RefineRecord) == REFINE_RECORD_BYTES);
        static_assert(sizeof(MrRequest) == 40);

        // 1 回の要求の写しの大きさ(要求の一覧 + 数)
        constexpr uint64_t REQUEST_UPLOAD_BYTES = (uint64_t{MR_MAX_REQUESTS} * sizeof(MrRequest)) + 256;

        uint32_t GroupsFor(uint32_t threads, uint32_t perGroup) {
            return std::max(1u, (threads + perGroup - 1) / perGroup);
        }

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
                                                                const MultiresCapacity& capacity) {
        GpuMultires result;
        result.m_capacity = capacity;
        result.m_blockCapacity = capacity.worldBlocks + capacity.observerBlocks;
        result.m_constants.blockCount = result.m_blockCapacity;
        result.m_constants.rootLevel = capacity.rootLevel;
        result.m_constants.worldBlocks = capacity.worldBlocks;
        result.m_constants.indexEntries = capacity.indexEntries;
        result.m_constants.ledgerColumns = capacity.ledgerColumns;

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

        for (uint32_t pass = 0; pass < TREE_PASS_COUNT; ++pass) {
            const auto bytecode = gpu::LoadShader(TREE_SHADERS[pass]);
            if (!bytecode)
                return std::unexpected(bytecode.error());

            m_treePipelines[pass] = gpu::CreateComputePipeline(device, m_rootSignature.Get(), *bytecode);
            if (!m_treePipelines[pass])
                return std::unexpected(std::format("木の管理のパイプラインを作れない({})", TREE_SHADERS[pass]));
        }

        const auto library = gpu::LoadShader("sim/multires_graph.cso");
        if (!library)
            return std::unexpected(library.error());

        auto graph = gpu::WorkGraph::Create(device, m_rootSignature.Get(), *library, L"Multires");
        if (!graph)
            return std::unexpected(graph.error());

        m_graph = std::make_unique<gpu::WorkGraph>(std::move(*graph));
        m_entries = {m_graph->EntrypointIndex(L"RefineNode"), m_graph->EntrypointIndex(L"CoarsenRequestNode"),
                     m_graph->EntrypointIndex(L"PullBackNode"), m_graph->EntrypointIndex(L"RemoveShadowNode")};
        if (std::ranges::contains(m_entries, UINT32_MAX))
            return std::unexpected("multires_graph.cso に入口のノードが足りない");

        m_constants.graphEntries = m_entries[EntryRefine] | (m_entries[EntryCoarsenRequest] << 16);

        return {};
    }

    std::expected<void, std::string> GpuMultires::CreateBuffers(ID3D12Device5* device,
                                                                const BakedReactionTable& table) {
        // 0 の大きさのバッファは作れないので、最低 16 バイト
        const std::array<uint64_t, BufferCount> sizes = BufferSizes();
        for (uint32_t i = 0; i < BufferCount; ++i) {
            const uint64_t bytes = std::max<uint64_t>(sizes[i], 16);
            m_buffers[i] = gpu::CreateBuffer(device, bytes, gpu::BufferKind::UnorderedAccess);
            m_uploads[i] = gpu::CreateBuffer(device, bytes, gpu::BufferKind::Upload);
            m_readbacks[i] = gpu::CreateBuffer(device, bytes, gpu::BufferKind::Readback);
            if (!m_buffers[i] || !m_uploads[i] || !m_readbacks[i])
                return std::unexpected("多重解像度のバッファを作れない");
        }

        m_requestUpload = gpu::CreateBuffer(device, REQUEST_UPLOAD_BYTES * REQUEST_UPLOAD_SLOTS,
                                            gpu::BufferKind::Upload);
        if (!m_requestUpload)
            return std::unexpected("要求の写しのバッファを作れない");

        const D3D12_GPU_VIRTUAL_ADDRESS graphInput = m_buffers[BufferGraphInput]->GetGPUVirtualAddress();
        m_constants.graphInputLow = static_cast<uint32_t>(graphInput);
        m_constants.graphInputHigh = static_cast<uint32_t>(graphInput >> 32);

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

    std::array<uint64_t, GpuMultires::BufferCount> GpuMultires::BufferSizes() const {
        std::array<uint64_t, BufferCount> sizes{};
        sizes[BufferBlocks] = uint64_t{m_blockCapacity} * sizeof(MrBlock);
        sizes[BufferCells] = uint64_t{m_blockCapacity} * MR_BLOCK_CELLS * sizeof(RxCell);
        sizes[BufferFractions] = uint64_t{m_capacity.fractions} * MR_BLOCK_CELLS * sizeof(MrFraction);
        sizes[BufferCounters] = uint64_t{MR_COUNTER_COUNT} * sizeof(uint32_t);
        sizes[BufferFreeBlocks] = uint64_t{m_capacity.worldBlocks} * sizeof(uint32_t);
        sizes[BufferFreeFractions] = uint64_t{m_capacity.fractions} * sizeof(uint32_t);
        sizes[BufferLedger] = uint64_t{MR_LEDGER_LEVELS} * m_capacity.ledgerColumns * sizeof(uint64_t);
        sizes[BufferIndex] = uint64_t{m_capacity.indexEntries} * sizeof(uint32_t);
        sizes[BufferRequests] = uint64_t{MR_MAX_REQUESTS} * sizeof(MrRequest);
        sizes[BufferStates] = uint64_t{MR_MAX_REQUESTS} * sizeof(MrRequestState);
        sizes[BufferClaims] = uint64_t{m_capacity.worldBlocks} * sizeof(uint32_t);
        sizes[BufferGraphInput] = GRAPH_INPUT_BYTES;

        return sizes;
    }

    bool GpuMultires::RecordUpload(ID3D12GraphicsCommandList10* list, const MultiresNest& nest) {
        if (nest.blocks.size() != m_blockCapacity ||
            nest.fractions.size() != size_t{m_capacity.fractions} * MR_BLOCK_CELLS ||
            nest.index.size() != m_capacity.indexEntries || nest.ledger.size() != BufferSizes()[BufferLedger] / 8)
            return false;

        // 要求の途中の値と GPU の入力は写さない(0 のまま。毎回の処理が書いてから読む)
        const std::vector<std::byte> zeros(std::max(BufferSizes()[BufferStates], uint64_t{GRAPH_INPUT_BYTES}));
        const std::array<std::span<const std::byte>, BufferCount> sources = {
            std::as_bytes(std::span(nest.blocks)),     std::as_bytes(std::span(nest.cells)),
            std::as_bytes(std::span(nest.fractions)),  std::as_bytes(std::span(nest.counters)),
            std::as_bytes(std::span(nest.freeBlocks)), std::as_bytes(std::span(nest.freeFractions)),
            std::as_bytes(std::span(nest.ledger)),     std::as_bytes(std::span(nest.index)),
            std::as_bytes(std::span(nest.requests)),   std::span(zeros).first(BufferSizes()[BufferStates]),
            std::as_bytes(std::span(nest.claims)),     std::span(zeros).first(GRAPH_INPUT_BYTES)};
        std::array<D3D12_RESOURCE_BARRIER, BufferCount> barriers{};
        for (uint32_t i = 0; i < BufferCount; ++i) {
            if (!WriteUpload(m_uploads[i].Get(), sources[i]))
                return false;

            // COMMON から暗黙に COPY_DEST へ昇格する。写した後は UAV へ
            list->CopyResource(m_buffers[i].Get(), m_uploads[i].Get());
            barriers[i] = gpu::Transition(m_buffers[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        list->ResourceBarrier(BufferCount, barriers.data());

        return true;
    }

    // --- 結び付けと起動 ---

    void GpuMultires::BindRoot(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing) const {
        static_assert(sizeof(RootConstants) == ROOT_CONSTANT_COUNT * sizeof(uint32_t));
        static_assert(BufferCount + 2 == UAV_COUNT);  // u4・u5 は外のバッファ
        for (uint32_t i = 0; i < BufferCount; ++i) {
            const uint32_t parameter = i < EXTERNAL_VIEW_FIRST ? i : i + 2;
            list->SetComputeRootUnorderedAccessView(parameter, m_buffers[i]->GetGPUVirtualAddress());
        }

        const D3D12_GPU_VIRTUAL_ADDRESS standIn = m_buffers[BufferCells]->GetGPUVirtualAddress();
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

    void GpuMultires::RecordTreePass(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                     uint32_t pass, uint32_t groupCount) {
        list->SetComputeRootSignature(m_rootSignature.Get());
        list->SetPipelineState(m_treePipelines[pass].Get());
        BindRoot(list, debugRing);
        list->Dispatch(groupCount, 1, 1);
        UavBarrier(list);
    }

    // --- 世界の木 ---

    bool GpuMultires::RecordRequests(ID3D12GraphicsCommandList10* list, std::span<const MrRequest> requests) {
        if (requests.size() > MR_MAX_REQUESTS)
            return false;

        // --- 写しの枠に [数][要求の一覧] を書く ---
        const uint64_t offset = REQUEST_UPLOAD_BYTES * (m_requestUploadCursor++ % REQUEST_UPLOAD_SLOTS);
        const auto count = static_cast<uint32_t>(requests.size());
        void* mapped = nullptr;
        const D3D12_RANGE noRead{.Begin = 0, .End = 0};
        if (FAILED(m_requestUpload->Map(0, &noRead, &mapped)))
            return false;

        auto* bytes = static_cast<std::byte*>(mapped) + offset;
        std::memcpy(bytes, &count, sizeof(count));
        std::memcpy(bytes + 256, requests.data(), requests.size_bytes());
        m_requestUpload->Unmap(0, nullptr);

        // --- 一覧と数える欄の「要求の数」へ写す ---
        ID3D12Resource* requestBuffer = m_buffers[BufferRequests].Get();
        ID3D12Resource* counters = m_buffers[BufferCounters].Get();
        const std::array<D3D12_RESOURCE_BARRIER, 2> toCopy = {
            gpu::Transition(requestBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
            gpu::Transition(counters, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST)};
        list->ResourceBarrier(static_cast<UINT>(toCopy.size()), toCopy.data());
        if (count > 0)
            list->CopyBufferRegion(requestBuffer, 0, m_requestUpload.Get(), offset + 256, requests.size_bytes());

        list->CopyBufferRegion(counters, uint64_t{MR_COUNTER_REQUESTS} * sizeof(uint32_t), m_requestUpload.Get(),
                               offset, sizeof(uint32_t));
        const std::array<D3D12_RESOURCE_BARRIER, 2> toUav = {
            gpu::Transition(requestBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            gpu::Transition(counters, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        list->ResourceBarrier(static_cast<UINT>(toUav.size()), toUav.data());

        return true;
    }

    void GpuMultires::RecordProcessRequests(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing) {
        const uint32_t requestGroups = GroupsFor(MR_MAX_REQUESTS, TREE_THREADS);
        RecordTreePass(list, debugRing, PassResolve, requestGroups);
        RecordTreePass(list, debugRing, PassSettle, requestGroups);
        RecordTreePass(list, debugRing, PassAllocate, 1);

        // --- 適用(Work Graph。GPU の入力は SRV の状態で読む)---
        ID3D12Resource* graphInput = m_buffers[BufferGraphInput].Get();
        const D3D12_RESOURCE_BARRIER toInput = gpu::Transition(graphInput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &toInput);
        SetGraphProgram(list);
        BindRoot(list, debugRing);
        gpu::WorkGraph::DispatchFromGpu(list, graphInput->GetGPUVirtualAddress() + GRAPH_INPUT_REFINE_HEADER);
        UavBarrier(list);
        gpu::WorkGraph::DispatchFromGpu(list, graphInput->GetGPUVirtualAddress() + GRAPH_INPUT_COARSEN_HEADER);
        UavBarrier(list);
        const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(graphInput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);

        // --- 解放と索引の作り直し(印がある時だけ働く)---
        RecordTreePass(list, debugRing, PassRelease, 1);
        RecordTreePass(list, debugRing, PassClearIndex, GroupsFor(m_capacity.indexEntries, TREE_THREADS));
        RecordTreePass(list, debugRing, PassFillIndex, GroupsFor(m_capacity.worldBlocks, TREE_THREADS));
    }

    // --- 観察の枠 ---

    void GpuMultires::RecordRefineShadow(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                         uint32_t parentSlot, uint32_t firstChildSlot, uint32_t levelCount,
                                         const MultiresPoint& point) {
        const std::array<int64_t, 3> coordinates = {point.x, point.y, point.z};
        for (size_t axis = 0; axis < coordinates.size(); ++axis) {
            const auto value = static_cast<uint64_t>(coordinates[axis]);
            m_constants.point[axis * 2] = static_cast<uint32_t>(value);
            m_constants.point[(axis * 2) + 1] = static_cast<uint32_t>(value >> 32);
        }

        m_constants.pointLevel = point.level;
        const RefineRecord record{.request = MR_NO_BLOCK,
                                  .parentSlot = parentSlot,
                                  .childSlot = firstChildSlot,
                                  .levelsLeft = levelCount,
                                  .kind = MR_BLOCK_SHADOW,
                                  .depth = 0};
        DispatchGraph(list, debugRing, EntryRefine, &record, sizeof(record));
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
        // 状態と索引(要求・途中の値・取り合いの印・GPU の入力は読まない)
        for (uint32_t i = 0; i <= BufferIndex; ++i)
            gpu::RecordCopyToReadback(list, m_buffers[i].Get(), m_readbacks[i].Get());

        if (m_timestampCount > 0) {
            list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, m_timestampCount,
                                   m_timestampReadback.Get(), 0);
        }
    }

    bool GpuMultires::Read(MultiresNest& nest) const {
        if (nest.blocks.size() != m_blockCapacity || nest.index.size() != m_capacity.indexEntries)
            nest = MakeMultiresNest(m_capacity);

        const std::array<std::span<std::byte>, BufferIndex + 1> destinations = {
            std::as_writable_bytes(std::span(nest.blocks)),     std::as_writable_bytes(std::span(nest.cells)),
            std::as_writable_bytes(std::span(nest.fractions)),  std::as_writable_bytes(std::span(nest.counters)),
            std::as_writable_bytes(std::span(nest.freeBlocks)), std::as_writable_bytes(std::span(nest.freeFractions)),
            std::as_writable_bytes(std::span(nest.ledger)),     std::as_writable_bytes(std::span(nest.index))};
        for (uint32_t i = 0; i < destinations.size(); ++i) {
            if (!gpu::ReadBuffer(m_readbacks[i].Get(), destinations[i]))
                return false;
        }

        return true;
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
