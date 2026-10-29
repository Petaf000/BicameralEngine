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
// OWNER:  配り手(T-1252。ADR-0500 の 1・20 §10.7)。一般の段(nest_fields::Refine / Coarsen・影)は写さない・平均しない。
//         値は木の操作の中の「セルの配りの口」(nest_fields::RefineOwners / CoarsenOwners)が、登録の配り手の番号
//         (SfFieldDesc::owner = SF_OWNER_*。エンジンの決まった関数の表)の関数で書く。セル・面の真実の欄だけ。
//         畳む・広げる・眠る・保存点・ハッシュは一般の段のまま(どれも値をビット単位で変えない)。保存量なら配りは和をビット単位で保つ
FX_CONST uint32_t SF_SCALE_SAME = 0;
FX_CONST uint32_t SF_SCALE_VOLUME = 1;
FX_CONST uint32_t SF_SCALE_OWNER = 2;
FX_CONST uint32_t SF_SCALE_COUNT = 3;

// --- 印 ----------------------------------------------------------------------------------------
FX_CONST uint32_t SF_FLAG_CONSERVED = 1u;  // 保存量(世界の帳簿の列に入る。VOLUME の欄と同じもの。T-0712・T-0762)
FX_CONST uint32_t SF_FLAG_HASHED = 2u;     // 状態のハッシュに入る(真実の欄だけが書く。導かれる値は種類から決まる)
// 導かれる値(T-0854。20 §10.1): 真実から計算できる(圧力・たわみと力・電位・磁場・放射の場など)。起きている所でだけ計算して書く。
// セル・面の欄だけ・SAME だけ・保存量でない。眠る(頁を畳む)時は一様の値の枠へ平均して残す(SF_DERIVED_SLEEP_KEEP_COARSE)
FX_CONST uint32_t SF_FLAG_DERIVED = 4u;
// 温めた始まり(T-0854。20 §10.4): 決まった回数で解く法則の解き始め(前の刻みの答え)。導かれる値だが答えを変えるので、
// SF_WARM_START_IN_STATE なら真実と同じくハッシュと保存点に入る。SF_FLAG_DERIVED と一緒に書く
FX_CONST uint32_t SF_FLAG_WARM_START = 8u;
// 細かくする時の置かれ方(T-1157。D-664・20 §10.5): 無印 = 子は親と同じ数(SAME)。どちらかの印の欄は、細かくする時に
// 置かれ方の重み(呼ぶ側が親のセルごとに 1 回作る。同じセルの欄が同じ重みを読む)で子へ配る。真実のセル・面の欄だけ。
//   凝縮相に乗る: 子の凝縮相の量の重み(凝縮相の運動量など。子の速さが親と同じ)
//   空きに乗る:   子の空きの体積の重み(気体の量・気体の運動量・舞う粒)
// 置かれ方が一様(気体だけ・詰まり切った・据わる固体)なら SAME とビット単位で同じ
FX_CONST uint32_t SF_FLAG_PLACE_CONDENSED = 16u;
FX_CONST uint32_t SF_FLAG_PLACE_OPEN = 32u;
// 眠る時に捨てる(T-1159。ADR-0469 の 2-3・20 §10.3): 導かれる値だけ。眠る(頁を畳む)時に一様の値の枠へ平均ではなく 0 を残す。
// 起きた時に使わない温めた始まり(支えの刻みをまたぐ共役勾配の向きと残差。D-649・wt2 T-1086)に付ける(保存点の無駄を省く)
FX_CONST uint32_t SF_FLAG_SLEEP_DISCARD = 64u;
// 内訳(T-1177。ADR-0469 の 2-2・D-659): 1 つの保存量の欄(親。SfFieldDesc::parent)の値のうち「止まれる側の状態」の量。
// 例: 粒の量のうち積もった層の量(残り = 親 − 内訳 が舞う粒)。物質・保存量は親 1 つのまま(作り手の書く値は増えない)。
//   - 真実の欄・SAME・ハッシュに入る(保存点にも入る)。保存量の列は親だけ(内訳は列を持たない)。0 ≤ 内訳 ≤ 親(SfBreakdownFits)
//   - 細かくする: 内訳は自分の置かれ方の印で、残り(親 − 内訳)は親の置かれ方の印で配り、子の親 = 子の内訳 + 子の残り(SfBreakdownChildren)
//   - 粗くする: 内訳は和 ÷ 8 の切り捨て・親は和 ÷ 8 + 端数(帳簿)。切り捨て ≤ 切り捨てなので 0 ≤ 内訳 ≤ 親 のまま
//   - 眠る: 残りが 0 でないセル(舞う粒がある)のある頁は畳まない(SfBreakdownKeepsAwake。止まった層だけなら畳める)
FX_CONST uint32_t SF_FLAG_BREAKDOWN = 128u;
// この刻みだけ(T-1252。ADR-0500 の 2・20 §10.7): 刻みの中で書いて同じ刻みに読む値(源の項)。状態ではない(SF_KIND_TICK)。
// ハッシュ・保存点・保存量に入らない。刻みの境目では全部 0(使い終わったら nest_fields::EndTickOnly で 0 に・頁を返す)。
// セル・面の欄・SAME だけ(細かく/粗く・畳む・眠るは刻みの境目にしか起きないので 0 を扱うだけ)。HASHED・導かれる値の印を持たない
FX_CONST uint32_t SF_FLAG_TICK_ONLY = 256u;
FX_CONST uint32_t SF_FLAG_MASK = 511u;

