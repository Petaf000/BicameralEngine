// gas_step.hlsl — 気体の流れの 1 小刻みを GPU の Compute で(G3 の第 1 段。T-0185・07 §2.3)。1 レベルの箱だけ。
// CPU リファレンスは engine/src/sim/gas_reference.cpp(G1・G2)で、各段はその関数を同じ式で写したもの。毎刻みビット一致を
// tests/gpu_gas_test.cpp が確かめる。呼ぶ順は engine/src/sim/gpu_gas.cpp(段の間は UAV バリア)。
//
// 1 小刻み = 9 段。面の段は面の番号(セル × 6 + 軸 × 2 + 端の境界の面)ごとに 1 スレッド、セルの段はセルごとに 1 スレッド。
//   GasDerive(セル): 導く値 → GasFaceForce(面): 力積と重さ → GasApplyForce(セル): 自分の面の力を集めて「力の後」のセル
//   → GasFaceFlow(面): 質量の流れと抑える前の割合 → GasOutflow(セル): 風上として出ていく割合の合計
//   → GasFaceMoved(面): 割合を抑え、移す成分の物質量(MUSCL・乱数の丸め)→ GasMovedSum(セル): 成分ごとの移す量の合計
//   → GasFaceFinal(面): 中身を超えた成分を抑え、移すエネルギーと運動量 → GasTransfer(セル): 自分の面の出入りを集めて次のセル。
// CPU は面ごとに両側へ足し引きする(散らす形)が、GPU はセルが自分の 6 面を集める(集める形)。足し引きは整数なので順番に依存せず、
// 同じ値になる(04 R2・R3)。箱の外との出入り(壁の力積・重さ・開いた境界の物)は帳簿へ 64bit の原子的な足し算で(整数なので順番に依存しない)。
#include "common/gas_gpu.hlsli"

RWStructuredBuffer<GasGpuCell> g_cells : register(u0);       // 状態(小刻みの初め → 終わり)
RWStructuredBuffer<GasGpuCell> g_forced : register(u1);      // 力を当てた後のセル
RWStructuredBuffer<GasGpuDerived> g_derived : register(u2);  // 小刻みの初めの導く値
RWStructuredBuffer<GasGpuFace> g_faces : register(u3);
RWStructuredBuffer<GasGpuCellSums> g_sums : register(u4);
RWByteAddressBuffer g_ledger : register(u5);
StructuredBuffer<GasGpuParameters> g_parameters : register(t0);
StructuredBuffer<GasGpuDerived> g_referenceDerived : register(t1);  // 層ごとの基準の導く値(p̃ = 0)
StructuredBuffer<GasGpuCell> g_ghost : register(t2);                // 層ごとの開いた境界の外(基準 + 風)

cbuffer RootConstants : register(b0) {
    uint32_t g_roundingTickLow;  // 小刻みの通し番号(乱数の丸め。R6)
    uint32_t g_roundingTickHigh;
    uint32_t g_cellCount;
    uint32_t g_unused;
};

#define GAS_P g_parameters[0]

// --- 物理の定数(gas_reference.cpp と同じ)---
static const uint64_t PRESSURE_PER_AMOUNT_TEMPERATURE_Q32 = 285682760;
static const int64_t HYDROSTATIC_MICROPASCAL_PER_MASS_X10000 = 392266;
static const uint64_t MICRO = 1000000;
static const uint64_t ONE_Q32 = FX_U64(1u, 0u);
static const uint64_t MAX_U64 = FX_U64(0xFFFFFFFFu, 0xFFFFFFFFu);
static const uint64_t MAX_CELL_AMOUNT = FX_U64(0u, 0x80000000u);
static const uint32_t GAS_ROUNDING_PURPOSE = 0x47610001u;

// --- 面の片側(箱の中のセル・開いた境界の外・壁)---
static const uint32_t SIDE_CELL = 0;
static const uint32_t SIDE_GHOST = 1;
static const uint32_t SIDE_WALL = 2;

struct GasSide {
    uint32_t kind;
    uint32_t index;  // セル: セルの番号 / 外: 層 z
};

struct GasFaceSides {
    GasSide left;
    GasSide right;
    GasSide leftOuter;
    GasSide rightOuter;
    uint32_t axis;
};

GasSide MakeSide(uint32_t kind, uint32_t index) {
    GasSide side;
    side.kind = kind;
    side.index = index;

    return side;
}

