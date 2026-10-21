// gpu_equilibrium.cpp — 釣り合う家族の共通部品の GPU 版(gpu_equilibrium.h。T-0915)。段の中身は shaders/sim/equilibrium.hlsl。
// CPU リファレンスの順(equilibrium.cpp の SolveEquilibrium の閉じない道): 対角・右辺(法則の段 0)→ BuildImplicitGrid の粗い段
//   (EquilibriumLevel)→ CycleImplicitGrid を solver.cycles 回(GpuImplicit::RecordSolve の fixedCycles)→ MeterResiduals(EquilibriumMeter)。
#include "sim/gpu_equilibrium.h"

#include <algorithm>
#include <format>

#include "common/equilibrium.hlsli"
#include "core/aliases.h"
#include "gpu/resources.h"

namespace bicameral::sim {

    namespace {

        constexpr uint32_t THREADS_PER_GROUP = 64;  // equilibrium.hlsl の numthreads
        constexpr uint32_t ROOT_CONSTANT_COUNT = 5;
        constexpr uint32_t MAX_LEVELS = 64;     // 段の数の上限(CPU の MAX_GRID_LEVELS)
        constexpr uint32_t LINKS_PER_NODE = 6;  // 段 0 の面の数・粗い段の行の上限(EQ_GPU_FACES_MAX・ROW_MAX)

        // u0 節・u1 隣・u2 子の一覧・u3 作業場・u4 作業用の 128bit / b0 / デバッグのリング
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
            .uavCount = 5, .rootConstantCount = ROOT_CONSTANT_COUNT, .debugRing = true};

        constexpr std::array<const char*, 2> SHADER_NAMES = {"sim/equilibrium_level.cso", "sim/equilibrium_meter.cso"};

        uint32_t GroupsFor(uint64_t threadCount) {
            return (uint32_t)((threadCount + THREADS_PER_GROUP - 1) / THREADS_PER_GROUP);
        }

        std::vector<uint32_t> LevelOffsets(const ImplicitGrid& grid) {
            std::vector<uint32_t> offsets(1, 0);
            for (const ImplicitGridLevel& level : grid.levels)
                offsets.push_back(offsets.back() + (uint32_t)level.levels.size());

            return offsets;
        }

        using CellKey = std::array<int64_t, 3>;

