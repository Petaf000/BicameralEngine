// magic_patch.hlsli — 魔法のパッチ(反応則への介入。docs/design/02-reaction-system.md §5・T-0024・ADR-0095)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU リファレンス(engine/src/sim/multires_nest.cpp の刻み)と
// GPU(shaders/sim/multires_patches.hlsli を通る多重解像度の刻み)が同じ関数を呼ぶので、パッチの効いた刻みもビット単位で同じになる。
//
// データの流れ:
//   コマンド(実験室なら LAB_COMMAND_TYPE_PATCH。06 §3。再生ファイルに入る)→ MpAddPatch がパッチの列(世界の状態。GPU は u5、CPU は
//   MultiresNest::magicPatches。どちらも同じ uint32 の語の並び)に足す → 刻みの反応の評価(MrStepCellWait)が、セルを覆うパッチを
//   MpPatchTable で表に重ねる(MpPatchedTable の Rate と、転換なら Rule が変わる)→ 反応の核(reaction.hlsli)はそのまま。
//
// 効き方(今あるもの): 規則 1 つの速さを 2^shift 倍にする(増幅・弱める)/ 止める(阻害)/ 活性化エネルギーを下げる(触媒。T-0288)/
//   生成物を替える(転換。T-0359)。
//   速さの表の値は「仮数 × 2^指数」なので、指数に shift を足すだけで、どの機械でも同じ整数の結果になる。
//   触媒は k = A·exp(−(Ea − ΔEa) / RT) = 表の値 × exp(ΔEa / RT)。倍率が温度で変わる(冷たいほど大きく効く)ので、
//   プレイヤーが温度を変えて測ると「2^shift 倍」(どの温度でも同じ倍率)と区別できる。ΔEa / RT はベイク(reaction_table.cpp の
//   BakeRate)と同じ整数の計算で、読む温度ごとにその場で求める(表を作り直さない。ADR-0095 追記)。下げる量は規則の Ea が上限(Ea' ≥ 0)。
//   **保存則は変えない**(D-428・D-419): 変わるのは「1 刻みにどれだけ進みたいか」だけで、進んだ分は反応の核が係数 × 進行度で
//   成分を動かす(元素の数とエネルギーはビット単位で保存される)。
// 転換(T-0359): 規則の生成物 1 つを、元素が釣り合う別の物質に替える(Rule が変わる。釣り合わない転換は足す時に断り、効かせる時も
//   効かない)。反応熱は替えた後の生成物の H0 で求め直すので、エネルギーも保存される。湧き出しと吸い込みは後のチケット。
// 規則は名前の鍵(RxRule::key)で持つ(T-0287): 規則の並びが変わる表の差し替え(T-0218・T-0242)でも、同じ名前の規則に効き続ける。
// 範囲と期間: 範囲はレベル 0(0.5 m)のセルの単位の直方体(両端を含む)。セルがその直方体と重なれば効く(粗いセルは丸ごと効く)。
//   期間は [始まりの刻み, 終わりの刻み)。速さが変わる刻み(始まりと終わり)に、範囲に重なるブロックを「つつく」(ADR-0018:
//   速さを変えるものは tc を書いてから評価する。待ちを古い乱数のまま新しい速さで使うと偏る)。
#ifndef BICAMERAL_MAGIC_PATCH_HLSLI
#define BICAMERAL_MAGIC_PATCH_HLSLI

#include "reaction.hlsli"

RX_NAMESPACE_BEGIN

// --- 列の形(uint32 の語。[0] = パッチの数・[1..3] = 予約(0)・その後に MP_PATCH_WORDS 語ずつ)-----------------
FX_CONST uint32_t MP_MAX_PATCHES = 16;
FX_CONST uint32_t MP_LIST_HEADER_WORDS = 4;
FX_CONST uint32_t MP_PATCH_WORDS = 13;
FX_CONST uint32_t MP_LIST_WORDS = MP_LIST_HEADER_WORDS + (MP_MAX_PATCHES * MP_PATCH_WORDS);

// パッチの語(列の中の 1 つ分)
FX_CONST uint32_t
    MP_WORD_RULE_KEY = 0;  // 規則の名前の鍵(RxRule::key。下位・上位。T-0287: ID で持つと表の差し替えでずれる)
FX_CONST uint32_t MP_WORD_EFFECT = 2;
FX_CONST uint32_t MP_WORD_START = 3;  // 始まりの刻み(下位・上位)
FX_CONST uint32_t MP_WORD_END = 5;    // 終わりの刻み(この刻みからは効かない。下位・上位)
FX_CONST uint32_t MP_WORD_LOW = 7;    // 小さい角 x, y, z(int32。レベル 0 のセルの単位)
FX_CONST uint32_t MP_WORD_HIGH = 10;  // 大きい角 x, y, z(両端を含む)

