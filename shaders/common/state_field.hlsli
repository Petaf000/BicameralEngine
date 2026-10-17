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
// VOLUME: 細かく = 子は親と同じ数。粗く = 和 ÷ 8 + 端数(余りは欄の帳簿のレベルごとの列へ。量の欄)
// ブロック・世界の欄は SAME だけ(ブロック: 細かく = 子のブロックは親の値・粗く = 親は自分の値のまま)
FX_CONST uint32_t SF_SCALE_SAME = 0;
FX_CONST uint32_t SF_SCALE_VOLUME = 1;
FX_CONST uint32_t SF_SCALE_COUNT = 2;

// --- 印 ----------------------------------------------------------------------------------------
FX_CONST uint32_t SF_FLAG_CONSERVED = 1u;  // 保存量(世界の帳簿の列に入る。T-0712)
FX_CONST uint32_t SF_FLAG_HASHED = 2u;     // 状態のハッシュに入る
FX_CONST uint32_t SF_FLAG_MASK = 3u;

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

    // ブロック・世界の欄は細かく/粗くで和を取らない
    return desc.scale == SF_SCALE_SAME || desc.place == SF_PLACE_CELL || desc.place == SF_PLACE_FACE;
}

// セルの頁に置く欄か(セル・面)
FX_FN bool SfOnCells(SfFieldDesc desc) {
    return desc.place == SF_PLACE_CELL || desc.place == SF_PLACE_FACE;
}

FX_FN bool SfHashed(SfFieldDesc desc) {
    return (desc.flags & SF_FLAG_HASHED) != 0u;
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

// 余り(子のレベルの単位で 0〜7)。VOLUME の欄は欄の帳簿へ・SAME の欄は捨てる
FX_FN uint32_t SfCoarsenRemainder(SfCoarsenSum sum) {
    return sum.low & 7u;
}

SF_NAMESPACE_END

#endif  // BICAMERAL_STATE_FIELD_HLSLI
