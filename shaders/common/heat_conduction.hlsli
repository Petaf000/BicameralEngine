// heat_conduction.hlsli — セルどうしの熱の伝導(docs/design/07-transport.md §1。T-0089)。温度の差で熱を受け渡す。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU リファレンス(sim/probe_sim.cpp)と GPU(shaders/sim/probe_conduct.hlsl)が同じ関数を呼ぶ。
//
// データの流れ:
//   刻みの終わりのセル(reaction.hlsli の RxCell)→ その熱(RxThermal)と伝導率 → 熱のキャッシュ(HcThermalCache: 温度と面の係数の上限)
//   → 次の刻みで、自分と 6 面の隣のキャッシュから面の流れ(HcFaceFlow)→ セルのエネルギーに足し引き
//
// 面の流れ(自分 → 隣、mJ/刻み)= min(両側のコンダクタンス) × ΔT × Δt。大きさは min(両側の熱容量) × ΔT の 1/8 まで。
//   - 両側が同じ式(min は対称、|ΔT| は同じ)で大きさを出し、向きだけ逆にするので、足し引きが打ち消して**エネルギーは構造的に保存**される(D-206)。
//   - 1/8 の上限: 6 面の重みの合計が 6/8 < 1 なので、新しい温度は自分と隣の温度の凸結合になり、行き過ぎない(陽解法の安定)。
//   - 係数は「2^-32 mJ / mK」の整数で持つ(割り算を面ごとに使わない。04 §6・ADR-0010)。
// コンダクタンス(mW/K)= セルの伝導率(物質量で重み付けた平均、mW/(m·K))× 面積 ÷ 距離(0.25 m² ÷ 0.5 m = 0.5 m)。
// 伝導率は物質の値。試験の表の値は、放射と対流(M3)が無い間も燃え広がりが見えるよう大きくしてある(reaction_test_table.cpp)。
#ifndef BICAMERAL_HEAT_CONDUCTION_HLSLI
#define BICAMERAL_HEAT_CONDUCTION_HLSLI

#include "reaction.hlsli"

RX_NAMESPACE_BEGIN

// --- 係数(1 刻み = 1/60 秒。04 §2 の単位)---

// コンダクタンス G(mW/K)→ 面の係数(2^-32 mJ/mK): G × Δt ÷ 1000(mK → K)= G ÷ 60000(mJ/mK)。floor(2^32 ÷ 60000)
FX_CONST uint64_t HC_LIMIT_PER_CONDUCTANCE = 71582;

// 熱容量 C(nJ/K)→ 上限の係数(2^-32 mJ/mK): C ÷ 8(1/8)÷ 1e9(nJ·mK → mJ)。
// (C >> 12) × floor(2^32 × 4096 ÷ 8e9) で近似する(小さめに切り捨てるので、上限がわずかに厳しくなるだけ)
FX_CONST uint32_t HC_CAPACITY_SHIFT = 12;
FX_CONST uint64_t HC_LIMIT_PER_CAPACITY = 2199;

// 伝導率(mW/(m·K))× 0.5 m = コンダクタンス(mW/K)
FX_CONST uint32_t HC_CONDUCTANCE_SHIFT = 1;

FX_CONST uint64_t HC_UINT32_MAX = 0xFFFFFFFFu;

// --- 熱のキャッシュ(セルごと。刻みの終わりに書き、次の刻みの伝導が自分と隣のものを読む)---
struct HcThermalCache {
    uint32_t temperature;  // mK(負なら 0)
    uint32_t conductance;  // mW/K(成分が変わった時だけ作り直す)
    uint64_t faceLimit;    // min(G の係数, C の上限の係数)。面の係数 = min(両側の faceLimit)
};

// セルのコンダクタンス(mW/K)。伝導率を物質量で重み付けた平均(割り算 1 回。成分が変わった刻みだけ)
// セルの形 Cell のテンプレート(RxCell・C++ の RxWideCell。T-0187)
template <typename Table, typename Cell>
FX_FN uint32_t HcCellConductance(Table table, Cell cell) {
    FxU128 weighted;
    weighted.hi = 0;
    weighted.lo = 0;
    uint64_t total = 0;
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        const FxU128 term = FxMulU64Full(cell.amounts[i], (uint64_t)table.Species(cell.species[i]).conductivity);
        const uint64_t low = weighted.lo + term.lo;
        weighted.hi += term.hi + (low < weighted.lo ? (uint64_t)1 : (uint64_t)0);
        weighted.lo = low;
        total += cell.amounts[i];
    }

    if (total == 0)
        return 0;

    const uint64_t conductivity = FxDivU128By64(weighted, total).quotient;
    const uint64_t conductance = conductivity >> HC_CONDUCTANCE_SHIFT;

    return conductance < HC_UINT32_MAX ? (uint32_t)conductance : (uint32_t)HC_UINT32_MAX;
}

FX_FN uint64_t HcFaceLimit(uint32_t conductance, uint64_t heatCapacity) {
    const uint64_t byConductance = (uint64_t)conductance * HC_LIMIT_PER_CONDUCTANCE;
    const uint64_t byCapacity = (heatCapacity >> HC_CAPACITY_SHIFT) * HC_LIMIT_PER_CAPACITY;

    return byConductance < byCapacity ? byConductance : byCapacity;
}

FX_FN HcThermalCache HcMakeCache(RxThermal thermal, uint32_t conductance) {
    HcThermalCache cache;
    cache.temperature = thermal.temperature > 0 ? (uint32_t)thermal.temperature : 0;
    cache.conductance = conductance;
    cache.faceLimit = HcFaceLimit(conductance, thermal.heatCapacity);

    return cache;
}

// 面の流れ(自分 → 隣が正、mJ)。格子の外は呼ばない(断熱 = 0)
FX_FN int64_t HcFaceFlow(HcThermalCache self, HcThermalCache neighbor) {
    const bool outward = self.temperature >= neighbor.temperature;
    const uint64_t difference = outward ? (uint64_t)(self.temperature - neighbor.temperature)
                                        : (uint64_t)(neighbor.temperature - self.temperature);
    const uint64_t coefficient = self.faceLimit < neighbor.faceLimit ? self.faceLimit : neighbor.faceLimit;

    // |ΔT| < 2^32、係数 < 2^64 なので積 < 2^96、流れ = 積 >> 32 < 2^64。符号付きに収まらない値は熱の上限の外(assert)
    const FxU128 product = FxMulU64Full(difference, coefficient);
    const uint64_t magnitude = (product.hi << 32) | (product.lo >> 32);
    FX_ASSERT((product.hi >> 32) == 0 && magnitude < FX_U64(0x80000000u, 0u));

    return outward ? (int64_t)magnitude : -(int64_t)magnitude;
}

RX_NAMESPACE_END

#endif  // BICAMERAL_HEAT_CONDUCTION_HLSLI
