// magic_patch.hlsli — 魔法のパッチ(反応則への介入。docs/design/02-reaction-system.md §5・T-0024・ADR-0095)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU リファレンス(engine/src/sim/multires_nest.cpp の刻み)と
// GPU(shaders/sim/multires_patches.hlsli を通る多重解像度の刻み)が同じ関数を呼ぶので、パッチの効いた刻みもビット単位で同じになる。
//
// データの流れ:
//   コマンド(実験室なら LAB_COMMAND_TYPE_PATCH。06 §3。再生ファイルに入る)→ MpAddPatch がパッチの列(世界の状態。GPU は u5、CPU は
//   MultiresNest::magicPatches。どちらも同じ uint32 の語の並び)に足す → 刻みの反応の評価(MrStepCellWait)が、セルを覆うパッチを
//   MpPatchTable で表に重ねる(MpPatchedTable の Rate だけが変わる)→ 反応の核(reaction.hlsli)はそのまま。
//
// 効き方(今あるもの): 規則 1 つの速さを 2^shift 倍にする(増幅・弱める)/ 止める(阻害)。
//   速さの表の値は「仮数 × 2^指数」なので、指数に shift を足すだけで、どの機械でも同じ整数の結果になる。
//   **保存則は変えない**(D-428・D-419): 変わるのは「1 刻みにどれだけ進みたいか」だけで、進んだ分は反応の核が係数 × 進行度で
//   成分を動かす(元素の数とエネルギーはビット単位で保存される)。触媒(活性化エネルギーを下げる)・転換・湧き出しと吸い込みは後のチケット。
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

// 効き方の語: 下位 8bit = 種類、8〜15bit = shift(符号付き 8bit。MP_KIND_SCALE だけ)
FX_CONST uint32_t MP_KIND_SCALE = 1;       // 速さを 2^shift 倍(増幅・弱める)
FX_CONST uint32_t MP_KIND_STOP = 2;        // 止める(阻害)
FX_CONST int32_t MP_MAX_SHIFT = 64;        // |shift| の上限(16 個重ねても指数は int32 に十分収まる)
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

FX_FN uint32_t MpEffectKind(uint32_t effect) {
    return effect & 0xFFu;
}

// 8〜15bit を符号付きで読む
FX_FN int32_t MpEffectShift(uint32_t effect) {
    return ((int32_t)(effect << 16)) >> 24;
}

FX_FN bool MpEffectValid(uint32_t effect) {
    if ((effect >> 16) != 0)
        return false;

    const uint32_t kind = MpEffectKind(effect);
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

// --- パッチの検査 ------------------------------------------------------------------------------

// 表に名前の鍵 key の規則があるか(ruleCount = 表の規則の数。足す時だけ呼ぶ)
template <typename Table>
FX_FN bool MpRuleKnown(Table table, uint64_t key, uint32_t ruleCount) {
    for (uint32_t rule = 0; rule < ruleCount; ++rule) {
        if (table.Rule(rule).key == key)
            return true;
    }

    return false;
}

// 当てられるか(規則が表にある・効き方が正しい・期間が 1 刻み以上・角の順)。ruleCount = 表の規則の数
template <typename Table>
FX_FN bool MpPatchValid(Table table, MpPatch patch, uint32_t ruleCount) {
    const bool ordered = patch.lowX <= patch.highX && patch.lowY <= patch.highY && patch.lowZ <= patch.highZ;
    if (!MpEffectValid(patch.effect) || patch.startTick >= patch.endTick || !ordered)
        return false;

    return MpRuleKnown(table, patch.ruleKey, ruleCount);
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

// 覆うパッチ(mask)のうち名前の鍵 ruleKey の規則のものを重ねた速さの表の値。止めるものが 1 つでもあれば 0、ほかは shift の和
// (重ねる順に依らない)
template <typename List>
FX_FN uint64_t MpPatchedRate(List list, uint32_t mask, uint64_t ruleKey, uint64_t packed) {
    int32_t shift = 0;
    for (uint32_t i = 0; i < MP_MAX_PATCHES; ++i) {
        if (((mask >> i) & 1u) == 0)
            continue;

        const uint32_t base = MP_LIST_HEADER_WORDS + (i * MP_PATCH_WORDS);
        const uint64_t key = FX_U64(list.PatchWord(base + MP_WORD_RULE_KEY + 1),
                                    list.PatchWord(base + MP_WORD_RULE_KEY));
        if (key != ruleKey)
            continue;

        const uint32_t effect = list.PatchWord(base + MP_WORD_EFFECT);
        if (MpEffectKind(effect) == MP_KIND_STOP)
            return 0;

        shift += MpEffectShift(effect);
    }

    return MpScaleRate(packed, shift);
}

// パッチを重ねた表(reaction.hlsli の Table の約束。Rate だけが変わる)。Table は PatchWord(word) も持つ(パッチの列を読む)
template <typename Table>
struct MpPatchedTable {
    Table base;
    uint32_t covering;  // MpCoveringMask

    RxSpecies Species(uint32_t id) { return base.Species(id); }

    RxRule Rule(uint32_t id) { return base.Rule(id); }

    uint32_t RuleIndex(uint32_t position) { return base.RuleIndex(position); }

    uint64_t Rate(uint32_t rule, uint32_t kelvin) {
        const uint64_t packed = base.Rate(rule, kelvin);
        if (covering == 0)
            return packed;

        return MpPatchedRate(base, covering, base.Rule(rule).key, packed);
    }
};

template <typename Table>
FX_FN MpPatchedTable<Table> MpPatchTable(Table table, int32_t level, int64_t x, int64_t y, int64_t z, uint64_t tick) {
    MpPatchedTable<Table> patched;
    patched.base = table;
    patched.covering = MpCoveringMask(table, level, x, y, z, tick);

    return patched;
}

RX_NAMESPACE_END

#endif  // BICAMERAL_MAGIC_PATCH_HLSLI
