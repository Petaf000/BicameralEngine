// cascade_light.hlsli — 世界の中の光: 距離ごとの段(Radiance Cascades の型)の共通の核(docs/design/23-radiation.md §3.3・ADR-0413。T-0877 R-R1)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU リファレンス(engine/src/sim/cascade_light.cpp)が呼ぶ。GPU 版は T-0958。
//
// データの流れ:
//   セルの温度・帯ごとの吸い込み(light.hlsli の LtCellBandOf → RdOpticalDepthQ32)→ 面ごとに RcFaceEmission(σT⁴ × 帯の割合 × α(1 − ρ)。Kirchhoff)
//   + 前の刻みにその面へ返された光(23 §3.4)→ 面の Lambert の重み(RcFaceWeight)で向きの升目(立方体の 6 面を n × n に分けたもの)へ整数で分ける
//   → 光線 1 本ごとに RcWalkRay: 向きの一番大きい軸 A の板(セル 1 枚)ごとに、板の真ん中の点を含むセルを引き、e^−τ で通し、
//     減った分 × 照り返しを「返す」、残りを「吸う」(Sink の Deposit)。区間の終わりで残った量を返す(呼ぶ側が次の段の袋へ)
// 約束:
//   - 段 k の場所の粗さ = 世界の木のレベル −k のブロック(2^k セル)。向きの升目 = 1 面 n 2^k(1 つの升目を次の段で 4 つに分ける)。
//     区間の長さはおよそ 2^k(intervalBlocks で決まる)なので、区間の終わりの光線の間隔がどの段でも同じ(Radiance Cascades の型)。
//   - 段 k から k + 1 へは、区間の終わりに残った量を (段 k + 1 のブロック・升目) の袋に足す。袋は量で重み付けた出どころの重心も持ち、
//     段 k + 1 の光線は重心から出る(1 つの焚き火なら焚き火から。ブロックの真ん中へずらさないので遠くの受け手の量が場所でずれない)。
//   - 保存: 光線ごとに 入った = 吸った + 返した + 区間の終わりに残った + 外へ(整数の引き算で分けるのでビット単位)。
//   - 透過が入った量の 1/16 未満になったら、残りをそのセルの「吸った」へ足して止める(ADR-0262 と同じ止め方)。
//   - 向きの升目の中の位置は (種・刻み・袋・升目) のハッシュでずらす(R6)。刻みごとに筋の場所が変わり、熱容量で均される(D-443 ②)。
// Grid の約束: bool Contains(int64_t x, int64_t y, int64_t z) と RcCellLight Light(int64_t x, int64_t y, int64_t z)。
// Sink の約束: void Deposit(int64_t x, int64_t y, int64_t z, uint32_t band, uint32_t face, uint64_t absorbed, uint64_t reflected)
//   (face = 光が入った面。返した光は次の刻みにその面から放つ。23 §3.4)。
// 量の単位は 2^−16 mJ(RC_ENERGY_SHIFT)。浮動小数点は使わない。
#ifndef BICAMERAL_CASCADE_LIGHT_HLSLI
#define BICAMERAL_CASCADE_LIGHT_HLSLI

#include "light.hlsli"

RX_NAMESPACE_BEGIN

// --- 単位 --------------------------------------------------------------------------------------
FX_CONST uint32_t RC_ENERGY_SHIFT = 16;  // 量は 2^−16 mJ
FX_CONST uint32_t RC_FACES = 6;
FX_CONST uint32_t RC_POSITION_SHIFT = 8;       // 位置は 1/256 セル
FX_CONST uint32_t RC_JITTER = 16;              // 升目の中のずらしの細かさ(1 辺を 16 に)
FX_CONST uint32_t RC_STOP_FRACTION_SHIFT = 4;  // 1/16 未満で止める

