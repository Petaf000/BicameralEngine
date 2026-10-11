// gas_tree_step.hlsl — 気体の多重解像度の木の 1 小刻みを GPU の Compute で(T-0209。07 §2.5・ADR-0117)。全部のレベルが同じ小刻み(σ = 0)の木。
// CPU リファレンスは engine/src/sim/gas_tree.cpp(StepGasTree の MicroStep)で、各段はその関数を同じ式で写したもの。毎刻みビット一致を
// tests/gpu_gas_tree_test.cpp が確かめる。呼ぶ順は engine/src/sim/gpu_gas_tree.cpp(段の間は UAV バリア)。
//
// 段の並びは 1 レベルの箱(gas_step.hlsl)と同じ 9 段。違うのは面の両側がレベルの違うセルでありうることだけ:
//   面の形(両側・外側の隣・軸・レベル・番号)は木を作った時に CPU が一度だけ作った一覧(g_layout。gas_tree.cpp の EnumerateFaces と同じ順)から読み、
//   セルは自分の面の並び(g_cellFaces の [faceBegin, faceEnd))を集める(CPU は面ごとに両側へ散らす。整数の足し算なので同じ値)。
//   面は細かい側のレベルの単位で計算し、粗い側の中身は × 8^d で読む。粗い側への増減は整数部 + 2^-64 の端数の 128bit の足し算(AddScaled)で、
//   どの順に足しても同じ値になる。帳簿(レベル 0 の whole + 2^-64)は下位へ 64bit の原子的な足し算 → 繰り上がりと上位を whole へ。
// 64bit の「0 で止める引き算」(a > b ? a − b : 0)は書かない(NVIDIA のドライバが誤って下ろす。T-0185)。繰り上がりも比べずにビットで出す。
#include "common/gas_tree_gpu.hlsli"

RWStructuredBuffer<GasGpuCell> g_cells : register(u0);                    // 状態(小刻みの初め → 終わり)
RWStructuredBuffer<GasTreeGpuFraction> g_fractions : register(u1);        // 状態の端数
RWStructuredBuffer<GasGpuCell> g_forced : register(u2);                   // 力を当てた後のセル
RWStructuredBuffer<GasTreeGpuFraction> g_forcedFractions : register(u3);  // その端数
RWStructuredBuffer<GasGpuDerived> g_derived : register(u4);               // 小刻みの初めの導く値(整数部だけから)
RWStructuredBuffer<GasGpuFace> g_faces : register(u5);                    // 面の動く値(g_layout と同じ順)
RWStructuredBuffer<GasGpuCellSums> g_sums : register(u6);
RWByteAddressBuffer g_ledger : register(u7);
StructuredBuffer<GasGpuParameters> g_parameters : register(t0);  // 設定とレベル 0 の係数
StructuredBuffer<GasTreeGpuLevel> g_levels : register(t1);
StructuredBuffer<GasGpuDerived> g_referenceDerived : register(t2);  // 層の表(レベルごとの層を続けて)の基準の導く値
StructuredBuffer<GasGpuCell> g_ghost : register(t3);                // 層の表の開いた境界の外(基準 + 風)
StructuredBuffer<GasTreeGpuFace> g_layout : register(t4);
StructuredBuffer<GasTreeGpuPlace> g_places : register(t5);
StructuredBuffer<uint32_t> g_cellFaces : register(t6);  // 面の番号 × 2 + 自分が右側なら 1

cbuffer RootConstants : register(b0) {
    uint32_t g_roundingTickLow;  // 小刻みの通し番号(乱数の丸め。R6。σ = 0 なのでどのレベルも同じ)
    uint32_t g_roundingTickHigh;
    uint32_t g_cellCount;
    uint32_t g_faceCount;
};

#define GAS_P g_parameters[0]

#include "common/gas_cell.hlsli"

// --- 面の片側 ---

struct TreeSide {
    uint32_t kind;   // GAS_TREE_GPU_SIDE_*
    uint32_t index;  // セル: セルの番号 / 外: 層の表の番号
    uint32_t level;
};