FX_CONST uint32_t SF_NO_PARENT = 0xFFFFFFFFu;  // 内訳でない欄の親(SfFieldDesc::parent)

// --- 配り手の表(T-1252。SF_SCALE_OWNER の欄の SfFieldDesc::owner。GPU は決まった数の回しで選ぶ = T-0397)---
// 使い手が足す: 光の配り(放射の持ち越し。T-1227)
FX_CONST uint32_t SF_OWNER_TEST = 0;  // 試験用(子 j の重み j + 1 で配る・粗く = 和 ÷ 8 + 端数。レベルの単位)
// 相の層の配り(Ec。T-1230・T-1231・ADR-0512 の 1): セルの中身を読む配り手。頁を持つ親の細かく/粗くは、セルの配りの口が
// 親と子のセルを読んで書く(sim の multires_phase_layer。式は phase_layer.hlsli)。この表の関数はセルを読まない一般の形
// (子は親と同じ数・粗く = 和 ÷ 8 の床。余りは捨てる = 保存量でない)で、一様な親・影・一様のままの見比べに使う
FX_CONST uint32_t SF_OWNER_PHASE_LAYER = 1;
FX_CONST uint32_t SF_OWNER_COUNT = 2;
FX_CONST uint32_t SF_NO_OWNER = 0xFFFFFFFFu;  // 配り手でない欄(SfFieldDesc::owner)

// --- 細かくする時の配り方(印から決まる。SfRefinePlacement)---
FX_CONST uint32_t SF_REFINE_SAME = 0;
FX_CONST uint32_t SF_REFINE_CONDENSED = 1;
FX_CONST uint32_t SF_REFINE_OPEN = 2;

// --- 欄の種類(印から決まる。SfFieldKind)---
FX_CONST uint32_t SF_KIND_TRUTH = 0;       // 真実(状態そのもの)
FX_CONST uint32_t SF_KIND_DERIVED = 1;     // 導かれる値(状態に入らない。読み込んだ後・起きた時に真実から解き直す)
FX_CONST uint32_t SF_KIND_WARM_START = 2;  // 温めた始まり(SF_WARM_START_IN_STATE なら状態に入る)
FX_CONST uint32_t SF_KIND_TICK = 3;        // この刻みだけ(源の項。状態に入らない・刻みの境目で 0。T-1252)

// D-549(仮・ユーザー未確認。おすすめ (A)): 温めた始まりをハッシュと保存点に入れる。(B)(真実だけ)に戻すならここを false にするだけ
// (読み込んだ直後の数刻みの導かれる値が、続けて遊んだ場合とビット単位で一致しなくなる)
FX_CONST bool SF_WARM_START_IN_STATE = true;

// D-542(ユーザー決定 (b)): 眠った所の導かれる値は粗くして残す(ブロックの一様の値の枠へ平均)。研究 R-F1 で使えなければ
// (a) 捨てる(0 にして、起きた時に解き直す)へ下げる = ここを false にするだけ
FX_CONST bool SF_DERIVED_SLEEP_KEEP_COARSE = true;

// セル・ブロック・世界の欄の値の数の上限(面は 3 つ決まり)。12 = 放射の持ち越し(入った面 6 × 帯 2。ADR-0500 の 5・T-1252)。
// 欄の置き場は欄ごとの値の数で取るので、上限を上げても今の欄の大きさは変わらない(docs/perf.md 2026-10-29)
FX_CONST uint32_t SF_MAX_COMPONENTS = 12;