// 黒い面 1 つ(0.25 m²)が 1 刻み(1/60 s)に放つ量 = σ T⁴ × 0.25 m² ÷ 60(2^−16 mJ)。
// t = T[mK] >> 4、q = (t²)² >> 32 と置くと 量 = (q × RC_FACE_BLACK_SCALE) >> 32。定数 = σ × 0.25 ÷ 60 × 1000 × 2^16 × 0.016^4 × 2^64
FX_CONST uint64_t RC_FACE_BLACK_SCALE = FX_U64(0x00000004u, 0x5BBB95C8u);  // 18,718,889,416
FX_CONST uint32_t RC_TEMPERATURE_LIMIT_MK = 4095000;                       // 法則の放射は 4095 K まで(D-588)

// 短い帯(< 0.7 µm)の黒体の割合 F(0.7 µm × T)(Q32)。T = 0, 64, …, 4096 K(65 個。Planck の分布の積分。ベイクした物理の定数の表)
FX_CONST uint32_t RC_BAND_STEP_MK = 64000;
FX_CONST uint32_t RC_SHORT_FRACTION_Q32[65] = {
    0u,          0u,         0u,         0u,         0u,         0u,         0u,         0u,         0u,
    0u,          0u,         3u,         33u,        210u,       994u,       3767u,      11949u,     32761u,
    79605u,      174812u,    352405u,    660427u,    1162473u,   1938167u,   3082486u,   4704006u,   6922228u,
    9864259u,    13661108u,  18443897u,  24340205u,  31470734u,  39946447u,  49866234u,  61315161u,  74363272u,
    89064930u,   105458637u, 123567258u, 143398587u, 164946196u, 188190466u, 213099779u, 239631788u, 267734732u,
    297348751u,  328407182u, 360837793u, 394563953u, 429505727u, 465580867u, 502705724u, 540796056u, 579767748u,
    619537437u,  660023054u, 701144283u, 742822950u, 784983334u, 827552423u, 870460115u, 913639356u, 957026255u,
    1000560135u, 1044183572u};

// --- 帯ごとの放つ量 ------------------------------------------------------------------------------

// セル 1 つの光の値(呼ぶ側が LtCellBandOf と RdOpticalDepthQ32 で作る。τ はセルの厚さ 0.5 m の分)
struct RcCellLight {
    uint64_t depthShortQ32;  // RD_DEPTH_SATURATED_Q32 で飽和
    uint64_t depthLongQ32;
    uint32_t reflectanceShortQ16;
    uint32_t reflectanceLongQ16;
};

struct RcBands {
    uint64_t shortBand;
    uint64_t longBand;
};

FX_FN uint64_t RcFaceBlackEmission(uint32_t temperatureMk) {
    const uint64_t clamped = temperatureMk < RC_TEMPERATURE_LIMIT_MK ? temperatureMk : RC_TEMPERATURE_LIMIT_MK;
    const uint64_t t = clamped >> 4;                                            // < 2^18
    const uint64_t quartic = FxShiftRightU128(FxMulU64Full(t * t, t * t), 32);  // < 2^40

    return FxShiftRightU128(FxMulU64Full(quartic, RC_FACE_BLACK_SCALE), 32);
}

// 短い帯の割合(Q32。表の間は整数の比例で)
FX_FN uint64_t RcShortFractionQ32(uint32_t temperatureMk) {
    const uint32_t clamped = temperatureMk < RC_TEMPERATURE_LIMIT_MK ? temperatureMk : RC_TEMPERATURE_LIMIT_MK;
    const uint32_t index = clamped / RC_BAND_STEP_MK;
    const uint64_t rest = clamped - (index * RC_BAND_STEP_MK);
    const uint64_t low = RC_SHORT_FRACTION_Q32[index];
    const uint64_t high = RC_SHORT_FRACTION_Q32[index + 1];

    return low + (((high - low) * rest) / RC_BAND_STEP_MK);
}

// 吸収率 α = 1 − e^−τ(Q16)
FX_FN uint64_t RcAbsorptanceQ16(uint64_t depthQ32) {
    return RdAbsorptanceQ16(depthQ32);
}