uint64_t MulShiftU64(uint64_t a, uint64_t b, uint32_t shift) {
    return FxShiftRightU128(FxMulU64Full(a, b), shift);
}

// HLSL の三項演算子は構造体を返せないので、側を選ぶ所はこれで
GasSide PickSide(bool condition, GasSide whenTrue, GasSide whenFalse) {
    if (condition)
        return whenTrue;

    return whenFalse;
}

int64_t MinS64(int64_t a, int64_t b) {
    return a < b ? a : b;
}

// --- セルから導く値(gas_reference.cpp の DeriveRaw・DeriveGasCell)---

uint64_t TotalAmount(GasGpuCell cell) {
    uint64_t total = 0;
    [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s) {
        total += cell.amounts[s];
    }

    return total;
}

uint64_t HeatCapacity(GasGpuCell cell) {
    uint64_t capacity = 0;
    [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s)
        capacity += cell.amounts[s] * (uint64_t)GAS_P.heatCapacity[s];

    return capacity;
}

int64_t ChemicalEnergy(GasGpuCell cell) {
    int64_t chemical = 0;
    [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s)
        chemical += (int64_t)cell.amounts[s] * GAS_P.formationEnergy[s];

    return chemical;
}

uint64_t CellMass(GasGpuCell cell) {
    uint64_t mass = 0;
    [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s)
        mass += MulShiftU64(cell.amounts[s], GAS_P.massPerAmount[s], 32);

    return mass;
}

uint32_t CellTemperature(GasGpuCell cell) {
    const int64_t heat = cell.energy * 1000 - ChemicalEnergy(cell);
    const uint64_t capacity = HeatCapacity(cell);
    if (heat <= 0 || capacity == 0)
        return 0;

    const uint64_t temperature = FxDivU128By64(FxMulU64Full((uint64_t)heat, MICRO), capacity).quotient;
    FX_ASSERT(temperature <= 0xFFFFFFFFu);

    return (uint32_t)temperature;
}

GasGpuDerived DeriveCell(GasGpuCell cell, uint32_t z) {
    const uint64_t amount = TotalAmount(cell);
    FX_ASSERT(amount < MAX_CELL_AMOUNT);

    GasGpuDerived derived;
    derived.mass = CellMass(cell);
    derived.inverseMass = derived.mass == 0 ? 0 : MAX_U64 / derived.mass;
    derived.inverseAmount = amount == 0 ? 0 : MAX_U64 / amount;
    derived.temperature = CellTemperature(cell);
    derived.unused = 0;

    // --- 本物の圧力 p = n × T × R ÷ V ---
    const FxU128 amountTemperature = FxMulU64Full(amount, derived.temperature);
    FX_ASSERT(amountTemperature.hi == 0);
    derived.pressure = (int64_t)MulShiftU64(amountTemperature.lo, PRESSURE_PER_AMOUNT_TEMPERATURE_Q32, 32);

    // --- p̃ = α(p − p_基準)。基準より熱いセルは α × T_基準 ÷ T ---
    const int64_t deviation = derived.pressure - g_referenceDerived[z].pressure;
    int64_t scaled = FxMulShiftS64(deviation, (int64_t)GAS_P.soundScale, 32);
    if (derived.temperature > GAS_P.soundTemperature)
        scaled = FxDivS64(FxMulShiftS64(scaled, (int64_t)GAS_P.soundTemperature, 0), (int64_t)derived.temperature);

    derived.scaledPressure = scaled;

    return derived;
}

// --- 箱の形と面の両側(gas_reference.cpp の SideAt・MakeFace)---

uint32_t CellIndex(uint32_t x, uint32_t y, uint32_t z) {
    return x + GAS_P.size[0] * (y + GAS_P.size[1] * z);
}

uint32_t CellLayer(uint32_t index) {
    return index / (GAS_P.size[0] * GAS_P.size[1]);
}

// セルの座標(x, y, z)。配列にして軸の番号で引く
void CellCoordinate(uint32_t index, out uint32_t coordinate[3]) {
    const uint32_t sizeX = GAS_P.size[0];
    const uint32_t sizeY = GAS_P.size[1];
    coordinate[0] = index % sizeX;
    coordinate[1] = (index / sizeX) % sizeY;
    coordinate[2] = index / (sizeX * sizeY);
}

