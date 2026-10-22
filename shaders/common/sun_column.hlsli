// sun_column.hlsli — 太陽の影の柱: 向き 1 つの平行な光線の束で、世界の多重解像度の木を掃く(docs/design/23-radiation.md §3.1・ADR-0396。T-0910)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU リファレンス(engine/src/sim/sun_column.cpp の SweepSunColumn)と
// GPU(shaders/sim/sun_column.hlsl の SunSweep)が同じ関数 SunWalkRay を呼ぶ。
//
// データの流れ:
//   太陽(向き・帯ごとの強さ。天候の層・環境の値。今は呼ぶ側が渡す)+ 世界の箱 → SunParams(engine/src/sim/sun_column.cpp の MakeSunParams)
//   → 光線 1 本ごとに SunWalkRay: 光の向きの一番大きい軸 A の板(光線の細かさ = rayLevel のセル 1 枚)ごとに、板の真ん中の点を含む葉のセルを
//     出し(今いるブロックの中は座標から。出たら木で引く: MrFindContaining。T-0957)、同じ葉が続く間を 1 区間にまとめる → 区間の光学的な厚さ τ = 吸い込みの和 × 道のり → 通す割合 e^−τ(Q16)
//   → 減った分のうち照り返しの割合は「返す」、残りは「吸う」(Sink の Deposit。セルごとに足す)。残りは先へ。箱を出た分は「外へ」
// 約束:
//   - 光線は rayLevel のセルの間隔の格子(A に垂直な面の上。セルの真ん中を通る)。1 本の持つ量 = 強さ × 面 × 刻み × cos(整数に切り捨て)。
//     同じ面積なら細かさによらず同じ量(細かさによらない一貫性)。量の単位は rayLevel のエネルギーの単位(8^−rayLevel mJ)。
//   - 保存: 光線ごとに 入った = 吸った + 返した + 外へ(整数の引き算で分けるのでビット単位)。足すのは可換な整数の足し算だけ(R4)。
//     どの順に光線を走らせても同じ結果(R3)。
//   - 通さないセル(τ ≥ 32)は量を全部取る(陰はちょうど 0)。
//   - 道のり: 板 1 枚の厚さ(rayLevel のセル)× |D| ÷ |D_A|。区間の τ = 深さ(レベル 0 の式。セルの量の数はレベルによらず密度の比)× 板の数 × |D| ÷ (|D_A| × 2^rayLevel)。
// Tree の約束(multires_activity.hlsli と同じ): MrBlock Block(uint32_t slot) と uint32_t Lookup(level, originX, originY, originZ)。
// Cells の約束: RxCell Cell(uint32_t slot, uint32_t index)。Lights の約束(light.hlsli): LtSpeciesLight Light(uint32_t speciesId)。
// Sink の約束: void Deposit(slot, index, band, absorbed, reflected)(セルごとに足す。状態を持たない: 値で渡しても同じ所へ書く)。
// 帳簿(入った・吸った・返した・外へ)は SunWalkRay が光線ごとに返し、呼ぶ側が足す。
// 物に取らせる口(T-0878・R-R2・ADR-0425): SunWalkRayCaught は Catcher の約束 SunCatch Catch(rayB, rayC) で、決まった板で
// 光線の残りのうち count ÷ 2^shift を「取られた」(caught)にする(レンズの物に入った量)。入った = 吸った + 返した + 外へ + 取られた。
// SunWalkRay は何も取らない版(SunNoCatch)で、前と同じ語を出す。
// 浮動小数点は使わない。
#ifndef BICAMERAL_SUN_COLUMN_HLSLI
#define BICAMERAL_SUN_COLUMN_HLSLI

#include "light.hlsli"
#include "multires_tree.hlsli"

MR_NAMESPACE_BEGIN

