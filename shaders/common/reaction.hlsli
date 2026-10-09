// reaction.hlsli — 1 セルの反応の評価(docs/design/02-reaction-system.md §3。T-0014)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束に従う)。CPU リファレンス(engine/src/sim/reaction.h)と
// GPU のカーネル(shaders/sim/reaction_cells.hlsl)が同じ関数を呼ぶので、同じセル・同じ表からはビット単位で同じ結果になる。
//
// データの流れ:
//   ベイクした表(engine/src/sim/reaction_table.cpp の BakeReactionTable: 物質・規則・索引・速度の表)
//   + セル(成分 = 物質 ID の昇順に並べた物質量、エネルギー)→ RxStepCellWait(待ちの丸め。ADR-0018)→ 次の刻みのセル
//
// 保存則(D-206・04 R2):
//   - 元素: 反応物と生成物は「係数 × 進行度」で整数のまま増減する(ADR-0012)。ベイクが元素の釣り合いを確かめるので、元素の数は完全に保存される。
//   - エネルギー: セルのエネルギーは「熱 + 化学(Σ 物質量 × H0)」の合計。反応は成分を変えるだけで合計を変えない(熱は導出値)。
//
// 表の読み方は呼ぶ側が Table 型で渡す(C++ は配列の span、HLSL はバッファを読む構造体)。Table に要るメソッド:
//   RxSpecies Species(uint32_t id) / RxRule Rule(uint32_t id) / uint32_t RuleIndex(uint32_t position) /
//   uint64_t Rate(uint32_t rule, uint32_t kelvin)
//
// セルの形も呼ぶ側が Cell 型で渡す(T-0175。成分の二段の CPU リファレンス):
//   - RxCell: 成分はインラインの RX_MAX_CELL_SPECIES 個まで(GPU と、今の多重解像度の世界)。超える生成物は待たせる(RX_LIMIT_PRODUCTS)
//   - RxWideCell(C++ だけ。engine/src/sim/reaction_wide_cell.h): 成分の数に上限が無い。生成物は待たずに足す
//   核は成分を species[i]・amounts[i]・speciesCount で読み、形ごとに違う所は下の「セルの形ごとの道具」(同じ名前の関数を形ごとに用意)を通す。
//   成分が RX_MAX_CELL_SPECIES 以下の間は、どちらの形でもビット単位で同じ結果になる
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
FX_CONST uint32_t
    RX_MAX_CELL_SPECIES = 8;  // セルの成分のインラインの数(溢れは RxWideCell〔CPU。T-0175〕・GPU は T-0176 / R-MULTI-4)
FX_CONST uint32_t RX_MAX_REACTANTS = 3;
FX_CONST uint32_t RX_MAX_PRODUCTS = 3;
FX_CONST uint32_t RX_MAX_CANDIDATES = 16;  // 1 セルで同時に進める規則の数(超えたら刻みごとに選ぶ。RX_LIMIT_CANDIDATES)
FX_CONST uint32_t RX_RATE_TABLE_KELVINS = 4096;  // 速度の表は 0〜4095 K を 1 K ごと。それより熱いと 4095 K の値
FX_CONST uint32_t RX_NO_SLOT = 0xFFFFFFFFu;
FX_CONST uint32_t RX_RANDOM_PURPOSE = 0x52780001u;  // 取り合いの丸めの乱数の用途(FxHash64)
FX_CONST uint32_t RX_SELECT_PURPOSE = 0x52780003u;  // 候補が上限を超えた時に選ぶ乱数の用途(FxHash64。T-0022)