        // 座標を 1 段縮めて(算術シフト = CPU の Coarsen の >> 1)同じものをまとめる
        std::vector<CellKey> CoarserKeys(const std::vector<CellKey>& keys) {
            std::vector<CellKey> coarser(keys.size());
            for (size_t i = 0; i < keys.size(); ++i)
                coarser[i] = {keys[i][0] >> 1, keys[i][1] >> 1, keys[i][2] >> 1};

            rng::sort(coarser);
            coarser.erase(std::unique(coarser.begin(), coarser.end()), coarser.end());

            return coarser;
        }

    }  // namespace

    // どの段も節が同じレベル(縮約はいつも親のセル)で、粗い段の行は ROW_MAX まで(equilibrium.hlsl の EquilibriumLevel の前提)。
    // 段 0 の行は面の数(EQ_GPU_FACES_MAX)まで(equilibrium_core.hlsli の EqGpuFinishNode)
    bool GpuEquilibrium::Supports(const ImplicitGrid& grid) {
        if (grid.levels.empty() || grid.cells.empty())
            return false;

        for (size_t depth = 0; depth < grid.levels.size(); ++depth) {
            const ImplicitGridLevel& level = grid.levels[depth];
            if (!level.levels.empty() &&
                !std::ranges::all_of(level.levels, [&](int32_t value) { return value == level.levels.front(); }))
                return false;

            for (size_t i = 0; i + 1 < level.rowStarts.size(); ++i) {
                if (level.rowStarts[i + 1] - level.rowStarts[i] > ROW_MAX)
                    return false;
            }
        }

        return true;
    }

    GpuImplicitLimits GpuEquilibrium::ShapeLimits(const ImplicitGrid& grid) {
        const ImplicitGridLevel& cells = grid.levels.front();
        std::vector<CellKey> keys(cells.x.size());
        for (size_t i = 0; i < keys.size(); ++i)
            keys[i] = {cells.x[i], cells.y[i], cells.z[i]};

        uint64_t nodes = keys.size();
        for (uint32_t depth = 1; depth < MAX_LEVELS && keys.size() > 1; ++depth) {
            keys = CoarserKeys(keys);
            nodes += keys.size();
        }

        return {.cells = (uint32_t)grid.cells.size(),
                .faces = 0,
                .nodes = (uint32_t)nodes,
                .links = (uint32_t)(nodes * LINKS_PER_NODE),
                .levels = MAX_LEVELS,
                .solveOnly = true};
    }

    std::expected<GpuEquilibrium, std::string> GpuEquilibrium::Create(ID3D12Device5* device, const ImplicitGrid& grid,
                                                                      const GpuEquilibriumOptions& options) {
        if (!Supports(grid))
            return std::unexpected("部品の GPU 版で解けない段の形(違うレベルの節・長い行。T-0939)");

        const GpuImplicitLimits limits = ShapeLimits(grid);
        auto implicit = GpuImplicit::Create(device, limits);
        if (!implicit)
            return std::unexpected(implicit.error());

        GpuEquilibrium result(std::move(*implicit));
        result.m_options = options;
        result.m_options.shape.fixedCycles = true;
        result.m_cells = (uint32_t)grid.cells.size();
        result.m_levelOffsets = LevelOffsets(grid);

        // --- パイプライン ---
        result.m_rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!result.m_rootSignature)
            return std::unexpected("部品のルート署名を作れない");

        for (size_t pass = 0; pass < SHADER_NAMES.size(); ++pass) {
            const auto bytecode = gpu::LoadShader(SHADER_NAMES[pass]);
            if (!bytecode)
                return std::unexpected(bytecode.error());

            result.m_pipelines[pass] = gpu::CreateComputePipeline(device, result.m_rootSignature.Get(), *bytecode);
            if (!result.m_pipelines[pass])
                return std::unexpected(std::format("パイプラインを作れない: {}", SHADER_NAMES[pass]));
        }

        // --- 作業用の 128bit の語(容量・対角・面の係数)と残差の計器 ---
        const uint32_t meteredNodes = options.meterResiduals ? result.m_cells : 0;
        const uint64_t words = multires::EqGpuScratchWords(result.MaxNodes(), result.MaxLinks(), meteredNodes);
        result.m_scratch = gpu::CreateBuffer(device, words * sizeof(uint64_t), gpu::BufferKind::UnorderedAccess);
        result.m_readback = gpu::CreateBuffer(device, sizeof(uint64_t), gpu::BufferKind::Readback);
        if (!result.m_scratch || !result.m_readback)
            return std::unexpected("部品のバッファを作れない");

        return result;
    }

    bool GpuEquilibrium::RecordUpload(ID3D12GraphicsCommandList* list, const ImplicitGrid& grid, bool zeroValues) {
        if (grid.cells.size() != m_cells || !Supports(grid) || !m_implicit.RecordUpload(list, grid))
            return false;

        m_levelOffsets = LevelOffsets(grid);
        if (!zeroValues)
            return true;

        // 前の答え(値 A・B)を 0 に。右辺は法則の段が書き、閾値は fixedCycles なので読まない(solveOnly は閾値の欄を持たない)
        return m_implicit.RecordZeroValues(list, m_cells);
    }

    void GpuEquilibrium::RecordPass(ID3D12GraphicsCommandList* list, uint64_t debugRing, Pass pass,
                                    const Constants& constants, uint32_t threads) const {
        static_assert(sizeof(Constants) == ROOT_CONSTANT_COUNT * sizeof(uint32_t));
        list->SetComputeRootSignature(m_rootSignature.Get());
        list->SetPipelineState(m_pipelines[(size_t)pass].Get());
        const std::array<ID3D12Resource*, 5> buffers = {NodesBuffer(), LinksBuffer(), ListsBuffer(), WorkBuffer(),
                                                        m_scratch.Get()};
        for (uint32_t i = 0; i < buffers.size(); ++i)
            list->SetComputeRootUnorderedAccessView(i, buffers[i]->GetGPUVirtualAddress());

        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT, &constants, 0);
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing);
        list->Dispatch(GroupsFor(threads), 1, 1);

        const D3D12_RESOURCE_BARRIER barrier = gpu::UavBarrier(nullptr);
        list->ResourceBarrier(1, &barrier);
    }

    void GpuEquilibrium::RecordLevels(ID3D12GraphicsCommandList* list, uint64_t debugRing) const {
        Constants constants{.maxNodes = MaxNodes(), .maxLinks = MaxLinks(), .cellCount = m_cells};
        for (size_t depth = 1; depth + 1 < m_levelOffsets.size(); ++depth) {
            constants.levelStart = m_levelOffsets[depth];
            constants.levelCount = m_levelOffsets[depth + 1] - m_levelOffsets[depth];
            RecordPass(list, debugRing, Pass::Level, constants, constants.levelCount);
        }
    }

    bool GpuEquilibrium::RecordSolve(ID3D12GraphicsCommandList* list, uint64_t debugRing) {
        if (!m_implicit.RecordSolve(list, debugRing, m_options.solver, m_options.shape))
            return false;

        const D3D12_RESOURCE_BARRIER barrier = gpu::UavBarrier(nullptr);
        list->ResourceBarrier(1, &barrier);
        if (m_options.meterResiduals)
            RecordMeter(list, debugRing);

        return true;
    }

    void GpuEquilibrium::RecordMeter(ID3D12GraphicsCommandList* list, uint64_t debugRing) const {
        if (!m_options.meterResiduals)
            return;  // 節ごとの計器の語は meterResiduals の時だけ持つ

        const Constants constants{.maxNodes = MaxNodes(), .maxLinks = MaxLinks(), .cellCount = m_cells};
        RecordPass(list, debugRing, Pass::Meter, constants, m_cells);
    }

    void GpuEquilibrium::RecordResidualReadback(ID3D12GraphicsCommandList* list) {
        D3D12_RESOURCE_BARRIER barrier = gpu::Transition(m_scratch.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                         D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        const uint64_t word = multires::EqGpuWorstResidualWord(MaxNodes(), MaxLinks());
        list->CopyBufferRegion(m_readback.Get(), 0, m_scratch.Get(), word * sizeof(uint64_t), sizeof(uint64_t));
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
    }

    bool GpuEquilibrium::ReadWorstResidual(uint64_t& worst) const {
        return gpu::ReadBuffer(m_readback.Get(), std::as_writable_bytes(std::span(&worst, 1)));
    }

    uint64_t GpuEquilibrium::DeviceBytes() const {
        return m_implicit.DeviceBytes() + m_scratch->GetDesc().Width;
    }

}  // namespace bicameral::sim