// コマンドの payload の語(実験室と、後で世界のコマンドが同じ並びを使う。始まりの刻みはコマンドの targetTick)
FX_CONST uint32_t MP_COMMAND_RULE_KEY = 0;  // 規則の名前の鍵(下位・上位)
FX_CONST uint32_t MP_COMMAND_EFFECT = 2;
FX_CONST uint32_t MP_COMMAND_DURATION = 3;  // 期間(刻み。1 以上)
FX_CONST uint32_t MP_COMMAND_LOW = 4;       // 小さい角 x, y, z
FX_CONST uint32_t MP_COMMAND_HIGH = 7;      // 大きい角 x, y, z
FX_CONST uint32_t MP_COMMAND_WORDS = 10;

// 効き方の語: 下位 8bit = 種類、8〜15bit = shift(符号付き 8bit。MP_KIND_SCALE だけ)、
// 8〜31bit = 下げる活性化エネルギー(J/mol。符号なし 24bit。MP_KIND_CATALYST だけ)、
// 8〜9bit = 生成物の欄・10〜31bit = 替える先の物質 ID(MP_KIND_CONVERT だけ)
FX_CONST uint32_t MP_KIND_SCALE = 1;                   // 速さを 2^shift 倍(増幅・弱める)
FX_CONST uint32_t MP_KIND_STOP = 2;                    // 止める(阻害)
FX_CONST uint32_t MP_KIND_CATALYST = 3;                // 活性化エネルギーを下げる(触媒。T-0288)
FX_CONST uint32_t MP_KIND_CONVERT = 4;                 // 生成物 1 つを別の物質に替える(転換。T-0359)
FX_CONST uint32_t MP_MAX_CONVERT_SPECIES = 0x3FFFFFu;  // 替える先の物質 ID の上限(22bit)
FX_CONST uint32_t MP_MAX_CONVERT_FACTOR = 0xFFFFu;     // 転換で掛け合わせる係数・公約数の上限(積が 32bit に収まる)
FX_CONST uint32_t MP_MAX_CATALYST_JOULES_PER_MOL = 0xFFFFFFu;  // 1 つのパッチが下げる量の上限(24bit。約 16.8 MJ/mol)
FX_CONST int32_t MP_MAX_SHIFT = 64;                            // |shift| の上限(16 個重ねても指数は int32 に十分収まる)
FX_CONST int32_t MP_MAX_LEVEL_SHIFT = 40;  // 範囲の比べ方で使うレベルの差の上限(64bit の座標があふれない)

// パッチ 1 つ
struct MpPatch {
    uint64_t ruleKey;  // RxRule::key(規則の名前のハッシュ)。表を替えても同じ名前の規則に効く
    uint32_t effect;
    uint64_t startTick;
    uint64_t endTick;
    int32_t lowX;
    int32_t lowY;
    int32_t lowZ;
    int32_t highX;
    int32_t highY;
    int32_t highZ;
};

// --- 効き方 ------------------------------------------------------------------------------------

FX_FN uint32_t MpMakeScaleEffect(int32_t shift) {
    return MP_KIND_SCALE | (((uint32_t)shift & 0xFFu) << 8);
}

FX_FN uint32_t MpMakeStopEffect() {
    return MP_KIND_STOP;
}

// 活性化エネルギーを loweredJoulesPerMol(J/mol。1〜MP_MAX_CATALYST_JOULES_PER_MOL)下げる
FX_FN uint32_t MpMakeCatalystEffect(uint32_t loweredJoulesPerMol) {
    return MP_KIND_CATALYST | ((loweredJoulesPerMol & MP_MAX_CATALYST_JOULES_PER_MOL) << 8);
}

// 規則の生成物の欄 productSlot(0〜RX_MAX_PRODUCTS − 1)の物質を物質 species に替える。係数は元素の数が同じになるように
// 求め直す(MpConvertedCoefficient)。物質 ID は今の表の ID(表を替えた後は、その表で釣り合う時だけ効く)
FX_FN uint32_t MpMakeConvertEffect(uint32_t productSlot, uint32_t species) {
    return MP_KIND_CONVERT | ((productSlot & 0x3u) << 8) | ((species & MP_MAX_CONVERT_SPECIES) << 10);
}

FX_FN uint32_t MpEffectKind(uint32_t effect) {
    return effect & 0xFFu;
}

// 8〜15bit を符号付きで読む
FX_FN int32_t MpEffectShift(uint32_t effect) {
    return ((int32_t)(effect << 16)) >> 24;
}

// 8〜31bit を下げる活性化エネルギー(J/mol)として読む
FX_FN uint32_t MpEffectCatalystJoulesPerMol(uint32_t effect) {
    return effect >> 8;
}

// 8〜9bit を生成物の欄、10〜31bit を替える先の物質 ID として読む
FX_FN uint32_t MpEffectConvertSlot(uint32_t effect) {
    return (effect >> 8) & 0x3u;
}