// 上限に当たった印(RxWaitStep::limits のビット。T-0022 の最初の段。D-401・D-428・04 R8: 黙って捨てない)。
// どちらも保存則は守る(元素・エネルギーはビット単位で変わらない)。上限そのものを無くすのは T-0022 の続き(成分の溢れの領域・候補の引き方)。
//   RX_LIMIT_PRODUCTS: 生成物を全部足すと成分がインラインの数を超えるので、セルに無い物質を作る規則をこの刻みは進めなかった(待たせた)。
//                      上限の無いセルの形(RxWideCell。T-0175)では立たない
//   RX_LIMIT_CANDIDATES: 進む規則が RX_MAX_CANDIDATES を超えたので、刻みごとの乱数で RX_MAX_CANDIDATES 個を選んで進めた
//                        (選ばれなかった規則はこの刻みは進まない。並びや ID に偏らない)
// 仮(ユーザー未確認。QUESTIONS Q19): 上限に当たった時のふるまい
FX_CONST uint32_t RX_LIMIT_PRODUCTS = 1u;
FX_CONST uint32_t RX_LIMIT_CANDIDATES = 2u;

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

// --- セルの形ごとの道具(T-0175。RxWideCell の版は engine/src/sim/reaction_wide_cell.h に同じ名前で)---
// 核は Cell 型の引数からこれらを呼ぶ。RxCell の版はインラインの数で止まり、何も伸ばさない(GPU のコードは前と同じ)

#ifdef __cplusplus
#define RX_UNUSED(x) (void)(x)
#else
#define RX_UNUSED(x)
#endif

// 成分をもう 1 つ増やせるか(RxCell はインラインの数まで)
FX_FN bool RxHasRoomForSpecies(RxCell cell) {
    return cell.speciesCount < RX_MAX_CELL_SPECIES;
}

// 書く前に、成分の数 + 1 個ぶんの場所を用意する(RxCell は初めからインラインの数だけあるので、そのまま)
FX_FN RxCell RxCellWithRoom(RxCell cell) {
    return cell;
}

// 同じ形の空のセル
FX_FN RxCell RxEmptyCellLike(RxCell cell, int64_t energy) {
    RX_UNUSED(cell);

    return RxMakeEmptyCell(energy);
}

// 物質 ID の位置(無ければ RX_NO_SLOT)
template <typename Cell>
FX_FN uint32_t RxFindSlot(Cell cell, uint32_t speciesId) {
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
template <typename Table, typename Cell>
FX_FN int64_t RxChemicalEnergy(Table table, Cell cell) {
    int64_t chemical = 0;
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        const int64_t term = RxMulS64Checked((int64_t)cell.amounts[i], table.Species(cell.species[i]).h0);
        FX_ASSERT(!FxAddOverflowsS64(chemical, term));
        chemical += term;
    }

    return chemical;
}

// 熱容量(nJ/K)= Σ 物質量 × 比熱
template <typename Table, typename Cell>
FX_FN uint64_t RxHeatCapacity(Table table, Cell cell) {
    uint64_t heatCapacity = 0;
    for (uint32_t i = 0; i < cell.speciesCount; ++i)
        heatCapacity += cell.amounts[i] * (uint64_t)table.Species(cell.species[i]).heatCapacity;

    return heatCapacity;
}

