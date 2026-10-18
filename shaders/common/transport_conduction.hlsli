// transport_conduction.hlsli — 流れの種類「熱の伝導(陽解法)」(土台 §2.3 の表の 1 行目。T-0714・ADR-0334 Proposed)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。当てる段(transport_apply.hlsli)に渡す 3 つ(坂・面の式・安定の条件)だけを書く。
//
//   坂: セル全体の温度(見立ての thermal.temperature)
//   面の式: F = min(両側の係数) × ΔT(heat_conduction.hlsli の HcFaceFlow。07 §1)
//   安定の条件: 係数は min(G × 4^(k − σ), C/8)(MrSubstepFaceLimit。粗い側は × 2^d ÷ 小刻みの回数 = MrSubstepCoarseLimit)・
//     小刻み 4^σ(MrSubcycleShift。T-0108)。C/8 で行き過ぎないので当てる段の上限は使わない
// 式そのものは multires_conduction.hlsli の関数 1 つずつ(GPU の伝導の段 multires_conduct.hlsli も同じ関数を呼ぶ。二重書きしない)。
// 欄は持たない(FieldCount = 0)。浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_TRANSPORT_CONDUCTION_HLSLI
#define BICAMERAL_TRANSPORT_CONDUCTION_HLSLI

#include "transport_apply.hlsli"

MR_NAMESPACE_BEGIN

struct TxConduction {
    // --- 安定の条件の値(MultiresStepOptions の subcycleBaseLevel・maxSubcycleGap)---
    int32_t subcycleBaseLevel;
    uint32_t maxSubcycleGap;

    // --- 坂 ---
    FX_FN int64_t Slope(FX_IN(CvCellView) view) { return (int64_t)view.thermal.temperature; }

    // --- 面の式 ---
    FX_FN uint64_t SideCoefficient(FX_IN(CvCellView) view, int32_t level, uint32_t shift) {
        return MrSubstepFaceLimit(view.thermal, level, shift);
    }

    FX_FN uint64_t CoarseSideCoefficient(FX_IN(CvCellView) coarse, int32_t fineLevel, uint32_t gap, uint32_t fineShift,
                                         uint32_t coarseShift) {
        return MrSubstepCoarseLimit(coarse.thermal, fineLevel, gap, fineShift, coarseShift);
    }

    FX_FN int64_t FaceFlow(int64_t slopeA, uint64_t coefficientA, int64_t slopeB, uint64_t coefficientB) {
        return HcFaceFlow(MrFlowSide((uint32_t)slopeA, coefficientA), MrFlowSide((uint32_t)slopeB, coefficientB));
    }

    // --- 安定の条件 ---
    FX_FN uint32_t SubcycleShift(int32_t level) { return MrSubcycleShift(level, subcycleBaseLevel, maxSubcycleGap); }
    FX_FN bool LimitsOutflow() { return false; }
    FX_FN uint32_t FieldCount() { return 0; }
    // 読む見立ての部分: セル全体の熱と伝導の係数だけ(相ごとの和・圧力・液体は作らない。T-0767)
    FX_FN uint32_t ViewParts() { return CV_VIEW_THERMAL; }
};

FX_FN TxConduction TxMakeConduction(int32_t subcycleBaseLevel, uint32_t maxSubcycleGap) {
    TxConduction kind;
    kind.subcycleBaseLevel = subcycleBaseLevel;
    kind.maxSubcycleGap = maxSubcycleGap;

    return kind;
}

MR_NAMESPACE_END

#endif  // BICAMERAL_TRANSPORT_CONDUCTION_HLSLI