FX_FN uint32_t MpEffectConvertSpecies(uint32_t effect) {
    return effect >> 10;
}

FX_FN bool MpEffectValid(uint32_t effect) {
    const uint32_t kind = MpEffectKind(effect);
    if (kind == MP_KIND_CATALYST)
        return MpEffectCatalystJoulesPerMol(effect) != 0;

    if (kind == MP_KIND_CONVERT)
        return MpEffectConvertSlot(effect) < RX_MAX_PRODUCTS && MpEffectConvertSpecies(effect) != 0;

    if ((effect >> 16) != 0)
        return false;

    if (kind == MP_KIND_STOP)
        return (effect >> 8) == 0;

    const int32_t shift = MpEffectShift(effect);

    return kind == MP_KIND_SCALE && shift >= -MP_MAX_SHIFT && shift <= MP_MAX_SHIFT;
}

// 速さの表の値(仮数 << 32 | 指数)を 2^shift 倍にする(仮数 0 = 進まない、はそのまま)
FX_FN uint64_t MpScaleRate(uint64_t packed, int32_t shift) {
    const uint64_t mantissa = RxRateMantissa(packed);
    if (mantissa == 0 || shift == 0)
        return packed;

    const int32_t exponent = RxRateExponent(packed) + shift;

    return (mantissa << 32) | (uint64_t)(uint32_t)exponent;
}

// 触媒: 速さの表の値(温度 kelvin K)を exp(ΔEa / RT) 倍にする(ΔEa = loweredJoulesPerMol J/mol。規則の Ea 以下に抑えてから渡す)。
// ΔEa / RT はベイク(reaction_table.cpp の BakeRate)と同じ式(nJ にして Q32 で割り、log2 e を掛ける)。log2 の整数の部分は指数へ、
// 端数は 2^端数(Q31)を仮数に掛けて 32bit に正規化する(どれも切り捨て。CPU と GPU で同じ整数)。仮数 0・0 K はそのまま
FX_CONST uint64_t MP_GAS_CONSTANT_NANOJOULES = FX_U64(0x1u, 0xEF94A19Au);  // 8.314462618 J/(mol·K) を nJ で
FX_CONST uint64_t MP_NANOJOULES_PER_JOULE = 1000000000u;

FX_FN uint64_t MpCatalyzeRate(uint64_t packed, uint32_t loweredJoulesPerMol, uint32_t kelvin) {
    const uint64_t mantissa = RxRateMantissa(packed);
    if (mantissa == 0 || loweredJoulesPerMol == 0 || kelvin == 0)
        return packed;

    // --- log2 の倍率(Q32。0 以上)= ΔEa / RT × log2 e ---
    const uint64_t lowered = (uint64_t)loweredJoulesPerMol * MP_NANOJOULES_PER_JOULE;  // < 2^62
    FxU128 numerator;
    numerator.hi = lowered >> 32;
    numerator.lo = lowered << 32;
    const int64_t loweredOverRt = (int64_t)FxDivU128By64(numerator, MP_GAS_CONSTANT_NANOJOULES * (uint64_t)kelvin)
                                      .quotient;
    const int64_t log2Factor = FxMulShiftS64(loweredOverRt, (int64_t)FX_LOG2E_Q62, 62);

    // --- 仮数 × 2^端数(Q31)→ 2^31〜2^33 未満。32bit に正規化 ---
    const uint64_t fractionFactor = FxExp2Q32((int64_t)((uint64_t)log2Factor & FX_LOW32_MASK), 31);
    uint64_t scaled = (mantissa * fractionFactor) >> 31;
    int64_t exponent = (int64_t)RxRateExponent(packed) + (log2Factor >> 32);
    if ((scaled >> 32) != 0) {
        scaled >>= 1;
        exponent += 1;
    }

    return (scaled << 32) | (uint64_t)(uint32_t)(int32_t)exponent;
}

// --- 転換(生成物の差し替え。T-0359)--------------------------------------------------------------
// 元素の釣り合い(02 §2 の 1・D-428): 替える前の c 個の物質 a と、替えた後の d 個の物質 b の元素が全部同じ時だけ替える。
// 物質の組成の型(RxSpecies::elementClass。原子の比)が同じで c × 公約数(a) = d × 公約数(b) になる整数 d があれば釣り合う
// (例: 2 O3 → 3 O2。ベイクが型を付ける。reaction_table.cpp の BakeElementClasses)。足す時(コマンド)にも、効かせる時
// (表を替えた後かもしれない)にも同じ関数で確かめるので、CPU と GPU で同じ判定になり、釣り合わない転換はどこでも効かない。
// 反応熱は替えた後の生成物の H0 で求め直す(ベイクの ReactionEnthalpy と同じ式。エネルギーの保存。D-428)。
// 速さ(と触媒の Ea)は変えない: 進む量は同じで、出てくる物質と熱が変わる。