template <typename Table, typename Cell>
FX_FN RxThermal RxComputeThermal(Table table, Cell cell) {
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

// --- 待ちの丸め: 遅い反応は「次に 1 単位進む刻み」を決める(T-0105・D-429。研究)-----------------------
// 今までの丸め(毎刻みの乱数と端数を比べる。T-0130 で消した)は、次に当たる刻みを閉じた式で求められないので、眠らせたまま起こす刻みを決められない。
// 待ちの丸めは、ブロックが最後に変わった刻み changedTick からの待ち n を幾何分布で引く: 進行度 = 整数部 + [刻み ≥ changedTick + n]。
//   - n は P(n = k) = (1 − f)^(k−1) f(f = 1 刻みの進みの端数)。乱数は (世界のシード, changedTick, セル, 規則の鍵) だけで決まり、刻みに依らない。
//   - 幾何分布は記憶が無いので、ブロックが変わるたびに引き直しても平均の速さは偏らない(Gillespie 1977 の 1 単位ずつの確率過程と同じ)。
//     毎刻み変わるセルでは n ≤ 1 ⇔ 確率 f で、今までの丸めと同じ分布になる。
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

// 反応物の量の積 × 仮数 × 2^指数 を、整数部と 64bit の端数に分ける(下限は無い。端数は待ちで丸める)
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

template <typename Table, typename Cell>
FX_FN RxRuleRate RxComputeRuleRate(Table table, Cell cell, uint32_t ruleId, RxRule rule, uint32_t kelvin) {
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

template <typename Table, typename Cell>
FX_FN RxWaitSample RxDesiredExtentWait(Table table, Cell cell, uint32_t ruleId, RxRule rule, uint32_t kelvin,
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

// 待ちの丸めで候補を集める。規則は「ID が最小の反応物」の索引にだけ入っているので、成分を順に見れば重複なく引ける。wait は進めない規則も含めた待ちの最小。
// offered は進む(望む進行度が 0 でない)規則の数。RX_MAX_CANDIDATES を超えたら、優先度(RxSelectPriority)の小さい RX_MAX_CANDIDATES 個を残す
// (T-0022。前は 17 個目以降を捨てていたので、ID の大きい物質の規則だけが系統的に起きなかった)
struct RxWaitCandidates {
    RxCandidates candidates;
    uint64_t wait;
    uint32_t offered;
};

// 候補が上限を超えた時に残す順(小さいほど残る)。刻みとセルごとの種と規則の鍵から作るので、表の並び・規則の ID・集める順に依らない
FX_FN uint64_t RxSelectPriority(uint64_t selectSeed, RxRule rule) {
    return FxHashCombine(selectSeed, rule.key);
}

// 規則 a が b より先に残るか(優先度、同じなら規則の ID)
FX_FN bool RxSelectBefore(uint64_t priorityA, uint32_t ruleA, uint64_t priorityB, uint32_t ruleB) {
    return priorityA < priorityB || (priorityA == priorityB && ruleA < ruleB);
}

// 候補が一杯の時に規則 ruleId(望む進行度 value)を入れる: 残っている中で最も後に回る候補より先なら置き換える。
// 集め終わった時に、全部の進む規則のうち優先度の小さい RX_MAX_CANDIDATES 個が残る(どの順に来ても同じ集合)
template <typename Table>
FX_FN RxCandidates RxReplaceLastCandidate(Table table, RxCandidates candidates, uint32_t ruleId, uint64_t value,
                                          uint64_t selectSeed) {
    uint32_t last = 0;
    uint64_t lastPriority = 0;
    for (uint32_t c = 0; c < RX_MAX_CANDIDATES; ++c) {
        const uint64_t priority = RxSelectPriority(selectSeed, table.Rule(candidates.rules[c]));
        if (c == 0 || RxSelectBefore(lastPriority, candidates.rules[last], priority, candidates.rules[c])) {
            last = c;
            lastPriority = priority;
        }
    }

    if (!RxSelectBefore(RxSelectPriority(selectSeed, table.Rule(ruleId)), ruleId, lastPriority, candidates.rules[last]))
        return candidates;

    candidates.rules[last] = ruleId;
    candidates.extents[last] = value;

    return candidates;
}

template <typename Table, typename Cell>
FX_FN RxWaitCandidates RxCollectCandidatesWait(Table table, Cell cell, uint32_t kelvin, uint64_t waitSeed,
                                               uint64_t elapsed, uint64_t selectSeed) {
    RxWaitCandidates result;
    result.wait = RX_WAIT_NEVER;
    result.offered = 0;
    result.candidates.count = 0;
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

            result.offered += 1;
            if (result.candidates.count >= RX_MAX_CANDIDATES) {
                result.candidates = RxReplaceLastCandidate(table, result.candidates, ruleId, sample.value, selectSeed);
                continue;
            }

            result.candidates.rules[result.candidates.count] = ruleId;
            result.candidates.extents[result.candidates.count] = sample.value;
            result.candidates.count += 1;
        }
    }

    return result;
}

// --- 取り合いの解決(02 §3 の 3。並び順に依存しない)------------------------------------------------

// 規則の候補が使う量の合計(反応物は必ずセルにあるので、成分の位置で持つ)と、吸熱の規則が使う熱。
// セルの形ごとに型が違う(RxWideCell は RxWideUsage)ので、核は RxEmptyUsage(cell) で作ったものを引数で受け取って型を決める
struct RxUsage {
    uint64_t amounts[RX_MAX_CELL_SPECIES];
    uint64_t heat;
};

// 0 の使う量(セルの形の道具。RxCell はインラインの数だけ)
FX_FN RxUsage RxEmptyUsage(RxCell cell) {
    RX_UNUSED(cell);
    RxUsage usage;
    // HLSL には範囲 for が無い
    // NOLINTNEXTLINE(modernize-loop-convert)
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i)
        usage.amounts[i] = 0;

    usage.heat = 0;

    return usage;
}

