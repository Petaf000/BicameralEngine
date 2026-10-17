// gas_tree_step.hlsl — 気体の多重解像度の木のいちばん細かい小刻み 1 回を GPU の Compute で(T-0209・T-0356。07 §2.5・ADR-0117)。
// CPU リファレンスは engine/src/sim/gas_tree.cpp(StepGasTree の MicroStep)で、各段はその関数を同じ式で写したもの。毎刻みビット一致を
// tests/gpu_gas_tree_test.cpp が確かめる。呼ぶ順は engine/src/sim/gpu_gas_tree.cpp(段の間は UAV バリア)。
//
// 段の並びは 1 レベルの箱(gas_step.hlsl)と同じ 9 段。違うのは面の両側がレベルの違うセルでありうることだけ:
//   面の形(両側・外側の隣・軸・レベル・番号)は木を作った時に CPU が一度だけ作った一覧(g_layout。gas_tree.cpp の EnumerateFaces と同じ順)から読み、
//   セルは自分の面の並び(g_cellFaces の [faceBegin, faceEnd))を集める(CPU は面ごとに両側へ散らす。整数の足し算なので同じ値)。
//   面は細かい側のレベルの単位で計算し、粗い側の中身は × 8^d で読む。粗い側への増減は整数部 + 2^-64 の端数の 128bit の足し算(AddScaled)で、
//   どの順に足しても同じ値になる。帳簿(レベル 0 の whole + 2^-64)は下位へ 64bit の原子的な足し算 → 繰り上がりと上位を whole へ。
// レベルごとの小刻み(T-0356。T-0320 の時間線): 1 刻みは いちばん細かい小刻み substeps × 2^σmax 回で、各回(g_micro)ではそこで小刻みを
//   始めるレベルのセルだけが導く値・forced・next を作り直し、始めるレベルの面だけを計算し、小刻みを終えるレベルのセルだけが next を状態へ写す。
//   粗い側への力積と塊は next(粗い小刻みの間ずっと持つ)へ。粗い側から細かい面が何度も引く時の抑えはセルごとの g_carry
//   (committedOutflow・taken)で。σ = 0(全部同じ小刻み)なら毎回 全部のセル・面が始まって終わる = T-0209 と同じ段・同じ値。
// 64bit の「0 で止める引き算」(a > b ? a − b : 0)は書かない(NVIDIA のドライバが誤って下ろす。T-0185)。繰り上がりも比べずにビットで出す。
#include "common/gas_tree_gpu.hlsli"

// 成分ごとの欄を持つもの(gas_gpu.hlsli・gas_tree_gpu.hlsli。成分の数に上限なし。T-0598)は ByteAddressBuffer: セル・端数・面・
// セルごとの合計・出した量は頭の並び + 成分ごとの面(件の数はセルの数か面の数)、設定・外・帳簿は「頭 + 欄 × g_speciesStride」の件。
// 回す成分の数は設定の speciesCount
RWByteAddressBuffer g_cells : register(u0);                  // 状態(小刻みの初め → 終わり)
RWByteAddressBuffer g_fractions : register(u1);              // 状態の端数
RWByteAddressBuffer g_forced : register(u2);                 // 力を当てた後のセル
RWByteAddressBuffer g_forcedFractions : register(u3);        // その端数
RWStructuredBuffer<GasGpuDerived> g_derived : register(u4);  // 小刻みの初めの導く値(整数部だけから)
RWByteAddressBuffer g_faces : register(u5);                  // 面の動く値(g_layout と同じ順)
RWByteAddressBuffer g_sums : register(u6);
RWByteAddressBuffer g_ledger : register(u7);
RWByteAddressBuffer g_next : register(u8);           // 小刻みの終わりの値(粗い小刻みの間は持ち越す。T-0356)
RWByteAddressBuffer g_nextFractions : register(u9);  // その端数
RWByteAddressBuffer g_carry : register(u10);         // 粗い小刻みの間に出した量
ByteAddressBuffer g_parameters : register(t0);       // 設定の頭 + 成分ごとの値(レベル 0 の係数)
StructuredBuffer<GasTreeGpuLevel> g_levels : register(t1);
StructuredBuffer<GasGpuDerived> g_referenceDerived : register(t2);  // 層の表(レベルごとの層を続けて)の基準の導く値
ByteAddressBuffer g_ghost : register(t3);                           // 層の表の開いた境界の外(基準 + 風。セルの件)
StructuredBuffer<GasTreeGpuFace> g_layout : register(t4);
StructuredBuffer<GasTreeGpuPlace> g_places : register(t5);
StructuredBuffer<uint32_t> g_cellFaces : register(t6);  // 面の番号 × 2 + 自分が右側なら 1