// 座標の軸 axis を position に替えたセルの番号
uint32_t ShiftedIndex(uint32_t cell[3], uint32_t axis, uint32_t position) {
    uint32_t shifted[3];
    shifted[0] = cell[0];
    shifted[1] = cell[1];
    shifted[2] = cell[2];
    shifted[axis] = position;

    return CellIndex(shifted[0], shifted[1], shifted[2]);
}

// セルから軸 axis の座標を position に替えた所の側。周期的なら反対側へ回し、それ以外の外は境界
GasSide SideAt(uint32_t cell[3], uint32_t axis, int position) {
    const int size = (int)GAS_P.size[axis];
    const uint32_t boundary = GAS_P.boundary[axis];
    if (position < 0 || position >= size) {
        if (boundary == GAS_GPU_BOUNDARY_OPEN)
            return MakeSide(SIDE_GHOST, cell[2]);

        if (boundary != GAS_GPU_BOUNDARY_PERIODIC)
            return MakeSide(SIDE_WALL, 0);

        position = ((position % size) + size) % size;
    }

    return MakeSide(SIDE_CELL, ShiftedIndex(cell, axis, (uint32_t)position));
}

// 面の番号 → 面の両側。端の境界の面のうち、無いもの(内側のセルの端の面・周期的な軸の端の面)は false
bool FaceSidesOf(uint32_t faceId, out GasFaceSides sides) {
    const uint32_t cellIndex = faceId / GAS_GPU_FACES_PER_CELL;
    const uint32_t axis = (faceId / 2) % 3;
    const bool high = (faceId & 1) != 0;
    sides.axis = axis;
    sides.left = MakeSide(SIDE_WALL, 0);
    sides.right = sides.left;
    sides.leftOuter = sides.left;
    sides.rightOuter = sides.left;
    if (cellIndex >= g_cellCount)
        return false;

    uint32_t cell[3];
    CellCoordinate(cellIndex, cell);
    const bool lastCell = cell[axis] + 1 == GAS_P.size[axis];
    if (high && !(lastCell && GAS_P.boundary[axis] != GAS_GPU_BOUNDARY_PERIODIC))
        return false;

    const int left = high ? (int)cell[axis] : (int)cell[axis] - 1;
    sides.leftOuter = SideAt(cell, axis, left - 1);
    sides.left = SideAt(cell, axis, left);
    sides.right = SideAt(cell, axis, left + 1);
    sides.rightOuter = SideAt(cell, axis, left + 2);

    return true;
}

// セルの軸 axis の 2 面: lowFace = 自分が右側の面(自分のマイナス側)、highFace = 自分が左側の面
// (プラス側の隣のマイナス側の面。周期的なら回った先の。箱の端なら自分の端の境界の面)
void CellFaces(uint32_t cellIndex, uint32_t axis, out uint32_t lowFace, out uint32_t highFace) {
    uint32_t cell[3];
    CellCoordinate(cellIndex, cell);
    lowFace = ((cellIndex * 3) + axis) * 2;
    if (cell[axis] + 1 < GAS_P.size[axis]) {
        highFace = ((ShiftedIndex(cell, axis, cell[axis] + 1) * 3) + axis) * 2;
        return;
    }

    if (GAS_P.boundary[axis] == GAS_GPU_BOUNDARY_PERIODIC) {
        highFace = ((ShiftedIndex(cell, axis, 0) * 3) + axis) * 2;
        return;
    }

    highFace = lowFace + 1;
}

// --- 側の値 ---

GasGpuCell ForcedSideCell(GasSide side) {
    if (side.kind == SIDE_GHOST)
        return g_ghost[side.index];

    return g_forced[side.index];
}

GasGpuDerived SideDerived(GasSide side) {
    if (side.kind == SIDE_GHOST)
        return g_referenceDerived[side.index];

    return g_derived[side.index];
}

int64_t Excess(GasSide side) {
    if (side.kind != SIDE_CELL)
        return 0;

    return (int64_t)g_derived[side.index].mass - (int64_t)g_referenceDerived[CellLayer(side.index)].mass;
}

int64_t HalfHydrostatic(int64_t excess) {
    return excess * HYDROSTATIC_MICROPASCAL_PER_MASS_X10000 / 20000;
}

int64_t DrivingPressure(GasFaceSides sides) {
    const int64_t difference = SideDerived(sides.left).scaledPressure - SideDerived(sides.right).scaledPressure;
    if (sides.axis != 2 || GAS_P.gravity == 0)
        return difference;

    return difference - HalfHydrostatic(Excess(sides.left) + Excess(sides.right));
}