// 面 1 つが熱で放つ量(帯ごと。σT⁴ × 帯の割合 × その帯の α (1 − 照り返し)。Kirchhoff: 当たった光を吸う割合 = 放つ割合。
// 光線は当たったセルに α (1 − ρ) を吸わせるので、同じ温度の物どうしは差し引き 0。23 §3.3「坂を下る」)。
// 面の向こうのセルがその帯を通さない(τ が飽和)なら、その帯はその面から放たない(触れている物どうしは伝導の仕事。
// 向こうからもこの面へは届かないので、釣り合いは崩れない)
FX_FN RcBands RcFaceEmission(uint32_t temperatureMk, RcCellLight light, RcCellLight across) {
    const uint64_t black = RcFaceBlackEmission(temperatureMk);
    const uint64_t blackShort = FxShiftRightU128(FxMulU64Full(black, RcShortFractionQ32(temperatureMk)), 32);
    const uint64_t blackLong = black - blackShort;
    const uint64_t emissivityShort = (RcAbsorptanceQ16(light.depthShortQ32) *
                                      ((uint64_t)LT_ONE_Q16 - (uint64_t)light.reflectanceShortQ16)) >>
                                     16;
    const uint64_t emissivityLong = (RcAbsorptanceQ16(light.depthLongQ32) *
                                     ((uint64_t)LT_ONE_Q16 - (uint64_t)light.reflectanceLongQ16)) >>
                                    16;

    RcBands bands;
    bands.shortBand = across.depthShortQ32 >= RD_DEPTH_SATURATED_Q32
                          ? 0
                          : FxShiftRightU128(FxMulU64Full(blackShort, emissivityShort), 16);
    bands.longBand = across.depthLongQ32 >= RD_DEPTH_SATURATED_Q32
                         ? 0
                         : FxShiftRightU128(FxMulU64Full(blackLong, emissivityLong), 16);

    return bands;
}

// 軸 axis に a、(axis + 1) % 3 に u、(axis + 2) % 3 に v を置いた時の、軸 want の値
FX_FN int64_t RcAxisValueOf(uint32_t want, uint32_t axis, int64_t a, int64_t u, int64_t v) {
    return want == axis ? a : (want == (axis + 1) % 3 ? u : v);
}

// --- 向きの升目 ----------------------------------------------------------------------------------
// 升目 = (面 face 0〜5: 軸 face >> 1・face & 1 が 0 なら + 向き)× (i, j)(0〜n − 1)。真ん中の向き(升目の単位)= A に ±n、
// (axis + 1) % 3 に 2i + 1 − n、(axis + 2) % 3 に 2j + 1 − n。升目の立体角 ∝ n ÷ r³(r² = n² + U² + V²)。
// 面 f から放つ・返す重み(Lambert)= 立体角 × max(0, ω·法線)÷ r = n max(0, ω_f) ÷ r⁴(× 2^40。升目の 1 辺は 2^10 まで)

struct RcTexelDirection {
    int64_t x;  // 升目の単位の向き(整数)
    int64_t y;
    int64_t z;
    uint64_t radiusSquared;
};

FX_FN RcTexelDirection RcTexelDirectionOf(uint32_t texels, uint32_t face, uint32_t i, uint32_t j) {
    const int64_t n = (int64_t)texels;
    const uint32_t axis = face >> 1;
    const int64_t a = (face & 1u) == 0 ? n : -n;
    const int64_t u = (2 * (int64_t)i) + 1 - n;
    const int64_t v = (2 * (int64_t)j) + 1 - n;

    RcTexelDirection direction;
    direction.x = RcAxisValueOf(0, axis, a, u, v);
    direction.y = RcAxisValueOf(1, axis, a, u, v);
    direction.z = RcAxisValueOf(2, axis, a, u, v);
    direction.radiusSquared = (uint64_t)((n * n) + (u * u) + (v * v));

    return direction;
}

FX_FN uint64_t RcMagnitude(int64_t value) {
    return (uint64_t)(value < 0 ? -value : value);
}

// 升目の立体角の重み(n ÷ r³ に比例。親の升目を 4 つの子に分ける時の割合)
FX_FN uint64_t RcSolidAngleWeight(uint32_t texels, uint32_t face, uint32_t i, uint32_t j) {
    const RcTexelDirection d = RcTexelDirectionOf(texels, face, i, j);
    const uint64_t radiusQ16 = FxSqrtU64(d.radiusSquared << 32);  // r × 2^16(< 2^28)

    return ((uint64_t)texels << 52) / (d.radiusSquared * radiusQ16);
}