TreeSide MakeTreeSide(uint32_t kind, uint32_t index, uint32_t level) {
    TreeSide side;
    side.kind = kind;
    side.index = index;
    side.level = level;

    return side;
}

TreeSide LeftSide(GasTreeGpuFace layout) {
    return MakeTreeSide(layout.leftKind, layout.leftIndex, layout.leftLevel);
}

TreeSide RightSide(GasTreeGpuFace layout) {
    return MakeTreeSide(layout.rightKind, layout.rightIndex, layout.rightLevel);
}

// MUSCL の外側の隣(面と同じレベルのセルか境界か Mixed)
TreeSide OuterSide(GasTreeGpuFace layout, bool forward) {
    if (forward)
        return MakeTreeSide(layout.leftOuterKind, layout.leftOuterIndex, layout.level);

    return MakeTreeSide(layout.rightOuterKind, layout.rightOuterIndex, layout.level);
}

// HLSL の三項演算子は構造体を返せないので、側を選ぶ所はこれで
TreeSide PickSide(bool condition, TreeSide whenTrue, TreeSide whenFalse) {
    if (condition)
        return whenTrue;

    return whenFalse;
}

// 側の中身を面の単位へ直すずらし(粗い側は 3d。同じレベル・境界は 0)
uint32_t ContentShift(GasTreeGpuFace layout, TreeSide side) {
    return side.kind == GAS_TREE_GPU_SIDE_CELL ? 3 * (layout.level - side.level) : 0;
}

// 粗い側の高さの倍率 2^d(静水圧の差と重さの力積。同じレベル・境界は 1)
int64_t HeightFactor(GasTreeGpuFace layout, TreeSide side) {
    if (side.kind != GAS_TREE_GPU_SIDE_CELL)
        return 1;

    return (int64_t)1 << (layout.level - side.level);
}

GasGpuCell ForcedSideCell(TreeSide side) {
    if (side.kind == GAS_TREE_GPU_SIDE_GHOST)
        return g_ghost[side.index];

    return g_forced[side.index];
}

GasGpuDerived SideDerived(TreeSide side) {
    if (side.kind == GAS_TREE_GPU_SIDE_GHOST)
        return g_referenceDerived[side.index];

    return g_derived[side.index];
}

// 基準との質量の差(数。密度のような値なのでレベルの単位のまま)。外・壁は 0
int64_t Excess(TreeSide side) {
    if (side.kind != GAS_TREE_GPU_SIDE_CELL)
        return 0;

    return (int64_t)g_derived[side.index].mass - (int64_t)g_referenceDerived[g_places[side.index].layer].mass;
}

// 静水圧の差の半分(µPa)= 質量の差 × 39.2266 ÷ 2 ÷ 2^k(gas_tree.cpp の HalfHydrostatic)
int64_t HalfHydrostatic(int64_t excess, uint32_t level) {
    return excess * HYDROSTATIC_MICROPASCAL_PER_MASS_X10000 / ((int64_t)20000 << level);
}

int64_t DrivingPressure(GasTreeGpuFace layout, TreeSide left, TreeSide right) {
    const int64_t difference = SideDerived(left).scaledPressure - SideDerived(right).scaledPressure;
    if (layout.axis != 2 || GAS_P.gravity == 0)
        return difference;

    const int64_t excess = Excess(left) * HeightFactor(layout, left) + Excess(right) * HeightFactor(layout, right);
    return difference - HalfHydrostatic(excess, layout.level);
}

// --- 端数つきの足し算(gas_tree.cpp の AddScaled。whole + fraction × 2^-64 に value × 2^-shift を足す 128bit の足し算)---

// a + b の 64bit の繰り上がり(比べずにビットで。0 か 1)
uint64_t CarryOut(uint64_t a, uint64_t b) {
    return ((a >> 1) + (b >> 1) + (a & b & 1)) >> 63;
}