cbuffer RootConstants : register(b0) {
    uint32_t g_tickLow;  // 刻みの番号(乱数の丸めの小刻みの通し番号はここからレベルごとに作る。R6)
    uint32_t g_tickHigh;
    uint32_t g_micro;        // いちばん細かい小刻みの通し番号(刻みの中。0 〜 substeps × 2^σmax − 1)
    uint32_t g_substeps;     // レベル 0 の小刻みの数(config.substeps)
    uint32_t g_maxExponent;  // σmax
    uint32_t g_cellCount;
    uint32_t g_faceCount;
    uint32_t g_speciesStride;  // 1 件の成分の欄の数(木を作る時に決まる)
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

// --- 件の読み書き(頭の並び + 成分ごとの面。セルと端数は同じ形、セルごとの合計と出した量も同じ形)---

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

uint32_t FaceAddress(uint32_t faceIndex) {
    return faceIndex * GAS_GPU_FACE_BYTES;
}

uint32_t FaceMovedAddress(uint32_t faceIndex, uint32_t s) {
    return GasGpuPlaneAddress(GAS_GPU_FACE_BYTES, g_faceCount, faceIndex, s);
}

uint32_t SumsAddress(uint32_t index) {
    return index * GAS_GPU_SUMS_BYTES;
}

uint32_t SumsAmountAddress(uint32_t index, uint32_t s) {
    return GasGpuPlaneAddress(GAS_GPU_SUMS_BYTES, g_cellCount, index, s);
}

// 開いた境界の外(層の表の件 = 頭 + 欄 × g_speciesStride)
uint32_t GhostAddress(uint32_t layer) {
    return layer * GasGpuCellStride(g_speciesStride);
}

GasGpuFace LoadFace(uint32_t faceIndex) {
    return g_faces.Load<GasGpuFace>(FaceAddress(faceIndex));
}

uint64_t FaceMoved(uint32_t faceIndex, uint32_t s) {
    return g_faces.Load<uint64_t>(FaceMovedAddress(faceIndex, s));
}

// --- レベルごとの小刻み(gas_tree.cpp の Exponent・Starts・Ends・RoundingTick・SubcycleGap。T-0356)---

uint32_t Exponent(uint32_t level) {
    return g_levels[level].subcycleExponent;
}

// レベル level の小刻みの長さ − 1(いちばん細かい小刻みの数で。2^(σmax − σ) − 1)
uint32_t PeriodMask(uint32_t level) {
    return (1u << (g_maxExponent - Exponent(level))) - 1;
}

bool Starts(uint32_t level) {
    return (g_micro & PeriodMask(level)) == 0;
}

bool Ends(uint32_t level) {
    return ((g_micro + 1) & PeriodMask(level)) == 0;
}

// 乱数の丸めの小刻みの通し番号はレベルごと(tick × そのレベルの小刻みの数 + その中の番号。σ = 0 なら tick × substeps + 小刻み)
uint64_t RoundingTick(uint32_t level) {
    const uint32_t exponent = Exponent(level);
    const uint64_t substeps = (uint64_t)g_substeps << exponent;

    return (FX_U64(g_tickHigh, g_tickLow) * substeps) + (uint64_t)(g_micro >> (g_maxExponent - exponent));
}

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
        return g_ghost.Load<GasGpuCell>(GhostAddress(side.index));

    return g_forced.Load<GasGpuCell>(CellAddress(side.index));
}

