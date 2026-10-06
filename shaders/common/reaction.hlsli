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

// 遅すぎる反応は進まない(T-0089・ユーザー 2026-10-01): 1 刻みの進みの期待値が 2^-16 µmol 未満(端数だけで、この値より小さい)なら 0。
// 端数を乱数で丸めるので、これが無いと室温の木のごく僅かな酸化のような反応が確率的に起き続け、
// 「何も変わらなかったブロックを眠らせる」と、全部のセルを計算した場合と結果がずれる。この下限があれば、
// どの規則も進めない(RxCellStep::possible == 0)セルは、刻みが変わっても(乱数が変わっても)必ず変わらない
FX_CONST uint64_t RX_EXTENT_CUTOFF_FRACTION = FX_U64(0u, 0x10000u);  // 端数(2^-32 µmol 単位)の下限 = 2^-16 µmol

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
    uint32_t conductivity;  // 熱伝導率(mW/(m·K))。セルの値は物質量で重み付けた平均(heat_conduction.hlsli。T-0089)
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
    uint32_t possible;  // 進める(望む進行度の期待値が下限以上の)規則が 1 つでもあれば 1。丸めで 0 になった規則も数える
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

// 望む進行度の 1 つの値(丸めた後)と、丸める前の期待値が下限(RX_EXTENT_CUTOFF_FRACTION)以上だったか
struct RxExtentSample {
    uint64_t value;
    uint32_t possible;
};

FX_FN RxExtentSample RxMakeExtentSample(uint64_t value, bool possible) {
    RxExtentSample sample;
    sample.value = value;
    sample.possible = possible ? 1 : 0;

    return sample;
}

// 反応物の量の積 × 仮数 × 2^指数 を整数にし、端数(32bit)は乱数 random と比べて丸める(確率的な丸め。R6 の乱数なので決定的)。
// 1 刻みの進行度が 1 µmol に満たない遅い反応も、平均では正しい速さで進む。ただし 2^-16 µmol 未満の端数だけなら 0(進まない)
FX_FN RxExtentSample RxScaleExtent(FxU128 product, uint64_t mantissa, int32_t exponent, uint32_t random) {
    if (mantissa == 0 || (product.hi == 0 && product.lo == 0))
        return RxMakeExtentSample(0, false);

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
            return RxMakeExtentSample(RX_EXTENT_SATURATION, true);

        const uint64_t exact = scaled.lo << (uint32_t)shift;

        return RxMakeExtentSample(exact, exact != 0);
    }

    // --- 右へずらす: 整数の部分と、その下の 32bit の端数 ---
    const uint32_t right = (uint32_t)(-shift);
    if (right < 32 && (scaled.hi >> right) != 0)
        return RxMakeExtentSample(RX_EXTENT_SATURATION, true);

    const uint64_t integerPart = RxShiftRightLow64(scaled, right);
    if (integerPart >= RX_EXTENT_SATURATION)
        return RxMakeExtentSample(RX_EXTENT_SATURATION, true);

    const uint64_t fraction = right >= 32 ? RxShiftRightLow64(scaled, right - 32) & FX_LOW32_MASK
                                          : (scaled.lo << (32 - right)) & FX_LOW32_MASK;

    if (integerPart == 0 && fraction < RX_EXTENT_CUTOFF_FRACTION)
        return RxMakeExtentSample(0, false);

    return RxMakeExtentSample(integerPart + ((uint64_t)random < fraction ? (uint64_t)1 : (uint64_t)0), true);
}