// value の下位 shift ビットを 2^-64 の単位の端数へ(shift は 1〜63)
uint64_t FractionPart(int64_t value, uint32_t shift) {
    const uint64_t mask = (FX_U64(0u, 1u) << shift) - 1;
    return ((uint64_t)value & mask) << (64 - shift);
}

void AddScaled(inout int64_t whole, inout uint64_t fraction, int64_t value, uint32_t shift) {
    if (shift == 0) {
        whole += value;
        return;
    }

    const uint64_t low = FractionPart(value, shift);
    const uint64_t before = fraction;
    fraction = before + low;
    whole += (value >> shift) + (int64_t)CarryOut(before, low);  // 符号付きの右シフトは −∞ 方向(算術シフト)
}

void AddScaledUnsigned(inout uint64_t whole, inout uint64_t fraction, int64_t value, uint32_t shift) {
    int64_t signedWhole = (int64_t)whole;
    AddScaled(signedWhole, fraction, value, shift);
    whole = (uint64_t)signedWhole;
}

// 軸を実行時の番号で選ぶ運動量の足し算(構造体の中の配列を実行時の番号で書かない)
void AddMomentum(inout GasGpuCell cell, inout GasTreeGpuFraction fraction, uint32_t axis, int64_t value,
                 uint32_t shift) {
    if (axis == 0)
        AddScaled(cell.momentum[0], fraction.momentum[0], value, shift);
    else if (axis == 1)
        AddScaled(cell.momentum[1], fraction.momentum[1], value, shift);
    else
        AddScaled(cell.momentum[2], fraction.momentum[2], value, shift);
}

// 帳簿(レベル 0 の whole + 2^-64)へ面のレベルの値を足す(shift = 3 × レベル)。下位の原子的な足し算の前の値から繰り上がりを出し、上位へ
void LedgerAddScaled(uint32_t offset, int64_t value, uint32_t shift) {
    if (value == 0)
        return;

    uint64_t before;
    if (shift == 0) {
        g_ledger.InterlockedAdd64(offset, (uint64_t)value, before);
        return;
    }

    const uint64_t low = FractionPart(value, shift);
    uint64_t carry = 0;
    if (low != 0) {
        g_ledger.InterlockedAdd64(offset + 8, low, before);
        carry = CarryOut(before, low);
    }

    const int64_t upper = (value >> shift) + (int64_t)carry;
    if (upper != 0)
        g_ledger.InterlockedAdd64(offset, (uint64_t)upper, before);
}

// 面の並びの 1 つ(セル index から見て)
struct CellFace {
    uint32_t faceIndex;
    bool right;      // 自分が面の右側
    uint32_t shift;  // 面の単位から自分の単位へ(3 × (面のレベル − 自分のレベル))
};

CellFace CellFaceAt(uint32_t entryIndex, uint32_t ownLevel) {
    const uint32_t entry = g_cellFaces[entryIndex];
    CellFace result;
    result.faceIndex = entry >> 1;
    result.right = (entry & 1) != 0;
    result.shift = 3 * (g_layout[result.faceIndex].level - ownLevel);

    return result;
}

// 自分が風上か(自分が右なら流れが負、左なら正)
bool IsDonor(CellFace cellFace, int64_t flow) {
    return cellFace.right ? flow < 0 : flow > 0;
}

