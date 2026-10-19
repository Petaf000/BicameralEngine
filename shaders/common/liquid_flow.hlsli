// liquid_flow.hlsli — 液体の流れの芯(柱の静水圧・面の力・摩擦・移流・速さの上限・位置/運動のエネルギー。T-0716・土台の段 2 ①)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束。heat_conduction.hlsli と同じ形)。CPU リファレンス(engine/src/sim/probe_liquid.cpp)と
// GPU(shaders/sim/probe_liquid_tick.hlsl)が同じ関数を呼ぶので、式の二重書きは無い。違うのは回し方(CPU は区画を順に・GPU はセル/面ごとの
// スレッド)と、値の置き場(CPU の配列・GPU の liquidWork)だけ。模型の形と約束は engine/src/sim/probe_liquid.h(ADR-0266・ADR-0276・ADR-0268)。
//
// 入力は 3 つに分ける(後で多重解像度の 1 段の液体〔T-0717〕がそのまま呼べるように。runner/design/substrate-decided.md §2.2・§2.3):
//   - セルの見立て LqCellView: 刻みの初めのセルから作る液体の相の値(質量・体積・空き・熱)。永続しない。
//     土台のセルの見立て(MakeCellView。T-0713)ができたら、その液体の分をここへ渡す
//   - 面とセルの欄: 持ち越す状態(面の運動量・面の運動のエネルギー・セルの位置のエネルギー・前の刻みの質量)。関数は値で受けて値で返す
//   - 見立ての値 LqParams: 環境・レベル・模型の値(重力・セルの 1 辺・摩擦・溢れの倍率・速さの上限・移流)
// 周りの決まり(区画・面の列挙・成分の書き戻し・帳簿への足し方)は呼ぶ側に残す(段 1 ④ の面の帳簿と当てる段へ移すのは後)。
// 出ていく上限(LqLimitOutflow)は式だけここに置く(当てる段ができたらそちらへ)。欄が物質に付く式は気体と共通の
// follow_matter.hlsli の FmMomentumAfterMassChange(T-0718)。
//
// 単位: 質量 mg・体積 pL(= µmol × mm³/mol)・面の運動量は質量の単位(本当の運動量 kg·m/s = 値 × 1e-6 × g × dt ÷ 2)・
// エネルギー mJ(セルの熱は µJ)・開き Q16(LQ_OPEN_FULL = 全部)。浮動小数点は使わない(04 §4)。
#ifndef BICAMERAL_LIQUID_FLOW_HLSLI
#define BICAMERAL_LIQUID_FLOW_HLSLI

#include "common/fixed.hlsli"
#include "common/follow_matter.hlsli"
#include "common/units.hlsli"