// 表の物質の数(0 番を含む)。ベイクが物質 0(使わない)の elementUnits に置く。GPU でルートの SRV の大きさを読まないため
template <typename Table>
FX_FN uint32_t MpSpeciesCount(Table table) {
    return table.Species(0).elementUnits;
}

// 生成物の欄 1 つを替えた結果: 係数(0 = 替えない)と、反応熱の変わる分(替えた後の係数 × H0 − 替える前の係数 × H0。J/mol)
struct MpSlotConversion {
    uint32_t coefficient;
    int64_t enthalpyChange;
};

// 規則 rule の生成物の欄 slot を物質 species に替えた時の係数(元素が釣り合わなければ 0 = 替えない)と反応熱の変わる分。
// 替える先は今の表にある物質で、元素を持ち、規則の反応物でないこと(反応物と同じ物を作ると、何も変わらない輪になる)
template <typename Table>
FX_FN MpSlotConversion MpConvertSlot(Table table, RxRule rule, uint32_t slot, uint32_t species) {
    MpSlotConversion result;
    result.coefficient = 0;
    result.enthalpyChange = 0;
    if (slot >= rule.productCount || species == 0 || species >= MpSpeciesCount(table))
        return result;

    for (uint32_t i = 0; i < rule.reactantCount; ++i) {
        if (rule.reactants[i] == species)
            return result;
    }

    // 欄 slot を決まった数の回しで選ぶ(動的な添字を使わない。GPU でローカルの配列にしない。T-0397)
    uint32_t beforeId = 0;
    uint32_t coefficient = 0;
    FX_UNROLL for (uint32_t p = 0; p < RX_MAX_PRODUCTS; ++p) {
        if (p == slot) {
            beforeId = rule.products[p];
            coefficient = rule.productCoefficients[p];
        }
    }

    const RxSpecies before = table.Species(beforeId);
    const RxSpecies after = table.Species(species);
    const bool sameClass = before.elementClass != 0 && before.elementClass == after.elementClass;
    const bool bounded = coefficient <= MP_MAX_CONVERT_FACTOR && before.elementUnits <= MP_MAX_CONVERT_FACTOR &&
                         after.elementUnits != 0;
    if (!sameClass || !bounded)
        return result;

    const uint32_t units = coefficient * before.elementUnits;
    if (units % after.elementUnits != 0)
        return result;

    result.coefficient = units / after.elementUnits;
    result.enthalpyChange = ((int64_t)result.coefficient * after.h0) - ((int64_t)coefficient * before.h0);

    return result;
}

template <typename Table>
FX_FN uint32_t MpConvertedCoefficient(Table table, RxRule rule, uint32_t slot, uint32_t species) {
    return MpConvertSlot(table, rule, slot, species).coefficient;
}

// 反応熱(進行度 1 µmol あたりの µJ)= Σ 生成物の H0 − Σ 反応物の H0(ベイクの ReactionEnthalpy と同じ式)
template <typename Table>
FX_FN int64_t MpRuleEnthalpy(Table table, RxRule rule) {
    int64_t enthalpy = 0;
    for (uint32_t i = 0; i < rule.productCount; ++i)
        enthalpy += (int64_t)rule.productCoefficients[i] * table.Species(rule.products[i]).h0;

    for (uint32_t i = 0; i < rule.reactantCount; ++i)
        enthalpy -= (int64_t)rule.reactantCoefficients[i] * table.Species(rule.reactants[i]).h0;

    return enthalpy;
}

// --- パッチの検査 ------------------------------------------------------------------------------

// 表で名前の鍵 key の規則の ID(無ければ ruleCount。ruleCount = 表の規則の数。足す時だけ呼ぶ)
template <typename Table>
FX_FN uint32_t MpFindRule(Table table, uint64_t key, uint32_t ruleCount) {
    for (uint32_t rule = 0; rule < ruleCount; ++rule) {
        if (table.Rule(rule).key == key)
            return rule;
    }

    return ruleCount;
}

// 表に名前の鍵 key の規則があるか
template <typename Table>
FX_FN bool MpRuleKnown(Table table, uint64_t key, uint32_t ruleCount) {
    return MpFindRule(table, key, ruleCount) < ruleCount;
}

// 当てられるか(規則が表にある・効き方が正しい・期間が 1 刻み以上・角の順・転換なら今の表で元素が釣り合う)。
// ruleCount = 表の規則の数
template <typename Table>
FX_FN bool MpPatchValid(Table table, MpPatch patch, uint32_t ruleCount) {
    const bool ordered = patch.lowX <= patch.highX && patch.lowY <= patch.highY && patch.lowZ <= patch.highZ;
    if (!MpEffectValid(patch.effect) || patch.startTick >= patch.endTick || !ordered)
        return false;

    const uint32_t rule = MpFindRule(table, patch.ruleKey, ruleCount);
    if (rule >= ruleCount)
        return false;

    if (MpEffectKind(patch.effect) != MP_KIND_CONVERT)
        return true;

    return MpConvertedCoefficient(table, table.Rule(rule), MpEffectConvertSlot(patch.effect),
                                  MpEffectConvertSpecies(patch.effect)) != 0;
}