// 光が入った面(外向きの法線が光の来た側を向く面): 軸 axis に沿って step の向き(正か負)に進む光は、その軸の反対側の面から入る。
// 面の番号 = 軸 × 2 + (− 向きの面なら 1)
FX_FN uint32_t RcEnteredFace(uint32_t axis, int32_t step) {
    return (axis * 2) + (step > 0 ? 1u : 0u);
}

// 面 sourceFace から放つ・返す重み(その面の外向きの法線の側だけ)
FX_FN uint64_t RcFaceWeight(uint32_t texels, uint32_t face, uint32_t i, uint32_t j, uint32_t sourceFace) {
    const RcTexelDirection d = RcTexelDirectionOf(texels, face, i, j);
    const uint32_t axis = sourceFace >> 1;
    const int64_t component = axis == 0 ? d.x : (axis == 1 ? d.y : d.z);
    const int64_t outward = (sourceFace & 1u) == 0 ? component : -component;
    if (outward <= 0)
        return 0;

    return (((uint64_t)texels * (uint64_t)outward) << 40) / (d.radiusSquared * d.radiusSquared);
}

// --- 光線 ----------------------------------------------------------------------------------------
// 区間は向きの軸 A の「向きに沿った座標」(+ なら A、− なら −1 − A。並べ替えても 2^k の揃いは保たれる)のセルの範囲 [firstA, endA)。
// 段 k の区間の終わり = 段 k + 1 のブロック(2^(k+1) セル)の始まり + intervalBlocks × 2^(k+1)。段 k + 1 の区間の始まりも同じ式なので、
// 同じ段 k + 1 のブロックから出た光は、どの子のブロックから来ても同じ板で受け渡す(隙間も重なりも無い)。
// 歩き方は整数の DDA(光線が通るセルを全部、通った長さとともに。ADR-0262 と同じ考え): 立方体の受け手が受ける量は向きの見かけの面に比例し、
// 面から放つ量(Lambert)と釣り合う(坂を下る)。

FX_CONST int64_t RC_ENDLESS = (int64_t)FX_U64(0x000000FFu,
                                              0xFFFFFFFFu);  // 区間の端の無限(2^40。× 256 が 64bit に収まる)
FX_CONST uint32_t RC_STEP_LIMIT = 1u << 22;  // 1 本が通るセルの数の上限(世界の端で必ず止まる。暴走の保険)

struct RcRay {
    int64_t originX;  // 1/256 セル(段 0 は放つ面の真ん中、段 k ≥ 1 は袋の量の重心)
    int64_t originY;
    int64_t originZ;
    int64_t firstA;  // 向きに沿った座標のセルの範囲 [firstA, endA)。始まりが光線の出どころより手前なら出どころから
    int64_t endA;
    uint64_t energyShort;
    uint64_t energyLong;
    uint32_t axis;       // A(向きの一番大きい軸)
    int32_t sign;        // A に沿って +1 か −1
    int32_t stepA;       // |D_A|(> 0)
    int32_t stepU;       // D の (axis + 1) % 3 の成分(|stepU| < stepA)
    int32_t stepV;       // D の (axis + 2) % 3 の成分
    uint32_t lengthQ16;  // |D| ÷ |D_A|(A に 1 セル進む間の道のり。Q16)
    uint32_t
        firstFace;  // 最初のセルに入る面(返した光をどの面から放つか)。段 0 は放つ面の真ん中から出るので、面を越えた先のセルの入る面
    uint32_t padding;
};

// 区間を歩いた結果(帯ごと。配列にしない: GPU でローカルの配列にしないため。T-0397)
struct RcWalk {
    uint64_t remainingShort;  // 区間の終わりに残った量(次の段の袋へ)
    uint64_t remainingLong;
    uint64_t absorbedShort;
    uint64_t absorbedLong;
    uint64_t reflectedShort;
    uint64_t reflectedLong;
    uint64_t escapedShort;  // 世界の外へ
    uint64_t escapedLong;
    uint32_t steps;  // 通ったセルの数(費用の目安)
    uint32_t padding;
};