#ifdef __cplusplus
#define LQ_NAMESPACE_BEGIN        \
    namespace bicameral::liquid { \
        using namespace ::bicameral::fx;
#define LQ_NAMESPACE_END }
#else
#define LQ_NAMESPACE_BEGIN
#define LQ_NAMESPACE_END
#endif

LQ_NAMESPACE_BEGIN

// --- 定数 ---

FX_CONST uint32_t LQ_OPEN_FULL = 65536;  // 割合の 1(Q16。sim/cell_volume の CELL_OPEN_FULL)

// 運動のエネルギー(mJ)= (面の運動量 × 重力加速度〔µm/s²〕)² ÷ (LQ_KINETIC_DENOMINATOR × 面の 2 倍の質量〔mg〕)。
// 本当の運動量 p = 値 × 1e-6 × g × dt ÷ 2・面の質量 M = 2 倍の質量 ÷ 2 から p² ÷ 2M を mJ に直した分母(14400 × 1e15)
FX_CONST uint64_t LQ_KINETIC_DENOMINATOR = (uint64_t)144 * 100000000 * 1000000000;

// 位置のエネルギー(mJ)= 質量(mg)× 高さ(mm)× 重力加速度(µm/s²)÷ 1e12
FX_CONST uint64_t LQ_POTENTIAL_DENOMINATOR = (uint64_t)1000000 * 1000000;

// --- 見立ての値(環境・レベル・模型。刻みの中は変わらない)---
struct LqParams {
    uint64_t gravityMicrometersPerSecond2;  // 0 でないこと(無重力の液体は断る)
    uint64_t cellEdgeMillimeters;           // セルの 1 辺(仮の世界は 500。多重解像度ではレベルの関数)
    uint32_t frictionShift;                 // 面の運動量を毎刻み 1/2^frictionShift 減らす(底と粘りの摩擦)
    uint32_t overfillStiffness;             // 体積が空きを超えたぶんの圧力の倍率
    uint32_t maxSpeedMillimetersPerSecond;  // 面の速さの上限
    uint32_t advection;                     // 1 = 運動量を液体と一緒に運ぶ
};

FX_FN LqParams LqMakeParams(uint64_t gravityMicrometersPerSecond2, uint64_t cellEdgeMillimeters, uint32_t frictionShift,
                            uint32_t overfillStiffness, uint32_t maxSpeedMillimetersPerSecond, uint32_t advection) {
    LqParams params;
    params.gravityMicrometersPerSecond2 = gravityMicrometersPerSecond2;
    params.cellEdgeMillimeters = cellEdgeMillimeters;
    params.frictionShift = frictionShift;
    params.overfillStiffness = overfillStiffness;
    params.maxSpeedMillimetersPerSecond = maxSpeedMillimetersPerSecond;
    params.advection = advection;

    return params;
}

// --- セルの見立て(液体の相の分。56 バイト。GPU の作業場に同じ並びで置く)---
struct LqCellView {
    uint64_t mass;        // mg(液体。1 mg 未満は流れない)
    uint64_t volume;      // pL(液体)
    uint64_t room;        // pL(液体が入れる体積 = セル − 固体)
    uint64_t equivalent;  // mg(空きが液体で満ちた時の質量をセル全部の液体に直したもの = 質量 ÷ 空き。圧力はこれで書く)
    int64_t heat;         // µJ(= エネルギー × 1000 − 化学)
    uint64_t capacity;    // nJ/K(セルの全部の成分)
    uint32_t open;        // Q16(液体の空き = 固体が占めない割合)
    uint32_t wall;        // 1 = 固体で満ちている(空き 0)
};

// --- 整数の道具 ---

// a × b ÷ c(切り捨て。結果が 64bit に収まること)
FX_FN uint64_t LqMulDiv(uint64_t a, uint64_t b, uint64_t c) {
    return FxDivU128By64(FxMulU64Full(a, b), c).quotient;
}

// value × numerator ÷ denominator(0 の向きへ切り捨て)
FX_FN int64_t LqScaleSigned(int64_t value, uint64_t numerator, uint64_t denominator) {
    return FxApplySign(LqMulDiv(FxAbsU64(value), numerator, denominator), value < 0);
}

// 床で割る(負の数も下へ。divisor は正)
FX_FN int64_t LqDivideDown(int64_t value, uint64_t divisor) {
    const uint64_t magnitude = FxAbsU64(value);
    const uint64_t quotient = magnitude / divisor;
    if (value >= 0)
        return (int64_t)quotient;

    return -(int64_t)(quotient + (magnitude % divisor != 0 ? 1 : 0));
}

// --- 見立てを作る(液体と固体の体積から)---

// セルの体積から占める分を引いた残り(pL。0 で止める)。64bit の「0 で止める引き算」は NVIDIA のドライバが誤って下ろすので(T-0185)、
// 符号付きの差を出して 0 以下を 0 にする。occupied は 2^62(sim/cell_volume の頭打ち)まで
FX_FN uint64_t LqRoomLeft(uint64_t occupied, uint64_t cellVolume) {
    const int64_t room = (int64_t)cellVolume - (int64_t)occupied;
    if (room <= 0)
        return 0;

    return (uint64_t)room;
}

// 液体の質量(mg)・体積(pL)・固体が占める体積(pL)・セルの体積(pL)・熱(µJ)・熱容量(nJ/K)から見立てを作る(T-0612・ADR-0268)。
// 空き(Q16)は切り捨て(sim/cell_volume の LiquidOpenQ16 と同じ値)
FX_FN LqCellView LqMakeCellView(uint64_t massMilligrams, uint64_t volume, uint64_t solid, uint64_t cellVolume,
                                int64_t heat, uint64_t capacity) {
    LqCellView view;
    view.mass = massMilligrams;
    view.volume = volume;
    view.room = LqRoomLeft(solid, cellVolume);
    view.open = (uint32_t)LqMulDiv(view.room, LQ_OPEN_FULL, cellVolume);
    view.wall = view.open == 0 ? 1u : 0u;
    view.equivalent = view.open == LQ_OPEN_FULL || view.open == 0 ? view.mass
                                                                  : LqMulDiv(view.mass, LQ_OPEN_FULL, view.open);
    view.heat = heat;
    view.capacity = capacity;

    return view;
}

// セルの液体の重さ(質量の単位。T-0759・ADR-0328)= 空きに収まる分だけ(体積が空きを超えたら 質量 × 空き ÷ 体積)。
// 溢れて縮んだ分は圧力を伝えるばね(溢れの圧力)で、重さを足さない。縮んだ分まで重さに数えると、蓋の下で圧力を運ぶために縮んだ柱は
// 隣の柱より段ごとに重くなり、どの段でも横に釣り合う静止の形が無くなる(蓋の下の柱が回り続ける)。重さを収まる分にすると、
// 満ちた柱どうしの段の間の重さの差は縮みによらず同じなので、溢れの圧力を 1 つの値にすればどの段でも釣り合う(静止の形がある)
FX_FN uint64_t LqWeight(LqCellView view) {
    if (view.volume <= view.room)
        return view.equivalent;

    return LqMulDiv(view.equivalent, view.room, view.volume);
}

// 面の開き(Q16): 両側の空きの小さい方(孔の形は読まない。T-0616)
FX_FN uint32_t LqFaceOpen(uint32_t openA, uint32_t openB) {
    return openA < openB ? openA : openB;
}

// --- 柱の静水圧(質量の単位。P = 2 × 支えている上の液体 + 自分 + 溢れ)---

struct LqColumnStep {
    int64_t pressure;     // このセルの圧力(mg)
    uint64_t above;       // 下のセルへ渡す「支えている上の液体」(mg)
    uint32_t overfilled;  // 1 = 液体の体積が空きを超えた
};

// 列を上から 1 セルずつ。above は上のセルが返した値(列の一番上は 0)。hasBelow = 0 は区画の床(全部支える)。
// 満ちていないセルは上の液体を満ち具合の分だけ支え(残りは落ちてくる力になる)、下に支えられている分だけ圧力になる
// (固体で満ちたセル・床は全部・液体は空きの満ち具合・空は 0。落ちている水は横へ押し広げない)。
// 孔の中の水の圧力 = 開けた水と同じ高さの圧力(空きの体積と「セル全部の液体に直した質量」で書く)
FX_FN LqColumnStep LqColumnPressure(LqCellView self, uint64_t above, uint32_t hasBelow, LqCellView below,
                                    LqParams params) {
    LqColumnStep result;
    result.pressure = 0;
    result.above = 0;
    result.overfilled = 0;
    if (self.wall != 0 || self.mass == 0)
        return result;

    // 溢れ = 体積 − 空き(0 で止める引き算を符号付きの差で。T-0185。体積は 2^63 未満)
    uint64_t overfill = 0;
    const int64_t excess = (int64_t)self.volume - (int64_t)self.room;
    if (excess > 0) {
        result.overfilled = 1;
        overfill = LqMulDiv(self.equivalent * params.overfillStiffness, (uint64_t)excess, self.volume);
    }

    const uint64_t filled = self.volume < self.room ? self.volume : self.room;
    const uint64_t supported = LqMulDiv(above, filled, self.room);

    uint64_t supportNumerator = 1;
    uint64_t supportDenominator = 1;
    if (hasBelow != 0 && below.wall == 0) {
        supportNumerator = below.volume < below.room ? below.volume : below.room;
        supportDenominator = below.room;
    }

    const uint64_t weight = LqWeight(self);
    result.pressure = (int64_t)LqMulDiv((2 * supported) + weight + overfill, supportNumerator, supportDenominator);
    result.above = supported + weight;

    return result;
}

// --- 面の力(圧力の差・重さ・壁・摩擦)と望む動く質量 ---

// 横の面で、セルの圧力が掛かる高さの割合(Q16。T-0757・ADR-0327)= 液体が空きを満たす割合(体積 ÷ 空き。1 で止める)。
// 液体はセルの底から溜まるので、横の面のうち液体が触れるのは下からこの高さだけ。満ちたセル(と溢れたセル)は 1
FX_FN uint64_t LqWetHeight(LqCellView view) {
    if (view.room == 0)
        return 0;

    const uint64_t filled = view.volume < view.room ? view.volume : view.room;

    return LqMulDiv(filled, LQ_OPEN_FULL, view.room);
}

// 横の面を押す力 = 両側の「圧力 × 濡れた高さの割合」の差(T-0757・ADR-0327)。
// 薄い層(満ち具合 φ)では φ² × 満ちた質量 の差になり、位置のエネルギー Σ 満ちた質量 × φ²(底に溜まった水の重心)の勾配と
// 一致する(浅い水の静水圧 ρ g h² ÷ 2)。満ちたセルどうしは今までの圧力の差と同じ。縦の面は割合を掛けない(床と天井は全部が濡れる)
FX_FN int64_t LqWetPressure(LqCellView view, int64_t pressure) {
    if (view.room == 0)
        return 0;

    const uint64_t filled = view.volume < view.room ? view.volume : view.room;

    return LqScaleSigned(pressure, filled, view.room);
}

FX_FN int64_t LqSidePush(LqCellView a, int64_t pressureA, LqCellView b, int64_t pressureB) {
    return LqWetPressure(a, pressureA) - LqWetPressure(b, pressureB);
}

// 面の力の結果。外から入った運動量の内訳(格子の軸の向き。和が帳簿の増え)
struct LqFaceForce {
    int64_t face;      // 力の後の面の運動量
    uint64_t desired;  // 面の望む動く質量(mg。上流から)
    int64_t wall;      // 壁・床・閉じた面の押し返し
    int64_t gravity;   // 重さ
    int64_t friction;  // 摩擦(底と粘り)
    int64_t dropped;   // まわりが空の面の運動量(行き場なし)
    int64_t carry;     // 持ち越す流れ(流れの分母の単位。符号は運動量の向き。坂がある 1 mg までの面だけ。T-0778)
};

FX_FN LqFaceForce LqEmptyFaceForce(int64_t face) {
    LqFaceForce force;
    force.face = face;
    force.desired = 0;
    force.wall = 0;
    force.gravity = 0;
    force.friction = 0;
    force.dropped = 0;
    force.carry = 0;

    return force;
}

// 1 刻みに動く質量(mg)= 面の運動量 × 重力加速度 × 刻みの長さ² ÷ (2 × セルの 1 辺)。
// 運動量 = 値 × g × dt ÷ 2、動く質量 = 運動量 × dt ÷ 1 辺 なので、分母 = 2 × 60² × 1 辺(µm)
FX_FN uint64_t LqFluxDenominator(LqParams params) {
    return (uint64_t)2 * TICKS_PER_SECOND * TICKS_PER_SECOND * params.cellEdgeMillimeters * 1000;
}

// セルの − 側の面が閉じている(区画の外・壁)時、その面を押す圧力は壁から返る(+ 向き)
FX_FN int64_t LqMinusWallPush(LqCellView self, int64_t pressure, uint32_t minusClosed) {
    return self.wall == 0 && minusClosed != 0 ? pressure : 0;
}

// + 側の面が閉じている(区画の外・どちらかのセルが固体で満ちている)。押す圧力と、残った運動量(壁が置かれた)は壁が受ける
FX_FN LqFaceForce LqClosedFace(int64_t face, LqCellView self, int64_t pressure) {
    LqFaceForce force = LqEmptyFaceForce(0);
    force.wall = (self.wall == 0 ? -pressure : 0) - face;

    return force;
}

// 摩擦で減らす運動量の大きさ(T-0776)= 大きさ ÷ 2^frictionShift を切り捨て、ただし 0 でない大きさは少なくとも 1 減らす(0 は越えない)。
// 切り捨てだけでは 2^frictionShift 未満の運動量が減らず、釣り合いの近くの小さな運動量が行き来し続けた(積んだ水が静止の丸めで眠らない・
// 蓋の下の小さな揺れ。T-0758・T-0759 の残り)。少なくとも 1 減らすので、力が掛からない面はどの大きさからも有限の刻みで 0 に着く
FX_FN uint64_t LqFrictionCut(uint64_t magnitude, uint32_t frictionShift) {
    const uint64_t cut = magnitude >> frictionShift;

    return cut == 0u && magnitude != 0u ? (uint64_t)1 : cut;
}

// + 側の開いた面(faceOpen > 0)。a = − 側(自分)・b = + 側の隣。vertical = 1 は縦の面(重さが掛かる)。
// 横の面は液体が触れる高さだけ押す(LqSidePush。濡れていない高さの分の圧力の差は壁の列へ: 帳簿の圧力の和は区画の端の押し返しで閉じる)。
// 一部だけ開いた面(孔)は力も重さも開いた割合だけ(押し切れない分は固体が受けて壁へ。T-0612)
FX_FN LqFaceForce LqOpenFaceForce(int64_t face, uint32_t faceOpen, uint32_t vertical, LqCellView a, int64_t pressureA,
                                  LqCellView b, int64_t pressureB, LqParams params) {
    LqFaceForce force = LqEmptyFaceForce(face);
    const int64_t fullPush = pressureA - pressureB;
    const int64_t push = vertical != 0 ? fullPush : LqSidePush(a, pressureA, b, pressureB);
    force.wall = push - fullPush;
    const int64_t weight = vertical != 0 ? (int64_t)(LqWeight(a) + LqWeight(b)) : 0;
    if (faceOpen == LQ_OPEN_FULL) {
        force.face += push - weight;
        force.gravity = -weight;
    } else {
        const int64_t openWeight = LqScaleSigned(weight, faceOpen, LQ_OPEN_FULL);
        const int64_t change = LqScaleSigned(push - weight, faceOpen, LQ_OPEN_FULL);
        force.face += change;
        force.gravity = -openWeight;
        force.wall += change - push + openWeight;
    }

    // まわりに液体が無い面の運動量は行き場が無い。移流なし(T-0609 の形)では上流が空の面も
    const uint64_t upwindMass = force.face > 0 ? a.mass : b.mass;
    if (a.mass + b.mass == 0 || (params.advection == 0 && upwindMass == 0)) {
        force.dropped = -force.face;
        force.face = 0;
        return force;
    }

    const int64_t slowed = FxApplySign(LqFrictionCut(FxAbsU64(force.face), params.frictionShift), force.face < 0);
    force.face -= slowed;
    force.friction = -slowed;

    // 上流が空: 運動量は残し(移流が液体と一緒に運ぶ)、この面は流れない
    if (upwindMass != 0)
        force.desired = LqMulDiv(FxAbsU64(force.face), params.gravityMicrometersPerSecond2, LqFluxDenominator(params));

    return force;
}

// --- 整数の釣り合い(離散の坂。T-0781・ADR-0341)---
// 質量は 1 mg 刻みでしか動かないので、力がちょうど 0 になる整数の状態が無い所がある(満ちたセルと満ちきらないセルの境: 1 mg 動かすと
// 「上の水が落ちる力」と「溢れの圧力」の間を行き来する)。そこでは残った力が面に積もり、1 mg 動いては行き過ぎて戻るのを繰り返した(T-0776 の測り)。
// 坂の原理の整数の版: 面の運動量がこの刻みに最小の量(1 mg)までしか動かさない大きさの時、1 mg をその向きに動かした後の力を計算し直し、
// 今の力との和(台形の近似で、動かした時のエネルギーの減り × 2 ÷ 1 mg)が動く向きに正でなければ、そこが整数の釣り合い(坂が無い)。
// 面の運動量は 0 にする(帳簿は摩擦の列。運動のエネルギーは刻みの終わりの計算し直しで熱へ)。1 mg の大きさでは勢いで坂を登る動き(波)を
// 格子が表せない(1 mg を動かす運動量は 0.5 m のセルで約 0.24 µm/s)ので、そこで止める。動いている水(1 刻みに 2 mg 以上動かす面)には触らない。
// 坂がある面は、1 刻みに動く質量の 1 mg 未満の端数を面の欄に持ち越す(T-0778・ADR-0357)。軽い水(宙の数 mg・満ちきらないセルの上の数 mg)は
// 摩擦と釣り合う運動量が 1 刻みに 1 mg に届かず、端数を毎刻み捨てると運動量だけ残って落ちきらなかった。端数を持つのは坂がある 1 mg までの面だけ
// (釣り合い・2 mg 以上動かす面・向きが変わった面は 0 から数え直す)なので、釣り合いの判定と組んで行き来しない。
// 1 mg までの面の質量は、面ごとの判定が正しいように 1 刻みに各セルで 1 つの面だけが動かす(LqHoldTurn。Jacobi の同時の動きで、
// 3 mg 足りないセルへ 4 つの隣が 1 mg ずつ入って溢れ、押し返されて戻るのを繰り返した。T-0778)。順番の刻みには持ち越した流れの整数の mg を
// まとめて動かすので、流れの速さは運動量のまま(順番は速さに上限を作らない。6 刻み = 0.1 秒の粒にまとめるだけ)。判定は丸めの幅を越えて
// 下がる時だけ(LQ_BALANCE_MARGIN。1 mg の入れ替えは丸めで両向きとも「下がる」と出て、余った 1 mg が行き来した)。

// 1 mg までの面が質量を動かせる刻みの巡り(軸 3 × 偶奇 2)。ある刻みに動かせるのは、軸の組 group の面のうち − 側のセルの
// その軸の座標の偶奇が合う面だけ = どのセルも、その刻みに動かせる面がちょうど 1 つ(その軸の + か − の面)
FX_CONST uint32_t LQ_HOLD_TURNS = 6;

// 判定の丸めの幅: 今の力と動かした後の力はどちらも切り捨ての差で 1 未満ずれるので、和(動く向き)が 2 以上の時だけ下がると見る
FX_CONST int64_t LQ_BALANCE_MARGIN = 2;

// 1 mg までの面が持ち越せる流れの上限(mg)。1 刻みの流れ < 2 mg を順番の 1 巡(6 刻み)溜めた量 < 12 mg(粗く/細かくした後も)
FX_CONST uint64_t LQ_HOLD_CARRY_MG = 2 * LQ_HOLD_TURNS;

// 世界の刻み tick に、軸の組 group(0〜2。仮の世界の格子の x・y・z の順)の面で − 側のセルの座標 coordinate が質量を動かせるか。
// 順番は世界の時計で決める(液体の状態に数を足さない: 何も無い世界・眠った区画の状態はビット単位で同じのまま)
FX_FN uint32_t LqHoldTurn(uint64_t tick, uint32_t group, uint32_t coordinate) {
    return (uint32_t)(tick % LQ_HOLD_TURNS) == (group * 2u) + (coordinate & 1u) ? 1u : 0u;
}

// 釣り合いを見る面の片側: 見立て・圧力・上から支えている液体(LqColumnPressure の above に渡した値)・下のセル(hasBelow = 0 は区画の床)
struct LqBalanceSide {
    LqCellView view;
    LqCellView below;
    uint64_t above;
    int64_t pressure;
    uint32_t hasBelow;
};

FX_FN LqBalanceSide LqMakeBalanceSide(LqCellView view, int64_t pressure, uint64_t above, uint32_t hasBelow,
                                      LqCellView below) {
    LqBalanceSide side;
    side.view = view;
    side.below = below;
    side.above = above;
    side.pressure = pressure;
    side.hasBelow = hasBelow;

    return side;
}

// amount mg 増やした(gain = 1)・減らした見立て。体積は動く液体の上流の密度で(movedVolume)。空き・開きは変わらない
FX_FN LqCellView LqShiftedView(LqCellView view, uint32_t gain, uint64_t amount, uint64_t movedVolume) {
    view.mass = gain != 0 ? view.mass + amount : view.mass - amount;
    view.volume = gain != 0 ? view.volume + movedVolume : view.volume - movedVolume;
    view.equivalent = view.open == LQ_OPEN_FULL || view.open == 0 ? view.mass
                                                                  : LqMulDiv(view.mass, LQ_OPEN_FULL, view.open);

    return view;
}

// 面を押す力(+ 向き。開いた割合は掛けない: 符号だけを見る)。縦は圧力の差 − 重さ、横は濡れた高さの分の圧力の差
FX_FN int64_t LqNetPush(uint32_t vertical, LqCellView a, int64_t pressureA, LqCellView b, int64_t pressureB) {
    if (vertical != 0)
        return pressureA - pressureB - (int64_t)(LqWeight(a) + LqWeight(b));

    return LqSidePush(a, pressureA, b, pressureB);
}

// amount mg(上流の中身以下)を forward(1 = a から b へ)に動かした後の面の力(+ 向き)。
// 縦の面は b が a の真上(b の下 = a・a の上から支える液体 = b が渡す値)
FX_FN int64_t LqPushAfterShift(uint32_t vertical, LqBalanceSide a, LqBalanceSide b, uint32_t forward, uint64_t amount,
                               LqParams params) {
    const uint64_t fromVolume = forward != 0 ? a.view.volume : b.view.volume;  // HLSL の ?: は構造体を選べない
    const uint64_t fromMass = forward != 0 ? a.view.mass : b.view.mass;
    const uint64_t movedVolume = LqMulDiv(fromVolume, amount, fromMass);
    const LqCellView shiftedA = LqShiftedView(a.view, forward != 0 ? 0u : 1u, amount, movedVolume);
    const LqCellView shiftedB = LqShiftedView(b.view, forward != 0 ? 1u : 0u, amount, movedVolume);
    if (vertical != 0) {
        const LqColumnStep upper = LqColumnPressure(shiftedB, b.above, 1u, shiftedA, params);
        const LqColumnStep lower = LqColumnPressure(shiftedA, upper.above, a.hasBelow, a.below, params);
        return LqNetPush(1u, shiftedA, lower.pressure, shiftedB, upper.pressure);
    }

    const LqColumnStep columnA = LqColumnPressure(shiftedA, a.above, a.hasBelow, a.below, params);
    const LqColumnStep columnB = LqColumnPressure(shiftedB, b.above, b.hasBelow, b.below, params);

    return LqNetPush(0u, shiftedA, columnA.pressure, shiftedB, columnB.pressure);
}

// 開いた面の力の後(LqOpenFaceForce の結果)に呼ぶ。運動量がこの刻みに 1 mg までしか動かさない大きさの面で、1 mg 動かしてもエネルギーが下がらなければ
// (今の力 + 動かした後の力 が動く向きに正でない・上流に 1 mg も無い)運動量を 0 にする(摩擦の列へ。望む動く質量も 0)。
// 坂があれば、前の刻みの持ち越し carry(force.carry と同じ形)を足し、順番の刻み(turn = LqHoldTurn)なら溜まった整数の mg を動かす。
// 残りを force.carry に返す(LQ_HOLD_CARRY_MG で止める)
FX_FN LqFaceForce LqHoldAtBalance(LqFaceForce force, int64_t carry, uint32_t turn, uint32_t vertical, LqBalanceSide a,
                                  LqBalanceSide b, LqParams params) {
    // 運動量の大きさで見る(上流が空で望む質量が 0 の面も、大きな運動量は移流が運ぶので触らない。T-0610)
    const uint64_t denominator = LqFluxDenominator(params);
    const uint64_t reach = LqMulDiv(FxAbsU64(force.face), params.gravityMicrometersPerSecond2, denominator);
    if (reach > 1 || force.face == 0)
        return force;

    const uint32_t forward = force.face > 0 ? 1u : 0u;
    const uint64_t fromMass = forward != 0 ? a.view.mass : b.view.mass;
    if (fromMass != 0) {
        const int64_t now = LqNetPush(vertical, a.view, a.pressure, b.view, b.pressure);
        const int64_t after = LqPushAfterShift(vertical, a, b, forward, 1u, params);
        const int64_t along = forward != 0 ? now + after : -(now + after);
        if (along >= LQ_BALANCE_MARGIN) {
            // 坂がある(動かすとエネルギーが下がる): 同じ向きの持ち越しを足し、順番の刻みに溜まった整数の mg を動かす
            // (流れの速さは運動量のまま。順番は同時に動く面を各セル 1 つにするだけで、速さに上限を作らない。T-0778 の 2 回目)。
            // reach ≤ 1 なので 1 刻みの流れ < 2 mg、順番の 1 巡で < 2 × 6 mg(持ち越しの上限 LQ_HOLD_CARRY_MG。64bit に収まる)
            const uint64_t limit = LQ_HOLD_CARRY_MG * denominator;
            const uint64_t magnitude = FxAbsU64(carry);
            const bool sameWay = carry != 0 && (carry > 0) == (force.face > 0);
            const uint64_t kept = sameWay ? (magnitude < limit ? magnitude : limit) : (uint64_t)0;
            uint64_t flux = (FxAbsU64(force.face) * params.gravityMicrometersPerSecond2) + kept;
            force.desired = 0;
            if (turn != 0 && flux >= denominator) {
                // 溜まった amount mg(上流の中身まで)を動かしても下がるなら全部、下がらなければ 1 mg(上で確かめた)
                const uint64_t whole = flux / denominator;
                const uint64_t amount = whole < fromMass ? whole : fromMass;
                force.desired = 1;
                if (amount > 1) {
                    const int64_t afterAll = LqPushAfterShift(vertical, a, b, forward, amount, params);
                    const int64_t alongAll = forward != 0 ? now + afterAll : -(now + afterAll);
                    force.desired = alongAll >= LQ_BALANCE_MARGIN ? amount : (uint64_t)1;
                }

                flux -= force.desired * denominator;
            }

            force.carry = FxApplySign(flux < limit ? flux : limit, force.face < 0);
            return force;
        }
    }

    force.friction -= force.face;
    force.face = 0;
    force.desired = 0;

    return force;
}

// --- 周りの決まり(式だけ。段 1 ④ の当てる段へ移す)---

// 出ていく上限: 出る望む合計 outgoing が上流の中身 fromMass を超えれば、面の望む質量を比で縮める
FX_FN uint64_t LqLimitOutflow(uint64_t desired, uint64_t fromMass, uint64_t outgoing) {
    return outgoing > fromMass ? LqMulDiv(desired, fromMass, outgoing) : desired;
}

// --- 液体と一緒に動くもの ---

// 上流のセル(液体 fromMass mg)から mass mg を動かす時に一緒に動く量(成分の物質量・位置のエネルギー)
FX_FN uint64_t LqMovedShare(uint64_t value, uint64_t mass, uint64_t fromMass) {
    return LqMulDiv(value, mass, fromMass);
}

// 動く液体のぶんのエネルギー(mJ): 熱は動く熱容量の比で(液体とセルの温度を同じと置く)+ 動く化学(µJ)。mJ へは床で
// (渡す側の熱が負にならない)。movedCapacity が 0 なら呼ばない
FX_FN int64_t LqMovedEnergy(LqCellView from, uint64_t movedCapacity, int64_t movedChemical) {
    const int64_t movedHeat = (int64_t)LqMulDiv((uint64_t)from.heat, movedCapacity, from.capacity);

    return LqDivideDown(movedHeat + movedChemical, 1000);
}

// --- 運動量の移流(T-0610・ADR-0276)---

struct LqCarried {
    int64_t momentum;  // 面 from から運ぶ運動量
    int64_t energy;    // 一緒に運ぶ運動のエネルギー(mJ)
};

// 面 from の運動量 faceBefore と運動のエネルギー kineticBefore を、面のまわりの体積の境を通る 2 倍の流れ share で風上に運ぶ。
// 面から出る 2 倍の流れの合計 leaving が面の 2 倍の質量 doubledMass を超えれば比で縮める(符号が反転しない)
FX_FN LqCarried LqAdvectShare(int64_t faceBefore, int64_t kineticBefore, uint64_t share, uint64_t doubledMass,
                              uint64_t leaving) {
    LqCarried carried;
    carried.momentum = 0;
    carried.energy = 0;
    if (faceBefore == 0)
        return carried;

    const uint64_t denominator = doubledMass > leaving ? doubledMass : leaving;
    const int64_t magnitude = (int64_t)LqMulDiv(FxAbsU64(faceBefore), share, denominator);
    carried.momentum = faceBefore > 0 ? magnitude : -magnitude;
    carried.energy = (int64_t)LqMulDiv((uint64_t)kineticBefore, share, denominator);

    return carried;
}

// --- 位置と運動のエネルギー・速さの上限(mJ。セルのエネルギーの外)---

// セルの中心の高さ(mm。区画の床から)
FX_FN uint64_t LqCellHeightMillimeters(uint32_t y, LqParams params) {
    return ((uint64_t)y * params.cellEdgeMillimeters) + (params.cellEdgeMillimeters / 2);
}

FX_FN int64_t LqPotentialEnergy(uint64_t massMilligrams, uint64_t heightMillimeters, LqParams params) {
    return (int64_t)LqMulDiv(massMilligrams * heightMillimeters, params.gravityMicrometersPerSecond2,
                             LQ_POTENTIAL_DENOMINATOR);
}

FX_FN int64_t LqKineticEnergy(int64_t momentum, uint64_t doubledMass, LqParams params) {
    if (momentum == 0 || doubledMass == 0)
        return 0;

    const FxU128 scaled = FxMulU64Full(FxAbsU64(momentum), params.gravityMicrometersPerSecond2);
    FX_ASSERT(scaled.hi == 0);  // 速さの上限で収まる(上限 30 m/s・2 倍の質量 1e12 mg まで)
    const FxU128 squared = FxMulU64Full(scaled.lo, scaled.lo);

    return (int64_t)(FxDivU128By64(squared, LQ_KINETIC_DENOMINATOR).quotient / doubledMass);
}

// 速さ(µm/s)→ 面の運動量の上限 = 速さ × 刻み/秒 × 2 倍の質量 ÷ 重力加速度
FX_FN uint64_t LqMaxMomentum(uint64_t doubledMass, LqParams params) {
    const uint64_t speedMicrometers = (uint64_t)params.maxSpeedMillimetersPerSecond * 1000;

    return LqMulDiv(speedMicrometers * TICKS_PER_SECOND, doubledMass, params.gravityMicrometersPerSecond2);
}

// 刻みの終わりの面: まわりが空になった面・速さの上限・運動のエネルギーを計算し直し、運んできた運動のエネルギーとの差を熱へ
struct LqSettledFace {
    int64_t face;     // 直した面の運動量
    int64_t dropped;  // まわりが空になって捨てた運動量(帳簿へ)
    int64_t capped;   // 速さの上限で止めた運動量(帳簿へ)
    int64_t kinetic;  // 面の運動のエネルギー(持ち越す)
    int64_t
        released;  // 熱になる分(運んできた運動のエネルギー − 今の値。内側の面は両側のセルへ LqReleasedShare で分ける)
    uint32_t cappedFace;
};

FX_FN LqSettledFace LqSettleFace(int64_t face, uint64_t doubledMass, int64_t carriedKinetic, LqParams params) {
    LqSettledFace settled;
    settled.face = face;
    settled.dropped = 0;
    settled.capped = 0;
    settled.cappedFace = 0;
    if (face != 0 && doubledMass == 0) {
        settled.dropped = -face;
        settled.face = 0;
    }

    const uint64_t limit = LqMaxMomentum(doubledMass, params);
    if (FxAbsU64(settled.face) > limit) {
        const int64_t kept = settled.face > 0 ? (int64_t)limit : -(int64_t)limit;
        settled.capped = -(settled.face - kept);
        settled.face = kept;
        settled.cappedFace = 1;
    }

    settled.kinetic = LqKineticEnergy(settled.face, doubledMass, params);
    settled.released = carriedKinetic - settled.kinetic;

    return settled;
}

// 内側の面の熱のうち + 側の隣へ渡す分(床で半分。− 側のセルは released − これ)
FX_FN int64_t LqReleasedShare(int64_t released) {
    return LqDivideDown(released, 2);
}

LQ_NAMESPACE_END

#endif  // BICAMERAL_LIQUID_FLOW_HLSLI
