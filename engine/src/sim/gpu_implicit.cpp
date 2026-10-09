// gpu_implicit.cpp — 細かいレベルの熱の陰解法の GPU 版(T-0117)。何をするかは gpu_implicit.h、段の中身は shaders/sim/implicit_conduct.hlsl。
// 段の順は CPU の StepImplicit と同じ: 温度 → V サイクル(VCycle の再帰と同じ順に Smooth・Restrict・Prolong)× 上限 → 面の流れ → 安全網 → 足す。
// 段と段の間は全体の UAV のバリア(次の段は前の段の書き込みを読む)。
// 固定費を減らす形(T-0120): 隣が多い節は 1 グループで足す・小さい段から下の V サイクルは ImTail の 1 Dispatch。
// 段の形は GPU のバッファから(T-0136): 長い行の節の一覧・ImTail の境・Dispatch の大きさは刻みの初めに GPU が作り(ImPlanLevels・ImPlanArgs)、
// V サイクルは記録の上限の段まで ExecuteIndirect で積む。大きさは上限(GpuImplicitLimits)から。
// 記録の形は刻みごとに選べる(T-0154): 積む段の数と最も粗い段の掃き出しを積むか。空の Dispatch とバリアを減らす。
#include "sim/gpu_implicit.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <span>
#include <utility>

#include "common/implicit_conduction.hlsli"
#include "gpu/resources.h"
#include "sim/gpu_implicit_levels.h"

using namespace bicameral::fx;
using namespace bicameral::multires;

namespace bicameral::sim {

    namespace {

        constexpr uint32_t THREADS = 64;  // implicit_conduct.hlsl の IM_THREADS
        constexpr uint32_t STATE_WORDS = 8;
        constexpr uint32_t STATE_CYCLES = 2;
        constexpr uint32_t STATE_LIMIT_DONE = 3;
        constexpr uint32_t STATE_LIMIT_ROUNDS = 5;
        constexpr uint32_t STATE_LIMITED_CELLS = 6;
        constexpr uint32_t WIDE_WORDS = 3;
        constexpr uint32_t WIDE_WORST_EXCESS = 2;
        constexpr uint32_t COLOR_COUNT = 2;

        constexpr uint32_t PREDICATE_CYCLES = 0;  // 述語の語(uint64): V サイクルを止めた
        constexpr uint32_t PREDICATE_LIMIT = 1;   // 安全網を止めた
        constexpr uint32_t PREDICATE_WORDS = 2;

        constexpr uint32_t SWEEP_BITS = 8;

        constexpr uint32_t UAV_COUNT = 11;
        constexpr uint32_t ARGS_BYTES = 3 * sizeof(uint32_t);  // D3D12_DISPATCH_ARGUMENTS

        constexpr uint32_t ROOT_CONSTANT_COUNT = 16;
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
            .uavCount = UAV_COUNT, .rootConstantCount = ROOT_CONSTANT_COUNT, .debugRing = true};

        // implicit_conduct.hlsl の入口(Pass の順)
        constexpr std::array<const char*, 15> SHADERS = {
            "sim/implicit_begin.cso",     "sim/implicit_start.cso",       "sim/implicit_smooth.cso",
            "sim/implicit_restrict.cso",  "sim/implicit_prolong.cso",     "sim/implicit_converged.cso",
            "sim/implicit_cycle_end.cso", "sim/implicit_flows.cso",       "sim/implicit_mark.cso",
            "sim/implicit_limit_end.cso", "sim/implicit_limit_faces.cso", "sim/implicit_apply.cso",
            "sim/implicit_tail.cso",      "sim/implicit_plan_levels.cso", "sim/implicit_plan_args.cso"};

        // RecordUpload が CPU の系から写すバッファ(Buffer の順。ほかは刻みの段が初めに書く)
        constexpr std::array<bool, 11> UPLOADED = {true,  true,  true,  true, true, false,
                                                   false, false, false, true, false};

        // 計画の段の表の語(implicit_levels.hlsl の見出しと同じ並び)
        constexpr uint32_t PlanDepthWord(uint32_t kind, uint32_t depth) {
            return IM_PLAN_DEPTH_BASE + (kind * IM_PLAN_DEPTH_STRIDE) + depth;
        }

        // 段 depth の間接の引数の始まり
        constexpr uint32_t DepthSlot(uint32_t depth) {
            return IM_SLOT_DEPTH_BASE + (depth * IM_SLOT_DEPTH_STRIDE);
        }

