// reaction.hlsli — 1 セルの反応の評価(docs/design/02-reaction-system.md §3。T-0014)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束に従う)。CPU リファレンス(engine/src/sim/reaction.h)と
// GPU のカーネル(shaders/sim/reaction_cells.hlsl)が同じ関数を呼ぶので、同じセル・同じ表からはビット単位で同じ結果になる。
//
// データの流れ:
//   ベイクした表(engine/src/sim/reaction_table.cpp の BakeReactionTable: 物質・規則・索引・速度の表)
//   + セル(成分 = 物質 ID の昇順に並べた物質量、エネルギー)→ RxEvaluateCell → 次の刻みのセル
//
// 保存則(D-206・04 R2):
//   - 元素: 反応物と生成物は「係数 × 進行度」で整数のまま増減する(ADR-0012)。ベイクが元素の釣り合いを確かめるので、元素の数は完全に保存される。
//   - エネルギー: セルのエネルギーは「熱 + 化学(Σ 物質量 × H0)」の合計。反応は成分を変えるだけで合計を変えない(熱は導出値)。
//
// 表の読み方は呼ぶ側が Table 型で渡す(C++ は配列の span、HLSL はバッファを読む構造体)。Table に要るメソッド:
//   RxSpecies Species(uint32_t id) / RxRule Rule(uint32_t id) / uint32_t RuleIndex(uint32_t position) /
//   uint64_t Rate(uint32_t rule, uint32_t kelvin)
#ifndef BICAMERAL_REACTION_HLSLI
#define BICAMERAL_REACTION_HLSLI

#include "fixed.hlsli"
#include "units.hlsli"