// 欄の登録 1 行(名前は C++ の側。GPU へは定数バッファでこの並びのまま渡す)
struct SfFieldDesc {
    uint32_t place;
    uint32_t type;
    uint32_t phase;
    uint32_t scale;
    uint32_t flags;
    uint32_t components;  // セル・ブロック・世界: 1〜3。面: 3(+ 側の 3 面)
    uint32_t parent;      // 内訳の欄の親の欄の番号(登録の順。親が先)。内訳でなければ SF_NO_PARENT(T-1177)
    uint32_t owner;       // 配り手の番号(SF_OWNER_*。scale が SF_SCALE_OWNER の時だけ)。でなければ SF_NO_OWNER(T-1252)
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
    desc.parent = SF_NO_PARENT;
    desc.owner = SF_NO_OWNER;

    return desc;
}

// 配り手の欄の登録 1 行(T-1252): 真実のセル・面の欄。flags は保存量(SF_FLAG_CONSERVED)・ハッシュ(SF_FLAG_HASHED)
FX_FN SfFieldDesc SfMakeOwnerDesc(uint32_t place, uint32_t type, uint32_t phase, uint32_t flags, uint32_t components,
                                  uint32_t owner) {
    SfFieldDesc desc = SfMakeFieldDesc(place, type, phase, SF_SCALE_OWNER, flags, components);
    desc.owner = owner;

    return desc;
}

// この刻みだけの欄の登録 1 行(T-1252): セル・面・SAME・保存量でない・ハッシュに入らない
FX_FN SfFieldDesc SfMakeTickDesc(uint32_t place, uint32_t type, uint32_t phase, uint32_t components) {
    return SfMakeFieldDesc(place, type, phase, SF_SCALE_SAME, SF_FLAG_TICK_ONLY, components);
}

// 内訳の欄の登録 1 行(T-1177): 真実・SAME・ハッシュに入る。flags には置かれ方の印(内訳そのものの配り方)を足せる
FX_FN SfFieldDesc SfMakeBreakdownDesc(uint32_t place, uint32_t type, uint32_t phase, uint32_t flags,
                                      uint32_t components, uint32_t parent) {
    SfFieldDesc desc = SfMakeFieldDesc(place, type, phase, SF_SCALE_SAME, flags | SF_FLAG_BREAKDOWN | SF_FLAG_HASHED,
                                       components);
    desc.parent = parent;

    return desc;
}

FX_FN bool SfSameDesc(SfFieldDesc a, SfFieldDesc b) {
    return a.place == b.place && a.type == b.type && a.phase == b.phase && a.scale == b.scale && a.flags == b.flags &&
           a.components == b.components && a.parent == b.parent && a.owner == b.owner;
}