// --- 段 1: 導く値(セル。gas_tree.cpp の DeriveGasTreeCell。整数部だけから・そのレベルの層の基準)---
[numthreads(64, 1, 1)] void GasTreeDerive(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    g_derived[index] = DeriveCellFromReference(g_cells[index], g_referenceDerived[g_places[index].layer].pressure);
}

    // --- 段 2: 面の力(gas_tree.cpp の ComputeFaceWeight・ComputeFaceForce)。箱の外の側の力と重さは帳簿へ(ApplyImpulse)---
    [numthreads(64, 1, 1)] void GasTreeFaceForce(uint32_t faceIndex : SV_DispatchThreadID) {
    if (faceIndex >= g_faceCount)
        return;

    const GasTreeGpuFace layout = g_layout[faceIndex];
    const GasTreeGpuLevel level = g_levels[layout.level];
    const TreeSide left = LeftSide(layout);
    const TreeSide right = RightSide(layout);
    GasGpuFace face = g_faces[faceIndex];
    face.impulse = 0;
    face.weightLeft = 0;
    face.weightRight = 0;

    // --- 重さ(縦の面。両側の質量の差の平均の 1/4。粗い側は細かい単位で × 2^d。壁の面は手前のセルだけ)---
    if (layout.axis == 2 && GAS_P.gravity != 0) {
        const int64_t coefficient = (int64_t)level.gravityImpulse;
        if (left.kind == GAS_TREE_GPU_SIDE_WALL)
            face.weightRight = FxMulShiftS64(Excess(right), coefficient, 17);
        else if (right.kind == GAS_TREE_GPU_SIDE_WALL)
            face.weightLeft = FxMulShiftS64(Excess(left), coefficient, 17);
        else {
            const int64_t quarter = FxMulShiftS64(Excess(left) + Excess(right), coefficient, 18);
            face.weightLeft = quarter * HeightFactor(layout, left);
            face.weightRight = quarter * HeightFactor(layout, right);
        }
    }

    // --- 圧力の力積(壁は手前のセルの p̃ を鏡に映す。縦の壁は半セル分の静水圧で延ばす)---
    const int64_t impulseCoefficient = (int64_t)level.pressureImpulse;
    if (left.kind == GAS_TREE_GPU_SIDE_WALL || right.kind == GAS_TREE_GPU_SIDE_WALL) {
        const bool wallBelow = left.kind == GAS_TREE_GPU_SIDE_WALL;
        const TreeSide inside = PickSide(wallBelow, right, left);
        int64_t pressure = g_derived[inside.index].scaledPressure;
        if (layout.axis == 2 && GAS_P.gravity != 0) {
            const int64_t hydrostatic = HalfHydrostatic(Excess(inside), layout.level);
            pressure += wallBelow ? hydrostatic : -hydrostatic;
        }

        face.impulse = FxMulShiftS64(pressure * 2, impulseCoefficient, 16);
    } else {
        const int64_t pressureSum = SideDerived(left).scaledPressure + SideDerived(right).scaledPressure;
        face.impulse = FxMulShiftS64(pressureSum, impulseCoefficient, 16);
    }

    g_faces[faceIndex] = face;

    // --- 帳簿(重さはセルの側の分・圧力の力積は箱の外の側の分)---
    const uint32_t ledgerShift = 3 * layout.level;
    const uint32_t momentumZ = GAS_TREE_GPU_LEDGER_MOMENTUM + (2 * GAS_TREE_GPU_LEDGER_STRIDE);
    const uint32_t momentumAxis = GAS_TREE_GPU_LEDGER_MOMENTUM + (layout.axis * GAS_TREE_GPU_LEDGER_STRIDE);
    if (left.kind == GAS_TREE_GPU_SIDE_CELL)
        LedgerAddScaled(momentumZ, -face.weightLeft, ledgerShift);
    else
        LedgerAddScaled(momentumAxis, face.impulse, ledgerShift);

    if (right.kind == GAS_TREE_GPU_SIDE_CELL)
        LedgerAddScaled(momentumZ, -face.weightRight, ledgerShift);
    else
        LedgerAddScaled(momentumAxis, -face.impulse, ledgerShift);
}