#ifdef __cplusplus
#define RX_NAMESPACE_BEGIN          \
    namespace bicameral::reaction { \
        using namespace ::bicameral::fx;
#define RX_NAMESPACE_END }
#else
#define RX_NAMESPACE_BEGIN
#define RX_NAMESPACE_END
#endif

RX_NAMESPACE_BEGIN

// --- 大きさ ------------------------------------------------------------------------------------
FX_CONST uint32_t RX_MAX_CELL_SPECIES = 8;  // セルの成分のインラインの数(溢れは T-0017 / R-MULTI-4)
FX_CONST uint32_t RX_MAX_REACTANTS = 3;
FX_CONST uint32_t RX_MAX_PRODUCTS = 3;
FX_CONST uint32_t RX_MAX_CANDIDATES = 16;        // 1 セルで同時に評価する規則の数の上限
FX_CONST uint32_t RX_RATE_TABLE_KELVINS = 4096;  // 速度の表は 0〜4095 K を 1 K ごと。それより熱いと 4095 K の値
FX_CONST uint32_t RX_NO_SLOT = 0xFFFFFFFFu;
FX_CONST uint32_t RX_RANDOM_PURPOSE = 0x52780001u;  // 進行度の端数を丸める乱数の用途(FxHash64)

// 吸熱の規則が 1 刻みに使える熱は、今の熱の 2^-RX_ENDOTHERMIC_HEAT_SHIFT まで(温度が 1 刻みで 1/8 より下がらない)。
// 1 刻みに 1 回の評価(陽的)だと、速い吸熱の反応は始めの温度の速さで進み続け、1 刻みで 0 K まで冷えてしまう(T-0014 で確認)。
// 温度が下がれば速さも指数的に落ちるので、刻みごとに少しずつ冷えて自然に止まる。R-REACT-1(細分)で見直す
FX_CONST uint32_t RX_ENDOTHERMIC_HEAT_SHIFT = 3;

// 望む進行度の上限。望む量は保存量ではないので飽和させてよく、この後で反応物の量に縮める
FX_CONST uint64_t RX_EXTENT_SATURATION = FX_U64(0x40000000u, 0u);

// エネルギー(mJ)を熱の単位(µJ)にする倍率、温度(mK)を熱 ÷ 熱容量(µJ ÷ nJ/K)から出す倍率
FX_CONST uint64_t RX_MICROJOULES_PER_MILLIJOULE = 1000;
FX_CONST uint64_t RX_MILLIKELVIN_NUMERATOR = 1000000;

// --- 表の形(ベイクが作り、GPU にはこのまま載せる。sim/reaction_table.h が大きさを static_assert する)---

// 物質(ID が添字。0 は使わない)
struct RxSpecies {
    int64_t h0;             // 化学のエネルギー(J/mol = µJ/µmol)。0 K 基準: 生成エンタルピー − 比熱 × 298.15 K
    uint32_t heatCapacity;  // 比熱(mJ/(mol·K))。相ごとの値は M2
    uint32_t ruleBegin;     // 索引(この物質を「ID が最小の反応物」に持つ規則)の始まり
    uint32_t ruleCount;
    uint32_t padding;
};

// 反応の規則
struct RxRule {
    // --- 反応物・生成物(係数は正の整数)---
    uint32_t reactantCount;
    uint32_t productCount;
    uint32_t reactants[RX_MAX_REACTANTS];
    uint32_t reactantCoefficients[RX_MAX_REACTANTS];
    uint32_t products[RX_MAX_PRODUCTS];
    uint32_t productCoefficients[RX_MAX_PRODUCTS];

    // --- 速度と熱 ---
    uint32_t firstOrderMask;  // 速度式で次数 1 の反応物(ビット i = reactants[i])。1〜2 個
    uint32_t padding;
    int64_t reactionEnthalpy;  // 進行度 1 µmol あたりの µJ(= J/mol)。Σ 生成物の H0 − Σ 反応物の H0。正なら吸熱
    uint64_t key;              // 規則の名前のハッシュ。乱数の入力に使い、表の並び(規則の ID)に結果を依存させない
};

// セル(レベル 0)。成分は物質 ID の昇順、物質量が 0 になったものは消す(同じ中身なら同じ並び = 同じハッシュ)
struct RxCell {
    int64_t energy;  // mJ(熱 + 化学)
    uint32_t speciesCount;
    uint32_t padding;
    uint32_t species[RX_MAX_CELL_SPECIES];
    uint64_t amounts[RX_MAX_CELL_SPECIES];  // µmol
};

// セルの熱(導出値)
struct RxThermal {
    int64_t heat;           // µJ = エネルギー × 1000 − Σ 物質量 × H0
    uint64_t heatCapacity;  // nJ/K = Σ 物質量 × 比熱
    int32_t temperature;    // mK
};

// 1 刻みで評価する規則の候補と、その進行度(µmol)
struct RxCandidates {
    uint32_t count;
    uint32_t rules[RX_MAX_CANDIDATES];
    uint64_t extents[RX_MAX_CANDIDATES];
};

// --- 小さな道具 --------------------------------------------------------------------------------

FX_FN RxCell RxMakeEmptyCell(int64_t energy) {
    RxCell cell;
    cell.energy = energy;
    cell.speciesCount = 0;
    cell.padding = 0;
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        cell.species[i] = 0;
        cell.amounts[i] = 0;
    }

    return cell;
}

// 物質 ID の位置(無ければ RX_NO_SLOT)
FX_FN uint32_t RxFindSlot(RxCell cell, uint32_t speciesId) {
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        if (cell.species[i] == speciesId)
            return i;
    }

    return RX_NO_SLOT;
}

// 符号つきの積が int64 に収まることを確かめて返す(R8)
FX_FN int64_t RxMulS64Checked(int64_t a, int64_t b) {
    const FxU128 product = FxMulS64Full(a, b);
    FX_ASSERT(product.hi == ((int64_t)product.lo < 0 ? FX_U64(0xFFFFFFFFu, 0xFFFFFFFFu) : (uint64_t)0));

    return (int64_t)product.lo;
}

// 128bit を右へずらした下位 64bit(はみ出す上位は捨てる。端数を取り出すため)
FX_FN uint64_t RxShiftRightLow64(FxU128 value, uint32_t shift) {
    if (shift == 0)
        return value.lo;

    if (shift < 64)
        return (value.lo >> shift) | (value.hi << (64 - shift));

    return shift < 128 ? value.hi >> (shift - 64) : (uint64_t)0;
}