// 刻み tick に効いているか
FX_FN bool MpActiveAt(MpPatch patch, uint64_t tick) {
    return patch.startTick <= tick && tick < patch.endTick;
}

// 刻み tick に速さが変わるか(始まりか終わり。範囲のブロックをつつく刻み)
FX_FN bool MpBoundaryAt(MpPatch patch, uint64_t tick) {
    return patch.startTick == tick || patch.endTick == tick;
}

// --- 範囲 --------------------------------------------------------------------------------------

// レベル level の座標 c のセルが覆う、レベル 0 のセルの範囲の端(high = false なら小さい端)。
// 細かいレベル(level > 0)のセルは、それを含むレベル 0 のセル(算術シフト = 下への切り捨て)
FX_FN int64_t MpLevelZeroEdge(int32_t level, int64_t c, bool high) {
    if (level >= 0) {
        const int32_t shift = level < MP_MAX_LEVEL_SHIFT ? level : MP_MAX_LEVEL_SHIFT;
        return c >> (uint32_t)shift;
    }

    const int32_t up = -level < MP_MAX_LEVEL_SHIFT ? -level : MP_MAX_LEVEL_SHIFT;
    const int64_t low = c * ((int64_t)1 << (uint32_t)up);

    return high ? low + ((int64_t)1 << (uint32_t)up) - 1 : low;
}

// 1 軸: レベル level の座標 [first, last] のセルたちが、レベル 0 の [low, high] と重なるか
FX_FN bool MpAxisOverlaps(int32_t level, int64_t first, int64_t last, int32_t low, int32_t high) {
    return MpLevelZeroEdge(level, first, false) <= (int64_t)high && MpLevelZeroEdge(level, last, true) >= (int64_t)low;
}

// レベル level の直方体 [first, last](各軸)がパッチの範囲と重なるか(セル 1 つなら first = last)
FX_FN bool MpOverlaps(MpPatch patch, int32_t level, int64_t firstX, int64_t firstY, int64_t firstZ, int64_t lastX,
                      int64_t lastY, int64_t lastZ) {
    return MpAxisOverlaps(level, firstX, lastX, patch.lowX, patch.highX) &&
           MpAxisOverlaps(level, firstY, lastY, patch.lowY, patch.highY) &&
           MpAxisOverlaps(level, firstZ, lastZ, patch.lowZ, patch.highZ);
}

// --- 列の読み書き(List は PatchWord(word)〔と書くなら SetPatchWord(word, value)〕を持つ型。C++ は span、HLSL はバッファ)---

template <typename List>
FX_FN uint32_t MpPatchCount(List list) {
    const uint32_t count = list.PatchWord(0);
    return count < MP_MAX_PATCHES ? count : MP_MAX_PATCHES;
}

template <typename List>
FX_FN MpPatch MpLoadPatch(List list, uint32_t index) {
    const uint32_t base = MP_LIST_HEADER_WORDS + (index * MP_PATCH_WORDS);
    MpPatch patch;
    patch.ruleKey = FX_U64(list.PatchWord(base + MP_WORD_RULE_KEY + 1), list.PatchWord(base + MP_WORD_RULE_KEY));
    patch.effect = list.PatchWord(base + MP_WORD_EFFECT);
    patch.startTick = FX_U64(list.PatchWord(base + MP_WORD_START + 1), list.PatchWord(base + MP_WORD_START));
    patch.endTick = FX_U64(list.PatchWord(base + MP_WORD_END + 1), list.PatchWord(base + MP_WORD_END));
    patch.lowX = (int32_t)list.PatchWord(base + MP_WORD_LOW);
    patch.lowY = (int32_t)list.PatchWord(base + MP_WORD_LOW + 1);
    patch.lowZ = (int32_t)list.PatchWord(base + MP_WORD_LOW + 2);
    patch.highX = (int32_t)list.PatchWord(base + MP_WORD_HIGH);
    patch.highY = (int32_t)list.PatchWord(base + MP_WORD_HIGH + 1);
    patch.highZ = (int32_t)list.PatchWord(base + MP_WORD_HIGH + 2);

    return patch;
}

