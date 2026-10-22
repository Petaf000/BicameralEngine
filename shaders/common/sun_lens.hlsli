// sun_lens.hlsli — 日の光を物で集める: レンズの物を日が照らした時だけ、物の細かさで決まった数の光線を屈折で曲げて世界へ運ぶ
// (docs/design/23-radiation.md §5・§6・ADR-0383・ADR-0425。研究 R-R2・T-0878)。HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。
//
// データの流れ:
//   太陽の影の柱(sun_column.hlsli の SunWalkRayCaught)がレンズの物に入った量を光線ごとに「取られた」として返す
//   → CPU(engine/src/sim/sun_lens.cpp)が取られた量をレンズの格子の点(物の細かさの間隔)へ整数で分ける
//   → 点ごとに LnTraceLens: 前の球面で屈折(Snell・屈折の値は描画と同じ物質の値)→ ガラスの中の道のりで吸う → 後の球面で屈折
//   → LnWalkWorld: 世界の葉のセルを整数の DDA で進み、セルごとに e^−τ で吸う・返す(太陽の影の柱と同じ分け方)。箱を出た分は「外へ」
//   → セルに当たった所(入った面と、面の上の点を太陽の円盤の大きさでにじませた範囲)を、面の上の 2 mm の升へ面積の割合で分けて Hits へ。
//     当たった面の強さ(W/m²)= 升の量 ÷ 升の面の一番大きい所(T-0878 の 2 本目。1 本目の「外接の長方形で割る」は芯の強さを薄めた)
// 約束:
//   - 長さは µm(世界の座標の 0 はセルの座標の 0)・向きは Q30 の単位ベクトル。平方根は FxSqrtU64(整数の決まった手順)。浮動小数点は使わない。
//   - 保存: 光線ごとに 入った = レンズが吸った + レンズが返した + 世界が吸った + 世界が返した + 外へ(整数の引き算で分ける)。
//   - 足すのは可換な整数の足し算と min・max だけ(R4)。どの順に光線を走らせても同じ結果(R3)。
//   - 物の箱は世界の軸に揃えた形(回る物の箱は物の物理と組む時。T-0878 の申し送り)。
// Tree・Cells・Lights・Sink の約束は sun_column.hlsli と同じ。Hits の約束: void Add(slot, index, axis, spotU, spotV, energy)(升ごとに足す)。
#ifndef BICAMERAL_SUN_LENS_HLSLI
#define BICAMERAL_SUN_LENS_HLSLI

#include "sun_column.hlsli"

MR_NAMESPACE_BEGIN

// --- 大きさと単位 -------------------------------------------------------------------------------
FX_CONST int64_t LN_ONE_Q30 = 1073741824;  // 1.0(Q30)
FX_CONST int64_t LN_CELL0_UM = 500000;     // レベル 0 のセルの辺(0.5 m)
FX_CONST uint32_t LN_MAX_STEPS = 4096;     // 1 本の光線が進むセルの数の上限(超えたら残りを「外へ」にして印を付ける)
FX_CONST int64_t LN_SUN_DISK_RADIUS_Q30 = 4992899;  // 太陽の円盤の見かけの半径 0.00465 rad(焦点の点の大きさを決める)
// レンズの光線の量の単位 = 太陽の影の柱の単位(8^−rayLevel mJ)÷ 2^16。小さなレンズの量を数千本に分けても 0 にならないように。
// 世界のセルへの出力もこの単位(セルのエネルギーへ当てる段が、粗い葉への換算と一緒に端数を帳簿へ。T-0910 の申し送り (1))
FX_CONST uint32_t LN_ENERGY_SHIFT = 16;
// 当たった面の強さを数える升の辺(2 mm。仮・ADR-0425)。焦点の強さ = 升に入った量 ÷ 升の面。世界の µm の格子なので細かさによらない
FX_CONST int64_t LN_SPOT_UM = 2000;

// 3 つの int64(位置は µm・向きは Q30)
struct LnVec {
    int64_t x;
    int64_t y;
    int64_t z;
};