// --- 熱と温度 ----------------------------------------------------------------------------------

// 化学のエネルギー(µJ)= Σ 物質量 × H0
template <typename Table>
FX_FN int64_t RxChemicalEnergy(Table table, RxCell cell) {
    int64_t chemical = 0;
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        const int64_t term = RxMulS64Checked((int64_t)cell.amounts[i], table.Species(cell.species[i]).h0);
        FX_ASSERT(!FxAddOverflowsS64(chemical, term));
        chemical += term;
    }

    return chemical;
}

// 熱容量(nJ/K)= Σ 物質量 × 比熱
template <typename Table>
FX_FN uint64_t RxHeatCapacity(Table table, RxCell cell) {
    uint64_t heatCapacity = 0;
    for (uint32_t i = 0; i < cell.speciesCount; ++i)
        heatCapacity += cell.amounts[i] * (uint64_t)table.Species(cell.species[i]).heatCapacity;

    return heatCapacity;
}

template <typename Table>
FX_FN RxThermal RxComputeThermal(Table table, RxCell cell) {
    const int64_t chemical = RxChemicalEnergy(table, cell);
    const uint64_t heatCapacity = RxHeatCapacity(table, cell);

    RxThermal thermal;
    thermal.heat = RxMulS64Checked(cell.energy, (int64_t)RX_MICROJOULES_PER_MILLIJOULE) - chemical;
    thermal.heatCapacity = heatCapacity;
    thermal.temperature = 0;
    FX_ASSERT(thermal.heat >= 0);  // 吸熱の規則は熱を資源として縮めるので、熱は負にならない
    if (heatCapacity == 0 || thermal.heat <= 0)
        return thermal;

    // 温度(mK)= 熱(µJ)× 1e6 ÷ 熱容量(nJ/K)。上位が熱容量以上なら 64bit に収まらない = 温度の上限を超える
    const FxU128 numerator = FxMulU64Full((uint64_t)thermal.heat, RX_MILLIKELVIN_NUMERATOR);
    FX_ASSERT(numerator.hi < heatCapacity);
    if (numerator.hi >= heatCapacity) {
        thermal.temperature = TEMPERATURE_MAX_MILLIKELVIN;
        return thermal;
    }

    const uint64_t temperature = FxDivU128By64(numerator, heatCapacity).quotient;
    FX_ASSERT(temperature <= (uint64_t)TEMPERATURE_MAX_MILLIKELVIN);
    thermal.temperature = (int32_t)(temperature < (uint64_t)TEMPERATURE_MAX_MILLIKELVIN
                                        ? temperature
                                        : (uint64_t)TEMPERATURE_MAX_MILLIKELVIN);

    return thermal;
}

// --- 速度: 望む進行度 --------------------------------------------------------------------------

// 速度の表の値(上位 32bit = 仮数、下位 32bit = 2 の指数(int32))。値 = 仮数 × 2^指数
FX_FN uint64_t RxRateMantissa(uint64_t packed) {
    return packed >> 32;
}

FX_FN int32_t RxRateExponent(uint64_t packed) {
    return (int32_t)(uint32_t)(packed & FX_LOW32_MASK);
}