// 候補が使う量を usage(RxEmptyUsage(cell) の 0 から)に足す
template <typename Table, typename Cell, typename Usage>
FX_FN Usage RxSumUsage(Table table, Cell cell, RxCandidates candidates, Usage usage) {
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

// 縮めるときの乱数(規則ごと)。取り合いの種と規則の鍵のハッシュの上位 32bit(下位 32bit は今までの丸め〔T-0130 で消した〕が使っていた)
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
template <typename Cell, typename Usage>
FX_FN bool RxOverdraws(RxRule rule, Cell cell, Usage used, uint64_t heatAvailable) {
    for (uint32_t i = 0; i < rule.reactantCount; ++i) {
        const uint32_t slot = RxFindSlot(cell, rule.reactants[i]);
        if (used.amounts[slot] > cell.amounts[slot])
            return true;
    }

    return rule.reactionEnthalpy > 0 && used.heat > heatAvailable;
}

// 丸め上げた規則(roundedUp のビット)のせいで足りなくなった資源があれば、その資源を使う丸め上げた規則のうち
// 進行度が最も大きいもの(同じなら鍵が小さいもの)から 1 つずつ丸め上げを戻す。大きい規則ほど 1 の差の割合が小さいので、
// 偏りは主な反応に寄せ、脇の反応の期待値は崩さない。切り捨ての値は必ずある量以下なので、全部戻せば必ず足りる。
// used = resolved の使う量(RxSumUsage。型をセルの形に合わせるため引数で受け取る)
template <typename Table, typename Cell, typename Usage>
FX_FN RxCandidates RxRevokeRoundUps(Table table, Cell cell, RxCandidates resolved, uint32_t roundedUp,
                                    uint64_t heatAvailable, Usage used) {
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
        used = RxSumUsage(table, cell, resolved, RxEmptyUsage(cell));
    }

    return resolved;
}

// 物質ごとに「要求の合計」と「ある量」を比べ、足りない資源を使う規則を min(ある量 ÷ 要求) の比で縮める。
// 吸熱の規則は熱(の 1/8。RX_ENDOTHERMIC_HEAT_SHIFT)も資源として同じく縮める(熱が負にならない)。どの規則も元の進行度に同じ比を掛けるので、並び順は結果に影響しない。
// 縮めた値は確率的に丸める(規則ごとに 1 つの乱数を、足りない資源のどれにも使う。丸めは単調なので、資源ごとに丸めた最小 = 比の最小を丸めた値)。
// 丸め上げで足りなくなった資源があれば RxRevokeRoundUps が戻すので、縮めた後の消費の合計は必ずある量以下
// demand = 候補の使う量の合計(RxSumUsage。型をセルの形に合わせるため引数で受け取る)
template <typename Table, typename Cell, typename Usage>
FX_FN RxCandidates RxShrinkToDemand(Table table, Cell cell, RxThermal thermal, RxCandidates candidates,
                                    uint64_t randomSeed, Usage demand) {
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

    return RxRevokeRoundUps(table, cell, resolved, roundedUp, heatAvailable,
                            RxSumUsage(table, cell, resolved, RxEmptyUsage(cell)));
}