FX_FN LnVec LnMake(int64_t x, int64_t y, int64_t z) {
    LnVec v;
    v.x = x;
    v.y = y;
    v.z = z;

    return v;
}

FX_FN LnVec LnAdd(LnVec a, LnVec b) {
    return LnMake(a.x + b.x, a.y + b.y, a.z + b.z);
}

FX_FN LnVec LnSub(LnVec a, LnVec b) {
    return LnMake(a.x - b.x, a.y - b.y, a.z - b.z);
}

// v × scale(Q30。0 方向に切り捨て)
FX_FN LnVec LnScaleQ30(LnVec v, int64_t scaleQ30) {
    return LnMake(FxMulShiftS64(v.x, scaleQ30, 30), FxMulShiftS64(v.y, scaleQ30, 30), FxMulShiftS64(v.z, scaleQ30, 30));
}

// a · b ÷ 2^30(片方が Q30 の向きなら、もう片方の単位のまま)
FX_FN int64_t LnDotQ30(LnVec a, LnVec b) {
    return FxMulShiftS64(a.x, b.x, 30) + FxMulShiftS64(a.y, b.y, 30) + FxMulShiftS64(a.z, b.z, 30);
}

FX_FN int64_t LnAxisOf(LnVec v, uint32_t axis) {
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

FX_FN LnVec LnWithAxis(LnVec v, uint32_t axis, int64_t value) {
    return LnMake(axis == 0 ? value : v.x, axis == 1 ? value : v.y, axis == 2 ? value : v.z);
}

// 長さ 1(Q30)にする。成分は 2^31 未満であること(屈折の後の向きは長さがほぼ 1)
FX_FN LnVec LnNormalizeQ30(LnVec v) {
    const uint64_t ax = FxAbsU64(v.x);
    const uint64_t ay = FxAbsU64(v.y);
    const uint64_t az = FxAbsU64(v.z);
    FX_ASSERT(ax < ((uint64_t)1 << 31) && ay < ((uint64_t)1 << 31) && az < ((uint64_t)1 << 31));
    const uint32_t length = FxSqrtU64((ax * ax) + (ay * ay) + (az * az));
    FX_ASSERT(length != 0);

    return LnMake(FxDivS64(v.x * LN_ONE_Q30, (int64_t)length), FxDivS64(v.y * LN_ONE_Q30, (int64_t)length),
                  FxDivS64(v.z * LN_ONE_Q30, (int64_t)length));
}

// 整数の向き(太陽の SunLight::direction)→ Q30 の単位ベクトル
FX_FN LnVec LnUnitOf(int32_t x, int32_t y, int32_t z) {
    const uint64_t squared = FxAbsU64(x) * FxAbsU64(x) + FxAbsU64(y) * FxAbsU64(y) + FxAbsU64(z) * FxAbsU64(z);
    const uint64_t lengthQ15 = FxSqrtU64(squared << 30);

    return LnMake(FxDivS64((int64_t)x * ((int64_t)1 << 45), (int64_t)lengthQ15),
                  FxDivS64((int64_t)y * ((int64_t)1 << 45), (int64_t)lengthQ15),
                  FxDivS64((int64_t)z * ((int64_t)1 << 45), (int64_t)lengthQ15));
}

// 床への割り算(分母は正)
FX_FN int64_t LnFloorDivide(int64_t value, int64_t divisor) {
    if (value >= 0)
        return (int64_t)((uint64_t)value / (uint64_t)divisor);

    return -(int64_t)(((uint64_t)(-value) + (uint64_t)divisor - 1) / (uint64_t)divisor);
}

// レベル level のセルの辺(µm。レベル 5 まで割り切れる)
FX_FN int64_t LnCellUm(int32_t level) {
    return level >= 0 ? (LN_CELL0_UM >> (uint32_t)level) : (LN_CELL0_UM << (uint32_t)(-level));
}

// --- 球面との交わりと屈折 ------------------------------------------------------------------------

// 点 start から向き direction で、中心 centre・半径 radius の球面に当たるまでの道のり(µm。当たらなければ −1)。
// outward: 球の外から入る(近い方の交わり)/ 球の中から出る(遠い方の交わり)
FX_FN int64_t LnSphereHit(LnVec start, LnVec direction, LnVec centre, int64_t radiusUm, bool fromOutside) {
    const LnVec relative = LnSub(start, centre);
    const int64_t b = LnDotQ30(relative, direction);
    const int64_t c = (relative.x * relative.x) + (relative.y * relative.y) + (relative.z * relative.z) -
                      (radiusUm * radiusUm);
    const int64_t discriminant = (b * b) - c;
    if (discriminant < 0)
        return -1;

    const int64_t root = (int64_t)FxSqrtU64((uint64_t)discriminant);
    const int64_t along = fromOutside ? -b - root : -b + root;

    return along >= 0 ? along : -1;
}

// 屈折(Snell の法則のベクトルの形)。eta = 入る側の屈折の値 ÷ 出る側(Q30)。normal は面の法線(どちら向きでもよい)。
// 全反射なら鏡の向きに返す
FX_FN LnVec LnRefract(LnVec direction, LnVec normal, int64_t etaQ30) {
    int64_t cosIncident = -LnDotQ30(normal, direction);
    LnVec facing = normal;
    if (cosIncident < 0) {
        facing = LnMake(-normal.x, -normal.y, -normal.z);
        cosIncident = -cosIncident;
    }

    const int64_t sinSquaredIncident = LN_ONE_Q30 - FxMulShiftS64(cosIncident, cosIncident, 30);
    const int64_t sinSquaredOut = FxMulShiftS64(FxMulShiftS64(etaQ30, etaQ30, 30), sinSquaredIncident, 30);
    if (sinSquaredOut > LN_ONE_Q30)
        return LnNormalizeQ30(LnAdd(direction, LnScaleQ30(facing, 2 * cosIncident)));

    const int64_t cosOut = (int64_t)FxSqrtU64((uint64_t)(LN_ONE_Q30 - sinSquaredOut) << 30);
    const int64_t normalPart = FxMulShiftS64(etaQ30, cosIncident, 30) - cosOut;

    return LnNormalizeQ30(LnAdd(LnScaleQ30(direction, etaQ30), LnScaleQ30(facing, normalPart)));
}

// --- レンズ ------------------------------------------------------------------------------------

// レンズの物 1 つ(両凸。物の箱は世界の軸に揃える。CPU の MakeLensParams が物質の値から作る)
struct LnLens {
    // --- 形(µm)---
    int64_t centreX;
    int64_t centreY;
    int64_t centreZ;
    int64_t apertureUm;     // 口の半径
    int64_t frontRadiusUm;  // 日の側の球面の半径(凸)
    int64_t backRadiusUm;   // 反対の側の球面の半径(凸)
    int64_t thicknessUm;    // 真ん中の厚さ

    // --- 物質(ガラス・氷など。光の値は light.hlsli の値を物の密度で)---
    uint64_t depthShortQ32;  // 0.5 m の道のりの τ(短い帯)
    uint64_t depthLongQ32;
    uint32_t reflectanceShortQ16;
    uint32_t reflectanceLongQ16;
    uint32_t refractionQ16;  // 屈折の値(1.0 = 65536。描画と同じ値。S12)
    uint32_t axis;           // 光の軸(世界の軸 0〜2)
    int32_t facing;          // 日の側の面の法線 = facing × 軸(±1)
    uint32_t padding;
};

// 世界の箱と木(µm と木のレベル)
struct LnWorld {
    int64_t lowX;
    int64_t lowY;
    int64_t lowZ;
    int64_t highX;
    int64_t highY;
    int64_t highZ;
    int32_t finestLevel;  // 木の最も細かい本物のブロックのレベル(葉を引くレベル)
    int32_t rootLevel;
};

// 光線 1 本の量と帳簿(帯ごと。配列にしない: T-0397)
struct LnRay {
    LnVec position;
    LnVec direction;
    int64_t travelledUm;  // レンズを出てからの道のり(焦点の点のにじみ = 道のり × 太陽の円盤の半径)
    uint64_t energyShort;
    uint64_t energyLong;
    uint64_t lensAbsorbedShort;
    uint64_t lensAbsorbedLong;
    uint64_t lensReflectedShort;
    uint64_t lensReflectedLong;
    uint64_t absorbedShort;
    uint64_t absorbedLong;
    uint64_t reflectedShort;
    uint64_t reflectedLong;
    uint64_t escapedShort;
    uint64_t escapedLong;
    uint32_t unfinished;  // 1 = LN_MAX_STEPS で止めた(残りは「外へ」に入れた)
    uint32_t refracted;   // 1 = 両方の球面を通った
};

FX_FN LnRay LnMakeRay(LnVec position, LnVec direction, uint64_t energyShort, uint64_t energyLong) {
    LnRay ray;
    ray.position = position;
    ray.direction = direction;
    ray.travelledUm = 0;
    ray.energyShort = energyShort;
    ray.energyLong = energyLong;
    ray.lensAbsorbedShort = 0;
    ray.lensAbsorbedLong = 0;
    ray.lensReflectedShort = 0;
    ray.lensReflectedLong = 0;
    ray.absorbedShort = 0;
    ray.absorbedLong = 0;
    ray.reflectedShort = 0;
    ray.reflectedLong = 0;
    ray.escapedShort = 0;
    ray.escapedLong = 0;
    ray.unfinished = 0;
    ray.refracted = 0;

    return ray;
}

// 道のり lengthUm の τ(Q32)= 0.5 m の τ × 道のり ÷ 0.5 m(飽和は RD_DEPTH_SATURATED_Q32)
FX_FN uint64_t LnDepthQ32(uint64_t depthPerCellQ32, int64_t lengthUm) {
    if (lengthUm <= 0 || depthPerCellQ32 == 0)
        return 0;

    const FxU128 numerator = FxMulU64Full(depthPerCellQ32, (uint64_t)lengthUm);
    if (numerator.hi >= (uint64_t)LN_CELL0_UM)
        return RD_DEPTH_SATURATED_Q32;

    const uint64_t depth = FxDivU128By64(numerator, (uint64_t)LN_CELL0_UM).quotient;

    return depth < RD_DEPTH_SATURATED_Q32 ? depth : RD_DEPTH_SATURATED_Q32;
}

// セルの帯の値 → 0.5 m の τ(Q32。飽和させない形は sun_column.hlsli と同じ式)
FX_FN uint64_t LnCellDepthQ32(LtCellBand values) {
    if (values.absorption.hi != 0)
        return RD_DEPTH_SATURATED_Q32;

    const uint64_t depth = FxShiftRightU128(FxMulU64Full(values.absorption.lo, RD_DEPTH_RECIPROCAL), RD_DEPTH_SHIFT);

    return depth < RD_DEPTH_SATURATED_Q32 ? depth : RD_DEPTH_SATURATED_Q32;
}

// 帯 1 つを τ で通す(減った分 × 照り返しを返す・残りを吸う。SunPassSegment と同じ分け方)
FX_FN SunSegment LnPassDepth(uint64_t energy, uint64_t depthQ32, uint32_t reflectanceQ16) {
    SunSegment result;
    result.passed = energy;
    result.absorbed = 0;
    result.reflected = 0;
    if (energy == 0 || depthQ32 == 0)
        return result;

    // 光線の量は細かい単位(LN_ENERGY_SHIFT)なので、掛け算は 128bit で
    const uint32_t transmittance = SunTransmittanceQ16(depthQ32);
    result.passed = FxShiftRightU128(FxMulU64Full(energy, (uint64_t)transmittance), 16);
    const uint64_t removed = energy - result.passed;
    result.reflected = FxShiftRightU128(FxMulU64Full(removed, (uint64_t)reflectanceQ16), 16);
    result.absorbed = removed - result.reflected;

    return result;
}

// 日の側の面の法線(Q30)
FX_FN LnVec LnLensNormal(LnLens lens) {
    const int64_t unit = (int64_t)lens.facing * LN_ONE_Q30;

    return LnMake(lens.axis == 0 ? unit : 0, lens.axis == 1 ? unit : 0, lens.axis == 2 ? unit : 0);
}

// 光線 1 本をレンズに通す: 口の面の点 through を通る日の光(向き sun)が、前の球面で入り、ガラスの中で吸われ、後の球面で出る。
// 返す光線の position は後の球面を出た点(球面に当たらなければ口の面の点からまっすぐ)
FX_FN LnRay LnTraceLens(LnLens lens, LnVec through, LnVec sun, uint64_t energyShort, uint64_t energyLong) {
    const LnVec normal = LnLensNormal(lens);
    const LnVec centre = LnMake(lens.centreX, lens.centreY, lens.centreZ);
    const int64_t halfThickness = lens.thicknessUm / 2;
    const LnVec frontCentre = LnSub(centre, LnScaleQ30(normal, lens.frontRadiusUm - halfThickness));
    const LnVec backCentre = LnAdd(centre, LnScaleQ30(normal, lens.backRadiusUm - halfThickness));

    // --- 前の面より日の側から始める(軸に沿って厚さ + 口の半径だけ戻る)---
    const int64_t cosAxis = FxAbsU64(LnDotQ30(sun, normal)) == 0 ? 1 : (int64_t)FxAbsU64(LnDotQ30(sun, normal));
    const int64_t backUm = FxDivS64((lens.thicknessUm + lens.apertureUm) * LN_ONE_Q30, cosAxis);
    const LnVec start = LnSub(through, LnScaleQ30(sun, backUm));
    LnRay ray = LnMakeRay(through, sun, energyShort, energyLong);
    const int64_t front = LnSphereHit(start, sun, frontCentre, lens.frontRadiusUm, true);
    if (front < 0)
        return ray;

    // --- 前の面で入る(空気 → 物質)---
    const LnVec entry = LnAdd(start, LnScaleQ30(sun, front));
    const LnVec frontNormal = LnNormalizeQ30(LnSub(entry, frontCentre));
    const int64_t enterEta = (int64_t)(((uint64_t)1 << 46) / (uint64_t)lens.refractionQ16);
    const LnVec inside = LnRefract(sun, frontNormal, enterEta);
    const int64_t back = LnSphereHit(entry, inside, backCentre, lens.backRadiusUm, false);
    if (back < 0)
        return ray;

    // --- ガラスの中の道のりで吸う・返す ---
    const SunSegment shortBand = LnPassDepth(energyShort, LnDepthQ32(lens.depthShortQ32, back),
                                             lens.reflectanceShortQ16);
    const SunSegment longBand = LnPassDepth(energyLong, LnDepthQ32(lens.depthLongQ32, back), lens.reflectanceLongQ16);
    ray.energyShort = shortBand.passed;
    ray.energyLong = longBand.passed;
    ray.lensAbsorbedShort = shortBand.absorbed;
    ray.lensAbsorbedLong = longBand.absorbed;
    ray.lensReflectedShort = shortBand.reflected;
    ray.lensReflectedLong = longBand.reflected;

    // --- 後の面で出る(物質 → 空気)---
    const LnVec exitPoint = LnAdd(entry, LnScaleQ30(inside, back));
    const LnVec backNormal = LnNormalizeQ30(LnSub(exitPoint, backCentre));
    ray.position = exitPoint;
    ray.direction = LnRefract(inside, backNormal, (int64_t)lens.refractionQ16 << 14);
    ray.refracted = 1;

    return ray;
}

// --- 世界を進む ----------------------------------------------------------------------------------

// 今いる葉のセル(木の無い所は最も細かいレベルの空のセル。slot = MR_NO_BLOCK)
struct LnCell {
    uint32_t slot;
    uint32_t index;
    int64_t sizeUm;
    int64_t lowX;
    int64_t lowY;
    int64_t lowZ;
};

// 位置の軸の値を、進む向きが負なら 1 µm 手前で読む(面の上にいる時に、これから入るセルを引く)
FX_FN int64_t LnProbe(int64_t position, int64_t direction) {
    return direction < 0 ? position - 1 : position;
}

template <typename Tree>
FX_FN LnCell LnFindCell(Tree tree, LnWorld world, LnVec position, LnVec direction) {
    const int64_t px = LnProbe(position.x, direction.x);
    const int64_t py = LnProbe(position.y, direction.y);
    const int64_t pz = LnProbe(position.z, direction.z);
    const int64_t finestUm = LnCellUm(world.finestLevel);
    const uint32_t slot = SunFindContaining(tree, world.finestLevel, LnFloorDivide(px, finestUm),
                                            LnFloorDivide(py, finestUm), LnFloorDivide(pz, finestUm), world.rootLevel);
    LnCell cell;
    cell.slot = slot;
    cell.index = 0;
    cell.sizeUm = finestUm;
    if (slot != MR_NO_BLOCK)
        cell.sizeUm = LnCellUm(tree.Block(slot).level);

    const int64_t cx = LnFloorDivide(px, cell.sizeUm);
    const int64_t cy = LnFloorDivide(py, cell.sizeUm);
    const int64_t cz = LnFloorDivide(pz, cell.sizeUm);
    cell.lowX = cx * cell.sizeUm;
    cell.lowY = cy * cell.sizeUm;
    cell.lowZ = cz * cell.sizeUm;
    if (slot != MR_NO_BLOCK) {
        const MrBlock block = tree.Block(slot);
        cell.index = MrCellIndex((uint32_t)(cx - block.originX), (uint32_t)(cy - block.originY),
                                 (uint32_t)(cz - block.originZ));
    }

    return cell;
}

// 軸 1 つで、セルの境まで進む道のり(µm。切り上げ。向きが 0 なら −1)
FX_FN int64_t LnDistanceToFace(int64_t position, int64_t direction, int64_t low, int64_t sizeUm) {
    if (direction == 0)
        return -1;

    const int64_t face = direction > 0 ? low + sizeUm : low;
    const uint64_t gap = FxAbsU64(face - position);
    const uint64_t speed = FxAbsU64(direction);

    return (int64_t)(((gap << 30) + speed - 1) / speed);
}

// 範囲 [uLow, uHigh) × [vLow, vHigh) に当たった量を、面の上の決まった格子(LN_SPOT_UM 角。世界の µm の格子なので細かさによらない)の
// 升へ、重なった面積の割合で整数に分ける(余りは最初の升へ)。Hits の約束: void Add(slot, index, axis, spotU, spotV, energy)
template <typename Hits>
FX_FN void LnSpreadOverSpots(Hits hits, LnCell cell, uint32_t axis, uint64_t energy, int64_t uLow, int64_t uHigh,
                             int64_t vLow, int64_t vHigh) {
    const uint64_t total = (uint64_t)(uHigh - uLow) * (uint64_t)(vHigh - vLow);
    const int64_t firstU = LnFloorDivide(uLow, LN_SPOT_UM);
    const int64_t lastU = LnFloorDivide(uHigh - 1, LN_SPOT_UM);
    const int64_t firstV = LnFloorDivide(vLow, LN_SPOT_UM);
    const int64_t lastV = LnFloorDivide(vHigh - 1, LN_SPOT_UM);
    uint64_t given = 0;
    for (int64_t spotV = firstV; spotV <= lastV; ++spotV) {
        for (int64_t spotU = firstU; spotU <= lastU; ++spotU) {
            const int64_t lowU = spotU * LN_SPOT_UM > uLow ? spotU * LN_SPOT_UM : uLow;
            const int64_t highU = (spotU + 1) * LN_SPOT_UM < uHigh ? (spotU + 1) * LN_SPOT_UM : uHigh;
            const int64_t lowV = spotV * LN_SPOT_UM > vLow ? spotV * LN_SPOT_UM : vLow;
            const int64_t highV = (spotV + 1) * LN_SPOT_UM < vHigh ? (spotV + 1) * LN_SPOT_UM : vHigh;
            const uint64_t overlap = (uint64_t)(highU - lowU) * (uint64_t)(highV - lowV);
            const uint64_t share = FxDivU128By64(FxMulU64Full(energy, overlap), total).quotient;
            given += share;
            hits.Add(cell.slot, cell.index, axis, spotU, spotV, share);
        }
    }

    hits.Add(cell.slot, cell.index, axis, firstU, firstV, energy - given);
}

// 当たった点を面の上の 2 つの軸(U・V。入った軸でない 2 つを番号の順に)で、にじみ blur だけ広げ、セルの面に切って Hits へ
template <typename Hits>
FX_FN void LnRecordHit(Hits hits, LnCell cell, uint32_t entryAxis, LnVec position, int64_t blurUm, uint64_t energy) {
    const uint32_t axisU = entryAxis == 0 ? 1u : 0u;
    const uint32_t axisV = entryAxis == 2 ? 1u : 2u;
    const LnVec low = LnMake(cell.lowX, cell.lowY, cell.lowZ);
    const int64_t lowU = LnAxisOf(low, axisU);
    const int64_t lowV = LnAxisOf(low, axisV);
    const int64_t u = LnAxisOf(position, axisU);
    const int64_t v = LnAxisOf(position, axisV);
    const int64_t uLow = u - blurUm > lowU ? u - blurUm : lowU;
    const int64_t uHigh = u + blurUm < lowU + cell.sizeUm ? u + blurUm : lowU + cell.sizeUm;
    const int64_t vLow = v - blurUm > lowV ? v - blurUm : lowV;
    const int64_t vHigh = v + blurUm < lowV + cell.sizeUm ? v + blurUm : lowV + cell.sizeUm;
    LnSpreadOverSpots(hits, cell, entryAxis, energy, uLow, uHigh, vLow, vHigh);
}

// レンズを出た光線を世界のセルへ進める(セルごとに e^−τ で吸う・返す。箱を出たら「外へ」)
template <typename Tree, typename Cells, typename Lights, typename Sink, typename Hits>
FX_FN LnRay LnWalkWorld(Tree tree, Cells cells, Lights lights, Sink sink, Hits hits, LnWorld world, LnRay ray) {
    LnRay result = ray;
    const LnVec d = ray.direction;
    const uint64_t dx = FxAbsU64(d.x);
    const uint64_t dy = FxAbsU64(d.y);
    const uint64_t dz = FxAbsU64(d.z);
    uint32_t entryAxis = dx >= dy && dx >= dz ? 0u : (dy >= dz ? 1u : 2u);
    bool walking = result.energyShort != 0 || result.energyLong != 0;
    uint32_t steps = 0;
    for (; walking && steps < LN_MAX_STEPS; ++steps) {
        const LnVec p = result.position;
        const bool inside = p.x >= world.lowX && p.x < world.highX && p.y >= world.lowY && p.y < world.highY &&
                            p.z >= world.lowZ && p.z < world.highZ;
        if (!inside) {
            walking = false;
            continue;
        }

        // --- このセルの出口(一番近い面)---
        const LnCell cell = LnFindCell(tree, world, p, d);
        const int64_t tx = LnDistanceToFace(p.x, d.x, cell.lowX, cell.sizeUm);
        const int64_t ty = LnDistanceToFace(p.y, d.y, cell.lowY, cell.sizeUm);
        const int64_t tz = LnDistanceToFace(p.z, d.z, cell.lowZ, cell.sizeUm);
        uint32_t exitAxis = 0;
        int64_t along = tx;
        if (along < 0 || (ty >= 0 && ty < along)) {
            exitAxis = 1;
            along = ty;
        }

        if (along < 0 || (tz >= 0 && tz < along)) {
            exitAxis = 2;
            along = tz;
        }

        // --- セルで吸う・返す(吸わないセルは何もしない)---
        if (cell.slot != MR_NO_BLOCK) {
            const RxCell values = cells.Cell(cell.slot, cell.index);
            const LtCellBand shortValues = LtCellBandOf(lights, values, LT_BAND_SHORT);
            const LtCellBand longValues = LtCellBandOf(lights, values, LT_BAND_LONG);
            const uint64_t shortDepth = LnDepthQ32(LnCellDepthQ32(shortValues), along);
            const uint64_t longDepth = LnDepthQ32(LnCellDepthQ32(longValues), along);
            if (shortDepth != 0 || longDepth != 0) {
                const int64_t blurUm = FxMulShiftS64(result.travelledUm, LN_SUN_DISK_RADIUS_Q30, 30) + 1;
                LnRecordHit(hits, cell, entryAxis, p, blurUm, result.energyShort + result.energyLong);
                const SunSegment shortBand = LnPassDepth(result.energyShort, shortDepth, shortValues.reflectanceQ16);
                const SunSegment longBand = LnPassDepth(result.energyLong, longDepth, longValues.reflectanceQ16);
                if (shortBand.absorbed != 0 || shortBand.reflected != 0)
                    sink.Deposit(cell.slot, cell.index, LT_BAND_SHORT, shortBand.absorbed, shortBand.reflected);

                if (longBand.absorbed != 0 || longBand.reflected != 0)
                    sink.Deposit(cell.slot, cell.index, LT_BAND_LONG, longBand.absorbed, longBand.reflected);

                result.energyShort = shortBand.passed;
                result.energyLong = longBand.passed;
                result.absorbedShort += shortBand.absorbed;
                result.absorbedLong += longBand.absorbed;
                result.reflectedShort += shortBand.reflected;
                result.reflectedLong += longBand.reflected;
            }
        }

        // --- 次のセルへ(出る軸は面の上にちょうど置く)---
        const LnVec moved = LnAdd(p, LnScaleQ30(d, along));
        const int64_t face = LnAxisOf(d, exitAxis) > 0
                                 ? LnAxisOf(LnMake(cell.lowX, cell.lowY, cell.lowZ), exitAxis) + cell.sizeUm
                                 : LnAxisOf(LnMake(cell.lowX, cell.lowY, cell.lowZ), exitAxis);
        result.position = LnWithAxis(moved, exitAxis, face);
        result.travelledUm += along;
        entryAxis = exitAxis;
        walking = result.energyShort != 0 || result.energyLong != 0;
    }

    result.unfinished = walking ? 1u : 0u;
    result.escapedShort = result.energyShort;
    result.escapedLong = result.energyLong;

    return result;
}

// --- 燃え始めの仮の行(02 §9.4 の置き場が決まるまで、この試作の中だけ。ADR-0426)---
// 当たる熱の流れの強さ q(W/m²)を、溜める時間 τ(ms。物質の値)で溜める: 刻みごとに 溜めた量 += q × Δt − 溜めた量 × Δt ÷ τ。
// 燃え始めは 溜めた量 ≥ 燃え始める強さ × τ(一定の q なら q が燃え始める強さを超えた時に、時間 −τ ln(1 − q_c ÷ q) で起きる)。
// 単位は mJ/m²(W/m² × ms)。Δt = 1/60 s。溜める時間は 1 刻み(17 ms)以上(漏れる量 ≤ 溜めた量になるので、引き算は 0 を下回らない)
FX_CONST uint32_t LN_MIN_ACCUMULATE_MS = 17;

FX_FN uint64_t LnHeatDoseStep(uint64_t doseMilliJoules, uint64_t irradianceWatts, uint32_t accumulateMs) {
    FX_ASSERT(accumulateMs >= LN_MIN_ACCUMULATE_MS);
    const uint64_t added = (irradianceWatts * 1000) / (uint64_t)SUN_TICKS_PER_SECOND;
    const uint64_t leaked = FxDivU128By64(FxMulU64Full(doseMilliJoules, 1000),
                                          (uint64_t)SUN_TICKS_PER_SECOND * (uint64_t)accumulateMs)
                                .quotient;

    return doseMilliJoules - leaked + added;
}

FX_FN bool LnIgnites(uint64_t doseMilliJoules, uint32_t ignitionWatts, uint32_t accumulateMs) {
    return doseMilliJoules >= (uint64_t)ignitionWatts * (uint64_t)accumulateMs;
}

MR_NAMESPACE_END

#endif  // BICAMERAL_SUN_LENS_HLSLI