// 反応物の量の積 × 仮数 × 2^指数 を整数にし、端数(32bit)は乱数 random と比べて丸める(確率的な丸め。R6 の乱数なので決定的)。
// 1 刻みの進行度が 1 µmol に満たない遅い反応も、平均では正しい速さで進む
FX_FN uint64_t RxScaleExtent(FxU128 product, uint64_t mantissa, int32_t exponent, uint32_t random) {
    if (mantissa == 0 || (product.hi == 0 && product.lo == 0))
        return 0;

    // --- 積を 64bit に収める(下位を切り捨て、そのぶん指数に足す)---
    int32_t shift = exponent;
    uint64_t value = product.lo;
    if (product.hi != 0) {
        const uint32_t dropped = FxMsbU64(product.hi) + 1;
        value = RxShiftRightLow64(product, dropped);
        shift += (int32_t)dropped;
    }

    // scaled < 2^96
    const FxU128 scaled = FxMulU64Full(value, mantissa);

    // --- 2^shift 倍が整数になる(端数なし)---
    if (shift >= 0) {
        if (shift >= 62 || scaled.hi != 0 || (scaled.lo >> (uint32_t)(62 - shift)) != 0)
            return RX_EXTENT_SATURATION;

        return scaled.lo << (uint32_t)shift;
    }

    // --- 右へずらす: 整数の部分と、その下の 32bit の端数 ---
    const uint32_t right = (uint32_t)(-shift);
    if (right < 32 && (scaled.hi >> right) != 0)
        return RX_EXTENT_SATURATION;

    const uint64_t integerPart = RxShiftRightLow64(scaled, right);
    if (integerPart >= RX_EXTENT_SATURATION)
        return RX_EXTENT_SATURATION;

    const uint64_t fraction = right >= 32 ? RxShiftRightLow64(scaled, right - 32) & FX_LOW32_MASK
                                          : (scaled.lo << (32 - right)) & FX_LOW32_MASK;

    return integerPart + ((uint64_t)random < fraction ? (uint64_t)1 : (uint64_t)0);
}

// 規則 rule をこのセルで評価したときの望む進行度(反応物が 1 つでも無ければ 0)。反応物それぞれの「ある量 ÷ 係数」で先に抑える
template <typename Table>
FX_FN uint64_t RxDesiredExtent(Table table, RxCell cell, uint32_t ruleId, RxRule rule, uint32_t kelvin,
                               uint64_t randomSeed) {
    uint64_t factors[2];
    factors[0] = 1;
    factors[1] = 1;
    uint32_t factorCount = 0;
    uint64_t ownLimit = RX_EXTENT_SATURATION;
    for (uint32_t i = 0; i < rule.reactantCount; ++i) {
        const uint32_t slot = RxFindSlot(cell, rule.reactants[i]);
        if (slot == RX_NO_SLOT)
            return 0;

        const uint64_t amount = cell.amounts[slot];
        const uint64_t limit = amount / (uint64_t)rule.reactantCoefficients[i];
        ownLimit = limit < ownLimit ? limit : ownLimit;
        if (((rule.firstOrderMask >> i) & 1) != 0 && factorCount < 2) {
            factors[factorCount] = amount;
            factorCount += 1;
        }
    }

    const uint64_t packed = table.Rate(ruleId, kelvin);
    const uint32_t random = (uint32_t)(FxHashCombine(randomSeed, rule.key) & FX_LOW32_MASK);
    const uint64_t desired = RxScaleExtent(FxMulU64Full(factors[0], factors[1]), RxRateMantissa(packed),
                                           RxRateExponent(packed), random);

    return desired < ownLimit ? desired : ownLimit;
}

// 候補の規則を集める。規則は「ID が最小の反応物」の索引にだけ入っているので、成分を順に見れば重複なく引ける
template <typename Table>
FX_FN RxCandidates RxCollectCandidates(Table table, RxCell cell, uint32_t kelvin, uint64_t randomSeed) {
    RxCandidates candidates;
    candidates.count = 0;
    for (uint32_t i = 0; i < RX_MAX_CANDIDATES; ++i) {
        candidates.rules[i] = 0;
        candidates.extents[i] = 0;
    }

    for (uint32_t slot = 0; slot < cell.speciesCount; ++slot) {
        const RxSpecies species = table.Species(cell.species[slot]);
        for (uint32_t j = 0; j < species.ruleCount; ++j) {
            const uint32_t ruleId = table.RuleIndex(species.ruleBegin + j);
            const uint64_t extent = RxDesiredExtent(table, cell, ruleId, table.Rule(ruleId), kelvin, randomSeed);
            if (extent == 0)
                continue;

            FX_ASSERT(candidates.count < RX_MAX_CANDIDATES);
            if (candidates.count >= RX_MAX_CANDIDATES)
                return candidates;

            candidates.rules[candidates.count] = ruleId;
            candidates.extents[candidates.count] = extent;
            candidates.count += 1;
        }
    }

    return candidates;
}