        template <typename T>
        std::vector<std::byte> ToBytes(const std::vector<T>& values) {
            std::vector<std::byte> bytes(values.size() * sizeof(T));
            if (!values.empty())
                std::memcpy(bytes.data(), values.data(), bytes.size());

            return bytes;
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

        void GlobalUavBarrier(ID3D12GraphicsCommandList* list) {
            const D3D12_RESOURCE_BARRIER barrier{.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
                                                 .UAV = {.pResource = nullptr}};
            list->ResourceBarrier(1, &barrier);
        }

        std::vector<ImGpuCell> MakeCellImage(const ImplicitGrid& grid, const std::vector<uint32_t>& faceStarts) {
            std::vector<ImGpuCell> cells(grid.cells.size());
            for (size_t i = 0; i < cells.size(); ++i) {
                const ImplicitCell& cell = grid.cells[i];
                cells[i] = {.energy = cell.energy,
                            .fraction = cell.fraction,
                            .heatCapacity = cell.heatCapacity,
                            .startTemperature = cell.startTemperature,
                            .faceStart = faceStarts[i],
                            .faceEnd = faceStarts[i + 1]};
            }

            return cells;
        }

        // セルごとの面の一覧(面の番号の昇順。番号 × 2 + 粗い側なら 1)と、その始まり(セルの数 + 1)
        std::pair<std::vector<uint32_t>, std::vector<uint32_t>> MakeCellFaces(const ImplicitGrid& grid) {
            std::vector<std::vector<uint32_t>> perCell(grid.cells.size());
            for (uint32_t f = 0; f < grid.faces.size(); ++f) {
                perCell[grid.faces[f].fine].push_back(f * 2);
                perCell[grid.faces[f].coarse].push_back((f * 2) + 1);
            }

            std::vector<uint32_t> list;
            std::vector<uint32_t> starts(1, 0);
            for (const std::vector<uint32_t>& faces : perCell) {
                list.insert(list.end(), faces.begin(), faces.end());
                starts.push_back(static_cast<uint32_t>(list.size()));
            }

            return {std::move(list), std::move(starts)};
        }

        struct NodeImages {
            std::vector<ImGpuNode> nodes;
            std::vector<ImGpuLink> links;
        };

        // 多重格子の節(段 0 から順に全部を 1 本に)と隣。節の子の一覧は lists の後ろに足す
        // 段 depth の節の子(1 つ細かい段で親がその節の節。番号の昇順)を lists に足す
        void AppendChildren(const ImplicitGrid& grid, size_t depth, const std::vector<uint32_t>& levelOffsets,
                            std::vector<ImGpuNode>& nodes, std::vector<uint32_t>& lists) {
            const ImplicitGridLevel& finer = grid.levels[depth - 1];
            const uint32_t offset = levelOffsets[depth];
            std::vector<std::vector<uint32_t>> children(grid.levels[depth].levels.size());
            for (uint32_t i = 0; i < finer.parents.size(); ++i)
                children[finer.parents[i]].push_back(levelOffsets[depth - 1] + i);

            for (size_t p = 0; p < children.size(); ++p) {
                nodes[offset + p].childStart = static_cast<uint32_t>(lists.size());
                lists.insert(lists.end(), children[p].begin(), children[p].end());
                nodes[offset + p].childEnd = static_cast<uint32_t>(lists.size());
            }
        }

        NodeImages MakeNodes(const ImplicitGrid& grid, const std::vector<uint32_t>& levelOffsets,
                             std::vector<uint32_t>& lists) {
            std::vector<ImGpuNode> nodes(levelOffsets.back());
            std::vector<ImGpuLink> links;
            for (size_t depth = 0; depth < grid.levels.size(); ++depth) {
                const ImplicitGridLevel& level = grid.levels[depth];
                const uint32_t offset = levelOffsets[depth];
                const bool coarsest = depth + 1 == grid.levels.size();
                for (size_t i = 0; i < level.levels.size(); ++i) {
                    ImGpuNode& node = nodes[offset + i];
                    node.selfWeight = level.selfWeights[i];
                    node.restrictWeight = coarsest ? 0 : level.restrictWeights[i];
                    node.parent = coarsest ? 0 : levelOffsets[depth + 1] + level.parents[i];
                    node.color = level.colors[i];
                    node.linkStart = static_cast<uint32_t>(links.size());
                    for (uint32_t k = level.rowStarts[i]; k < level.rowStarts[i + 1]; ++k)
                        links.push_back({.weight = level.weights[k], .neighbor = offset + level.neighbors[k]});

                    node.linkEnd = static_cast<uint32_t>(links.size());
                }

                if (depth != 0)
                    AppendChildren(grid, depth, levelOffsets, nodes, lists);
            }

            return {.nodes = std::move(nodes), .links = std::move(links)};
        }

    }  // namespace

