// follow_matter.hlsli — 欄が物質に付く(当てる段の 4。T-0718・ADR-0286・ADR-0350 Proposed。設計 20 §2.4 ④)の式の 1 か所。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。気体のセル(CPU = gas_cells.cpp・GPU = probe_gas_tick.hlsl)・液体の面
// (CPU = probe_liquid.cpp・multires_liquid.cpp・GPU = probe_liquid_tick.hlsl)が同じ関数を呼ぶ(前は気体 GfMomentumAfterMassChange・
// 液体 LqFollowMassChange の 2 つの同じ式)。
//
// 決まり(ADR-0286): 流れの欄(運動量)は物質に付く。段の外(反応・相変化・置く操作)で、欄の持ち主(気体のセル・液体の面の両側)の物質の
// 質量が前の段の終わりの before から now に変わったら:
//   - 減った: 減った物質は同じ速さのまま相を移った = 速さを保つ(運動量 × now ÷ before。0 の向きへ切り捨て)。減った分の運動量は帳簿
//     (世界の帳簿の理由「段の外の質量の変化」WL_REASON_FOLLOW_MASS)。
//   - 増えた: 増えた物質は静止して現れた = 運動量を保つ。
// 運動のエネルギーはどちらも前の値を超えない(種類が今の質量から計算し直し、差を熱へ。気体 = 前の値を熱へ戻して計算し直す・液体 = 刻みの終わりに
// 計算し直す)。位置のエネルギーは種類が今の質量から計算し直し、差を帳簿の同じ理由へ(液体の potentialEntered)。
// 浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_FOLLOW_MATTER_HLSLI
#define BICAMERAL_FOLLOW_MATTER_HLSLI

#include "common/fixed.hlsli"

#ifdef __cplusplus
namespace bicameral::follow {
    using namespace ::bicameral::fx;
#endif

    // 欄の持ち主の質量が before から now に変わった後の運動量(1 つの値。気体は軸ごとに呼ぶ)。差(momentum − 戻り値)が帳簿へ出る運動量
    FX_FN int64_t FmMomentumAfterMassChange(int64_t momentum, uint64_t before, uint64_t now) {
        if (momentum == 0 || now >= before)
            return momentum;

        return FxApplySign(FxDivU128By64(FxMulU64Full(FxAbsU64(momentum), now), before).quotient, momentum < 0);
    }

#ifdef __cplusplus
}  // namespace bicameral::follow
#endif

#endif  // BICAMERAL_FOLLOW_MATTER_HLSLI
