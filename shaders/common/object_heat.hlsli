// object_heat.hlsli — 小さい物が自分の熱を持つ(物の箱のボクセルの熱と反応・セルとの対の面の 4 つの口)の共有の式(T-1064・ADR-0434・20 §12.4)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。CPU リファレンス(tests/object_heat_cpu.h)と GPU(shaders/sim/object_heat.hlsl)が
// 同じ関数を呼び、ビット一致する(tests/gpu_object_heat_test.cpp)。式は T-0991 の CPU の試作(tests/object_heat_test.cpp)と同じで、
// 試作の 1 枚の紙を「たくさんの物・物の形は箱の中の並び + ボクセルごとの外に面する数」に広げたもの(試作とのビット一致も object_heat_test が確かめる)。
//
// データの流れ(1 刻み。対の面は小刻み 4^(L−6) 回。どれも小刻みの初めの値から計算する = Jacobi):
//   ① 下ごしらえ(ボクセルごと): ボクセルの温度・熱容量・物の中の伝導率(体積の平行。D-637)→ 作業場
//   ② 流れ(ボクセルごと): 囲むセル(読むだけ)との対の面の口(伝導・気体の混ざり・放射。D-639 の膜の温度の形)と光、
//      隣のボクセルとの伝導 → 自分のボクセルに当て、セルへ出す分をボクセルの記録に書く
//   ③ 物ごとの和と対の端数(物ごと): ボクセルの記録を足し、粗い側(セル)の 1 単位に満たない分を対の端数に持ち越す → 物ごとの粗い量
//   ④ セルへ当てる(物の入っているセルごと): そのセルの物の粗い量を足してセルへ
//   ⑤ 反応(ボクセルごと): セルと同じ関数(RxStepCellWait)
// 保存: Σ セル × 8^(L−Lc) + Σ ボクセル + Σ 対の端数 − 帳簿(入った光)= 一定(エネルギーと元素。ビット単位)。
// 単位: ボクセルの量とエネルギーはそのレベルの単位(8^−L µmol・8^−L mJ)。量の数は濃さ × 0.125 m³ なので、反応と温度の式はセルと同じ。
#ifndef BICAMERAL_OBJECT_HEAT_HLSLI
#define BICAMERAL_OBJECT_HEAT_HLSLI

#include "heat_conduction.hlsli"
#include "radiation.hlsli"
#include "reaction.hlsli"

RX_NAMESPACE_BEGIN

// --- 決まった値(32bit を超える定数を式に書かない。32bit の定数の積と割り算で書く。T-1078)---

FX_CONST uint32_t OH_MAX_GASES = 8;  // 対の面で混ざる気体の数の上限(表の気体。最小の形。超える表は T-1076)
FX_CONST uint32_t OH_NO_GAS = 0xFFFFFFFFu;
FX_CONST int64_t OH_CELL_UM = 500000;  // レベル 0 の一辺(µm)
FX_CONST uint64_t OH_MEGA = 1000000;
FX_CONST uint64_t OH_BOX_MEGA_MM3 = 125000;  // 0.125 m³ = 125000 × 10^6 mm³(そのレベルの単位の量 = 濃さ × 0.125 m³)
// mW/(m·K) × µm × mK = 10^-12 W・1/60 s → mJ: ÷ (10^6 × 60000)
FX_CONST uint64_t OH_CONDUCTION_DIVISOR_MEGA = 60000;  // × 10^6
// mm²/s × µm × (単位 ÷ 0.125 m³)× 1/60 s → 単位 × 8 ÷ (60 × 10^12)
FX_CONST uint64_t OH_DIFFUSION_DIVISOR_MEGA = 60000000;  // × 10^6
FX_CONST uint32_t OH_RADIATION_SHARE_Q16 = 32768;        // 放射の 1 小刻みの上限(熱容量 × ΔT の半分)

// ボクセルの形の語(OhVoxelInfo::shape)
FX_CONST uint32_t OH_NEIGHBOUR_MASK = 0x3Fu;  // ビット 0〜5 = 隣のボクセルがある(−x・+x・−y・+y・−z・+z)
FX_CONST uint32_t OH_EXPOSED_SHIFT = 8;       // ビット 8〜11 = 囲むセルに面する面の数(0〜6)
FX_CONST uint32_t OH_EXPOSED_MASK = 0xFu;