// --- 大きさと単位 -------------------------------------------------------------------------------
FX_CONST uint32_t SUN_DIRECTION_LIMIT = 32768;  // 向きの成分の絶対値の上限(2^15。|D|² が 2^32 未満)
FX_CONST uint32_t SUN_LENGTH_SHIFT = 15;        // |D| を Q15 で持つ
FX_CONST uint32_t SUN_TICKS_PER_SECOND = 60;
FX_CONST uint32_t SUN_RAY_LEVEL_LIMIT = 20;  // 光線の細かさの上限(1 本の量の掛け算を 128bit に収める)
// 1 本の量 = 強さ(mW/m²)× 面(0.25 × 4^−L m²)× 刻み(1/60 s)× 8^L × cos = 強さ × 2^L × cos ÷ 240
FX_CONST uint32_t SUN_ENERGY_DIVISOR = 4 * SUN_TICKS_PER_SECOND;

// --- 出力の並び(uint64 の語。CPU の SunColumnResult::words と GPU の u5 が同じ)---
// [帳簿: 帯 × (入った・吸った・返した・外へ)][セル: (枠 × 512 + 番号) × (吸った 短・長、返した 短・長)]
FX_CONST uint32_t SUN_LEDGER_ENTERED = 0;
FX_CONST uint32_t SUN_LEDGER_ABSORBED = 1;
FX_CONST uint32_t SUN_LEDGER_REFLECTED = 2;
FX_CONST uint32_t SUN_LEDGER_ESCAPED = 3;
FX_CONST uint32_t SUN_LEDGER_ENTRIES = 4;
FX_CONST uint32_t SUN_LEDGER_WORDS = LT_BANDS * SUN_LEDGER_ENTRIES;
FX_CONST uint32_t SUN_KIND_ABSORBED = 0;
FX_CONST uint32_t SUN_KIND_REFLECTED = 1;
FX_CONST uint32_t SUN_CELL_WORDS = 2 * LT_BANDS;
FX_CONST uint32_t SUN_WORK_PARAMS_BYTES = 128;  // GPU の作業のバッファ(u4): [SunParams][LtSpeciesLight × 物質]
FX_CONST uint32_t SUN_SPECIES_LIGHT_BYTES = 16;

FX_FN uint32_t SunLedgerWord(uint32_t band, uint32_t entry) {
    return (band * SUN_LEDGER_ENTRIES) + entry;
}

FX_FN uint32_t SunCellWord(uint32_t slot, uint32_t index, uint32_t band, uint32_t kind) {
    return SUN_LEDGER_WORDS + (((slot * MR_BLOCK_CELLS) + index) * SUN_CELL_WORDS) + (kind * LT_BANDS) + band;
}

// 1 刻みの掃き方(CPU が MakeSunParams で作る。GPU は作業のバッファの先頭から読む)。64bit の欄を先に置く
struct SunParams {
    // --- 世界の箱(光線の細かさ rayLevel のセルの座標。軸は A・B・C の順に並べ替えたもの)---
    int64_t lowA;
    int64_t lowB;
    int64_t lowC;
    int64_t sizeA;
    int64_t sizeB;
    int64_t sizeC;

    // --- 光線の格子(入口の面の上の番号。B・C の箱の低い端からのセルの数。横の面から入る光線のため負もある)---
    int64_t firstRayB;
    int64_t firstRayC;

    // --- 1 本の量(帯ごと。rayLevel のエネルギーの単位)---
    uint64_t energyShort;
    uint64_t energyLong;

    // --- 向き(A = 成分の絶対値が一番大きい軸。stepA > 0、signA は A に沿って進む向き)---
    uint32_t axisA;
    uint32_t axisB;
    uint32_t axisC;
    int32_t signA;
    int32_t stepA;
    int32_t stepB;
    int32_t stepC;
    uint32_t lengthQ15;  // |D| × 2^15(切り捨て)

    // --- 木と光線の数 ---
    int32_t rayLevel;
    int32_t rootLevel;
    uint32_t rayCountB;
    uint32_t rayCountC;
};