// --- 取り合いの解決(02 §3 の 3。並び順に依存しない)------------------------------------------------

// extent × available ÷ demand の切り捨て(available < demand のときだけ呼ぶ)
FX_FN uint64_t RxShrink(uint64_t extent, uint64_t available, uint64_t demand) {
    return FxDivU128By64(FxMulU64Full(extent, available), demand).quotient;
}

// 物質ごとに「要求の合計」と「ある量」を比べ、足りない資源を使う規則を min(ある量 ÷ 要求) の比で縮める。
// 吸熱の規則は熱(の 1/8。RX_ENDOTHERMIC_HEAT_SHIFT)も資源として同じく縮める(熱が負にならない)。どの規則も元の進行度に同じ比を掛けるので、並び順は結果に影響しない。
// 切り捨てなので、縮めた後の消費の合計は必ずある量以下
template <typename Table>
FX_FN RxCandidates RxResolveContention(Table table, RxCell cell, RxThermal thermal, RxCandidates candidates) {
    // --- 要求の合計(反応物は必ずセルにあるので、成分の位置で持つ)---
    uint64_t demand[RX_MAX_CELL_SPECIES];
    // HLSL には範囲 for が無い
    // NOLINTNEXTLINE(modernize-loop-convert)
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i)
        demand[i] = 0;

    uint64_t heatDemand = 0;
    for (uint32_t c = 0; c < candidates.count; ++c) {
        const RxRule rule = table.Rule(candidates.rules[c]);
        for (uint32_t i = 0; i < rule.reactantCount; ++i) {
            const uint32_t slot = RxFindSlot(cell, rule.reactants[i]);
            const uint64_t amount = candidates.extents[c] * (uint64_t)rule.reactantCoefficients[i];
            FX_ASSERT(demand[slot] + amount >= amount);
            demand[slot] += amount;
        }

        if (rule.reactionEnthalpy > 0) {
            const FxU128 heat = FxMulU64Full(candidates.extents[c], (uint64_t)rule.reactionEnthalpy);
            FX_ASSERT(heat.hi == 0 && heatDemand + heat.lo >= heatDemand);
            heatDemand += heat.lo;
        }
    }

    // --- 縮める(元の進行度から、足りない資源ごとの比の最小)---
    const uint64_t heatAvailable = thermal.heat > 0 ? (uint64_t)thermal.heat >> RX_ENDOTHERMIC_HEAT_SHIFT : (uint64_t)0;
    RxCandidates resolved = candidates;
    for (uint32_t c = 0; c < candidates.count; ++c) {
        const RxRule rule = table.Rule(candidates.rules[c]);
        const uint64_t extent = candidates.extents[c];
        uint64_t result = extent;
        for (uint32_t i = 0; i < rule.reactantCount; ++i) {
            const uint32_t slot = RxFindSlot(cell, rule.reactants[i]);
            if (demand[slot] <= cell.amounts[slot])
                continue;

            const uint64_t shrunk = RxShrink(extent, cell.amounts[slot], demand[slot]);
            result = shrunk < result ? shrunk : result;
        }

        if (rule.reactionEnthalpy > 0 && heatDemand > heatAvailable) {
            const uint64_t shrunk = RxShrink(extent, heatAvailable, heatDemand);
            result = shrunk < result ? shrunk : result;
        }

        resolved.extents[c] = result;
    }

    return resolved;
}

// --- 成分を更新する ----------------------------------------------------------------------------

// 物質量が 0 になった成分を消して詰める(並びは保つ)
FX_FN RxCell RxCompactCell(RxCell cell) {
    RxCell result = RxMakeEmptyCell(cell.energy);
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        if (cell.amounts[i] == 0)
            continue;

        result.species[result.speciesCount] = cell.species[i];
        result.amounts[result.speciesCount] = cell.amounts[i];
        result.speciesCount += 1;
    }

    return result;
}