// --- 表(物質ごとの対の面の値。ベイクの値から作る。添字 = 物質 ID)---
struct OhSpecies {
    uint32_t gasIndex;                 // 気体なら対の面の気体の番号(0〜OH_MAX_GASES−1)、気体でなければ OH_NO_GAS
    uint32_t diffusivityMm2PerS;       // 混ざる速さ(室温の値。書いていなければ既定)
    uint32_t molarVolumeMm3PerMol;     // 固体・液体のモル体積(体積の平行の伝導率に使う)
    uint32_t absorptionLongMm2PerMol;  // 長い帯の吸い込み(放射の口)
};

// --- 物(物の見出し。並びは囲むセルの順)---
struct OhObject {
    // --- ボクセルの並び ---
    uint32_t voxelBegin;  // 全部の物のボクセルの並びの中の始まり
    uint32_t voxelCount;  // = sizeX × sizeY × sizeZ
    uint32_t sizeX;
    uint32_t sizeY;

    // --- 細かさと囲むセル ---
    uint32_t cell;      // 囲むセルの番号
    int32_t bodyLevel;  // 物の箱のレベル(7.8 mm = 6)
    int32_t cellLevel;  // 囲むセルのレベル
    uint32_t substeps;  // 対の面の小刻み(4^(L − 6)。T-1016)

    // --- 膜 ---
    uint32_t filmUm;  // 境の膜の厚さ(物の形と流れで決まる物理の長さ。0 は使わない)

    // --- 物の中のブロック(64 ボクセル以下。眠らせる単位。T-1098)---
    uint32_t blockBegin;  // 全部の物のブロックの並びの中の始まり
    uint32_t blocksX;
    uint32_t blocksY;
    uint32_t blocksZ;
    uint32_t blockEdgeX;  // ブロックの一辺のボクセルの数(物の形で選ぶ。積は 64 以下)
    uint32_t blockEdgeY;
    uint32_t blockEdgeZ;
    uint32_t padding;
};

// --- 物の中のブロック(眠らせる単位。T-1098・ADR-0459)---
// 刻みの中の受け渡し(小刻みごと)と反応を、それぞれ通し番号の「段」(pass。1 から)で数える。
// ブロックは、最後に評価した時に静か(どのボクセルも変わらず、どの流れも 0)で、それから自分・隣のブロック・囲むセル・光のどれも
// 変わっていなければ、受け渡しを回しても厳密に何も起きない(流れは段の初めの値だけの関数なので同じ 0 になる)ので回さない。
// 反応は、ボクセルが変わった刻み(changedTick)と待ちの丸めの起こす刻み(wakeTick)で同じく決まる(ADR-0018)。
FX_CONST uint32_t OH_BLOCK_VOXELS = 64;
FX_CONST uint32_t OH_NO_BLOCK = 0xFFFFFFFFu;
FX_CONST uint32_t OH_BLOCK_EXPOSED = 1u;  // 囲むセルに面するボクセルがある
FX_CONST uint32_t OH_BLOCK_LIT = 2u;      // 光を吸うボクセルがある

struct OhBlock {
    uint32_t object;
    uint32_t x;  // ブロックの座標(物の箱の中。ボクセルは × 4)
    uint32_t y;
    uint32_t z;
};

struct OhBlockState {
    // --- 段の番号 ---
    uint32_t changedPass;   // どれかのボクセルが最後に変わった段
    uint32_t exchangePass;  // 受け渡しを最後に評価した段
    uint32_t reactPass;     // 反応を最後に評価した段
    uint32_t quiet;         // 最後の受け渡しの評価で静かだった(1)

    // --- 反応・この段 ---
    uint64_t minWakeTick;  // ボクセルの起こす刻みの最小
    uint32_t awake;        // この段で回す(OhWakeBlocks が書く)
    uint32_t flags;        // OH_BLOCK_*(変わらない)

    // --- 面ごと(方向 0〜5)に、その面に並ぶボクセルが最後に変わった段。隣のブロックはこれだけを読む ---
    uint32_t facePass[6];
    uint32_t padding0;
    uint32_t padding1;
};