FX_FN bool SfDescValid(SfFieldDesc desc) {
    if (desc.place >= SF_PLACE_COUNT || desc.type >= SF_TYPE_COUNT || desc.phase >= SF_PHASE_COUNT ||
        desc.scale >= SF_SCALE_COUNT || (desc.flags & ~SF_FLAG_MASK) != 0u)
        return false;

    if (desc.components == 0u || desc.components > SF_MAX_COMPONENTS)
        return false;

    if (desc.place == SF_PLACE_FACE && desc.components != 3u)
        return false;

    // 保存量 ⇒ VOLUME か配り手・VOLUME ⇒ 保存量(T-1252。前は「保存量 ⇔ VOLUME」T-0762): 粗くした余りを載せる帳簿の列は
    // 保存量の欄だけが持ち、保存量の検査(ComputeConservedTotals)は和を取る欄(配り手の欄も)だけを数える。配り手は和を保つ約束
    const bool conserved = (desc.flags & SF_FLAG_CONSERVED) != 0u;
    if ((conserved && desc.scale == SF_SCALE_SAME) || (desc.scale == SF_SCALE_VOLUME && !conserved))
        return false;

    // 配り手(T-1252): 配り手の番号を持つ ⇔ SF_SCALE_OWNER。表の中の番号
    const bool owned = desc.scale == SF_SCALE_OWNER;
    if (owned != (desc.owner != SF_NO_OWNER) || (owned && desc.owner >= SF_OWNER_COUNT))
        return false;

    // 温めた始まりは導かれる値の一つ
    const bool derived = (desc.flags & SF_FLAG_DERIVED) != 0u;
    if ((desc.flags & SF_FLAG_WARM_START) != 0u && !derived)
        return false;

    // 導かれる値: セル・面の欄・量でない(SAME)・保存量でない。ハッシュに入るかは種類から決まるので HASHED を書かない(T-0854)
    if (derived && (desc.scale != SF_SCALE_SAME || (desc.flags & (SF_FLAG_CONSERVED | SF_FLAG_HASHED)) != 0u ||
                    (desc.place != SF_PLACE_CELL && desc.place != SF_PLACE_FACE)))
        return false;

    // 眠る時に捨てるのは導かれる値だけ(真実は眠っても状態そのもの。T-1159)
    if ((desc.flags & SF_FLAG_SLEEP_DISCARD) != 0u && !derived)
        return false;

    // 置かれ方の印は真実のセル・面の欄だけ・どちらか 1 つ(T-1157)。配り手の欄は配り手が配る(印を持たない。T-1252)
    const uint32_t placeFlags = desc.flags & (SF_FLAG_PLACE_CONDENSED | SF_FLAG_PLACE_OPEN);
    if (placeFlags != 0u && (placeFlags == (SF_FLAG_PLACE_CONDENSED | SF_FLAG_PLACE_OPEN) || derived || owned ||
                             (desc.place != SF_PLACE_CELL && desc.place != SF_PLACE_FACE)))
        return false;

    // 配り手の欄は真実のセル・面の欄(T-1252)
    const bool onCells = desc.place == SF_PLACE_CELL || desc.place == SF_PLACE_FACE;
    if (owned && (derived || !onCells))
        return false;

    // この刻みだけ(T-1252): セル・面・SAME。真実の印(HASHED)・導かれる値・保存量・置かれ方・内訳の印を持たない
    const uint32_t notWithTick = SF_FLAG_HASHED | SF_FLAG_DERIVED | SF_FLAG_WARM_START | SF_FLAG_SLEEP_DISCARD |
                                 SF_FLAG_CONSERVED | SF_FLAG_PLACE_CONDENSED | SF_FLAG_PLACE_OPEN | SF_FLAG_BREAKDOWN;
    if ((desc.flags & SF_FLAG_TICK_ONLY) != 0u &&
        ((desc.flags & notWithTick) != 0u || desc.scale != SF_SCALE_SAME || !onCells))
        return false;

    // 内訳(T-1177): 真実のセル・面の欄・SAME(保存量の列は親だけ)・ハッシュに入る・親を持つ。内訳でない欄は親を持たない
    const bool breakdown = (desc.flags & SF_FLAG_BREAKDOWN) != 0u;
    if (breakdown != (desc.parent != SF_NO_PARENT))
        return false;

    if (breakdown && (derived || desc.scale != SF_SCALE_SAME || (desc.flags & SF_FLAG_HASHED) == 0u ||
                      (desc.place != SF_PLACE_CELL && desc.place != SF_PLACE_FACE)))
        return false;

    // ブロック・世界の欄は細かく/粗くで和を取らない
    return desc.scale == SF_SCALE_SAME || desc.place == SF_PLACE_CELL || desc.place == SF_PLACE_FACE;
}

// 細かくする時の配り方(SF_REFINE_*。T-1157)
FX_FN uint32_t SfRefinePlacement(SfFieldDesc desc) {
    if ((desc.flags & SF_FLAG_PLACE_CONDENSED) != 0u)
        return SF_REFINE_CONDENSED;

    return (desc.flags & SF_FLAG_PLACE_OPEN) != 0u ? SF_REFINE_OPEN : SF_REFINE_SAME;
}

// 導かれる値の登録 1 行(T-0854): セル・面・SAME・保存量でない。warmStart なら温めた始まり
FX_FN SfFieldDesc SfMakeDerivedDesc(uint32_t place, uint32_t type, uint32_t phase, bool warmStart,
                                    uint32_t components) {
    const uint32_t flags = SF_FLAG_DERIVED | (warmStart ? SF_FLAG_WARM_START : 0u);

    return SfMakeFieldDesc(place, type, phase, SF_SCALE_SAME, flags, components);
}

