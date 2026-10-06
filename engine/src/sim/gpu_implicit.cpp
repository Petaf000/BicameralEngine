// gpu_implicit.cpp — 細かいレベルの熱の陰解法の GPU 版(T-0117)。何をするかは gpu_implicit.h、段の中身は shaders/sim/implicit_conduct.hlsl。
// 段の順は CPU の StepImplicit と同じ: 温度 → V サイクル(VCycle の再帰と同じ順に Smooth・Restrict・Prolong)× 上限 → 面の流れ → 安全網 → 足す。
// 段と段の間は全体の UAV のバリア(次の段は前の段の書き込みを読む)。
// 固定費を減らす形(T-0120): 隣が多い節は 1 グループで足す(節の並び u9)・小さい段から下の V サイクルは ImTail の 1 Dispatch。
#include "sim/gpu_implicit.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <span>
#include <utility>

#include "common/implicit_conduction.hlsli"
#include "gpu/resources.h"

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

        // 隣がこれより多い節は 1 グループ = 1 節で足す(T-0120。熱い点では違うレベルの面の粗い側で 127 個)
        constexpr uint32_t LONG_ROW_LINKS = 16;

        constexpr uint32_t SWEEP_BITS = 8;

        constexpr uint32_t UAV_COUNT = 10;

        constexpr uint32_t ROOT_CONSTANT_COUNT = 16;
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
            .uavCount = UAV_COUNT, .rootConstantCount = ROOT_CONSTANT_COUNT, .debugRing = true};

        // implicit_conduct.hlsl の入口(Pass の順)
        constexpr std::array<const char*, 13> SHADERS = {
            "sim/implicit_begin.cso",     "sim/implicit_start.cso",       "sim/implicit_smooth.cso",
            "sim/implicit_restrict.cso",  "sim/implicit_prolong.cso",     "sim/implicit_converged.cso",
            "sim/implicit_cycle_end.cso", "sim/implicit_flows.cso",       "sim/implicit_mark.cso",
            "sim/implicit_limit_end.cso", "sim/implicit_limit_faces.cso", "sim/implicit_apply.cso",
            "sim/implicit_tail.cso"};

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

        // 節の並びを作る時に見る段の形(写す前の節と番号の一覧)
        struct NodeShape {
            std::vector<ImGpuNode> nodes;
            std::vector<uint32_t> lists;

            NodeShape(const std::vector<std::byte>& nodeImage, const std::vector<std::byte>& listImage)
                : nodes(nodeImage.size() / sizeof(ImGpuNode)), lists(listImage.size() / sizeof(uint32_t)) {
                std::memcpy(nodes.data(), nodeImage.data(), nodes.size() * sizeof(ImGpuNode));
                std::memcpy(lists.data(), listImage.data(), lists.size() * sizeof(uint32_t));
            }

            [[nodiscard]] uint32_t Links(uint32_t node) const { return nodes[node].linkEnd - nodes[node].linkStart; }

            // 掃き出しの色 color で計算する節なら隣の数、写すだけなら 0
            [[nodiscard]] uint32_t ColorLinks(uint32_t node, uint32_t color) const {
                return nodes[node].color == color ? Links(node) : 0;
            }

            [[nodiscard]] uint32_t MostChildLinks(uint32_t node) const {
                uint32_t most = 0;
                for (uint32_t k = nodes[node].childStart; k < nodes[node].childEnd; ++k)
                    most = std::max(most, Links(lists[k]));

                return most;
            }
        };

        // 節 begin〜end を order に足す: weight が LONG_ROW_LINKS 以下の節 → 超える節(どちらも番号の昇順)。足した数 {前, 後ろ}
        template <typename Weight>
        std::pair<uint32_t, uint32_t> AppendOrder(std::vector<uint32_t>& order, uint32_t begin, uint32_t end,
                                                  const Weight& weight) {
            const size_t start = order.size();
            for (uint32_t node = begin; node < end; ++node) {
                if (weight(node) <= LONG_ROW_LINKS)
                    order.push_back(node);
            }

            const auto shortCount = static_cast<uint32_t>(order.size() - start);
            for (uint32_t node = begin; node < end; ++node) {
                if (weight(node) > LONG_ROW_LINKS)
                    order.push_back(node);
            }

            return {shortCount, static_cast<uint32_t>(order.size() - start) - shortCount};
        }

        // ImTail が受け持つ最初の段: そこから最も粗い段まで、どの段も節が tailMaxNodes 以下で隣が tailMaxLinks 以下。
        // そういう段が無くても、最も粗い段の節が coarsestTailMaxNodes 以下なら受け持つ(最も粗い段の掃き出しを 1 Dispatch に)。
        // 受け持たない時は段の数を返す(最も粗い段も掃き出しの Dispatch で回す)
        uint32_t FindTailDepth(const NodeShape& shape, const std::vector<uint32_t>& levelOffsets,
                               const GpuImplicitTuning& tuning) {
            const auto smallLevel = [&](uint32_t depth) {
                uint32_t most = 0;
                for (uint32_t node = levelOffsets[depth]; node < levelOffsets[depth + 1]; ++node)
                    most = std::max(most, shape.Links(node));

                return levelOffsets[depth + 1] - levelOffsets[depth] <= tuning.tailMaxNodes &&
                       most <= tuning.tailMaxLinks;
            };

            const auto levelCount = static_cast<uint32_t>(levelOffsets.size() - 1);
            uint32_t depth = levelCount;
            while (depth > 0 && smallLevel(depth - 1))
                --depth;

            if (depth < levelCount)
                return depth;

            const uint32_t coarsestNodes = levelOffsets[levelCount] - levelOffsets[levelCount - 1];

            return coarsestNodes <= tuning.coarsestTailMaxNodes ? levelCount - 1 : levelCount;
        }

    }  // namespace

    std::expected<GpuImplicit, std::string> GpuImplicit::Create(ID3D12Device5* device, const ImplicitGrid& grid,
                                                                const GpuImplicitTuning& tuning) {
        if (grid.levels.empty() || grid.cells.empty())
            return std::unexpected("陰解法の段が無い");

        GpuImplicit result;
        result.m_tuning = tuning;
        result.MakeImages(grid);
        if (auto pipelines = result.CreatePipelines(device); !pipelines)
            return std::unexpected(pipelines.error());

        if (auto buffers = result.CreateBuffers(device); !buffers)
            return std::unexpected(buffers.error());

        return result;
    }

    // 段の形を GPU の並びに: 節は段 0(= セル)から順に全部を 1 本に、隣・親・子は全体の番号に
    void GpuImplicit::MakeImages(const ImplicitGrid& grid) {
        m_cellCount = static_cast<uint32_t>(grid.cells.size());
        m_faceCount = static_cast<uint32_t>(grid.faces.size());
        m_levelOffsets.assign(1, 0);
        for (const ImplicitGridLevel& level : grid.levels)
            m_levelOffsets.push_back(m_levelOffsets.back() + static_cast<uint32_t>(level.levels.size()));

        m_nodeTotal = m_levelOffsets.back();

        // --- セルの面の一覧と、節の子の一覧(番号の一覧は 1 本: 面 → 子)---
        auto [lists, faceStarts] = MakeCellFaces(grid);
        auto [nodes, links] = MakeNodes(grid, m_levelOffsets, lists);

        std::vector<ImGpuFace> faces(m_faceCount);
        for (size_t f = 0; f < faces.size(); ++f) {
            const ImplicitFace& face = grid.faces[f];
            faces[f] = {.coefficientHigh = face.coefficient.hi,
                        .coefficientLow = face.coefficient.lo,
                        .fine = face.fine,
                        .coarse = face.coarse,
                        .gap = face.gap,
                        .coarseFraction = grid.cells[face.coarse].coarseFraction ? 1u : 0u};
        }

        if (lists.empty())
            lists.push_back(0);

        if (links.empty())
            links.push_back({});

        m_images[BufferCells] = ToBytes(MakeCellImage(grid, faceStarts));
        m_images[BufferFaces] = ToBytes(faces.empty() ? std::vector<ImGpuFace>(1) : faces);
        m_images[BufferLists] = ToBytes(lists);
        m_images[BufferNodes] = ToBytes(nodes);
        m_images[BufferLinks] = ToBytes(links);
        m_images[BufferWork].resize((size_t{3} * m_nodeTotal + m_cellCount + size_t{2} * m_faceCount + 1) * 8);
        m_images[BufferState].resize((size_t{STATE_WORDS} + m_cellCount) * 4);
        m_images[BufferWide].resize(size_t{WIDE_WORDS} * 8);
        m_images[BufferPredicate].resize(size_t{PREDICATE_WORDS} * 8);
        MakeOrders(m_images[BufferNodes], m_images[BufferLists]);

        m_constants.nodeTotal = m_nodeTotal;
        m_constants.cellCount = m_cellCount;
        m_constants.faceCount = m_faceCount;
    }

    // 隣を足す段の節の並び(段ごと): 隣が少ない節を前に、多い節を後ろに(どちらも番号の昇順)。
    // 掃き出しは色ごとに、その色の節の隣の数で分ける(ほかの色の節は写すだけなので 1 スレッドで)。
    // 縮約は子の隣の数の最大で分ける(1 スレッドの子の回しが長い行に止められないように)
    void GpuImplicit::MakeOrders(const std::vector<std::byte>& nodeImage, const std::vector<std::byte>& listImage) {
        const NodeShape shape(nodeImage, listImage);
        std::vector<uint32_t> order;
        const auto append = [&](uint32_t depth, auto&& weight) {
            const auto start = static_cast<uint32_t>(order.size());
            const auto [shortCount, longCount] = AppendOrder(order, m_levelOffsets[depth], m_levelOffsets[depth + 1],
                                                             weight);
            return OrderRange{.start = start, .shortCount = shortCount, .longCount = longCount};
        };

        const auto levelCount = static_cast<uint32_t>(m_levelOffsets.size() - 1);
        m_smoothOrders.clear();
        m_restrictOrders.assign(1, OrderRange{});
        for (uint32_t depth = 0; depth < levelCount; ++depth) {
            std::array<OrderRange, COLOR_COUNT> colors{};
            for (uint32_t color = 0; color < COLOR_COUNT; ++color)
                colors[color] = append(depth, [&](uint32_t node) { return shape.ColorLinks(node, color); });

            m_smoothOrders.push_back(colors);
        }

        m_convergedOrder = append(0, [&](uint32_t node) { return shape.Links(node); });
        for (uint32_t depth = 1; depth < levelCount; ++depth)
            m_restrictOrders.push_back(append(depth, [&](uint32_t node) { return shape.MostChildLinks(node); }));

        // --- ImTail: 段ごとの節の始まりと、受け持つ最初の段 ---
        m_constants.tailStart = static_cast<uint32_t>(order.size());
        order.insert(order.end(), m_levelOffsets.begin(), m_levelOffsets.end());
        m_tailDepth = FindTailDepth(shape, m_levelOffsets, m_tuning);
        m_constants.tailDepth = m_tailDepth;
        m_constants.levelTotal = levelCount;

        m_images[BufferOrder] = ToBytes(order);
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

        return {};
    }

    std::expected<void, std::string> GpuImplicit::CreateBuffers(ID3D12Device5* device) {
        for (uint32_t i = 0; i < BufferCount; ++i) {
            const uint64_t size = m_images[i].size();
            m_buffers[i] = gpu::CreateBuffer(device, size, gpu::BufferKind::UnorderedAccess);
            m_uploads[i] = gpu::CreateBuffer(device, size, gpu::BufferKind::Upload);
            if (!m_buffers[i] || !m_uploads[i])
                return std::unexpected("陰解法のバッファを作れない");
        }

        m_cellsReadback = gpu::CreateBuffer(device, m_images[BufferCells].size(), gpu::BufferKind::Readback);
        m_stateReadback = gpu::CreateBuffer(device, m_images[BufferState].size(), gpu::BufferKind::Readback);
        m_wideReadback = gpu::CreateBuffer(device, m_images[BufferWide].size(), gpu::BufferKind::Readback);
        m_timestampReadback = gpu::CreateBuffer(device, uint64_t{MAX_TIMESTAMPS} * sizeof(uint64_t),
                                                gpu::BufferKind::Readback);
        if (!m_cellsReadback || !m_stateReadback || !m_wideReadback || !m_timestampReadback)
            return std::unexpected("陰解法の読み戻しを作れない");

        const D3D12_QUERY_HEAP_DESC queryDesc{
            .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = MAX_TIMESTAMPS, .NodeMask = 0};
        if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_timestamps))))
            return std::unexpected("タイムスタンプのヒープを作れない");

        return {};
    }

    bool GpuImplicit::RecordUpload(ID3D12GraphicsCommandList* list, const ImplicitGrid& grid) {
        if (grid.cells.size() != m_cellCount || grid.faces.size() != m_faceCount)
            return false;

        // --- セルのエネルギー・端数だけ作り直す(面の一覧の始まりは同じ)---
        std::vector<ImGpuCell> cells(m_cellCount);
        std::memcpy(cells.data(), m_images[BufferCells].data(), m_images[BufferCells].size());
        for (size_t i = 0; i < cells.size(); ++i) {
            cells[i].energy = grid.cells[i].energy;
            cells[i].fraction = grid.cells[i].fraction;
            cells[i].startTemperature = grid.cells[i].startTemperature;
        }

        m_images[BufferCells] = ToBytes(cells);

        std::array<D3D12_RESOURCE_BARRIER, BufferCount> barriers{};
        for (uint32_t i = 0; i < BufferCount; ++i) {
            if (!WriteUpload(m_uploads[i].Get(), m_images[i]))
                return false;

            // COMMON から暗黙に COPY_DEST へ昇格する。写した後は UAV へ
            list->CopyResource(m_buffers[i].Get(), m_uploads[i].Get());
            barriers[i] = gpu::Transition(m_buffers[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        list->ResourceBarrier(BufferCount, barriers.data());

        return true;
    }

    void GpuImplicit::Dispatch(ID3D12GraphicsCommandList* list, Pass pass, uint32_t threads) {
        list->SetPipelineState(m_pipelines[pass].Get());
        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT, &m_constants, 0);
        list->Dispatch(std::max<uint32_t>(1, (threads + THREADS - 1) / THREADS), 1, 1);
        GlobalUavBarrier(list);
    }

    // 節の並びの区間を回す段: 1 スレッドで足す節のグループ + グループで足す節 1 つに 1 グループ
    void GpuImplicit::DispatchOrdered(ID3D12GraphicsCommandList* list, Pass pass, const OrderRange& range) {
        m_constants.orderStart = range.start;
        m_constants.shortCount = range.shortCount;
        m_constants.longCount = range.longCount;
        const uint32_t groups = ((range.shortCount + THREADS - 1) / THREADS) + range.longCount;
        list->SetPipelineState(m_pipelines[pass].Get());
        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT, &m_constants, 0);
        list->Dispatch(std::max<uint32_t>(1, groups), 1, 1);
        GlobalUavBarrier(list);
    }

    // 赤黒の掃き出し sweeps 回(1 回 = 色 0〜1。CPU の Smooth)
    void GpuImplicit::RecordSmooth(ID3D12GraphicsCommandList* list, uint32_t depth, uint32_t sweeps) {
        for (uint32_t sweep = 0; sweep < sweeps; ++sweep) {
            for (uint32_t color = 0; color < COLOR_COUNT; ++color) {
                m_constants.color = color;
                DispatchOrdered(list, PassSmooth, m_smoothOrders[depth][color]);
            }
        }
    }

    // CPU の VCycle と同じ再帰
    void GpuImplicit::RecordVCycle(ID3D12GraphicsCommandList* list, uint32_t depth, const ImplicitOptions& options) {
        if (depth == m_tailDepth) {
            m_constants.sweeps = options.preSmooth | (options.postSmooth << SWEEP_BITS) |
                                 (options.coarsestSweeps << (2 * SWEEP_BITS));
            Dispatch(list, PassTail, 1);
            return;
        }

        if (depth + 1 == LevelCount()) {
            RecordSmooth(list, depth, options.coarsestSweeps);
            return;
        }

        RecordSmooth(list, depth, options.preSmooth);

        DispatchOrdered(list, PassRestrict, m_restrictOrders[depth + 1]);

        RecordVCycle(list, depth + 1, options);

        m_constants.levelOffset = m_levelOffsets[depth];
        m_constants.levelCount = m_levelOffsets[depth + 1] - m_levelOffsets[depth];
        Dispatch(list, PassProlong, m_constants.levelCount);

        RecordSmooth(list, depth, options.postSmooth);
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
                                 uint32_t maxLimitRounds) {
        if (options.method != ImplicitMethod::Multigrid)
            return false;

        list->SetComputeRootSignature(m_rootSignature.Get());
        for (uint32_t i = 0; i < BufferCount; ++i)
            list->SetComputeRootUnorderedAccessView(i, m_buffers[i]->GetGPUVirtualAddress());

        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing);
        m_constants.tolerance = options.toleranceMillikelvin;
        m_constants.slack = options.limitSlackMillikelvin;
        m_constants.correctionScale = static_cast<int32_t>(options.correctionScale);

        // --- 温度・範囲 → V サイクル × 上限(止めた後の回は述語で飛ばす)---
        Dispatch(list, PassBegin, 1);
        Dispatch(list, PassStart, m_cellCount);
        for (uint32_t cycle = 0; cycle < options.cycles; ++cycle) {
            const bool skippable = cycle != 0 && options.toleranceMillikelvin != 0;
            if (skippable)
                BeginSkippable(list, PREDICATE_CYCLES);

            RecordVCycle(list, 0, options);
            if (options.toleranceMillikelvin != 0)
                DispatchOrdered(list, PassConverged, m_convergedOrder);

            Dispatch(list, PassCycleEnd, 1);
            if (skippable)
                EndSkippable(list);
        }

        // --- 面の流れ → 安全網(印 → 止めるか → 面を戻す)× (上限 + 1)→ 足す ---
        Dispatch(list, PassFlows, m_faceCount);
        for (uint32_t pass = 0; pass <= maxLimitRounds; ++pass) {
            if (pass != 0)
                BeginSkippable(list, PREDICATE_LIMIT);

            Dispatch(list, PassMark, m_cellCount);
            Dispatch(list, PassLimitEnd, 1);
            Dispatch(list, PassLimitFaces, m_faceCount);
            if (pass != 0)
                EndSkippable(list);
        }

        Dispatch(list, PassApply, m_cellCount);

        return true;
    }

    uint32_t GpuImplicit::DispatchesPerCycle(const ImplicitOptions& options) const {
        const uint32_t smoothing = COLOR_COUNT * (options.preSmooth + options.postSmooth);

        const uint32_t judge = options.toleranceMillikelvin != 0 ? 2 : 1;
        if (m_tailDepth >= LevelCount())
            return ((LevelCount() - 1) * (smoothing + 2)) + (COLOR_COUNT * options.coarsestSweeps) + judge;

        return (m_tailDepth * (smoothing + 2)) + 1 + judge;
    }

    void GpuImplicit::RecordTimestamp(ID3D12GraphicsCommandList* list, uint32_t index) {
        if (index >= MAX_TIMESTAMPS)
            return;

        list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
        m_timestampCount = std::max(m_timestampCount, index + 1);
    }

    void GpuImplicit::RecordReadback(ID3D12GraphicsCommandList* list) {
        gpu::RecordCopyToReadback(list, m_buffers[BufferCells].Get(), m_cellsReadback.Get());
        gpu::RecordCopyToReadback(list, m_buffers[BufferState].Get(), m_stateReadback.Get());
        gpu::RecordCopyToReadback(list, m_buffers[BufferWide].Get(), m_wideReadback.Get());
        if (m_timestampCount > 0) {
            list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, m_timestampCount,
                                   m_timestampReadback.Get(), 0);
        }
    }

    bool GpuImplicit::Read(ImplicitGrid& grid, GpuImplicitCost& cost) const {
        if (grid.cells.size() != m_cellCount)
            return false;

        std::vector<ImGpuCell> cells(m_cellCount);
        std::vector<uint32_t> state(STATE_WORDS);
        std::vector<int64_t> wide(WIDE_WORDS);
        if (!gpu::ReadBuffer(m_cellsReadback.Get(), std::as_writable_bytes(std::span(cells))) ||
            !gpu::ReadBuffer(m_stateReadback.Get(), std::as_writable_bytes(std::span(state))) ||
            !gpu::ReadBuffer(m_wideReadback.Get(), std::as_writable_bytes(std::span(wide))))
            return false;

        for (size_t i = 0; i < cells.size(); ++i) {
            grid.cells[i].energy = cells[i].energy;
            grid.cells[i].fraction = cells[i].fraction;
        }

        cost = {.cycles = state[STATE_CYCLES],
                .limitedCells = state[STATE_LIMITED_CELLS],
                .limitRounds = state[STATE_LIMIT_ROUNDS],
                .worstExcessMillikelvin = wide[WIDE_WORST_EXCESS],
                .limitFinished = state[STATE_LIMIT_DONE] != 0};

        return true;
    }

    std::vector<uint64_t> GpuImplicit::ReadTimestamps(uint32_t count) const {
        std::vector<uint64_t> ticks(std::min(count, m_timestampCount));
        if (!gpu::ReadBuffer(m_timestampReadback.Get(), std::as_writable_bytes(std::span(ticks))))
            ticks.clear();

        return ticks;
    }

}  // namespace bicameral::sim