// ボクセルの待ち(反応の待ちの丸め)
struct OhVoxelWait {
    uint64_t changedTick;  // 最後に変わった刻み(受け渡しで変わった刻み t は t − 1、反応で変わった刻み t は t)
    uint64_t wakeTick;     // 変わらなければ次に反応を評価する刻み
};

// ブロックの中の番号(0〜63)のボクセルの番号(箱の外なら OH_NO_BLOCK)
FX_FN uint32_t OhBlockVoxel(OhObject object, OhBlock block, uint32_t local) {
    const uint32_t edgeXY = object.blockEdgeX * object.blockEdgeY;
    const uint32_t localZ = local / edgeXY;
    const uint32_t x = (block.x * object.blockEdgeX) + (local % object.blockEdgeX);
    const uint32_t y = (block.y * object.blockEdgeY) + ((local / object.blockEdgeX) % object.blockEdgeY);
    const uint32_t z = (block.z * object.blockEdgeZ) + localZ;
    const uint32_t sizeZ = object.voxelCount / (object.sizeX * object.sizeY);
    if (localZ >= object.blockEdgeZ || x >= object.sizeX || y >= object.sizeY || z >= sizeZ)
        return OH_NO_BLOCK;

    return object.voxelBegin + x + (object.sizeX * (y + (object.sizeY * z)));
}

// ブロックの中の番号のボクセルが並ぶブロックの面(ビット 0〜5 = −x・+x・−y・+y・−z・+z)
FX_FN uint32_t OhVoxelFaces(OhObject object, uint32_t local) {
    const uint32_t x = local % object.blockEdgeX;
    const uint32_t y = (local / object.blockEdgeX) % object.blockEdgeY;
    const uint32_t z = local / (object.blockEdgeX * object.blockEdgeY);

    return (x == 0 ? 1u : 0u) | (x + 1 == object.blockEdgeX ? 2u : 0u) | (y == 0 ? 4u : 0u) |
           (y + 1 == object.blockEdgeY ? 8u : 0u) | (z == 0 ? 16u : 0u) | (z + 1 == object.blockEdgeZ ? 32u : 0u);
}

// 隣のブロックの番号(方向 0〜5 = −x・+x・−y・+y・−z・+z。物の外なら OH_NO_BLOCK)
FX_FN uint32_t OhNeighbourBlock(OhObject object, OhBlock block, uint32_t direction) {
    int32_t x = (int32_t)block.x;
    int32_t y = (int32_t)block.y;
    int32_t z = (int32_t)block.z;
    const int32_t step = (direction & 1) != 0 ? 1 : -1;
    x += direction < 2 ? step : 0;
    y += (direction == 2 || direction == 3) ? step : 0;
    z += direction >= 4 ? step : 0;
    const bool outside = x < 0 || y < 0 || z < 0 || x >= (int32_t)object.blocksX || y >= (int32_t)object.blocksY ||
                         z >= (int32_t)object.blocksZ;
    if (outside)
        return OH_NO_BLOCK;

    return object.blockBegin + (uint32_t)x + (object.blocksX * ((uint32_t)y + (object.blocksY * (uint32_t)z)));
}

// この受け渡しの段で回すか。newestChange = 自分の changedPass と、隣のブロックの自分に向いた面の facePass の最大。
// 隣・セルが自分を評価したのと同じ段で変わった時(自分が読んだ後に変わった)も起こすので ≥ で比べる
FX_FN bool OhExchangeAwake(OhBlockState state, uint32_t newestChange, uint32_t cellChangedPass, bool lightOn) {
    const bool cellChanged = (state.flags & OH_BLOCK_EXPOSED) != 0 && cellChangedPass >= state.exchangePass;
    const bool lit = lightOn && (state.flags & OH_BLOCK_LIT) != 0;

    return state.quiet == 0 || newestChange >= state.exchangePass || cellChanged || lit;
}

// この刻みに反応を評価するか(最後の評価から変わった・起こす刻みが来た)
FX_FN bool OhReactAwake(OhBlockState state, uint64_t tick) {
    return state.changedPass > state.reactPass || state.minWakeTick <= tick;
}

