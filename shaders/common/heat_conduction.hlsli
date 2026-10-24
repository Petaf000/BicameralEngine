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
// コンダクタンス(mW/K)= セルの伝導率(mW/(m·K))× 面積 ÷ 距離(0.25 m² ÷ 0.5 m = 0.5 m)。
// セルの伝導率は体積で重み付けた平行の道(D-637・T-1066。物の箱 tests/object_heat_test.cpp と同じ形):
//   k = (Σ 凝縮相の体積 × 伝導率 + 残りの体積 × 残りの伝導率) ÷ セルの体積
//   残り(気体と、密度を書いていない物質 = 場所を取らない物質。cell_volume.h)は物質量の平均。気体の伝導率は ∝ √(T ÷ 室温)(D-639)。
// 伝導率は物質の室温の値(試験の倍率は外した。T-1066)。
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

// --- 温度で変わる気体の値(D-639。物質の値は室温の値として書く)---

// 室温(mK)。物質の伝導率・混ざる速さはこの温度の値
FX_CONST uint32_t HC_ROOM_TEMPERATURE_MK = 298150;

// √(T ÷ 室温)(Q16)= √(T × round(2^48 ÷ 298150)) >> 8。T < 2^32 mK なので積 < 2^62
FX_CONST uint64_t HC_ROOT_MULTIPLIER = 944071698;

// T ÷ 室温(Q16)= (T × round(2^40 ÷ 298150)) >> 24
FX_CONST uint64_t HC_RATIO_MULTIPLIER = 3687780;

// 気体の値の温度の倍率
struct HcGasScale {
    uint32_t conductionQ16;  // 伝導率 ∝ T^0.5
    uint32_t diffusionQ16;   // 混ざる速さ ∝ T^1.5
};

FX_FN HcGasScale HcGasScaleAt(uint32_t temperatureMk) {
    const uint64_t rootQ16 = (uint64_t)FxSqrtU64((uint64_t)temperatureMk * HC_ROOT_MULTIPLIER) >> 8;
    const uint64_t ratioQ16 = ((uint64_t)temperatureMk * HC_RATIO_MULTIPLIER) >> 24;
    const uint64_t diffusionQ16 = (ratioQ16 * rootQ16) >> 16;

    HcGasScale scale;
    scale.conductionQ16 = (uint32_t)rootQ16;
    scale.diffusionQ16 = diffusionQ16 < HC_UINT32_MAX ? (uint32_t)diffusionQ16 : (uint32_t)HC_UINT32_MAX;

    return scale;
}

// --- セルの伝導率(体積で重み付けた平行の道。D-637)---

// 1 セル(レベル 0 の 0.5 m 角)の体積(pL = µmol × mm³/mol)= 1.25 × 10^8 × 10^6 pL。量の数は 濃さ × 0.125 m³ なので、どのレベルでもこの体積で割合を出す。
// 32bit を超える定数をシミュの式に書かない(32bit の定数の積と割り算に分ける。GPU で上の語が壊れた実例: T-1078・ADR-0451)
FX_CONST uint64_t HC_MEGA = 1000000;
FX_CONST uint64_t HC_CELL_VOLUME_MEGA_PL = 125000000;

// 伝導率の和(1 成分ずつ足す。GPU の溢れを持つセルも同じ順で足す。T-0272)
struct HcConductivitySums {
    FxU128 condensedWeighted;  // Σ 凝縮相の体積(pL)× 伝導率
    uint64_t condensedVolume;  // Σ 凝縮相の体積(pL)
    FxU128 gasWeighted;        // Σ 気体の物質量 × 伝導率(室温の値)
    uint64_t gasAmount;
    FxU128 looseWeighted;  // Σ 場所を取らない凝縮相(密度を書いていない)の物質量 × 伝導率
    uint64_t looseAmount;
};

FX_FN FxU128 HcAddU128(FxU128 a, FxU128 b) {
    FxU128 sum;
    sum.lo = a.lo + b.lo;
    sum.hi = a.hi + b.hi + (sum.lo < a.lo ? (uint64_t)1 : (uint64_t)0);

    return sum;
}

FX_FN FxU128 HcSubU128(FxU128 a, FxU128 b) {
    FxU128 difference;
    difference.lo = a.lo - b.lo;
    difference.hi = a.hi - b.hi - (a.lo < b.lo ? (uint64_t)1 : (uint64_t)0);

    return difference;
}