void LedgerAdd(uint32_t offset, int64_t value) {
    if (value == 0)
        return;

    uint64_t before;
    g_ledger.InterlockedAdd64(offset, (uint64_t)value, before);
}

// --- 段 1: 導く値(セル)---
[numthreads(64, 1, 1)] void GasDerive(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    g_derived[index] = DeriveCell(g_cells[index], CellLayer(index));
}

    // --- 段 2: 面の力(gas_reference.cpp の ComputeFaceWeight・ComputeFaceForce)。箱の外の側の力は帳簿へ ---
    [numthreads(64, 1, 1)] void GasFaceForce(uint32_t faceId : SV_DispatchThreadID) {
    GasFaceSides sides;
    if (!FaceSidesOf(faceId, sides))
        return;

    GasGpuFace face = g_faces[faceId];
    face.impulse = 0;
    face.weightLeft = 0;
    face.weightRight = 0;

    // --- 重さ(縦の面。両側の質量の差の平均 × g を 1/4 ずつ。壁の面は手前のセルだけ)---
    if (sides.axis == 2 && GAS_P.gravity != 0) {
        const int64_t coefficient = (int64_t)GAS_P.gravityImpulse;
        if (sides.left.kind == SIDE_WALL)
            face.weightRight = FxMulShiftS64(Excess(sides.right), coefficient, 17);
        else if (sides.right.kind == SIDE_WALL)
            face.weightLeft = FxMulShiftS64(Excess(sides.left), coefficient, 17);
        else {
            const int64_t quarter = FxMulShiftS64(Excess(sides.left) + Excess(sides.right), coefficient, 18);
            face.weightLeft = quarter;
            face.weightRight = quarter;
        }
    }

    // --- 圧力の力積(壁は手前のセルの p̃ を鏡に映す。縦の壁は半セル分の静水圧で延ばす)---
    const int64_t impulseCoefficient = (int64_t)GAS_P.pressureImpulse;
    if (sides.left.kind == SIDE_WALL || sides.right.kind == SIDE_WALL) {
        const bool wallBelow = sides.left.kind == SIDE_WALL;
        const GasSide inside = PickSide(wallBelow, sides.right, sides.left);
        int64_t pressure = g_derived[inside.index].scaledPressure;
        if (sides.axis == 2 && GAS_P.gravity != 0) {
            const int64_t hydrostatic = HalfHydrostatic(Excess(inside));
            pressure += wallBelow ? hydrostatic : -hydrostatic;
        }

        face.impulse = FxMulShiftS64(pressure * 2, impulseCoefficient, 16);
    } else {
        const int64_t pressureSum = SideDerived(sides.left).scaledPressure + SideDerived(sides.right).scaledPressure;
        face.impulse = FxMulShiftS64(pressureSum, impulseCoefficient, 16);
    }

    g_faces[faceId] = face;

    // --- 帳簿(gas_reference.cpp の ApplyImpulse の箱の外の側)---
    if (sides.left.kind == SIDE_CELL)
        LedgerAdd(GAS_GPU_LEDGER_MOMENTUM + 16, -face.weightLeft);

    if (sides.right.kind == SIDE_CELL)
        LedgerAdd(GAS_GPU_LEDGER_MOMENTUM + 16, -face.weightRight);

    if (sides.left.kind != SIDE_CELL)
        LedgerAdd(GAS_GPU_LEDGER_MOMENTUM + (sides.axis * 8), face.impulse);

    if (sides.right.kind != SIDE_CELL)
        LedgerAdd(GAS_GPU_LEDGER_MOMENTUM + (sides.axis * 8), -face.impulse);
}

