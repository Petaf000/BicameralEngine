// gas_step.hlsl — 気体の流れの 1 小刻みを GPU の Compute で(G3 の第 1 段。T-0185・07 §2.3)。1 レベルの箱だけ。
// CPU リファレンスは engine/src/sim/gas_reference.cpp(G1・G2)。式は両方が shaders/common/gas_flow.hlsli の同じ関数を呼ぶ(T-0719。
// GPU の欄の並びからの包みは gas_cell.hlsli)。ここにあるのは面とセルの回し方・欄の読み書き・帳簿だけ。毎刻みビット一致を
// tests/gpu_gas_test.cpp が確かめる(回し方・atomic の取りこぼしの検査)。呼ぶ順は engine/src/sim/gpu_gas.cpp(段の間は UAV バリア)。
//
// 1 小刻み = 9 段。面の段は面の番号(セル × 6 + 軸 × 2 + 端の境界の面)ごとに 1 スレッド、セルの段はセルごとに 1 スレッド。
//   GasDerive(セル): 導く値 → GasFaceForce(面): 力積と重さ → GasApplyForce(セル): 自分の面の力を集めて「力の後」のセル
//   → GasFaceFlow(面): 質量の流れと抑える前の割合 → GasOutflow(セル): 風上として出ていく割合の合計
//   → GasFaceMoved(面): 割合を抑え、移す成分の物質量(MUSCL・乱数の丸め)→ GasMovedSum(セル): 成分ごとの移す量の合計
//   → GasFaceFinal(面): 中身を超えた成分を抑え、移すエネルギーと運動量 → GasTransfer(セル): 自分の面の出入りを集めて次のセル。
// 1 刻みの小刻みの後に、粘り(T-0758・T-0775。粘りのある箱だけ)の 2 段: GasViscosity(セル): 自分の 6 面で隣と分け合う運動量と熱を集めて
//   g_forced の頭へ → GasViscosityApply(セル): 頭を g_cells へ写す(Jacobi = 刻みの値だけから決める。CPU の ApplyViscosity と同じ値)。
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
ByteAddressBuffer g_open : register(t3);  // セルの空き(uint32 Q16 × セル。設定の openness が 1 の時だけ読む。T-0617)

