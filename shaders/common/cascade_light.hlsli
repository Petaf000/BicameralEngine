// cascade_light.hlsli — 世界の中の光: 距離ごとの段(Radiance Cascades の型)の共通の核(docs/design/23-radiation.md §3.3・ADR-0413。T-0877 R-R1)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU リファレンス(engine/src/sim/cascade_light.cpp。T-0958)が呼ぶ。GPU 版は T-0959。
//
// データの流れ:
//   セルの温度・帯ごとの吸い込み(light.hlsli の LtCellBandOf → RdOpticalDepthQ32)→ 面ごとに RcFaceEmission(σT⁴ × 帯の割合 × α(1 − ρ)。Kirchhoff)
//   + 前の刻みにその面へ返された光(23 §3.4)→ 面の Lambert の重み(RcFaceWeight)で向きの升目(立方体の 6 面を n × n に分けたもの)へ整数で分ける
//   → 光線 1 本ごとに RcWalkRay: 向きの一番大きい軸 A の板(セル 1 枚)ごとに、板の真ん中の点を含むセルを引き、e^−τ で通し、
//     減った分 × 照り返しを「返す」、残りを「吸う」(Sink の Deposit)。区間の終わりで残った量を返す(呼ぶ側が次の段の袋へ)
// 約束:
//   - 段 k の場所の粗さ = 世界の木のレベル −k のブロック(2^k セル)。向きの升目 = 1 面 n 2^k(1 つの升目を次の段で 4 つに分ける)。
//     区間の長さはおよそ 2^k(intervalBlocks で決まる)なので、区間の終わりの光線の間隔がどの段でも同じ(Radiance Cascades の型)。
//   - 段 k から k + 1 へは、区間の終わりに残った量を (段 k + 1 のブロック・升目) の袋に足す。段 k + 1 の光線は、袋に入った光線から
//     量に比例して 1 本選んだ出どころから出る(1 つの焚き火なら焚き火から。ADR-0419。重心から出す ADR-0413 の形は閉じた箱で焦点を作った)。
//   - 段 0 の出どころは面の上で散らし、升目の重みはずらした向きの cos(RcAimedFaceWeight。ADR-0419)。
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

// 参照の温度との差の放つ量(T-1102・ADR-0461)。referenceWord の下 31bit = 参照の温度(mK。0 なら今のまま)・bit 31 = 負の側。
// 正の側は「自分 − 参照」、負の側は「参照 − 自分」の大きさ(どちらも 0 未満は 0)。正の側と負の側を別の刻みの組で運び、差を取ると
// 参照の温度の黒体との差だけを運んだことになる(同じ温度の所は何も放たない = 坂を下るが構造で 0)。
// 64bit の「0 で止める引き算」は HW で誤るので符号付きの差で(T-0185。量は 2^63 未満)
FX_CONST uint32_t RC_REFERENCE_NEGATIVE = 0x80000000u;

FX_FN uint64_t RcPositivePart(uint64_t a, uint64_t b) {
    const int64_t difference = (int64_t)a - (int64_t)b;
    if (difference <= 0)
        return 0;

    return (uint64_t)difference;
}