// --- 段 3: 力を当てたセル(自分が右の面の力積は足し、左の面の力積は引く。重さはどちらも引く)---
[numthreads(64, 1, 1)] void GasApplyForce(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    GasGpuCell cell = g_cells[index];
    [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
        uint32_t lowFace;
        uint32_t highFace;
        CellFaces(index, axis, lowFace, highFace);

        const GasGpuFace low = g_faces[lowFace];
        const GasGpuFace high = g_faces[highFace];
        cell.momentum[axis] += low.impulse - high.impulse;
        cell.momentum[2] -= low.weightRight + high.weightLeft;
    }

    g_forced[index] = cell;
}

    // --- 段 4: 面の質量の流れと、抑える前の割合(gas_reference.cpp の ComputeFaceFlow・RawFraction)---
    [numthreads(64, 1, 1)] void GasFaceFlow(uint32_t faceId : SV_DispatchThreadID) {
    GasFaceSides sides;
    if (!FaceSidesOf(faceId, sides))
        return;

    int64_t flow = 0;
    if (sides.left.kind != SIDE_WALL && sides.right.kind != SIDE_WALL) {
        const int64_t momentumSum = ForcedSideCell(sides.left).momentum[sides.axis] +
                                    ForcedSideCell(sides.right).momentum[sides.axis];
        const int64_t central = FxMulShiftS64(momentumSum, (int64_t)GAS_P.centralFlow, 52);
        const int64_t diffusion = FxMulShiftS64(DrivingPressure(sides), (int64_t)GAS_P.pressureFlow, 40);
        flow = central + diffusion;
    }

    uint64_t fraction = 0;
    if (flow != 0) {
        const GasSide donor = PickSide(flow > 0, sides.left, sides.right);
        fraction = MulShiftU64(FxAbsU64(flow), SideDerived(donor).inverseMass, 32);
    }

    g_faces[faceId].flow = flow;
    g_faces[faceId].fraction = fraction;
}

// --- 段 5: 風上として出ていく割合の合計(セル。gas_reference.cpp の LimitOutflow の 1 回目の走査)---
[numthreads(64, 1, 1)] void GasOutflow(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    uint64_t outflow = 0;
    [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
        uint32_t lowFace;
        uint32_t highFace;
        CellFaces(index, axis, lowFace, highFace);

        const GasGpuFace low = g_faces[lowFace];  // 自分が右: 流れが負なら風上
        if (low.flow < 0)
            outflow += low.fraction;

        const GasGpuFace high = g_faces[highFace];  // 自分が左: 流れが正なら風上
        if (high.flow > 0)
            outflow += high.fraction;
    }

    g_sums[index].outflow = outflow;
}

// --- 移す成分の物質量(gas_reference.cpp の LimitedSlope・SpeciesRatio・HalfSlope・MovedAmountQ32・RoundAmount)---

int64_t LimitedSlope(int64_t a, int64_t b) {
    if (a == 0 || b == 0 || (a > 0) != (b > 0))
        return 0;

    const int64_t sign = a > 0 ? 1 : -1;
    const int64_t smaller = MinS64(a * sign, b * sign);
    if (GAS_P.reconstruction == GAS_GPU_RECONSTRUCTION_MINMOD)
        return smaller * sign;

    const int64_t central = ((a * sign) + (b * sign)) / 2;
    return MinS64(2 * smaller, central) * sign;
}

int64_t SpeciesRatio(GasSide side, uint32_t s) {
    return (int64_t)MulShiftU64(ForcedSideCell(side).amounts[s], SideDerived(side).inverseAmount, 32);
}

int64_t HalfSlope(GasFaceSides sides, int64_t flow, uint64_t fraction, uint32_t s) {
    const bool forward = flow > 0;
    const GasSide outer = PickSide(forward, sides.leftOuter, sides.rightOuter);
    const GasSide donor = PickSide(forward, sides.left, sides.right);
    const GasSide receiver = PickSide(forward, sides.right, sides.left);
    if (donor.kind != SIDE_CELL || outer.kind == SIDE_WALL || receiver.kind == SIDE_WALL)
        return 0;

    const int64_t donorRatio = SpeciesRatio(donor, s);
    const int64_t slope = LimitedSlope(donorRatio - SpeciesRatio(outer, s), SpeciesRatio(receiver, s) - donorRatio);

    return FxMulShiftS64(slope, (int64_t)(ONE_Q32 - fraction), 33);
}

// 引く側は「0 で止める引き算」(a > b ? a − b : 0)の形を書かない: NVIDIA のドライバ(RTX 3070 Ti)が 64bit のこの形を誤って下ろし、
// 上位 32bit がでたらめな値になった(debug の DXIL。WARP は一致。T-0124 の飽和する足し算と同じ類い。T-0185)。
// 符号付きの差を出してから 0 以下を 0 にする(同じ値: moved < 2^63・correction < 2^63。物質量 < 2^31 と割合 ≤ 2^32 から)
uint64_t MovedAmountQ32(GasFaceSides sides, GasGpuCell source, int64_t flow, uint64_t fraction, uint32_t s) {
    const uint64_t moved = source.amounts[s] * fraction;
    if (GAS_P.reconstruction == GAS_GPU_RECONSTRUCTION_UPWIND)
        return moved;

    const int64_t halfSlope = HalfSlope(sides, flow, fraction, s);
    const uint64_t total = TotalAmount(source) * fraction;
    const uint64_t correction = MulShiftU64(total, FxAbsU64(halfSlope), 32);
    if (halfSlope >= 0)
        return moved + correction;

    const int64_t remaining = (int64_t)moved - (int64_t)correction;
    if (remaining <= 0)
        return 0;

    return (uint64_t)remaining;
}