// 光線 1 本が持つ量(帯ごと)。強さ irradiance は光に垂直な面の mW/m²。A に垂直な面に落ちる割合 cos = |D_A| ÷ |D| を掛ける
FX_FN uint64_t SunEnergyPerRay(uint32_t irradianceMilliwatts, int32_t rayLevel, int32_t stepA, uint32_t lengthQ15) {
    FX_ASSERT(rayLevel >= 0 && rayLevel <= (int32_t)SUN_RAY_LEVEL_LIMIT && lengthQ15 != 0);
    const uint64_t scaled = (uint64_t)irradianceMilliwatts << (uint32_t)rayLevel;  // < 2^52
    const FxU128 numerator = FxMulU64Full(scaled, (uint64_t)stepA << SUN_LENGTH_SHIFT);
    const uint64_t divisor = (uint64_t)SUN_ENERGY_DIVISOR * (uint64_t)lengthQ15;
    FX_ASSERT(numerator.hi < divisor);

    return FxDivU128By64(numerator, divisor).quotient;
}

// 区間(同じ葉のセルを板 slabs 枚)の光学的な厚さ τ(Q32。RD_DEPTH_SATURATED_Q32 で飽和)
FX_FN uint64_t SunSegmentDepthQ32(FxU128 absorption, uint32_t slabs, SunParams params) {
    if (absorption.hi != 0)
        return RD_DEPTH_SATURATED_Q32;

    // レベル 0 の式の深さ(飽和させない。< 2^59)
    const uint64_t depth = FxShiftRightU128(FxMulU64Full(absorption.lo, RD_DEPTH_RECIPROCAL), RD_DEPTH_SHIFT);
    const FxU128 numerator = FxMulU64Full(depth, (uint64_t)slabs * (uint64_t)params.lengthQ15);
    const uint64_t divisor = (uint64_t)params.stepA << (SUN_LENGTH_SHIFT + (uint32_t)params.rayLevel);
    if (numerator.hi >= divisor)
        return RD_DEPTH_SATURATED_Q32;

    const uint64_t quotient = FxDivU128By64(numerator, divisor).quotient;

    return quotient < RD_DEPTH_SATURATED_Q32 ? quotient : RD_DEPTH_SATURATED_Q32;
}

// 通す割合 e^−τ(Q16。0〜65536。飽和していれば 0)
FX_FN uint32_t SunTransmittanceQ16(uint64_t depthQ32) {
    if (depthQ32 >= RD_DEPTH_SATURATED_Q32)
        return 0u;

    const uint64_t transmitted = FxExpQ32(-(int64_t)depthQ32, 16);

    return transmitted >= (uint64_t)LT_ONE_Q16 ? LT_ONE_Q16 : (uint32_t)transmitted;
}

// 1 区間を通した結果(帯 1 つ)
struct SunSegment {
    uint64_t passed;
    uint64_t absorbed;
    uint64_t reflected;
};

// 1 区間を通す(energy < 2^47: MakeSunParams が確かめる。× 2^16 が 64bit に収まる): 減った分 = 入った − 通した、返す = 減った × 照り返し、吸う = 減った − 返す。
// 吸い込みが 0 なら e^−0 = 1 で全部通すので、e^−τ を計算せずに同じ値を返す(T-0957)
template <typename Sink>
FX_FN SunSegment SunPassSegment(Sink sink, LtCellBand values, uint32_t slot, uint32_t index, uint32_t slabs,
                                uint32_t band, uint64_t energy, SunParams params) {
    SunSegment result;
    result.passed = energy;
    result.absorbed = 0;
    result.reflected = 0;
    if (energy == 0 || (values.absorption.hi == 0 && values.absorption.lo == 0))
        return result;

    const uint32_t transmittance = SunTransmittanceQ16(SunSegmentDepthQ32(values.absorption, slabs, params));
    result.passed = (energy * (uint64_t)transmittance) >> 16;

    const uint64_t removed = energy - result.passed;
    result.reflected = (removed * (uint64_t)values.reflectanceQ16) >> 16;
    result.absorbed = removed - result.reflected;
    if (removed != 0)
        sink.Deposit(slot, index, band, result.absorbed, result.reflected);

    return result;
}