// --- 段 3: 力を当てたセル(自分の面の力積と重さを、面の単位から自分の単位へ端数つきで)---
[numthreads(64, 1, 1)] void GasTreeApplyForce(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    const GasTreeGpuPlace place = g_places[index];
    GasGpuCell cell = g_cells[index];
    GasTreeGpuFraction fraction = g_fractions[index];
    for (uint32_t entry = place.faceBegin; entry < place.faceEnd; ++entry) {
        const CellFace cellFace = CellFaceAt(entry, place.level);
        const GasGpuFace face = g_faces[cellFace.faceIndex];
        const uint32_t axis = g_layout[cellFace.faceIndex].axis;
        const int64_t weight = cellFace.right ? face.weightRight : face.weightLeft;
        AddScaled(cell.momentum[2], fraction.momentum[2], -weight, cellFace.shift);
        AddMomentum(cell, fraction, axis, cellFace.right ? face.impulse : -face.impulse, cellFace.shift);
    }

    g_forced[index] = cell;
    g_forcedFractions[index] = fraction;
}

    // --- 段 4: 面の質量の流れと、抑える前の割合(gas_tree.cpp の ComputeFaceFlow・RawFraction)---
    [numthreads(64, 1, 1)] void GasTreeFaceFlow(uint32_t faceIndex : SV_DispatchThreadID) {
    if (faceIndex >= g_faceCount)
        return;

    const GasTreeGpuFace layout = g_layout[faceIndex];
    const GasTreeGpuLevel level = g_levels[layout.level];
    const TreeSide left = LeftSide(layout);
    const TreeSide right = RightSide(layout);
    int64_t flow = 0;
    if (left.kind != GAS_TREE_GPU_SIDE_WALL && right.kind != GAS_TREE_GPU_SIDE_WALL) {
        const int64_t momentumSum = ForcedSideCell(left).momentum[layout.axis] +
                                    ForcedSideCell(right).momentum[layout.axis];
        const int64_t central = FxMulShiftS64(momentumSum, (int64_t)level.centralFlow, 52);
        const int64_t diffusion = FxMulShiftS64(DrivingPressure(layout, left, right), (int64_t)level.pressureFlow, 40);
        flow = central + diffusion;
    }

    // --- 風上の中身(面の単位。粗い風上は質量 × 8^d)のうち移す割合 ---
    uint64_t fraction = 0;
    if (flow != 0) {
        const TreeSide donor = PickSide(flow > 0, left, right);
        const GasGpuDerived donorDerived = SideDerived(donor);
        const uint32_t shift = ContentShift(layout, donor);
        uint64_t inverseMass = donorDerived.inverseMass;
        if (shift != 0)
            inverseMass = donorDerived.mass == 0 ? 0 : MAX_U64 / (donorDerived.mass << shift);

        fraction = MulShiftU64(FxAbsU64(flow), inverseMass, 32);
    }

    g_faces[faceIndex].flow = flow;
    g_faces[faceIndex].fraction = fraction;
}

// --- 段 5: 風上として出ていく割合の合計(セル。gas_tree.cpp の LimitOutflow の 1 回目の走査。σ = 0 なので × 2^g は無い)---
[numthreads(64, 1, 1)] void GasTreeOutflow(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    const GasTreeGpuPlace place = g_places[index];
    uint64_t outflow = 0;
    for (uint32_t entry = place.faceBegin; entry < place.faceEnd; ++entry) {
        const CellFace cellFace = CellFaceAt(entry, place.level);
        const GasGpuFace face = g_faces[cellFace.faceIndex];
        if (face.fraction != 0 && IsDonor(cellFace, face.flow))
            outflow += face.fraction;
    }

    g_sums[index].outflow = outflow;
}

// --- 移す成分の物質量(gas_tree.cpp の SpeciesRatio・HalfSlope・MovedAmountQ32・RoundAmount)---

int64_t SpeciesRatio(TreeSide side, uint32_t s) {
    return (int64_t)MulShiftU64(ForcedSideCell(side).amounts[s], SideDerived(side).inverseAmount, 32);
}

// MUSCL は同じレベルの 3 つ並びだけ(外側の隣が壁・Mixed なら勾配 0)
int64_t HalfSlope(GasTreeGpuFace layout, TreeSide left, TreeSide right, int64_t flow, uint64_t fraction, uint32_t s) {
    const bool forward = flow > 0;
    const TreeSide outer = OuterSide(layout, forward);
    const TreeSide donor = PickSide(forward, left, right);
    const TreeSide receiver = PickSide(forward, right, left);
    const bool outerUsable = outer.kind != GAS_TREE_GPU_SIDE_WALL && outer.kind != GAS_TREE_GPU_SIDE_MIXED;
    if (donor.kind != GAS_TREE_GPU_SIDE_CELL || !outerUsable || receiver.kind == GAS_TREE_GPU_SIDE_WALL)
        return 0;

    const int64_t donorRatio = SpeciesRatio(donor, s);
    const int64_t slope = LimitedSlope(donorRatio - SpeciesRatio(outer, s), SpeciesRatio(receiver, s) - donorRatio);

    return FxMulShiftS64(slope, (int64_t)(ONE_Q32 - fraction), 33);
}