uint64_t RoundAmount(uint64_t amountQ32, uint64_t hash) {
    const uint64_t whole = amountQ32 >> 32;
    if (GAS_P.stochasticRounding == 0)
        return whole;

    return whole + ((hash >> 32) < (amountQ32 & FX_LOW32_MASK) ? 1 : 0);
}

// --- 段 6: 割合を抑え、移す成分の物質量(面。gas_reference.cpp の LimitOutflow の 2 回目・ComputeMovedAmounts)---
[numthreads(64, 1, 1)] void GasFaceMoved(uint32_t faceId : SV_DispatchThreadID) {
    GasFaceSides sides;
    if (!FaceSidesOf(faceId, sides))
        return;

    GasGpuFace face = g_faces[faceId];
    const GasSide donor = PickSide(face.flow > 0, sides.left, sides.right);
    if (face.fraction != 0 && donor.kind == SIDE_CELL) {
        const uint64_t outflow = g_sums[donor.index].outflow;
        if (outflow > ONE_Q32)
            face.fraction = FxDivU128By64(FxMulU64Full(face.fraction, ONE_Q32), outflow).quotient;
    }

    [unroll] for (uint32_t clear = 0; clear < GAS_GPU_MAX_SPECIES; ++clear) {
        face.moved[clear] = 0;
    }

    if (face.fraction != 0) {
        const GasGpuCell source = ForcedSideCell(donor);
        const uint64_t roundingTick = FX_U64(g_roundingTickHigh, g_roundingTickLow);
        const uint64_t faceHash = FxHash64(GAS_P.randomSeed, roundingTick, faceId, GAS_ROUNDING_PURPOSE);
        [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s) {
            const uint64_t amountQ32 = MovedAmountQ32(sides, source, face.flow, face.fraction, s);
            face.moved[s] = RoundAmount(amountQ32, FxHashCombine(faceHash, s));
        }
    }

    g_faces[faceId] = face;
}

    // --- 段 7: 成分ごとの移す量の合計(セル。gas_reference.cpp の LimitMovedAmounts の 1 回目の走査)---
    [numthreads(64, 1, 1)] void GasMovedSum(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    GasGpuCellSums sums = g_sums[index];
    [unroll] for (uint32_t clear = 0; clear < GAS_GPU_MAX_SPECIES; ++clear) {
        sums.outgoing[clear] = 0;
    }

    [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
        uint32_t lowFace;
        uint32_t highFace;
        CellFaces(index, axis, lowFace, highFace);

        const GasGpuFace low = g_faces[lowFace];
        const GasGpuFace high = g_faces[highFace];
        [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s) {
            if (low.fraction != 0 && low.flow < 0)
                sums.outgoing[s] += low.moved[s];

            if (high.fraction != 0 && high.flow > 0)
                sums.outgoing[s] += high.moved[s];
        }
    }

    g_sums[index] = sums;
}

// --- 移す塊のエネルギー(gas_reference.cpp の DivideNearest・MovedEnergy)---

int64_t DivideNearest(int64_t a, uint64_t b) {
    FxU128 product;
    product.hi = 0;
    product.lo = FxAbsU64(a);
    const FxDivResult result = FxDivU128By64(product, b);
    const uint64_t quotient = result.quotient + (result.remainder * 2 >= b ? 1 : 0);

    return FxApplySign(quotient, a < 0);
}