// 負もある分子 ÷ 正の分母の床
FX_FN int64_t RcFloorDivide(int64_t numerator, uint64_t denominator) {
    if (numerator >= 0)
        return (int64_t)((uint64_t)numerator / denominator);

    const uint64_t magnitude = (uint64_t)(-numerator);

    return -(int64_t)((magnitude + denominator - 1) / denominator);
}

// 段 level のブロック(番号 block。実際の座標)の、向きに沿った座標の始まり
FX_FN int64_t RcBlockStartAlong(int64_t block, uint32_t level, int32_t sign) {
    return sign > 0 ? block << level : -((block + 1) << level);
}

// 段 level の区間の終わり(向きに沿った座標)。start = 段 level のブロックの向きに沿った始まり(段 0 はセル)
FX_FN int64_t RcIntervalEnd(int64_t start, uint32_t level, uint32_t intervalBlocks) {
    const uint32_t parent = level + 1;

    return ((start >> parent) << parent) + ((int64_t)intervalBlocks << parent);
}

// 升目 (face, i, j)(1 辺 texels)を、ずらし (jitterU, jitterV)(0〜RC_JITTER − 1)で光線の向きにする
FX_FN RcRay RcAimRay(RcRay ray, uint32_t face, uint32_t texels, uint32_t i, uint32_t j, uint32_t jitterU,
                     uint32_t jitterV) {
    RcRay aimed = ray;
    const int64_t scale = (int64_t)RC_JITTER;
    aimed.axis = face >> 1;
    aimed.sign = (face & 1u) == 0 ? 1 : -1;
    aimed.stepA = (int32_t)((int64_t)texels * scale);
    aimed.stepU = (int32_t)((((2 * (int64_t)i) - (int64_t)texels) * scale) + (2 * (int64_t)jitterU) + 1);
    aimed.stepV = (int32_t)((((2 * (int64_t)j) - (int64_t)texels) * scale) + (2 * (int64_t)jitterV) + 1);

    const uint64_t a = (uint64_t)aimed.stepA;
    const uint64_t u = RcMagnitude(aimed.stepU);
    const uint64_t v = RcMagnitude(aimed.stepV);
    aimed.lengthQ16 = (uint32_t)((uint64_t)FxSqrtU64(((a * a) + (u * u) + (v * v)) << 32) / a);  // [2^16, √3 × 2^16]

    return aimed;
}

FX_FN int64_t RcOriginOn(uint32_t want, RcRay ray) {
    return want == 0 ? ray.originX : (want == 1 ? ray.originY : ray.originZ);
}

// 1 区間(帯 1 つ)を通す。道のり pathQ16(セルの厚さの倍。Q16)。減った分 = 入った − 通した、返す = 減った × 照り返し、吸う = 減った − 返す。
// 通さないセル(τ が飽和)は少しでも通れば全部取る。通した量が光線の始まりの量の 1/16 未満なら残りも吸う(ADR-0262 と同じ止め方)
struct RcPass {
    uint64_t passed;
    uint64_t absorbed;
    uint64_t reflected;
};

FX_FN RcPass RcPassBand(uint64_t energy, uint64_t start, uint64_t depthQ32, uint32_t reflectanceQ16, uint64_t pathQ16) {
    RcPass pass;
    pass.passed = energy;
    pass.absorbed = 0;
    pass.reflected = 0;
    if (energy == 0 || depthQ32 == 0 || pathQ16 == 0)
        return pass;

    uint64_t kept = 0;
    if (depthQ32 < RD_DEPTH_SATURATED_Q32) {
        const uint64_t pathDepth = (depthQ32 * pathQ16) >> 16;  // < 2^37 × 2^17
        const uint64_t transmitted = pathDepth >= RD_DEPTH_SATURATED_Q32 ? 0 : FxExpQ32(-(int64_t)pathDepth, 16);
        kept = transmitted >= (uint64_t)LT_ONE_Q16 ? (uint64_t)LT_ONE_Q16 : transmitted;
    }

    pass.passed = FxShiftRightU128(FxMulU64Full(energy, kept), 16);
    const uint64_t removed = energy - pass.passed;
    pass.reflected = FxShiftRightU128(FxMulU64Full(removed, (uint64_t)reflectanceQ16), 16);
    pass.absorbed = removed - pass.reflected;
    if ((pass.passed << RC_STOP_FRACTION_SHIFT) < start) {
        pass.absorbed += pass.passed;
        pass.passed = 0;
    }

    return pass;
}

