// state_field.hlsli — 持ち越す状態の欄の登録の形(運ぶ計算の共通の土台の段 1 ①。T-0711。ADR-0329。設計 §2.1・§3)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU(engine/src/sim/multires_fields.cpp)が今使い、
// GPU(段 6)が同じ形・同じ関数で欄のバッファを作る。名前(文字列)は C++ の側だけが持つ(MultiresField::name)。
//
// 欄 = セル(成分 + エネルギー)の外に持ち越す値(例: 気体の運動量 ×3・液体の面の運動量 ×3・前の相の質量・機械の蓄え)。
// 1 行の登録(SfFieldDesc)で、置き場・型・付く相・保存量か・細かく/粗くする時の規則・ハッシュに入るかを決め、
// 保存点・写し・ハッシュ・細かく/粗くするは登録を回るだけにする(欄を足しても 2〜3 か所を直さない)。
//
// 値はどれも 64bit の符号付き整数で持つ(型は範囲の約束。I32 / U32 は 64bit の中の範囲を書く側が守る)。
// 量の単位はセルと同じくレベル k で 8^-k(細かくすると子は親と同じ数 = 和が 8 倍の単位で保たれる)。
//
// 欄の種類(T-0854。20 §10・ADR-0373・ADR-0399): 真実(状態そのもの。既定)/ 導かれる値(真実から計算できる。SF_FLAG_DERIVED)/
// 温めた始まり(解き始めに使う導かれる値。SF_FLAG_DERIVED | SF_FLAG_WARM_START)。ハッシュ・保存点・保存量・粗くする規則・眠る時の扱いは
// 種類から決まる(SfHashed・SfSaved・SfDescValid・SfSleepValue)。作業役は欄ごとに考えない。
#ifndef BICAMERAL_STATE_FIELD_HLSLI
#define BICAMERAL_STATE_FIELD_HLSLI

#include "fixed.hlsli"