    std::expected<GpuImplicit, std::string> GpuImplicit::Create(ID3D12Device5* device, const GpuImplicitLimits& limits,
                                                                const GpuImplicitTuning& tuning) {
        if (limits.cells == 0 || limits.nodes < limits.cells || limits.levels == 0 ||
            limits.levels > IM_PLAN_MAX_LEVELS)
            return std::unexpected("陰解法の上限が正しくない");

        GpuImplicit result;
        result.m_limits = limits;
        result.m_tuning = tuning;
        result.m_constants = {.maxNodes = limits.nodes,
                              .maxCells = limits.cells,
                              .maxFaces = limits.faces,
                              .dispatchLevels = std::clamp<uint32_t>(tuning.dispatchLevels, 1, limits.levels),
                              .tailMaxNodes = tuning.tailMaxNodes,
                              .tailMaxLinks = tuning.tailMaxLinks,
                              .coarsestTailMaxNodes = tuning.coarsestTailMaxNodes,
                              .maxLevels = limits.levels};
        result.m_maxDispatchLevels = result.m_constants.dispatchLevels;
        if (auto pipelines = result.CreatePipelines(device); !pipelines)
            return std::unexpected(pipelines.error());

        if (auto buffers = result.CreateBuffers(device); !buffers)
            return std::unexpected(buffers.error());

        return result;
    }

    std::expected<GpuImplicit, std::string> GpuImplicit::Create(ID3D12Device5* device, const ImplicitGrid& grid,
                                                                const GpuImplicitTuning& tuning) {
        if (grid.levels.empty() || grid.cells.empty())
            return std::unexpected("陰解法の段が無い");

        return Create(device, LimitsOf(grid), tuning);
    }

    GpuImplicitLimits GpuImplicit::LimitsOf(const ImplicitGrid& grid) {
        GpuImplicitLimits limits{.cells = static_cast<uint32_t>(grid.cells.size()),
                                 .faces = static_cast<uint32_t>(grid.faces.size()),
                                 .nodes = 0,
                                 .links = 0,
                                 .levels = std::max<uint32_t>(1, static_cast<uint32_t>(grid.levels.size()))};
        for (const ImplicitGridLevel& level : grid.levels) {
            limits.nodes += static_cast<uint32_t>(level.levels.size());
            limits.links += static_cast<uint32_t>(level.neighbors.size());
        }

        return limits;
    }

    // 前の刻みで下りが止まった段(ImTail の境か最も粗い段)まで段ごとに積めば、同じ形の刻みは空の段を積まない。
    // 形が深くなった刻みは、その分を ImTail が回す(値は同じ。その刻みだけ遅い。次の刻みで形が追いつく)
    GpuImplicitRecordShape GpuImplicit::ShapeFrom(const GpuImplicitCost& previous) {
        const uint32_t levels = std::max(previous.levelCount, 1u);
        const uint32_t terminal = std::min(previous.wantedTailDepth, levels - 1);

        return {.dispatchLevels = terminal + 1, .coarsestDispatch = previous.wantedTailDepth >= previous.levelCount};
    }

    // バッファの大きさ(上限から。空にならないように 1 つ分は持つ)
    uint64_t GpuImplicit::BufferBytes(Buffer buffer) const {
        const uint64_t nodes = m_limits.nodes;
        const uint64_t cells = m_limits.cells;
        const uint64_t faces = m_limits.faces;
        switch (buffer) {
            case BufferCells: return cells * sizeof(ImGpuCell);
            case BufferFaces: return std::max<uint64_t>(1, faces) * sizeof(ImGpuFace);
            case BufferLists: return ((2 * faces) + nodes + 1) * sizeof(uint32_t);
            case BufferNodes: return nodes * sizeof(ImGpuNode);
            case BufferLinks: return std::max<uint64_t>(1, m_limits.links) * sizeof(ImGpuLink);
            case BufferWork: return ((3 * nodes) + cells + (2 * faces) + 1) * sizeof(int64_t);
            case BufferState: return (STATE_WORDS + cells) * sizeof(uint32_t);
            case BufferWide: return uint64_t{WIDE_WORDS} * sizeof(int64_t);
            case BufferPredicate: return uint64_t{PREDICATE_WORDS} * sizeof(uint64_t);
            case BufferPlan: return (IM_PLAN_FLAGS_BASE + (4 * nodes)) * sizeof(uint32_t);
            case BufferArgs:
                return uint64_t{IM_SLOT_DEPTH_BASE + (IM_SLOT_DEPTH_STRIDE * m_maxDispatchLevels)} * ARGS_BYTES;
            default: return 0;
        }
    }

    std::expected<void, std::string> GpuImplicit::CreatePipelines(ID3D12Device5* device) {
        m_rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!m_rootSignature)
            return std::unexpected("陰解法のルート署名を作れない");

        for (uint32_t pass = 0; pass < PassCount; ++pass) {
            const auto bytecode = gpu::LoadShader(SHADERS[pass]);
            if (!bytecode)
                return std::unexpected(bytecode.error());

            m_pipelines[pass] = gpu::CreateComputePipeline(device, m_rootSignature.Get(), *bytecode);
            if (!m_pipelines[pass])
                return std::unexpected(std::format("陰解法のパイプラインを作れない({})", SHADERS[pass]));
        }