template <typename Table, typename Cell>
FX_FN RxCandidates RxResolveContention(Table table, Cell cell, RxThermal thermal, RxCandidates candidates,
                                       uint64_t randomSeed) {
    return RxShrinkToDemand(table, cell, thermal, candidates, randomSeed,
                            RxSumUsage(table, cell, candidates, RxEmptyUsage(cell)));
}

// --- 成分を更新する ----------------------------------------------------------------------------

// 物質量が 0 になった成分を消して詰める(並びは保つ)
template <typename Cell>
FX_FN Cell RxCompactCell(Cell cell) {
    Cell result = RxEmptyCellLike(cell, cell.energy);
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        if (cell.amounts[i] == 0)
            continue;

        result = RxCellWithRoom(result);
        result.species[result.speciesCount] = cell.species[i];
        result.amounts[result.speciesCount] = cell.amounts[i];
        result.speciesCount += 1;
    }

    return result;
}

// 物質を足した結果。overflowed = 成分がインラインの数を超えるので足せなかった(cell は足す前のまま。RxWideCell では立たない)
template <typename Cell>
struct RxAddedOf {
    Cell cell;
    uint32_t overflowed;
};

typedef RxAddedOf<RxCell> RxAdded;

// 物質を足す(無ければ ID の順の位置に差し込む)。入りきらなければ足さずに overflowed を立てる(呼ぶ側が保存を守る)
template <typename Cell>
FX_FN RxAddedOf<Cell> RxTryAddSpecies(Cell cell, uint32_t speciesId, uint64_t amount) {
    RxAddedOf<Cell> added;
    added.cell = cell;
    added.overflowed = 0;
    if (amount == 0)
        return added;

    uint32_t position = 0;
    while (position < cell.speciesCount && cell.species[position] < speciesId)
        position += 1;

    if (position < cell.speciesCount && cell.species[position] == speciesId) {
        FX_ASSERT(cell.amounts[position] + amount >= amount);
        added.cell.amounts[position] += amount;
        return added;
    }

    if (!RxHasRoomForSpecies(cell)) {
        added.overflowed = 1;
        return added;
    }

    added.cell = RxCellWithRoom(cell);
    for (uint32_t i = cell.speciesCount; i > position; --i) {
        added.cell.species[i] = cell.species[i - 1];
        added.cell.amounts[i] = cell.amounts[i - 1];
    }

    added.cell.species[position] = speciesId;
    added.cell.amounts[position] = amount;
    added.cell.speciesCount += 1;

    return added;
}

// 物質を足す。呼ぶ側が入りきることを保証する所だけで使う(初めのセル・畳んだ値〔成分は 8 つまで〕・影の引き戻し〔親の成分だけ〕)。
// 反応の生成物は RxApplyExtents が入りきるかを確かめてから足す(T-0022)
template <typename Cell>
FX_FN Cell RxAddSpecies(Cell cell, uint32_t speciesId, uint64_t amount) {
    const RxAddedOf<Cell> added = RxTryAddSpecies(cell, speciesId, amount);
    FX_ASSERT(added.overflowed == 0);

    return added.cell;
}

// 決まった進行度で反応物を引き、生成物を足す。エネルギーは変えない(化学のエネルギーが熱に変わるだけ)。
// 生成物が入りきらなければ overflowed を立てる(その時の cell は使わない。RxApplyExtentsHeld が規則を待たせてやり直す)
template <typename Table, typename Cell>
FX_FN RxAddedOf<Cell> RxApplyExtentsChecked(Table table, Cell cell, RxCandidates resolved) {
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
    RxAddedOf<Cell> result;
    result.cell = RxCompactCell(cell);
    result.overflowed = 0;
    for (uint32_t c = 0; c < resolved.count; ++c) {
        const RxRule rule = table.Rule(resolved.rules[c]);
        for (uint32_t i = 0; i < rule.productCount; ++i) {
            const uint64_t amount = resolved.extents[c] * (uint64_t)rule.productCoefficients[i];
            const RxAddedOf<Cell> added = RxTryAddSpecies(result.cell, rule.products[i], amount);
            result.cell = added.cell;
            result.overflowed |= added.overflowed;
        }
    }

    return result;
}