FX_FN HcConductivitySums HcMakeConductivitySums() {
    HcConductivitySums sums;
    sums.condensedWeighted.hi = 0;
    sums.condensedWeighted.lo = 0;
    sums.condensedVolume = 0;
    sums.gasWeighted = sums.condensedWeighted;
    sums.gasAmount = 0;
    sums.looseWeighted = sums.condensedWeighted;
    sums.looseAmount = 0;

    return sums;
}

FX_FN HcConductivitySums HcAddConductivity(HcConductivitySums sums, RxSpecies entry, uint64_t amount) {
    if ((entry.flags & RX_SPECIES_GAS) != 0) {
        sums.gasWeighted = HcAddU128(sums.gasWeighted, FxMulU64Full(amount, (uint64_t)entry.conductivity));
        sums.gasAmount += amount;
    } else if (entry.molarVolume == 0) {
        sums.looseWeighted = HcAddU128(sums.looseWeighted, FxMulU64Full(amount, (uint64_t)entry.conductivity));
        sums.looseAmount += amount;
    } else {
        // 体積 = 量 × モル体積(µmol × mm³/mol = pL)。セル 1 つ分の固体で約 10^14 なので 64bit に収まる
        const FxU128 volume = FxMulU64Full(amount, (uint64_t)entry.molarVolume);
        FX_ASSERT(volume.hi == 0);
        sums.condensedWeighted = HcAddU128(sums.condensedWeighted,
                                           FxMulU64Full(volume.lo, (uint64_t)entry.conductivity));
        sums.condensedVolume += volume.lo;
    }

    return sums;
}

// a × q(q < 2^32。a.hi × q は 64bit に収まる)
FX_FN FxU128 HcMulU128ByU64(FxU128 a, uint64_t q) {
    FxU128 product = FxMulU64Full(a.lo, q);
    product.hi += a.hi * q;

    return product;
}

// (a × q) >> 16(q は Q16)。a.hi × q は 64bit に収まる(伝導率の和の上位語は小さい)
FX_FN FxU128 HcMulU128ByQ16(FxU128 a, uint32_t q) {
    FxU128 product = FxMulU64Full(a.lo, (uint64_t)q);
    product.hi += a.hi * (uint64_t)q;

    FxU128 result;
    result.lo = (product.lo >> 16) | (product.hi << 48);
    result.hi = product.hi >> 16;

    return result;
}

// 和と温度から伝導率(mW/(m·K))。セルの伝導と物の熱(object_heat.hlsli)が使う一つの式(D-637・D-639)
FX_FN uint64_t HcConductivityFromSums(HcConductivitySums sums, uint32_t temperatureMk) {
    // --- 残り(気体 + 場所を取らない物質)の伝導率: 物質量の平均。気体の分は ∝ √(T ÷ 室温)---
    const uint64_t poolAmount = sums.gasAmount + sums.looseAmount;
    uint64_t poolConductivity = 0;
    if (poolAmount != 0) {
        const HcGasScale scale = HcGasScaleAt(temperatureMk);
        const FxU128 poolWeighted = HcAddU128(HcMulU128ByQ16(sums.gasWeighted, scale.conductionQ16),
                                              sums.looseWeighted);
        poolConductivity = FxDivU128By64(poolWeighted, poolAmount).quotient;
    }

    // --- 凝縮相の体積と残りの体積で重み付ける。詰め過ぎ(体積の和 ≥ セル)は凝縮相だけをその和で割る ---
    // セルの体積は 32bit の定数の積(HC_CELL_VOLUME_MEGA_PL × HC_MEGA)で、割り算は 2 回に分ける
    // (floor(floor(x ÷ a) ÷ b) = floor(x ÷ ab))。64bit の「0 で止める引き算」も書かない(T-0185)
    uint64_t conductivity = poolConductivity;
    if (sums.condensedVolume != 0) {
        FxU128 volume;
        volume.hi = 0;
        volume.lo = sums.condensedVolume;
        const bool full = FxDivU128By64(volume, HC_MEGA).quotient >= HC_CELL_VOLUME_MEGA_PL;
        if (full) {
            conductivity = FxDivU128By64(sums.condensedWeighted, sums.condensedVolume).quotient;
        } else {
            // 凝縮相の重み + 残りの伝導率 × (セル − 凝縮相の体積)。セル > 凝縮相の体積なので 128bit の差は負にならない
            const FxU128 poolTimesCell = HcMulU128ByU64(FxMulU64Full(poolConductivity, HC_CELL_VOLUME_MEGA_PL),
                                                        HC_MEGA);
            const FxU128 total = HcSubU128(HcAddU128(sums.condensedWeighted, poolTimesCell),
                                           FxMulU64Full(poolConductivity, sums.condensedVolume));
            FxU128 perMega;
            perMega.hi = 0;
            perMega.lo = FxDivU128By64(total, HC_MEGA).quotient;
            conductivity = FxDivU128By64(perMega, HC_CELL_VOLUME_MEGA_PL).quotient;
        }
    }

    return conductivity;
}

