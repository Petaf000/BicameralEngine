// gpu_implicit_levels.cpp — 細かいレベルの熱の陰解法の多重格子の段と重みを GPU で作る(T-0134)。何をするかは gpu_implicit_levels.h、
// 段の中身と番号の付け方は shaders/sim/implicit_levels.hlsl。段の間は UAV のバリア、回は述語で飛ばす(GpuImplicit と同じ形)。
#include "sim/gpu_implicit_levels.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <span>
#include <string_view>
#include <utility>

#include "gpu/resources.h"

using namespace bicameral::multires;

namespace bicameral::sim {

    namespace {

        constexpr uint32_t THREADS = 64;         // implicit_levels.hlsl の LV_THREADS
        constexpr uint32_t SCAN_THREADS = 1024;  // implicit_levels.hlsl の LV_SCAN_THREADS
        constexpr uint32_t UAV_COUNT = 7;
        constexpr uint32_t ROOT_CONSTANT_COUNT = 13;
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
            .uavCount = UAV_COUNT, .rootConstantCount = ROOT_CONSTANT_COUNT, .debugRing = true};

        // implicit_levels.hlsl の作業場の並び(語)
        constexpr uint32_t DEPTH_BASE = 16;
        constexpr uint32_t DEPTH_STRIDE = 72;
        constexpr uint32_t HEADER_WORDS = DEPTH_BASE + (8 * DEPTH_STRIDE);
        constexpr uint32_t LEVEL_COUNT = 0;
        constexpr uint32_t OVERFLOW_WORD = 1;
        constexpr uint32_t NODE_OFFSET = 0;
        constexpr uint32_t NODE_COUNT = 1;
        constexpr uint32_t LINK_OFFSET = 2;
        constexpr uint32_t LINK_COUNT = 3;
        constexpr uint32_t NODE_WORDS = 16 + 10;  // 節の欄 LvNode + 節の語
        constexpr uint32_t LINK_WORDS = 10;
        constexpr uint32_t MAX_LEVELS = 64;  // DEPTH_STRIDE に収まる段の数

        static_assert(sizeof(ImGpuNode) == 40 && sizeof(ImGpuLink) == 16, "implicit_levels.hlsl の並び");
        // 見出しは GpuImplicit の計画の段の形へそのまま写す(RecordCopyTo。T-0136)
        static_assert(HEADER_WORDS == IM_PLAN_SHAPE_WORDS && DEPTH_BASE == IM_PLAN_DEPTH_BASE &&
                          DEPTH_STRIDE == IM_PLAN_DEPTH_STRIDE && LEVEL_COUNT == IM_PLAN_LEVEL_COUNT &&
                          NODE_OFFSET == IM_PLAN_NODE_OFFSET && NODE_COUNT == IM_PLAN_NODE_COUNT,
                      "implicit_levels.hlsl の見出しと GpuImplicit の計画の並び");

        // implicit_levels.hlsl の入口(段の順。Lv を除いた名前)
        enum Pass : uint8_t {
            PassClear,
            PassCells,
            PassCellLinks,
            PassNodeHash,
            PassNodeFirst,
            PassParentScanLocal,
            PassParentScanGroups,
            PassParents,
            PassLinkHash,
            PassCount,
            PassRowScanLocal,
            PassRowScanGroups,
            PassPlace,
            PassSort,
            PassAddCoefficients,
            PassCoarseNodes,
            PassFinish,
            PassRoundEnd,
            PassTail,
            PassTotal
        };

        constexpr std::array<std::string_view, PassTotal> PASS_NAMES = {"clear",
                                                                        "cells",
                                                                        "cell_links",
                                                                        "node_hash",
                                                                        "node_first",
                                                                        "parent_scan_local",
                                                                        "parent_scan_groups",
                                                                        "parents",
                                                                        "link_hash",
                                                                        "count",
                                                                        "row_scan_local",
                                                                        "row_scan_groups",
                                                                        "place",
                                                                        "sort",
                                                                        "add_coefficients",
                                                                        "coarse_nodes",
                                                                        "finish",
                                                                        "round_end",
                                                                        "tail"};