// 光線の位置: 板 slab の真ん中で、B(か C)の軸のセルの番号(箱の低い端から)。
// 位置 = 光線の番号 + 0.5 + (slab + 0.5) × D_B ÷ D_A を 2 D_A 倍した整数で床へ(負も下へ)
FX_FN int64_t SunCellAcross(int64_t ray, int64_t slab, int32_t step, int32_t stepA) {
    const int64_t numerator = ((2 * ray + 1) * (int64_t)stepA) + ((2 * slab + 1) * (int64_t)step);
    const uint64_t denominator = 2 * (uint64_t)stepA;
    // 符号つきの割り算を避ける(分母は正。負の分子は −⌈−n ÷ d⌉)
    if (numerator >= 0)
        return (int64_t)((uint64_t)numerator / denominator);

    const uint64_t magnitude = (uint64_t)(-numerator);

    return -(int64_t)((magnitude + denominator - 1) / denominator);
}

// 横の軸 1 つ(B か C)の歩み: SunCellAcross と同じ番号を、割り算なしで板 1 枚ずつ進める(T-0957)。
// 分子 = cell × denominator + rest(0 ≤ rest < denominator)。板 1 枚で分子に 2 D_B を足す。|D_B| ≤ |D_A| なので桁の繰り上がりは 1 回まで
struct SunAcross {
    int64_t cell;
    int64_t rest;
    int64_t twiceStep;
    int64_t denominator;
};

FX_FN SunAcross SunStartAcross(int64_t ray, int32_t step, int32_t stepA) {
    SunAcross across;
    across.cell = SunCellAcross(ray, 0, step, stepA);
    across.twiceStep = 2 * (int64_t)step;
    across.denominator = 2 * (int64_t)stepA;
    const int64_t numerator = ((2 * ray + 1) * (int64_t)stepA) + (int64_t)step;
    across.rest = numerator - (across.cell * across.denominator);

    return across;
}

FX_FN SunAcross SunNextAcross(SunAcross across) {
    SunAcross next = across;
    next.rest += across.twiceStep;
    if (next.rest >= next.denominator) {
        next.rest -= next.denominator;
        next.cell += 1;
    } else if (next.rest < 0) {
        next.rest += next.denominator;
        next.cell -= 1;
    }

    return next;
}

// 世界の座標(rayLevel のセル)の軸 axis の値を、A・B・C の 3 つから選ぶ
FX_FN int64_t SunWorldAxis(uint32_t axis, SunParams params, int64_t a, int64_t b, int64_t c) {
    return axis == params.axisA ? a : (axis == params.axisB ? b : c);
}

// 葉のセル(見つからなければ slot = MR_NO_BLOCK = 箱の中でも木の無い所か、一様で光を吸わないブロック。どちらも素通し)
struct SunLeaf {
    uint32_t slot;
    uint32_t index;
};

// レベル level の座標の点を含む、level 以下で最も細かい本物のブロック(MrFindContaining と同じ結果。内側のループを return で抜けない)
template <typename Tree>
FX_FN uint32_t SunFindContaining(Tree tree, int32_t level, int64_t x, int64_t y, int64_t z, int32_t rootLevel) {
    uint32_t found = MR_NO_BLOCK;
    for (int32_t candidate = level; found == MR_NO_BLOCK && candidate >= rootLevel; --candidate) {
        const uint32_t shift = (uint32_t)(level - candidate);
        found = tree.Lookup(candidate, MrBlockOriginOf(x >> shift), MrBlockOriginOf(y >> shift),
                            MrBlockOriginOf(z >> shift));
    }

    return found;
}

// 光線が今いるブロック(T-0957: 板ごとに索引を引かず、同じブロックの中は座標から葉を出す)。
// 点がこのブロックの中で、その八分の一が本物の子に覆われていなければ、点を含む最も細かい本物のブロックはこのブロック
// (細かい本物のブロックは必ず親の children に載る)。一様で 2 つの帯とも吸わないブロックは丸ごと素通し(clear)
struct SunBlockView {
    int64_t lowX;  // ブロックの低い角(光線の細かさのセル)
    int64_t lowY;
    int64_t lowZ;
    int64_t extent;  // 8 << shift
    uint32_t slot;   // MR_NO_BLOCK = 持っていない
    uint32_t shift;  // rayLevel − ブロックのレベル
    uint32_t coveredMask;
    uint32_t clear;
};

