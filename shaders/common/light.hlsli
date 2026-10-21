// light.hlsli — 物質の光の値(2 つの帯の吸い込み・照り返し)と、セル 1 つの帯ごとの吸い込みの和・照り返しの割合
// (docs/design/23-radiation.md §2・ADR-0382・ADR-0395。T-0910)。HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。
//
// データの流れ:
//   パッケージの物質の光の値(light_absorption_*・light_reflectance_*。名前は仮)→ ベイク(sim/reaction_table.cpp。書かない値は §2.3 の仮)
//   → LtSpeciesLight(物質 ID ごと。BakedReactionTable::speciesLight。GPU は sim/gpu_sun_column が写す)
//   → LtCellBandOf(セルの成分 × 物質の値 → 帯ごとの Σ a·n と、吸い込みで重み付けた照り返しの割合)
//   → 太陽の影の柱(sun_column.hlsli)。距離ごとの段(R-R1)・日の光を集める(R-R2)も同じ値を読む(一つの道。S12)
// 吸い込みの意味は ADR-0252 と同じ「1 mol が遮る面積 a(mm²/mol)」を帯ごとに 2 つにしたもの。通しやすさは 1 − 吸い込みの形で導く(別の欄を持たない)。
// 浮動小数点は使わない。
#ifndef BICAMERAL_LIGHT_HLSLI
#define BICAMERAL_LIGHT_HLSLI

#include "radiation.hlsli"

RX_NAMESPACE_BEGIN

// --- 帯 ----------------------------------------------------------------------------------------
FX_CONST uint32_t LT_BAND_SHORT = 0;  // 短い帯(見える光。紫外も含める。境は約 0.7 µm〔仮〕)
FX_CONST uint32_t LT_BAND_LONG = 1;   // 長い帯(赤外)
FX_CONST uint32_t LT_BANDS = 2;

// --- 書かない物質の値(23 §2.3・D-610 で確定。前の ADR-0382 仮 1)---
// 固体・液体: 「灰色で通さない」。吸い込みは表の上限(1 mmol でセルの厚さの τ ≈ 17。細かさで割っても通さない)
FX_CONST uint32_t LT_OPAQUE_ABSORPTION_MM2_PER_MOL = 0xFFFFFFFFu;
FX_CONST uint32_t
    LT_DEFAULT_CONDENSED_REFLECTANCE_PERMILLE = 500;  // 照り返し 0.5(値は仮。D-610 は灰色で通さないことを決めた)
FX_CONST uint32_t LT_PERMILLE = 1000;
FX_CONST uint32_t LT_ONE_Q16 = 65536u;

// 照り返しの重みを 2^44 未満に縮める(重み × 割合 < 2^60、8 成分の和 < 2^63 で 64bit に収める)
FX_CONST uint32_t LT_WEIGHT_BITS = 43;

// 物質 1 つの光の値(16 バイト。添字 = 物質 ID。ID 0 は全部 0)
struct LtSpeciesLight {
    uint32_t absorptionShort;      // 1 mol が短い帯を遮る面積(mm²/mol)
    uint32_t absorptionLong;       // 1 mol が長い帯を遮る面積(mm²/mol)
    uint32_t reflectanceShortQ16;  // 当たった短い帯のうち吸わずに返す割合(Q16。0〜65536)
    uint32_t reflectanceLongQ16;
};

// セル 1 つ・帯 1 つ
struct LtCellBand {
    FxU128 absorption;        // Σ a·n(mm²/mol × µmol。セルのレベルの単位の数のまま。細かさの換算は使う側)
    uint32_t reflectanceQ16;  // 照り返しの割合(物質ごとの割合を a·n で重み付けた平均。吸わない物質は効かない)
    uint32_t padding;
};

FX_FN uint32_t LtAbsorptionOf(LtSpeciesLight light, uint32_t band) {
    return band == LT_BAND_SHORT ? light.absorptionShort : light.absorptionLong;
}

FX_FN uint32_t LtReflectanceOf(LtSpeciesLight light, uint32_t band) {
    return band == LT_BAND_SHORT ? light.reflectanceShortQ16 : light.reflectanceLongQ16;
}

// 千分率 → Q16(最も近い値)
FX_FN uint32_t LtPermilleToQ16(uint32_t permille) {
    return ((permille * LT_ONE_Q16) + (LT_PERMILLE / 2)) / LT_PERMILLE;
}

// 128bit の最上位の 1 の位置(0 なら 0)
FX_FN uint32_t LtMsbU128(FxU128 value) {
    if (value.hi != 0)
        return 64 + FxMsbU64(value.hi);

    return value.lo == 0 ? 0u : FxMsbU64(value.lo);
}

// セル cell の帯 band の吸い込みの和と照り返しの割合。Lights の約束: LtSpeciesLight Light(uint32_t speciesId)。
// 成分の順(物質 ID の昇順)に足すので、CPU と GPU で同じビット
template <typename Lights>
FX_FN LtCellBand LtCellBandOf(Lights lights, FX_IN(RxCell) cell, uint32_t band) {
    LtCellBand result;
    result.absorption.hi = 0;
    result.absorption.lo = 0;
    result.reflectanceQ16 = 0;
    result.padding = 0;

    // --- Σ a·n ---
    FX_UNROLL
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        if (i < cell.speciesCount) {
            const uint32_t absorption = LtAbsorptionOf(lights.Light(cell.species[i]), band);
            result.absorption = RdAddAbsorption(result.absorption, absorption, cell.amounts[i]);
        }
    }

    const uint32_t msb = LtMsbU128(result.absorption);
    if (msb == 0 && result.absorption.lo == 0)
        return result;

    // --- 照り返し: Σ ρ·w ÷ Σ w(w = a·n を 2^44 未満に縮めた重み)---
    const uint32_t shift = msb > LT_WEIGHT_BITS ? msb - LT_WEIGHT_BITS : 0u;
    uint64_t weightSum = 0;
    uint64_t weightedSum = 0;
    FX_UNROLL
    for (uint32_t j = 0; j < RX_MAX_CELL_SPECIES; ++j) {
        if (j < cell.speciesCount) {
            const LtSpeciesLight light = lights.Light(cell.species[j]);
            const FxU128 term = FxMulU64Full(cell.amounts[j], (uint64_t)LtAbsorptionOf(light, band));
            const uint64_t weight = FxShiftRightU128(term, shift);
            weightSum += weight;
            weightedSum += weight * (uint64_t)LtReflectanceOf(light, band);
        }
    }

    result.reflectanceQ16 = weightSum == 0 ? 0u : (uint32_t)(weightedSum / weightSum);

    return result;
}

RX_NAMESPACE_END

#endif  // BICAMERAL_LIGHT_HLSLI