cbuffer RootConstants : register(b0) {
    uint32_t g_roundingTickLow;  // 小刻みの通し番号(乱数の丸め。R6)
    uint32_t g_roundingTickHigh;
    uint32_t g_cellCount;
    uint32_t g_speciesStride;  // 1 件の成分の欄の数(箱を作る時に決まる)
#ifdef GAS_STEP_EXTRA_CONSTANTS
    GAS_STEP_EXTRA_CONSTANTS  // 同じ形の段を足す入れ物(投影の段 gas_projection.hlsl。T-0845)
#endif
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

// 面の件は置き場の番号で並ぶ(gas_gpu.hlsli の GasGpuFaceSlot。T-0918)。面の番号は乱数の丸めの鍵と両側の決め方に使う
uint32_t FaceSlot(uint32_t faceId) {
    const GasGpuParameters parameters = GAS_P;

    return GasGpuFaceSlot(parameters.size[0], parameters.size[1], parameters.size[2], faceId);
}

uint32_t FaceSlotCount() {
    const GasGpuParameters parameters = GAS_P;

    return GasGpuFaceSlotCount(parameters.size[0], parameters.size[1], parameters.size[2]);
}

uint32_t FaceAddress(uint32_t faceId) {
    return FaceSlot(faceId) * GAS_GPU_FACE_BYTES;
}

uint32_t FaceMovedAddress(uint32_t faceId, uint32_t s) {
    return GasGpuPlaneAddress(GAS_GPU_FACE_BYTES, FaceSlotCount(), FaceSlot(faceId), s);
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

// --- セルの空き(gas_reference.cpp の SideOpen・FaceOpen。T-0593・T-0617)---

// セルの空き(Q16)。空きを読まない箱は全部
uint32_t CellOpen(uint32_t index) {
    if (GAS_P.openness == 0)
        return GAS_GPU_OPEN_FULL;

    return g_open.Load(index * 4);
}

// 面の片側の空き。箱の外(開いた境界の外・壁)は全部
uint32_t SideOpen(GasSide side) {
    if (side.kind != SIDE_CELL)
        return GAS_GPU_OPEN_FULL;

    return CellOpen(side.index);
}

// 面の開き = 両側の空きの小さい方
uint32_t FaceOpen(GasFaceSides sides) {
    return min(SideOpen(sides.left), SideOpen(sides.right));
}

// 基準との質量の差(mg。gas_flow.hlsli の GfExcess)。開いた境界の外・壁は 0
int64_t Excess(GasSide side) {
    if (side.kind != SIDE_CELL)
        return 0;

    return GfExcess(GasFlowParams(), g_derived[side.index].mass, g_referenceDerived[CellLayer(side.index)].mass,
                    CellOpen(side.index));
}

// 縦の面で重さが働く側の質量の差(横の面・重さの無い箱は 0。gas_reference.cpp の VerticalExcess)
int64_t VerticalExcess(GasFaceSides sides, GasSide side) {
    if (sides.axis != 2 || GAS_P.gravity == 0)
        return 0;

    return Excess(side);
}

// 圧力の拡散を起こす圧力の差(gas_flow.hlsli の GfDrivingPressure)
int64_t DrivingPressure(GasFaceSides sides) {
    return GfDrivingPressure(GasFlowParams(), sides.axis, SideDerived(sides.left).scaledPressure,
                             SideDerived(sides.right).scaledPressure, VerticalExcess(sides, sides.left),
                             VerticalExcess(sides, sides.right));
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

// 面の段のスレッドの面の番号(1 行に収まらない箱は 2 次元で投げる。gas_gpu.hlsli の GAS_GPU_FACE_ROW_GROUPS。T-0845)
uint32_t GasFaceThread(uint3 thread) {
    return thread.x + (thread.y * GAS_GPU_FACE_ROW_GROUPS * 64);
}

// --- 段 1: 導く値(セル)---
[numthreads(64, 1, 1)] void GasDerive(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    GasGpuTotals totals = GasZeroTotals();
    const uint32_t speciesCount = SpeciesCount();
    for (uint32_t s = 0; s < speciesCount; ++s)
        GasAddSpecies(totals, s, g_cells.Load<uint64_t>(CellAmountAddress(index, s)));

    // 空きが一部のセルは、本物の圧力を空きの体積で読む(gas_reference.cpp の DeriveOpenCell)
    const GasGpuCell cell = g_cells.Load<GasGpuCell>(CellAddress(index));
    const int64_t referencePressure = g_referenceDerived[CellLayer(index)].pressure;
    const uint32_t open = CellOpen(index);
    if (open == GAS_GPU_OPEN_FULL)
        g_derived[index] = DeriveCellFromReference(cell, totals, referencePressure);
    else
        g_derived[index] = DeriveOpenCellFromReference(cell, totals, referencePressure, open);
}

// 一部だけ開いた面の 1 つの側が受ける力積: 開いた所は両側の平均の圧力(fullImpulse = 全面の時の力積)、塞がった所は
// その側のセル自身の圧力(壁と同じ鏡。縦の面は半セルの静水圧で延ばす)。above = その側が面の上(+ 側)にある
// (gas_reference.cpp の PartialImpulse)
int64_t PartialImpulse(GasFaceSides sides, GasSide side, bool above, int64_t fullImpulse, uint32_t open) {
    return GfPartialImpulse(GasFlowParams(), sides.axis, SideDerived(side).scaledPressure, VerticalExcess(sides, side),
                            above, fullImpulse, open);
}

// 空きを読む箱の面の力積(gas_reference.cpp の ComputeFaceForce の後半): 一部だけ開いた面は両側が違う力積を受け、
// 気体の無いセルは押さない。face.impulse は全面の時の力積が入っている
void ApplyFaceOpenness(GasFaceSides sides, inout GasGpuFace face) {
    const bool wall = sides.left.kind == SIDE_WALL || sides.right.kind == SIDE_WALL;
    const uint32_t open = FaceOpen(sides);
    if (!wall && open != GAS_GPU_OPEN_FULL) {
        const int64_t fullImpulse = face.impulse;
        face.impulse = PartialImpulse(sides, sides.left, false, fullImpulse, open);
        face.impulseRight = PartialImpulse(sides, sides.right, true, fullImpulse, open);
    }

    if (sides.left.kind == SIDE_CELL && g_derived[sides.left.index].mass == 0)
        face.impulse = 0;

    if (sides.right.kind == SIDE_CELL && g_derived[sides.right.index].mass == 0)
        face.impulseRight = 0;
}

// --- 段 2: 面の力(gas_reference.cpp の ComputeFaceWeight・ComputeFaceForce)。箱の外の側の力は帳簿へ ---
[numthreads(64, 1, 1)] void GasFaceForce(uint3 thread : SV_DispatchThreadID) {
    const uint32_t faceId = GasFaceThread(thread);
    GasFaceSides sides;
    if (!FaceSidesOf(faceId, sides))
        return;

    const GfParams params = GasFlowParams();
    GasGpuFace face = LoadFace(faceId);

    // --- 重さ(縦の面。両側の質量の差の平均 × g を 1/4 ずつ。壁の面は手前のセルだけ。gas_flow.hlsli の GfFaceWeight)---
    const bool leftWall = sides.left.kind == SIDE_WALL;
    const bool rightWall = sides.right.kind == SIDE_WALL;
    const GfWeights weights = GfFaceWeight(params, sides.axis, leftWall, rightWall, VerticalExcess(sides, sides.left),
                                           VerticalExcess(sides, sides.right));
    face.weightLeft = weights.left;
    face.weightRight = weights.right;

    // --- 圧力の力積(壁は手前のセルの p̃ を鏡に映す。縦の壁は半セル分の静水圧で延ばす)---
    if (leftWall || rightWall) {
        const GasSide inside = PickSide(leftWall, sides.right, sides.left);
        face.impulse = GfMirrorImpulse(params, sides.axis, g_derived[inside.index].scaledPressure,
                                       VerticalExcess(sides, inside), leftWall);
    } else {
        face.impulse = GfSharedImpulse(params, SideDerived(sides.left).scaledPressure,
                                       SideDerived(sides.right).scaledPressure);
    }

    face.impulseRight = face.impulse;
    if (GAS_P.openness != 0)
        ApplyFaceOpenness(sides, face);

    g_faces.Store<GasGpuFace>(FaceAddress(faceId), face);

    // --- 帳簿(gas_reference.cpp の ApplyImpulse の箱の外の側)---
    if (sides.left.kind == SIDE_CELL)
        LedgerAdd(GAS_GPU_LEDGER_MOMENTUM + 16, -face.weightLeft);

    if (sides.right.kind == SIDE_CELL)
        LedgerAdd(GAS_GPU_LEDGER_MOMENTUM + 16, -face.weightRight);

    // 帳簿 = セルが受けた力積の和(両側が同じなら 0。一部だけ開いた面の差は塞いだ物が受けた。T-0593)
    int64_t received = 0;
    if (sides.left.kind == SIDE_CELL)
        received -= face.impulse;

    if (sides.right.kind == SIDE_CELL)
        received += face.impulseRight;

    LedgerAdd(GAS_GPU_LEDGER_MOMENTUM + (sides.axis * 8), received);
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
        cell.momentum[axis] += low.impulseRight - high.impulse;
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
[numthreads(64, 1, 1)] void GasFaceFlow(uint3 thread : SV_DispatchThreadID) {
    const uint32_t faceId = GasFaceThread(thread);
    GasFaceSides sides;
    if (!FaceSidesOf(faceId, sides))
        return;

    int64_t flow = 0;
    if (sides.left.kind != SIDE_WALL && sides.right.kind != SIDE_WALL && FaceOpen(sides) != 0) {
        flow = GfFaceFlow(GasFlowParams(), ForcedSideCell(sides.left).momentum[sides.axis],
                          ForcedSideCell(sides.right).momentum[sides.axis], SideOpen(sides.left), SideOpen(sides.right),
                          DrivingPressure(sides));
    }

    uint64_t fraction = 0;
    if (flow != 0) {
        const GasSide donor = PickSide(flow > 0, sides.left, sides.right);
        fraction = GfRawFraction(flow, SideDerived(donor).inverseMass);
    }

    g_faces.Store<int64_t>(FaceAddress(faceId) + FACE_FLOW, flow);
    g_faces.Store<uint64_t>(FaceAddress(faceId) + FACE_FRACTION, fraction);
}

// 出ていく割合の合計の換算(gas_cell.hlsli の EffectiveOutflow。T-0646・ADR-0286)。熱容量と物質量は小刻みの初めのセル(g_cells)の
// 成分から(CPU は box.cells)
uint64_t CellEffectiveOutflow(uint32_t index, uint64_t outflow) {
    if (outflow == 0)
        return 0;

    GasGpuTotals totals = GasZeroTotals();
    const uint32_t speciesCount = SpeciesCount();
    for (uint32_t s = 0; s < speciesCount; ++s)
        GasAddSpecies(totals, s, g_cells.Load<uint64_t>(CellAmountAddress(index, s)));

    return EffectiveOutflow(totals, outflow);
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

    g_sums.Store<uint64_t>(SumsAddress(index), CellEffectiveOutflow(index, outflow));
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

// 風上のセルの割合から面へ延ばす差(gas_flow.hlsli の GfHalfSlope)。使えない並びは 0
int64_t HalfSlope(MusclSides muscl, uint64_t fraction, uint32_t s) {
    if (!muscl.usable)
        return 0;

    return GfHalfSlope(GasFlowParams(), GfSpeciesRatio(ForcedSideAmount(muscl.outer, s), muscl.outerInverse),
                       GfSpeciesRatio(ForcedSideAmount(muscl.donor, s), muscl.donorInverse),
                       GfSpeciesRatio(ForcedSideAmount(muscl.receiver, s), muscl.receiverInverse), fraction);
}

// --- 段 6: 割合を抑え、移す成分の物質量(面。gas_reference.cpp の LimitOutflow の 2 回目・ComputeMovedAmounts)---
[numthreads(64, 1, 1)] void GasFaceMoved(uint3 thread : SV_DispatchThreadID) {
    const uint32_t faceId = GasFaceThread(thread);
    GasFaceSides sides;
    if (!FaceSidesOf(faceId, sides))
        return;

    const GasGpuFace face = LoadFace(faceId);
    const GasSide donor = PickSide(face.flow > 0, sides.left, sides.right);
    uint64_t fraction = face.fraction;
    if (donor.kind == SIDE_CELL)
        fraction = GfLimitFraction(fraction, g_sums.Load<uint64_t>(SumsAddress(donor.index)));

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

    const GfParams params = GasFlowParams();
    const MusclSides muscl = MusclSidesOf(sides, face.flow);
    const uint64_t roundingTick = FX_U64(g_roundingTickHigh, g_roundingTickLow);
    const uint64_t faceHash = GfFaceHash(GAS_P.randomSeed, roundingTick, faceId);
    for (uint32_t s = 0; s < speciesCount; ++s) {
        const uint64_t amountQ32 = GfMovedAmountQ32(params, ForcedSideAmount(donor, s), sourceTotal, fraction,
                                                    HalfSlope(muscl, fraction, s));
        const uint64_t moved = GfRoundAmount(params, amountQ32, FxHashCombine(faceHash, GAS_SPECIES(s).roundingKey));
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

// --- 段 8: 中身を超えた成分を抑え、移すエネルギーと運動量(面。gas_reference.cpp の LimitMovedAmounts の 2 回目・Transfer の前半)---
[numthreads(64, 1, 1)] void GasFaceFinal(uint3 thread : SV_DispatchThreadID) {
    const uint32_t faceId = GasFaceThread(thread);
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
                moved = GfLimitMovedAmount(moved, content, total);
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
    face.receivedEnergy = DivideNearest(
        energy - GfGravityWork(GasFlowParams(), sides.axis, face.flow, movedTotals.mass), 1000);
    [unroll] for (uint32_t axis = 0; axis < 3; ++axis)
        face.movedMomentum[axis] = GfMovedMomentum(source.momentum[axis], face.fraction);

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

// --- 粘り(1 刻みに 1 回。gas_reference.cpp の ApplyViscosity。式は gas_flow.hlsli の GfViscousMomentum・GfViscousEnergy)---

// セルの質量と物質の粘りの動粘性(gas_reference.cpp の CellMass・CellMolecularViscosity)
struct ViscousSide {
    uint64_t mass;
    uint64_t viscosity;  // mm²/s
};

ViscousSide ViscousSideOf(uint32_t index) {
    GasGpuTotals totals = GasZeroTotals();
    uint64_t weighted = 0;
    const uint32_t speciesCount = SpeciesCount();
    for (uint32_t s = 0; s < speciesCount; ++s) {
        const uint64_t amount = g_cells.Load<uint64_t>(CellAmountAddress(index, s));
        GasAddSpecies(totals, s, amount);
        weighted += amount * (uint64_t)GAS_SPECIES(s).viscosity;
    }

    ViscousSide side;
    side.mass = totals.mass;
    side.viscosity = GfMolecularViscosity(weighted, totals.amount, totals.mass, CellOpen(index));

    return side;
}

// 段 10: 自分の 6 面(両側が箱の中のセルで、開きが 0 でない面)で隣と運動量を分け合い、運動のエネルギーの減った分を熱へ。
// 面の左 = a・右 = b として CPU と同じ ΔP・ΔE を作り、自分が左なら引き、右なら足す(散らす形と同じ値。整数の足し算)。結果は g_forced の頭へ
[numthreads(64, 1, 1)] void GasViscosity(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    const GfParams params = GasFlowParams();
    const uint64_t eddy = GAS_P.eddyViscosity;
    const CellFaceSet faces = CellFaceSetOf(index);
    const ViscousSide self = ViscousSideOf(index);
    const GasGpuCell selfCell = g_cells.Load<GasGpuCell>(CellAddress(index));
    GasGpuCell next = selfCell;
    for (uint32_t f = 0; f < 6; ++f) {
        GasFaceSides sides;
        if (!FaceSidesOf(faces.ids[f], sides) || sides.left.kind != SIDE_CELL || sides.right.kind != SIDE_CELL)
            continue;

        const uint32_t open = FaceOpen(sides);
        if (open == 0)
            continue;

        // 自分が右の面なら隣は左(a)、左の面なら隣は右(b)
        const bool right = faces.right[f];
        const uint32_t other = right ? sides.left.index : sides.right.index;
        const ViscousSide otherSide = ViscousSideOf(other);
        const GasGpuCell otherCell = g_cells.Load<GasGpuCell>(CellAddress(other));
        const uint64_t massA = right ? otherSide.mass : self.mass;
        const uint64_t massB = right ? self.mass : otherSide.mass;
        const uint64_t total = eddy + ((otherSide.viscosity + self.viscosity) / 2);
        const uint64_t coefficient = (GfViscosityQ32(total) * open) >> 16;
        int64_t energy = 0;
        [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
            const int64_t pa = right ? otherCell.momentum[axis] : selfCell.momentum[axis];
            const int64_t pb = right ? selfCell.momentum[axis] : otherCell.momentum[axis];
            const int64_t moved = GfViscousMomentum(pa, massA, pb, massB, coefficient);
            next.momentum[axis] += right ? moved : -moved;
            energy += GfViscousEnergy(params, moved, pa, massA, pb, massB);
        }

        next.energy += right ? energy : -energy;
    }

    g_forced.Store<GasGpuCell>(CellAddress(index), next);
}

    // 段 11: 粘りの後の頭(エネルギーと運動量)を状態へ。成分は変わらない
    [numthreads(64, 1, 1)] void GasViscosityApply(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    g_cells.Store<GasGpuCell>(CellAddress(index), g_forced.Load<GasGpuCell>(CellAddress(index)));
}