template <typename Tree, typename Cells, typename Lights>
FX_FN SunBlockView SunViewBlock(Tree tree, Cells cells, Lights lights, SunParams params, uint32_t slot) {
    SunBlockView view;
    view.lowX = 0;
    view.lowY = 0;
    view.lowZ = 0;
    view.extent = 0;
    view.slot = slot;
    view.shift = 0;
    view.coveredMask = 0;
    view.clear = 0;
    if (slot == MR_NO_BLOCK)
        return view;

    const MrBlock block = tree.Block(slot);
    view.shift = (uint32_t)(params.rayLevel - block.level);
    view.lowX = block.originX << view.shift;
    view.lowY = block.originY << view.shift;
    view.lowZ = block.originZ << view.shift;
    view.extent = (int64_t)MR_BLOCK_EDGE << view.shift;

    // --- 覆われた八分の一と、一様なら値を見るセル(覆われていない最初の八分の一の最初のセル)---
    uint32_t firstOpen = MR_BLOCK_CELLS;
    FX_UNROLL
    for (uint32_t octant = 0; octant < 8; ++octant) {
        const bool covered = block.children[octant] != MR_NO_BLOCK;
        view.coveredMask |= covered ? (1u << octant) : 0u;
        firstOpen = (!covered && firstOpen == MR_BLOCK_CELLS) ? MrOctantCell(octant, 0) : firstOpen;
    }

    if (!MrIsUniform(block) || firstOpen == MR_BLOCK_CELLS)
        return view;

    const RxCell cell = cells.Cell(slot, firstOpen);
    const LtCellBand shortBand = LtCellBandOf(lights, cell, LT_BAND_SHORT);
    const LtCellBand longBand = LtCellBandOf(lights, cell, LT_BAND_LONG);
    const bool clearShort = shortBand.absorption.hi == 0 && shortBand.absorption.lo == 0;
    const bool clearLong = longBand.absorption.hi == 0 && longBand.absorption.lo == 0;
    view.clear = clearShort && clearLong ? 1u : 0u;

    return view;
}

// 点(光線の細かさの座標)がこのブロックの覆われていない所にあるか
FX_FN bool SunViewHolds(SunBlockView view, int64_t x, int64_t y, int64_t z) {
    if (view.slot == MR_NO_BLOCK)
        return false;

    const int64_t dx = x - view.lowX;
    const int64_t dy = y - view.lowY;
    const int64_t dz = z - view.lowZ;
    const bool inside = dx >= 0 && dx < view.extent && dy >= 0 && dy < view.extent && dz >= 0 && dz < view.extent;
    if (!inside)
        return false;

    const uint32_t octantShift = view.shift + 2;
    const uint32_t octant = (uint32_t)(dx >> octantShift) | ((uint32_t)(dy >> octantShift) << 1) |
                            ((uint32_t)(dz >> octantShift) << 2);

    return ((view.coveredMask >> octant) & 1u) == 0;
}

// ブロックの中の点の葉(素通しのブロックは MR_NO_BLOCK)
FX_FN SunLeaf SunLeafIn(SunBlockView view, int64_t x, int64_t y, int64_t z) {
    SunLeaf leaf;
    leaf.slot = view.clear != 0 ? MR_NO_BLOCK : view.slot;
    leaf.index = 0;
    if (leaf.slot == MR_NO_BLOCK)
        return leaf;

    leaf.index = MrCellIndex((uint32_t)((x - view.lowX) >> view.shift), (uint32_t)((y - view.lowY) >> view.shift),
                             (uint32_t)((z - view.lowZ) >> view.shift));

    return leaf;
}

// 光線 1 本の残りと帳簿(帯ごと。配列にしない: GPU でローカルの配列にしないため。T-0397)
struct SunRay {
    uint64_t energyShort;
    uint64_t energyLong;
    uint64_t enteredShort;
    uint64_t enteredLong;
    uint64_t absorbedShort;
    uint64_t absorbedLong;
    uint64_t reflectedShort;
    uint64_t reflectedLong;
    uint64_t escapedShort;
    uint64_t escapedLong;
    uint64_t caughtShort;  // 物に取られた(SunWalkRayCaught だけ。T-0878)
    uint64_t caughtLong;
};