template <typename List>
FX_FN void MpStorePatch(List list, uint32_t index, MpPatch patch) {
    const uint32_t base = MP_LIST_HEADER_WORDS + (index * MP_PATCH_WORDS);
    list.SetPatchWord(base + MP_WORD_RULE_KEY, (uint32_t)(patch.ruleKey & FX_LOW32_MASK));
    list.SetPatchWord(base + MP_WORD_RULE_KEY + 1, (uint32_t)(patch.ruleKey >> 32));
    list.SetPatchWord(base + MP_WORD_EFFECT, patch.effect);
    list.SetPatchWord(base + MP_WORD_START, (uint32_t)(patch.startTick & FX_LOW32_MASK));
    list.SetPatchWord(base + MP_WORD_START + 1, (uint32_t)(patch.startTick >> 32));
    list.SetPatchWord(base + MP_WORD_END, (uint32_t)(patch.endTick & FX_LOW32_MASK));
    list.SetPatchWord(base + MP_WORD_END + 1, (uint32_t)(patch.endTick >> 32));
    list.SetPatchWord(base + MP_WORD_LOW, (uint32_t)patch.lowX);
    list.SetPatchWord(base + MP_WORD_LOW + 1, (uint32_t)patch.lowY);
    list.SetPatchWord(base + MP_WORD_LOW + 2, (uint32_t)patch.lowZ);
    list.SetPatchWord(base + MP_WORD_HIGH, (uint32_t)patch.highX);
    list.SetPatchWord(base + MP_WORD_HIGH + 1, (uint32_t)patch.highY);
    list.SetPatchWord(base + MP_WORD_HIGH + 2, (uint32_t)patch.highZ);
}

// 終わった(終わりの刻み ≤ tick)パッチを列から除く。残りの順は変えない(CPU と GPU で同じ並び)。
// 終わりの刻みのつつきは、この前に済ませておく(MpBoundaryAt)
template <typename List>
FX_FN void MpRetireExpired(List list, uint64_t tick) {
    const uint32_t count = MpPatchCount(list);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const MpPatch patch = MpLoadPatch(list, i);
        if (patch.endTick <= tick)
            continue;

        if (kept != i)
            MpStorePatch(list, kept, patch);

        kept += 1;
    }

    list.SetPatchWord(0, kept);
}

// パッチを足す(先に終わったものを除く)。列が一杯・当てられないパッチなら足さずに false
// (どちらも CPU と GPU で同じ判定なので、断った結果も一致する。断ったことは呼ぶ側が数える)。table は今の反応表
template <typename List, typename Table>
FX_FN bool MpAddPatch(List list, Table table, MpPatch patch, uint64_t tick, uint32_t ruleCount) {
    MpRetireExpired(list, tick);
    const uint32_t count = MpPatchCount(list);
    if (count >= MP_MAX_PATCHES || !MpPatchValid(table, patch, ruleCount))
        return false;

    MpStorePatch(list, count, patch);
    list.SetPatchWord(0, count + 1);

    return true;
}

// --- 物質の付け替え(物質を足す・消す反応表の差し替え。ADR-0065・T-0377)-----------------------------------

// 効き方の語の物質 ID を新しい表の ID に付け替える(転換の替える先だけ。ほかの効き方の語はそのまま)。
// remap は common/species_remap.hlsli の Remap の約束(NewId・SpeciesCount)。替える先が消えた(と、今の表の範囲の外の)転換は
// 替える先 0 = 「替える先が無い」にする: 効かせる時は MpConvertedCoefficient が 0 を返すので効かず、足す時は MpEffectValid が断る
template <typename Remap>
FX_FN uint32_t MpRemapEffect(Remap remap, uint32_t effect) {
    if (MpEffectKind(effect) != MP_KIND_CONVERT)
        return effect;

    const uint32_t species = MpEffectConvertSpecies(effect);
    uint32_t remapped = 0;
    if (species != 0 && species < remap.SpeciesCount())
        remapped = remap.NewId(species);

    return MpMakeConvertEffect(MpEffectConvertSlot(effect), remapped);
}

// パッチの列の転換の替える先を新しい表の ID に付け替える(列の順・ほかの語はそのまま。表を差し替える刻み tick の始め、
// 終わったパッチを除く前に呼ぶ)。替える先が消えて効かなくなった転換の数を返す(呼ぶ側が報告する。tick より前に終わった
// パッチ〔この刻みに除かれる〕と、もう替える先が無かった転換は数えない)。GPU(実験室の RemapLabSpecies・仮の世界の RemapSpecies の
// スレッド 0)と CPU リファレンス(sim/lab_box.cpp の RemapLabBox・ProbeReference::RemapSpecies)が同じ関数を呼ぶ
template <typename List, typename Remap>
FX_FN uint32_t MpRemapPatchList(List list, Remap remap, uint64_t tick) {
    const uint32_t count = MpPatchCount(list);
    uint32_t disabled = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t base = MP_LIST_HEADER_WORDS + (i * MP_PATCH_WORDS);
        const uint32_t effect = list.PatchWord(base + MP_WORD_EFFECT);
        if (MpEffectKind(effect) != MP_KIND_CONVERT)
            continue;

        const uint32_t remapped = MpRemapEffect(remap, effect);
        const uint64_t endTick = FX_U64(list.PatchWord(base + MP_WORD_END + 1), list.PatchWord(base + MP_WORD_END));
        const bool lost = MpEffectConvertSpecies(effect) != 0 && MpEffectConvertSpecies(remapped) == 0;
        if (lost && endTick > tick)
            disabled += 1;

        list.SetPatchWord(base + MP_WORD_EFFECT, remapped);
    }

    return disabled;
}