        // 回の中の段(述語で飛ばす。RoundEnd は述語の外)
        constexpr std::array<Pass, 14> ROUND_PASSES = {PassNodeHash,
                                                       PassNodeFirst,
                                                       PassParentScanLocal,
                                                       PassParentScanGroups,
                                                       PassParents,
                                                       PassLinkHash,
                                                       PassCount,
                                                       PassRowScanLocal,
                                                       PassRowScanGroups,
                                                       PassPlace,
                                                       PassSort,
                                                       PassAddCoefficients,
                                                       PassCoarseNodes,
                                                       PassFinish};

        uint32_t Groups(uint64_t threads) {
            return std::max<uint32_t>(1, static_cast<uint32_t>((threads + THREADS - 1) / THREADS));
        }

        uint32_t ScanGroups(uint64_t count) {
            return std::max<uint32_t>(1, static_cast<uint32_t>((count + SCAN_THREADS - 1) / SCAN_THREADS));
        }

        void GlobalUavBarrier(ID3D12GraphicsCommandList* list) {
            const D3D12_RESOURCE_BARRIER barrier{.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
                                                 .UAV = {.pResource = nullptr}};
            list->ResourceBarrier(1, &barrier);
        }

        // UAV のバッファの先頭 bytes を読み戻しへ写す(UAV → COPY_SOURCE → UAV)
        void CopyToReadback(ID3D12GraphicsCommandList* list, ID3D12Resource* source, ID3D12Resource* readback,
                            uint64_t bytes) {
            D3D12_RESOURCE_BARRIER barrier = gpu::Transition(source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &barrier);
            list->CopyBufferRegion(readback, 0, source, 0, bytes);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list->ResourceBarrier(1, &barrier);
        }

        template <typename T>
        std::vector<T> Slice(const std::vector<std::byte>& bytes, uint32_t count) {
            std::vector<T> values(count);
            if (count != 0)
                std::memcpy(values.data(), bytes.data(), size_t{count} * sizeof(T));

            return values;
        }