// ボクセルの変わらない値
struct OhVoxelInfo {
    uint32_t object;  // 物の番号
    uint32_t shape;   // OH_NEIGHBOUR_MASK と外に面する面の数
};

// ① の作業場(ボクセルごと)
struct OhVoxelHeat {
    int32_t temperature;  // mK
    uint32_t padding;
    uint64_t heatCapacity;       // nJ/K
    uint64_t innerConductivity;  // mW/(m·K)(体積で重み付けた平行。D-637)
};

// ② の記録・③ の和と端数(ボクセル → セルが正。ボクセルの単位)
struct OhFlow {
    int64_t energy;
    int64_t amounts[OH_MAX_GASES];  // 対の面の気体の番号ごと
};

// セルへ当てる物の範囲(④。物の入っているセルごと)
struct OhCellRange {
    uint32_t cell;
    uint32_t objectBegin;
    uint32_t objectCount;
    uint32_t padding;
};

// --- 整数の道具 ---

// value × multiplier ÷ divisor(符号つき。128bit の積で桁あふれしない。0 の向きに切る = 向きを逆にすると値も逆)
FX_FN int64_t OhMulDivToward0(int64_t value, uint64_t multiplier, uint64_t divisor) {
    const uint64_t magnitude = FxAbsU64(value);
    const int64_t result = (int64_t)FxDivU128By64(FxMulU64Full(magnitude, multiplier), divisor).quotient;

    return value < 0 ? -result : result;
}

// 64bit ÷ 64bit(FxDivU128By64 で。GPU で定数の 64bit の割り算〔÷ 0.125 m³ など〕が CPU と食い違ったので、この式では整数の / を使わない。T-1064)
FX_FN uint64_t OhDivideU64(uint64_t numerator, uint64_t divisor) {
    FxU128 wide;
    wide.hi = 0;
    wide.lo = numerator;

    return FxDivU128By64(wide, divisor).quotient;
}

FX_FN OhFlow OhZeroFlow() {
    OhFlow flow;
    flow.energy = 0;
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g)
        flow.amounts[g] = 0;

    return flow;
}

FX_FN bool OhFlowIsZero(OhFlow flow) {
    bool zero = flow.energy == 0;
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g)
        zero = zero && flow.amounts[g] == 0;

    return zero;
}

template <typename Cell>
FX_FN uint64_t OhAmountOf(Cell cell, uint32_t species) {
    const uint32_t slot = RxFindSlot(cell, species);

    return slot == RX_NO_SLOT ? (uint64_t)0 : RX_AMOUNT_AT(cell, slot);
}

// 物質の量を足し引きした結果(ok = 0 なら負になる・成分の枠があふれる。cell は変えない)
struct OhAmountChange {
    RxCell cell;
    uint32_t ok;
};

FX_FN OhAmountChange OhAddAmount(RxCell cell, uint32_t species, int64_t delta) {
    OhAmountChange change;
    change.cell = cell;
    change.ok = 1;
    if (delta == 0)
        return change;

    const uint32_t slot = RxFindSlot(cell, species);
    if (slot == RX_NO_SLOT) {
        if (delta < 0) {
            change.ok = 0;
            return change;
        }

        const RxAdded added = RxTryAddSpecies(cell, species, (uint64_t)delta);
        change.cell = added.cell;
        change.ok = added.overflowed == 0 ? 1u : 0u;

        return change;
    }

    const int64_t now = (int64_t)RX_AMOUNT_AT(cell, slot) + delta;
    if (now < 0) {
        change.ok = 0;
        return change;
    }

    // now ≥ 0 を確かめたので、符号なしの足し算(負の delta は 2^64 の剰余)で同じ値になる
    RxCell changed = cell;
    RX_ADD_AMOUNT(changed, slot, (uint64_t)delta);
    change.cell = RxCompactCell(changed);

    return change;
}

// --- 膜の気体の値(D-639: 膜の温度〔両側の平均〕で、剛体球の気体の運動論の形。混ざる速さ ∝ T^1.5・伝導率 ∝ T^0.5)---

struct OhFilmScale {
    uint64_t diffusionQ16;
    uint64_t conductionQ16;
};