// コマンドの payload の語(MP_COMMAND_*)からパッチを作る。始まり = 当てる刻み tick、終わり = tick + 期間(あふれは当てない印 0)
FX_FN MpPatch MpPatchFromCommand(uint64_t ruleKey, uint32_t effect, uint32_t duration, int32_t lowX, int32_t lowY,
                                 int32_t lowZ, int32_t highX, int32_t highY, int32_t highZ, uint64_t tick) {
    MpPatch patch;
    patch.ruleKey = ruleKey;
    patch.effect = effect;
    patch.startTick = tick;
    patch.endTick = tick + (uint64_t)duration;
    patch.lowX = lowX;
    patch.lowY = lowY;
    patch.lowZ = lowZ;
    patch.highX = highX;
    patch.highY = highY;
    patch.highZ = highZ;

    return patch;
}

// 列の要約(状態のハッシュに混ぜる。T-0323)。列が空なら 0(今までの記録のハッシュが変わらない)。効いているパッチの語だけを順に混ぜる
// (並びは CPU と GPU で同じ。MpRetireExpired)。仮の世界(ProbeSim)は状態の要約がセルの和なので、これを足す
template <typename List>
FX_FN uint64_t MpListHash(List list) {
    const uint32_t count = MpPatchCount(list);
    if (count == 0)
        return 0;

    uint64_t hash = FxHashCombine(FX_U64(0x4D414749u, 0x43504154u), (uint64_t)count);
    const uint32_t used = MP_LIST_HEADER_WORDS + (count * MP_PATCH_WORDS);
    for (uint32_t word = MP_LIST_HEADER_WORDS; word < used; ++word)
        hash = FxHashCombine(hash, (uint64_t)list.PatchWord(word));

    return hash;
}

// --- 表に重ねる --------------------------------------------------------------------------------

// レベル level・座標 (x, y, z) のセルを刻み tick に覆うパッチの印(ビット i = 列の i 番目)
template <typename List>
FX_FN uint32_t MpCoveringMask(List list, int32_t level, int64_t x, int64_t y, int64_t z, uint64_t tick) {
    const uint32_t count = MpPatchCount(list);
    uint32_t mask = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const MpPatch patch = MpLoadPatch(list, i);
        if (MpActiveAt(patch, tick) && MpOverlaps(patch, level, x, y, z, x, y, z))
            mask |= 1u << i;
    }

    return mask;
}

// 覆うパッチ(mask)のうち規則 rule(名前の鍵で比べる)のものを、温度 kelvin の速さの表の値 packed に重ねる。
// 止めるものが 1 つでもあれば 0。ほかは触媒の下げる量の和(規則の Ea が上限)で exp(ΔEa / RT) 倍してから 2^(shift の和) 倍
// (重ねる順に依らない)
template <typename List>
FX_FN uint64_t MpPatchedRate(List list, uint32_t mask, RxRule rule, uint32_t kelvin, uint64_t packed) {
    int32_t shift = 0;
    uint32_t lowered = 0;  // 16 個 × 2^24 未満 = 2^28 未満
    for (uint32_t i = 0; i < MP_MAX_PATCHES; ++i) {
        if (((mask >> i) & 1u) == 0)
            continue;

        const uint32_t base = MP_LIST_HEADER_WORDS + (i * MP_PATCH_WORDS);
        const uint64_t key = FX_U64(list.PatchWord(base + MP_WORD_RULE_KEY + 1),
                                    list.PatchWord(base + MP_WORD_RULE_KEY));
        if (key != rule.key)
            continue;

        const uint32_t effect = list.PatchWord(base + MP_WORD_EFFECT);
        const uint32_t kind = MpEffectKind(effect);
        if (kind == MP_KIND_STOP)
            return 0;

        if (kind == MP_KIND_CATALYST)
            lowered += MpEffectCatalystJoulesPerMol(effect);
        else if (kind == MP_KIND_SCALE)
            shift += MpEffectShift(effect);
    }

    const uint32_t limited = lowered < rule.activationEnergy ? lowered : rule.activationEnergy;

    return MpScaleRate(MpCatalyzeRate(packed, limited, kelvin), shift);
}

