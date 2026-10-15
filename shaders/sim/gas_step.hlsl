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
#include "common/units.hlsli"

// 成分ごとの欄を持つもの(gas_gpu.hlsli。成分の数に上限なし。T-0598)は ByteAddressBuffer: セル・面・セルごとの合計は頭の並び + 成分ごとの面
// (件の数はセルの数・面の数 = セル × 6)、設定・外・帳簿は「頭 + 欄 × g_speciesStride」の件。回す成分の数は設定の speciesCount(≤ 欄の数)
RWByteAddressBuffer g_cells : register(u0);                  // 状態(小刻みの初め → 終わり)
RWByteAddressBuffer g_forced : register(u1);                 // 力を当てた後のセル
RWStructuredBuffer<GasGpuDerived> g_derived : register(u2);  // 小刻みの初めの導く値
RWByteAddressBuffer g_faces : register(u3);
RWByteAddressBuffer g_sums : register(u4);
RWByteAddressBuffer g_ledger : register(u5);
ByteAddressBuffer g_parameters : register(t0);                      // 設定の頭 + 成分ごとの値
StructuredBuffer<GasGpuDerived> g_referenceDerived : register(t1);  // 層ごとの基準の導く値(p̃ = 0)
ByteAddressBuffer g_ghost : register(t2);                           // 層ごとの開いた境界の外(基準 + 風。セルの件)

cbuffer RootConstants : register(b0) {
    uint32_t g_roundingTickLow;  // 小刻みの通し番号(乱数の丸め。R6)
    uint32_t g_roundingTickHigh;
    uint32_t g_cellCount;
    uint32_t g_speciesStride;  // 1 件の成分の欄の数(箱を作る時に決まる)
};

GasGpuParameters GasParameters() {
    return g_parameters.Load<GasGpuParameters>(0);
}

GasGpuSpecies GasSpeciesAt(uint32_t s) {
    return g_parameters.Load<GasGpuSpecies>(GAS_GPU_PARAMETERS_BYTES + (s * GAS_GPU_SPECIES_BYTES));
}

#define GAS_P GasParameters()
#define GAS_SPECIES(s) GasSpeciesAt(s)

#include "common/gas_cell.hlsli"

// --- 件の読み書き(頭 + 欄。gas_gpu.hlsli)---

static const uint32_t FACE_FLOW = 24;  // GasGpuFace の flow・fraction の場所
static const uint32_t FACE_FRACTION = 32;

uint32_t SpeciesCount() {
    return GAS_P.speciesCount;
}

uint32_t CellAddress(uint32_t index) {
    return index * GAS_GPU_CELL_BYTES;
}

uint32_t CellAmountAddress(uint32_t index, uint32_t s) {
    return GasGpuPlaneAddress(GAS_GPU_CELL_BYTES, g_cellCount, index, s);
}

uint32_t FaceAddress(uint32_t faceId) {
    return faceId * GAS_GPU_FACE_BYTES;
}

uint32_t FaceMovedAddress(uint32_t faceId, uint32_t s) {
    return GasGpuPlaneAddress(GAS_GPU_FACE_BYTES, g_cellCount * GAS_GPU_FACES_PER_CELL, faceId, s);
}

uint32_t SumsAddress(uint32_t index) {
    return index * GAS_GPU_SUMS_BYTES;
}

uint32_t OutgoingAddress(uint32_t index, uint32_t s) {
    return GasGpuPlaneAddress(GAS_GPU_SUMS_BYTES, g_cellCount, index, s);
}

GasGpuFace LoadFace(uint32_t faceId) {
    return g_faces.Load<GasGpuFace>(FaceAddress(faceId));
}

uint64_t FaceMoved(uint32_t faceId, uint32_t s) {
    return g_faces.Load<uint64_t>(FaceMovedAddress(faceId, s));
}

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

// HLSL の三項演算子は構造体を返せないので、側を選ぶ所はこれで
GasSide PickSide(bool condition, GasSide whenTrue, GasSide whenFalse) {
    if (condition)
        return whenTrue;

    return whenFalse;
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
        return g_ghost.Load<GasGpuCell>(side.index * GasGpuCellStride(g_speciesStride));

    return g_forced.Load<GasGpuCell>(CellAddress(side.index));
}