// 移す成分 s の物質量(Q32 の面の単位。128bit)。粗い風上は中身 × 8^d(1 次の風上)、同じ単位なら gas_step.hlsl の MovedAmountQ32 と同じ
// (引く側は「0 で止める引き算」を書かず、符号付きの差を出してから 0 以下を 0 にする。T-0185)
FxU128 MovedAmountQ32(GasTreeGpuFace layout, TreeSide left, TreeSide right, TreeSide donor, GasGpuCell source,
                      int64_t flow, uint64_t fraction, uint32_t s) {
    FxU128 result;
    const uint32_t shift = ContentShift(layout, donor);
    if (shift != 0) {
        const FxU128 product = FxMulU64Full(source.amounts[s], fraction);
        result.hi = (product.hi << shift) | (product.lo >> (64 - shift));
        result.lo = product.lo << shift;
        return result;
    }

    result.hi = 0;
    const uint64_t moved = source.amounts[s] * fraction;
    result.lo = moved;
    if (GAS_P.reconstruction == GAS_GPU_RECONSTRUCTION_UPWIND)
        return result;

    const int64_t halfSlope = HalfSlope(layout, left, right, flow, fraction, s);
    const uint64_t total = TotalAmount(source) * fraction;
    const uint64_t correction = MulShiftU64(total, FxAbsU64(halfSlope), 32);
    if (halfSlope >= 0) {
        result.lo = moved + correction;
        return result;
    }

    const int64_t remaining = (int64_t)moved - (int64_t)correction;
    if (remaining <= 0) {
        result.lo = 0;
        return result;
    }

    result.lo = (uint64_t)remaining;
    return result;
}

uint64_t RoundAmount128(FxU128 amountQ32, uint64_t hash) {
    const uint64_t whole = FxShiftRightU128(amountQ32, 32);
    if (GAS_P.stochasticRounding == 0)
        return whole;

    return whole + ((hash >> 32) < (amountQ32.lo & FX_LOW32_MASK) ? 1 : 0);
}