int64_t MovedEnergy(GasGpuCell source, GasGpuCell moved) {
    const uint64_t sourceCapacity = HeatCapacity(source);
    const int64_t movedChemical = ChemicalEnergy(moved);
    if (sourceCapacity == 0)
        return DivideNearest(movedChemical, 1000);

    const int64_t sourceHeat = source.energy * 1000 - ChemicalEnergy(source);
    const FxU128 numerator = FxMulU64Full(FxAbsU64(sourceHeat), HeatCapacity(moved));
    const FxDivResult share = FxDivU128By64(numerator, sourceCapacity);
    const uint64_t heat = share.quotient + (share.remainder * 2 >= sourceCapacity ? 1 : 0);
    const int64_t movedHeat = FxApplySign(heat, sourceHeat < 0);

    return DivideNearest(movedHeat + movedChemical, 1000);
}

// --- 段 8: 中身を超えた成分を抑え、移すエネルギーと運動量(面。gas_reference.cpp の LimitMovedAmounts の 2 回目・Transfer の前半)---
[numthreads(64, 1, 1)] void GasFaceFinal(uint32_t faceId : SV_DispatchThreadID) {
    GasFaceSides sides;
    if (!FaceSidesOf(faceId, sides))
        return;

    GasGpuFace face = g_faces[faceId];
    if (face.fraction == 0)
        return;

    const bool forward = face.flow > 0;
    const GasSide donor = PickSide(forward, sides.left, sides.right);
    const GasSide receiver = PickSide(forward, sides.right, sides.left);
    const GasGpuCell source = ForcedSideCell(donor);
    if (donor.kind == SIDE_CELL) {
        const GasGpuCellSums sums = g_sums[donor.index];
        [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s) {
            const uint64_t total = sums.outgoing[s];
            if (total > source.amounts[s])
                face.moved[s] = FxDivU128By64(FxMulU64Full(face.moved[s], source.amounts[s]), total).quotient;
        }
    }

    // --- 移す塊(成分・エネルギー・運動量)---
    GasGpuCell moved;
    [unroll] for (uint32_t copy = 0; copy < GAS_GPU_MAX_SPECIES; ++copy) {
        moved.amounts[copy] = face.moved[copy];
    }

    moved.energy = 0;
    moved.momentum[0] = 0;
    moved.momentum[1] = 0;
    moved.momentum[2] = 0;
    face.movedEnergy = MovedEnergy(source, moved);
    [unroll] for (uint32_t axis = 0; axis < 3; ++axis)
        face.movedMomentum[axis] = FxMulShiftS64(source.momentum[axis], (int64_t)face.fraction, 32);

    g_faces[faceId] = face;

    // --- 帳簿(開いた境界の外から入る物は正、出る物は負)---
    if (donor.kind == SIDE_CELL && receiver.kind == SIDE_CELL)
        return;

    const int64_t sign = donor.kind == SIDE_CELL ? -1 : 1;
    [unroll] for (uint32_t species = 0; species < GAS_GPU_MAX_SPECIES; ++species)
        LedgerAdd(GAS_GPU_LEDGER_AMOUNTS + (species * 8), sign * (int64_t)face.moved[species]);

    LedgerAdd(GAS_GPU_LEDGER_ENERGY, sign * face.movedEnergy);
    [unroll] for (uint32_t momentumAxis = 0; momentumAxis < 3; ++momentumAxis)
        LedgerAdd(GAS_GPU_LEDGER_MOMENTUM + (momentumAxis * 8), sign * face.movedMomentum[momentumAxis]);
}

// 面の出入りを 1 つ当てる(自分が風上なら引き、風下なら足す)
void ApplyMoved(inout GasGpuCell cell, GasGpuFace face, bool donor) {
    const int64_t sign = donor ? -1 : 1;
    [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s) {
        FX_ASSERT(!donor || cell.amounts[s] >= face.moved[s]);
        cell.amounts[s] += (uint64_t)(sign * (int64_t)face.moved[s]);
    }

    cell.energy += sign * face.movedEnergy;
    [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
        cell.momentum[axis] += sign * face.movedMomentum[axis];
    }
}

// --- 段 9: 移す(セル。gas_reference.cpp の Transfer)---
[numthreads(64, 1, 1)] void GasTransfer(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    GasGpuCell cell = g_forced[index];
    [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
        uint32_t lowFace;
        uint32_t highFace;
        CellFaces(index, axis, lowFace, highFace);

        const GasGpuFace low = g_faces[lowFace];  // 自分が右
        if (low.fraction != 0)
            ApplyMoved(cell, low, low.flow < 0);

        const GasGpuFace high = g_faces[highFace];  // 自分が左
        if (high.fraction != 0)
            ApplyMoved(cell, high, high.flow > 0);
    }

    g_cells[index] = cell;
}