// 膜の温度の気体の値はセルと同じ式(HcGasScaleAt。室温 HC_ROOM_TEMPERATURE_MK。T-1066)
FX_FN OhFilmScale OhFilmScaleOf(int32_t voxelMilliKelvin, int32_t airMilliKelvin) {
    const uint64_t film = ((uint64_t)voxelMilliKelvin + (uint64_t)airMilliKelvin) / 2;
    const HcGasScale gas = HcGasScaleAt((uint32_t)film);
    OhFilmScale scale;
    scale.diffusionQ16 = (uint64_t)gas.diffusionQ16;
    scale.conductionQ16 = (uint64_t)gas.conductionQ16;

    return scale;
}

// 外に面する面の「広さ ÷ 膜の厚さ」(µm)= 面の数 × s² ÷ 膜
FX_FN int64_t OhPairLengthUm(OhObject object, uint32_t exposedFaces) {
    const int64_t facesUm2 = ((int64_t)exposedFaces * OH_CELL_UM * OH_CELL_UM) >> (2 * object.bodyLevel);

    return facesUm2 / (int64_t)object.filmUm;
}

// --- ① 下ごしらえ: 箱の伝導率はセルと同じ式(HcCellConductivity。体積で重み付けた平行の道・気体は ∝ √T。D-637・D-639・T-1066)---

template <typename Table>
FX_FN OhVoxelHeat OhPrepareVoxel(Table table, RxCell cell) {
    const RxThermal thermal = RxComputeThermal(table, cell);
    OhVoxelHeat heat;
    heat.temperature = thermal.temperature;
    heat.padding = 0;
    heat.heatCapacity = thermal.heatCapacity;
    heat.innerConductivity = HcCellConductivity(table, cell, HcCacheTemperature(thermal));

    return heat;
}

// --- ② 対の面の口(ボクセル → セルが正。ボクセルの単位)---

// 伝導: 囲むセルの気体の伝導率 × (面の広さ ÷ 膜)× ΔT × Δt
FX_FN int64_t OhPairConduction(OhObject object, uint64_t airConductivity, int64_t pairLengthUm, OhVoxelHeat voxel,
                               int32_t airMilliKelvin, OhFilmScale film) {
    const int64_t difference = (int64_t)voxel.temperature - (int64_t)airMilliKelvin;
    const uint64_t multiplier = (airConductivity * (uint64_t)pairLengthUm * film.conductionQ16) >> 16;

    return OhMulDivToward0(difference * ((int64_t)1 << (3 * object.bodyLevel)), multiplier,
                           (uint64_t)object.substeps * OH_CONDUCTION_DIVISOR_MEGA * OH_MEGA);
}

// 放射: ボクセルの吸い込み(長い帯)× 囲む所(吸い込み 1)。RdPairCoefficient は 0.25 m² の面の値なので、
// 外に面する面の広さ(面の数 × s²)とボクセルの単位(× 8^L)にする: × 面の数 × 2^L
template <typename Table>
FX_FN int64_t OhPairRadiation(Table table, OhObject object, uint32_t exposedFaces, RxCell cell, OhVoxelHeat voxel,
                              int32_t airMilliKelvin) {
    FxU128 sum;
    sum.hi = 0;
    sum.lo = 0;
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        const uint32_t absorption = table.ObjectSpecies(RX_SPECIES_AT(cell, i)).absorptionLongMm2PerMol;
        sum = RdAddAbsorption(sum, absorption, RX_AMOUNT_AT(cell, i));
    }

    // 面の広さ s² の吸い込み: τ = Σ a·n ÷ (0.25 m² × 2^L)(n はボクセルの単位)
    const uint32_t absorptance = RdAbsorptanceQ16(RdOpticalDepthQ32(sum) >> object.bodyLevel);
    const uint64_t weight = RdPairWeightQ32(absorptance, RD_ONE_Q16, RD_ONE_Q16);
    const uint64_t coefficient = (RdPairCoefficient((uint32_t)voxel.temperature, (uint32_t)airMilliKelvin, weight) *
                                  (uint64_t)exposedFaces)
                                 << object.bodyLevel;
    const uint64_t limit = RdPairLimit(voxel.heatCapacity, voxel.heatCapacity, OH_RADIATION_SHARE_Q16);
    const uint64_t bounded = coefficient < limit ? coefficient : limit;

    return RdPairFlow((uint32_t)voxel.temperature, (uint32_t)airMilliKelvin, bounded / (uint64_t)object.substeps);
}