// セル cell に無い物質を作る規則の進行度を 0 にする(この刻みは進めない)。ほかの規則の生成物はどれも cell にある物質なので、
// 反応物が使い切られて消えても足し直す位置があり、成分の数は cell の数を超えない(= 必ず入りきる)
template <typename Table, typename Cell>
FX_FN RxCandidates RxHoldNewSpeciesRules(Table table, Cell cell, RxCandidates resolved) {
    for (uint32_t c = 0; c < resolved.count; ++c) {
        const RxRule rule = table.Rule(resolved.rules[c]);
        bool makesNew = false;
        for (uint32_t i = 0; i < rule.productCount; ++i)
            makesNew = makesNew || RxFindSlot(cell, rule.products[i]) == RX_NO_SLOT;

        if (makesNew)
            resolved.extents[c] = 0;
    }

    return resolved;
}

// 反応物を引き、生成物を足す。生成物が入りきらなければ、セルに無い物質を作る規則をこの刻みは待たせて(進行度 0)やり直す
// (T-0022。前は 9 種目の生成物を黙って捨て、反応物は引いてあったので元素とエネルギーの保存が破れた)。
// 待たせた規則の取り合いの分け前はほかの規則に回さない(使う量が減るだけなので、ある量を超えない)。overflowed = 待たせた
template <typename Table, typename Cell>
FX_FN RxAddedOf<Cell> RxApplyExtentsHeld(Table table, Cell cell, RxCandidates resolved) {
    RxCandidates applying = resolved;
    RxAddedOf<Cell> applied;
    applied.cell = cell;
    applied.overflowed = 0;
    uint32_t held = 0;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        applied = RxApplyExtentsChecked(table, cell, applying);
        if (applied.overflowed == 0)
            break;

        applying = RxHoldNewSpeciesRules(table, cell, applying);
        held = 1;
    }

    FX_ASSERT(applied.overflowed == 0);
    applied.overflowed = held;

    return applied;
}

// --- 1 セルの 1 刻み(02 §3。待ちの丸め。T-0105・D-429)-------------------------------------------------------------------

// 取り合いの丸めの乱数の種は (世界のシード, 刻み, セルの ID) から作る(R6)。規則ごとの乱数は、これに規則の鍵を混ぜる
FX_FN uint64_t RxRandomSeed(uint64_t worldSeed, uint64_t tick, uint64_t cellId) {
    return FxHash64(worldSeed, tick, cellId, RX_RANDOM_PURPOSE);
}

// 候補が上限を超えた時に選ぶ乱数の種(取り合いの種と同じ入力で用途だけ違う。刻みごとに選び直すので、どの規則も平均して同じ割合で進む)
FX_FN uint64_t RxSelectSeed(uint64_t worldSeed, uint64_t tick, uint64_t cellId) {
    return FxHash64(worldSeed, tick, cellId, RX_SELECT_PURPOSE);
}

// 1 刻みの結果: 新しいセル、その熱、変わらなければ次に評価が要る刻み(wakeTick。進めないなら RX_WAIT_NEVER)。
// 刻み tick ≤ t < wakeTick の間にこのセルを評価しても(ブロックが変わらなければ)何も変わらないので、眠らせてよい。
// limits = 上限に当たった印(RX_LIMIT_*。保存は守った上で、進め方を変えた)
template <typename Cell>
struct RxWaitStepOf {
    Cell cell;
    RxThermal thermal;
    uint64_t wakeTick;
    uint32_t limits;
};