// 和と温度からコンダクタンス(mW/K。レベル 0 の大きさのセルとして)
FX_FN uint32_t HcConductanceFromSums(HcConductivitySums sums, uint32_t temperatureMk) {
    const uint64_t conductance = HcConductivityFromSums(sums, temperatureMk) >> HC_CONDUCTANCE_SHIFT;

    return conductance < HC_UINT32_MAX ? (uint32_t)conductance : (uint32_t)HC_UINT32_MAX;
}

// セル(または物のボクセル)の伝導率(mW/(m·K))。temperatureMk = 温度(負なら 0 にした値)
template <typename Table, typename Cell>
FX_FN uint64_t HcCellConductivity(Table table, Cell cell, uint32_t temperatureMk) {
    HcConductivitySums sums = HcMakeConductivitySums();
    for (uint32_t i = 0; i < cell.speciesCount; ++i)
        sums = HcAddConductivity(sums, table.Species(cell.species[i]), cell.amounts[i]);

    return HcConductivityFromSums(sums, temperatureMk);
}

// セルのコンダクタンス(mW/K)。temperatureMk = セルの温度(負なら 0 にした値)
// セルの形 Cell のテンプレート(RxCell・C++ の RxWideCell。T-0187)
template <typename Table, typename Cell>
FX_FN uint32_t HcCellConductance(Table table, Cell cell, uint32_t temperatureMk) {
    HcConductivitySums sums = HcMakeConductivitySums();
    for (uint32_t i = 0; i < cell.speciesCount; ++i)
        sums = HcAddConductivity(sums, table.Species(cell.species[i]), cell.amounts[i]);

    return HcConductanceFromSums(sums, temperatureMk);
}

// キャッシュの温度(mK。負なら 0)
FX_FN uint32_t HcCacheTemperature(RxThermal thermal) {
    return thermal.temperature > 0 ? (uint32_t)thermal.temperature : 0;
}

FX_FN uint64_t HcFaceLimit(uint32_t conductance, uint64_t heatCapacity) {
    const uint64_t byConductance = (uint64_t)conductance * HC_LIMIT_PER_CONDUCTANCE;
    const uint64_t byCapacity = (heatCapacity >> HC_CAPACITY_SHIFT) * HC_LIMIT_PER_CAPACITY;

    return byConductance < byCapacity ? byConductance : byCapacity;
}

FX_FN HcThermalCache HcMakeCache(RxThermal thermal, uint32_t conductance) {
    HcThermalCache cache;
    cache.temperature = HcCacheTemperature(thermal);
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
    // 最も近い整数に丸める(切り捨てだと現実の伝導率の粗いセルで流れが 1 mJ 単位で系統的に小さくなる: 空気のレベル 0 で実効の拡散率 −23 %。T-1066)。
    // 両側が同じ大きさを出すので保存は変わらない。上限 1/8 を超える分は高々 0.5 mJ
    FxU128 product = FxMulU64Full(difference, coefficient);
    const uint64_t rounded = product.lo + (uint64_t)0x80000000u;
    product.hi += rounded < product.lo ? (uint64_t)1 : (uint64_t)0;
    product.lo = rounded;
    const uint64_t magnitude = (product.hi << 32) | (product.lo >> 32);
    FX_ASSERT((product.hi >> 32) == 0 && magnitude < FX_U64(0x80000000u, 0u));

    return outward ? (int64_t)magnitude : -(int64_t)magnitude;
}

RX_NAMESPACE_END

#endif  // BICAMERAL_HEAT_CONDUCTION_HLSLI