// 気体の混ざり: 気体ごとに 濃さの差 × 混ざる速さ × (面の広さ ÷ 膜)× Δt。運ぶ気体は出ていく側の温度の熱と化学のエネルギーを持っていく。
// flow に足して返す
template <typename Table, typename GasList>
FX_FN OhFlow OhPairGas(Table table, GasList gases, OhObject object, int64_t pairLengthUm, RxCell cell, RxCell air,
                       OhVoxelHeat voxel, int32_t airMilliKelvin, OhFilmScale film, OhFlow flow) {
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g) {
        const uint32_t species = gases.GasSpecies(g);
        if (species == 0)
            continue;

        // 濃さ × 0.125 m³ は、どのレベルでも単位の数そのもの(ボクセルの単位とセルの単位で比べられる)
        const uint64_t inVoxel = OhAmountOf(cell, species);
        const int64_t difference = (int64_t)inVoxel - (int64_t)OhAmountOf(air, species);
        const uint64_t diffusivity = (uint64_t)table.ObjectSpecies(species).diffusivityMm2PerS;
        const uint64_t multiplier = (diffusivity * (uint64_t)pairLengthUm * film.diffusionQ16) >> 16;
        int64_t moved = OhMulDivToward0(difference * ((int64_t)8 << (3 * object.bodyLevel)), multiplier,
                                        (uint64_t)object.substeps * OH_DIFFUSION_DIVISOR_MEGA * OH_MEGA);
        if (moved > (int64_t)inVoxel)
            moved = (int64_t)inVoxel;

        if (moved == 0)
            continue;

        // 運ぶエネルギー = 量 × (h0 + 比熱 × 出ていく側の温度)。µJ/µmol と nJ/(µmol·K) × mK → mJ は ÷ 10^9(積は 128bit。T-1016)
        const RxSpecies values = table.Species(species);
        const int64_t temperature = moved > 0 ? (int64_t)voxel.temperature : (int64_t)airMilliKelvin;
        const int64_t perUnit = (values.h0 * 1000000) + ((int64_t)values.heatCapacity * temperature);
        const int64_t carried = OhMulDivToward0(moved, FxAbsU64(perUnit), 1000000000);
        flow.amounts[g] += moved;
        flow.energy += perUnit < 0 ? -carried : carried;
    }

    return flow;
}

// 物の中の隣のボクセルどうしの伝導(面 s²・距離 s。伝導率は両側の小さい方)。自分 → 隣が正
FX_FN int64_t OhInnerConduction(OhObject object, OhVoxelHeat self, OhVoxelHeat other) {
    const uint64_t conductivity = self.innerConductivity < other.innerConductivity ? self.innerConductivity
                                                                                   : other.innerConductivity;
    const int64_t difference = (int64_t)self.temperature - (int64_t)other.temperature;
    const uint64_t multiplier = conductivity * (uint64_t)(OH_CELL_UM >> object.bodyLevel);

    return OhMulDivToward0(difference * ((int64_t)1 << (3 * object.bodyLevel)), multiplier,
                           (uint64_t)object.substeps * OH_CONDUCTION_DIVISOR_MEGA * OH_MEGA);
}

// 隣のボクセルの番号の差(方向 0〜5 = −x・+x・−y・+y・−z・+z)
FX_FN int32_t OhNeighbourOffset(OhObject object, uint32_t direction) {
    const int32_t stride = direction < 2
                               ? 1
                               : (direction < 4 ? (int32_t)object.sizeX : (int32_t)(object.sizeX * object.sizeY));

    return (direction & 1) != 0 ? stride : -stride;
}

// 光を小刻みに等しく分ける(端数は後ろの小刻みへ。全部の小刻みを足すと 1 刻みの量に戻る)
FX_FN int64_t OhLightShare(int64_t lightPerTick, uint32_t substep, uint32_t substeps) {
    const int64_t steps = (int64_t)substeps;

    return ((lightPerTick * ((int64_t)substep + 1)) / steps) - ((lightPerTick * (int64_t)substep) / steps);
}

