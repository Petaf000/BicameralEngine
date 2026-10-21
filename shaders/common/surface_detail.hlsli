// surface_detail.hlsli — 表面の細かい形(約 2 cm〜0.5 m)の整数の関数(docs/design/23 §7・ADR-0384。研究 R-T1・T-0879)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU リファレンス(engine/src/sim/surface_detail.cpp と試験)と
// GPU(shaders/sim/surface_detail.hlsl)が同じ関数を呼ぶ。描画の小数の版は shaders/render/surface_detail_float.hlsli(同じ形・同じ乱数)。
//
// 形(仮・研究の 1 回目。名前も仮):
//   地面の高さ G(x, z)(µm。区画の床から上へ)= 下地 + 自然の細かい形 − 彫った深さ
//   - 下地: 柱(0.5 m 四方の列)ごとの地面の高さ。2 つの形を選べる(SdConfig::baseSmooth。どちらにするかはチケットの問い):
//     平ら = 柱の中は同じ高さ(セルの固い物の量と形の体積が平均で一致する)/ 滑らか = 柱の中心どうしで線形に補間(隣のセルを読む。段にならない)
//   - 自然の細かい形: 物質の「肌理」(SdRelief: 振れ幅・波の長さ・波の数・割れ目)と場所の種から、値の雑音を重ねる。
//     波の長さは 2^19 µm(約 52 cm)から半分ずつ、2^15 µm(約 3.3 cm)で止める(約 2 cm 未満は描画の質感。23 §7)。記録しない
//   - 彫った深さ(プレイヤー・出来事が変えた所。真実に記録する差分)。持ち方の候補 2 つ(R-T1 で比べる):
//     候補 B(SD_MODE_LIST)= 彫った形の一覧(線分 + 半径 + 深さの溝。断面は放物線。足跡は短い溝)。柱ごとに当たる形を登録して足す
//     候補 A(SD_MODE_GRID)= 高さの差の格子(柱ごとに 17 × 17 点・31.25 mm 間隔の µm。線形に補間)
//   法則へは、セルの固い物の割合(fill)と面の開き具合(+x・+z・上の面)として効く(SdColumnOpennessAt。21 §2.7 の塞ぐ面と同じ量)。
//
// 座標: 区画の中の µm(uint32。区画は 1 辺 4.29 km まで)。乱数の格子だけ世界の原点からの µm(uint64)で数える(区画をまたいで同じ形)。
// 浮動小数点は使わない(04 §4)。丸めは 0 方向の切り捨て(SdMulQ16)。
#ifndef BICAMERAL_SURFACE_DETAIL_HLSLI
#define BICAMERAL_SURFACE_DETAIL_HLSLI

#include "common/fixed.hlsli"