FX_FN RcBands RcFaceEmissionRelative(uint32_t temperatureMk, uint32_t referenceWord, RcCellLight light,
                                     RcCellLight across) {
    const uint32_t referenceMk = referenceWord & ~RC_REFERENCE_NEGATIVE;
    const bool negative = (referenceWord & RC_REFERENCE_NEGATIVE) != 0;
    const RcBands own = RcFaceEmission(temperatureMk, light, across);
    if (referenceMk == 0 && !negative)
        return own;

    const RcBands reference = RcFaceEmission(referenceMk, light, across);
    RcBands bands;
    bands.shortBand = negative ? RcPositivePart(reference.shortBand, own.shortBand)
                               : RcPositivePart(own.shortBand, reference.shortBand);
    bands.longBand = negative ? RcPositivePart(reference.longBand, own.longBand)
                              : RcPositivePart(own.longBand, reference.longBand);

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

// 向けた光線(RcAimRay の後)の、面 sourceFace から放つ重み(Lambert × 升目の中の位置の密度)= |D_A| × max(0, D·法線) ÷ |D|⁴(× 2^64)。
// 升目の中は升目の面の上で一様にずらすので、立体角の密度 |D_A| ÷ |D|³ を掛けると、ずらした向きの cos で重みが決まり、
// 同じ温度の閉じた箱で受ける量と放つ量が期待値で揃う(升目の真ん中の重みのままだと、粗い升目で斜めの面どうしに 17 % の正味の流れが残った。T-0958)。
// D·法線 ≤ |D|、|D_A| ≤ |D| なので分子 < |D|⁴(|D| > 1)で商は 64bit に収まる。|D|² < 2^30(升目の 1 辺 2^10 × RC_JITTER)
FX_FN uint64_t RcAimedFaceWeight(RcRay aimed, uint32_t sourceFace) {
    const uint32_t axis = sourceFace >> 1;
    const int64_t alongA = (int64_t)aimed.stepA * (int64_t)aimed.sign;
    const int64_t component = axis == aimed.axis
                                  ? alongA
                                  : (axis == (aimed.axis + 1) % 3 ? (int64_t)aimed.stepU : (int64_t)aimed.stepV);
    const int64_t outward = (sourceFace & 1u) == 0 ? component : -component;
    if (outward <= 0)
        return 0;

    const uint64_t a = (uint64_t)aimed.stepA;
    const uint64_t u = RcMagnitude(aimed.stepU);
    const uint64_t v = RcMagnitude(aimed.stepV);
    const uint64_t radiusSquared = (a * a) + (u * u) + (v * v);
    FxU128 numerator;
    numerator.hi = a * (uint64_t)outward;
    numerator.lo = 0;

    return FxDivU128By64(numerator, radiusSquared * radiusSquared).quotient;
}

// 向けた光線の立体角の密度 |D_A| ÷ |D|³(× 2^56)。段 k の光線を段 k + 1 の子の升目 4 つへ分ける時の重み:
// 子の光線は升目の面の上で一様にずらすので、ずらした向きの密度で分けると、親の升目の中で立体角あたり一様になる(T-0958)。
// |D_A| < 2^15、|D|² < 2^30、|D| × 2^16 < 2^31 なので分母 < 2^61、分子 < 2^71(128bit)、商 < 2^56
FX_FN uint64_t RcAimedSolidAngleWeight(RcRay aimed) {
    const uint64_t a = (uint64_t)aimed.stepA;
    const uint64_t u = RcMagnitude(aimed.stepU);
    const uint64_t v = RcMagnitude(aimed.stepV);
    const uint64_t radiusSquared = (a * a) + (u * u) + (v * v);
    const uint64_t radiusQ16 = FxSqrtU64(radiusSquared << 32);
    FxU128 numerator;
    numerator.hi = a >> 8;
    numerator.lo = a << 56;

    return FxDivU128By64(numerator, radiusSquared * radiusQ16).quotient;
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

// --- 段の受け渡し(CPU リファレンスと GPU が同じ関数を呼ぶ。T-0959・ADR-0432)-------------------------------------
// 1 刻みの流れ: 段 0 = 放つ面ごとに升目へ分けて歩く(RcLevel0Ray・RcLevel0Weight)→ 区間の終わりに残った量を
// RcPassToNextLevel で段 k + 1 の袋へ → 段 k + 1 は袋ごとに raysPerBag 本(RcBagRay)→ … 最後の段は世界の端まで。
// 形は T-0972 の「最後の 1 回」: 1b(升目の中のずらしを刻みで巡る層に分ける)+ 2(袋に「量がどの軸の面から出たか」を 3 つの欄で持ち越し、
// 子の升目へはその面の Lambert の重みで分ける = 向きの前平均と相互の重み)。

// 1 刻みの段の定数
struct RcStepConstants {
    uint64_t seed;
    uint64_t tick;
    uint32_t baseTexels;
    uint32_t levels;
    uint32_t intervalBlocks;
    uint32_t raysPerBag;  // 段 k ≥ 1 の袋 1 つから出す光線の数(1 = 今の形。量を等分し、ずらしの巡りの続きを使う)
    uint32_t emitSlots;   // 放つ面を最上段のブロックごとに何組に分けて刻みで回すか(1 = 毎刻み全部。T-1010・ADR-0444)
    uint32_t thermalInSlots;  // 1 なら熱で放つ光も組の刻みまで持ち越しの表で待つ(0 なら照り返しだけが待つ)
};

FX_CONST uint32_t RC_SLOTS_LIMIT = 64;

// 放つ面の組(T-1010・ADR-0444): 最上段のブロック(2^(段 − 1) セル)ごとに組を決め、刻み % 組の数 が自分の組の刻みにだけ、
// 持ち越しの表にたまった光(照り返し・待った熱の光)を放つ。待つ間の光は表にあり、放つ時にまとめて出る(保存はそのまま)。
// 組をブロックで揃えるのは、同じ刻みに放つ面が同じ最上段のブロックに集まり、段の袋の数(費用の芯)が組の数で割れるから。
// 組の数は奇数にする(升目の中のずらしの巡り 159 × 組の数 が 256 と互いに素で、組の刻みだけでも 16 × 16 を一巡する)。
FX_FN bool RcEmitSlotActive(FX_IN(RcStepConstants) constants, int64_t cellX, int64_t cellY, int64_t cellZ) {
    if (constants.emitSlots <= 1)
        return true;

    const uint32_t shift = constants.levels - 1;
    const int64_t slots = (int64_t)constants.emitSlots;
    const int64_t spread = (cellX >> shift) + (3 * (cellZ >> shift)) + (5 * (cellY >> shift));
    const int64_t slot = ((spread % slots) + slots) % slots;

    return (uint64_t)slot == constants.tick % (uint64_t)constants.emitSlots;
}

// 乱数の用途(R6)
FX_CONST uint32_t RC_PURPOSE_JITTER = 0x52430001u;
FX_CONST uint32_t RC_PURPOSE_REMAINDER = 0x52430002u;
FX_CONST uint32_t RC_PURPOSE_RESERVOIR = 0x52430003u;
FX_CONST uint32_t RC_PURPOSE_FACE_POINT = 0x52430004u;

FX_CONST uint32_t RC_CHILDREN = 4;              // 段 k の升目 1 つ = 段 k + 1 の升目 4 つ
FX_CONST uint64_t RC_JITTER_CYCLE_STEP = 159;   // 升目の中の 16 × 16 を 256 刻みで一巡する歩幅(奇数)
FX_CONST uint64_t RC_SMALL_ENERGY = 64;         // これより少ない量の光線は溜めでほぼ選ばれない
FX_CONST uint32_t RC_ORIGIN_BITS = 15;          // 溜めの出どころの 1 軸(1/256 セル。世界の 1 辺 127 セルまで)
FX_CONST uint32_t RC_RESERVOIR_CODE_BITS = 18;  // 溜めの鍵の符号(ビットの長さ 7bit + 仮数 11bit。詰めた語は 2^63 未満)
FX_CONST uint64_t RC_RESERVOIR_EMPTY = FX_U64(0xFFFFFFFFu, 0xFFFFFFFFu);

// 段の光線 1 本(段 0 は放つ面の升目 1 つ、段 k ≥ 1 は袋 1 つの sub 本目)
struct RcLevelRay {
    RcRay ray;      // 出どころと量(向き・区間は RcWalkLevelRay が決める)
    int64_t homeX;  // 段 level のブロック(レベル −level の座標。段 0 はセル)
    int64_t homeY;
    int64_t homeZ;
    uint64_t id;  // ずらしの鍵(段 0 = RcLevel0Id、段 k = RcBagId)
    // 量が出た軸ごとの割合(段 0 は放つ面の軸だけ。袋は入った量の軸ごとの和。割合だけを使う)
    uint64_t lobeShortX;
    uint64_t lobeShortY;
    uint64_t lobeShortZ;
    uint64_t lobeLongX;
    uint64_t lobeLongY;
    uint64_t lobeLongZ;
    uint32_t level;
    uint32_t face;  // 升目(面・i・j)
    uint32_t i;
    uint32_t j;
    uint32_t sub;  // 袋の中の何本目
    uint32_t padding;
};

// 袋へ足す 1 つ分(RcPassToNextLevel → Bags の Add)
struct RcBagShare {
    int64_t homeX;  // 段 level のブロック
    int64_t homeY;
    int64_t homeZ;
    uint64_t energyShort;
    uint64_t energyLong;
    uint64_t lobeShortX;
    uint64_t lobeShortY;
    uint64_t lobeShortZ;
    uint64_t lobeLongX;
    uint64_t lobeLongY;
    uint64_t lobeLongZ;
    uint64_t chosen;  // 溜めの候補(RcPackReservoir。袋は min を残す)
    uint32_t level;
    uint32_t face;
    uint32_t i;
    uint32_t j;
};

// ビットの長さ(0 なら 0)
FX_FN uint32_t RcBitLength(uint64_t value) {
    uint64_t rest = value;
    uint32_t length = 0;
    if ((rest >> 32) != 0) {
        length += 32;
        rest >>= 32;
    }
    if ((rest >> 16) != 0) {
        length += 16;
        rest >>= 16;
    }
    if ((rest >> 8) != 0) {
        length += 8;
        rest >>= 8;
    }
    if ((rest >> 4) != 0) {
        length += 4;
        rest >>= 4;
    }
    if ((rest >> 2) != 0) {
        length += 2;
        rest >>= 2;
    }
    if ((rest >> 1) != 0) {
        length += 1;
        rest >>= 1;
    }

    return length + (uint32_t)rest;
}

FX_FN uint64_t RcAxisOf3(uint64_t x, uint64_t y, uint64_t z, uint32_t axis) {
    return axis == 0 ? x : (axis == 1 ? y : z);
}

// 量 × 重み ÷ 重みの和(床。重み ≤ 重みの和なので商は 64bit に収まる)
FX_FN uint64_t RcShare(uint64_t energy, uint64_t weight, uint64_t weightSum) {
    return FxDivU128By64(FxMulU64Full(energy, weight), weightSum).quotient;
}

FX_FN uint64_t RcBagId(uint32_t level, int64_t blockX, int64_t blockY, int64_t blockZ, uint32_t face, uint32_t i,
                       uint32_t j) {
    uint64_t hash = FxHashCombine((uint64_t)level, (uint64_t)blockX);
    hash = FxHashCombine(hash, (uint64_t)blockY);
    hash = FxHashCombine(hash, (uint64_t)blockZ);
    hash = FxHashCombine(hash, (uint64_t)face);
    hash = FxHashCombine(hash, (uint64_t)i);

    return FxHashCombine(hash, (uint64_t)j);
}

FX_FN uint64_t RcLevel0Id(uint64_t sourceKey, uint32_t texelIndex) {
    return FxHashCombine(sourceKey, (uint64_t)texelIndex);
}

// 余り・溜めの乱数の鍵(袋の 2 本目からは袋の鍵と何本目から)
FX_FN uint64_t RcRayIdentity(FX_IN(RcLevelRay) levelRay) {
    return levelRay.sub == 0 ? levelRay.id : FxHashCombine(levelRay.id, (uint64_t)levelRay.sub);
}

// 1b: 升目の中のずらし(16 × 16 の番号)。光線ごとに決まった始まりから、刻み(袋の中の何本目)ごとに歩幅 159 で巡る。
// 256 刻みで升目の中を一巡するので、時間の平均で升目の中が一様になる(層に分けた前平均。T-0972)
FX_FN uint32_t RcJitterIndex(FX_IN(RcStepConstants) constants, uint64_t id, uint32_t level, uint32_t sub) {
    const uint64_t start = FxHash64(constants.seed, 0, id, RC_PURPOSE_JITTER);
    const uint64_t stride = level == 0 ? 1 : (uint64_t)constants.raysPerBag;
    const uint64_t turn = (constants.tick * stride) + (uint64_t)sub;

    return (uint32_t)((start + (turn * RC_JITTER_CYCLE_STEP)) & 255);
}

FX_FN RcRay RcAimLevelRay(FX_IN(RcStepConstants) constants, FX_IN(RcLevelRay) levelRay) {
    const uint32_t texels = constants.baseTexels << levelRay.level;
    const uint32_t jitter = RcJitterIndex(constants, levelRay.id, levelRay.level, levelRay.sub);

    return RcAimRay(levelRay.ray, levelRay.face, texels, levelRay.i, levelRay.j, jitter & 15u, jitter >> 4);
}

// 向けた光線が軸 axis に沿って進む側の面(その面から放つ向き)
FX_FN uint32_t RcFaceOfAxis(RcRay aimed, uint32_t axis) {
    const int64_t alongA = (int64_t)aimed.stepA * (int64_t)aimed.sign;
    const int64_t component = axis == aimed.axis
                                  ? alongA
                                  : (axis == (aimed.axis + 1) % 3 ? (int64_t)aimed.stepU : (int64_t)aimed.stepV);

    return (axis * 2) + (component < 0 ? 1u : 0u);
}

// 段 level の区間(向きに沿った座標)と最初に入る面を決める。段 0 は放つセルから、段 k ≥ 1 は段 k のブロックの始まり + 区間の長さから。
// 終わり = 段 k + 1 のブロックの始まり + 区間の長さ × 2^(k+1)(RcIntervalEnd)。最後の段は世界の端まで
FX_FN RcRay RcLevelInterval(FX_IN(RcStepConstants) constants, FX_IN(RcLevelRay) levelRay, RcRay aimed) {
    const int64_t home = aimed.axis == 0 ? levelRay.homeX : (aimed.axis == 1 ? levelRay.homeY : levelRay.homeZ);
    const int64_t start = RcBlockStartAlong(home, levelRay.level, aimed.sign);
    const int64_t interval = (int64_t)constants.intervalBlocks << levelRay.level;
    RcRay ray = aimed;
    ray.firstA = levelRay.level == 0 ? start : start + interval;
    ray.endA = levelRay.level + 1 >= constants.levels ? RC_ENDLESS
                                                      : RcIntervalEnd(start, levelRay.level, constants.intervalBlocks);
    // 段 k ≥ 1 の光線は区間の始まりの板(向きの軸の面)から最初のセルに入る
    if (levelRay.level != 0)
        ray.firstFace = RcEnteredFace(aimed.axis, aimed.sign);

    return ray;
}

// 段の光線 1 本を向けて区間を歩かせる(残りは呼ぶ側が RcPassToNextLevel へ)
template <typename Grid, typename Sink>
FX_FN RcWalk RcWalkLevelRay(FX_IN(RcStepConstants) constants, FX_IN(RcLevelRay) levelRay, Grid grid, Sink sink) {
    const RcRay ray = RcLevelInterval(constants, levelRay, RcAimLevelRay(constants, levelRay));

    return RcWalkRay(grid, sink, ray);
}

// 全部 0 の光線(HLSL と C++ で同じ書き方で初期化するため)
FX_FN RcLevelRay RcEmptyLevelRay() {
    RcLevelRay levelRay;
    levelRay.ray.originX = 0;
    levelRay.ray.originY = 0;
    levelRay.ray.originZ = 0;
    levelRay.ray.firstA = 0;
    levelRay.ray.endA = 0;
    levelRay.ray.energyShort = 0;
    levelRay.ray.energyLong = 0;
    levelRay.ray.axis = 0;
    levelRay.ray.sign = 1;
    levelRay.ray.stepA = 1;
    levelRay.ray.stepU = 0;
    levelRay.ray.stepV = 0;
    levelRay.ray.lengthQ16 = 0;
    levelRay.ray.firstFace = 0;
    levelRay.ray.padding = 0;
    levelRay.homeX = 0;
    levelRay.homeY = 0;
    levelRay.homeZ = 0;
    levelRay.id = 0;
    levelRay.lobeShortX = 0;
    levelRay.lobeShortY = 0;
    levelRay.lobeShortZ = 0;
    levelRay.lobeLongX = 0;
    levelRay.lobeLongY = 0;
    levelRay.lobeLongZ = 0;
    levelRay.level = 0;
    levelRay.face = 0;
    levelRay.i = 0;
    levelRay.j = 0;
    levelRay.sub = 0;
    levelRay.padding = 0;

    return levelRay;
}

// --- 段 0: 面から放つ ---

// 段 0 の光線(放つ面 sourceFace の升目 texelIndex 番目 = (face, i, j))。sourceKey = セルの番号 × 6 + 放つ面。
// 出どころは面の上の点(1/256 セル)で、面の中の位置は光線ごとにハッシュで散らす(R6。ADR-0419 の 1)。量と軸の割合は呼ぶ側が入れる
FX_FN RcLevelRay RcLevel0Ray(FX_IN(RcStepConstants) constants, uint64_t sourceKey, int64_t cellX, int64_t cellY,
                             int64_t cellZ, uint32_t sourceFace, uint32_t texelIndex, uint32_t face, uint32_t i,
                             uint32_t j) {
    RcLevelRay levelRay = RcEmptyLevelRay();
    levelRay.id = RcLevel0Id(sourceKey, texelIndex);
    const uint64_t hash = FxHash64(constants.seed, constants.tick, levelRay.id, RC_PURPOSE_FACE_POINT);
    const int64_t cellPosition = (int64_t)1 << RC_POSITION_SHIFT;
    const uint32_t axis = sourceFace >> 1;
    const int64_t onFace = (sourceFace & 1u) == 0 ? 1 : 0;
    levelRay.ray.originX = (cellX * cellPosition) + (int64_t)(hash & 255);
    levelRay.ray.originY = (cellY * cellPosition) + (int64_t)((hash >> 8) & 255);
    levelRay.ray.originZ = (cellZ * cellPosition) + (int64_t)((hash >> 16) & 255);
    if (axis == 0)
        levelRay.ray.originX = (cellX + onFace) * cellPosition;
    else if (axis == 1)
        levelRay.ray.originY = (cellY + onFace) * cellPosition;
    else
        levelRay.ray.originZ = (cellZ + onFace) * cellPosition;

    levelRay.ray.firstFace = sourceFace ^ 1u;  // 面を越えた先のセルには、向かい合う面から入る
    levelRay.homeX = cellX;
    levelRay.homeY = cellY;
    levelRay.homeZ = cellZ;
    levelRay.face = face;
    levelRay.i = i;
    levelRay.j = j;

    return levelRay;
}

// 段 0 の升目の重み = ずらした向きの cos と立体角の密度(RcAimedFaceWeight。ADR-0419 の 2)
FX_FN uint64_t RcLevel0Weight(FX_IN(RcStepConstants) constants, FX_IN(RcLevelRay) levelRay, uint32_t sourceFace) {
    return RcAimedFaceWeight(RcAimLevelRay(constants, levelRay), sourceFace);
}

// 段 0 の光線に量を入れる(軸の割合は放つ面の軸だけ)
FX_FN RcLevelRay RcLevel0WithEnergy(RcLevelRay levelRay, uint32_t sourceFace, uint64_t energyShort,
                                    uint64_t energyLong) {
    RcLevelRay filled = levelRay;
    const uint32_t axis = sourceFace >> 1;
    filled.ray.energyShort = energyShort;
    filled.ray.energyLong = energyLong;
    filled.lobeShortX = axis == 0 ? energyShort : 0;
    filled.lobeShortY = axis == 1 ? energyShort : 0;
    filled.lobeShortZ = axis == 2 ? energyShort : 0;
    filled.lobeLongX = axis == 0 ? energyLong : 0;
    filled.lobeLongY = axis == 1 ? energyLong : 0;
    filled.lobeLongZ = axis == 2 ? energyLong : 0;

    return filled;
}

// --- 袋: 重み付きの溜め(ADR-0419 の 3)---

// log2(1 + k/256)(Q32。k = 0〜255。k = 256 は 2^32。物理の定数ではなく数の表)
FX_CONST uint32_t RC_LOG2_TABLE_Q32[256] = {
    0u,          24157255u,   48220695u,   72191046u,   96069025u,   119855343u,  143550699u,  167155786u,  190671291u,
    214097890u,  237436253u,  260687042u,  283850912u,  306928510u,  329920477u,  352827446u,  375650043u,  398388887u,
    421044590u,  443617759u,  466108993u,  488518883u,  510848017u,  533096975u,  555266330u,  577356649u,  599368495u,
    621302422u,  643158981u,  664938715u,  686642163u,  708269857u,  729822324u,  751300086u,  772703658u,  794033552u,
    815290272u,  836474320u,  857586191u,  878626374u,  899595355u,  920493615u,  941321628u,  962079865u,  982768792u,
    1003388871u, 1023940559u, 1044424306u, 1064840562u, 1085189769u, 1105472367u, 1125688789u, 1145839467u, 1165924827u,
    1185945290u, 1205901275u, 1225793196u, 1245621463u, 1265386481u, 1285088654u, 1304728379u, 1324306051u, 1343822060u,
    1363276795u, 1382670639u, 1402003972u, 1421277169u, 1440490605u, 1459644648u, 1478739665u, 1497776018u, 1516754066u,
    1535674166u, 1554536671u, 1573341930u, 1592090289u, 1610782092u, 1629417679u, 1647997388u, 1666521551u, 1684990500u,
    1703404565u, 1721764068u, 1740069334u, 1758320682u, 1776518428u, 1794662886u, 1812754368u, 1830793181u, 1848779632u,
    1866714024u, 1884596657u, 1902427829u, 1920207835u, 1937936969u, 1955615520u, 1973243777u, 1990822024u, 2008350545u,
    2025829620u, 2043259528u, 2060640543u, 2077972941u, 2095256991u, 2112492963u, 2129681124u, 2146821738u, 2163915068u,
    2180961373u, 2197960912u, 2214913940u, 2231820712u, 2248681479u, 2265496490u, 2282265995u, 2298990237u, 2315669461u,
    2332303909u, 2348893820u, 2365439432u, 2381940981u, 2398398701u, 2414812824u, 2431183582u, 2447511201u, 2463795910u,
    2480037932u, 2496237492u, 2512394810u, 2528510107u, 2544583599u, 2560615505u, 2576606038u, 2592555411u, 2608463835u,
    2624331521u, 2640158677u, 2655945509u, 2671692221u, 2687399018u, 2703066101u, 2718693670u, 2734281925u, 2749831063u,
    2765341278u, 2780812767u, 2796245722u, 2811640333u, 2826996792u, 2842315287u, 2857596005u, 2872839132u, 2888044853u,
    2903213350u, 2918344806u, 2933439400u, 2948497313u, 2963518722u, 2978503803u, 2993452732u, 3008365682u, 3023242827u,
    3038084339u, 3052890387u, 3067661140u, 3082396766u, 3097097433u, 3111763305u, 3126394546u, 3140991321u, 3155553791u,
    3170082117u, 3184576458u, 3199036973u, 3213463820u, 3227857155u, 3242217134u, 3256543910u, 3270837638u, 3285098468u,
    3299326552u, 3313522041u, 3327685082u, 3341815825u, 3355914416u, 3369981001u, 3384015725u, 3398018732u, 3411990165u,
    3425930167u, 3439838878u, 3453716438u, 3467562987u, 3481378662u, 3495163602u, 3508917943u, 3522641820u, 3536335369u,
    3549998721u, 3563632012u, 3577235372u, 3590808933u, 3604352825u, 3617867177u, 3631352118u, 3644807776u, 3658234277u,
    3671631748u, 3685000315u, 3698340100u, 3711651229u, 3724933824u, 3738188006u, 3751413898u, 3764611620u, 3777781291u,
    3790923031u, 3804036958u, 3817123189u, 3830181840u, 3843213029u, 3856216870u, 3869193478u, 3882142967u, 3895065449u,
    3907961038u, 3920829844u, 3933671979u, 3946487554u, 3959276677u, 3972039458u, 3984776005u, 3997486426u, 4010170828u,
    4022829316u, 4035461997u, 4048068976u, 4060650357u, 4073206244u, 4085736740u, 4098241947u, 4110721967u, 4123176902u,
    4135606852u, 4148011918u, 4160392197u, 4172747791u, 4185078796u, 4197385310u, 4209667431u, 4221925255u, 4234158878u,
    4246368396u, 4258553902u, 4270715492u, 4282853259u};

FX_FN uint64_t RcLog2TableQ32(uint32_t index) {
    return index >= 256 ? ((uint64_t)1 << 32) : (uint64_t)RC_LOG2_TABLE_Q32[index];
}

// −log2(x ÷ 2^32)(Q32。x は (0, 2^32])。仮数の上 8bit で表を引き、次の 24bit で直線で補う(単調。誤差は log2 で 3 × 10^−6 ほど)。
// 細かさが粗い(Q16 など)と小さい鍵が同じ値に揃い、溜めが出どころの小さい方へ偏った(T-0959 の測り: 8 m の円の 500 刻みの平均が ±25 %)。
// FxLnU64 は GPU と CPU でこの鍵だけ食い違った(T-0959。原因は未確認。表の形にして避けた)
FX_FN uint64_t RcMinusLog2Q32(uint64_t x) {
    const uint32_t msb = FxMsbU64(x);
    const uint64_t mantissa = x << (63 - msb);  // Q63 で [1, 2)
    const uint32_t index = (uint32_t)((mantissa >> 55) & 255);
    const uint64_t rest = (mantissa >> 31) & 0xFFFFFF;
    const uint64_t low = RcLog2TableQ32(index);
    const uint64_t high = RcLog2TableQ32(index + 1);
    const uint64_t log2Q32 = ((uint64_t)msb << 32) + low + (((high - low) * rest) >> 24);
    const uint64_t whole = (uint64_t)32 << 32;

    return log2Q32 >= whole ? 0 : whole - log2Q32;
}

// 鍵 = −log2(u) ÷ 量(Q64 ÷ 量。指数分布の最小なので、量に比例した確率で選ばれる。−ln でなく −log2 なのは
// 全部の鍵が同じ倍率になるだけで選ばれ方は変わらないから)。u は (0, 1] の Q32
FX_FN uint64_t RcReservoirKey(uint64_t hash, uint64_t energy) {
    if (energy < RC_SMALL_ENERGY)
        return RC_RESERVOIR_EMPTY - 1;

    const uint64_t unit = (hash >> 32) + 1;           // (0, 2^32]
    const uint64_t minusLog2 = RcMinusLog2Q32(unit);  // < 2^37
    FxU128 numerator;
    numerator.hi = minusLog2 >> 32;
    numerator.lo = minusLog2 << 32;

    return FxDivU128By64(numerator, energy).quotient;
}

// 鍵を 18bit の単調な符号に(ビットの長さ 7bit・仮数 = 頭の 1 の次の 11bit)。相対の分解能 2^−11。
// 鍵と出どころを 64bit 1 語に詰めて min の 1 回の atomic で選ぶため(GPU。同じ符号の候補は出どころの小さい方。ADR-0432)
FX_FN uint64_t RcReservoirCode(uint64_t key) {
    if (key == 0)
        return 0;

    const uint32_t lengthBits = RcBitLength(key);
    const uint64_t mantissa = lengthBits > 12 ? (key >> (lengthBits - 12)) & 0x7FF : (key << (12 - lengthBits)) & 0x7FF;

    return ((uint64_t)lengthBits << 11) | mantissa;
}

// 溜めの候補 = 鍵の符号 18bit | 出どころ 3 × 15bit(世界の低い隅からの 1/256 セル)。2^63 未満なので符号つきの min でも同じ順
FX_FN uint64_t RcPackReservoir(uint64_t key, RcRay origin, int64_t lowX, int64_t lowY, int64_t lowZ) {
    const int64_t cellPosition = (int64_t)1 << RC_POSITION_SHIFT;
    const uint64_t x = (uint64_t)(origin.originX - (lowX * cellPosition));
    const uint64_t y = (uint64_t)(origin.originY - (lowY * cellPosition));
    const uint64_t z = (uint64_t)(origin.originZ - (lowZ * cellPosition));

    return (RcReservoirCode(key) << (3 * RC_ORIGIN_BITS)) | (x << (2 * RC_ORIGIN_BITS)) | (y << RC_ORIGIN_BITS) | z;
}

FX_FN int64_t RcUnpackOrigin(uint64_t chosen, uint32_t axis, int64_t low) {
    const uint64_t mask = ((uint64_t)1 << RC_ORIGIN_BITS) - 1;
    const uint64_t value = (chosen >> ((2 - axis) * RC_ORIGIN_BITS)) & mask;

    return (int64_t)value + (low << RC_POSITION_SHIFT);
}

// 袋の sub 本目の光線(量は raysPerBag で等分し、余りは 0 本目へ。軸の割合は袋のまま)
FX_FN RcLevelRay RcBagRay(FX_IN(RcBagShare) bag, uint32_t sub, uint32_t raysPerBag, int64_t lowX, int64_t lowY,
                          int64_t lowZ) {
    RcLevelRay levelRay = RcEmptyLevelRay();
    const uint64_t baseShort = bag.energyShort / raysPerBag;
    const uint64_t baseLong = bag.energyLong / raysPerBag;
    levelRay.ray.originX = RcUnpackOrigin(bag.chosen, 0, lowX);
    levelRay.ray.originY = RcUnpackOrigin(bag.chosen, 1, lowY);
    levelRay.ray.originZ = RcUnpackOrigin(bag.chosen, 2, lowZ);
    levelRay.ray.energyShort = sub == 0 ? bag.energyShort - (baseShort * (raysPerBag - 1)) : baseShort;
    levelRay.ray.energyLong = sub == 0 ? bag.energyLong - (baseLong * (raysPerBag - 1)) : baseLong;
    levelRay.homeX = bag.homeX;
    levelRay.homeY = bag.homeY;
    levelRay.homeZ = bag.homeZ;
    levelRay.id = RcBagId(bag.level, bag.homeX, bag.homeY, bag.homeZ, bag.face, bag.i, bag.j);
    levelRay.lobeShortX = bag.lobeShortX;
    levelRay.lobeShortY = bag.lobeShortY;
    levelRay.lobeShortZ = bag.lobeShortZ;
    levelRay.lobeLongX = bag.lobeLongX;
    levelRay.lobeLongY = bag.lobeLongY;
    levelRay.lobeLongZ = bag.lobeLongZ;
    levelRay.level = bag.level;
    levelRay.face = bag.face;
    levelRay.i = bag.i;
    levelRay.j = bag.j;
    levelRay.sub = sub;

    return levelRay;
}

// --- 段 k から k + 1 へ(2: 向きの前平均と相互の重み)---

// 合計 remaining を軸の割合 (x, y, z) で分けた軸 want の分(余りは最初の 0 でない軸へ)
FX_FN uint64_t RcLobePart(uint64_t remaining, uint64_t x, uint64_t y, uint64_t z, uint32_t want) {
    const uint64_t total = x + y + z;
    if (total == 0 || remaining == 0)
        return 0;

    const uint64_t partX = RcShare(remaining, x, total);
    const uint64_t partY = RcShare(remaining, y, total);
    const uint64_t partZ = RcShare(remaining, z, total);
    const uint32_t first = x != 0 ? 0u : (y != 0 ? 1u : 2u);
    const uint64_t rest = remaining - partX - partY - partZ;
    const uint64_t part = RcAxisOf3(partX, partY, partZ, want);

    return want == first ? part + rest : part;
}

// 区間の終わりに残った量を段 level + 1 の袋(子の升目 4 つ)へ分ける。量は「どの軸の面から出たか」(袋の軸の割合)ごとに、
// 子の袋の光線の向き(ずらしは袋の鍵から決まる)がその面から出る Lambert の重み(RcAimedFaceWeight)で分ける。
// 余りはハッシュで決めた子へ。Bags の約束: void Add(RcBagShare share)
template <typename Bags>
FX_FN void RcPassToNextLevel(FX_IN(RcStepConstants) constants, FX_IN(RcLevelRay) from, RcWalk walk, int64_t lowX,
                             int64_t lowY, int64_t lowZ, Bags bags) {
    if (walk.remainingShort == 0 && walk.remainingLong == 0)
        return;

    // --- 子の升目の向きと、軸ごとの重み ---
    const uint32_t next = from.level + 1;
    const uint32_t texels = constants.baseTexels << next;
    const int64_t homeX = from.homeX >> 1;
    const int64_t homeY = from.homeY >> 1;
    const int64_t homeZ = from.homeZ >> 1;
    uint64_t weight[RC_CHILDREN][3];
    uint64_t weightSum[3];
    weightSum[0] = 0;
    weightSum[1] = 0;
    weightSum[2] = 0;
    FX_UNROLL
    for (uint32_t child = 0; child < RC_CHILDREN; ++child) {
        const uint32_t childI = (2 * from.i) + (child & 1u);
        const uint32_t childJ = (2 * from.j) + (child >> 1);
        const uint64_t childId = RcBagId(next, homeX, homeY, homeZ, from.face, childI, childJ);
        const uint32_t jitter = RcJitterIndex(constants, childId, next, 0);
        const RcRay aimed = RcAimRay(from.ray, from.face, texels, childI, childJ, jitter & 15u, jitter >> 4);
        FX_UNROLL
        for (uint32_t axis = 0; axis < 3; ++axis) {
            weight[child][axis] = RcAimedFaceWeight(aimed, RcFaceOfAxis(aimed, axis));
            weightSum[axis] += weight[child][axis];
        }
    }

    // --- 軸ごとの残りを子へ(余りは決まった子へ)---
    const uint64_t identity = RcRayIdentity(from);
    const uint32_t lucky = FxRandomBelow(FxHash64(constants.seed, constants.tick, identity, RC_PURPOSE_REMAINDER),
                                         RC_CHILDREN);
    uint64_t shareShort[RC_CHILDREN][3];
    uint64_t shareLong[RC_CHILDREN][3];
    FX_UNROLL
    for (uint32_t axis = 0; axis < 3; ++axis) {
        const uint64_t restShort = RcLobePart(walk.remainingShort, from.lobeShortX, from.lobeShortY, from.lobeShortZ,
                                              axis);
        const uint64_t restLong = RcLobePart(walk.remainingLong, from.lobeLongX, from.lobeLongY, from.lobeLongZ, axis);
        uint64_t givenShort = 0;
        uint64_t givenLong = 0;
        FX_UNROLL
        for (uint32_t child = 0; child < RC_CHILDREN; ++child) {
            shareShort[child][axis] = RcShare(restShort, weight[child][axis], weightSum[axis]);
            shareLong[child][axis] = RcShare(restLong, weight[child][axis], weightSum[axis]);
            givenShort += shareShort[child][axis];
            givenLong += shareLong[child][axis];
        }

        FX_UNROLL
        for (uint32_t child = 0; child < RC_CHILDREN; ++child) {
            if (child == lucky) {
                shareShort[child][axis] += restShort - givenShort;
                shareLong[child][axis] += restLong - givenLong;
            }
        }
    }

    // --- 袋へ ---
    FX_UNROLL
    for (uint32_t child = 0; child < RC_CHILDREN; ++child) {
        RcBagShare share;
        share.energyShort = shareShort[child][0] + shareShort[child][1] + shareShort[child][2];
        share.energyLong = shareLong[child][0] + shareLong[child][1] + shareLong[child][2];
        if (share.energyShort != 0 || share.energyLong != 0) {
            const uint64_t pick = FxHash64(constants.seed, constants.tick, FxHashCombine(identity, (uint64_t)child),
                                           RC_PURPOSE_RESERVOIR);
            share.homeX = homeX;
            share.homeY = homeY;
            share.homeZ = homeZ;
            share.lobeShortX = shareShort[child][0];
            share.lobeShortY = shareShort[child][1];
            share.lobeShortZ = shareShort[child][2];
            share.lobeLongX = shareLong[child][0];
            share.lobeLongY = shareLong[child][1];
            share.lobeLongZ = shareLong[child][2];
            share.chosen = RcPackReservoir(RcReservoirKey(pick, share.energyShort + share.energyLong), from.ray, lowX,
                                           lowY, lowZ);
            share.level = next;
            share.face = from.face;
            share.i = (2 * from.i) + (child & 1u);
            share.j = (2 * from.j) + (child >> 1);
            bags.Add(share);
        }
    }
}

// --- GPU 版の置き場(shaders/sim/cascade_light.hlsl・sim/gpu_cascade_light.cpp。T-0959)----------------------------------------
// 作業のバッファ(u4。CPU が書いて既定のヒープへ写す。読むだけ)= [RcGpuParams][升目の表][セルの光の値][セルの温度][初めの持ち越し]
// 状態のバッファ(u5。uint64 の語)= [頭の語][放った][吸った][持ち越しの表 × 2][放つ面の量][照り返しの密な受け][放つ面の列][面ごとの分け方]
//   [袋の表 × 2][袋の列 × 2]。袋の表は開いた番地の hash(鍵の CAS + 量の 64bit の atomic の足し算・溜めの atomic min)。
//   どれも可換な整数の足し算か min なので、光線と袋の処理の順によらず CPU リファレンスとビット一致する(R3・R4)。
// 位置はすべてバイト(< 2^32)。
struct RcGpuParams {
    uint64_t seed;
    int64_t lowX;
    int64_t lowY;
    int64_t lowZ;
    int64_t sizeX;
    int64_t sizeY;
    int64_t sizeZ;
    uint32_t baseTexels;
    uint32_t levels;
    uint32_t intervalBlocks;
    uint32_t raysPerBag;
    uint32_t quietBelowMk;
    uint32_t cellCount;
    uint32_t texelsPerFace;  // 段 0 の放つ面 1 つの升目の数
    uint32_t bagCapacity;    // 袋の表 1 つの番地の数(2 の冪)
    uint32_t faceCapacity;   // 1 刻みに放つ面の数の上限
    uint32_t carryCapacity;  // 持ち越しの表 1 つの上限
    uint32_t initialCarryCount;
    uint32_t workTexels;  // 作業のバッファの中の位置
    uint32_t workLights;
    uint32_t workTemperatures;
    uint32_t workCarry;
    uint32_t stateEmitted;  // 状態のバッファの中の位置
    uint32_t stateAbsorbed;
    uint32_t stateCarryList0;
    uint32_t stateCarryList1;
    uint32_t stateSources;
    uint32_t stateCarryDense;
    uint32_t stateFaces;
    uint32_t stateFaceInfo;
    uint32_t stateBags0;
    uint32_t stateBags1;
    uint32_t stateBagList0;
    uint32_t stateBagList1;
    uint32_t stateWords;  // 状態のバッファの語の数
    uint32_t emitSlots;   // RcStepConstants と同じ
    uint32_t thermalInSlots;
    uint32_t stateFaceFlags;  // 放つ面の列に入れた印(セル × 6 の uint32。T-1010)
    uint32_t referenceWord;   // 参照の温度と負の側の印(RcFaceEmissionRelative。0 = 今のまま。T-1102)
};

// 頭の語(uint64 の番号)
FX_CONST uint32_t RC_GPU_LEDGER = 0;        // 帯 × 5(放った・持ち越しを放った・吸った・返した・外へ)
FX_CONST uint32_t RC_GPU_RAYS = 10;         // 段ごとの光線の本数(8)
FX_CONST uint32_t RC_GPU_STEPS = 18;        // 通ったセルの数
FX_CONST uint32_t RC_GPU_FACE_COUNT = 19;   // 放つ面の数
FX_CONST uint32_t RC_GPU_BAG_COUNT = 20;    // 袋の表ごとの袋の数(2)
FX_CONST uint32_t RC_GPU_CARRY_COUNT = 22;  // 持ち越しの表ごとの数(2)
FX_CONST uint32_t RC_GPU_OVERFLOW = 24;     // 溢れの印(1 = 放つ面・2 = 袋・4 = 持ち越し)
FX_CONST uint32_t RC_GPU_BAG_PEAK = 25;     // 段 1 つの袋の数の最大(置き場の大きさを決める目安。作ってからの最大)
FX_CONST uint32_t RC_GPU_HELD = 26;         // 帯ごとの、熱で放って組の刻みまで待たせた量(2。T-1010)
FX_CONST uint32_t RC_GPU_KEPT = 28;         // 帯ごとの、持ち越しの表で次の刻みへそのまま送った量(2)
FX_CONST uint32_t RC_GPU_HEADER_WORDS = 32;
FX_CONST uint32_t RC_GPU_LEDGER_KINDS = 5;
FX_CONST uint32_t RC_GPU_BAG_WORDS = 10;                                   // 鍵・量 2・軸の割合 6・溜め
FX_CONST uint32_t RC_GPU_FACE_INFO_WORDS = 4;                              // 重みの和・余りの升目・余り 2
FX_CONST uint32_t RC_GPU_CARRY_WORDS = 2;                                  // 鍵(セル × 12 + 面 × 2 + 帯)・量
FX_CONST uint64_t RC_GPU_CHOSEN_EMPTY = FX_U64(0x7FFFFFFFu, 0xFFFFFFFFu);  // 溜めの空(詰めた語は 2^63 未満)
FX_CONST uint32_t RC_GPU_TEXEL_LIMIT = 512;                                // 袋の鍵に升目を 9bit で詰める
FX_CONST uint32_t RC_GPU_BLOCK_BITS = 14;

RX_NAMESPACE_END

#endif  // BICAMERAL_CASCADE_LIGHT_HLSLI