uint64_t ForcedSideAmount(TreeSide side, uint32_t s) {
    if (side.kind == GAS_TREE_GPU_SIDE_GHOST)
        return g_ghost.Load<uint64_t>(GhostAddress(side.index) + GAS_GPU_CELL_BYTES + (s * 8));

    return g_forced.Load<uint64_t>(CellAmountAddress(side.index, s));
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

// 静水圧の差の半分(µPa)= 質量の差 × 層の重さの係数(標準の重力で 39.2266)÷ 2 ÷ 2^k(gas_tree.cpp の HalfHydrostatic)
int64_t HalfHydrostatic(int64_t excess, uint32_t level) {
    return excess * (int64_t)GAS_P.hydrostatic / ((int64_t)20000 << level);
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
    bool active;     // 面がこの小刻み(g_micro)で計算される(面のレベルの小刻みがここで始まる)
    uint32_t gap;    // 面の小刻みと自分の小刻みの差 g(自分の小刻み 1 回の間に面は 2^g 回。gas_tree.cpp の SubcycleGap)
};

CellFace CellFaceAt(uint32_t entryIndex, uint32_t ownLevel) {
    const uint32_t entry = g_cellFaces[entryIndex];
    const uint32_t faceLevel = g_layout[entry >> 1].level;
    CellFace result;
    result.faceIndex = entry >> 1;
    result.right = (entry & 1) != 0;
    result.shift = 3 * (faceLevel - ownLevel);
    result.active = Starts(faceLevel);
    result.gap = Exponent(faceLevel) - Exponent(ownLevel);

    return result;
}

// 自分が風上か(自分が右なら流れが負、左なら正)
bool IsDonor(CellFace cellFace, int64_t flow) {
    return cellFace.right ? flow < 0 : flow > 0;
}

// --- 段 1: 小刻みを始めるレベルのセル(gas_tree.cpp の BeginCells): 導く値(DeriveGasTreeCell。整数部だけから・そのレベルの層の基準)・
// 力を当てる前の値・出した量の数える欄 ---
[numthreads(64, 1, 1)] void GasTreeDerive(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    const GasTreeGpuPlace place = g_places[index];
    if (!Starts(place.level))
        return;

    // --- 成分(導く値の和・forced へ写す・出した量を 0 に)---
    GasGpuTotals totals = GasZeroTotals();
    const uint32_t speciesCount = SpeciesCount();
    for (uint32_t s = 0; s < speciesCount; ++s) {
        const uint32_t address = CellAmountAddress(index, s);
        const uint64_t amount = g_cells.Load<uint64_t>(address);
        GasAddSpecies(totals, s, amount);
        g_forced.Store<uint64_t>(address, amount);
        g_forcedFractions.Store<uint64_t>(address, g_fractions.Load<uint64_t>(address));
        g_carry.Store<uint64_t>(SumsAmountAddress(index, s), 0);
    }

    const GasGpuCell cell = g_cells.Load<GasGpuCell>(CellAddress(index));
    g_derived[index] = DeriveCellFromReference(cell, totals, g_referenceDerived[place.layer].pressure);
    g_forced.Store<GasGpuCell>(CellAddress(index), cell);
    g_forcedFractions.Store<GasTreeGpuFraction>(CellAddress(index),
                                                g_fractions.Load<GasTreeGpuFraction>(CellAddress(index)));
    g_carry.Store<uint64_t>(SumsAddress(index), 0);
}

    // --- 段 2: 面の力(gas_tree.cpp の ComputeFaceWeight・ComputeFaceForce)。箱の外の側の力と重さは帳簿へ(ApplyImpulse)---
    [numthreads(64, 1, 1)] void GasTreeFaceForce(uint32_t faceIndex : SV_DispatchThreadID) {
    if (faceIndex >= g_faceCount)
        return;

    const GasTreeGpuFace layout = g_layout[faceIndex];
    if (!Starts(layout.level))
        return;

    const GasTreeGpuLevel level = g_levels[layout.level];
    const TreeSide left = LeftSide(layout);
    const TreeSide right = RightSide(layout);
    GasGpuFace face = LoadFace(faceIndex);
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

    g_faces.Store<GasGpuFace>(FaceAddress(faceIndex), face);

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

// 自分の面の力積と重さを、面の単位から自分の単位へ端数つきで足す(この小刻みで計算した面だけ。gas_tree.cpp の ApplySideForce)。
// immediate(forced の回): 小刻みの差が 0 の面は 1 倍、0 でない面(自分が粗い側)は自分の小刻みが始まる時だけ見込みの 2^g 倍。
// そうでない回(next の回): 小刻みの差が 0 でない面を 1 倍、自分の小刻みが始まる時は見込みを引いて 1 − 2^g 倍(T-0479)
void AddFaceForces(GasTreeGpuPlace place, bool immediate, bool starts, inout GasGpuCell cell,
                   inout GasTreeGpuFraction fraction) {
    for (uint32_t entry = place.faceBegin; entry < place.faceEnd; ++entry) {
        const CellFace cellFace = CellFaceAt(entry, place.level);
        if (!cellFace.active)
            continue;

        const int64_t predicted = (int64_t)1 << cellFace.gap;
        int64_t multiplier = 1;
        if (immediate) {
            if (cellFace.gap != 0 && !starts)
                continue;

            multiplier = predicted;
        } else {
            if (cellFace.gap == 0)
                continue;

            multiplier = starts ? 1 - predicted : 1;
        }

        const GasGpuFace face = LoadFace(cellFace.faceIndex);
        const uint32_t axis = g_layout[cellFace.faceIndex].axis;
        const int64_t weight = cellFace.right ? face.weightRight : face.weightLeft;
        AddScaled(cell.momentum[2], fraction.momentum[2], -weight * multiplier, cellFace.shift);
        AddMomentum(cell, fraction, axis, (cellFace.right ? face.impulse : -face.impulse) * multiplier, cellFace.shift);
    }
}

// --- 段 3: 力を当てる(gas_tree.cpp の ApplyForces)。小刻みを始めるセルは同じ小刻みの面の力積と細かい小刻みの面の見込みを forced へ・
// next は forced の写し。細かい小刻みの面の力積は next へ(粗い側は粗い小刻みの終わりに受ける。T-0320。見込みの分は引く。T-0479)---
[numthreads(64, 1, 1)] void GasTreeApplyForce(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    const GasTreeGpuPlace place = g_places[index];
    const uint32_t address = CellAddress(index);
    GasGpuCell next;
    GasTreeGpuFraction nextFraction;
    const bool starts = Starts(place.level);
    if (starts) {
        GasGpuCell cell = g_forced.Load<GasGpuCell>(address);
        GasTreeGpuFraction fraction = g_forcedFractions.Load<GasTreeGpuFraction>(address);
        AddFaceForces(place, true, true, cell, fraction);
        g_forced.Store<GasGpuCell>(address, cell);
        g_forcedFractions.Store<GasTreeGpuFraction>(address, fraction);
        next = cell;
        nextFraction = fraction;

        // 成分は力で変わらないので forced のまま next へ
        const uint32_t speciesCount = SpeciesCount();
        for (uint32_t s = 0; s < speciesCount; ++s) {
            const uint32_t amountAddress = CellAmountAddress(index, s);
            g_next.Store<uint64_t>(amountAddress, g_forced.Load<uint64_t>(amountAddress));
            g_nextFractions.Store<uint64_t>(amountAddress, g_forcedFractions.Load<uint64_t>(amountAddress));
        }
    } else {
        next = g_next.Load<GasGpuCell>(address);
        nextFraction = g_nextFractions.Load<GasTreeGpuFraction>(address);
    }

    AddFaceForces(place, false, starts, next, nextFraction);
    g_next.Store<GasGpuCell>(address, next);
    g_nextFractions.Store<GasTreeGpuFraction>(address, nextFraction);
}

    // --- 段 4: 面の質量の流れと、抑える前の割合(gas_tree.cpp の ComputeFaceFlow・RawFraction)---
    [numthreads(64, 1, 1)] void GasTreeFaceFlow(uint32_t faceIndex : SV_DispatchThreadID) {
    if (faceIndex >= g_faceCount)
        return;

    const GasTreeGpuFace layout = g_layout[faceIndex];
    if (!Starts(layout.level))
        return;

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

    g_faces.Store<int64_t>(FaceAddress(faceIndex) + FACE_FLOW, flow);
    g_faces.Store<uint64_t>(FaceAddress(faceIndex) + FACE_FRACTION, fraction);
}

// --- 段 5: 風上として出ていく割合の合計(セル。gas_tree.cpp の LimitOutflow の 1 回目の走査)。
// 自分の小刻み 1 回の間に 2^g 回引く細かい面は × 2^g で数える(T-0320)---
[numthreads(64, 1, 1)] void GasTreeOutflow(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    const GasTreeGpuPlace place = g_places[index];
    uint64_t outflow = 0;
    for (uint32_t entry = place.faceBegin; entry < place.faceEnd; ++entry) {
        const CellFace cellFace = CellFaceAt(entry, place.level);
        if (!cellFace.active)
            continue;

        const GasGpuFace face = LoadFace(cellFace.faceIndex);
        if (face.fraction != 0 && IsDonor(cellFace, face.flow))
            outflow += face.fraction << cellFace.gap;
    }

    g_sums.Store<uint64_t>(SumsAddress(index), outflow);
}

// --- 移す成分の物質量(gas_tree.cpp の SpeciesRatio・HalfSlope・MovedAmountQ32・RoundAmount)---

// MUSCL の 3 つ並び(風上の外側・風上・風下)と、それぞれの 1 ÷ 物質量。同じレベルの 3 つ並びだけ(外側の隣が壁・Mixed なら勾配 0)
struct MusclSides {
    bool usable;
    TreeSide outer;
    TreeSide donor;
    TreeSide receiver;
    uint64_t outerInverse;
    uint64_t donorInverse;
    uint64_t receiverInverse;
};

MusclSides MusclSidesOf(GasTreeGpuFace layout, TreeSide left, TreeSide right, int64_t flow) {
    const bool forward = flow > 0;
    MusclSides muscl;
    muscl.outer = OuterSide(layout, forward);
    muscl.donor = PickSide(forward, left, right);
    muscl.receiver = PickSide(forward, right, left);
    const bool outerUsable = muscl.outer.kind != GAS_TREE_GPU_SIDE_WALL && muscl.outer.kind != GAS_TREE_GPU_SIDE_MIXED;
    muscl.usable = GAS_P.reconstruction != GAS_GPU_RECONSTRUCTION_UPWIND &&
                   muscl.donor.kind == GAS_TREE_GPU_SIDE_CELL && outerUsable &&
                   muscl.receiver.kind != GAS_TREE_GPU_SIDE_WALL;
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

int64_t SpeciesRatio(TreeSide side, uint64_t inverseAmount, uint32_t s) {
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

// 移す成分 s の物質量(Q32 の面の単位。128bit)。粗い風上は中身 × 8^d(1 次の風上)、同じ単位なら gas_step.hlsl の MovedAmountQ32 と同じ
// (引く側は「0 で止める引き算」を書かず、符号付きの差を出してから 0 以下を 0 にする。T-0185)。sourceTotal = 風上の物質量の合計
FxU128 MovedAmountQ32(MusclSides muscl, uint32_t shift, uint64_t sourceAmount, uint64_t sourceTotal, uint64_t fraction,
                      uint32_t s) {
    FxU128 result;
    if (shift != 0) {
        const FxU128 product = FxMulU64Full(sourceAmount, fraction);
        result.hi = (product.hi << shift) | (product.lo >> (64 - shift));
        result.lo = product.lo << shift;
        return result;
    }

    result.hi = 0;
    const uint64_t moved = sourceAmount * fraction;
    result.lo = moved;
    if (GAS_P.reconstruction == GAS_GPU_RECONSTRUCTION_UPWIND)
        return result;

    const int64_t halfSlope = HalfSlope(muscl, fraction, s);
    const uint64_t total = sourceTotal * fraction;
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
    if (!Starts(layout.level))
        return;

    const TreeSide left = LeftSide(layout);
    const TreeSide right = RightSide(layout);
    const GasGpuFace face = LoadFace(faceIndex);
    const TreeSide donor = PickSide(face.flow > 0, left, right);
    uint64_t fraction = face.fraction;
    if (fraction != 0 && donor.kind == GAS_TREE_GPU_SIDE_CELL) {
        // 箱と同じ換算(gas_cell.hlsli の EffectiveOutflow。gas_tree.cpp の LimitOutflow。T-0647)。使える分 = 1 − 風上の小刻みの初めに
        // 同じ小刻みの面が出した分(換算した値。換算しても 2^63 より十分小さい)。「0 で止める引き算」は書かず、符号付きの差を出してから
        // 0 以下を 0 に(T-0185。1 で頭打ちにしてから引く形〔1 − min(c, 1)〕では subcycled_fast が CPU と食い違った。下ろし方は未確認)
        GasGpuTotals donorTotals = GasZeroTotals();
        const uint32_t donorSpeciesCount = SpeciesCount();
        for (uint32_t t = 0; t < donorSpeciesCount; ++t)
            GasAddSpecies(donorTotals, t, ForcedSideAmount(donor, t));

        const uint64_t outflow = EffectiveOutflow(donorTotals, g_sums.Load<uint64_t>(SumsAddress(donor.index)));
        const uint64_t committed = EffectiveOutflow(donorTotals, g_carry.Load<uint64_t>(SumsAddress(donor.index)));
        const int64_t room = (int64_t)ONE_Q32 - (int64_t)committed;
        const uint64_t available = room > 0 ? (uint64_t)room : 0;
        if (outflow > available)
            fraction = FxDivU128By64(FxMulU64Full(fraction, available), outflow).quotient;
    }

    g_faces.Store<uint64_t>(FaceAddress(faceIndex) + FACE_FRACTION, fraction);
    const uint32_t speciesCount = SpeciesCount();
    if (fraction == 0) {
        for (uint32_t clear = 0; clear < speciesCount; ++clear)
            g_faces.Store<uint64_t>(FaceMovedAddress(faceIndex, clear), 0);

        return;
    }

    // --- 風上の物質量の合計(同じ単位の MUSCL の補正)---
    const uint32_t shift = ContentShift(layout, donor);
    uint64_t sourceTotal = 0;
    if (shift == 0 && GAS_P.reconstruction != GAS_GPU_RECONSTRUCTION_UPWIND) {
        for (uint32_t t = 0; t < speciesCount; ++t)
            sourceTotal += ForcedSideAmount(donor, t);
    }

    // 乱数は(世界のシード, そのレベルの小刻みの通し番号, 面の番号, 用途 + レベル)+ 成分の鍵。レベル 0 は gas_step.hlsl と同じ
    const MusclSides muscl = MusclSidesOf(layout, left, right, face.flow);
    const uint64_t roundingTick = RoundingTick(layout.level);
    const uint64_t faceHash = FxHash64(GAS_P.randomSeed, roundingTick, layout.id, GAS_ROUNDING_PURPOSE + layout.level);
    for (uint32_t s = 0; s < speciesCount; ++s) {
        const FxU128 amountQ32 = MovedAmountQ32(muscl, shift, ForcedSideAmount(donor, s), sourceTotal, fraction, s);
        const uint64_t moved = RoundAmount128(amountQ32, FxHashCombine(faceHash, GAS_SPECIES(s).roundingKey));
        g_faces.Store<uint64_t>(FaceMovedAddress(faceIndex, s), moved);
    }
}

    // --- 段 7: 成分ごとの移す量の合計(セル。gas_tree.cpp の LimitMovedAmounts の 1 回目の走査。風上のレベル + 1 の単位で数える)---
    [numthreads(64, 1, 1)] void GasTreeMovedSum(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    const GasTreeGpuPlace place = g_places[index];
    const uint32_t speciesCount = SpeciesCount();
    for (uint32_t clear = 0; clear < speciesCount; ++clear)
        g_sums.Store<uint64_t>(SumsAmountAddress(index, clear), 0);

    // 面ごとに自分の件へ足す(自分のスレッドだけが書くので、足す順は結果に関わらない)
    for (uint32_t entry = place.faceBegin; entry < place.faceEnd; ++entry) {
        const CellFace cellFace = CellFaceAt(entry, place.level);
        if (!cellFace.active)
            continue;

        const GasGpuFace face = LoadFace(cellFace.faceIndex);
        if (face.fraction == 0 || !IsDonor(cellFace, face.flow))
            continue;

        const uint32_t unitShift = 3 - cellFace.shift;
        for (uint32_t s = 0; s < speciesCount; ++s) {
            const uint32_t address = SumsAmountAddress(index, s);
            g_sums.Store<uint64_t>(address,
                                   g_sums.Load<uint64_t>(address) + (FaceMoved(cellFace.faceIndex, s) << unitShift));
        }
    }
}

// 重さの仕事(µJ。gas_tree.cpp の GravityWork。1 mg が 1 セル〔0.5 m × 2^-k〕上がる仕事。movedMass = 移す塊の質量)
int64_t GravityWork(GasTreeGpuFace layout, int64_t flow, uint64_t movedMass) {
    if (layout.axis != 2 || GAS_P.gravity == 0 || GAS_P.thermodynamics != GAS_GPU_THERMODYNAMICS_ADIABATIC)
        return 0;

    const int64_t work = (int64_t)(movedMass * GAS_P.gravityWork / (MICRO << layout.level));

    return flow > 0 ? work : -work;
}

// 風上の中身を面の単位へ(粗い風上は × 8^d。gas_tree.cpp の Transfer の前半)。成分の和は物質量・比熱・化学のエネルギーを 2^shift 倍
// (成分ごとにずらしてから足すのと同じ値。整数の掛け算と足し算は 2^64 を法として分配する。質量は使わないのでそのまま)
void ScaleSource(inout GasGpuCell source, inout GasGpuDerived sourceDerived, inout GasGpuTotals sourceTotals,
                 uint32_t shift) {
    if (shift == 0)
        return;

    sourceTotals.amount = sourceTotals.amount << shift;
    sourceTotals.capacity = sourceTotals.capacity << shift;
    sourceTotals.chemical = sourceTotals.chemical * ((int64_t)1 << shift);
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

    const GasTreeGpuFace layout = g_layout[faceIndex];
    if (!Starts(layout.level))
        return;

    GasGpuFace face = LoadFace(faceIndex);
    if (face.fraction == 0)
        return;

    const bool forward = face.flow > 0;
    const TreeSide donor = PickSide(forward, LeftSide(layout), RightSide(layout));
    const TreeSide receiver = PickSide(forward, RightSide(layout), LeftSide(layout));
    const bool inside = donor.kind == GAS_TREE_GPU_SIDE_CELL && receiver.kind == GAS_TREE_GPU_SIDE_CELL;
    const int64_t sign = donor.kind == GAS_TREE_GPU_SIDE_CELL ? -1 : 1;
    const uint32_t ledgerShift = 3 * layout.level;

    // --- 成分(中身からは風上の小刻みの間にもう出した分を引く。細かい面が何度も引く時。T-0320。出した分は中身を超えない)。
    // 風上の中身と移す塊の和を作り、箱の外との出入りは帳簿へ ---
    GasGpuTotals sourceTotals = GasZeroTotals();
    GasGpuTotals movedTotals = GasZeroTotals();
    const uint32_t speciesCount = SpeciesCount();
    for (uint32_t s = 0; s < speciesCount; ++s) {
        const uint64_t amount = ForcedSideAmount(donor, s);
        uint64_t moved = FaceMoved(faceIndex, s);
        if (donor.kind == GAS_TREE_GPU_SIDE_CELL) {
            const uint64_t total = g_sums.Load<uint64_t>(SumsAmountAddress(donor.index, s));
            const uint64_t content = (amount << 3) - g_carry.Load<uint64_t>(SumsAmountAddress(donor.index, s));
            if (total > content) {
                moved = FxDivU128By64(FxMulU64Full(moved, content), total).quotient;
                g_faces.Store<uint64_t>(FaceMovedAddress(faceIndex, s), moved);
            }
        }

        GasAddSpecies(sourceTotals, s, amount);
        GasAddSpecies(movedTotals, s, moved);
        if (!inside) {
            LedgerAddScaled(GAS_TREE_GPU_LEDGER_AMOUNTS + (s * GAS_TREE_GPU_LEDGER_STRIDE), sign * (int64_t)moved,
                            ledgerShift);
        }
    }

    // --- 移す塊(エネルギー・運動量。面の単位)---
    GasGpuCell source = ForcedSideCell(donor);
    GasGpuDerived sourceDerived = SideDerived(donor);
    ScaleSource(source, sourceDerived, sourceTotals, ContentShift(layout, donor));
    [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
        face.movedMomentum[axis] = FxMulShiftS64(source.momentum[axis], (int64_t)face.fraction, 32);
    }

    const int64_t energy = MovedEnergy(source, sourceTotals, sourceDerived, movedTotals, face.fraction);
    face.movedEnergy = DivideNearest(energy, 1000);
    face.receivedEnergy = DivideNearest(energy - GravityWork(layout, face.flow, movedTotals.mass), 1000);
    g_faces.Store<GasGpuFace>(FaceAddress(faceIndex), face);

    // --- 帳簿(開いた境界の外から入る物は正、出る物は負。エネルギーは重さの仕事〔受けた − 引いた〕も)---
    if (inside) {
        LedgerAddScaled(GAS_TREE_GPU_LEDGER_ENERGY, face.receivedEnergy - face.movedEnergy, ledgerShift);
        return;
    }

    LedgerAddScaled(GAS_TREE_GPU_LEDGER_ENERGY,
                    donor.kind == GAS_TREE_GPU_SIDE_CELL ? -face.movedEnergy : face.receivedEnergy, ledgerShift);
    [unroll] for (uint32_t momentumAxis = 0; momentumAxis < 3; ++momentumAxis) {
        LedgerAddScaled(GAS_TREE_GPU_LEDGER_MOMENTUM + (momentumAxis * GAS_TREE_GPU_LEDGER_STRIDE),
                        sign * face.movedMomentum[momentumAxis], ledgerShift);
    }
}

// 端数つきの足し算を件の欄へ(whole はセルの件、fraction は端数の件の同じ場所)
void AddScaledAmount(uint32_t address, int64_t value, uint32_t shift) {
    uint64_t whole = g_next.Load<uint64_t>(address);
    uint64_t fraction = g_nextFractions.Load<uint64_t>(address);
    AddScaledUnsigned(whole, fraction, value, shift);
    g_next.Store<uint64_t>(address, whole);
    g_nextFractions.Store<uint64_t>(address, fraction);
}

// --- 段 9: 移す(セル。gas_tree.cpp の Transfer・AddParcel。自分の面の出入りを、面の単位から自分の単位へ端数つきで next へ)。
// 出した量を数える欄(LimitOutflow・LimitMovedAmounts の 3 回目の走査)を足し、小刻みを終えるレベルのセルは next を状態へ ---
[numthreads(64, 1, 1)] void GasTreeTransfer(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    const GasTreeGpuPlace place = g_places[index];
    const uint32_t address = CellAddress(index);
    const uint32_t speciesCount = SpeciesCount();
    GasGpuCell cell = g_next.Load<GasGpuCell>(address);
    GasTreeGpuFraction fraction = g_nextFractions.Load<GasTreeGpuFraction>(address);
    uint64_t committedOutflow = g_carry.Load<uint64_t>(SumsAddress(index));
    for (uint32_t entry = place.faceBegin; entry < place.faceEnd; ++entry) {
        const CellFace cellFace = CellFaceAt(entry, place.level);
        if (!cellFace.active)
            continue;

        const GasGpuFace face = LoadFace(cellFace.faceIndex);
        if (face.fraction == 0)
            continue;

        const bool donor = IsDonor(cellFace, face.flow);
        const uint32_t unitShift = 3 - cellFace.shift;
        if (donor && cellFace.gap == 0)
            committedOutflow += face.fraction;

        // --- 成分(自分のスレッドだけが書く件なので、面ごとに足し込む)---
        const int64_t sign = donor ? -1 : 1;
        for (uint32_t s = 0; s < speciesCount; ++s) {
            const uint64_t moved = FaceMoved(cellFace.faceIndex, s);
            if (donor) {
                const uint32_t takenAddress = SumsAmountAddress(index, s);
                g_carry.Store<uint64_t>(takenAddress, g_carry.Load<uint64_t>(takenAddress) + (moved << unitShift));
            }

            AddScaledAmount(CellAmountAddress(index, s), sign * (int64_t)moved, cellFace.shift);
        }

        AddScaled(cell.energy, fraction.energy, donor ? -face.movedEnergy : face.receivedEnergy, cellFace.shift);
        [unroll] for (uint32_t axis = 0; axis < 3; ++axis) {
            AddScaled(cell.momentum[axis], fraction.momentum[axis], sign * face.movedMomentum[axis], cellFace.shift);
        }
    }

    g_next.Store<GasGpuCell>(address, cell);
    g_nextFractions.Store<GasTreeGpuFraction>(address, fraction);
    g_carry.Store<uint64_t>(SumsAddress(index), committedOutflow);
    if (!Ends(place.level))
        return;

    // --- 小刻みを終えるレベルのセルは next を状態へ ---
    g_cells.Store<GasGpuCell>(address, cell);
    g_fractions.Store<GasTreeGpuFraction>(address, fraction);
    for (uint32_t s = 0; s < speciesCount; ++s) {
        const uint32_t amountAddress = CellAmountAddress(index, s);
        g_cells.Store<uint64_t>(amountAddress, g_next.Load<uint64_t>(amountAddress));
        g_fractions.Store<uint64_t>(amountAddress, g_nextFractions.Load<uint64_t>(amountAddress));
    }
}