#ifdef __cplusplus
#define SF_NAMESPACE_BEGIN             \
    namespace bicameral::state_field { \
        using namespace ::bicameral::fx;
#define SF_NAMESPACE_END }
#else
#define SF_NAMESPACE_BEGIN
#define SF_NAMESPACE_END
#endif

SF_NAMESPACE_BEGIN

// --- 置き場 ------------------------------------------------------------------------------------
FX_CONST uint32_t SF_PLACE_CELL = 0;   // セルごと。セルの頁と同じ番号の「欄の頁」(一様なブロックは値 1 つ)
FX_CONST uint32_t SF_PLACE_FACE = 1;   // セルの + 側の 3 面(x+・y+・z+。値は 3 つ。置き方はセルと同じ)
FX_CONST uint32_t SF_PLACE_BLOCK = 2;  // ブロックごと(見出しの枠と同じ番号)
FX_CONST uint32_t SF_PLACE_WORLD = 3;  // 世界に 1 つ
FX_CONST uint32_t SF_PLACE_COUNT = 4;

// --- 型(値の範囲の約束。持ち方はどれも 64bit の符号付き)------------------------------------------
FX_CONST uint32_t SF_TYPE_I64 = 0;
FX_CONST uint32_t SF_TYPE_I32 = 1;
FX_CONST uint32_t SF_TYPE_U32 = 2;
FX_CONST uint32_t SF_TYPE_COUNT = 3;

// --- 付く相(欄が物質に付く: 前の相の質量と比べて合わせ直す相。段 2〜3 で使う)-----------------------
FX_CONST uint32_t SF_PHASE_NONE = 0;
FX_CONST uint32_t SF_PHASE_GAS = 1;
FX_CONST uint32_t SF_PHASE_LIQUID = 2;
FX_CONST uint32_t SF_PHASE_COUNT = 3;

// --- 細かく/粗くする時の規則 --------------------------------------------------------------------
// SAME:   細かく = 子は親と同じ数。粗く = 子 8 つの平均(和 ÷ 8 の切り捨て。余りは捨てる。量でない欄)
// VOLUME: 細かく = 子は親と同じ数。粗く = 和 ÷ 8 + 端数(余りは世界の帳簿の「粗くした端数・出」の、その欄の列へ。量の欄 = 保存量。T-0762)
// ブロック・世界の欄は SAME だけ(ブロック: 細かく = 子のブロックは親の値・粗く = 親は自分の値のまま)
// 観察の影(親を覆わない・世界に返さない。D-403): 細かく = 子は親と同じ数。引き戻す = 子の和を親 × 8 に(SfPullBackChild。ADR-0361)
FX_CONST uint32_t SF_SCALE_SAME = 0;
FX_CONST uint32_t SF_SCALE_VOLUME = 1;
FX_CONST uint32_t SF_SCALE_COUNT = 2;

// --- 印 ----------------------------------------------------------------------------------------
FX_CONST uint32_t SF_FLAG_CONSERVED = 1u;  // 保存量(世界の帳簿の列に入る。VOLUME の欄と同じもの。T-0712・T-0762)
FX_CONST uint32_t SF_FLAG_HASHED = 2u;     // 状態のハッシュに入る(真実の欄だけが書く。導かれる値は種類から決まる)
// 導かれる値(T-0854。20 §10.1): 真実から計算できる(圧力・たわみと力・電位・磁場・放射の場など)。起きている所でだけ計算して書く。
// セル・面の欄だけ・SAME だけ・保存量でない。眠る(頁を畳む)時は一様の値の枠へ平均して残す(SF_DERIVED_SLEEP_KEEP_COARSE)
FX_CONST uint32_t SF_FLAG_DERIVED = 4u;
// 温めた始まり(T-0854。20 §10.4): 決まった回数で解く法則の解き始め(前の刻みの答え)。導かれる値だが答えを変えるので、
// SF_WARM_START_IN_STATE なら真実と同じくハッシュと保存点に入る。SF_FLAG_DERIVED と一緒に書く
FX_CONST uint32_t SF_FLAG_WARM_START = 8u;
FX_CONST uint32_t SF_FLAG_MASK = 15u;

// --- 欄の種類(印から決まる。SfFieldKind)---
FX_CONST uint32_t SF_KIND_TRUTH = 0;       // 真実(状態そのもの)
FX_CONST uint32_t SF_KIND_DERIVED = 1;     // 導かれる値(状態に入らない。読み込んだ後・起きた時に真実から解き直す)
FX_CONST uint32_t SF_KIND_WARM_START = 2;  // 温めた始まり(SF_WARM_START_IN_STATE なら状態に入る)

// D-549(仮・ユーザー未確認。おすすめ (A)): 温めた始まりをハッシュと保存点に入れる。(B)(真実だけ)に戻すならここを false にするだけ
// (読み込んだ直後の数刻みの導かれる値が、続けて遊んだ場合とビット単位で一致しなくなる)
FX_CONST bool SF_WARM_START_IN_STATE = true;

// D-542(ユーザー決定 (b)): 眠った所の導かれる値は粗くして残す(ブロックの一様の値の枠へ平均)。研究 R-F1 で使えなければ
// (a) 捨てる(0 にして、起きた時に解き直す)へ下げる = ここを false にするだけ
FX_CONST bool SF_DERIVED_SLEEP_KEEP_COARSE = true;

FX_CONST uint32_t SF_MAX_COMPONENTS = 3;  // セル・ブロック・世界の欄の値の数の上限(面は 3 つ決まり)

// 欄の登録 1 行(名前は C++ の側。GPU へは定数バッファでこの並びのまま渡す)
struct SfFieldDesc {
    uint32_t place;
    uint32_t type;
    uint32_t phase;
    uint32_t scale;
    uint32_t flags;
    uint32_t components;  // セル・ブロック・世界: 1〜3。面: 3(+ 側の 3 面)
};

FX_FN SfFieldDesc SfMakeFieldDesc(uint32_t place, uint32_t type, uint32_t phase, uint32_t scale, uint32_t flags,
                                  uint32_t components) {
    SfFieldDesc desc;
    desc.place = place;
    desc.type = type;
    desc.phase = phase;
    desc.scale = scale;
    desc.flags = flags;
    desc.components = place == SF_PLACE_FACE ? 3u : components;

    return desc;
}

FX_FN bool SfSameDesc(SfFieldDesc a, SfFieldDesc b) {
    return a.place == b.place && a.type == b.type && a.phase == b.phase && a.scale == b.scale && a.flags == b.flags &&
           a.components == b.components;
}

FX_FN bool SfDescValid(SfFieldDesc desc) {
    if (desc.place >= SF_PLACE_COUNT || desc.type >= SF_TYPE_COUNT || desc.phase >= SF_PHASE_COUNT ||
        desc.scale >= SF_SCALE_COUNT || (desc.flags & ~SF_FLAG_MASK) != 0u)
        return false;

    if (desc.components == 0u || desc.components > SF_MAX_COMPONENTS)
        return false;

    if (desc.place == SF_PLACE_FACE && desc.components != 3u)
        return false;

    // 量の欄(VOLUME)と保存量は同じもの: 粗くした余りを載せる帳簿の列は保存量の欄だけが持ち、
    // 保存量の検査(ComputeConservedTotals)は和を取る欄だけを数える(T-0762。仮)
    if (((desc.flags & SF_FLAG_CONSERVED) != 0u) != (desc.scale == SF_SCALE_VOLUME))
        return false;

    // 温めた始まりは導かれる値の一つ
    const bool derived = (desc.flags & SF_FLAG_DERIVED) != 0u;
    if ((desc.flags & SF_FLAG_WARM_START) != 0u && !derived)
        return false;

    // 導かれる値: セル・面の欄・量でない(SAME)・保存量でない。ハッシュに入るかは種類から決まるので HASHED を書かない(T-0854)
    if (derived && (desc.scale != SF_SCALE_SAME || (desc.flags & (SF_FLAG_CONSERVED | SF_FLAG_HASHED)) != 0u ||
                    (desc.place != SF_PLACE_CELL && desc.place != SF_PLACE_FACE)))
        return false;

    // ブロック・世界の欄は細かく/粗くで和を取らない
    return desc.scale == SF_SCALE_SAME || desc.place == SF_PLACE_CELL || desc.place == SF_PLACE_FACE;
}

// 導かれる値の登録 1 行(T-0854): セル・面・SAME・保存量でない。warmStart なら温めた始まり
FX_FN SfFieldDesc SfMakeDerivedDesc(uint32_t place, uint32_t type, uint32_t phase, bool warmStart,
                                    uint32_t components) {
    const uint32_t flags = SF_FLAG_DERIVED | (warmStart ? SF_FLAG_WARM_START : 0u);

    return SfMakeFieldDesc(place, type, phase, SF_SCALE_SAME, flags, components);
}

// 欄の種類(SF_KIND_*)
FX_FN uint32_t SfFieldKind(SfFieldDesc desc) {
    if ((desc.flags & SF_FLAG_DERIVED) == 0u)
        return SF_KIND_TRUTH;

    return (desc.flags & SF_FLAG_WARM_START) != 0u ? SF_KIND_WARM_START : SF_KIND_DERIVED;
}

FX_FN bool SfDerived(SfFieldDesc desc) {
    return (desc.flags & SF_FLAG_DERIVED) != 0u;
}

// セルの頁に置く欄か(セル・面)
FX_FN bool SfOnCells(SfFieldDesc desc) {
    return desc.place == SF_PLACE_CELL || desc.place == SF_PLACE_FACE;
}

// 保存量の欄か(世界の帳簿に値の数だけ列を持つ)
FX_FN bool SfConserved(SfFieldDesc desc) {
    return (desc.flags & SF_FLAG_CONSERVED) != 0u;
}

// 状態のハッシュに入るか: 真実は HASHED の印・温めた始まりは SF_WARM_START_IN_STATE・導かれる値は入らない(T-0854)
FX_FN bool SfHashed(SfFieldDesc desc) {
    const uint32_t kind = SfFieldKind(desc);
    if (kind == SF_KIND_TRUTH)
        return (desc.flags & SF_FLAG_HASHED) != 0u;

    return kind == SF_KIND_WARM_START && SF_WARM_START_IN_STATE;
}

// 保存点・写しに入るか: 真実はいつも・温めた始まりは SF_WARM_START_IN_STATE・導かれる値は入らない
// (戻した時は 0 にして、起きている所は次に計算する時に真実から解き直す。15 §1「差分に入るのは真実だけ」)
FX_FN bool SfSaved(SfFieldDesc desc) {
    const uint32_t kind = SfFieldKind(desc);

    return kind == SF_KIND_TRUTH || (kind == SF_KIND_WARM_START && SF_WARM_START_IN_STATE);
}

// 真実の要約(眠らせても変わらないはずのもの)に入るか: 真実でハッシュに入る欄だけ
FX_FN bool SfTruthHashed(SfFieldDesc desc) {
    return SfFieldKind(desc) == SF_KIND_TRUTH && (desc.flags & SF_FLAG_HASHED) != 0u;
}

// 値が型の範囲に入るか(書く側の約束を確かめる)
FX_FN bool SfValueFits(SfFieldDesc desc, int64_t value) {
    if (desc.type == SF_TYPE_I32)
        return value >= -(int64_t)0x80000000u && value <= (int64_t)0x7FFFFFFF;

    if (desc.type == SF_TYPE_U32)
        return value >= 0 && value <= (int64_t)0xFFFFFFFFu;

    return true;
}

// --- 細かくする: 子は親と同じ数 ------------------------------------------------------------------
FX_FN int64_t SfRefineValue(int64_t parentValue) {
    return parentValue;
}

// --- 粗くする: 子 8 つの和 ÷ 8(負の値も −∞ 向きの切り捨て)と余り(0〜7。子のレベルの単位)--------------
// 和は 64bit を越えうるので、各値を 8 × (v >> 3) + (v & 7) に分けて足す(Σ(v & 7) ≤ 56)
struct SfCoarsenSum {
    int64_t whole;  // Σ (v >> 3)
    uint32_t low;   // Σ (v & 7)
};

FX_FN SfCoarsenSum SfMakeCoarsenSum() {
    SfCoarsenSum sum;
    sum.whole = 0;
    sum.low = 0u;

    return sum;
}

FX_FN SfCoarsenSum SfAddCoarsenChild(SfCoarsenSum sum, int64_t childValue) {
    sum.whole += childValue >> 3;
    sum.low += (uint32_t)(childValue & 7);

    return sum;
}

// 親の値(和 ÷ 8 の切り捨て)
FX_FN int64_t SfCoarsenValue(SfCoarsenSum sum) {
    return sum.whole + (int64_t)(sum.low >> 3u);
}

// 余り(子のレベルの単位で 0〜7。切り捨てなのでいつも 0 以上 = セルから減った量)。VOLUME の欄は世界の帳簿の「粗くした端数・出」へ・SAME の欄は捨てる
FX_FN uint32_t SfCoarsenRemainder(SfCoarsenSum sum) {
    return sum.low & 7u;
}

// --- 眠る時の導かれる値: 覆われていないセルの平均(T-0854。20 §10.3)----------------------------------------
// 頁を畳む(眠る)時、導かれる値はブロックの一様の値の枠へ、覆われていないセルの値の平均(−∞ 向きの切り捨て)を 1 つ残す。
// 起きる(頁に広げる)時は一様の値を覆われていないセルへ配る(SAME: 子は親と同じ数 = 今の ExpandPage と同じ)。
// 和は 512 個の 64bit で 64bit を越えうるので 128bit(2 の補数)で足す。余りは捨てる(保存量でない)
struct SfMeanSum {
    int64_t high;    // 上の 64bit(符号付き)
    uint64_t low;    // 下の 64bit
    uint32_t count;  // 足した値の数
};

FX_FN SfMeanSum SfMakeMeanSum() {
    SfMeanSum sum;
    sum.high = 0;
    sum.low = 0;
    sum.count = 0u;

    return sum;
}

FX_FN SfMeanSum SfAddMeanValue(SfMeanSum sum, int64_t value) {
    const uint64_t low = sum.low + (uint64_t)value;
    const int64_t carry = low < sum.low ? (int64_t)1 : (int64_t)0;
    sum.high += (value < 0 ? (int64_t)-1 : (int64_t)0) + carry;
    sum.low = low;
    sum.count += 1u;

    return sum;
}

// 平均(−∞ 向きの切り捨て)。値が無ければ 0。|和| ≤ 数 × 2^63 なので、絶対値の上の 64bit は数より小さい(FxDivU128By64 の約束)
FX_FN int64_t SfMeanValue(SfMeanSum sum) {
    if (sum.count == 0u)
        return 0;

    const bool negative = sum.high < 0;
    const uint64_t magnitudeLow = negative ? ~sum.low + 1u : sum.low;
    const uint64_t magnitudeHigh = negative ? ~(uint64_t)sum.high + (magnitudeLow == 0 ? (uint64_t)1 : (uint64_t)0)
                                            : (uint64_t)sum.high;
    const FxU128 magnitude = {magnitudeHigh, magnitudeLow};
    const FxDivResult division = FxDivU128By64(magnitude, (uint64_t)sum.count);
    const uint64_t rounded = division.quotient + (negative && division.remainder != 0 ? (uint64_t)1 : (uint64_t)0);

    return FxApplySign(rounded, negative);
}

// 眠る時に一様の値の枠へ残す値(平均 mean。(a) 捨てるなら 0)
FX_FN int64_t SfSleepValue(int64_t mean) {
    return SF_DERIVED_SLEEP_KEEP_COARSE ? mean : (int64_t)0;
}

// --- 観察の影を親に引き戻す(T-0761。ADR-0361)-------------------------------------------------------
// 影の子 2³ の値を、子の和が「親 × 8」(子のレベルの単位)になるように合わせる = 影を粗くすると親と同じ値・余り 0。
// セルの引き戻し(multires.hlsli の MrPullBackShadow)と同じ芯: 影は親に従って細部を動かすだけで、世界に返さない(D-403)。
//   親と子 8 つが全部 0 以上(2^60 未満。質量・エネルギーなど負にならない量。今その値の所): 親 × 8 を子の今の値の比で配る
//     (切り捨て。余りは 0 でない子に番号の順に 1 ずつ。全部 0 なら親と同じ数。セルの物質と同じ配り方 = 負にならない・偏りの形を保つ)。
//   そのほか(負の値がある。運動量など): 足りない分(負もある)を 8 つに等しく足す(余りは子の番号の順に 1 ずつ。影の中の違いはそのまま)。
// 型(I64・I32・U32)にも SAME / VOLUME にもよらない 1 つの規則(SAME を粗くした平均も親と同じになる)。
// 子の値は読むだけ(書き換えた配列を動的な添字で読まない。T-0397)
FX_CONST uint32_t SF_CHILDREN = 8;  // 1 つの親のセルの子のセルの数(MR_CHILDREN_PER_CELL と同じ)

// 影の子 2³ の 1 つの値(子の番号は MrChildCell の j)
struct SfFamily {
    int64_t values[SF_CHILDREN];
};

// 負の値がある: 子の値 + 等しく足す分(8 × 親 − 和 = 8 × (親 − Σ(v >> 3)) − Σ(v & 7) を「8 × 足す分 + 余り」に分ける。64bit を越えない)
FX_FN int64_t SfPullBackShiftedChild(int64_t parentValue, SfFamily family, uint32_t child) {
    SfCoarsenSum sum = SfMakeCoarsenSum();
    FX_UNROLL
    for (uint32_t j = 0; j < SF_CHILDREN; ++j)
        sum = SfAddCoarsenChild(sum, family.values[j]);

    const uint32_t lowCeil = (sum.low + 7u) & ~7u;  // Σ(v & 7) を 8 の倍数に切り上げ
    const int64_t share = parentValue - sum.whole - (int64_t)(lowCeil >> 3u);
    const uint32_t rest = lowCeil - sum.low;  // 0〜7

    return family.values[child] + share + (child < rest ? (int64_t)1 : (int64_t)0);
}

// 親と子が全部 0 以上か(比で配る方を使うか)。2^60 以上の値がある時も等しく足す方(親 × 8 と子の和が 63bit に入る範囲だけ比で配る)
FX_FN bool SfPullBackNonNegative(int64_t parentValue, SfFamily family) {
    bool nonNegative = (parentValue >> 60) == 0;
    FX_UNROLL
    for (uint32_t j = 0; j < SF_CHILDREN; ++j)
        nonNegative = nonNegative && (family.values[j] >> 60) == 0;

    return nonNegative;
}

// 負にならない: 親 × 8 を子の今の値の比で配った child の分(multires.hlsli の MrDistribute と同じ配り方)
FX_FN int64_t SfPullBackSharedChild(int64_t parentValue, SfFamily family, uint32_t child) {
    FX_ASSERT(SfPullBackNonNegative(parentValue, family));
    const uint64_t total = (uint64_t)parentValue << 3u;
    uint64_t weightSum = 0;
    FX_UNROLL
    for (uint32_t j = 0; j < SF_CHILDREN; ++j)
        weightSum += (uint64_t)family.values[j];

    if (weightSum == 0)
        return parentValue;

    // --- 切り捨ての分・その和・child より前の 0 でない子の数 ---
    uint64_t given = 0;
    uint64_t mine = 0;
    uint32_t rank = 0;
    FX_UNROLL
    for (uint32_t k = 0; k < SF_CHILDREN; ++k) {
        const uint64_t share = FxDivU128By64(FxMulU64Full(total, (uint64_t)family.values[k]), weightSum).quotient;
        given += share;
        mine = k == child ? share : mine;
        rank += k < child && family.values[k] != 0 ? 1u : 0u;
    }

    // 切り捨てで足りない分(0 でない子の数より少ない)を 0 でない子に番号順に 1 ずつ
    const uint64_t rest = total - given;
    const bool bonus = family.values[child] != 0 && (uint64_t)rank < rest;

    return (int64_t)(mine + (bonus ? 1u : 0u));
}

// 影の子 child の引き戻した値(親 parentValue は親のセルの値。子の和 = 親 × 8 になる)
FX_FN int64_t SfPullBackChild(int64_t parentValue, SfFamily family, uint32_t child) {
    if (SfPullBackNonNegative(parentValue, family))
        return SfPullBackSharedChild(parentValue, family, child);

    return SfPullBackShiftedChild(parentValue, family, child);
}

SF_NAMESPACE_END

#endif  // BICAMERAL_STATE_FIELD_HLSLI