// 物質を足す(無ければ ID の順の位置に差し込む)
FX_FN RxCell RxAddSpecies(RxCell cell, uint32_t speciesId, uint64_t amount) {
    if (amount == 0)
        return cell;

    uint32_t position = 0;
    while (position < cell.speciesCount && cell.species[position] < speciesId)
        position += 1;

    if (position < cell.speciesCount && cell.species[position] == speciesId) {
        FX_ASSERT(cell.amounts[position] + amount >= amount);
        cell.amounts[position] += amount;
        return cell;
    }

    // 溢れは T-0017 / R-MULTI-4 で連鎖にする。今は止める(試験の表の物質は 7 つ)
    FX_ASSERT(cell.speciesCount < RX_MAX_CELL_SPECIES);
    if (cell.speciesCount >= RX_MAX_CELL_SPECIES)
        return cell;

    for (uint32_t i = cell.speciesCount; i > position; --i) {
        cell.species[i] = cell.species[i - 1];
        cell.amounts[i] = cell.amounts[i - 1];
    }

    cell.species[position] = speciesId;
    cell.amounts[position] = amount;
    cell.speciesCount += 1;

    return cell;
}

// 決まった進行度で反応物を引き、生成物を足す。エネルギーは変えない(化学のエネルギーが熱に変わるだけ)
template <typename Table>
FX_FN RxCell RxApplyExtents(Table table, RxCell cell, RxCandidates resolved) {
    // --- 反応物を引く(差し込みで位置がずれる前に)---
    for (uint32_t c = 0; c < resolved.count; ++c) {
        const RxRule rule = table.Rule(resolved.rules[c]);
        for (uint32_t i = 0; i < rule.reactantCount; ++i) {
            const uint32_t slot = RxFindSlot(cell, rule.reactants[i]);
            const uint64_t amount = resolved.extents[c] * (uint64_t)rule.reactantCoefficients[i];
            FX_ASSERT(amount <= cell.amounts[slot]);
            cell.amounts[slot] -= amount;
        }
    }

    // --- 生成物を足す ---
    RxCell result = RxCompactCell(cell);
    for (uint32_t c = 0; c < resolved.count; ++c) {
        const RxRule rule = table.Rule(resolved.rules[c]);
        for (uint32_t i = 0; i < rule.productCount; ++i) {
            const uint64_t amount = resolved.extents[c] * (uint64_t)rule.productCoefficients[i];
            result = RxAddSpecies(result, rule.products[i], amount);
        }
    }

    return result;
}

// --- 1 セルの 1 刻み(02 §3)-------------------------------------------------------------------

// 乱数の種は (世界のシード, 刻み, セルの ID) から作る(R6)。規則ごとの乱数は、これに規則の鍵を混ぜる
FX_FN uint64_t RxRandomSeed(uint64_t worldSeed, uint64_t tick, uint64_t cellId) {
    return FxHash64(worldSeed, tick, cellId, RX_RANDOM_PURPOSE);
}

template <typename Table>
FX_FN RxCell RxEvaluateCell(Table table, RxCell cell, uint64_t worldSeed, uint64_t tick, uint64_t cellId) {
    const RxThermal thermal = RxComputeThermal(table, cell);
    const uint32_t kelvin = (uint32_t)thermal.temperature / (uint32_t)MILLIKELVIN_PER_KELVIN;
    const uint32_t tableKelvin = kelvin < RX_RATE_TABLE_KELVINS ? kelvin : RX_RATE_TABLE_KELVINS - 1;
    const RxCandidates candidates = RxCollectCandidates(table, cell, tableKelvin,
                                                        RxRandomSeed(worldSeed, tick, cellId));
    if (candidates.count == 0)
        return cell;

    const RxCandidates resolved = RxResolveContention(table, cell, thermal, candidates);

    return RxApplyExtents(table, cell, resolved);
}

RX_NAMESPACE_END

#endif  // BICAMERAL_REACTION_HLSLI