// 欄の種類(SF_KIND_*)
FX_FN uint32_t SfFieldKind(SfFieldDesc desc) {
    if ((desc.flags & SF_FLAG_TICK_ONLY) != 0u)
        return SF_KIND_TICK;

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

// 状態のハッシュに入るか: 真実は HASHED の印・温めた始まりは SF_WARM_START_IN_STATE・導かれる値とこの刻みだけは入らない(T-0854)
FX_FN bool SfHashed(SfFieldDesc desc) {
    const uint32_t kind = SfFieldKind(desc);
    if (kind == SF_KIND_TRUTH)
        return (desc.flags & SF_FLAG_HASHED) != 0u;

    return kind == SF_KIND_WARM_START && SF_WARM_START_IN_STATE;
}

// 保存点・写しに入るか: 真実はいつも・温めた始まりは SF_WARM_START_IN_STATE・導かれる値とこの刻みだけは入らない
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

// 配り手の欄か(T-1252。一般の段は写さず、セルの配りの口が配り手の関数で書く)
FX_FN bool SfOwned(SfFieldDesc desc) {
    return desc.scale == SF_SCALE_OWNER;
}

// この刻みだけの欄か(T-1252)
FX_FN bool SfTickOnly(SfFieldDesc desc) {
    return (desc.flags & SF_FLAG_TICK_ONLY) != 0u;
}

// --- 内訳(T-1177。SF_FLAG_BREAKDOWN)---
FX_FN bool SfBreakdown(SfFieldDesc desc) {
    return (desc.flags & SF_FLAG_BREAKDOWN) != 0u;
}

// 内訳と親の組が正しいか(登録の時): 親は保存量の欄・内訳でない・置き場と値の数が同じ
FX_FN bool SfBreakdownPairValid(SfFieldDesc desc, SfFieldDesc parentDesc) {
    return SfBreakdown(desc) && SfConserved(parentDesc) && !SfBreakdown(parentDesc) && parentDesc.place == desc.place &&
           parentDesc.components == desc.components;
}

// 状態の検査: 0 ≤ 内訳 ≤ 親(引き算をしない。64bit の引き算と比べを並べない約束。T-1078)
FX_FN bool SfBreakdownFits(int64_t parentValue, int64_t value) {
    return value >= 0 && value <= parentValue;
}

// 眠る判定: 残り(親 − 内訳。舞う粒)があるセルは起きている
FX_FN bool SfBreakdownKeepsAwake(int64_t parentValue, int64_t value) {
    return value != parentValue;
}

// --- 細かくする: 子は親と同じ数 ------------------------------------------------------------------
FX_FN int64_t SfRefineValue(int64_t parentValue) {
    return parentValue;
}

// --- 細かくする: 置かれ方の重みで配る(T-1157。D-664・20 §10.5)------------------------------------------
// 親の値 v(親のレベルの単位)を childCount 個の子へ、重み weight / weightSum で配る(子のレベルの単位。和は childCount × v)。
// 重みのある最後の子以外はこれで、その子は残り(SfPlacedLastValue。重みの無い子に端数を入れない)。重みが全部同じなら子は v そのもの(= SAME)。weightSum が 0 なら呼ぶ側が SAME で配る
FX_FN int64_t SfPlacedValue(int64_t parentValue, uint64_t weight, uint64_t weightSum, uint32_t childCount) {
    return FxMulDivS64(parentValue, weight * childCount, weightSum);
}

FX_FN int64_t SfPlacedLastValue(int64_t parentValue, int64_t given, uint32_t childCount) {
    FX_ASSERT(parentValue < (int64_t)FX_U64(0x08000000u, 0u) && parentValue > -(int64_t)FX_U64(0x08000000u, 0u));
    return (parentValue * (int64_t)childCount) - given;
}

// --- 置かれ方(見立ての 1 関数。T-1157。wt3 T-1106 の SettleColumn・SettlePictureCell を移した。D-659 の積もった高さ・D-665 の層の形も読む)---
// 凝縮相(粒・液体)は重さの向きに下の子から「沈み固まった体積」で詰め、横は隣の列の表面の傾き(minmod)に沿って列を分ける。
// 量は mg、密度は kg/m³(= 1e-3 mg/mm³)、体積は mm³。詰まり・傾きは Q16(1 = 子の体積の和)。回しは呼ぶ側(CPU: sim/placement_refine.cpp)
FX_CONST int64_t SF_SETTLE_FULL_Q16 = 65536;
FX_CONST uint32_t SF_SETTLE_GAS = 0;
FX_CONST uint32_t SF_SETTLE_LIQUID = 1;
FX_CONST uint32_t SF_SETTLE_GRAIN = 2;
FX_CONST uint32_t SF_SETTLE_ANCHORED = 3;  // 据わる固体(寄せない)

// 量 → 物質そのものの体積(mm³)
FX_FN uint64_t SfSubstanceVolume(int64_t massMg, uint32_t densityKgPerM3) {
    if (densityKgPerM3 == 0u || massMg <= 0)
        return 0;

    return ((uint64_t)massMg * 1000u) / densityKgPerM3;
}

// 物質量(µmol。世界の木のセルの数)→ 体積(mm³)= 数 × モル体積(mm³/mol)÷ 10^6(T-1176。RxSpecies::molarVolume。0 は場所を取らない)
FX_FN uint64_t SfAmountVolume(int64_t amountMicromol, uint32_t molarVolumeMm3PerMol) {
    if (molarVolumeMm3PerMol == 0u || amountMicromol <= 0)
        return 0;

    return (uint64_t)FxMulDivS64(amountMicromol, (uint64_t)molarVolumeMm3PerMol, 1000000u);
}

// 物質の体積: モル体積を書いた物質(世界の木。量は µmol)はそちら、無ければ密度(量は mg。案 B の場面)
FX_FN uint64_t SfMaterialVolume(int64_t amount, uint32_t densityKgPerM3, uint32_t molarVolumeMm3PerMol) {
    if (molarVolumeMm3PerMol != 0u)
        return SfAmountVolume(amount, molarVolumeMm3PerMol);

    return SfSubstanceVolume(amount, densityKgPerM3);
}

// 沈み固まった体積の足し合わせ: 粒は粒の体積 ÷ 詰まり、液体は粒の孔を満たしてから上に重なる(大きい方)。気体は数えない
struct SfSettleSum {
    uint64_t grainVolume;
    uint64_t condensedVolume;
    bool anchored;  // 据わる固体を含む
};

FX_FN SfSettleSum SfMakeSettleSum() {
    SfSettleSum sum;
    sum.grainVolume = 0;
    sum.condensedVolume = 0;
    sum.anchored = false;

    return sum;
}

// 量は mg(密度)か µmol(モル体積。0 でなければこちら。T-1176)
FX_FN SfSettleSum SfAddSettleOf(SfSettleSum sum, uint32_t kind, uint32_t densityKgPerM3, uint32_t packedFractionPct,
                                uint32_t molarVolumeMm3PerMol, int64_t massMg) {
    const uint64_t volume = SfMaterialVolume(massMg, densityKgPerM3, molarVolumeMm3PerMol);
    if (kind == SF_SETTLE_GRAIN) {
        const uint32_t packed = packedFractionPct == 0u ? 100u : packedFractionPct;
        sum.grainVolume += (volume * 100u) / packed;
    }

    if (kind != SF_SETTLE_GAS)
        sum.condensedVolume += volume;

    if (kind == SF_SETTLE_ANCHORED && massMg > 0)
        sum.anchored = true;

    return sum;
}

FX_FN SfSettleSum SfAddSettle(SfSettleSum sum, uint32_t kind, uint32_t densityKgPerM3, uint32_t packedFractionPct,
                              int64_t massMg) {
    return SfAddSettleOf(sum, kind, densityKgPerM3, packedFractionPct, 0u, massMg);
}

FX_FN uint64_t SfSettledVolume(SfSettleSum sum) {
    return sum.grainVolume > sum.condensedVolume ? sum.grainVolume : sum.condensedVolume;
}

// 寄せるか: 据わる固体が無く、沈み固まった体積が 0 より大きく器より小さい
FX_FN bool SfSettles(uint64_t settledVolume, uint64_t capacityVolume, bool anchored) {
    return !anchored && settledVolume > 0 && settledVolume < capacityVolume;
}

// 詰まり(Q16。隣の列が傾きを読む)。据わる固体・物は詰まり切る
FX_FN int64_t SfSettleFullnessQ16(uint64_t settledVolume, uint64_t capacityVolume, bool anchored) {
    if (anchored)
        return SF_SETTLE_FULL_Q16;

    const uint64_t fullness = (settledVolume * (uint64_t)SF_SETTLE_FULL_Q16) / capacityVolume;

    return fullness < (uint64_t)SF_SETTLE_FULL_Q16 ? (int64_t)fullness : SF_SETTLE_FULL_Q16;
}

// 列の表面の高さ(段の下の端から。Q16): 段の下・段・段の上の詰まりの和 − 1。世界の外は下 = 詰まり切る・上 = 空で呼ぶ
FX_FN int64_t SfColumnSurfaceQ16(int64_t belowQ16, int64_t hereQ16, int64_t aboveQ16) {
    return belowQ16 + hereQ16 + aboveQ16 - SF_SETTLE_FULL_Q16;
}

// 表面の傾き(Q16。正の向きへ 1 セルで上がる高さ): 両側の差の小さい方・向きが違えば 0(MUSCL の minmod。山の頂や谷を作らない)。
// 片側が無い(世界の端)なら 0。
// 表面は詰まり(0〜1)の 3 つの和 − 1 なので −1〜2、差は ±3(Q16 で 2^18 未満)。32bit で計算する: 64bit の引き算と同じ値どうしの比べ
// (0 で止める・小さい方を選ぶ)を並べる形は HW の GPU で差の上の 32bit が壊れた(T-1078・T-1159。CLAUDE.md §4)ので、64bit の差を作らない(T-1176)
FX_FN int64_t SfSurfaceSlopeQ16(int64_t lowSideQ16, int64_t hereQ16, int64_t highSideQ16, bool bothSides) {
    if (!bothSides)
        return 0;

    FX_ASSERT(lowSideQ16 >= -(1 << 20) && lowSideQ16 <= (1 << 20));
    FX_ASSERT(hereQ16 >= -(1 << 20) && hereQ16 <= (1 << 20));
    FX_ASSERT(highSideQ16 >= -(1 << 20) && highSideQ16 <= (1 << 20));
    const int32_t here = (int32_t)hereQ16;
    const int32_t low = here - (int32_t)lowSideQ16;
    const int32_t high = (int32_t)highSideQ16 - here;
    if (low == 0 || high == 0 || (low > 0) != (high > 0))
        return 0;

    const int32_t lowMagnitude = low < 0 ? -low : low;
    const int32_t highMagnitude = high < 0 ? -high : high;

    return (int64_t)(lowMagnitude < highMagnitude ? low : high);
}

// 列の傾きを、n 列の詰まりが 0〜1 に収まるまで弱める(|g| ≤ min(r, 1 − r) × 2n ÷ (n − 1))。
// 詰まりは 0〜1・上限は 4 以下(2n ÷ (n − 1) ≤ 4)なので 32bit で(上と同じ理由。T-1176)。傾きは 32bit に収まる値で呼ぶ
FX_FN int64_t SfClampColumnSlopeQ16(int64_t fullnessQ16, int64_t slopeQ16, uint32_t columns) {
    FX_ASSERT(fullnessQ16 >= 0 && fullnessQ16 <= SF_SETTLE_FULL_Q16);
    FX_ASSERT(slopeQ16 >= -(int64_t)0x7FFFFFFF && slopeQ16 <= (int64_t)0x7FFFFFFF);
    FX_ASSERT(columns >= 2u && columns < 16384u);
    const int32_t fullness = (int32_t)fullnessQ16;
    const int32_t empty = (int32_t)SF_SETTLE_FULL_Q16 - fullness;
    const int32_t room = fullness < empty ? fullness : empty;
    const int32_t limit = (room * 2 * (int32_t)columns) / ((int32_t)columns - 1);
    const int32_t slope = (int32_t)slopeQ16;
    if (slope > limit)
        return (int64_t)limit;

    return (int64_t)(slope < -limit ? -limit : slope);
}

// 列 j(n 列の左から)の重み = 2n r + g (2j + 1 − n)(弱めた傾きで 0 以上)
FX_FN int64_t SfColumnWeight(int64_t fullnessQ16, int64_t clampedSlopeQ16, uint32_t columns, uint32_t column) {
    return (2 * (int64_t)columns * fullnessQ16) + (clampedSlopeQ16 * ((2 * (int64_t)column) + 1 - (int64_t)columns));
}

// 一番上の詰まった子(下から。その下の子は沈み固まった体積 = 子の体積)
FX_FN uint32_t SfSettleTopChild(uint64_t settledVolume, uint64_t childVolume) {
    FX_ASSERT(settledVolume > 0);
    return (uint32_t)((settledVolume - 1) / childVolume);
}

// 一番上より下の子 1 つへ寄せる量(列の量 × 子の体積 ÷ 沈み固まった体積)。一番上の子は残り
FX_FN int64_t SfSettleBelowShare(int64_t columnMassMg, uint64_t childVolume, uint64_t settledVolume) {
    return FxMulDivS64(columnMassMg, childVolume, settledVolume);
}

// 子の空きの体積(気体・舞う粒の重み): 子の体積 − 凝縮相の体積(0 で止める)。
// 64bit の引き算と同じ 2 つの値の比べを並べる形(比べて 0 を選ぶ・印を掛ける)は、HW の GPU で差の上の 32bit が別の値になった
// (差だけ・比べだけなら正しい。WARP と DXIL は正しい。T-1159。T-1078・ADR-0451 と同じ形)。32bit の 2 つに分けて借りを手で回す(同じ値。
// 体積は 2^63 未満なので、差の一番上のビットで負を見分ける)
FX_FN uint64_t SfOpenVolume(uint64_t childVolume, uint64_t condensedVolume) {
    const uint32_t childLow = (uint32_t)(childVolume & 0xFFFFFFFFu);
    const uint32_t childHigh = (uint32_t)(childVolume >> 32);
    const uint32_t condensedLow = (uint32_t)(condensedVolume & 0xFFFFFFFFu);
    const uint32_t condensedHigh = (uint32_t)(condensedVolume >> 32);
    const uint32_t borrow = childLow < condensedLow ? 1u : 0u;
    const uint32_t low = childLow - condensedLow;
    const uint32_t high = childHigh - condensedHigh - borrow;
    if ((high & 0x80000000u) != 0u || (high | low) == 0u)
        return 0;

    return ((uint64_t)high << 32) | (uint64_t)low;
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

// 欄ごと(T-1159): 眠る時に捨てる印(SF_FLAG_SLEEP_DISCARD)の欄は 0、ほかは SfSleepValue
FX_FN int64_t SfSleepValueOf(SfFieldDesc desc, int64_t mean) {
    return (desc.flags & SF_FLAG_SLEEP_DISCARD) != 0u ? (int64_t)0 : SfSleepValue(mean);
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

// --- 配り手の関数(T-1252。ADR-0500 の 1。CPU と GPU の 1 つの源。配り手は owner の決まった数の分かれで選ぶ = T-0397)-------------
// 細かくする: 頁を持つ親のセルの値 → 子 child(子の番号は MrChildCell の j)。一様な親 → 一様な子の値 1 つ(子 8 つが同じ値)。
// 粗くする:   子 8 つ → 親の値と余り(SfCoarsenSum の形。保存量の欄の余りは一般の段と同じく帳簿の「粗くした端数・出」へ)。
// 保存量の配り手は「子の和 = 親 × 8(子のレベルの単位)」と「粗く = 和 ÷ 8 + 余り」で和をビット単位で保つ。
// セルの中身を読む配り手(相の層の配り。T-1230)は、頁を持つ親の時だけセルの配りの口がセルを読んで書く(ADR-0512 の 1)。
// ここの関数はその配り手の一般の形(子が親の写しの時の値)

// 試験用の配り手の重み: 子 j は j + 1(和 36)。子 7 が残り(SfPlacedLastValue)
FX_CONST uint64_t SF_OWNER_TEST_WEIGHT_SUM = 36;

FX_FN int64_t SfOwnerTestChild(int64_t parentValue, uint32_t child) {
    if (child + 1u < SF_CHILDREN)
        return SfPlacedValue(parentValue, (uint64_t)(child + 1u), SF_OWNER_TEST_WEIGHT_SUM, SF_CHILDREN);

    int64_t given = 0;
    FX_UNROLL
    for (uint32_t j = 0; j + 1u < SF_CHILDREN; ++j)
        given += SfPlacedValue(parentValue, (uint64_t)(j + 1u), SF_OWNER_TEST_WEIGHT_SUM, SF_CHILDREN);

    return SfPlacedLastValue(parentValue, given, SF_CHILDREN);
}

FX_FN int64_t SfOwnerRefineChild(uint32_t owner, int64_t parentValue, uint32_t child) {
    if (owner == SF_OWNER_TEST)
        return SfOwnerTestChild(parentValue, child);

    if (owner == SF_OWNER_PHASE_LAYER)
        return parentValue;  // 子が親の写し(セルを読む配りは頁を持つ親の時にセルの配りの口が)

    return 0;
}

FX_FN int64_t SfOwnerRefineUniform(uint32_t owner, int64_t parentValue) {
    if (owner == SF_OWNER_TEST || owner == SF_OWNER_PHASE_LAYER)
        return parentValue;  // 一様な親の子は親と同じ数(子の和 = 親 × 8)

    return 0;
}

FX_FN SfCoarsenSum SfOwnerCoarsen(uint32_t owner, SfFamily family) {
    SfCoarsenSum sum = SfMakeCoarsenSum();
    if (owner != SF_OWNER_TEST && owner != SF_OWNER_PHASE_LAYER)
        return sum;

    FX_UNROLL
    for (uint32_t j = 0; j < SF_CHILDREN; ++j)
        sum = SfAddCoarsenChild(sum, family.values[j]);

    return sum;
}

// 観察の影を引き戻す(ADR-0361 の配り手の形。T-1252): 引き戻した子を配り手の粗くする関数で粗くすると親と同じ値・余り 0 になる。
// 粗く = 和 ÷ 8 の配り手(試験用・相の層の配り)では ADR-0361 の「子の和 = 親 × 8」と同じ(SfPullBackChild)。
// 粗く = 面の側の子の和の配り手(光の配り。T-1227)は「面の側の子の和 = 親」に合わせる関数を足す(使い手の作業)
FX_FN int64_t SfOwnerPullBackChild(uint32_t owner, int64_t parentValue, SfFamily family, uint32_t child) {
    if (owner == SF_OWNER_TEST || owner == SF_OWNER_PHASE_LAYER)
        return SfPullBackChild(parentValue, family, child);

    return 0;
}

SF_NAMESPACE_END

#endif  // BICAMERAL_STATE_FIELD_HLSLI