uint64_t ForcedSideAmount(GasSide side, uint32_t s) {
    if (side.kind == SIDE_GHOST)
        return g_ghost.Load<uint64_t>((side.index * GasGpuCellStride(g_speciesStride)) + GAS_GPU_CELL_BYTES + (s * 8));

    return g_forced.Load<uint64_t>(CellAmountAddress(side.index, s));
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
    return excess * (int64_t)GAS_P.hydrostatic / 20000;
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

// セルの 6 面(軸ごとに 自分が右の面・自分が左の面の順)と、それぞれで自分が風上か
struct CellFaceSet {
    uint32_t ids[6];
    bool right[6];  // 自分が面の右側
};

CellFaceSet CellFaceSetOf(uint32_t index) {
    CellFaceSet set;
    [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
        uint32_t lowFace;
        uint32_t highFace;
        CellFaces(index, axis, lowFace, highFace);
        set.ids[axis * 2] = lowFace;
        set.right[axis * 2] = true;
        set.ids[(axis * 2) + 1] = highFace;
        set.right[(axis * 2) + 1] = false;
    }

    return set;
}

// 自分が風上か(自分が右なら流れが負、左なら正)
bool IsDonor(bool right, int64_t flow) {
    return right ? flow < 0 : flow > 0;
}

// --- 段 1: 導く値(セル)---
[numthreads(64, 1, 1)] void GasDerive(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    GasGpuTotals totals = GasZeroTotals();
    const uint32_t speciesCount = SpeciesCount();
    for (uint32_t s = 0; s < speciesCount; ++s)
        GasAddSpecies(totals, s, g_cells.Load<uint64_t>(CellAmountAddress(index, s)));

    const GasGpuCell cell = g_cells.Load<GasGpuCell>(CellAddress(index));
    g_derived[index] = DeriveCellFromReference(cell, totals, g_referenceDerived[CellLayer(index)].pressure);
}

    // --- 段 2: 面の力(gas_reference.cpp の ComputeFaceWeight・ComputeFaceForce)。箱の外の側の力は帳簿へ ---
    [numthreads(64, 1, 1)] void GasFaceForce(uint32_t faceId : SV_DispatchThreadID) {
    GasFaceSides sides;
    if (!FaceSidesOf(faceId, sides))
        return;

    GasGpuFace face = LoadFace(faceId);
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

    g_faces.Store<GasGpuFace>(FaceAddress(faceId), face);

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

// --- 段 3: 力を当てたセル(自分が右の面の力積は足し、左の面の力積は引く。重さはどちらも引く)。成分はそのまま写す ---
[numthreads(64, 1, 1)] void GasApplyForce(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    GasGpuCell cell = g_cells.Load<GasGpuCell>(CellAddress(index));
    [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
        uint32_t lowFace;
        uint32_t highFace;
        CellFaces(index, axis, lowFace, highFace);

        const GasGpuFace low = LoadFace(lowFace);
        const GasGpuFace high = LoadFace(highFace);
        cell.momentum[axis] += low.impulse - high.impulse;
        cell.momentum[2] -= low.weightRight + high.weightLeft;
    }

    g_forced.Store<GasGpuCell>(CellAddress(index), cell);
    const uint32_t speciesCount = SpeciesCount();
    for (uint32_t s = 0; s < speciesCount; ++s) {
        const uint32_t address = CellAmountAddress(index, s);
        g_forced.Store<uint64_t>(address, g_cells.Load<uint64_t>(address));
    }
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

    g_faces.Store<int64_t>(FaceAddress(faceId) + FACE_FLOW, flow);
    g_faces.Store<uint64_t>(FaceAddress(faceId) + FACE_FRACTION, fraction);
}

// 出ていく割合の合計の、引く側の熱を負にしない換算(gas_reference.cpp の EffectiveOutflow。T-0646・ADR-0286):
// 割合の合計 × (C + n R) ÷ C(C = 熱容量。押し出す仕事 n R T のぶん)。等温はそのまま
// 熱容量と物質量は小刻みの初めのセル(g_cells)の成分から(CPU は box.cells)
uint64_t EffectiveOutflow(uint32_t index, uint64_t outflow) {
    if (outflow == 0 || GAS_P.thermodynamics != GAS_GPU_THERMODYNAMICS_ADIABATIC)
        return outflow;

    GasGpuTotals totals = GasZeroTotals();
    const uint32_t speciesCount = SpeciesCount();
    for (uint32_t s = 0; s < speciesCount; ++s)
        GasAddSpecies(totals, s, g_cells.Load<uint64_t>(CellAmountAddress(index, s)));

    const uint64_t capacity = totals.capacity;
    if (capacity == 0)
        return outflow;

    const FxDivResult push = FxDivU128By64(FxMulU64Full(totals.amount, GAS_CONSTANT_NANOJOULES_PER_MOLE_KELVIN), MICRO);
    const uint64_t pushCapacity = push.quotient + (push.remainder != 0 ? 1 : 0);  // n R(nJ/K。切り上げ)

    return FxDivU128By64(FxMulU64Full(outflow, capacity + pushCapacity), capacity).quotient;
}

// --- 段 5: 風上として出ていく割合の合計(セル。gas_reference.cpp の LimitOutflow の 1 回目の走査。EffectiveOutflow で換算)---
[numthreads(64, 1, 1)] void GasOutflow(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    const CellFaceSet faces = CellFaceSetOf(index);
    uint64_t outflow = 0;
    [unroll] for (uint32_t f = 0; f < 6; ++f) {
        const GasGpuFace face = LoadFace(faces.ids[f]);
        if (IsDonor(faces.right[f], face.flow))
            outflow += face.fraction;
    }

    g_sums.Store<uint64_t>(SumsAddress(index), EffectiveOutflow(index, outflow));
}

// --- 移す成分の物質量(gas_reference.cpp の LimitedSlope・SpeciesRatio・HalfSlope・MovedAmountQ32・RoundAmount)---

// MUSCL の 3 つ並び(風上の外側・風上・風下)と、それぞれの 1 ÷ 物質量(成分の割合を作る)。使えなければ勾配 0
struct MusclSides {
    bool usable;
    GasSide outer;
    GasSide donor;
    GasSide receiver;
    uint64_t outerInverse;
    uint64_t donorInverse;
    uint64_t receiverInverse;
};

MusclSides MusclSidesOf(GasFaceSides sides, int64_t flow) {
    const bool forward = flow > 0;
    MusclSides muscl;
    muscl.outer = PickSide(forward, sides.leftOuter, sides.rightOuter);
    muscl.donor = PickSide(forward, sides.left, sides.right);
    muscl.receiver = PickSide(forward, sides.right, sides.left);
    muscl.usable = GAS_P.reconstruction != GAS_GPU_RECONSTRUCTION_UPWIND && muscl.donor.kind == SIDE_CELL &&
                   muscl.outer.kind != SIDE_WALL && muscl.receiver.kind != SIDE_WALL;
    muscl.outerInverse = 0;
    muscl.donorInverse = 0;
    muscl.receiverInverse = 0;
    if (muscl.usable) {
        muscl.outerInverse = SideDerived(muscl.outer).inverseAmount;
        muscl.donorInverse = SideDerived(muscl.donor).inverseAmount;
        muscl.receiverInverse = SideDerived(muscl.receiver).inverseAmount;
    }

    return muscl;
}

int64_t SpeciesRatio(GasSide side, uint64_t inverseAmount, uint32_t s) {
    return (int64_t)MulShiftU64(ForcedSideAmount(side, s), inverseAmount, 32);
}

int64_t HalfSlope(MusclSides muscl, uint64_t fraction, uint32_t s) {
    if (!muscl.usable)
        return 0;

    const int64_t donorRatio = SpeciesRatio(muscl.donor, muscl.donorInverse, s);
    const int64_t outerRatio = SpeciesRatio(muscl.outer, muscl.outerInverse, s);
    const int64_t receiverRatio = SpeciesRatio(muscl.receiver, muscl.receiverInverse, s);
    const int64_t slope = LimitedSlope(donorRatio - outerRatio, receiverRatio - donorRatio);

    return FxMulShiftS64(slope, (int64_t)(ONE_Q32 - fraction), 33);
}

// 引く側は「0 で止める引き算」(a > b ? a − b : 0)の形を書かない: NVIDIA のドライバ(RTX 3070 Ti)が 64bit のこの形を誤って下ろし、
// 上位 32bit がでたらめな値になった(debug の DXIL。WARP は一致。T-0124 の飽和する足し算と同じ類い。T-0185)。
// 符号付きの差を出してから 0 以下を 0 にする(同じ値: moved < 2^63・correction < 2^63。物質量 < 2^31 と割合 ≤ 2^32 から)。
// sourceTotal = 風上の物質量の合計
uint64_t MovedAmountQ32(MusclSides muscl, uint64_t sourceAmount, uint64_t sourceTotal, uint64_t fraction, uint32_t s) {
    const uint64_t moved = sourceAmount * fraction;
    if (GAS_P.reconstruction == GAS_GPU_RECONSTRUCTION_UPWIND)
        return moved;

    const int64_t halfSlope = HalfSlope(muscl, fraction, s);
    const uint64_t total = sourceTotal * fraction;
    const uint64_t correction = MulShiftU64(total, FxAbsU64(halfSlope), 32);
    if (halfSlope >= 0)
        return moved + correction;

    const int64_t remaining = (int64_t)moved - (int64_t)correction;
    if (remaining <= 0)
        return 0;

    return (uint64_t)remaining;
}

// --- 段 6: 割合を抑え、移す成分の物質量(面。gas_reference.cpp の LimitOutflow の 2 回目・ComputeMovedAmounts)---
[numthreads(64, 1, 1)] void GasFaceMoved(uint32_t faceId : SV_DispatchThreadID) {
    GasFaceSides sides;
    if (!FaceSidesOf(faceId, sides))
        return;

    const GasGpuFace face = LoadFace(faceId);
    const GasSide donor = PickSide(face.flow > 0, sides.left, sides.right);
    uint64_t fraction = face.fraction;
    if (fraction != 0 && donor.kind == SIDE_CELL) {
        const uint64_t outflow = g_sums.Load<uint64_t>(SumsAddress(donor.index));
        if (outflow > ONE_Q32)
            fraction = FxDivU128By64(FxMulU64Full(fraction, ONE_Q32), outflow).quotient;
    }

    g_faces.Store<uint64_t>(FaceAddress(faceId) + FACE_FRACTION, fraction);
    const uint32_t speciesCount = SpeciesCount();
    if (fraction == 0) {
        for (uint32_t clear = 0; clear < speciesCount; ++clear)
            g_faces.Store<uint64_t>(FaceMovedAddress(faceId, clear), 0);

        return;
    }

    // --- 風上の物質量の合計(MUSCL の補正)---
    uint64_t sourceTotal = 0;
    if (GAS_P.reconstruction != GAS_GPU_RECONSTRUCTION_UPWIND) {
        for (uint32_t t = 0; t < speciesCount; ++t)
            sourceTotal += ForcedSideAmount(donor, t);
    }

    const MusclSides muscl = MusclSidesOf(sides, face.flow);
    const uint64_t roundingTick = FX_U64(g_roundingTickHigh, g_roundingTickLow);
    const uint64_t faceHash = FxHash64(GAS_P.randomSeed, roundingTick, faceId, GAS_ROUNDING_PURPOSE);
    for (uint32_t s = 0; s < speciesCount; ++s) {
        const uint64_t amountQ32 = MovedAmountQ32(muscl, ForcedSideAmount(donor, s), sourceTotal, fraction, s);
        const uint64_t moved = RoundAmount(amountQ32, FxHashCombine(faceHash, GAS_SPECIES(s).roundingKey));
        g_faces.Store<uint64_t>(FaceMovedAddress(faceId, s), moved);
    }
}

    // --- 段 7: 成分ごとの移す量の合計(セル。gas_reference.cpp の LimitMovedAmounts の 1 回目の走査)---
    [numthreads(64, 1, 1)] void GasMovedSum(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    // --- 自分が風上の面(移す割合が 0 でない)---
    const CellFaceSet faces = CellFaceSetOf(index);
    bool outgoing[6];
    [unroll] for (uint32_t f = 0; f < 6; ++f) {
        const GasGpuFace face = LoadFace(faces.ids[f]);
        outgoing[f] = face.fraction != 0 && IsDonor(faces.right[f], face.flow);
    }

    const uint32_t speciesCount = SpeciesCount();
    for (uint32_t s = 0; s < speciesCount; ++s) {
        uint64_t total = 0;
        [unroll] for (uint32_t g = 0; g < 6; ++g) {
            if (outgoing[g])
                total += FaceMoved(faces.ids[g], s);
        }

        g_sums.Store<uint64_t>(OutgoingAddress(index, s), total);
    }
}

// 重さの仕事(µJ。gas_reference.cpp の GravityWork): 縦の面を上へ動く塊が失う m g Δx(movedMass = 移す塊の質量)
int64_t GravityWork(uint32_t axis, int64_t flow, uint64_t movedMass) {
    if (axis != 2 || GAS_P.gravity == 0 || GAS_P.thermodynamics != GAS_GPU_THERMODYNAMICS_ADIABATIC)
        return 0;

    const int64_t work = (int64_t)(movedMass * GAS_P.gravityWork / MICRO);

    return flow > 0 ? work : -work;
}

// --- 段 8: 中身を超えた成分を抑え、移すエネルギーと運動量(面。gas_reference.cpp の LimitMovedAmounts の 2 回目・Transfer の前半)---
[numthreads(64, 1, 1)] void GasFaceFinal(uint32_t faceId : SV_DispatchThreadID) {
    GasFaceSides sides;
    if (!FaceSidesOf(faceId, sides))
        return;

    GasGpuFace face = LoadFace(faceId);
    if (face.fraction == 0)
        return;

    const bool forward = face.flow > 0;
    const GasSide donor = PickSide(forward, sides.left, sides.right);
    const GasSide receiver = PickSide(forward, sides.right, sides.left);
    const bool inside = donor.kind == SIDE_CELL && receiver.kind == SIDE_CELL;
    const int64_t sign = donor.kind == SIDE_CELL ? -1 : 1;

    // --- 成分(中身を超えたら抑える)。風上の中身と移す塊の和を作り、箱の外との出入りは帳簿へ ---
    GasGpuTotals sourceTotals = GasZeroTotals();
    GasGpuTotals movedTotals = GasZeroTotals();
    const uint32_t speciesCount = SpeciesCount();
    for (uint32_t s = 0; s < speciesCount; ++s) {
        const uint64_t content = ForcedSideAmount(donor, s);
        uint64_t moved = FaceMoved(faceId, s);
        if (donor.kind == SIDE_CELL) {
            const uint64_t total = g_sums.Load<uint64_t>(OutgoingAddress(donor.index, s));
            if (total > content) {
                moved = FxDivU128By64(FxMulU64Full(moved, content), total).quotient;
                g_faces.Store<uint64_t>(FaceMovedAddress(faceId, s), moved);
            }
        }

        GasAddSpecies(sourceTotals, s, content);
        GasAddSpecies(movedTotals, s, moved);
        if (!inside)
            LedgerAdd(GAS_GPU_LEDGER_AMOUNTS + (s * 8), sign * (int64_t)moved);
    }

    // --- 移す塊のエネルギーと運動量 ---
    const GasGpuCell source = ForcedSideCell(donor);
    const int64_t energy = MovedEnergy(source, sourceTotals, SideDerived(donor), movedTotals, face.fraction);
    face.movedEnergy = DivideNearest(energy, 1000);
    face.receivedEnergy = DivideNearest(energy - GravityWork(sides.axis, face.flow, movedTotals.mass), 1000);
    [unroll] for (uint32_t axis = 0; axis < 3; ++axis)
        face.movedMomentum[axis] = FxMulShiftS64(source.momentum[axis], (int64_t)face.fraction, 32);

    g_faces.Store<GasGpuFace>(FaceAddress(faceId), face);

    // --- 帳簿(開いた境界の外から入る物は正、出る物は負。エネルギーは重さの仕事〔受けた − 引いた〕も)---
    if (inside) {
        LedgerAdd(GAS_GPU_LEDGER_ENERGY, face.receivedEnergy - face.movedEnergy);
        return;
    }

    LedgerAdd(GAS_GPU_LEDGER_ENERGY, donor.kind == SIDE_CELL ? -face.movedEnergy : face.receivedEnergy);
    [unroll] for (uint32_t momentumAxis = 0; momentumAxis < 3; ++momentumAxis)
        LedgerAdd(GAS_GPU_LEDGER_MOMENTUM + (momentumAxis * 8), sign * face.movedMomentum[momentumAxis]);
}

    // --- 段 9: 移す(セル。gas_reference.cpp の Transfer)。自分の 6 面の出入りを集める(自分が風上なら引き、風下なら足す)---
    [numthreads(64, 1, 1)] void GasTransfer(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    const CellFaceSet faces = CellFaceSetOf(index);
    GasGpuCell cell = g_forced.Load<GasGpuCell>(CellAddress(index));
    bool active[6];
    bool donor[6];
    [unroll] for (uint32_t f = 0; f < 6; ++f) {
        const GasGpuFace face = LoadFace(faces.ids[f]);
        active[f] = face.fraction != 0;
        donor[f] = IsDonor(faces.right[f], face.flow);
        if (!active[f])
            continue;

        const int64_t sign = donor[f] ? -1 : 1;
        cell.energy += donor[f] ? -face.movedEnergy : face.receivedEnergy;
        [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
            cell.momentum[axis] += sign * face.movedMomentum[axis];
        }
    }

    g_cells.Store<GasGpuCell>(CellAddress(index), cell);

    // --- 成分 ---
    const uint32_t speciesCount = SpeciesCount();
    for (uint32_t s = 0; s < speciesCount; ++s) {
        uint64_t amount = g_forced.Load<uint64_t>(CellAmountAddress(index, s));
        [unroll] for (uint32_t g = 0; g < 6; ++g) {
            if (!active[g])
                continue;

            const uint64_t moved = FaceMoved(faces.ids[g], s);
            FX_ASSERT(!donor[g] || amount >= moved);
            amount += (uint64_t)((donor[g] ? -1 : 1) * (int64_t)moved);
        }

        g_cells.Store<uint64_t>(CellAmountAddress(index, s), amount);
    }
}