// 横の軸 1 つの次の境(光線の出どころから A に沿って進む量 s = 分子 ÷ |step|。step = 0 なら無し)
struct RcCrossing {
    uint64_t numerator;  // s × |step|(1/256 セル)
    uint32_t exists;
    uint32_t padding;
};

FX_FN RcCrossing RcNextCrossing(int64_t cell, int32_t step, int64_t positionTimesStepA, int64_t stepA) {
    RcCrossing crossing;
    crossing.exists = step != 0 ? 1u : 0u;
    crossing.numerator = 0;
    crossing.padding = 0;
    if (step == 0)
        return crossing;

    const int64_t cellSize = (int64_t)1 << RC_POSITION_SHIFT;
    const int64_t boundary = step > 0 ? (cell + 1) * cellSize : cell * cellSize;
    const int64_t difference = (boundary * stepA) - positionTimesStepA;
    crossing.numerator = RcMagnitude(difference);

    return crossing;
}

// 光線 1 本の区間を歩く(通ったセルごとに e^−τ で通し、吸う・返す)
template <typename Grid, typename Sink>
FX_FN RcWalk RcWalkRay(Grid grid, Sink sink, RcRay ray) {
    RcWalk walk;
    walk.remainingShort = ray.energyShort;
    walk.remainingLong = ray.energyLong;
    walk.absorbedShort = 0;
    walk.absorbedLong = 0;
    walk.reflectedShort = 0;
    walk.reflectedLong = 0;
    walk.escapedShort = 0;
    walk.escapedLong = 0;
    walk.steps = 0;
    walk.padding = 0;

    // --- 始まり(s = 出どころから A に沿って進んだ量。1/256 セル)---
    const int64_t cellSize = (int64_t)1 << RC_POSITION_SHIFT;
    const uint32_t axisU = (ray.axis + 1) % 3;
    const uint32_t axisV = (ray.axis + 2) % 3;
    const int64_t alongOrigin = ray.sign > 0 ? RcOriginOn(ray.axis, ray) : -RcOriginOn(ray.axis, ray);
    const int64_t stepA = (int64_t)ray.stepA;
    const int64_t baseU = RcOriginOn(axisU, ray) * stepA;  // U × |D_A| の出どころの値
    const int64_t baseV = RcOriginOn(axisV, ray) * stepA;
    const uint64_t magnitudeU = RcMagnitude(ray.stepU);
    const uint64_t magnitudeV = RcMagnitude(ray.stepV);
    const int64_t firstPosition = ray.firstA * cellSize;
    const int64_t ahead = firstPosition - alongOrigin;
    const int64_t start = ahead > 0 ? ahead : 0;
    int64_t cellA = RcFloorDivide(alongOrigin + start, (uint64_t)cellSize);
    // 境の上にいる時は進む向きの側のセル(負へ進むなら 1 を引いてから床へ)
    int64_t cellU = RcFloorDivide(baseU + (start * ray.stepU) - (ray.stepU < 0 ? 1 : 0), (uint64_t)(cellSize * stepA));
    int64_t cellV = RcFloorDivide(baseV + (start * ray.stepV) - (ray.stepV < 0 ? 1 : 0), (uint64_t)(cellSize * stepA));
    uint64_t travelledQ16 = (uint64_t)start << RC_POSITION_SHIFT;
    uint32_t face = ray.firstFace;
    bool walking = (walk.remainingShort != 0 || walk.remainingLong != 0) && cellA < ray.endA;

    for (uint32_t guard = 0; walking && guard < RC_STEP_LIMIT; ++guard) {
        const int64_t worldA = ray.sign > 0 ? cellA : -1 - cellA;
        const int64_t x = RcAxisValueOf(0, ray.axis, worldA, cellU, cellV);
        const int64_t y = RcAxisValueOf(1, ray.axis, worldA, cellU, cellV);
        const int64_t z = RcAxisValueOf(2, ray.axis, worldA, cellU, cellV);
        if (!grid.Contains(x, y, z)) {
            walk.escapedShort = walk.remainingShort;
            walk.escapedLong = walk.remainingLong;
            walk.remainingShort = 0;
            walk.remainingLong = 0;
            walking = false;
            continue;
        }

        // --- 次の境: A の板・U・V のうち一番近いもの(同じなら A・U・V の順)---
        const uint64_t nextA = (uint64_t)(((cellA + 1) * cellSize) - alongOrigin);
        const RcCrossing crossU = RcNextCrossing(cellU, ray.stepU, baseU, stepA);
        const RcCrossing crossV = RcNextCrossing(cellV, ray.stepV, baseV, stepA);
        uint32_t kind = 0;
        if (crossU.exists != 0 && crossU.numerator < nextA * magnitudeU)
            kind = 1;

        const bool vBeforeA = crossV.exists != 0 && crossV.numerator < nextA * magnitudeV;
        const bool vBeforeU = crossV.numerator * magnitudeU < crossU.numerator * magnitudeV;
        if (vBeforeA && (kind == 0 || vBeforeU))
            kind = 2;

        const uint64_t eventQ16 = kind == 0 ? nextA << RC_POSITION_SHIFT
                                            : (kind == 1 ? (crossU.numerator << RC_POSITION_SHIFT) / magnitudeU
                                                         : (crossV.numerator << RC_POSITION_SHIFT) / magnitudeV);
        // 境は厳密な有理数の順に選ぶので、床へ丸めても前の境より手前にならない
        FX_ASSERT(eventQ16 >= travelledQ16);
        const uint64_t segmentQ16 = eventQ16 - travelledQ16;
        travelledQ16 = eventQ16;

        // --- このセルを通す・吸う・返す(道のり = A に進んだ量 × |D| ÷ |D_A|)---
        const uint64_t pathQ16 = (segmentQ16 * (uint64_t)ray.lengthQ16) >> 16;
        const RcCellLight light = grid.Light(x, y, z);
        const RcPass shortPass = RcPassBand(walk.remainingShort, ray.energyShort, light.depthShortQ32,
                                            light.reflectanceShortQ16, pathQ16);
        const RcPass longPass = RcPassBand(walk.remainingLong, ray.energyLong, light.depthLongQ32,
                                           light.reflectanceLongQ16, pathQ16);
        if (shortPass.absorbed != 0 || shortPass.reflected != 0)
            sink.Deposit(x, y, z, LT_BAND_SHORT, face, shortPass.absorbed, shortPass.reflected);

        if (longPass.absorbed != 0 || longPass.reflected != 0)
            sink.Deposit(x, y, z, LT_BAND_LONG, face, longPass.absorbed, longPass.reflected);

        walk.remainingShort = shortPass.passed;
        walk.remainingLong = longPass.passed;
        walk.absorbedShort += shortPass.absorbed;
        walk.absorbedLong += longPass.absorbed;
        walk.reflectedShort += shortPass.reflected;
        walk.reflectedLong += longPass.reflected;
        walk.steps += 1;

        // --- 次のセルへ ---
        if (kind == 0) {
            cellA += 1;
            face = RcEnteredFace(ray.axis, ray.sign);
        } else if (kind == 1) {
            cellU += ray.stepU > 0 ? 1 : -1;
            face = RcEnteredFace(axisU, ray.stepU);
        } else {
            cellV += ray.stepV > 0 ? 1 : -1;
            face = RcEnteredFace(axisV, ray.stepV);
        }

        walking = (walk.remainingShort != 0 || walk.remainingLong != 0) && cellA < ray.endA;
    }

    return walk;
}

RX_NAMESPACE_END

#endif  // BICAMERAL_CASCADE_LIGHT_HLSLI
