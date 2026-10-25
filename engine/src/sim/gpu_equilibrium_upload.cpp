// gpu_equilibrium_upload.cpp — 部品の GPU 版を CPU が組んだ系で回す口(gpu_equilibrium_upload.h。T-1111)。段 0 の段は shaders/sim/equilibrium_upload.hlsl。
// CPU リファレンスの順(equilibrium.cpp の SolveEquilibrium の閉じない道): Diagonals・SourceRhs・MakeLocal の段 0(この口の段)
//   → BuildImplicitGrid の粗い段(部品の EquilibriumLevel)→ CycleImplicitGrid を cycles 回(部品の RecordSolve を 1 回ずつ)。
#include "sim/gpu_equilibrium_upload.h"

#include <algorithm>
#include <array>
#include <format>

#include "common/equilibrium.hlsli"
#include "common/implicit_conduction.hlsli"

using namespace bicameral::fx;
using namespace bicameral::multires;

namespace bicameral::sim {

    namespace {

        constexpr uint32_t THREADS_PER_GROUP = 64;  // equilibrium_upload.hlsl の numthreads
        constexpr uint32_t ROOT_CONSTANT_COUNT = 5;
        constexpr uint32_t FACES_MAX = 6;  // EQ_GPU_FACES_MAX

        // --- 段 0 の写しの並び(equilibrium_upload.hlsl の EQ_UPLOAD_*)---
        constexpr uint32_t NODE_WORDS = 18;
        constexpr uint32_t COEFFICIENTS = 2;
        constexpr uint32_t NEIGHBORS = 14;
        constexpr uint32_t TERMS = 17;
        constexpr uint32_t TERM_WORDS = 3;
        constexpr uint32_t NO_NEIGHBOR = 0xFFFFFFFFu;

        // u0 節・u1 隣・u2 子の一覧・u3 作業場・u4 作業用の 128bit(部品のもの)/ b0 / デバッグのリング / t0 段 0 の写し
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
            .uavCount = 5, .rootConstantCount = ROOT_CONSTANT_COUNT, .debugRing = true, .srvCount = 1};

        // b0(equilibrium_upload.hlsl の EquilibriumUploadConstants と同じ順)
        struct Constants {
            uint32_t maxNodes = 0;
            uint32_t maxLinks = 0;
            uint32_t nodeCount = 0;
            uint32_t termStart = 0;
            uint32_t rounding = 0;
        };

        struct Row {
            std::array<uint32_t, FACES_MAX> neighbors{};
            std::array<FxU128, FACES_MAX> coefficients{};
            uint32_t used = 0;
        };

        struct Term {
            int64_t value = 0;
            FxU128 factor{};
        };

        // 隣を 1 つ足す(6 面を超える・同じ隣が 2 回なら false = 部品の段 0 の形に入らない)
        bool AddNeighbor(Row& row, uint32_t neighbor, FxU128 coefficient) {
            if (row.used == FACES_MAX || std::find(row.neighbors.begin(), row.neighbors.begin() + row.used, neighbor) !=
                                             row.neighbors.begin() + row.used)
                return false;

            row.neighbors[row.used] = neighbor;
            row.coefficients[row.used] = coefficient;
            ++row.used;

            return true;
        }

