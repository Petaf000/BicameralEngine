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

// --- 決まった値(32bit を超える値は FX_U64 で書く。HLSL の数の書き方は 32bit に切れる)---

FX_CONST uint32_t OH_MAX_GASES = 8;  // 対の面で混ざる気体の数の上限(表の気体。最小の形。超える表は T-1076)
FX_CONST uint32_t OH_NO_GAS = 0xFFFFFFFFu;
FX_CONST int64_t OH_CELL_UM = 500000;                       // レベル 0 の一辺(µm)
FX_CONST uint64_t OH_BOX_MM3 = FX_U64(0x1Du, 0x1A94A200u);  // 0.125 m³(そのレベルの単位の量 = 濃さ × 0.125 m³)
FX_CONST uint64_t OH_ROOM_TEMPERATURE_MK = 293150;          // 物質の値(混ざる速さ・伝導率)を書いた温度(D-639)
// mW/(m·K) × µm × mK = 10^-12 W・1/60 s → mJ: ÷ (10^6 × 60000)
FX_CONST uint64_t OH_CONDUCTION_DIVISOR = FX_U64(0xDu, 0xF8475800u);  // 6 × 10^10
// mm²/s × µm × (単位 ÷ 0.125 m³)× 1/60 s → 単位 × 8 ÷ (60 × 10^12)
FX_CONST uint64_t OH_DIFFUSION_DIVISOR = FX_U64(0x3691u, 0xD6AFC000u);  // 6 × 10^13
FX_CONST uint32_t OH_RADIATION_SHARE_Q16 = 32768;                       // 放射の 1 小刻みの上限(熱容量 × ΔT の半分)

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
    uint32_t padding0;
    uint32_t padding1;
    uint32_t padding2;
};

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

FX_FN OhFilmScale OhFilmScaleOf(int32_t voxelMilliKelvin, int32_t airMilliKelvin) {
    const uint64_t film = ((uint64_t)voxelMilliKelvin + (uint64_t)airMilliKelvin) / 2;
    const uint64_t ratioQ16 = (film << 16) / OH_ROOM_TEMPERATURE_MK;
    const uint64_t rootQ16 = (uint64_t)FxSqrtU64(ratioQ16 << 16);
    OhFilmScale scale;
    scale.diffusionQ16 = (ratioQ16 * rootQ16) >> 16;
    scale.conductionQ16 = rootQ16;

    return scale;
}

// 外に面する面の「広さ ÷ 膜の厚さ」(µm)= 面の数 × s² ÷ 膜
FX_FN int64_t OhPairLengthUm(OhObject object, uint32_t exposedFaces) {
    const int64_t facesUm2 = ((int64_t)exposedFaces * OH_CELL_UM * OH_CELL_UM) >> (2 * object.bodyLevel);

    return facesUm2 / (int64_t)object.filmUm;
}

// --- ① 下ごしらえ: 体積で重み付けた箱の伝導率(D-637。固体・液体の体積 = 量 × モル体積、残りは気体〔気体どうしは物質量の平均〕)---

template <typename Table>
FX_FN uint64_t OhVolumeConductivity(Table table, RxCell cell) {
    uint64_t condensedMm3 = 0;
    uint64_t condensedWeighted = 0;
    uint64_t gasAmount = 0;
    uint64_t gasWeighted = 0;
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        const uint32_t species = RX_SPECIES_AT(cell, i);
        const uint64_t amount = RX_AMOUNT_AT(cell, i);
        const uint64_t conductivity = (uint64_t)table.Species(species).conductivity;
        const OhSpecies values = table.ObjectSpecies(species);
        if (values.gasIndex != OH_NO_GAS) {
            gasAmount += amount;
            gasWeighted += amount * conductivity;
            continue;
        }

        const uint64_t volume = OhDivideU64(amount * (uint64_t)values.molarVolumeMm3PerMol, 1000000);
        condensedMm3 += volume;
        condensedWeighted += volume * conductivity;
    }

    const uint64_t gasConductivity = gasAmount == 0 ? (uint64_t)0 : OhDivideU64(gasWeighted, gasAmount);
    const uint64_t gasMm3 = OH_BOX_MM3 - (condensedMm3 < OH_BOX_MM3 ? condensedMm3 : OH_BOX_MM3);

    return OhDivideU64(condensedWeighted + (gasMm3 * gasConductivity), OH_BOX_MM3);
}

template <typename Table>
FX_FN OhVoxelHeat OhPrepareVoxel(Table table, RxCell cell) {
    const RxThermal thermal = RxComputeThermal(table, cell);
    OhVoxelHeat heat;
    heat.temperature = thermal.temperature;
    heat.padding = 0;
    heat.heatCapacity = thermal.heatCapacity;
    heat.innerConductivity = OhVolumeConductivity(table, cell);

    return heat;
}

// --- ② 対の面の口(ボクセル → セルが正。ボクセルの単位)---

// 伝導: 囲むセルの気体の伝導率 × (面の広さ ÷ 膜)× ΔT × Δt
FX_FN int64_t OhPairConduction(OhObject object, uint64_t airConductivity, int64_t pairLengthUm, OhVoxelHeat voxel,
                               int32_t airMilliKelvin, OhFilmScale film) {
    const int64_t difference = (int64_t)voxel.temperature - (int64_t)airMilliKelvin;
    const uint64_t multiplier = (airConductivity * (uint64_t)pairLengthUm * film.conductionQ16) >> 16;

    return OhMulDivToward0(difference * ((int64_t)1 << (3 * object.bodyLevel)), multiplier,
                           OH_CONDUCTION_DIVISOR * (uint64_t)object.substeps);
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
                                        OH_DIFFUSION_DIVISOR * (uint64_t)object.substeps);
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
                           OH_CONDUCTION_DIVISOR * (uint64_t)object.substeps);
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
    uint32_t ok;  // 0 = 量が負になる・成分の枠があふれる(試作の誤り。R8)
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