// --- 段 6: 割合を抑え、移す成分の物質量(面。gas_tree.cpp の LimitOutflow の 2 回目・ComputeMovedAmounts)---
[numthreads(64, 1, 1)] void GasTreeFaceMoved(uint32_t faceIndex : SV_DispatchThreadID) {
    if (faceIndex >= g_faceCount)
        return;

    const GasTreeGpuFace layout = g_layout[faceIndex];
    const TreeSide left = LeftSide(layout);
    const TreeSide right = RightSide(layout);
    GasGpuFace face = g_faces[faceIndex];
    const TreeSide donor = PickSide(face.flow > 0, left, right);
    if (face.fraction != 0 && donor.kind == GAS_TREE_GPU_SIDE_CELL) {
        const uint64_t outflow = g_sums[donor.index].outflow;
        if (outflow > ONE_Q32)
            face.fraction = FxDivU128By64(FxMulU64Full(face.fraction, ONE_Q32), outflow).quotient;
    }

    [unroll] for (uint32_t clear = 0; clear < GAS_GPU_MAX_SPECIES; ++clear) {
        face.moved[clear] = 0;
    }

    // 乱数は(世界のシード, 小刻みの通し番号, 面の番号, 用途 + レベル)+ 成分。レベル 0 は gas_step.hlsl と同じ
    if (face.fraction != 0) {
        const GasGpuCell source = ForcedSideCell(donor);
        const uint64_t roundingTick = FX_U64(g_roundingTickHigh, g_roundingTickLow);
        const uint64_t faceHash = FxHash64(GAS_P.randomSeed, roundingTick, layout.id,
                                           GAS_ROUNDING_PURPOSE + layout.level);
        [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s) {
            const FxU128 amountQ32 = MovedAmountQ32(layout, left, right, donor, source, face.flow, face.fraction, s);
            face.moved[s] = RoundAmount128(amountQ32, FxHashCombine(faceHash, s));
        }
    }

    g_faces[faceIndex] = face;
}

    // --- 段 7: 成分ごとの移す量の合計(セル。gas_tree.cpp の LimitMovedAmounts の 1 回目の走査。風上のレベル + 1 の単位で数える)---
    [numthreads(64, 1, 1)] void GasTreeMovedSum(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    const GasTreeGpuPlace place = g_places[index];
    GasGpuCellSums sums = g_sums[index];
    [unroll] for (uint32_t clear = 0; clear < GAS_GPU_MAX_SPECIES; ++clear) {
        sums.outgoing[clear] = 0;
    }

    for (uint32_t entry = place.faceBegin; entry < place.faceEnd; ++entry) {
        const CellFace cellFace = CellFaceAt(entry, place.level);
        const GasGpuFace face = g_faces[cellFace.faceIndex];
        if (face.fraction == 0 || !IsDonor(cellFace, face.flow))
            continue;

        const uint32_t unitShift = 3 - cellFace.shift;
        [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s) {
            sums.outgoing[s] += face.moved[s] << unitShift;
        }
    }

    g_sums[index] = sums;
}

// 重さの仕事(µJ。gas_tree.cpp の GravityWork。1 mg が 1 セル〔0.5 m × 2^-k〕上がる仕事)
int64_t GravityWork(GasTreeGpuFace layout, int64_t flow, GasGpuCell moved) {
    if (layout.axis != 2 || GAS_P.gravity == 0 || GAS_P.thermodynamics != GAS_GPU_THERMODYNAMICS_ADIABATIC)
        return 0;

    const int64_t work = (int64_t)(CellMass(moved) * GRAVITY_WORK_MICROJOULE_PER_MASS_X1E6 / (MICRO << layout.level));

    return flow > 0 ? work : -work;
}

// 風上の中身を面の単位へ(粗い風上は × 8^d。gas_tree.cpp の Transfer の前半)
void ScaleSource(inout GasGpuCell source, inout GasGpuDerived sourceDerived, uint32_t shift) {
    if (shift == 0)
        return;

    [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s) {
        source.amounts[s] = source.amounts[s] << shift;
    }

    source.energy = source.energy * ((int64_t)1 << shift);
    [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
        source.momentum[axis] = source.momentum[axis] * ((int64_t)1 << shift);
    }

    sourceDerived.mass = sourceDerived.mass << shift;
}