// 規則 rule をこのセルで評価したときの望む進行度(反応物が 1 つでも無ければ 0)。反応物それぞれの「ある量 ÷ 係数」で先に抑える。
// 進めるか(possible)は、ある量で抑えた結果が 0 なら 0(反応物が係数より少ない規則は、刻みが変わっても進まない)
template <typename Table>
FX_FN RxExtentSample RxDesiredExtent(Table table, RxCell cell, uint32_t ruleId, RxRule rule, uint32_t kelvin,
                                     uint64_t randomSeed) {
    uint64_t factors[2];
    factors[0] = 1;
    factors[1] = 1;
    uint32_t factorCount = 0;
    uint64_t ownLimit = RX_EXTENT_SATURATION;
    for (uint32_t i = 0; i < rule.reactantCount; ++i) {
        const uint32_t slot = RxFindSlot(cell, rule.reactants[i]);
        if (slot == RX_NO_SLOT)
            return RxMakeExtentSample(0, false);

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
    const RxExtentSample desired = RxScaleExtent(FxMulU64Full(factors[0], factors[1]), RxRateMantissa(packed),
                                                 RxRateExponent(packed), random);

    return RxMakeExtentSample(desired.value < ownLimit ? desired.value : ownLimit,
                              desired.possible != 0 && ownLimit != 0);
}

// 候補の規則を集める。規則は「ID が最小の反応物」の索引にだけ入っているので、成分を順に見れば重複なく引ける
template <typename Table>
FX_FN RxCandidates RxCollectCandidates(Table table, RxCell cell, uint32_t kelvin, uint64_t randomSeed) {
    RxCandidates candidates;
    candidates.count = 0;
    candidates.possible = 0;
    for (uint32_t i = 0; i < RX_MAX_CANDIDATES; ++i) {
        candidates.rules[i] = 0;
        candidates.extents[i] = 0;
    }

    for (uint32_t slot = 0; slot < cell.speciesCount; ++slot) {
        const RxSpecies species = table.Species(cell.species[slot]);
        for (uint32_t j = 0; j < species.ruleCount; ++j) {
            const uint32_t ruleId = table.RuleIndex(species.ruleBegin + j);
            const RxExtentSample sample = RxDesiredExtent(table, cell, ruleId, table.Rule(ruleId), kelvin, randomSeed);
            candidates.possible |= sample.possible;
            const uint64_t extent = sample.value;
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

// --- 待ちの丸め: 遅い反応は「次に 1 単位進む刻み」を決める(T-0105・D-429。研究)-----------------------
// 上の丸め(毎刻みの乱数と端数を比べる)は、次に当たる刻みを閉じた式で求められないので、眠らせたまま起こす刻みを決められない。
// 待ちの丸めは、ブロックが最後に変わった刻み changedTick からの待ち n を幾何分布で引く: 進行度 = 整数部 + [刻み ≥ changedTick + n]。
//   - n は P(n = k) = (1 − f)^(k−1) f(f = 1 刻みの進みの端数)。乱数は (世界のシード, changedTick, セル, 規則の鍵) だけで決まり、刻みに依らない。
//   - 幾何分布は記憶が無いので、ブロックが変わるたびに引き直しても平均の速さは偏らない(Gillespie 1977 の 1 単位ずつの確率過程と同じ)。
//     毎刻み変わるセルでは n ≤ 1 ⇔ 確率 f で、上の丸めと同じ分布になる。
//   - 速さ f はセルと規則の表だけで決まるので、ブロックが変わらない間は変わらない(魔法のパッチは「変わった」とみなして引き直す。ADR-0018)。
//   - 眠ったブロックを changedTick + n(セル・規則の最小)に起こせば、全部を計算した場合とビット一致する。D-424 の下限は要らない。
// n = max(1, ceil(log2 U ÷ log2(1 − f)))、U = (乱数 | 1) ÷ 2^64。log2 は整数(FxLog2U64。R7)。f が 1/16 未満の時は log2(1 − f) を
// 級数(−ln(1 − f) = f Σ f^(k−1) ÷ k)で求める(1 − f を作ると f の下の桁が消えるので)。
// 待ちが約 2^61 刻み(60 刻み/秒で約 12 億年。割る数の正規化で 2^60.5〜2^61.6)を超える引きは「起きない」
FX_CONST uint32_t RX_WAIT_PURPOSE = 0x52780002u;  // 待ちの乱数の用途(FxHash64)
FX_CONST uint64_t RX_WAIT_NEVER = FX_U64(0xFFFFFFFFu, 0xFFFFFFFFu);
FX_CONST uint64_t RX_WAIT_SERIES_LIMIT = FX_U64(0x10000000u, 0u);  // 端数(2^-64 単位)がこれ未満(f < 1/16)なら級数
FX_CONST uint32_t RX_WAIT_SERIES_TERMS = 16;                       // f < 1/16 なら 16 項で 2^-64 まで
FX_CONST uint32_t RX_WAIT_MAX_NUMERATOR_BITS = 122;                // 割られる数の桁の上限(商 = 待ちは 2^62 未満)
FX_CONST uint64_t RX_WAIT_TICK_LIMIT = FX_U64(0x40000000u, 0u);    // 待ちと刻みの印の上限 2^62(RxWakeTickOf)

// floor(2^64 ÷ k)(k = 2〜16。級数の 1 ÷ k)
FX_CONST uint64_t RX_RECIPROCAL_Q64[17] = {0u,
                                           0u,
                                           FX_U64(0x80000000u, 0x00000000u),
                                           FX_U64(0x55555555u, 0x55555555u),
                                           FX_U64(0x40000000u, 0x00000000u),
                                           FX_U64(0x33333333u, 0x33333333u),
                                           FX_U64(0x2AAAAAAAu, 0xAAAAAAAAu),
                                           FX_U64(0x24924924u, 0x92492492u),
                                           FX_U64(0x20000000u, 0x00000000u),
                                           FX_U64(0x1C71C71Cu, 0x71C71C71u),
                                           FX_U64(0x19999999u, 0x99999999u),
                                           FX_U64(0x1745D174u, 0x5D1745D1u),
                                           FX_U64(0x15555555u, 0x55555555u),
                                           FX_U64(0x13B13B13u, 0xB13B13B1u),
                                           FX_U64(0x12492492u, 0x49249249u),
                                           FX_U64(0x11111111u, 0x11111111u),
                                           FX_U64(0x10000000u, 0x00000000u)};

// 1 刻みの進みの期待値 = 整数部 + 端数 ÷ 2^64(µmol)
struct RxRate {
    uint64_t integerPart;
    uint64_t fraction;
};

FX_FN RxRate RxMakeRate(uint64_t integerPart, uint64_t fraction) {
    RxRate rate;
    rate.integerPart = integerPart;
    rate.fraction = fraction;

    return rate;
}

// 反応物の量の積 × 仮数 × 2^指数 を、整数部と 64bit の端数に分ける(RxScaleExtent と同じ桁の扱い。下限は無い)
FX_FN RxRate RxSplitRate(FxU128 product, uint64_t mantissa, int32_t exponent) {
    if (mantissa == 0 || (product.hi == 0 && product.lo == 0))
        return RxMakeRate(0, 0);

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
            return RxMakeRate(RX_EXTENT_SATURATION, 0);

        return RxMakeRate(scaled.lo << (uint32_t)shift, 0);
    }

    // --- 右へずらす: 整数の部分と、その下の 64bit の端数 ---
    const uint32_t right = (uint32_t)(-shift);
    if (right < 64 && (scaled.hi >> right) != 0)
        return RxMakeRate(RX_EXTENT_SATURATION, 0);

    const uint64_t integerPart = RxShiftRightLow64(scaled, right);
    if (integerPart >= RX_EXTENT_SATURATION)
        return RxMakeRate(RX_EXTENT_SATURATION, 0);

    const uint64_t fraction = right >= 64 ? RxShiftRightLow64(scaled, right - 64) : scaled.lo << (64 - right);

    return RxMakeRate(integerPart, fraction);
}

// −log2(1 − f) を 仮数 × 2^-scale で(f = fraction ÷ 2^64、0 < f < 1)
struct RxWaitDivisor {
    uint64_t mantissa;
    uint32_t scale;
};

FX_FN RxWaitDivisor RxWaitDivisorOf(uint64_t fraction) {
    RxWaitDivisor divisor;

    // --- f ≥ 1/16: 1 − f をそのまま log2(Q32)---
    if (fraction >= RX_WAIT_SERIES_LIMIT) {
        divisor.mantissa = ((uint64_t)64 << 32) - (uint64_t)FxLog2U64((uint64_t)0 - fraction);
        divisor.scale = 32;

        return divisor;
    }

    // --- f < 1/16: Σ f^(k−1) ÷ k(Q63)× log2 e(Q62)→ Q61、それに f を掛ける ---
    uint64_t power = FX_U64(0x80000000u, 0u);  // f^(k−1)(Q63)
    uint64_t series = power;
    for (uint32_t k = 2; k <= RX_WAIT_SERIES_TERMS; ++k) {
        power = FxMulHiU64(power, fraction);
        series += FxMulHiU64(power, RX_RECIPROCAL_Q64[k]);
    }

    const uint64_t perFraction = FxMulHiU64(series, FX_LOG2E_Q62);  // Q61。1.44〜1.49
    const uint32_t top = FxMsbU64(fraction);
    divisor.mantissa = FxMulHiU64(fraction << (63 - top), perFraction);  // = f × 2^-64 × … を 2^(124 − top) 倍した値
    divisor.scale = 124 - top;

    return divisor;
}

// 待ちの刻みの数 n ≥ 1(f = fraction ÷ 2^64 の幾何分布。hash は 64bit の乱数)。f = 0 なら RX_WAIT_NEVER
FX_FN uint64_t RxWaitTicks(uint64_t fraction, uint64_t hash) {
    if (fraction == 0)
        return RX_WAIT_NEVER;

    // −log2 U(Q32)。U = (hash | 1) ÷ 2^64 なので 0 < U < 1 で、値は 1 以上 2^38 以下
    const uint64_t numerator = ((uint64_t)64 << 32) - (uint64_t)FxLog2U64(hash | 1);
    const RxWaitDivisor divisor = RxWaitDivisorOf(fraction);

    // n = ceil(numerator × 2^-32 ÷ (仮数 × 2^-scale)) = ceil(numerator × 2^(scale − 32) ÷ 仮数)
    const uint32_t shift = divisor.scale - 32;
    if (FxMsbU64(numerator) + shift >= RX_WAIT_MAX_NUMERATOR_BITS)
        return RX_WAIT_NEVER;

    FxU128 shifted;
    shifted.hi = shift == 0 ? 0 : (shift >= 64 ? numerator << (shift - 64) : numerator >> (64 - shift));
    shifted.lo = shift >= 64 ? 0 : numerator << shift;
    const FxDivResult divided = FxDivU128By64(shifted, divisor.mantissa);
    const uint64_t ticks = divided.quotient + (divided.remainder != 0 ? (uint64_t)1 : (uint64_t)0);

    return ticks > 1 ? ticks : (uint64_t)1;
}

// 規則 rule のこのセルでの 1 刻みの進みの期待値(反応物が 1 つでも無ければ 0)と、反応物それぞれの「ある量 ÷ 係数」の最小
struct RxRuleRate {
    RxRate rate;
    uint64_t ownLimit;
};

template <typename Table>
FX_FN RxRuleRate RxComputeRuleRate(Table table, RxCell cell, uint32_t ruleId, RxRule rule, uint32_t kelvin) {
    RxRuleRate result;
    result.rate = RxMakeRate(0, 0);
    result.ownLimit = 0;

    uint64_t factors[2];
    factors[0] = 1;
    factors[1] = 1;
    uint32_t factorCount = 0;
    uint64_t ownLimit = RX_EXTENT_SATURATION;
    for (uint32_t i = 0; i < rule.reactantCount; ++i) {
        const uint32_t slot = RxFindSlot(cell, rule.reactants[i]);
        if (slot == RX_NO_SLOT)
            return result;

        const uint64_t amount = cell.amounts[slot];
        const uint64_t limit = amount / (uint64_t)rule.reactantCoefficients[i];
        ownLimit = limit < ownLimit ? limit : ownLimit;
        if (((rule.firstOrderMask >> i) & 1) != 0 && factorCount < 2) {
            factors[factorCount] = amount;
            factorCount += 1;
        }
    }

    const uint64_t packed = table.Rate(ruleId, kelvin);
    result.rate = RxSplitRate(FxMulU64Full(factors[0], factors[1]), RxRateMantissa(packed), RxRateExponent(packed));
    result.ownLimit = ownLimit;

    return result;
}

// 待ちの丸めの望む進行度と、changedTick から数えて何刻み目に初めて 0 でなくなるか(wait。整数部があれば 1、進めないなら RX_WAIT_NEVER)
struct RxWaitSample {
    uint64_t value;
    uint64_t wait;
};

template <typename Table>
FX_FN RxWaitSample RxDesiredExtentWait(Table table, RxCell cell, uint32_t ruleId, RxRule rule, uint32_t kelvin,
                                       uint64_t waitSeed, uint64_t elapsed) {
    const RxRuleRate ruleRate = RxComputeRuleRate(table, cell, ruleId, rule, kelvin);
    RxWaitSample sample;
    sample.value = 0;
    sample.wait = RX_WAIT_NEVER;
    if (ruleRate.ownLimit == 0)
        return sample;

    const uint64_t ticks = RxWaitTicks(ruleRate.rate.fraction, FxHashCombine(waitSeed, rule.key));
    const uint64_t desired = ruleRate.rate.integerPart + (elapsed >= ticks ? (uint64_t)1 : (uint64_t)0);
    sample.value = desired < ruleRate.ownLimit ? desired : ruleRate.ownLimit;
    sample.wait = ruleRate.rate.integerPart != 0 ? (uint64_t)1 : ticks;

    return sample;
}

// 待ちの丸めで候補を集める(RxCollectCandidates と同じ順)。wait は進めない規則も含めた待ちの最小
struct RxWaitCandidates {
    RxCandidates candidates;
    uint64_t wait;
};

template <typename Table>
FX_FN RxWaitCandidates RxCollectCandidatesWait(Table table, RxCell cell, uint32_t kelvin, uint64_t waitSeed,
                                               uint64_t elapsed) {
    RxWaitCandidates result;
    result.wait = RX_WAIT_NEVER;
    result.candidates.count = 0;
    result.candidates.possible = 0;
    for (uint32_t i = 0; i < RX_MAX_CANDIDATES; ++i) {
        result.candidates.rules[i] = 0;
        result.candidates.extents[i] = 0;
    }

    for (uint32_t slot = 0; slot < cell.speciesCount; ++slot) {
        const RxSpecies species = table.Species(cell.species[slot]);
        for (uint32_t j = 0; j < species.ruleCount; ++j) {
            const uint32_t ruleId = table.RuleIndex(species.ruleBegin + j);
            const RxWaitSample sample = RxDesiredExtentWait(table, cell, ruleId, table.Rule(ruleId), kelvin, waitSeed,
                                                            elapsed);
            result.wait = sample.wait < result.wait ? sample.wait : result.wait;
            if (sample.value == 0)
                continue;

            FX_ASSERT(result.candidates.count < RX_MAX_CANDIDATES);
            if (result.candidates.count >= RX_MAX_CANDIDATES)
                return result;

            result.candidates.rules[result.candidates.count] = ruleId;
            result.candidates.extents[result.candidates.count] = sample.value;
            result.candidates.count += 1;
        }
    }

    result.candidates.possible = result.wait != RX_WAIT_NEVER ? 1 : 0;

    return result;
}

// --- 取り合いの解決(02 §3 の 3。並び順に依存しない)------------------------------------------------

// 規則の候補が使う量の合計(反応物は必ずセルにあるので、成分の位置で持つ)と、吸熱の規則が使う熱
struct RxUsage {
    uint64_t amounts[RX_MAX_CELL_SPECIES];
    uint64_t heat;
};

template <typename Table>
FX_FN RxUsage RxSumUsage(Table table, RxCell cell, RxCandidates candidates) {
    RxUsage usage;
    // HLSL には範囲 for が無い
    // NOLINTNEXTLINE(modernize-loop-convert)
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i)
        usage.amounts[i] = 0;

    usage.heat = 0;
    for (uint32_t c = 0; c < candidates.count; ++c) {
        const RxRule rule = table.Rule(candidates.rules[c]);
        for (uint32_t i = 0; i < rule.reactantCount; ++i) {
            const uint32_t slot = RxFindSlot(cell, rule.reactants[i]);
            const uint64_t amount = candidates.extents[c] * (uint64_t)rule.reactantCoefficients[i];
            FX_ASSERT(usage.amounts[slot] + amount >= amount);
            usage.amounts[slot] += amount;
        }

        if (rule.reactionEnthalpy > 0) {
            const FxU128 heat = FxMulU64Full(candidates.extents[c], (uint64_t)rule.reactionEnthalpy);
            FX_ASSERT(heat.hi == 0 && usage.heat + heat.lo >= usage.heat);
            usage.heat += heat.lo;
        }
    }

    return usage;
}

// 縮めるときの乱数(規則ごと)。望む進行度の丸め(RxDesiredExtent)と同じハッシュの上位 32bit なので、2 つの丸めは独立
FX_FN uint32_t RxShrinkRandom(uint64_t randomSeed, RxRule rule) {
    return (uint32_t)(FxHashCombine(randomSeed, rule.key) >> 32);
}

// extent × available ÷ demand を確率的に丸める(available < demand のときだけ呼ぶ。T-0106・D-431)。
// 端数 = 余り ÷ demand を乱数 random(2^-32 単位)と比べる: random ÷ 2^32 < 余り ÷ demand ⇔ random × demand < 余り × 2^32。
// 切り捨てだと、取り合いで 1 刻み 1 未満に縮んだ脇の反応(酸素不足の火の炭の燃焼など)が毎刻み 0 になって消える
// 縮めた進行度: 切り捨てと、確率的に丸めた値(切り捨てか、その + 1)
struct RxShrunk {
    uint64_t truncated;
    uint64_t rounded;
};

FX_FN RxShrunk RxShrink(uint64_t extent, uint64_t available, uint64_t demand, uint32_t random) {
    const FxDivResult divided = FxDivU128By64(FxMulU64Full(extent, available), demand);
    const FxU128 threshold = FxMulU64Full((uint64_t)random, demand);
    const uint64_t remainderHigh = divided.remainder >> 32;
    const uint64_t remainderLow = divided.remainder << 32;
    const bool roundUp = threshold.hi < remainderHigh || (threshold.hi == remainderHigh && threshold.lo < remainderLow);

    RxShrunk shrunk;
    shrunk.truncated = divided.quotient;
    shrunk.rounded = divided.quotient + (roundUp ? (uint64_t)1 : (uint64_t)0);

    return shrunk;
}

// 規則が、ある量を超えて使われている資源(物質か、吸熱なら熱)を使うか
FX_FN bool RxOverdraws(RxRule rule, RxCell cell, RxUsage used, uint64_t heatAvailable) {
    for (uint32_t i = 0; i < rule.reactantCount; ++i) {
        const uint32_t slot = RxFindSlot(cell, rule.reactants[i]);
        if (used.amounts[slot] > cell.amounts[slot])
            return true;
    }

    return rule.reactionEnthalpy > 0 && used.heat > heatAvailable;
}

// 丸め上げた規則(roundedUp のビット)のせいで足りなくなった資源があれば、その資源を使う丸め上げた規則のうち
// 進行度が最も大きいもの(同じなら鍵が小さいもの)から 1 つずつ丸め上げを戻す。大きい規則ほど 1 の差の割合が小さいので、
// 偏りは主な反応に寄せ、脇の反応の期待値は崩さない。切り捨ての値は必ずある量以下なので、全部戻せば必ず足りる
template <typename Table>
FX_FN RxCandidates RxRevokeRoundUps(Table table, RxCell cell, RxCandidates resolved, uint32_t roundedUp,
                                    uint64_t heatAvailable) {
    RxUsage used = RxSumUsage(table, cell, resolved);
    for (uint32_t step = 0; step < RX_MAX_CANDIDATES && roundedUp != 0; ++step) {
        uint32_t pick = RX_NO_SLOT;
        uint64_t pickKey = 0;
        for (uint32_t c = 0; c < resolved.count; ++c) {
            const RxRule rule = table.Rule(resolved.rules[c]);
            if (((roundedUp >> c) & 1) == 0 || !RxOverdraws(rule, cell, used, heatAvailable))
                continue;

            const bool larger = pick == RX_NO_SLOT || resolved.extents[c] > resolved.extents[pick] ||
                                (resolved.extents[c] == resolved.extents[pick] && rule.key < pickKey);
            if (!larger)
                continue;

            pick = c;
            pickKey = rule.key;
        }

        if (pick == RX_NO_SLOT)
            break;

        resolved.extents[pick] -= 1;
        roundedUp &= ~(1u << pick);
        used = RxSumUsage(table, cell, resolved);
    }

    return resolved;
}

// 物質ごとに「要求の合計」と「ある量」を比べ、足りない資源を使う規則を min(ある量 ÷ 要求) の比で縮める。
// 吸熱の規則は熱(の 1/8。RX_ENDOTHERMIC_HEAT_SHIFT)も資源として同じく縮める(熱が負にならない)。どの規則も元の進行度に同じ比を掛けるので、並び順は結果に影響しない。
// 縮めた値は確率的に丸める(規則ごとに 1 つの乱数を、足りない資源のどれにも使う。丸めは単調なので、資源ごとに丸めた最小 = 比の最小を丸めた値)。
// 丸め上げで足りなくなった資源があれば RxRevokeRoundUps が戻すので、縮めた後の消費の合計は必ずある量以下
template <typename Table>
FX_FN RxCandidates RxResolveContention(Table table, RxCell cell, RxThermal thermal, RxCandidates candidates,
                                       uint64_t randomSeed) {
    const RxUsage demand = RxSumUsage(table, cell, candidates);
    const uint64_t heatAvailable = thermal.heat > 0 ? (uint64_t)thermal.heat >> RX_ENDOTHERMIC_HEAT_SHIFT : (uint64_t)0;

    // --- 縮める(元の進行度から、足りない資源ごとの比の最小)---
    RxCandidates resolved = candidates;
    uint32_t roundedUp = 0;
    for (uint32_t c = 0; c < candidates.count; ++c) {
        const RxRule rule = table.Rule(candidates.rules[c]);
        const uint64_t extent = candidates.extents[c];
        const uint32_t random = RxShrinkRandom(randomSeed, rule);
        uint64_t result = extent;
        uint64_t truncated = extent;
        for (uint32_t i = 0; i < rule.reactantCount; ++i) {
            const uint32_t slot = RxFindSlot(cell, rule.reactants[i]);
            if (demand.amounts[slot] <= cell.amounts[slot])
                continue;

            const RxShrunk shrunk = RxShrink(extent, cell.amounts[slot], demand.amounts[slot], random);
            result = shrunk.rounded < result ? shrunk.rounded : result;
            truncated = shrunk.truncated < truncated ? shrunk.truncated : truncated;
        }

        if (rule.reactionEnthalpy > 0 && demand.heat > heatAvailable) {
            const RxShrunk shrunk = RxShrink(extent, heatAvailable, demand.heat, random);
            result = shrunk.rounded < result ? shrunk.rounded : result;
            truncated = shrunk.truncated < truncated ? shrunk.truncated : truncated;
        }

        resolved.extents[c] = result;
        roundedUp |= result > truncated ? (1u << c) : 0u;
    }

    if (roundedUp == 0)
        return resolved;

    return RxRevokeRoundUps(table, cell, resolved, roundedUp, heatAvailable);
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

// 1 刻みの結果: 新しいセル、その熱(導出値。反応が無ければ入力の熱のまま)、進める規則があったか(眠れるかの判定。T-0089)
struct RxCellStep {
    RxCell cell;
    RxThermal thermal;
    uint32_t possible;
};

template <typename Table>
FX_FN RxCellStep RxStepCell(Table table, RxCell cell, uint64_t worldSeed, uint64_t tick, uint64_t cellId) {
    RxCellStep step;
    step.cell = cell;
    step.thermal = RxComputeThermal(table, cell);
    step.possible = 0;

    const uint32_t kelvin = (uint32_t)step.thermal.temperature / (uint32_t)MILLIKELVIN_PER_KELVIN;
    const uint32_t tableKelvin = kelvin < RX_RATE_TABLE_KELVINS ? kelvin : RX_RATE_TABLE_KELVINS - 1;
    const uint64_t randomSeed = RxRandomSeed(worldSeed, tick, cellId);
    const RxCandidates candidates = RxCollectCandidates(table, cell, tableKelvin, randomSeed);
    step.possible = candidates.possible;
    if (candidates.count == 0)
        return step;

    const RxCandidates resolved = RxResolveContention(table, cell, step.thermal, candidates, randomSeed);
    step.cell = RxApplyExtents(table, cell, resolved);
    step.thermal = RxComputeThermal(table, step.cell);

    return step;
}

template <typename Table>
FX_FN RxCell RxEvaluateCell(Table table, RxCell cell, uint64_t worldSeed, uint64_t tick, uint64_t cellId) {
    return RxStepCell(table, cell, worldSeed, tick, cellId).cell;
}

// --- 1 セルの 1 刻み(待ちの丸め。T-0105・D-429。研究)-----------------------------------------------

// 1 刻みの結果: 新しいセル、その熱、変わらなければ次に評価が要る刻み(wakeTick。進めないなら RX_WAIT_NEVER)。
// 刻み tick ≤ t < wakeTick の間にこのセルを評価しても(ブロックが変わらなければ)何も変わらないので、眠らせてよい
struct RxWaitStep {
    RxCell cell;
    RxThermal thermal;
    uint64_t wakeTick;
};

// 待ちの乱数の種は (世界のシード, ブロックが最後に変わった刻み, セルの ID) から作る。規則ごとの乱数は、これに規則の鍵を混ぜる
FX_FN uint64_t RxWaitSeed(uint64_t worldSeed, uint64_t changedTick, uint64_t cellId) {
    return FxHash64(worldSeed, changedTick, cellId, RX_WAIT_PURPOSE);
}

// changedTick から wait 刻み目の印(wait = RX_WAIT_NEVER なら RX_WAIT_NEVER)。待ちは 2^62 未満(RxWaitTicks の商)、印も 2^62 未満
// (60 刻み/秒で約 24 億年)なので足しても溢れない(R8: 飽和させない)。
// 「溢れたら RX_WAIT_NEVER」の飽和する足し算の形(wait >= ~changedTick ? ~0 : 和)は、NVIDIA のドライバ(RTX 3070 Ti)で
// 結果の下位 32bit が 0xFFFFFFFF になった(T-0124)。比べるのは RX_WAIT_NEVER との一致だけにする
FX_FN uint64_t RxWakeTickOf(uint64_t changedTick, uint64_t wait) {
    FX_ASSERT(changedTick < RX_WAIT_TICK_LIMIT);
    FX_ASSERT(wait == RX_WAIT_NEVER || wait < RX_WAIT_TICK_LIMIT);

    return wait == RX_WAIT_NEVER ? RX_WAIT_NEVER : changedTick + wait;
}

// 刻み tick のセルを評価する。changedTick = セルのブロックが最後に変わった刻み(tick より前。刻みの初めに読んだ値)。
// 取り合いの丸めは今までどおり刻みの乱数(取り合うのは進む規則があるセル = 起きているセルだけ)
template <typename Table>
FX_FN RxWaitStep RxStepCellWait(Table table, RxCell cell, uint64_t worldSeed, uint64_t tick, uint64_t changedTick,
                                uint64_t cellId) {
    FX_ASSERT(changedTick < tick);
    RxWaitStep step;
    step.cell = cell;
    step.thermal = RxComputeThermal(table, cell);

    const uint32_t kelvin = (uint32_t)step.thermal.temperature / (uint32_t)MILLIKELVIN_PER_KELVIN;
    const uint32_t tableKelvin = kelvin < RX_RATE_TABLE_KELVINS ? kelvin : RX_RATE_TABLE_KELVINS - 1;
    const RxWaitCandidates collected = RxCollectCandidatesWait(
        table, cell, tableKelvin, RxWaitSeed(worldSeed, changedTick, cellId), tick - changedTick);

    // --- 進む規則が無い: 待ちの最小の刻みまで変わらない ---
    if (collected.candidates.count == 0) {
        step.wakeTick = RxWakeTickOf(changedTick, collected.wait);

        return step;
    }

    // --- 進む規則がある: 変わればブロックの changedTick が tick になる。変わらなくても次の刻みにまた評価する ---
    const RxCandidates resolved = RxResolveContention(table, cell, step.thermal, collected.candidates,
                                                      RxRandomSeed(worldSeed, tick, cellId));
    step.cell = RxApplyExtents(table, cell, resolved);
    step.thermal = RxComputeThermal(table, step.cell);
    step.wakeTick = tick + 1;

    return step;
}

RX_NAMESPACE_END

#endif  // BICAMERAL_REACTION_HLSLI