// 覆う転換のパッチ(converting。MpConvertingMask)のうち規則 rule(名前の鍵で比べる)のものを当てた規則。
// 生成物の欄ごとに、列の前の(先に足した)パッチが勝つ(後のものは同じ欄に効かない。順は CPU と GPU で同じ)。
// 釣り合いは替える前の規則で欄ごとに確かめる(欄どうしは独立なので、どの組み合わせでも全体が釣り合う)。替えたら反応熱を求め直す
template <typename Table>
FX_FN RxRule MpConvertedRule(Table table, uint32_t converting, RxRule rule) {
    RxRule converted = rule;
    uint32_t doneSlots = 0;
    int64_t enthalpyChange = 0;
    for (uint32_t i = 0; i < MP_MAX_PATCHES; ++i) {
        if (((converting >> i) & 1u) == 0)
            continue;

        const uint32_t base = MP_LIST_HEADER_WORDS + (i * MP_PATCH_WORDS);
        const uint64_t key = FX_U64(table.PatchWord(base + MP_WORD_RULE_KEY + 1),
                                    table.PatchWord(base + MP_WORD_RULE_KEY));
        const uint32_t effect = table.PatchWord(base + MP_WORD_EFFECT);
        const uint32_t slot = MpEffectConvertSlot(effect);
        if (key != rule.key || ((doneSlots >> slot) & 1u) != 0)
            continue;

        const uint32_t species = MpEffectConvertSpecies(effect);
        const MpSlotConversion conversion = MpConvertSlot(table, rule, slot, species);
        if (conversion.coefficient == 0)
            continue;

        // 欄 slot を書き換える(動的な添字を使わない。GPU でレジスタのまま)
        FX_UNROLL for (uint32_t p = 0; p < RX_MAX_PRODUCTS; ++p) {
            if (p == slot) {
                converted.products[p] = species;
                converted.productCoefficients[p] = conversion.coefficient;
            }
        }

        enthalpyChange += conversion.enthalpyChange;
        doneSlots |= 1u << slot;
    }

    // 反応熱は替えた欄の分だけ足し直す。ベイクの反応熱は Σ 生成物の H0 − Σ 反応物の H0(reaction_table.cpp の ReactionEnthalpy)
    // なので、整数で MpRuleEnthalpy を求め直したのとビット単位で同じ。規則全部の物質を読み直さない(核が規則を読む所ごとに
    // 展開されるので、HW の Work Graph が大きくなりすぎて止まった。T-0397)
    converted.reactionEnthalpy = rule.reactionEnthalpy + enthalpyChange;

    return converted;
}

// 覆うパッチ(covering)のうち転換のもの
template <typename List>
FX_FN uint32_t MpConvertingMask(List list, uint32_t covering) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < MP_MAX_PATCHES; ++i) {
        if (((covering >> i) & 1u) == 0)
            continue;

        const uint32_t effect = list.PatchWord(MP_LIST_HEADER_WORDS + (i * MP_PATCH_WORDS) + MP_WORD_EFFECT);
        if (MpEffectKind(effect) == MP_KIND_CONVERT)
            mask |= 1u << i;
    }

    return mask;
}

// パッチを重ねた表(reaction.hlsli の Table の約束。Rate と、転換が覆うセルでは Rule が変わる)。
// Table は PatchWord(word) も持つ(パッチの列を読む)
template <typename Table>
struct MpPatchedTable {
    Table base;
    uint32_t covering;    // MpCoveringMask
    uint32_t converting;  // そのうち転換のもの(MpConvertingMask)

    RxSpecies Species(uint32_t id) { return base.Species(id); }

    RxRule Rule(uint32_t id) {
        const RxRule rule = base.Rule(id);
        if (converting == 0)
            return rule;

        return MpConvertedRule(base, converting, rule);
    }

    // 速さと優先度だけに使う規則(reaction.hlsli の Table の約束。転換は速さの欄を変えないので替える前の規則。T-0397)
    RxRule RateRule(uint32_t id) { return base.Rule(id); }

    uint32_t RuleIndex(uint32_t position) { return base.RuleIndex(position); }

    uint64_t Rate(uint32_t rule, uint32_t kelvin) {
        const uint64_t packed = base.Rate(rule, kelvin);
        if (covering == 0)
            return packed;

        return MpPatchedRate(base, covering, base.Rule(rule), kelvin, packed);
    }
};

template <typename Table>
FX_FN MpPatchedTable<Table> MpPatchTable(Table table, int32_t level, int64_t x, int64_t y, int64_t z, uint64_t tick) {
    MpPatchedTable<Table> patched;
    patched.base = table;
    patched.covering = MpCoveringMask(table, level, x, y, z, tick);
    patched.converting = MpConvertingMask(table, patched.covering);

    return patched;
}

RX_NAMESPACE_END

#endif  // BICAMERAL_MAGIC_PATCH_HLSLI