// --- 段 8: 中身を超えた成分を抑え、移すエネルギーと運動量(面。gas_tree.cpp の LimitMovedAmounts の 2 回目・Transfer)---
[numthreads(64, 1, 1)] void GasTreeFaceFinal(uint32_t faceIndex : SV_DispatchThreadID) {
    if (faceIndex >= g_faceCount)
        return;

    GasGpuFace face = g_faces[faceIndex];
    if (face.fraction == 0)
        return;

    const GasTreeGpuFace layout = g_layout[faceIndex];
    const bool forward = face.flow > 0;
    const TreeSide donor = PickSide(forward, LeftSide(layout), RightSide(layout));
    const TreeSide receiver = PickSide(forward, RightSide(layout), LeftSide(layout));
    GasGpuCell source = ForcedSideCell(donor);
    if (donor.kind == GAS_TREE_GPU_SIDE_CELL) {
        const GasGpuCellSums sums = g_sums[donor.index];
        [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s) {
            const uint64_t total = sums.outgoing[s];
            const uint64_t content = source.amounts[s] << 3;
            if (total > content)
                face.moved[s] = FxDivU128By64(FxMulU64Full(face.moved[s], content), total).quotient;
        }
    }

    // --- 移す塊(成分・エネルギー・運動量。面の単位)---
    GasGpuDerived sourceDerived = SideDerived(donor);
    ScaleSource(source, sourceDerived, ContentShift(layout, donor));
    GasGpuCell moved;
    [unroll] for (uint32_t copy = 0; copy < GAS_GPU_MAX_SPECIES; ++copy) {
        moved.amounts[copy] = face.moved[copy];
    }

    moved.energy = 0;
    [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
        moved.momentum[axis] = FxMulShiftS64(source.momentum[axis], (int64_t)face.fraction, 32);
        face.movedMomentum[axis] = moved.momentum[axis];
    }

    const int64_t energy = MovedEnergy(source, sourceDerived, moved, face.fraction);
    face.movedEnergy = DivideNearest(energy, 1000);
    face.receivedEnergy = DivideNearest(energy - GravityWork(layout, face.flow, moved), 1000);
    g_faces[faceIndex] = face;

    // --- 帳簿(開いた境界の外から入る物は正、出る物は負。エネルギーは重さの仕事〔受けた − 引いた〕も)---
    const uint32_t ledgerShift = 3 * layout.level;
    if (donor.kind == GAS_TREE_GPU_SIDE_CELL && receiver.kind == GAS_TREE_GPU_SIDE_CELL) {
        LedgerAddScaled(GAS_TREE_GPU_LEDGER_ENERGY, face.receivedEnergy - face.movedEnergy, ledgerShift);
        return;
    }

    const int64_t sign = donor.kind == GAS_TREE_GPU_SIDE_CELL ? -1 : 1;
    [unroll] for (uint32_t species = 0; species < GAS_GPU_MAX_SPECIES; ++species) {
        LedgerAddScaled(GAS_TREE_GPU_LEDGER_AMOUNTS + (species * GAS_TREE_GPU_LEDGER_STRIDE),
                        sign * (int64_t)face.moved[species], ledgerShift);
    }

    LedgerAddScaled(GAS_TREE_GPU_LEDGER_ENERGY,
                    donor.kind == GAS_TREE_GPU_SIDE_CELL ? -face.movedEnergy : face.receivedEnergy, ledgerShift);
    [unroll] for (uint32_t momentumAxis = 0; momentumAxis < 3; ++momentumAxis) {
        LedgerAddScaled(GAS_TREE_GPU_LEDGER_MOMENTUM + (momentumAxis * GAS_TREE_GPU_LEDGER_STRIDE),
                        sign * face.movedMomentum[momentumAxis], ledgerShift);
    }
}

    // --- 段 9: 移す(セル。gas_tree.cpp の AddParcel。自分の面の出入りを、面の単位から自分の単位へ端数つきで)---
    [numthreads(64, 1, 1)] void GasTreeTransfer(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    const GasTreeGpuPlace place = g_places[index];
    GasGpuCell cell = g_forced[index];
    GasTreeGpuFraction fraction = g_forcedFractions[index];
    for (uint32_t entry = place.faceBegin; entry < place.faceEnd; ++entry) {
        const CellFace cellFace = CellFaceAt(entry, place.level);
        const GasGpuFace face = g_faces[cellFace.faceIndex];
        if (face.fraction == 0)
            continue;

        const bool donor = IsDonor(cellFace, face.flow);
        const int64_t sign = donor ? -1 : 1;
        [unroll] for (uint32_t s = 0; s < GAS_GPU_MAX_SPECIES; ++s) {
            AddScaledUnsigned(cell.amounts[s], fraction.amounts[s], sign * (int64_t)face.moved[s], cellFace.shift);
        }

        AddScaled(cell.energy, fraction.energy, donor ? -face.movedEnergy : face.receivedEnergy, cellFace.shift);
        [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
            AddScaled(cell.momentum[axis], fraction.momentum[axis], sign * face.movedMomentum[axis], cellFace.shift);
        }
    }

    g_cells[index] = cell;
    g_fractions[index] = fraction;
}