        void PutWide(std::vector<uint64_t>& words, size_t at, FxU128 value) {
            words[at] = value.hi;
            words[at + 1] = value.lo;
        }

    }  // namespace

    std::vector<uint64_t> GpuEquilibriumUpload::StageWords(const EquilibriumSystem& system, uint32_t& terms) {
        const size_t count = system.nodes.size();

        // --- 自分だけの係数(C_i + Σ 固定した面。CPU の MakeCellLevel の own)と行(同じレベルの面だけ)---
        std::vector<FxU128> own(count);
        for (size_t i = 0; i < count; ++i)
            own[i] = ImWide(system.nodes[i].capacity);

        for (const EquilibriumFixedFace& fixed : system.fixedFaces)
            own[fixed.node] = ImWideAdd(own[fixed.node], fixed.coefficient);

        std::vector<Row> rows(count);
        for (const EquilibriumFace& face : system.faces) {
            if (face.gap != 0 || !AddNeighbor(rows[face.fine], face.coarse, face.coefficient) ||
                !AddNeighbor(rows[face.coarse], face.fine, face.coefficient))
                return {};
        }

        // --- 右辺の項(右辺の項と固定した面の値の項。CPU の SourceRhs・MakeLocal の FixedTerm)---
        std::vector<std::vector<Term>> nodeTerms(count);
        for (const EquilibriumSource& source : system.sources)
            nodeTerms[source.node].push_back({.value = source.value, .factor = source.factor});

        for (const EquilibriumFixedFace& fixed : system.fixedFaces)
            nodeTerms[fixed.node].push_back({.value = fixed.value, .factor = fixed.coefficient});

        // --- 語へ ---
        terms = 0;
        for (const std::vector<Term>& list : nodeTerms)
            terms += static_cast<uint32_t>(list.size());

        std::vector<uint64_t> words((count * NODE_WORDS) + (size_t{terms} * TERM_WORDS), 0);
        uint32_t next = 0;
        for (size_t i = 0; i < count; ++i) {
            const size_t base = i * NODE_WORDS;
            PutWide(words, base, own[i]);
            for (uint32_t f = 0; f < FACES_MAX; ++f) {
                const bool used = f < rows[i].used;
                PutWide(words, base + COEFFICIENTS + (2 * f), used ? rows[i].coefficients[f] : ImWide(0));
                const uint64_t neighbor = used ? rows[i].neighbors[f] : NO_NEIGHBOR;
                words[base + NEIGHBORS + (f / 2)] |= (f & 1) == 0 ? neighbor : neighbor << 32;
            }

            const auto termCount = static_cast<uint32_t>(nodeTerms[i].size());
            words[base + TERMS] = uint64_t{next} | (uint64_t{termCount} << 32);
            for (const Term& term : nodeTerms[i]) {
                const size_t at = (count * NODE_WORDS) + (size_t{next} * TERM_WORDS);
                words[at] = static_cast<uint64_t>(term.value);
                PutWide(words, at + 1, term.factor);
                ++next;
            }
        }

        return words;
    }

    std::expected<GpuEquilibriumUpload, std::string> GpuEquilibriumUpload::Create(ID3D12Device5* device,
                                                                                  const EquilibriumSystem& system,
                                                                                  const ImplicitGrid& grid) {
        if (grid.cells.size() != system.nodes.size())
            return std::unexpected("段と系の節の数が違う(閉じた領域の系は扱わない)");

        uint32_t terms = 0;
        const std::vector<uint64_t> words = StageWords(system, terms);
        if (words.empty())
            return std::unexpected("段 0 の形に入らない系(違うレベルの面・6 面を超える節)");

        // V は 1 回ずつ積む(cycles 回は RecordSolve を cycles 回)
        auto equilibrium = GpuEquilibrium::Create(device, grid,
                                                  {.solver = {.method = ImplicitMethod::Multigrid, .cycles = 1}});
        if (!equilibrium)
            return std::unexpected(equilibrium.error());

        GpuEquilibriumUpload result(std::move(*equilibrium));
        result.m_cells = static_cast<uint32_t>(system.nodes.size());
        result.m_terms = terms;
        result.m_rounding = system.rounding;

        // --- パイプライン・写し・読み戻し・時刻印 ---
        result.m_rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        const auto bytecode = gpu::LoadShader("sim/equilibrium_upload_node.cso");
        if (!result.m_rootSignature || !bytecode)
            return std::unexpected(bytecode ? std::string("ルート署名を作れない") : bytecode.error());

        result.m_pipeline = gpu::CreateComputePipeline(device, result.m_rootSignature.Get(), *bytecode);
        result.m_stage = gpu::CreateResidentBuffer(device, std::as_bytes(std::span(words)));
        result.m_valuesReadback = gpu::CreateBuffer(device, uint64_t{result.m_cells} * sizeof(int64_t),
                                                    gpu::BufferKind::Readback);
        result.m_timestampReadback = gpu::CreateBuffer(device, uint64_t{MAX_TIMESTAMPS} * sizeof(uint64_t),
                                                       gpu::BufferKind::Readback);
        const D3D12_QUERY_HEAP_DESC queryDesc{
            .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = MAX_TIMESTAMPS, .NodeMask = 0};
        if (!result.m_pipeline || !result.m_stage.Valid() || !result.m_valuesReadback || !result.m_timestampReadback ||
            FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&result.m_timestamps))))
            return std::unexpected("段 0 の写しのバッファかパイプラインを作れない");

        return result;
    }

    bool GpuEquilibriumUpload::RecordUpload(ID3D12GraphicsCommandList* list, const ImplicitGrid& grid) {
        if (!m_equilibrium.RecordUpload(list, grid, true))
            return false;

        const std::array<gpu::ResidentBuffer, 1> stage = {m_stage};
        gpu::RecordResidentUploads(list, stage);

        return true;
    }

    void GpuEquilibriumUpload::RecordStage(ID3D12GraphicsCommandList* list, uint64_t debugRing) const {
        static_assert(sizeof(Constants) == ROOT_CONSTANT_COUNT * sizeof(uint32_t));
        const Constants constants{.maxNodes = m_equilibrium.MaxNodes(),
                                  .maxLinks = m_equilibrium.MaxLinks(),
                                  .nodeCount = m_cells,
                                  .termStart = m_cells * NODE_WORDS,
                                  .rounding = m_rounding};
        list->SetComputeRootSignature(m_rootSignature.Get());
        list->SetPipelineState(m_pipeline.Get());
        const std::array<ID3D12Resource*, 5> buffers = {m_equilibrium.NodesBuffer(), m_equilibrium.LinksBuffer(),
                                                        m_equilibrium.ListsBuffer(), m_equilibrium.WorkBuffer(),
                                                        m_equilibrium.ScratchBuffer()};
        for (uint32_t i = 0; i < buffers.size(); ++i)
            list->SetComputeRootUnorderedAccessView(i, buffers[i]->GetGPUVirtualAddress());

        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT, &constants, 0);
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing);
        list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(0), m_stage.buffer->GetGPUVirtualAddress());
        list->Dispatch((m_cells + THREADS_PER_GROUP - 1) / THREADS_PER_GROUP, 1, 1);

        const D3D12_RESOURCE_BARRIER barrier = gpu::UavBarrier(nullptr);
        list->ResourceBarrier(1, &barrier);
    }

    bool GpuEquilibriumUpload::RecordSolve(ID3D12GraphicsCommandList* list, uint64_t debugRing, uint32_t cycles,
                                           bool reweight) {
        if (reweight) {
            RecordStage(list, debugRing);
            m_equilibrium.RecordLevels(list, debugRing);
        }

        for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
            if (!m_equilibrium.RecordSolve(list, debugRing))
                return false;
        }

        return true;
    }

    void GpuEquilibriumUpload::RecordValuesReadback(ID3D12GraphicsCommandList* list) {
        ID3D12Resource* work = m_equilibrium.WorkBuffer();
        D3D12_RESOURCE_BARRIER barrier = gpu::Transition(work, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                         D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        list->CopyBufferRegion(m_valuesReadback.Get(), 0, work, 0, uint64_t{m_cells} * sizeof(int64_t));
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
    }

    bool GpuEquilibriumUpload::ReadValues(std::vector<int64_t>& values) const {
        values.resize(m_cells);
        return gpu::ReadBuffer(m_valuesReadback.Get(), std::as_writable_bytes(std::span(values)));
    }

    void GpuEquilibriumUpload::RecordTimestamp(ID3D12GraphicsCommandList* list, uint32_t index) const {
        if (index < MAX_TIMESTAMPS)
            list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
    }

    void GpuEquilibriumUpload::RecordTimestampReadback(ID3D12GraphicsCommandList* list, uint32_t count) const {
        list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, std::min(count, MAX_TIMESTAMPS),
                               m_timestampReadback.Get(), 0);
    }

    std::vector<uint64_t> GpuEquilibriumUpload::ReadTimestamps(uint32_t count) const {
        std::vector<uint64_t> ticks(std::min(count, MAX_TIMESTAMPS));
        if (!gpu::ReadBuffer(m_timestampReadback.Get(), std::as_writable_bytes(std::span(ticks))))
            return {};

        return ticks;
    }

    uint64_t GpuEquilibriumUpload::DeviceBytes() const {
        return m_equilibrium.DeviceBytes() + m_stage.buffer->GetDesc().Width;
    }

}  // namespace bicameral::sim
