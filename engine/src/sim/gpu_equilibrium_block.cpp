// gpu_equilibrium_block.cpp — 成分を持つ節の釣り合いの GPU 版(gpu_equilibrium_block.h。T-0970)。段の中身は shaders/sim/equilibrium_block.hlsl。
// CPU リファレンスの順(equilibrium_block.cpp の SolveEquilibriumBlocks): 右辺 = 右辺の項 + 固定した面 → V サイクルを cycles 回
//   (8 色の Gauss–Seidel を preSmooth 回 → 残差を Pᵀ で親へ → 親で直し〔最も粗い段は coarsestSweeps 回〕→ P で子へ足す → postSmooth 回)。
// 段の形(節・隣・親子)は CPU の BuildEquilibriumBlockGrid が作ったものを写し、粗い段の係数(Galerkin)と節の LDLᵀ は GPU が作る。
// 色の中の節どうしは隣でないので、色ごとに 1 回の Dispatch で節を並べて解いても CPU の 1 本の回しと同じ値になる(R3)。
#include "sim/gpu_equilibrium_block.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>

#include "core/aliases.h"

namespace bicameral::sim {

    using namespace equilibrium_block;

    namespace {

        constexpr uint32_t THREADS_PER_GROUP = 64;  // equilibrium_block.hlsl の numthreads
        constexpr uint32_t ROOT_CONSTANT_COUNT = 6;
        constexpr std::array<uint32_t, 4> SHADER_COMPONENTS = {1, 2, 3, 6};
        constexpr std::array<const char*, 5> PASS_NAMES = {"finish", "coarsen", "smooth", "restrict", "prolong"};

        // u0 係数・u1 作業場 / b0 / デバッグのリング / t0 節・t1 添字
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
            .uavCount = 2, .rootConstantCount = ROOT_CONSTANT_COUNT, .debugRing = true, .srvCount = 2};

        uint32_t GroupsFor(uint32_t threads) {
            return (threads + THREADS_PER_GROUP - 1) / THREADS_PER_GROUP;
        }

        // k × k(置き場の [行 × 6 + 列])を詰めた k² 語へ
        void PackBlock(std::vector<int64_t>& image, uint32_t word, const EquilibriumBlock& block, uint32_t k) {
            for (uint32_t row = 0; row < k; ++row) {
                for (uint32_t column = 0; column < k; ++column)
                    image[word + (row * k) + column] = block[(row * EQ_MAX_COMPONENTS) + column];
            }
        }

        // 一覧を順のまま、または逆に
        std::vector<uint32_t> Ordered(std::vector<uint32_t> items, bool reverse) {
            if (reverse)
                rng::reverse(items);

            return items;
        }