        // --- 間接の Dispatch(引数は ImPlanArgs が書く。ルート定数は変えないので署名は要らない)---
        const D3D12_INDIRECT_ARGUMENT_DESC argument{.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
        const D3D12_COMMAND_SIGNATURE_DESC signature{
            .ByteStride = ARGS_BYTES, .NumArgumentDescs = 1, .pArgumentDescs = &argument, .NodeMask = 0};
        if (FAILED(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&m_dispatchSignature))))
            return std::unexpected("陰解法の間接の Dispatch の署名を作れない");

        return {};
    }

    std::expected<void, std::string> GpuImplicit::CreateBuffers(ID3D12Device5* device) {
        for (uint32_t i = 0; i < BufferCount; ++i) {
            const auto buffer = static_cast<Buffer>(i);
            m_buffers[i] = gpu::CreateBuffer(device, BufferBytes(buffer), gpu::BufferKind::UnorderedAccess);
            if (!m_buffers[i])
                return std::unexpected("陰解法のバッファを作れない");

            if (!UPLOADED[i])
                continue;

            const uint64_t uploadBytes = buffer == BufferPlan ? uint64_t{IM_PLAN_HEADER_WORDS} * sizeof(uint32_t)
                                                              : BufferBytes(buffer);
            m_uploads[i] = gpu::CreateBuffer(device, uploadBytes, gpu::BufferKind::Upload);
            if (!m_uploads[i])
                return std::unexpected("陰解法の写すバッファを作れない");
        }

        m_cellsReadback = gpu::CreateBuffer(device, BufferBytes(BufferCells), gpu::BufferKind::Readback);
        m_stateReadback = gpu::CreateBuffer(device, BufferBytes(BufferState), gpu::BufferKind::Readback);
        m_wideReadback = gpu::CreateBuffer(device, BufferBytes(BufferWide), gpu::BufferKind::Readback);
        m_planReadback = gpu::CreateBuffer(device, uint64_t{IM_PLAN_HEADER_WORDS} * sizeof(uint32_t),
                                           gpu::BufferKind::Readback);
        m_timestampReadback = gpu::CreateBuffer(device, uint64_t{MAX_TIMESTAMPS} * sizeof(uint64_t),
                                                gpu::BufferKind::Readback);
        if (!m_cellsReadback || !m_stateReadback || !m_wideReadback || !m_planReadback || !m_timestampReadback)
            return std::unexpected("陰解法の読み戻しを作れない");

        const D3D12_QUERY_HEAP_DESC queryDesc{
            .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = MAX_TIMESTAMPS, .NodeMask = 0};
        if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_timestamps))))
            return std::unexpected("タイムスタンプのヒープを作れない");

        return {};
    }

    // CPU の系を GPU の並びに: 節は段 0(= セル)から順に全部を 1 本に、隣・親・子は全体の番号に。
    // 番号の一覧は面の一覧(面の数 × 2)→ 面の上限 × 2 まで空き → 子の一覧。計画の見出しに段の表と面の数
    bool GpuImplicit::RecordUpload(ID3D12GraphicsCommandList* list, const ImplicitGrid& grid) {
        const GpuImplicitLimits need = LimitsOf(grid);
        if (grid.cells.empty() || need.cells > m_limits.cells || need.faces > m_limits.faces ||
            need.nodes > m_limits.nodes || need.links > m_limits.links || need.levels > m_limits.levels)
            return false;

        std::vector<uint32_t> levelOffsets(1, 0);
        for (const ImplicitGridLevel& level : grid.levels)
            levelOffsets.push_back(levelOffsets.back() + static_cast<uint32_t>(level.levels.size()));

        auto [lists, faceStarts] = MakeCellFaces(grid);
        auto [nodes, links] = MakeNodes(grid, levelOffsets, lists);
        lists.insert(lists.begin() + static_cast<std::ptrdiff_t>(2 * grid.faces.size()),
                     size_t{2} * (m_limits.faces - need.faces), 0u);
        if (links.empty())
            links.push_back({});

        std::vector<ImGpuFace> faces(std::max<size_t>(1, grid.faces.size()));
        for (size_t f = 0; f < grid.faces.size(); ++f) {
            const ImplicitFace& face = grid.faces[f];
            faces[f] = {.coefficientHigh = face.coefficient.hi,
                        .coefficientLow = face.coefficient.lo,
                        .fine = face.fine,
                        .coarse = face.coarse,
                        .gap = face.gap,
                        .coarseFraction = grid.cells[face.coarse].coarseFraction ? 1u : 0u};
        }

        std::vector<uint32_t> plan(IM_PLAN_HEADER_WORDS, 0);
        plan[IM_PLAN_LEVEL_COUNT] = need.levels;
        plan[IM_PLAN_FACES] = need.faces;
        for (uint32_t depth = 0; depth < grid.levels.size(); ++depth) {
            plan[PlanDepthWord(IM_PLAN_NODE_OFFSET, depth)] = levelOffsets[depth];
            plan[PlanDepthWord(IM_PLAN_NODE_COUNT, depth)] = levelOffsets[depth + 1] - levelOffsets[depth];
        }

        std::array<std::vector<std::byte>, BufferCount> images;
        images[BufferCells] = ToBytes(MakeCellImage(grid, faceStarts));
        images[BufferFaces] = ToBytes(faces);
        images[BufferLists] = ToBytes(lists);
        images[BufferNodes] = ToBytes(nodes);
        images[BufferLinks] = ToBytes(links);
        images[BufferPlan] = ToBytes(plan);

        // --- 写すものは COMMON から暗黙に COPY_DEST へ昇格して写し、ほかは COMMON から。どれも UAV へ ---
        std::array<D3D12_RESOURCE_BARRIER, BufferCount> barriers{};
        for (uint32_t i = 0; i < BufferCount; ++i) {
            barriers[i] = gpu::Transition(m_buffers[i].Get(), D3D12_RESOURCE_STATE_COMMON,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (!UPLOADED[i])
                continue;

            if (!WriteUpload(m_uploads[i].Get(), images[i]))
                return false;

            list->CopyBufferRegion(m_buffers[i].Get(), 0, m_uploads[i].Get(), 0, images[i].size());
            barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        }

        list->ResourceBarrier(BufferCount, barriers.data());

        return true;
    }

    void GpuImplicit::RecordReset(ID3D12GraphicsCommandList* list) {
        std::array<D3D12_RESOURCE_BARRIER, BufferCount> barriers{};
        for (uint32_t i = 0; i < BufferCount; ++i) {
            barriers[i] = gpu::Transition(m_buffers[i].Get(), D3D12_RESOURCE_STATE_COMMON,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        list->ResourceBarrier(BufferCount, barriers.data());
    }

    void GpuImplicit::RecordCopySystem(ID3D12GraphicsCommandList* list, ID3D12Resource* source, uint64_t cellsOffset,
                                       uint64_t facesOffset, uint64_t listsOffset, ID3D12Resource* header,
                                       uint64_t faceCountOffset) {
        const std::array<CopyRegion, 4> regions = {
            CopyRegion{.target = BufferCells,
                       .targetOffset = 0,
                       .source = source,
                       .sourceOffset = cellsOffset,
                       .bytes = BufferBytes(BufferCells)},
            CopyRegion{.target = BufferFaces,
                       .targetOffset = 0,
                       .source = source,
                       .sourceOffset = facesOffset,
                       .bytes = BufferBytes(BufferFaces)},
            CopyRegion{.target = BufferLists,
                       .targetOffset = 0,
                       .source = source,
                       .sourceOffset = listsOffset,
                       .bytes = uint64_t{2} * m_limits.faces * sizeof(uint32_t)},
            CopyRegion{.target = BufferPlan,
                       .targetOffset = uint64_t{IM_PLAN_FACES} * sizeof(uint32_t),
                       .source = header,
                       .sourceOffset = faceCountOffset,
                       .bytes = sizeof(uint32_t)}};
        RecordCopies(list, regions);
    }

    void GpuImplicit::RecordCopyLevels(ID3D12GraphicsCommandList* list, ID3D12Resource* header, ID3D12Resource* nodes,
                                       ID3D12Resource* links, ID3D12Resource* children) {
        const uint64_t childrenOffset = uint64_t{2} * m_limits.faces * sizeof(uint32_t);
        const std::array<CopyRegion, 4> regions = {
            CopyRegion{.target = BufferPlan,
                       .targetOffset = 0,
                       .source = header,
                       .sourceOffset = 0,
                       .bytes = uint64_t{IM_PLAN_SHAPE_WORDS} * sizeof(uint32_t)},
            CopyRegion{.target = BufferNodes,
                       .targetOffset = 0,
                       .source = nodes,
                       .sourceOffset = 0,
                       .bytes = BufferBytes(BufferNodes)},
            CopyRegion{.target = BufferLinks,
                       .targetOffset = 0,
                       .source = links,
                       .sourceOffset = 0,
                       .bytes = BufferBytes(BufferLinks)},
            CopyRegion{.target = BufferLists,
                       .targetOffset = childrenOffset,
                       .source = children,
                       .sourceOffset = 0,
                       .bytes = BufferBytes(BufferLists) - childrenOffset}};
        RecordCopies(list, regions);
    }

    // 写す(大きさは写す先の残りと source の残りの小さい方)。写す先は UAV → COPY_DEST → UAV
    void GpuImplicit::RecordCopies(ID3D12GraphicsCommandList* list, std::span<const CopyRegion> regions) {
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        for (const CopyRegion& region : regions) {
            ID3D12Resource* target = m_buffers[region.target].Get();
            const bool seen = std::ranges::any_of(barriers, [&](const D3D12_RESOURCE_BARRIER& barrier) {
                return barrier.Transition.pResource == target;
            });
            if (!seen) {
                barriers.push_back(
                    gpu::Transition(target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST));
            }
        }

        list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        for (const CopyRegion& region : regions) {
            const uint64_t sourceBytes = region.source->GetDesc().Width;
            const uint64_t targetBytes = BufferBytes(region.target);
            if (region.sourceOffset >= sourceBytes || region.targetOffset >= targetBytes)
                continue;

            const uint64_t bytes = std::min(
                {region.bytes, sourceBytes - region.sourceOffset, targetBytes - region.targetOffset});
            if (bytes != 0) {
                list->CopyBufferRegion(m_buffers[region.target].Get(), region.targetOffset, region.source,
                                       region.sourceOffset, bytes);
            }
        }

        for (D3D12_RESOURCE_BARRIER& barrier : barriers)
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);

        list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    }

    // 比べる用の段の形の像(GpuImplicit が Create で写すものと同じ。T-0134)
    GpuImplicitLevelImages MakeGpuImplicitLevelImages(const ImplicitGrid& grid) {
        GpuImplicitLevelImages images;
        images.levelOffsets.assign(1, 0);
        for (const ImplicitGridLevel& level : grid.levels)
            images.levelOffsets.push_back(images.levelOffsets.back() + static_cast<uint32_t>(level.levels.size()));

        auto [lists, faceStarts] = MakeCellFaces(grid);
        auto [nodes, links] = MakeNodes(grid, images.levelOffsets, lists);
        images.nodes = std::move(nodes);
        images.links = std::move(links);
        images.children.assign(lists.begin() + static_cast<std::ptrdiff_t>(2 * grid.faces.size()), lists.end());

        return images;
    }

    void GpuImplicit::Dispatch(ID3D12GraphicsCommandList* list, Pass pass, uint32_t groups) {
        list->SetPipelineState(m_pipelines[pass].Get());
        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT, &m_constants, 0);
        list->Dispatch(groups, 1, 1);
        GlobalUavBarrier(list);
    }

    // 間接の Dispatch: グループの数は計画の引数 slot(ImPlanArgs が書く。その段が要らなければ 0)
    void GpuImplicit::DispatchIndirect(ID3D12GraphicsCommandList* list, Pass pass, uint32_t slot) {
        list->SetPipelineState(m_pipelines[pass].Get());
        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT, &m_constants, 0);
        list->ExecuteIndirect(m_dispatchSignature.Get(), 1, m_buffers[BufferArgs].Get(), uint64_t{slot} * ARGS_BYTES,
                              nullptr, 0);
        GlobalUavBarrier(list);
    }

    // 段の形から長い行の節の一覧・ImTail の境・間接の引数を作り、引数を INDIRECT_ARGUMENT にする(T-0136)
    void GpuImplicit::RecordPlan(ID3D12GraphicsCommandList* list) {
        Dispatch(list, PassPlanLevels, m_constants.maxLevels);
        Dispatch(list, PassPlanArgs, 1);
        const D3D12_RESOURCE_BARRIER barrier = gpu::Transition(
            m_buffers[BufferArgs].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        list->ResourceBarrier(1, &barrier);
    }

    // 赤黒の掃き出し sweeps 回(1 回 = 色 0〜1。CPU の Smooth)。slot + 色が引数
    void GpuImplicit::RecordSmooth(ID3D12GraphicsCommandList* list, uint32_t depth, uint32_t slot, uint32_t sweeps) {
        m_constants.depth = depth;
        for (uint32_t sweep = 0; sweep < sweeps; ++sweep) {
            for (uint32_t color = 0; color < COLOR_COUNT; ++color) {
                m_constants.color = color;
                DispatchIndirect(list, PassSmooth, slot + color);
            }
        }
    }

    // CPU の VCycle の再帰を、記録の上限の段まで開いた形: 下り(前の掃き出し → 縮約)× 段 → 下りが止まる段(ImTail か最も粗い段の掃き出し)
    // → 上り(直し → 後の掃き出し)× 段。下りが止まる段より下の Dispatch は引数が 0 グループ(ImPlanArgs)
    void GpuImplicit::RecordVCycle(ID3D12GraphicsCommandList* list, const ImplicitOptions& options) {
        const uint32_t levels = m_constants.dispatchLevels;
        for (uint32_t depth = 0; depth + 1 < levels; ++depth) {
            const uint32_t slot = DepthSlot(depth);
            RecordSmooth(list, depth, slot + IM_SLOT_SMOOTH, options.preSmooth);
            m_constants.depth = depth + 1;
            DispatchIndirect(list, PassRestrict, slot + IM_SLOT_RESTRICT);
        }

        DispatchIndirect(list, PassTail, IM_SLOT_TAIL);
        if (m_constants.coarsestDispatch != 0)
            RecordSmooth(list, IM_TERMINAL_DEPTH, IM_SLOT_COARSEST, options.coarsestSweeps);

        for (uint32_t depth = levels - 1; depth-- > 0;) {
            const uint32_t slot = DepthSlot(depth);
            m_constants.depth = depth;
            DispatchIndirect(list, PassProlong, slot + IM_SLOT_PROLONG);
            RecordSmooth(list, depth, slot + IM_SLOT_SMOOTH, options.postSmooth);
        }
    }

    // ここから EndSkippable までの Dispatch を、述語の語 word が 0 でなければ飛ばす(語は前の段が UAV で書いた。バリアは飛ばない)
    void GpuImplicit::BeginSkippable(ID3D12GraphicsCommandList* list, uint32_t word) {
        if (!m_predication)
            return;

        ID3D12Resource* predicate = m_buffers[BufferPredicate].Get();
        const D3D12_RESOURCE_BARRIER barrier = gpu::Transition(predicate, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                               D3D12_RESOURCE_STATE_PREDICATION);
        list->ResourceBarrier(1, &barrier);
        list->SetPredication(predicate, uint64_t{word} * 8, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO);
    }

    void GpuImplicit::EndSkippable(ID3D12GraphicsCommandList* list) {
        if (!m_predication)
            return;

        list->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        const D3D12_RESOURCE_BARRIER barrier = gpu::Transition(
            m_buffers[BufferPredicate].Get(), D3D12_RESOURCE_STATE_PREDICATION, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &barrier);
    }

    bool GpuImplicit::RecordStep(ID3D12GraphicsCommandList* list, uint64_t debugRing, const ImplicitOptions& options,
                                 uint32_t maxLimitRounds, const GpuImplicitRecordShape& shape) {
        if (options.method != ImplicitMethod::Multigrid)
            return false;

        // 記録の形(T-0154)。計画(ImPlanArgs)も同じ段の数で ImTail の境を切る
        const uint32_t requested = shape.dispatchLevels == 0 ? m_maxDispatchLevels : shape.dispatchLevels;
        m_constants.dispatchLevels = std::clamp(requested, 1u, m_maxDispatchLevels);
        m_constants.coarsestDispatch = shape.coarsestDispatch ? 1 : 0;

        list->SetComputeRootSignature(m_rootSignature.Get());
        for (uint32_t i = 0; i < BufferCount; ++i)
            list->SetComputeRootUnorderedAccessView(i, m_buffers[i]->GetGPUVirtualAddress());

        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing);
        m_constants.tolerance = options.toleranceMillikelvin;
        m_constants.slack = options.limitSlackMillikelvin;
        m_constants.correctionScale = static_cast<int32_t>(options.correctionScale);
        m_constants.sweeps = options.preSmooth | (options.postSmooth << SWEEP_BITS) |
                             (options.coarsestSweeps << (2 * SWEEP_BITS));

        // --- 計画 → 温度・範囲 → V サイクル × 上限(止めた後の回は述語で飛ばす)---
        RecordPlan(list);
        Dispatch(list, PassBegin, 1);
        DispatchIndirect(list, PassStart, IM_SLOT_CELLS);
        for (uint32_t cycle = 0; cycle < options.cycles; ++cycle) {
            const bool skippable = cycle != 0 && options.toleranceMillikelvin != 0;
            if (skippable)
                BeginSkippable(list, PREDICATE_CYCLES);

            RecordVCycle(list, options);

            if (options.toleranceMillikelvin != 0)
                DispatchIndirect(list, PassConverged, IM_SLOT_CONVERGED);

            Dispatch(list, PassCycleEnd, 1);
            if (skippable)
                EndSkippable(list);
        }

        // --- 面の流れ → 安全網(印 → 止めるか → 面を戻す)× (上限 + 1)→ 足す ---
        DispatchIndirect(list, PassFlows, IM_SLOT_FACES);
        for (uint32_t pass = 0; pass <= maxLimitRounds; ++pass) {
            if (pass != 0)
                BeginSkippable(list, PREDICATE_LIMIT);

            DispatchIndirect(list, PassMark, IM_SLOT_CELLS);
            Dispatch(list, PassLimitEnd, 1);
            DispatchIndirect(list, PassLimitFaces, IM_SLOT_FACES);
            if (pass != 0)
                EndSkippable(list);
        }

        DispatchIndirect(list, PassApply, IM_SLOT_CELLS);

        const D3D12_RESOURCE_BARRIER barrier = gpu::Transition(
            m_buffers[BufferArgs].Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &barrier);

        return true;
    }

    uint32_t GpuImplicit::DispatchesPerCycle(const ImplicitOptions& options) const {
        const uint32_t perLevel = (COLOR_COUNT * (options.preSmooth + options.postSmooth)) + 2;
        const uint32_t judge = options.toleranceMillikelvin != 0 ? 2 : 1;

        const uint32_t coarsest = m_constants.coarsestDispatch != 0 ? COLOR_COUNT * options.coarsestSweeps : 0;

        return ((m_constants.dispatchLevels - 1) * perLevel) + 1 + coarsest + judge;
    }

    void GpuImplicit::RecordTimestamp(ID3D12GraphicsCommandList* list, uint32_t index) {
        if (index >= MAX_TIMESTAMPS)
            return;

        list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
        m_timestampCount = std::max(m_timestampCount, index + 1);
    }

    void GpuImplicit::RecordRelease(ID3D12GraphicsCommandList* list) {
        std::array<D3D12_RESOURCE_BARRIER, BufferCount> barriers{};
        for (uint32_t i = 0; i < BufferCount; ++i) {
            barriers[i] = gpu::Transition(m_buffers[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                          D3D12_RESOURCE_STATE_COMMON);
        }

        list->ResourceBarrier(BufferCount, barriers.data());
    }

    void GpuImplicit::RecordReadback(ID3D12GraphicsCommandList* list) {
        gpu::RecordCopyToReadback(list, m_buffers[BufferCells].Get(), m_cellsReadback.Get());
        RecordCostReadback(list);
    }

    void GpuImplicit::RecordCostReadback(ID3D12GraphicsCommandList* list) {
        gpu::RecordCopyToReadback(list, m_buffers[BufferState].Get(), m_stateReadback.Get());
        gpu::RecordCopyToReadback(list, m_buffers[BufferWide].Get(), m_wideReadback.Get());

        D3D12_RESOURCE_BARRIER barrier = gpu::Transition(
            m_buffers[BufferPlan].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        list->CopyBufferRegion(m_planReadback.Get(), 0, m_buffers[BufferPlan].Get(), 0,
                               uint64_t{IM_PLAN_HEADER_WORDS} * sizeof(uint32_t));
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        if (m_timestampCount > 0) {
            list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, m_timestampCount,
                                   m_timestampReadback.Get(), 0);
        }
    }

    bool GpuImplicit::Read(ImplicitGrid& grid, GpuImplicitCost& cost) const {
        if (grid.cells.size() > m_limits.cells)
            return false;

        std::vector<ImGpuCell> cells(grid.cells.size());
        if (!gpu::ReadBuffer(m_cellsReadback.Get(), std::as_writable_bytes(std::span(cells))) || !ReadCost(cost))
            return false;

        for (size_t i = 0; i < cells.size(); ++i) {
            grid.cells[i].energy = cells[i].energy;
            grid.cells[i].fraction = cells[i].fraction;
        }

        return true;
    }

    bool GpuImplicit::ReadCost(GpuImplicitCost& cost) const {
        std::vector<uint32_t> state(STATE_WORDS);
        std::vector<int64_t> wide(WIDE_WORDS);
        std::vector<uint32_t> plan(IM_PLAN_HEADER_WORDS);
        if (!gpu::ReadBuffer(m_stateReadback.Get(), std::as_writable_bytes(std::span(state))) ||
            !gpu::ReadBuffer(m_wideReadback.Get(), std::as_writable_bytes(std::span(wide))) ||
            !gpu::ReadBuffer(m_planReadback.Get(), std::as_writable_bytes(std::span(plan))))
            return false;

        cost = {.cycles = state[STATE_CYCLES],
                .limitedCells = state[STATE_LIMITED_CELLS],
                .limitRounds = state[STATE_LIMIT_ROUNDS],
                .worstExcessMillikelvin = wide[WIDE_WORST_EXCESS],
                .limitFinished = state[STATE_LIMIT_DONE] != 0,
                .levelCount = plan[IM_PLAN_LEVEL_COUNT],
                .tailDepth = plan[IM_PLAN_TAIL],
                .wantedTailDepth = plan[IM_PLAN_WANTED_TAIL]};

        return true;
    }

    std::vector<uint64_t> GpuImplicit::ReadTimestamps(uint32_t count) const {
        std::vector<uint64_t> ticks(std::min(count, m_timestampCount));
        if (!gpu::ReadBuffer(m_timestampReadback.Get(), std::as_writable_bytes(std::span(ticks))))
            ticks.clear();

        return ticks;
    }

}  // namespace bicameral::sim