// ② の結果: 当てた後のボクセルと、セルへ出す分
struct OhVoxelStep {
    RxCell cell;
    OhFlow toCell;
    uint32_t ok;         // 0 = 量が負になる・成分の枠があふれる(試作の誤り。R8)
    uint32_t flowsZero;  // 1 = 光もセルとの流れも 0(静かなブロックの見分け。T-1098)
};

// 外に面する口(伝導・放射・気体)をまとめて。inner = 物の中の伝導で入る熱、sunlight = この小刻みに吸う光
template <typename Table, typename GasList>
FX_FN OhVoxelStep OhApplyVoxel(Table table, GasList gases, OhObject object, uint32_t shape, RxCell cell,
                               OhVoxelHeat heat, RxCell air, RxThermal airThermal, uint64_t airConductivity,
                               int64_t inner, int64_t sunlight) {
    OhVoxelStep step;
    step.toCell = OhZeroFlow();
    const uint32_t exposed = (shape >> OH_EXPOSED_SHIFT) & OH_EXPOSED_MASK;
    if (exposed != 0) {
        const int64_t pairLength = OhPairLengthUm(object, exposed);
        const OhFilmScale film = OhFilmScaleOf(heat.temperature, airThermal.temperature);
        step.toCell.energy = OhPairConduction(object, airConductivity, pairLength, heat, airThermal.temperature, film) +
                             OhPairRadiation(table, object, exposed, cell, heat, airThermal.temperature);
        step.toCell = OhPairGas(table, gases, object, pairLength, cell, air, heat, airThermal.temperature, film,
                                step.toCell);
    }

    // 当てる(気体は表の気体の番号の順。足し引きの順は結果を変えない)
    step.ok = 1;
    cell.energy += sunlight + inner - step.toCell.energy;
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g) {
        const OhAmountChange change = OhAddAmount(cell, gases.GasSpecies(g), -step.toCell.amounts[g]);
        cell = change.cell;
        step.ok = step.ok & change.ok;
    }

    step.cell = cell;
    step.flowsZero = (sunlight == 0 && OhFlowIsZero(step.toCell)) ? 1u : 0u;

    return step;
}

// --- ③ 物ごとの和を粗い側の単位にし、1 単位に満たない分を対の端数に持ち越す ---

struct OhCarryStep {
    OhFlow carry;   // 新しい対の端数(ボクセルの単位)
    OhFlow coarse;  // セルへ渡す量(セルの単位)
};

FX_FN OhCarryStep OhSplitCarry(OhObject object, OhFlow total, OhFlow carry) {
    // 粗い側の 1 単位 = 細かい側の 8^(L − Lc) 単位。算術の右シフト = 負の側へ切る(floor)
    const uint32_t shift = (uint32_t)(3 * (object.bodyLevel - object.cellLevel));
    OhCarryStep step;
    const int64_t energy = total.energy + carry.energy;
    step.coarse.energy = energy >> shift;
    step.carry.energy = energy - (step.coarse.energy << shift);
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g) {
        const int64_t amount = total.amounts[g] + carry.amounts[g];
        step.coarse.amounts[g] = amount >> shift;
        step.carry.amounts[g] = amount - (step.coarse.amounts[g] << shift);
    }

    return step;
}

FX_FN OhFlow OhAddFlow(OhFlow a, OhFlow b) {
    a.energy += b.energy;
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g)
        a.amounts[g] += b.amounts[g];

    return a;
}

// --- ④ セルへ当てる(そのセルの物の粗い量の和)---

template <typename GasList>
FX_FN OhAmountChange OhApplyToCell(GasList gases, RxCell cell, OhFlow coarse) {
    OhAmountChange result;
    result.ok = 1;
    cell.energy += coarse.energy;
    FX_UNROLL
    for (uint32_t g = 0; g < OH_MAX_GASES; ++g) {
        const OhAmountChange change = OhAddAmount(cell, gases.GasSpecies(g), coarse.amounts[g]);
        cell = change.cell;
        result.ok = result.ok & change.ok;
    }

    result.cell = cell;

    return result;
}

RX_NAMESPACE_END

#endif  // BICAMERAL_OBJECT_HEAT_HLSLI