typedef RxWaitStepOf<RxCell> RxWaitStep;

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
template <typename Table, typename Cell>
FX_FN RxWaitStepOf<Cell> RxStepCellWait(Table table, Cell cell, uint64_t worldSeed, uint64_t tick, uint64_t changedTick,
                                        uint64_t cellId) {
    FX_ASSERT(changedTick < tick);
    RxWaitStepOf<Cell> step;
    step.cell = cell;
    step.thermal = RxComputeThermal(table, cell);
    step.limits = 0;

    const uint32_t kelvin = (uint32_t)step.thermal.temperature / (uint32_t)MILLIKELVIN_PER_KELVIN;
    const uint32_t tableKelvin = kelvin < RX_RATE_TABLE_KELVINS ? kelvin : RX_RATE_TABLE_KELVINS - 1;
    const RxWaitCandidates collected = RxCollectCandidatesWait(
        table, cell, tableKelvin, RxWaitSeed(worldSeed, changedTick, cellId), tick - changedTick,
        RxSelectSeed(worldSeed, tick, cellId));
    step.limits |= collected.offered > RX_MAX_CANDIDATES ? RX_LIMIT_CANDIDATES : 0u;

    // --- 進む規則が無い: 待ちの最小の刻みまで変わらない ---
    if (collected.candidates.count == 0) {
        step.wakeTick = RxWakeTickOf(changedTick, collected.wait);

        return step;
    }

    // --- 進む規則がある: 変わればブロックの changedTick が tick になる。変わらなくても次の刻みにまた評価する ---
    const RxCandidates resolved = RxResolveContention(table, cell, step.thermal, collected.candidates,
                                                      RxRandomSeed(worldSeed, tick, cellId));
    const RxAddedOf<Cell> applied = RxApplyExtentsHeld(table, cell, resolved);
    step.cell = applied.cell;
    step.limits |= applied.overflowed != 0 ? RX_LIMIT_PRODUCTS : 0u;
    step.thermal = RxComputeThermal(table, step.cell);
    step.wakeTick = tick + 1;

    return step;
}

// --- 1 セルだけのブロック(反応の試験の CPU リファレンスと shaders/sim/reaction_cells.hlsl。T-0130)-----------------

// 1 セルだけのブロック: セルと、それが最後に変わった刻み(待ちの丸めの changedTick)と、上限に当たった刻みの数(RX_LIMIT_* ごと)
struct RxLoneCell {
    RxCell cell;
    uint64_t changedTick;
    uint32_t productsHeldTicks;     // RX_LIMIT_PRODUCTS の刻みの数
    uint32_t candidatesLimitTicks;  // RX_LIMIT_CANDIDATES の刻みの数
};

FX_FN RxLoneCell RxMakeLoneCell(RxCell cell, uint64_t changedTick) {
    RxLoneCell lone;
    lone.cell = cell;
    lone.changedTick = changedTick;
    lone.productsHeldTicks = 0;
    lone.candidatesLimitTicks = 0;

    return lone;
}

// 2 つのセルがビット単位で同じか(padding は見ない)
FX_FN bool RxSameCell(RxCell a, RxCell b) {
    bool same = a.energy == b.energy && a.speciesCount == b.speciesCount;
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i)
        same = same && a.species[i] == b.species[i] && a.amounts[i] == b.amounts[i];

    return same;
}

// 刻み tickBegin から tickCount 刻み、待ちの丸めで進める(tickBegin > changedTick)。セルが変わった刻みを changedTick にする
template <typename Table>
FX_FN RxLoneCell RxAdvanceLoneCell(Table table, RxLoneCell lone, uint64_t worldSeed, uint64_t tickBegin,
                                   uint32_t tickCount, uint64_t cellId) {
    for (uint32_t i = 0; i < tickCount; ++i) {
        const uint64_t tick = tickBegin + i;
        const RxWaitStep step = RxStepCellWait(table, lone.cell, worldSeed, tick, lone.changedTick, cellId);
        if (!RxSameCell(step.cell, lone.cell))
            lone.changedTick = tick;

        lone.cell = step.cell;
        lone.productsHeldTicks += (step.limits & RX_LIMIT_PRODUCTS) != 0 ? 1u : 0u;
        lone.candidatesLimitTicks += (step.limits & RX_LIMIT_CANDIDATES) != 0 ? 1u : 0u;
    }

    return lone;
}

RX_NAMESPACE_END

#endif  // BICAMERAL_REACTION_HLSLI