FX_FN SunRay SunMakeRay(SunParams params) {
    SunRay ray;
    ray.energyShort = params.energyShort;
    ray.energyLong = params.energyLong;
    ray.enteredShort = 0;
    ray.enteredLong = 0;
    ray.absorbedShort = 0;
    ray.absorbedLong = 0;
    ray.reflectedShort = 0;
    ray.reflectedLong = 0;
    ray.escapedShort = 0;
    ray.escapedLong = 0;
    ray.caughtShort = 0;
    ray.caughtLong = 0;

    return ray;
}

// --- 物に取らせる口(T-0878・ADR-0425)---
// 光線 1 本が板 slab で物に取られる割合 count ÷ 2^shift(slab < 0 なら取られない)
struct SunCatch {
    int32_t slab;
    uint32_t count;
    uint32_t shift;
    uint32_t padding;
};

// 何も取らない(SunWalkRay)
struct SunNoCatch {
    uint32_t unused;

    SunCatch Catch(uint32_t rayB, uint32_t rayC) {
        RX_UNUSED(rayB);
        RX_UNUSED(rayC);
        SunCatch none;
        none.slab = -1;
        none.count = 0;
        none.shift = 0;
        none.padding = 0;

        return none;
    }
};

// 残す量 = energy × (2^shift − count) ÷ 2^shift(切り捨て)。取られた = energy − 残す(余りは取られた側へ。保存はビット単位)
FX_FN uint64_t SunCatchKept(uint64_t energy, SunCatch caught) {
    const uint64_t whole = (uint64_t)1 << caught.shift;

    return FxShiftRightU128(FxMulU64Full(energy, whole - (uint64_t)caught.count), caught.shift);
}

FX_FN SunRay SunTakeCatch(SunRay ray, SunCatch caught) {
    SunRay result = ray;
    const uint64_t keptShort = SunCatchKept(ray.energyShort, caught);
    const uint64_t keptLong = SunCatchKept(ray.energyLong, caught);
    result.caughtShort += ray.energyShort - keptShort;
    result.caughtLong += ray.energyLong - keptLong;
    result.energyShort = keptShort;
    result.energyLong = keptLong;

    return result;
}

// 2 つの帯を同じ区間で通す(木の無い所・素通しのブロック・空の区間は何もしない。セルは 1 回だけ読む)
template <typename Cells, typename Lights, typename Sink>
FX_FN SunRay SunPassBoth(Cells cells, Lights lights, Sink sink, SunLeaf leaf, uint32_t slabs, SunParams params,
                         SunRay ray) {
    if (leaf.slot == MR_NO_BLOCK || slabs == 0 || (ray.energyShort == 0 && ray.energyLong == 0))
        return ray;

    const RxCell cell = cells.Cell(leaf.slot, leaf.index);
    const LtCellBand shortValues = LtCellBandOf(lights, cell, LT_BAND_SHORT);
    const LtCellBand longValues = LtCellBandOf(lights, cell, LT_BAND_LONG);
    const SunSegment shortBand = SunPassSegment(sink, shortValues, leaf.slot, leaf.index, slabs, LT_BAND_SHORT,
                                                ray.energyShort, params);
    const SunSegment longBand = SunPassSegment(sink, longValues, leaf.slot, leaf.index, slabs, LT_BAND_LONG,
                                               ray.energyLong, params);

    SunRay result = ray;
    result.energyShort = shortBand.passed;
    result.energyLong = longBand.passed;
    result.absorbedShort += shortBand.absorbed;
    result.absorbedLong += longBand.absorbed;
    result.reflectedShort += shortBand.reflected;
    result.reflectedLong += longBand.reflected;

    return result;
}