#ifdef __cplusplus
#include <span>
#define SD_NAMESPACE_BEGIN         \
    namespace bicameral::surface { \
        using namespace ::bicameral::fx;
#define SD_NAMESPACE_END }
#else
#define SD_NAMESPACE_BEGIN
#define SD_NAMESPACE_END
#endif

SD_NAMESPACE_BEGIN

// --- 定数 ---

FX_CONST uint32_t SD_CELL_UM = 500000;  // セル(柱)の 1 辺
FX_CONST uint32_t SD_HALF_CELL_UM = 250000;
FX_CONST int64_t SD_ONE_Q16 = 65536;
FX_CONST uint32_t SD_MIN_WAVE_SHIFT = 15;  // 最も小さい波 2^15 µm(約 3.3 cm)
FX_CONST uint32_t SD_MAX_WAVE_SHIFT = 19;  // 最も大きい波 2^19 µm(約 52 cm)

// 候補 A の格子(柱ごと。点 i の位置 = 柱の角 + i × 31.25 mm。i = 16 は隣の柱の角と同じ所で、両方の柱が同じ値を持つ)
FX_CONST uint32_t SD_GRID_STEPS = 16;
FX_CONST uint32_t SD_GRID_NODES = 17;
FX_CONST uint32_t SD_GRID_NODE_COUNT = 289;
FX_CONST uint32_t SD_GRID_SPACING_UM = 31250;
FX_CONST uint32_t SD_NO_GRID = 0xFFFFFFFFu;

// 開き具合の標本(セルの面の上 16 × 16 の小さな四角の中心。面の上は 16 点)
FX_CONST uint32_t SD_SAMPLE_SIDE = 16;
FX_CONST uint32_t SD_SAMPLE_HALF_STEP_UM = 15625;

// 彫った形の線分の長さの上限(2^22 µm ≒ 4.2 m。長い溝は折れ線に分ける。整数の積が 64bit に収まるため)
FX_CONST uint32_t SD_MAX_CARVE_LENGTH_UM = 4194304;
FX_CONST uint32_t SD_MAX_CARVE_RADIUS_UM = 500000;
FX_CONST uint32_t SD_MAX_CARVE_DEPTH_UM = 1000000;

// 当たりの傾きを取る差の幅(1 cm)
FX_CONST int64_t SD_GRADIENT_STEP_UM = 10000;

// 乱数の用途(04 R6)
FX_CONST uint32_t SD_PURPOSE_DETAIL = 0x53440001u;
FX_CONST uint32_t SD_PURPOSE_CRACK = 0x53440002u;

// 差分の持ち方
FX_CONST uint32_t SD_MODE_NATURAL = 0;  // 差分なし(自然の形だけ)
FX_CONST uint32_t SD_MODE_LIST = 1;     // 候補 B: 彫った形の一覧
FX_CONST uint32_t SD_MODE_GRID = 2;     // 候補 A: 高さの差の格子

// --- 物質の肌理(作り手が物質に書く値。名前は仮)---
struct SdRelief {
    uint32_t amplitudeUm;     // 最も大きい波の振れ幅(µm)。波ごとに半分
    uint32_t waveShift;       // 最も大きい波の長さ = 2^waveShift µm(15〜19)
    uint32_t octaveCount;     // 波の数(2^15 µm より細かい波は作らない)
    uint32_t crackDepthUm;    // 割れ目の深さ(0 = 割れ目なし)
    uint32_t crackWidthQ16;   // 割れ目の幅(割れ目の雑音の 0 の近くの幅。Q16)
    uint32_t crackWaveShift;  // 割れ目の並びの長さ = 2^crackWaveShift µm
};

// --- 区画の値(刻みの中は変わらない)---
struct SdConfig {
    SdRelief relief;
    uint64_t seed;       // 場所の種(世界の種から)
    uint64_t originXUm;  // 区画の角の世界の座標(µm。乱数の格子を世界で揃える)
    uint64_t originZUm;
    uint32_t columnsX;  // 柱の数(2 以上)
    uint32_t columnsZ;
    uint32_t
        baseSmooth;  // 1 = 下地を柱の中心どうしで線形に補間する / 0 = 柱の中は平ら(セルの固い物の量と形の体積が一致する)
    uint32_t reserved;
};

// --- 彫った形(候補 B の 1 件。32 バイト)---
// 線分 a → b のまわり半径 radiusUm を、中心で depthUm・縁で 0 の放物線の断面で彫る(重なれば深さを足す)
struct SdCarve {
    uint32_t ax;
    uint32_t az;
    uint32_t bx;
    uint32_t bz;
    uint32_t radiusUm;
    uint32_t depthUm;
    uint32_t reserved0;
    uint32_t reserved1;
};

// --- 当たり(物の点 1 つ)---
// 当たりを問う点(物の点 1 つ。区画の中の µm)
struct SdProbePoint {
    uint32_t xUm;
    uint32_t zUm;
    int64_t yUm;
};

struct SdContact {
    int64_t penetrationUm;  // 地面 − 点の高さ(正 = めり込み)
    int32_t slopeXQ16;      // 地面の傾き dG/dx(Q16)
    int32_t slopeZQ16;
};

// --- 区画のデータの読み方(C++ は SdSceneData の span・HLSL は読む側のシェーダーが SD_COLUMN_HEIGHT などを先に定義する)---
#ifdef __cplusplus
struct SdSceneData {
    std::span<const uint32_t> columnHeights;  // 柱の地面の高さ(µm。柱の中心の値。columnsX × columnsZ)
    std::span<const uint32_t> carveBegin;     // 候補 B: 柱ごとの一覧の始まり(柱の数 + 1)
    std::span<const SdCarve> carves;          // 候補 B: 柱ごとに登録した形
    std::span<const uint32_t> gridIndex;      // 候補 A: 柱 → 格子の番号(SD_NO_GRID = 無い)
    std::span<const int32_t> gridNodes;       // 候補 A: 格子の点(µm。格子の番号 × 289 + j × 17 + i)
};
#define SD_SCENE FX_IN(SdSceneData) scene,
#define SD_PASS scene,
#define SD_COLUMN_HEIGHT(i) scene.columnHeights[i]
#define SD_CARVE_BEGIN(i) scene.carveBegin[i]
#define SD_CARVE(i) scene.carves[i]
#define SD_GRID_INDEX(i) scene.gridIndex[i]
#define SD_GRID_NODE(i) scene.gridNodes[i]
#else
#ifndef SD_COLUMN_HEIGHT
#error "surface_detail.hlsli を読む前に SD_COLUMN_HEIGHT などの読み方を定義する(shaders/sim/surface_detail.hlsl)"
#endif
#define SD_SCENE
#define SD_PASS
#endif

// --- 整数の道具 ---

// value × weight ÷ 2^16(weight ≥ 0。0 方向の切り捨て)
FX_FN int64_t SdMulQ16(int64_t value, int64_t weightQ16) {
    FX_ASSERT(weightQ16 >= 0 && weightQ16 <= SD_ONE_Q16);
    const uint64_t magnitude = (FxAbsU64(value) * (uint64_t)weightQ16) >> 16;

    return FxApplySign(magnitude, value < 0);
}

// noiseQ16(−1〜1 の Q16)× amplitude ÷ 2^16(0 方向の切り捨て)
FX_FN int64_t SdScaleQ16(int64_t noiseQ16, uint32_t amplitude) {
    const uint64_t magnitude = (FxAbsU64(noiseQ16) * (uint64_t)amplitude) >> 16;

    return FxApplySign(magnitude, noiseQ16 < 0);
}

FX_FN int64_t SdLerpQ16(int64_t a, int64_t b, int64_t weightQ16) {
    return a + SdMulQ16(b - a, weightQ16);
}

// 3t² − 2t³(Q16。0〜65536)
FX_FN int64_t SdSmoothQ16(int64_t t) {
    return (((t * t) >> 16) * ((3 * SD_ONE_Q16) - (2 * t))) >> 16;
}

// 格子の点の値(Q16。−65536〜65535)。(種, 用途, 波, 格子の座標)だけで決まる(04 R6)
FX_FN int64_t SdLatticeValue(uint64_t seed, uint32_t purpose, uint32_t octave, uint64_t ix, uint64_t iz) {
    const uint64_t hash = FxHash64(seed, (uint64_t)octave, (ix & FX_LOW32_MASK) | (iz << 32), purpose);

    return (int64_t)(hash >> 47) - SD_ONE_Q16;
}

// 値の雑音(波の長さ 2^shift µm。格子の点の値を 3t² − 2t³ で補間。Q16)
FX_FN int64_t SdValueNoise(uint64_t seed, uint32_t purpose, uint32_t octave, uint32_t shift, uint64_t x, uint64_t z) {
    const uint64_t mask = ((uint64_t)1 << shift) - 1;
    const uint64_t ix = x >> shift;
    const uint64_t iz = z >> shift;
    const int64_t sx = SdSmoothQ16((int64_t)(((x & mask) << 16) >> shift));
    const int64_t sz = SdSmoothQ16((int64_t)(((z & mask) << 16) >> shift));

    const int64_t v00 = SdLatticeValue(seed, purpose, octave, ix, iz);
    const int64_t v10 = SdLatticeValue(seed, purpose, octave, ix + 1, iz);
    const int64_t v01 = SdLatticeValue(seed, purpose, octave, ix, iz + 1);
    const int64_t v11 = SdLatticeValue(seed, purpose, octave, ix + 1, iz + 1);

    return SdLerpQ16(SdLerpQ16(v00, v10, sx), SdLerpQ16(v01, v11, sx), sz);
}

// --- 自然の細かい形(µm。記録しない)---
FX_FN int64_t SdNaturalDetailUm(FX_IN(SdConfig) config, uint32_t xUm, uint32_t zUm) {
    const uint64_t x = config.originXUm + xUm;
    const uint64_t z = config.originZUm + zUm;
    int64_t height = 0;
    for (uint32_t octave = 0; octave < config.relief.octaveCount; ++octave) {
        if (config.relief.waveShift < SD_MIN_WAVE_SHIFT + octave)
            break;

        const uint32_t shift = config.relief.waveShift - octave;
        const int64_t octaveValue = SdValueNoise(config.seed, SD_PURPOSE_DETAIL, octave, shift, x, z);
        height += SdScaleQ16(octaveValue, config.relief.amplitudeUm >> octave);
    }

    // 割れ目: 割れ目の雑音が 0 に近い細い帯を、中心で crackDepthUm の V 字に彫る
    // (64bit の「0 で止める引き算」は HW で誤るので、符号つきの差で判定する。T-0185)
    const int64_t width = (int64_t)config.relief.crackWidthQ16;
    if (config.relief.crackDepthUm == 0 || width == 0)
        return height;

    const int64_t crack = SdValueNoise(config.seed, SD_PURPOSE_CRACK, 0, config.relief.crackWaveShift, x, z);
    const int64_t inside = width - (int64_t)FxAbsU64(crack);
    if (inside <= 0)
        return height;

    return height - (((int64_t)config.relief.crackDepthUm * inside) / width);
}

// --- 下地: 柱の中心どうしの線形の補間(区画の端の外は端の柱の値)---
struct SdAxisWeight {
    uint32_t index;     // 低い側の柱
    int64_t weightQ16;  // 高い側の重み
};

FX_FN SdAxisWeight SdCenterWeight(uint32_t positionUm, uint32_t columnCount) {
    SdAxisWeight result;
    result.index = 0;
    result.weightQ16 = 0;
    if (positionUm <= SD_HALF_CELL_UM)
        return result;

    const uint32_t fromCenter = positionUm - SD_HALF_CELL_UM;
    const uint32_t index = fromCenter / SD_CELL_UM;
    if (index + 1 >= columnCount) {
        result.index = columnCount - 2;
        result.weightQ16 = SD_ONE_Q16;
        return result;
    }

    result.index = index;
    result.weightQ16 = (int64_t)((((uint64_t)(fromCenter - (index * SD_CELL_UM))) << 16) / SD_CELL_UM);

    return result;
}

FX_FN int64_t SdBaseHeightUm(SD_SCENE FX_IN(SdConfig) config, uint32_t xUm, uint32_t zUm) {
    if (config.baseSmooth == 0)
        return (int64_t)SD_COLUMN_HEIGHT((xUm / SD_CELL_UM) + ((zUm / SD_CELL_UM) * config.columnsX));

    const SdAxisWeight wx = SdCenterWeight(xUm, config.columnsX);
    const SdAxisWeight wz = SdCenterWeight(zUm, config.columnsZ);
    const uint32_t corner = wx.index + (wz.index * config.columnsX);
    const int64_t h00 = (int64_t)SD_COLUMN_HEIGHT(corner);
    const int64_t h10 = (int64_t)SD_COLUMN_HEIGHT(corner + 1);
    const int64_t h01 = (int64_t)SD_COLUMN_HEIGHT(corner + config.columnsX);
    const int64_t h11 = (int64_t)SD_COLUMN_HEIGHT(corner + config.columnsX + 1);

    return SdLerpQ16(SdLerpQ16(h00, h10, wx.weightQ16), SdLerpQ16(h01, h11, wx.weightQ16), wz.weightQ16);
}

// --- 候補 B: 彫った形 1 件の深さ(µm。線分から半径の外は 0)---
FX_FN int64_t SdCarveDepthUm(FX_IN(SdCarve) carve, uint32_t xUm, uint32_t zUm) {
    const int64_t dx = (int64_t)carve.bx - (int64_t)carve.ax;
    const int64_t dz = (int64_t)carve.bz - (int64_t)carve.az;
    const int64_t wx = (int64_t)xUm - (int64_t)carve.ax;
    const int64_t wz = (int64_t)zUm - (int64_t)carve.az;
    const int64_t length2 = (dx * dx) + (dz * dz);

    // 線分の上の最も近い点 q = a + d × t(t は Q16。0〜1 に収める)
    int64_t qx = carve.ax;
    int64_t qz = carve.az;
    if (length2 != 0) {
        int64_t along = (wx * dx) + (wz * dz);
        along = along < 0 ? 0 : (along > length2 ? length2 : along);
        const int64_t tQ16 = (int64_t)((((uint64_t)along) << 16) / (uint64_t)length2);
        qx += SdMulQ16(dx, tQ16);
        qz += SdMulQ16(dz, tQ16);
    }

    const int64_t radius = carve.radiusUm;
    const int64_t ex = (int64_t)xUm - qx;
    const int64_t ez = (int64_t)zUm - qz;
    if (ex > radius || ex < -radius || ez > radius || ez < -radius)
        return 0;

    const int64_t distance2 = (ex * ex) + (ez * ez);
    const int64_t radius2 = radius * radius;
    if (distance2 >= radius2)
        return 0;

    return (int64_t)(((uint64_t)carve.depthUm * (uint64_t)(radius2 - distance2)) / (uint64_t)radius2);
}

// 柱 column に登録した形の深さの和
FX_FN int64_t SdListCarveUm(SD_SCENE uint32_t column, uint32_t xUm, uint32_t zUm) {
    const uint32_t end = SD_CARVE_BEGIN(column + 1);
    int64_t carved = 0;
    for (uint32_t i = SD_CARVE_BEGIN(column); i < end; ++i)
        carved += SdCarveDepthUm(SD_CARVE(i), xUm, zUm);

    return carved;
}

// --- 候補 A: 高さの差の格子(柱の中の位置 local。µm)---
FX_FN int64_t SdGridDeltaUm(SD_SCENE uint32_t column, uint32_t localXUm, uint32_t localZUm) {
    const uint32_t grid = SD_GRID_INDEX(column);
    if (grid == SD_NO_GRID)
        return 0;

    const uint32_t i = localXUm / SD_GRID_SPACING_UM;
    const uint32_t j = localZUm / SD_GRID_SPACING_UM;
    const int64_t tx = (int64_t)((((uint64_t)(localXUm - (i * SD_GRID_SPACING_UM))) << 16) / SD_GRID_SPACING_UM);
    const int64_t tz = (int64_t)((((uint64_t)(localZUm - (j * SD_GRID_SPACING_UM))) << 16) / SD_GRID_SPACING_UM);
    const uint32_t base = (grid * SD_GRID_NODE_COUNT) + i + (j * SD_GRID_NODES);
    const int64_t n00 = (int64_t)SD_GRID_NODE(base);
    const int64_t n10 = (int64_t)SD_GRID_NODE(base + 1);
    const int64_t n01 = (int64_t)SD_GRID_NODE(base + SD_GRID_NODES);
    const int64_t n11 = (int64_t)SD_GRID_NODE(base + SD_GRID_NODES + 1);

    return SdLerpQ16(SdLerpQ16(n00, n10, tx), SdLerpQ16(n01, n11, tx), tz);
}

// --- 地面の高さ(µm。区画の床から)。区画の外の点は区画の端(最後の 1 µm)に寄せる ---
FX_FN int64_t SdGroundHeightUm(SD_SCENE FX_IN(SdConfig) config, uint32_t mode, uint32_t xUm, uint32_t zUm) {
    const uint32_t lastX = (config.columnsX * SD_CELL_UM) - 1;
    const uint32_t lastZ = (config.columnsZ * SD_CELL_UM) - 1;
    xUm = xUm > lastX ? lastX : xUm;
    zUm = zUm > lastZ ? lastZ : zUm;
    int64_t height = SdBaseHeightUm(SD_PASS config, xUm, zUm) + SdNaturalDetailUm(config, xUm, zUm);
    const uint32_t columnX = xUm / SD_CELL_UM;
    const uint32_t columnZ = zUm / SD_CELL_UM;
    const uint32_t column = columnX + (columnZ * config.columnsX);
    if (mode == SD_MODE_LIST)
        height -= SdListCarveUm(SD_PASS column, xUm, zUm);
    else if (mode == SD_MODE_GRID)
        height += SdGridDeltaUm(SD_PASS column, xUm - (columnX * SD_CELL_UM), zUm - (columnZ * SD_CELL_UM));

    return height;
}

// --- 当たり: 物の点(x, y, z)と地面(めり込みと傾き。傾きは 1 cm 先との差)---
FX_FN SdContact SdContactAt(SD_SCENE FX_IN(SdConfig) config, uint32_t mode, uint32_t xUm, uint32_t zUm, int64_t yUm) {
    const int64_t here = SdGroundHeightUm(SD_PASS config, mode, xUm, zUm);
    const int64_t alongX = SdGroundHeightUm(SD_PASS config, mode, xUm + (uint32_t)SD_GRADIENT_STEP_UM, zUm);
    const int64_t alongZ = SdGroundHeightUm(SD_PASS config, mode, xUm, zUm + (uint32_t)SD_GRADIENT_STEP_UM);

    SdContact contact;
    contact.penetrationUm = here - yUm;
    contact.slopeXQ16 = (int32_t)FxDivS64((alongX - here) * SD_ONE_Q16, SD_GRADIENT_STEP_UM);
    contact.slopeZQ16 = (int32_t)FxDivS64((alongZ - here) * SD_ONE_Q16, SD_GRADIENT_STEP_UM);

    return contact;
}

// --- 開き具合 ---

// 地面がセルの中を埋める高さ(0〜0.5 m)
FX_FN uint64_t SdFilledUm(int64_t groundUm, int64_t bottomUm) {
    const int64_t filled = groundUm - bottomUm;
    if (filled <= 0)
        return 0;

    return filled >= (int64_t)SD_CELL_UM ? (uint64_t)SD_CELL_UM : (uint64_t)filled;
}

// 面の上の 16 点の埋まりの和から、面の開き(Q16)
FX_FN uint32_t SdFaceOpenQ16(uint64_t filledSumUm) {
    const uint64_t filledQ16 = (filledSumUm << 16) / ((uint64_t)SD_SAMPLE_SIDE * SD_CELL_UM);

    return (uint32_t)((uint64_t)SD_ONE_Q16 - filledQ16);
}

// --- 開き具合: 柱 1 本の、下から SD_COLUMN_LAYERS 個のセル(床が bottomUm のセルから上へ)---
// 標本は柱の中 256 点(16 × 16 の小さな四角の中心)と、+ x・+ z の面の上 16 点ずつ。標本ごとの地面の高さは 1 回だけ計算し、
// 4 つのセルの和に足す(和は整数なので足す順によらない。GPU は 1 グループ = 1 柱で標本を分けて groupshared に足す)。
FX_CONST uint32_t SD_COLUMN_LAYERS = 4;
FX_CONST uint32_t SD_INTERIOR_SAMPLES = 256;
FX_CONST int64_t SD_SEALED_GROUND_UM = (int64_t)FX_U64(0x00007FFFu, 0xFFFFFFFFu);  // 区画の端の面を閉じる高さ

struct SdOpennessSums {
    uint32_t fill[4];     // 埋まった高さの和(µm。256 点 × 0.5 m = 1.28e8 まで)
    uint32_t openTop[4];  // 上が開いている点の数
    uint32_t faceX[4];    // + x の面の上の埋まりの和(µm)
    uint32_t faceZ[4];
};

struct SdColumnOpenness {
    uint32_t fillQ16[4];     // 固い物の割合(地面の下の体積 ÷ セル)
    uint32_t openXQ16[4];    // + x の面の開き(区画の端の面は 0)
    uint32_t openZQ16[4];    // + z の面
    uint32_t openTopQ16[4];  // 上の面(地面がセルの上に届かない所の割合)
};

FX_FN SdOpennessSums SdEmptySums() {
    SdOpennessSums sums;
    FX_UNROLL
    for (uint32_t layer = 0; layer < SD_COLUMN_LAYERS; ++layer) {
        sums.fill[layer] = 0;
        sums.openTop[layer] = 0;
        sums.faceX[layer] = 0;
        sums.faceZ[layer] = 0;
    }

    return sums;
}

// 柱の中の標本 sampleIndex(0〜255)の地面の高さ
FX_FN int64_t SdInteriorGroundUm(SD_SCENE FX_IN(SdConfig) config, uint32_t mode, uint32_t columnX, uint32_t columnZ,
                                 uint32_t sampleIndex) {
    const uint32_t x = (columnX * SD_CELL_UM) + (((2 * (sampleIndex % SD_SAMPLE_SIDE)) + 1) * SD_SAMPLE_HALF_STEP_UM);
    const uint32_t z = (columnZ * SD_CELL_UM) + (((2 * (sampleIndex / SD_SAMPLE_SIDE)) + 1) * SD_SAMPLE_HALF_STEP_UM);

    return SdGroundHeightUm(SD_PASS config, mode, x, z);
}

// 面の標本 k(0〜15)の地面の高さ(axisZ = 0 は + x の面、1 は + z の面。区画の端の面は閉じる高さ)
FX_FN int64_t SdFaceGroundUm(SD_SCENE FX_IN(SdConfig) config, uint32_t mode, uint32_t columnX, uint32_t columnZ,
                             uint32_t axisZ, uint32_t k) {
    const uint32_t offset = ((2 * k) + 1) * SD_SAMPLE_HALF_STEP_UM;
    if (axisZ == 0) {
        if (columnX + 1 >= config.columnsX)
            return SD_SEALED_GROUND_UM;

        return SdGroundHeightUm(SD_PASS config, mode, (columnX + 1) * SD_CELL_UM, (columnZ * SD_CELL_UM) + offset);
    }

    if (columnZ + 1 >= config.columnsZ)
        return SD_SEALED_GROUND_UM;

    return SdGroundHeightUm(SD_PASS config, mode, (columnX * SD_CELL_UM) + offset, (columnZ + 1) * SD_CELL_UM);
}

FX_FN SdOpennessSums SdAddInterior(SdOpennessSums sums, int64_t groundUm, int64_t bottomUm) {
    FX_UNROLL
    for (uint32_t layer = 0; layer < SD_COLUMN_LAYERS; ++layer) {
        const uint64_t filled = SdFilledUm(groundUm, bottomUm + ((int64_t)layer * SD_CELL_UM));
        sums.fill[layer] += (uint32_t)filled;
        sums.openTop[layer] += filled < SD_CELL_UM ? 1u : 0u;
    }

    return sums;
}

FX_FN SdOpennessSums SdAddFace(SdOpennessSums sums, uint32_t axisZ, int64_t groundUm, int64_t bottomUm) {
    FX_UNROLL
    for (uint32_t layer = 0; layer < SD_COLUMN_LAYERS; ++layer) {
        const uint32_t filled = (uint32_t)SdFilledUm(groundUm, bottomUm + ((int64_t)layer * SD_CELL_UM));
        if (axisZ == 0)
            sums.faceX[layer] += filled;
        else
            sums.faceZ[layer] += filled;
    }

    return sums;
}

FX_FN SdColumnOpenness SdFinishOpenness(FX_IN(SdOpennessSums) sums) {
    SdColumnOpenness result;
    FX_UNROLL
    for (uint32_t layer = 0; layer < SD_COLUMN_LAYERS; ++layer) {
        result.fillQ16[layer] = (uint32_t)(((uint64_t)sums.fill[layer] << 16) /
                                           ((uint64_t)SD_INTERIOR_SAMPLES * SD_CELL_UM));
        result.openXQ16[layer] = SdFaceOpenQ16(sums.faceX[layer]);
        result.openZQ16[layer] = SdFaceOpenQ16(sums.faceZ[layer]);
        result.openTopQ16[layer] = (uint32_t)(((uint64_t)sums.openTop[layer] << 16) / SD_INTERIOR_SAMPLES);
    }

    return result;
}

// CPU の回し方(1 柱を順に)。GPU は shaders/sim/surface_detail.hlsl の Openness が標本を 64 スレッドに分ける
FX_FN SdColumnOpenness SdColumnOpennessAt(SD_SCENE FX_IN(SdConfig) config, uint32_t mode, uint32_t columnX,
                                          uint32_t columnZ, int64_t bottomUm) {
    SdOpennessSums sums = SdEmptySums();
    for (uint32_t sampleIndex = 0; sampleIndex < SD_INTERIOR_SAMPLES; ++sampleIndex)
        sums = SdAddInterior(sums, SdInteriorGroundUm(SD_PASS config, mode, columnX, columnZ, sampleIndex), bottomUm);

    for (uint32_t k = 0; k < SD_SAMPLE_SIDE; ++k) {
        sums = SdAddFace(sums, 0, SdFaceGroundUm(SD_PASS config, mode, columnX, columnZ, 0, k), bottomUm);
        sums = SdAddFace(sums, 1, SdFaceGroundUm(SD_PASS config, mode, columnX, columnZ, 1, k), bottomUm);
    }

    return SdFinishOpenness(sums);
}

// --- 開き具合: 1 辺を選べるセル 1 つ(T-0937。多重解像度のレベル k のセル = 0.5 m ÷ 2^k)---
// SdColumnOpennessAt と同じ数え方(中 16 × 16 点・+ x と + z の面の上 16 点ずつ)を、角 (x0Um, bottomUm, z0Um)・1 辺 edgeUm のセルで。
// 違いは面の上の点の高さ: 面の両側(面の 1 µm 手前と面の上)の高い方を取る(面は両側のどちらかの固い物が触れていれば塞がる。
// SdColumnOpennessAt は + 側の柱だけを見る)。区画の端の面は、外の形を知らないので手前の側だけ(SdGroundHeightUm が端に寄せる)。
// セルの面の開き具合の欄(common/face_opening.hlsli)を形から導く時に読む(CPU は sim/face_opening)。
struct SdCellOpenness {
    uint32_t fillQ16;     // 固い物の割合(地面の下の体積 ÷ セル)
    uint32_t openXQ16;    // + x の面の開き(区画の端の面は手前の側の形だけで)
    uint32_t openZQ16;    // + z の面
    uint32_t openTopQ16;  // 上の面(地面がセルの上に届かない所の割合)
};

// 1 辺 edgeUm を SD_SAMPLE_SIDE 個に分けた k 番目の小さな区間の中心(セルの角から)
FX_FN uint32_t SdSampleOffsetUm(uint32_t edgeUm, uint32_t k) {
    return (uint32_t)(((uint64_t)((2 * k) + 1) * edgeUm) / (2 * SD_SAMPLE_SIDE));
}

// 面の上の点の地面の高さ(面の 1 µm 手前と面の上の高い方)
FX_FN int64_t SdFaceSideMaxUm(SD_SCENE FX_IN(SdConfig) config, uint32_t mode, uint32_t xUm, uint32_t zUm,
                              uint32_t axisZ) {
    const int64_t onFace = SdGroundHeightUm(SD_PASS config, mode, xUm, zUm);
    const int64_t before = axisZ == 0 ? SdGroundHeightUm(SD_PASS config, mode, xUm - 1, zUm)
                                      : SdGroundHeightUm(SD_PASS config, mode, xUm, zUm - 1);

    return onFace > before ? onFace : before;
}

// 地面が 1 辺 edgeUm のセルの中を埋める高さ(0〜edgeUm)
FX_FN uint64_t SdFilledInUm(int64_t groundUm, int64_t bottomUm, uint32_t edgeUm) {
    const int64_t filled = groundUm - bottomUm;
    if (filled <= 0)
        return 0;

    return filled >= (int64_t)edgeUm ? (uint64_t)edgeUm : (uint64_t)filled;
}

// 面の上の SD_SAMPLE_SIDE 点の埋まりの和から、面の開き(Q16)
FX_FN uint32_t SdCellFaceOpenQ16(uint64_t filledSumUm, uint32_t edgeUm) {
    const uint64_t filledQ16 = (filledSumUm << 16) / ((uint64_t)SD_SAMPLE_SIDE * edgeUm);

    return (uint32_t)((uint64_t)SD_ONE_Q16 - filledQ16);
}

FX_FN SdCellOpenness SdCellOpennessAt(SD_SCENE FX_IN(SdConfig) config, uint32_t mode, uint32_t x0Um, uint32_t z0Um,
                                      int64_t bottomUm, uint32_t edgeUm) {
    uint64_t fill = 0;
    uint32_t openTop = 0;
    for (uint32_t j = 0; j < SD_SAMPLE_SIDE; ++j) {
        for (uint32_t i = 0; i < SD_SAMPLE_SIDE; ++i) {
            const int64_t ground = SdGroundHeightUm(SD_PASS config, mode, x0Um + SdSampleOffsetUm(edgeUm, i),
                                                    z0Um + SdSampleOffsetUm(edgeUm, j));
            const uint64_t filled = SdFilledInUm(ground, bottomUm, edgeUm);
            fill += filled;
            openTop += filled < edgeUm ? 1u : 0u;
        }
    }

    uint64_t faceX = 0;
    uint64_t faceZ = 0;
    for (uint32_t k = 0; k < SD_SAMPLE_SIDE; ++k) {
        const uint32_t offset = SdSampleOffsetUm(edgeUm, k);
        faceX += SdFilledInUm(SdFaceSideMaxUm(SD_PASS config, mode, x0Um + edgeUm, z0Um + offset, 0), bottomUm, edgeUm);
        faceZ += SdFilledInUm(SdFaceSideMaxUm(SD_PASS config, mode, x0Um + offset, z0Um + edgeUm, 1), bottomUm, edgeUm);
    }

    SdCellOpenness result;
    result.fillQ16 = (uint32_t)((fill << 16) / ((uint64_t)SD_INTERIOR_SAMPLES * edgeUm));
    result.openXQ16 = SdCellFaceOpenQ16(faceX, edgeUm);
    result.openZQ16 = SdCellFaceOpenQ16(faceZ, edgeUm);
    result.openTopQ16 = (uint32_t)(((uint64_t)openTop << 16) / SD_INTERIOR_SAMPLES);

    return result;
}

SD_NAMESPACE_END

#endif  // BICAMERAL_SURFACE_DETAIL_HLSLI