        uint32_t DepthWord(uint32_t kind, uint32_t depth) {
            return DEPTH_BASE + (kind * DEPTH_STRIDE) + depth;
        }

    }  // namespace

    std::expected<GpuImplicitLevels, std::string> GpuImplicitLevels::Create(ID3D12Device5* device,
                                                                            const GpuImplicitBuild& build,
                                                                            const GpuImplicitLevelLimits& limits) {
        const GpuImplicitBuildLimits& buildLimits = build.Limits();
        if (limits.nodes < buildLimits.cells || limits.links == 0 || limits.levels == 0 || limits.levels > MAX_LEVELS)
            return std::unexpected("多重格子の段の上限が正しくない");

        GpuImplicitLevels result;
        Constants& constants = result.m_constants;
        constants.maxCells = buildLimits.cells;
        constants.maxNodes = limits.nodes;
        constants.maxLinks = limits.links;
        constants.levelLinks = 2 * MR_FACES * buildLimits.unknowns;
        constants.maxLevels = limits.levels;
        constants.nodeTableMask = std::bit_ceil(2 * constants.maxCells) - 1;
        constants.linkTableMask = std::bit_ceil(2 * constants.levelLinks) - 1;
        constants.keysWord = build.CellKeysWord();
        constants.listCellsWord = build.ListCellsWord();
        constants.facesByte = static_cast<uint32_t>(build.FacesOffset());
        constants.listsByte = static_cast<uint32_t>(build.ListsOffset());
        constants.tailMaxNodes = limits.tailMaxNodes;
        result.m_dispatchRounds = std::min(limits.dispatchRounds, limits.levels - 1);

        // --- 作業場: 見出し・節・隣・仮の子の一覧・2 つの表・接頭和のグループの和(implicit_levels.hlsl の番地の関数)---
        const uint64_t words = HEADER_WORDS + (uint64_t{NODE_WORDS} * limits.nodes) +
                               (uint64_t{LINK_WORDS} * limits.links) + limits.nodes + constants.nodeTableMask + 1 +
                               constants.linkTableMask + 1 + (2 * (uint64_t{ScanGroups(constants.maxCells)} + 1));
        result.m_workBytes = words * sizeof(uint32_t);

        result.m_rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!result.m_rootSignature)
            return std::unexpected("多重格子の段のルート署名を作れない");

        for (const std::string_view name : PASS_NAMES) {
            const std::string shader = std::format("sim/implicit_levels_{}.cso", name);
            const auto bytecode = gpu::LoadShader(shader);
            if (!bytecode)
                return std::unexpected(bytecode.error());

            auto pipeline = gpu::CreateComputePipeline(device, result.m_rootSignature.Get(), *bytecode);
            if (!pipeline)
                return std::unexpected(std::format("多重格子の段のパイプラインを作れない({})", shader));

            result.m_pipelines.push_back(std::move(pipeline));
        }

        const uint64_t nodeBytes = uint64_t{limits.nodes} * sizeof(ImGpuNode);
        const uint64_t linkBytes = uint64_t{limits.links} * sizeof(ImGpuLink);
        const uint64_t childBytes = uint64_t{limits.nodes} * sizeof(uint32_t);
        result.m_work = gpu::CreateBuffer(device, result.m_workBytes, gpu::BufferKind::UnorderedAccess);
        result.m_nodes = gpu::CreateBuffer(device, nodeBytes, gpu::BufferKind::UnorderedAccess);
        result.m_links = gpu::CreateBuffer(device, linkBytes, gpu::BufferKind::UnorderedAccess);
        result.m_children = gpu::CreateBuffer(device, childBytes, gpu::BufferKind::UnorderedAccess);
        result.m_predicate = gpu::CreateBuffer(device, uint64_t{MAX_LEVELS} * 8, gpu::BufferKind::UnorderedAccess);
        result.m_headerReadback = gpu::CreateBuffer(device, uint64_t{HEADER_WORDS} * 4, gpu::BufferKind::Readback);
        result.m_nodesReadback = gpu::CreateBuffer(device, nodeBytes, gpu::BufferKind::Readback);
        result.m_linksReadback = gpu::CreateBuffer(device, linkBytes, gpu::BufferKind::Readback);
        result.m_childrenReadback = gpu::CreateBuffer(device, childBytes, gpu::BufferKind::Readback);
        result.m_timestampReadback = gpu::CreateBuffer(device, uint64_t{MAX_TIMESTAMPS} * sizeof(uint64_t),
                                                       gpu::BufferKind::Readback);
        if (!result.m_work || !result.m_nodes || !result.m_links || !result.m_children || !result.m_predicate ||
            !result.m_headerReadback || !result.m_nodesReadback || !result.m_linksReadback ||
            !result.m_childrenReadback || !result.m_timestampReadback)
            return std::unexpected("多重格子の段のバッファを作れない");

        const D3D12_QUERY_HEAP_DESC queryDesc{
            .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = MAX_TIMESTAMPS, .NodeMask = 0};
        if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&result.m_timestamps))))
            return std::unexpected("タイムスタンプのヒープを作れない");

        return result;
    }

    void GpuImplicitLevels::Dispatch(ID3D12GraphicsCommandList* list, uint32_t pass, uint32_t groups) {
        list->SetPipelineState(m_pipelines[pass].Get());
        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT, &m_constants, 0);
        list->Dispatch(groups, 1, 1);
        GlobalUavBarrier(list);
    }

    // 回 coarsening の段を、その回の述語の語が 0 でなければ飛ばす(語は前の RoundEnd か CellLinks が UAV で書いた)
    void GpuImplicitLevels::BeginSkippable(ID3D12GraphicsCommandList* list, uint32_t coarsening) {
        const D3D12_RESOURCE_BARRIER barrier = gpu::Transition(m_predicate.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                               D3D12_RESOURCE_STATE_PREDICATION);
        list->ResourceBarrier(1, &barrier);
        list->SetPredication(m_predicate.Get(), uint64_t{coarsening} * 8, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
    }

    void GpuImplicitLevels::EndSkippable(ID3D12GraphicsCommandList* list) {
        list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        const D3D12_RESOURCE_BARRIER barrier = gpu::Transition(m_predicate.Get(), D3D12_RESOURCE_STATE_PREDICATION,
                                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &barrier);
    }

    void GpuImplicitLevels::RecordBuild(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                        const GpuImplicitBuild& build) {
        list->SetComputeRootSignature(m_rootSignature.Get());
        const std::array<ID3D12Resource*, UAV_COUNT> buffers = {
            build.SystemBuffer(), build.WorkBuffer(), m_work.Get(),     m_nodes.Get(),
            m_links.Get(),        m_children.Get(),   m_predicate.Get()};
        for (uint32_t i = 0; i < UAV_COUNT; ++i)
            list->SetComputeRootUnorderedAccessView(i, buffers[i]->GetGPUVirtualAddress());

        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing);

        // --- 段ごとのグループの数(数は GPU が決めるので上限から。超えたスレッドは何もしない。間接の引数は T-0135)---
        const uint32_t cells = m_constants.maxCells;
        const uint32_t links = m_constants.levelLinks;
        const uint32_t both = std::max(cells, links);
        const uint32_t clearWords = std::max(
            {HEADER_WORDS, m_constants.nodeTableMask + 1, m_constants.linkTableMask + 1, m_constants.maxLevels});
        std::array<uint32_t, PassTotal> groups{};
        groups[PassClear] = Groups(clearWords);
        groups[PassCells] = Groups(cells);
        groups[PassCellLinks] = Groups(links);
        groups[PassNodeHash] = Groups(cells);
        groups[PassNodeFirst] = Groups(cells);
        groups[PassParentScanLocal] = ScanGroups(cells);
        groups[PassParentScanGroups] = 1;
        groups[PassParents] = Groups(cells);
        groups[PassLinkHash] = Groups(links);
        groups[PassCount] = Groups(both);
        groups[PassRowScanLocal] = ScanGroups(cells);
        groups[PassRowScanGroups] = 1;
        groups[PassPlace] = Groups(both);
        groups[PassSort] = Groups(both);
        groups[PassAddCoefficients] = Groups(links);
        groups[PassCoarseNodes] = Groups(cells);
        groups[PassFinish] = Groups(both);
        groups[PassRoundEnd] = 1;
        groups[PassTail] = 1;

        // --- 段 0 → 回 d(段 d を縮約して段 d + 1)を Dispatch で m_dispatchRounds 回 → 残りは LvTail(T-0135)---
        m_constants.depth = 0;
        for (const Pass pass : {PassClear, PassCells, PassCellLinks})
            Dispatch(list, pass, groups[pass]);

        for (uint32_t coarsening = 0; coarsening < m_dispatchRounds; ++coarsening) {
            m_constants.depth = coarsening;
            BeginSkippable(list, coarsening);
            for (const Pass pass : ROUND_PASSES)
                Dispatch(list, pass, groups[pass]);

            EndSkippable(list);
            Dispatch(list, PassRoundEnd, groups[PassRoundEnd]);
        }

        if (UsesTail())
            Dispatch(list, PassTail, groups[PassTail]);
    }

    bool GpuImplicitLevels::UsesTail() const {
        return m_dispatchRounds + 1 < m_constants.maxLevels || m_constants.tailMaxNodes != 0;
    }

    uint32_t GpuImplicitLevels::DispatchCount() const {
        return 3 + (UsesTail() ? 1 : 0) + (m_dispatchRounds * static_cast<uint32_t>(ROUND_PASSES.size() + 1));
    }

    void GpuImplicitLevels::RecordCopyTo(ID3D12GraphicsCommandList* list, GpuImplicit& implicit) {
        const std::array<ID3D12Resource*, 4> sources = {m_work.Get(), m_nodes.Get(), m_links.Get(), m_children.Get()};
        std::array<D3D12_RESOURCE_BARRIER, 4> barriers{};
        for (size_t i = 0; i < sources.size(); ++i) {
            barriers[i] = gpu::Transition(sources[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                          D3D12_RESOURCE_STATE_COPY_SOURCE);
        }

        list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        implicit.RecordCopyLevels(list, m_work.Get(), m_nodes.Get(), m_links.Get(), m_children.Get());
        for (D3D12_RESOURCE_BARRIER& barrier : barriers)
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);

        list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    }

    void GpuImplicitLevels::RecordTimestamp(ID3D12GraphicsCommandList* list, uint32_t index) {
        if (index >= MAX_TIMESTAMPS)
            return;

        list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
        m_timestampCount = std::max(m_timestampCount, index + 1);
    }

    void GpuImplicitLevels::RecordReadback(ID3D12GraphicsCommandList* list) {
        CopyToReadback(list, m_work.Get(), m_headerReadback.Get(), uint64_t{HEADER_WORDS} * 4);
        CopyToReadback(list, m_nodes.Get(), m_nodesReadback.Get(), uint64_t{m_constants.maxNodes} * sizeof(ImGpuNode));
        CopyToReadback(list, m_links.Get(), m_linksReadback.Get(), uint64_t{m_constants.maxLinks} * sizeof(ImGpuLink));
        CopyToReadback(list, m_children.Get(), m_childrenReadback.Get(), uint64_t{m_constants.maxNodes} * 4);
        if (m_timestampCount > 0) {
            list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, m_timestampCount,
                                   m_timestampReadback.Get(), 0);
        }
    }

    std::expected<GpuImplicitLevelSystem, std::string> GpuImplicitLevels::Read() const {
        std::array<uint32_t, HEADER_WORDS> header{};
        std::vector<std::byte> nodes(uint64_t{m_constants.maxNodes} * sizeof(ImGpuNode));
        std::vector<std::byte> links(uint64_t{m_constants.maxLinks} * sizeof(ImGpuLink));
        std::vector<std::byte> children(uint64_t{m_constants.maxNodes} * 4);
        if (!gpu::ReadBuffer(m_headerReadback.Get(), std::as_writable_bytes(std::span(header))) ||
            !gpu::ReadBuffer(m_nodesReadback.Get(), nodes) || !gpu::ReadBuffer(m_linksReadback.Get(), links) ||
            !gpu::ReadBuffer(m_childrenReadback.Get(), children))
            return std::unexpected("作った段を読み戻せない");

        GpuImplicitLevelSystem system;
        system.overflow = header[OVERFLOW_WORD] != 0;
        const uint32_t levelCount = header[LEVEL_COUNT];
        if (system.overflow || levelCount == 0 || levelCount > MAX_LEVELS)
            return system;

        // --- 段ごとの始まり(最後は最も粗い段の終わり)---
        GpuImplicitLevelImages& images = system.images;
        for (uint32_t depth = 0; depth < levelCount; ++depth) {
            images.levelOffsets.push_back(header[DepthWord(NODE_OFFSET, depth)]);
            system.linkOffsets.push_back(header[DepthWord(LINK_OFFSET, depth)]);
        }

        const uint32_t last = levelCount - 1;
        images.levelOffsets.push_back(header[DepthWord(NODE_OFFSET, last)] + header[DepthWord(NODE_COUNT, last)]);
        system.linkOffsets.push_back(header[DepthWord(LINK_OFFSET, last)] + header[DepthWord(LINK_COUNT, last)]);
        images.nodes = Slice<ImGpuNode>(nodes, images.levelOffsets.back());
        images.links = Slice<ImGpuLink>(links, system.linkOffsets.back());
        images.children = Slice<uint32_t>(children, images.levelOffsets[last]);

        return system;
    }

    std::vector<uint64_t> GpuImplicitLevels::ReadTimestamps(uint32_t count) const {
        std::vector<uint64_t> ticks(std::min(count, m_timestampCount));
        if (!gpu::ReadBuffer(m_timestampReadback.Get(), std::as_writable_bytes(std::span(ticks))))
            ticks.clear();

        return ticks;
    }

}  // namespace bicameral::sim