// 光線 1 本(格子の番号 rayB・rayC。0 から rayCountB・rayCountC 未満)を走らせ、帳簿を返す
// (入った = 吸った + 返した + 外へ + 取られた。箱に入らなかった光線は全部 0)。取られる板では、手前の区間を通してから取る
template <typename Tree, typename Cells, typename Lights, typename Sink, typename Catcher>
FX_FN SunRay SunWalkRayCaught(Tree tree, Cells cells, Lights lights, Sink sink, Catcher catcher, SunParams params,
                              uint32_t rayB, uint32_t rayC) {
    const SunCatch caught = catcher.Catch(rayB, rayC);
    SunAcross acrossB = SunStartAcross(params.firstRayB + (int64_t)rayB, params.stepB, params.stepA);
    SunAcross acrossC = SunStartAcross(params.firstRayC + (int64_t)rayC, params.stepC, params.stepA);
    SunRay ray = SunMakeRay(params);
    bool entered = false;
    bool walking = true;
    SunBlockView view = SunViewBlock(tree, cells, lights, params, MR_NO_BLOCK);
    SunLeaf current;
    current.slot = MR_NO_BLOCK;
    current.index = 0;
    uint32_t slabs = 0;

    // --- 板ごとに: 箱の中なら葉を出し(同じブロックの中は座標から)、同じ葉が続く間は区間を伸ばす ---
    for (int64_t slab = 0; walking && slab < params.sizeA; ++slab) {
        const int64_t b = acrossB.cell;
        const int64_t c = acrossC.cell;
        acrossB = SunNextAcross(acrossB);
        acrossC = SunNextAcross(acrossC);
        const bool inside = b >= 0 && b < params.sizeB && c >= 0 && c < params.sizeC;
        if (!inside) {
            walking = !entered;  // 入った後に出たら終わり(箱は凸なので戻らない)
            continue;
        }

        if (!entered) {
            entered = true;
            ray.enteredShort = ray.energyShort;
            ray.enteredLong = ray.energyLong;
        }

        if (slab == (int64_t)caught.slab) {
            ray = SunPassBoth(cells, lights, sink, current, slabs, params, ray);
            current.slot = MR_NO_BLOCK;
            current.index = 0;
            slabs = 0;
            ray = SunTakeCatch(ray, caught);
        }

        const int64_t a = params.signA > 0 ? params.lowA + slab : params.lowA + params.sizeA - 1 - slab;
        const int64_t x = SunWorldAxis(0, params, a, params.lowB + b, params.lowC + c);
        const int64_t y = SunWorldAxis(1, params, a, params.lowB + b, params.lowC + c);
        const int64_t z = SunWorldAxis(2, params, a, params.lowB + b, params.lowC + c);
        if (!SunViewHolds(view, x, y, z))
            view = SunViewBlock(tree, cells, lights, params,
                                SunFindContaining(tree, params.rayLevel, x, y, z, params.rootLevel));

        const SunLeaf leaf = SunLeafIn(view, x, y, z);
        if (leaf.slot == current.slot && leaf.index == current.index) {
            slabs += 1;
            continue;
        }

        ray = SunPassBoth(cells, lights, sink, current, slabs, params, ray);
        current = leaf;
        slabs = 1;
        walking = ray.energyShort != 0 || ray.energyLong != 0;
    }

    // --- 最後の区間と、箱を出た分 ---
    ray = SunPassBoth(cells, lights, sink, current, slabs, params, ray);
    if (entered) {
        ray.escapedShort = ray.energyShort;
        ray.escapedLong = ray.energyLong;
    }

    return ray;
}

// 何も取らない版(R-R0 の掃き。T-0910)
template <typename Tree, typename Cells, typename Lights, typename Sink>
FX_FN SunRay SunWalkRay(Tree tree, Cells cells, Lights lights, Sink sink, SunParams params, uint32_t rayB,
                        uint32_t rayC) {
    SunNoCatch none;
    none.unused = 0;

    return SunWalkRayCaught(tree, cells, lights, sink, none, params, rayB, rayC);
}

MR_NAMESPACE_END

#endif  // BICAMERAL_SUN_COLUMN_HLSLI