        bool WriteUpload(ID3D12Resource* upload, std::span<const int64_t> words) {
            void* mapped = nullptr;
            const D3D12_RANGE noRead{.Begin = 0, .End = 0};
            if (FAILED(upload->Map(0, &noRead, &mapped)))
                return false;

            std::memcpy(mapped, words.data(), words.size_bytes());
            upload->Unmap(0, nullptr);

            return true;
        }

    }  // namespace

    bool GpuEquilibriumBlock::Supports(uint32_t components) {
        return rng::find(SHADER_COMPONENTS, components) != SHADER_COMPONENTS.end();
    }

    // --- 段の形: 節(全部の段を続けた番号)と添字の一覧(隣 → 子 → 色) ---
    void GpuEquilibriumBlock::BuildShape(const EquilibriumBlockSystem& system, const EquilibriumBlockGrid& grid,
                                         std::vector<EqbGpuNode>& nodes, std::vector<uint32_t>& indices) {
        const bool reverse = m_options.reverseOrder;
        std::vector<uint32_t> offsets(1, 0);
        for (const EquilibriumBlockLevel& level : grid.levels)
            offsets.push_back(offsets.back() + (uint32_t)level.x.size());

        m_nodeCapacity = offsets.back();
        nodes.assign(m_nodeCapacity, EqbGpuNode{});
        m_levels.resize(grid.levels.size());

        // --- 隣(全部の段。段 0 の写し方を覚える)---
        for (size_t depth = 0; depth < grid.levels.size(); ++depth) {
            const EquilibriumBlockLevel& level = grid.levels[depth];
            m_levels[depth].first = offsets[depth];
            m_levels[depth].count = (uint32_t)level.x.size();
            for (size_t i = 0; i < level.x.size(); ++i) {
                EqbGpuNode& node = nodes[offsets[depth] + i];
                node.rowStart = (uint32_t)indices.size();
                std::vector<uint32_t> links;
                for (uint32_t link = level.rowStarts[i]; link < level.rowStarts[i + 1]; ++link)
                    links.push_back(link);

                for (const uint32_t link : Ordered(links, reverse)) {
                    indices.push_back(offsets[depth] + level.neighbors[link]);
                    if (depth == 0)
                        m_linkSource.push_back(link);
                }

                node.rowEnd = (uint32_t)indices.size();
                // 最も粗い段は親を持たない(CPU の段は捨てた粗い段の親を残すことがある)
                const bool hasParent = depth + 1 < grid.levels.size();
                node.parent = hasParent ? offsets[depth + 1] + level.parents[i] : 0;
                node.octant = hasParent ? level.octants[i] : 0;
            }
        }

        m_linkCapacity = (uint32_t)indices.size();
        m_cellLinks = (uint32_t)m_linkSource.size();

        // --- 子(段 1 から)---
        for (size_t depth = 1; depth < grid.levels.size(); ++depth) {
            std::vector<std::vector<uint32_t>> children(grid.levels[depth].x.size());
            const EquilibriumBlockLevel& fine = grid.levels[depth - 1];
            for (size_t i = 0; i < fine.x.size(); ++i)
                children[fine.parents[i]].push_back(offsets[depth - 1] + (uint32_t)i);

            for (size_t i = 0; i < children.size(); ++i) {
                EqbGpuNode& node = nodes[offsets[depth] + i];
                node.childStart = (uint32_t)indices.size();
                for (const uint32_t child : Ordered(children[i], reverse))
                    indices.push_back(child);

                node.childEnd = (uint32_t)indices.size();
            }
        }

        // --- 色の一覧(段ごと・色ごと)---
        for (size_t depth = 0; depth < grid.levels.size(); ++depth) {
            const EquilibriumBlockLevel& level = grid.levels[depth];
            for (uint32_t color = 0; color < EQB_OCTANTS; ++color) {
                std::vector<uint32_t> members;
                for (size_t i = 0; i < level.x.size(); ++i) {
                    if (level.colors[i] == color)
                        members.push_back(offsets[depth] + (uint32_t)i);
                }

                m_levels[depth].colorStarts[color] = (uint32_t)indices.size();
                m_levels[depth].colorCounts[color] = (uint32_t)members.size();
                for (const uint32_t member : Ordered(members, reverse))
                    indices.push_back(member);
            }
        }

        // --- 固定した面(段 0 の節ごとにまとめる)---
        std::vector<std::vector<uint32_t>> fixedByNode(system.nodes.size());
        for (uint32_t f = 0; f < system.fixedFaces.size(); ++f)
            fixedByNode[system.fixedFaces[f].node].push_back(f);

        for (size_t i = 0; i < fixedByNode.size(); ++i) {
            nodes[i].fixedStart = (uint32_t)m_fixedSource.size();
            for (const uint32_t f : Ordered(fixedByNode[i], reverse))
                m_fixedSource.push_back(f);

            nodes[i].fixedEnd = (uint32_t)m_fixedSource.size();
        }

        m_fixedCapacity = (uint32_t)m_fixedSource.size();
    }

    bool GpuEquilibriumBlock::CreateBuffers(std::span<const EqbGpuNode> nodes, std::span<const uint32_t> indices) {
        const uint32_t k = m_components;
        m_nodes = gpu::CreateResidentBuffer(m_device, std::as_bytes(nodes));
        m_indices = gpu::CreateResidentBuffer(m_device, std::as_bytes(indices));
        const uint64_t coefficientBytes = uint64_t{EqbCoefficientWords(k, m_nodeCapacity, m_linkCapacity,
                                                                       m_fixedCapacity)} *
                                          sizeof(int64_t);
        const uint64_t workBytes = uint64_t{EqbWorkWords(k, m_nodeCapacity, m_cells)} * sizeof(int64_t);
        const uint64_t valueBytes = uint64_t{m_cells} * k * sizeof(int64_t);
        m_coefficients = gpu::CreateBuffer(m_device, coefficientBytes, gpu::BufferKind::UnorderedAccess);
        m_work = gpu::CreateBuffer(m_device, workBytes, gpu::BufferKind::UnorderedAccess);
        m_valuesReadback = gpu::CreateBuffer(m_device, valueBytes * std::max(1u, m_options.readbackSlots),
                                             gpu::BufferKind::Readback);
        m_timestampReadback = gpu::CreateBuffer(m_device, uint64_t{MAX_TIMESTAMPS} * sizeof(uint64_t),
                                                gpu::BufferKind::Readback);
        const D3D12_QUERY_HEAP_DESC queryDesc{
            .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = MAX_TIMESTAMPS, .NodeMask = 0};
        if (FAILED(m_device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_timestamps))))
            return false;

        return m_nodes.Valid() && m_indices.Valid() && m_coefficients && m_work && m_valuesReadback &&
               m_timestampReadback;
    }

    std::expected<GpuEquilibriumBlock, std::string> GpuEquilibriumBlock::Create(
        ID3D12Device5* device, const EquilibriumBlockSystem& system, const EquilibriumBlockGrid& grid,
        const GpuEquilibriumBlockOptions& options) {
        if (!Supports(system.components) || grid.levels.empty() || system.nodes.empty())
            return std::unexpected(std::format("成分を持つ節の GPU 版で解けない系(成分 {})", system.components));

        GpuEquilibriumBlock result;
        result.m_device = device;
        result.m_options = options;
        result.m_components = system.components;
        result.m_coefficientShift = system.coefficientShift;
        result.m_cells = (uint32_t)system.nodes.size();

        std::vector<EqbGpuNode> nodes;
        std::vector<uint32_t> indices;
        result.BuildShape(system, grid, nodes, indices);
        if (!result.CreateBuffers(nodes, indices))
            return std::unexpected("成分を持つ節の GPU のバッファを作れない");

        // --- パイプライン(k ごとのシェーダー)---
        result.m_rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!result.m_rootSignature)
            return std::unexpected("成分を持つ節のルート署名を作れない");

        for (size_t pass = 0; pass < PASS_NAMES.size(); ++pass) {
            const std::string name = std::format("sim/equilibrium_block_k{}_{}.cso", system.components,
                                                 PASS_NAMES[pass]);
            const auto bytecode = gpu::LoadShader(name);
            if (!bytecode)
                return std::unexpected(bytecode.error());

            result.m_pipelines[pass] = gpu::CreateComputePipeline(device, result.m_rootSignature.Get(), *bytecode);
            if (!result.m_pipelines[pass])
                return std::unexpected(std::format("パイプラインを作れない: {}", name));
        }

        return result;
    }

    // --- 写す像 ---
    std::vector<int64_t> GpuEquilibriumBlock::CoefficientImage(const EquilibriumBlockSystem& system,
                                                               const EquilibriumBlockGrid& grid) const {
        const uint32_t k = m_components;
        std::vector<int64_t> image(EqbCoefficientWords(k, m_nodeCapacity, m_linkCapacity, m_fixedCapacity), 0);
        for (uint32_t octant = 0; octant < EQB_OCTANTS; ++octant) {
            PackBlock(image, EqbProlongWord(k, octant), grid.prolongation[octant], k);
            PackBlock(image, EqbRestrictWord(k, octant), grid.restriction[octant], k);
        }

        const EquilibriumBlockLevel& cells = grid.levels.front();
        for (uint32_t node = 0; node < m_cells; ++node)
            PackBlock(image, EqbDiagonalWord(k, node), cells.diagonals[node], k);

        for (uint32_t slot = 0; slot < m_cellLinks; ++slot)
            PackBlock(image, EqbBlockWord(k, m_nodeCapacity, slot), cells.blocks[m_linkSource[slot]], k);

        for (uint32_t slot = 0; slot < m_fixedCapacity; ++slot) {
            const EquilibriumBlockFixed& fixed = system.fixedFaces[m_fixedSource[slot]];
            const uint32_t word = EqbFixedWord(k, m_nodeCapacity, m_linkCapacity, slot);
            PackBlock(image, word, fixed.block, k);
            for (uint32_t c = 0; c < k; ++c)
                image[word + (k * k) + c] = fixed.value[c];
        }

        return image;
    }

    std::vector<int64_t> GpuEquilibriumBlock::WorkImage(const EquilibriumBlockSystem& system,
                                                        std::span<const int64_t> warmValues) const {
        const uint32_t k = m_components;
        std::vector<int64_t> image(EqbWorkWords(k, m_nodeCapacity, m_cells), 0);
        if (warmValues.size() == size_t{m_cells} * k)
            rng::copy(warmValues, image.begin() + EqbValueWord(k, 0));

        for (const EquilibriumBlockSource& source : system.sources)
            image[EqbSourceWord(k, m_nodeCapacity, source.node) + source.component] += source.value;

        return image;
    }

    // --- 記録 ---
    void GpuEquilibriumBlock::RecordStateChange(ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES before,
                                                D3D12_RESOURCE_STATES after) const {
        const std::array<D3D12_RESOURCE_BARRIER, 2> barriers = {gpu::Transition(m_coefficients.Get(), before, after),
                                                                gpu::Transition(m_work.Get(), before, after)};
        list->ResourceBarrier((uint32_t)barriers.size(), barriers.data());
    }

    void GpuEquilibriumBlock::RecordBegin(ID3D12GraphicsCommandList* list) {
        if (m_residentPending) {
            const std::array<gpu::ResidentBuffer, 2> resident = {m_nodes, m_indices};
            gpu::RecordResidentUploads(list, resident);
            m_residentPending = false;
        }

        RecordStateChange(list, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    void GpuEquilibriumBlock::RecordEnd(ID3D12GraphicsCommandList* list) const {
        RecordStateChange(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    }

    bool GpuEquilibriumBlock::RecordUpload(ID3D12GraphicsCommandList* list, const EquilibriumBlockSystem& system,
                                           const EquilibriumBlockGrid& grid, std::span<const int64_t> warmValues) {
        if (system.components != m_components || system.nodes.size() != m_cells ||
            system.fixedFaces.size() != m_fixedCapacity || grid.levels.size() != m_levels.size() ||
            grid.levels.front().neighbors.size() != m_cellLinks)
            return false;

        const std::vector<int64_t> coefficients = CoefficientImage(system, grid);
        const std::vector<int64_t> work = WorkImage(system, warmValues);
        m_coefficientUpload = gpu::CreateBuffer(m_device, coefficients.size() * sizeof(int64_t),
                                                gpu::BufferKind::Upload);
        m_workUpload = gpu::CreateBuffer(m_device, work.size() * sizeof(int64_t), gpu::BufferKind::Upload);
        if (!m_coefficientUpload || !m_workUpload || !WriteUpload(m_coefficientUpload.Get(), coefficients) ||
            !WriteUpload(m_workUpload.Get(), work))
            return false;

        RecordStateChange(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(m_coefficients.Get(), 0, m_coefficientUpload.Get(), 0,
                               coefficients.size() * sizeof(int64_t));
        list->CopyBufferRegion(m_work.Get(), 0, m_workUpload.Get(), 0, work.size() * sizeof(int64_t));
        RecordStateChange(list, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        return true;
    }

    void GpuEquilibriumBlock::ReleaseStaging() {
        m_coefficientUpload.Reset();
        m_workUpload.Reset();
    }

    GpuEquilibriumBlock::Constants GpuEquilibriumBlock::BaseConstants() const {
        return {.nodeCapacity = m_nodeCapacity, .linkCapacity = m_linkCapacity, .shift = m_coefficientShift};
    }

    void GpuEquilibriumBlock::RecordPass(ID3D12GraphicsCommandList* list, uint64_t debugRing, Pass pass,
                                         Constants constants) const {
        static_assert(sizeof(Constants) == ROOT_CONSTANT_COUNT * sizeof(uint32_t));
        if (constants.count == 0)
            return;

        list->SetComputeRootSignature(m_rootSignature.Get());
        list->SetPipelineState(m_pipelines[(size_t)pass].Get());
        list->SetComputeRootUnorderedAccessView(0, m_coefficients->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(1, m_work->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT, &constants, 0);
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing);
        list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(0), m_nodes.buffer->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(1), m_indices.buffer->GetGPUVirtualAddress());
        list->Dispatch(GroupsFor(constants.count), 1, 1);

        const D3D12_RESOURCE_BARRIER barrier = gpu::UavBarrier(nullptr);
        list->ResourceBarrier(1, &barrier);
    }

    void GpuEquilibriumBlock::RecordPrepare(ID3D12GraphicsCommandList* list, uint64_t debugRing) const {
        Constants constants = BaseConstants();
        constants.first = 0;
        constants.count = m_cells;
        RecordPass(list, debugRing, Pass::Finish, constants);
        for (size_t depth = 1; depth < m_levels.size(); ++depth) {
            constants.first = m_levels[depth].first;
            constants.count = m_levels[depth].count;
            RecordPass(list, debugRing, Pass::Coarsen, constants);
        }
    }

    void GpuEquilibriumBlock::RecordSmooth(ID3D12GraphicsCommandList* list, uint64_t debugRing, const Level& level,
                                           uint32_t sweeps) const {
        Constants constants = BaseConstants();
        for (uint32_t sweep = 0; sweep < sweeps; ++sweep) {
            for (uint32_t color = 0; color < EQB_OCTANTS; ++color) {
                constants.list = level.colorStarts[color];
                constants.count = level.colorCounts[color];
                RecordPass(list, debugRing, Pass::Smooth, constants);
            }
        }
    }

    // equilibrium_block.cpp の VCycle と同じ順(粗い段の直しは Restrict が 0 にする)
    void GpuEquilibriumBlock::RecordCycle(ID3D12GraphicsCommandList* list, uint64_t debugRing, size_t depth) const {
        const EquilibriumBlockOptions& solve = m_options.solve;
        const Level& level = m_levels[depth];
        if (depth + 1 == m_levels.size()) {
            RecordSmooth(list, debugRing, level, solve.coarsestSweeps);
            return;
        }

        RecordSmooth(list, debugRing, level, solve.preSmooth);

        Constants constants = BaseConstants();
        constants.first = m_levels[depth + 1].first;
        constants.count = m_levels[depth + 1].count;
        RecordPass(list, debugRing, Pass::Restrict, constants);

        RecordCycle(list, debugRing, depth + 1);

        constants.first = level.first;
        constants.count = level.count;
        RecordPass(list, debugRing, Pass::Prolong, constants);

        RecordSmooth(list, debugRing, level, solve.postSmooth);
    }

    void GpuEquilibriumBlock::RecordSolve(ID3D12GraphicsCommandList* list, uint64_t debugRing) const {
        for (uint32_t cycle = 0; cycle < m_options.solve.cycles; ++cycle)
            RecordCycle(list, debugRing, 0);
    }

    uint64_t GpuEquilibriumBlock::DispatchesPerSolve() const {
        const EquilibriumBlockOptions& solve = m_options.solve;
        uint64_t count = 0;
        for (size_t depth = 0; depth < m_levels.size(); ++depth) {
            const uint64_t colors = (uint64_t)rng::count_if(m_levels[depth].colorCounts,
                                                            [](uint32_t members) { return members > 0; });
            const bool coarsest = depth + 1 == m_levels.size();
            count += coarsest ? colors * solve.coarsestSweeps : colors * (solve.preSmooth + solve.postSmooth) + 2;
        }

        return count * solve.cycles;
    }

    // --- 読み戻し・計測 ---
    void GpuEquilibriumBlock::RecordValuesReadback(ID3D12GraphicsCommandList* list, uint32_t slot) const {
        const uint64_t bytes = uint64_t{m_cells} * m_components * sizeof(int64_t);
        D3D12_RESOURCE_BARRIER barrier = gpu::Transition(m_work.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                         D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        list->CopyBufferRegion(m_valuesReadback.Get(), slot * bytes, m_work.Get(),
                               uint64_t{EqbValueWord(m_components, 0)} * sizeof(int64_t), bytes);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
    }

    bool GpuEquilibriumBlock::ReadValues(std::vector<int64_t>& values, uint32_t slot) const {
        const size_t count = size_t{m_cells} * m_components;
        if (slot >= std::max(1u, m_options.readbackSlots))
            return false;

        std::vector<int64_t> all(count * (slot + 1));
        if (!gpu::ReadBuffer(m_valuesReadback.Get(), std::as_writable_bytes(std::span(all))))
            return false;

        values.assign(all.begin() + static_cast<std::ptrdiff_t>(count * slot), all.end());

        return true;
    }

    void GpuEquilibriumBlock::RecordTimestamp(ID3D12GraphicsCommandList* list, uint32_t index) const {
        if (index < MAX_TIMESTAMPS)
            list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
    }

    void GpuEquilibriumBlock::RecordTimestampReadback(ID3D12GraphicsCommandList* list, uint32_t count) const {
        list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, std::min(count, MAX_TIMESTAMPS),
                               m_timestampReadback.Get(), 0);
    }

    std::vector<uint64_t> GpuEquilibriumBlock::ReadTimestamps(uint32_t count) const {
        std::vector<uint64_t> ticks(std::min(count, MAX_TIMESTAMPS));
        if (!gpu::ReadBuffer(m_timestampReadback.Get(), std::as_writable_bytes(std::span(ticks))))
            ticks.clear();

        return ticks;
    }

    uint64_t GpuEquilibriumBlock::DeviceBytes() const {
        return m_coefficients->GetDesc().Width + m_work->GetDesc().Width + m_nodes.buffer->GetDesc().Width +
               m_indices.buffer->GetDesc().Width;
    }

}  // namespace bicameral::sim
