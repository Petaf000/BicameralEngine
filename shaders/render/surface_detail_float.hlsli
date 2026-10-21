// surface_detail_float.hlsli — 表面の細かい形の描画の版(小数。23 §7・R-T1・T-0879)。法則の整数の版(common/surface_detail.hlsli)と
// 同じ形を同じ乱数(格子の点の値は整数のハッシュ)で作り、補間と重ね合わせだけを float で行う。描画は状態を変えない(23 §1)。
// HLSL と C++ の両方でコンパイルする(C++ は試験がずれを測るため。tests/surface_detail_test.cpp)。
//
// 位置は区画の角からの m(float)。乱数の格子の番号は「区画の角の格子の番号(整数)+ floor(位置 ÷ 波の長さ)」で数える
// (区画の角は最も大きい波 2^20 µm にそろえる約束。そろっていれば格子の番号は整数の版と同じになる)。
#ifndef BICAMERAL_SURFACE_DETAIL_FLOAT_HLSLI
#define BICAMERAL_SURFACE_DETAIL_FLOAT_HLSLI

#include "common/surface_detail.hlsli"

#ifdef __cplusplus
#include <cmath>
#define SDF_FLOOR(value) std::floor(value)
#define SDF_FN inline  // std::floor は constexpr でないので FX_FN(constexpr)にしない
#define SDF_NAMESPACE_BEGIN        \
    namespace bicameral::surface { \
        using namespace ::bicameral::fx;
#define SDF_NAMESPACE_END }
#else
#define SDF_FLOOR(value) floor(value)
#define SDF_FN
#define SDF_NAMESPACE_BEGIN
#define SDF_NAMESPACE_END
#endif

SDF_NAMESPACE_BEGIN

FX_CONST float SDF_METERS_PER_UM = 0.000001f;
FX_CONST float SDF_CELL_M = 0.5f;

// 格子の点の値(−1〜1)。整数の版の SdLatticeValue と同じハッシュ
SDF_FN float SdfLatticeValue(uint64_t seed, uint32_t purpose, uint32_t octave, uint64_t ix, uint64_t iz) {
    return (float)SdLatticeValue(seed, purpose, octave, ix, iz) * (1.0f / 65536.0f);
}

SDF_FN float SdfSmooth(float t) {
    return t * t * (3.0f - (2.0f * t));
}

SDF_FN float SdfLerp(float a, float b, float t) {
    return a + ((b - a) * t);
}

// 値の雑音(波の長さ 2^shift µm)。origin は区画の角の世界の座標(µm。2^shift の倍数)
SDF_FN float SdfValueNoise(uint64_t seed, uint32_t purpose, uint32_t octave, uint32_t shift, uint64_t originXUm,
                           uint64_t originZUm, float xM, float zM) {
    const float waveM = (float)((uint64_t)1 << shift) * SDF_METERS_PER_UM;
    const float u = xM / waveM;
    const float v = zM / waveM;
    const float cellU = SDF_FLOOR(u);
    const float cellV = SDF_FLOOR(v);
    const uint64_t ix = (originXUm >> shift) + (uint64_t)(int64_t)cellU;
    const uint64_t iz = (originZUm >> shift) + (uint64_t)(int64_t)cellV;
    const float sx = SdfSmooth(u - cellU);
    const float sz = SdfSmooth(v - cellV);

    const float v00 = SdfLatticeValue(seed, purpose, octave, ix, iz);
    const float v10 = SdfLatticeValue(seed, purpose, octave, ix + 1, iz);
    const float v01 = SdfLatticeValue(seed, purpose, octave, ix, iz + 1);
    const float v11 = SdfLatticeValue(seed, purpose, octave, ix + 1, iz + 1);

    return SdfLerp(SdfLerp(v00, v10, sx), SdfLerp(v01, v11, sx), sz);
}

SDF_FN float SdfNaturalDetailM(FX_IN(SdConfig) config, float xM, float zM) {
    float height = 0.0f;
    for (uint32_t octave = 0; octave < config.relief.octaveCount; ++octave) {
        if (config.relief.waveShift < SD_MIN_WAVE_SHIFT + octave)
            break;

        const uint32_t shift = config.relief.waveShift - octave;
        const float octaveValue = SdfValueNoise(config.seed, SD_PURPOSE_DETAIL, octave, shift, config.originXUm,
                                                config.originZUm, xM, zM);
        height += octaveValue * (float)(config.relief.amplitudeUm >> octave) * SDF_METERS_PER_UM;
    }

    const float width = (float)config.relief.crackWidthQ16 * (1.0f / 65536.0f);
    if (config.relief.crackDepthUm == 0 || config.relief.crackWidthQ16 == 0)
        return height;

    const float crack = SdfValueNoise(config.seed, SD_PURPOSE_CRACK, 0, config.relief.crackWaveShift, config.originXUm,
                                      config.originZUm, xM, zM);
    const float inside = width - (crack < 0.0f ? -crack : crack);
    if (inside <= 0.0f)
        return height;

    return height - ((float)config.relief.crackDepthUm * SDF_METERS_PER_UM * inside / width);
}

SDF_FN float SdfBaseHeightM(SD_SCENE FX_IN(SdConfig) config, float xM, float zM) {
    const uint32_t columnX = (uint32_t)(xM / SDF_CELL_M);
    const uint32_t columnZ = (uint32_t)(zM / SDF_CELL_M);
    if (config.baseSmooth == 0)
        return (float)SD_COLUMN_HEIGHT(columnX + (columnZ * config.columnsX)) * SDF_METERS_PER_UM;

    // 柱の中心どうしの線形の補間(端の外は端の柱)
    float u = (xM / SDF_CELL_M) - 0.5f;
    float v = (zM / SDF_CELL_M) - 0.5f;
    const float lastU = (float)(config.columnsX - 1);
    const float lastV = (float)(config.columnsZ - 1);
    u = u < 0.0f ? 0.0f : (u > lastU ? lastU : u);
    v = v < 0.0f ? 0.0f : (v > lastV ? lastV : v);
    uint32_t iu = (uint32_t)u;
    uint32_t iv = (uint32_t)v;
    iu = iu + 1 >= config.columnsX ? config.columnsX - 2 : iu;
    iv = iv + 1 >= config.columnsZ ? config.columnsZ - 2 : iv;
    const float tu = u - (float)iu;
    const float tv = v - (float)iv;
    const uint32_t corner = iu + (iv * config.columnsX);
    const float h00 = (float)SD_COLUMN_HEIGHT(corner);
    const float h10 = (float)SD_COLUMN_HEIGHT(corner + 1);
    const float h01 = (float)SD_COLUMN_HEIGHT(corner + config.columnsX);
    const float h11 = (float)SD_COLUMN_HEIGHT(corner + config.columnsX + 1);

    return SdfLerp(SdfLerp(h00, h10, tu), SdfLerp(h01, h11, tu), tv) * SDF_METERS_PER_UM;
}

SDF_FN float SdfCarveDepthM(FX_IN(SdCarve) carve, float xM, float zM) {
    const float ax = (float)carve.ax * SDF_METERS_PER_UM;
    const float az = (float)carve.az * SDF_METERS_PER_UM;
    const float dx = ((float)carve.bx * SDF_METERS_PER_UM) - ax;
    const float dz = ((float)carve.bz * SDF_METERS_PER_UM) - az;
    const float length2 = (dx * dx) + (dz * dz);
    float t = 0.0f;
    if (length2 > 0.0f) {
        t = (((xM - ax) * dx) + ((zM - az) * dz)) / length2;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }

    const float ex = xM - (ax + (dx * t));
    const float ez = zM - (az + (dz * t));
    const float radius = (float)carve.radiusUm * SDF_METERS_PER_UM;
    const float distance2 = (ex * ex) + (ez * ez);
    if (distance2 >= radius * radius)
        return 0.0f;

    return (float)carve.depthUm * SDF_METERS_PER_UM * (1.0f - (distance2 / (radius * radius)));
}

SDF_FN float SdfGroundHeightM(SD_SCENE FX_IN(SdConfig) config, uint32_t mode, float xM, float zM) {
    float height = SdfBaseHeightM(SD_PASS config, xM, zM) + SdfNaturalDetailM(config, xM, zM);
    const uint32_t columnX = (uint32_t)(xM / SDF_CELL_M);
    const uint32_t columnZ = (uint32_t)(zM / SDF_CELL_M);
    const uint32_t column = columnX + (columnZ * config.columnsX);
    if (mode == SD_MODE_LIST) {
        const uint32_t end = SD_CARVE_BEGIN(column + 1);
        for (uint32_t i = SD_CARVE_BEGIN(column); i < end; ++i)
            height -= SdfCarveDepthM(SD_CARVE(i), xM, zM);
    } else if (mode == SD_MODE_GRID) {
        const uint32_t grid = SD_GRID_INDEX(column);
        if (grid != SD_NO_GRID) {
            const float spacingM = (float)SD_GRID_SPACING_UM * SDF_METERS_PER_UM;
            const float u = (xM - ((float)columnX * SDF_CELL_M)) / spacingM;
            const float v = (zM - ((float)columnZ * SDF_CELL_M)) / spacingM;
            uint32_t i = (uint32_t)u;
            uint32_t j = (uint32_t)v;
            i = i >= SD_GRID_STEPS ? SD_GRID_STEPS - 1 : i;
            j = j >= SD_GRID_STEPS ? SD_GRID_STEPS - 1 : j;
            const uint32_t base = (grid * SD_GRID_NODE_COUNT) + i + (j * SD_GRID_NODES);
            const float n00 = (float)SD_GRID_NODE(base);
            const float n10 = (float)SD_GRID_NODE(base + 1);
            const float n01 = (float)SD_GRID_NODE(base + SD_GRID_NODES);
            const float n11 = (float)SD_GRID_NODE(base + SD_GRID_NODES + 1);
            const float delta = SdfLerp(SdfLerp(n00, n10, u - (float)i), SdfLerp(n01, n11, u - (float)i), v - (float)j);
            height += delta * SDF_METERS_PER_UM;
        }
    }

    return height;
}

SDF_NAMESPACE_END

#endif  // BICAMERAL_SURFACE_DETAIL_FLOAT_HLSLI
