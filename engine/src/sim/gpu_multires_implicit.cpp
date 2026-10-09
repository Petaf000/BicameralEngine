// gpu_multires_implicit.cpp — GPU の伝導の段から呼ぶ陰解法(T-0132)。何をするかは gpu_multires_implicit.h。
// 段の間は各段の記録が入れる UAV のバリア(GpuMultires・GpuImplicit とも全体の UAV のバリア)。
#include "sim/gpu_multires_implicit.h"

#include <utility>

using namespace bicameral::multires;

namespace bicameral::sim {

    namespace {

        // 多重格子の段で、細かい段の節がこれ以下の回は 1 グループの LvTail で回す(T-0135 の計測: 熱い点 1.94 → 0.19 ms。docs/perf.md)
        constexpr uint32_t LEVEL_TAIL_MAX_NODES = 256;

        // 木の陰解法と同じ選択(multires_implicit_conduction.cpp の MakeImplicitOptions。ADR-0019): V(2,2)・新しい温度の誤差の見込み 1 mK で止める
        ImplicitOptions TreeImplicitOptions(const MultiresStepOptions& options) {
            ImplicitOptions implicit;
            implicit.method = ImplicitMethod::Multigrid;
            implicit.cycles = options.implicitMaxCycles;
            implicit.toleranceMillikelvin = 1;

            return implicit;
        }

    }  // namespace

    std::expected<GpuMultiresImplicit, std::string> GpuMultiresImplicit::Create(
        ID3D12Device5* device, const GpuMultires& multires, const MultiresCapacity& capacity,
        const GpuMultiresImplicitLimits& limits) {
        if (limits.unknowns == 0 || limits.cells == 0 || limits.nodes == 0 || limits.links == 0)
            return std::unexpected("陰解法の上限は全部の欄が要る(T-0178)");

        // --- 系を作る段のバッファは予算の大きさ(未知数 ≤ 隣 / 12・セル ≤ 節。段 0 がいつも入る)---
        const MultiresImplicitBudget budget = MakeMultiresImplicitBudget(limits);
        auto build = GpuImplicitBuild::Create(device, multires, capacity,
                                              {.unknowns = budget.unknowns, .cells = budget.cells});
        if (!build)
            return std::unexpected(build.error());

        // 多重格子の段: 小さい段の回(節 ≤ LEVEL_TAIL_MAX_NODES)と積んだ回の後の残りを LvTail で(T-0135 のおすすめ・T-0179。
        // WARP も同じ。T-0147)。作る段は全部 Dispatch で積む時と番号まで同じ
        GpuImplicitLevelLimits levelLimits;
        levelLimits.nodes = limits.nodes;
        levelLimits.links = limits.links;
        levelLimits.tailMaxNodes = LEVEL_TAIL_MAX_NODES;
        auto levels = GpuImplicitLevels::Create(device, *build, levelLimits);
        if (!levels)
            return std::unexpected(levels.error());

        const GpuImplicitLimits solveLimits{.cells = budget.cells,
                                            .faces = MR_FACES * budget.unknowns,
                                            .nodes = limits.nodes,
                                            .links = limits.links,
                                            .levels = levelLimits.levels};
        auto implicit = GpuImplicit::Create(device, solveLimits);
        if (!implicit)
            return std::unexpected(implicit.error());

        return GpuMultiresImplicit(std::move(*build), std::move(*levels), std::move(*implicit));
    }

    void GpuMultiresImplicit::RecordAdmit(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                          GpuMultires& multires, const MultiresStepOptions& options) {
        FX_ASSERT(options.implicitOverflow == ImplicitOverflow::Explicit);  // B・C は未実装(Q22)
        m_build.RecordAdmit(list, debugRing, multires, options);
    }

    void GpuMultiresImplicit::Record(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                     GpuMultires& multires, const MultiresStepOptions& options) {
        // --- 記録の形は前の刻みの数から(初めの刻みは上限まで積む)---
        GpuImplicitCost previous;
        const bool known = m_recorded && m_implicit.ReadCost(previous);
        // 多重格子の段を Dispatch で積む回も前の刻みの段の数から(T-0179)
        GpuImplicitRecordShape shape = known ? GpuImplicit::ShapeFrom(previous) : GpuImplicitRecordShape{};
        uint32_t levelRounds = known ? GpuImplicitLevels::RoundsFrom(previous) : GpuImplicitLevels::ALL_ROUNDS;
        if (m_forceCheapest) {
            shape.dispatchCycles = 1;
            levelRounds = 0;
        }

        // --- 系を作る(凍った印を見る)→ 多重格子の段 → 写す ---
        const auto stamp = [&](uint32_t phase) {
            if (m_stampPhases)
                multires.RecordTimestamp(list, PHASE_STAMP_FIRST + phase);
        };
        stamp(0);
        m_build.RecordBuild(list, debugRing, multires, options, true, true);
        stamp(1);
        m_levels.RecordBuild(list, debugRing, m_build, levelRounds);
        m_implicit.RecordReset(list);
        m_build.RecordCopyTo(list, m_implicit);
        m_levels.RecordCopyTo(list, m_implicit);

        // --- 1 刻み解いて、解いた変化を伝導の表へ ---
        stamp(2);
        [[maybe_unused]] const bool recorded = m_implicit.RecordStep(list, debugRing, TreeImplicitOptions(options),
                                                                     GpuImplicit::DEFAULT_MAX_LIMIT_ROUNDS, shape);
        FX_ASSERT(recorded);
        stamp(3);
        m_build.RecordApply(list, debugRing, multires, options, m_implicit.CellsBuffer());
        stamp(4);

        // --- 次の刻みの形のための数を読み戻し、バッファを COMMON に戻す(同じリストの次の刻みが RecordReset できるように)---
        m_implicit.RecordCostReadback(list);
        m_implicit.RecordRelease(list);
        m_recorded = true;
    }

}  // namespace bicameral::sim
