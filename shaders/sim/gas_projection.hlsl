// gas_projection.hlsl — 気体の圧力の投影の 1 刻み(本物の音速の陰解法。T-0503・T-0842・T-0845)の、式を作る段・段の重み・当てる段を GPU の Compute で。
// CPU リファレンスは engine/src/sim/gas_projection.cpp の StepGasProjection(fixedCycles・外側の反復 1)と gas_reference.cpp の
// BeginGasPressure・FinishGasPressure・implicit_conduction.cpp の BuildImplicitGrid(段の重み)。式は gas_flow.hlsli・implicit_conduction.hlsli の
// 同じ関数を呼ぶ。毎刻みビット一致を tests/gpu_gas_projection_test.cpp(--t0845)が確かめる。呼ぶ順は engine/src/sim/gpu_gas_projection.cpp:
//   GasDerive(gas_step)→ GasProjectFace(面): K_f と F* → GasProjectCell(セル): C_i・右辺・段 0 の重み
//   → GasProjectLevel(段 1 から、節): 粗い段の容量・面の係数・重み・子の縮約の重み → V サイクル(GpuImplicit::RecordSolve)
//   → GasProjectPressure(セル): 解いた x を導く値の p̃ へ → GasFaceForce・GasApplyForce(gas_step)→ GasProjectFlow(面): F = F* + K_f (x_左 − x_右)
//   → GasOutflow 〜 GasTransfer(gas_step)
// 段の形(節・隣・親子の番号)は CPU の BuildImplicitGrid が作ったものを GpuImplicit に写してあり、系の形が同じ刻みはそのまま使う
// (値だけを毎刻み作り直す。形が変わった刻みは呼ぶ側が写し直す)。気体の箱はどのセルも同じレベルなので、縮約はいつも親のセル(÷ 8)。
// 浮動小数点は使わない(04 §4)。
#define GAS_STEP_EXTRA_CONSTANTS                                                              \
    uint32_t g_pjMaxNodes;    /* GpuImplicit の節の上限(作業場の番地)*/                       \
    uint32_t g_pjMaxLinks;    /* 隣の上限(作業用の係数の番地)*/                               \
    uint32_t g_pjLevelStart;  /* GasProjectLevel: この段の節の始まり(全体の番号)*/            \
    uint32_t g_pjLevelCount;  /* この段の節の数 */                                            \
    uint32_t g_pjPerSoundLow; /* 式の C_i の 2^45 × 10^6 × T_地面 ÷ c̃²(GasPressurePerSound)*/ \
    uint32_t g_pjPerSoundHigh;                                                                \
    uint32_t g_pjStatePerSoundLow; /* E_i の V ÷ c_箱² の同じ値(箱の音速)*/                   \
    uint32_t g_pjStatePerSoundHigh;                                                           \
    uint32_t g_pjLinkLow; /* K(mg/µPa の Q48。GasPressureLinkCoefficient)*/                   \
    uint32_t g_pjLinkHigh;

#include "common/implicit_conduction.hlsli"
#include "sim/gas_step.hlsl"

// GpuImplicit のバッファ(implicit_conduct.hlsl の u2〜u5 と同じもの)と作業用の 128bit の値
RWStructuredBuffer<ImGpuNode> g_pjNodes : register(u6);
RWStructuredBuffer<ImGpuLink> g_pjLinks : register(u7);
RWStructuredBuffer<uint32_t> g_pjLists : register(u8);  // 子の一覧(節の childStart〜childEnd)
RWStructuredBuffer<int64_t> g_pjWork : register(u9);    // 作業場: 値 A × 節・値 B × 節・右辺 × 節
// 作業用(uint64 の 2 語で 128bit。上・下の順): 容量 × 節・対角 × 節・面の係数 × 隣
RWStructuredBuffer<uint64_t> g_pjScratch : register(u10);

// --- 値と番地 ---

uint64_t PjPerSound() {
    return FX_U64(g_pjPerSoundHigh, g_pjPerSoundLow);
}

uint64_t PjStatePerSound() {
    return FX_U64(g_pjStatePerSoundHigh, g_pjStatePerSoundLow);
}

uint64_t PjLink() {
    return FX_U64(g_pjLinkHigh, g_pjLinkLow);
}

uint32_t PjValueA(uint32_t node) {
    return node;
}

uint32_t PjValueB(uint32_t node) {
    return g_pjMaxNodes + node;
}

uint32_t PjRhs(uint32_t node) {
    return (2 * g_pjMaxNodes) + node;
}

FxU128 PjLoadWide(uint32_t word) {
    FxU128 value;
    value.hi = g_pjScratch[word];
    value.lo = g_pjScratch[word + 1];

    return value;
}

void PjStoreWide(uint32_t word, FxU128 value) {
    g_pjScratch[word] = value.hi;
    g_pjScratch[word + 1] = value.lo;
}

uint32_t PjCapacityWord(uint32_t node) {
    return 2 * node;
}

uint32_t PjDiagonalWord(uint32_t node) {
    return (2 * g_pjMaxNodes) + (2 * node);
}

uint32_t PjCoefficientWord(uint32_t link) {
    return (4 * g_pjMaxNodes) + (2 * link);
}

// --- 面の式(gas_reference.cpp の PressureFaceCoefficient・PredictedFlow)---

// 面の係数 K_f(壁の面は 0。開き 0 の面も 0)
uint64_t PjFaceCoefficient(GasFaceSides sides) {
    if (sides.left.kind == SIDE_WALL || sides.right.kind == SIDE_WALL)
        return 0;

    return GfPressureFaceCoefficient(PjLink(), FaceOpen(sides));
}

// 力を当てる前のセル(刻みの初め。開いた境界の外は外の件)
GasGpuCell PjStartSideCell(GasSide side) {
    if (side.kind == SIDE_GHOST)
        return g_ghost.Load<GasGpuCell>(side.index * GasGpuCellStride(g_speciesStride));

    return g_cells.Load<GasGpuCell>(CellAddress(side.index));
}

// 面の予測の流れ F*(mg。左 → 右が正): 中心の流れ − K_f × 縦の面の両側の質量の差の静水圧。一部だけ開いた面は各側の運動量を「開き ÷ 空き」倍
int64_t PjPredictedFlow(GasFaceSides sides, uint64_t link) {
    const int64_t leftMomentum = PjStartSideCell(sides.left).momentum[sides.axis];
    const int64_t rightMomentum = PjStartSideCell(sides.right).momentum[sides.axis];
    int64_t momentumSum = leftMomentum + rightMomentum;
    const uint32_t open = FaceOpen(sides);
    if (open != GAS_GPU_OPEN_FULL) {
        momentumSum = GfScaleTowardZero(leftMomentum, open, SideOpen(sides.left)) +
                      GfScaleTowardZero(rightMomentum, open, SideOpen(sides.right));
    }

    const int64_t central = FxMulShiftS64(momentumSum, (int64_t)GAS_P.centralFlow, 52);
    if (sides.axis != 2 || GAS_P.gravity == 0)
        return central;

    const int64_t hydrostatic = GfHalfHydrostatic(GasFlowParams(), Excess(sides.left) + Excess(sides.right));

    return central - FxMulShiftS64((int64_t)link, hydrostatic, 48);
}

// --- 段 P1: 面の係数 K_f と予測の流れ F*(面)。面の件の flow に F*、fraction に K_f を置く(GasFaceForce は両方をそのまま残す)---
[numthreads(64, 1, 1)] void GasProjectFace(uint3 thread : SV_DispatchThreadID) {
    const uint32_t faceId = GasFaceThread(thread);
    GasFaceSides sides;
    if (!FaceSidesOf(faceId, sides))
        return;

    const uint64_t link = PjFaceCoefficient(sides);
    const int64_t predicted = link != 0 ? PjPredictedFlow(sides, link) : 0;
    g_faces.Store<int64_t>(FaceAddress(faceId) + FACE_FLOW, predicted);
    g_faces.Store<uint64_t>(FaceAddress(faceId) + FACE_FRACTION, link);
}

// --- 段 P2: 節の式と段 0 の重み・右辺(セル。gas_reference.cpp の BeginGasPressure・gas_projection.cpp の MakePressureGrid・PressureRhs・
//     implicit_conduction.cpp の MakeCellLevel)---

// 圧力の式の V の割合(Q16)= セルの空き(下限 1/256)
uint32_t PjVolumeShare(uint32_t index) {
    if (GAS_P.openness == 0)
        return GAS_GPU_OPEN_FULL;

    const uint32_t open = CellOpen(index);

    return open > GF_OPEN_PRESSURE_FLOOR ? open : GF_OPEN_PRESSURE_FLOOR;
}

[numthreads(64, 1, 1)] void GasProjectCell(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    // --- C_i・E_i の係数と p̃(頭打ちなし)---
    const GasGpuDerived derived = g_derived[index];
    const uint32_t share = PjVolumeShare(index);
    uint64_t capacity = GfShareCapacity(GfPressureCapacity(PjPerSound(), derived.temperature), share);
    const uint64_t stateCapacity = GfShareCapacity(GfPressureCapacity(PjStatePerSound(), derived.temperature), share);
    const int64_t deviation = derived.pressure - g_referenceDerived[CellLayer(index)].pressure;
    const int64_t statePressure = FxMulShiftS64(deviation, (int64_t)GAS_P.soundScale, 32);

    // --- 自分の 6 面: 出ていく F* の和・開いた境界の面の K(対角へ)・隣のセルとの面の K ---
    const CellFaceSet faces = CellFaceSetOf(index);
    int64_t divergence = 0;
    uint64_t neighborLinks[6];
    uint32_t neighbors[6];
    FxU128 diagonal = ImWide(0);
    [unroll] for (uint32_t f = 0; f < 6; ++f) {
        neighborLinks[f] = 0;
        neighbors[f] = 0xFFFFFFFFu;
        GasFaceSides sides;
        FaceSidesOf(faces.ids[f], sides);
        const GasGpuFace face = LoadFace(faces.ids[f]);
        const uint64_t link = face.fraction;
        if (link == 0)
            continue;

        divergence += faces.right[f] ? -face.flow : face.flow;
        const GasSide other = PickSide(faces.right[f], sides.left, sides.right);
        if (other.kind != SIDE_CELL) {
            capacity += link;
            continue;
        }

        neighborLinks[f] = link;
        neighbors[f] = other.index;
        diagonal = ImWideAdd(diagonal, ImWide(link));
    }

    // --- 段 0 の節(容量・対角・自分の重み)と隣の重み(行の順は GpuImplicit の隣の一覧のまま。隣のセルで面を探す)---
    diagonal = ImWideAdd(diagonal, ImWide(capacity));
    PjStoreWide(PjCapacityWord(index), ImWide(capacity));
    PjStoreWide(PjDiagonalWord(index), diagonal);
    g_pjNodes[index].selfWeight = ImWeight(ImWide(capacity), diagonal);
    const ImGpuNode node = g_pjNodes[index];
    for (uint32_t k = node.linkStart; k < node.linkEnd; ++k) {
        const uint32_t neighbor = g_pjLinks[k].neighbor;
        uint64_t link = 0;
        [unroll] for (uint32_t f = 0; f < 6; ++f) {
            if (link == 0 && neighbors[f] == neighbor)
                link = neighborLinks[f];
        }

        g_pjLinks[k].weight = ImWeight(ImWide(link), diagonal);
        PjStoreWide(PjCoefficientWord(k), ImWide(link));
    }

    // --- 右辺(値の単位 µPa)=(V p̃ ÷ c_箱² − Σ F* × 2^48)÷ D。初めの近似 = 前の刻みの解(値 A)---
    const int64_t state = GfMulDivSigned(statePressure, stateCapacity, diagonal.lo);
    const int64_t outflow = GfMulDivSigned(divergence, FX_U64(0x10000u, 0u), diagonal.lo);
    g_pjWork[PjRhs(index)] = state - outflow;
    g_pjWork[PjValueB(index)] = g_pjWork[PjValueA(index)];
}

// --- 段 P3: 粗い段(節。implicit_conduction.cpp の Coarsen・FinishLevel。どの節も縮約した親なので子の値は ÷ 8、面の係数はさらに ÷ 2)---
// 行(隣の親)は PJ_ROW_MAX まで(気体の箱の粗い段は 6。呼ぶ側が段の形を写す時に確かめる)。子の隣を 1 回だけ回し、隣の親で行を探して足す
static const uint32_t PJ_ROW_MAX = 6;

[numthreads(64, 1, 1)] void GasProjectLevel(uint32_t thread : SV_DispatchThreadID) {
    if (thread >= g_pjLevelCount)
        return;

    const uint32_t parent = g_pjLevelStart + thread;
    const ImGpuNode node = g_pjNodes[parent];
    const uint32_t rowCount = node.linkEnd - node.linkStart;
    uint32_t rowNeighbors[PJ_ROW_MAX];
    FxU128 rowSums[PJ_ROW_MAX];
    [unroll] for (uint32_t r = 0; r < PJ_ROW_MAX; ++r) {
        rowNeighbors[r] = r < rowCount ? g_pjLinks[node.linkStart + r].neighbor : 0xFFFFFFFFu;
        rowSums[r] = ImWide(0);
    }

    // --- 容量 = 子の容量の和。隣の親との面の係数 = 子の隣のうち、その親の子へ向かうものの和 ---
    FxU128 capacity = ImWide(0);
    for (uint32_t c = node.childStart; c < node.childEnd; ++c) {
        const uint32_t childIndex = g_pjLists[c];
        capacity = ImWideAdd(capacity,
                             ImWideShiftRight(PjLoadWide(PjCapacityWord(childIndex)), IM_ENERGY_BITS_PER_LEVEL));
        const ImGpuNode child = g_pjNodes[childIndex];
        for (uint32_t m = child.linkStart; m < child.linkEnd; ++m) {
            const uint32_t other = g_pjNodes[g_pjLinks[m].neighbor].parent;
            if (other == parent)
                continue;

            const FxU128 coefficient = ImWideShiftRight(PjLoadWide(PjCoefficientWord(m)), IM_ENERGY_BITS_PER_LEVEL);
            [unroll] for (uint32_t r = 0; r < PJ_ROW_MAX; ++r) {
                if (rowNeighbors[r] == other)
                    rowSums[r] = ImWideAdd(rowSums[r], coefficient);
            }
        }
    }

    FxU128 diagonal = capacity;
    [unroll] for (uint32_t r = 0; r < PJ_ROW_MAX; ++r) {
        rowSums[r] = ImWideShiftRight(rowSums[r], 1);
        if (r < rowCount)
            diagonal = ImWideAdd(diagonal, rowSums[r]);
    }

    // --- 重み(自分・隣)と、子の縮約の重み(子の D〔親の単位〕÷ 親の D)---
    PjStoreWide(PjCapacityWord(parent), capacity);
    PjStoreWide(PjDiagonalWord(parent), diagonal);
    g_pjNodes[parent].selfWeight = ImWeight(capacity, diagonal);
    [unroll] for (uint32_t r = 0; r < PJ_ROW_MAX; ++r) {
        if (r < rowCount) {
            PjStoreWide(PjCoefficientWord(node.linkStart + r), rowSums[r]);
            g_pjLinks[node.linkStart + r].weight = ImWeight(rowSums[r], diagonal);
        }
    }

    for (uint32_t c = node.childStart; c < node.childEnd; ++c) {
        const uint32_t child = g_pjLists[c];
        g_pjNodes[child].restrictWeight = ImWeight(
            ImWideShiftRight(PjLoadWide(PjDiagonalWord(child)), IM_ENERGY_BITS_PER_LEVEL), diagonal);
    }
}

    // --- 段 P4: 解いた x(値 A)を導く値の p̃ へ(面の力積が読む。gas_reference.cpp の FinishGasPressure の (1))---
    [numthreads(64, 1, 1)] void GasProjectPressure(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    g_derived[index].scaledPressure = g_pjWork[PjValueA(index)];
}

// --- 段 P5: 面の流れ F = F* + K_f (x_左 − x_右)(開いた境界の外は x = 0。壁・閉じた面は 0)と抑える前の割合
//     (FinishGasPressure の (3)。GasFaceFlow の代わり)---
int64_t PjSidePressure(GasSide side) {
    if (side.kind != SIDE_CELL)
        return 0;

    return g_pjWork[PjValueA(side.index)];
}

[numthreads(64, 1, 1)] void GasProjectFlow(uint3 thread : SV_DispatchThreadID) {
    const uint32_t faceId = GasFaceThread(thread);
    GasFaceSides sides;
    if (!FaceSidesOf(faceId, sides))
        return;

    const GasGpuFace face = LoadFace(faceId);
    const uint64_t link = face.fraction;
    int64_t flow = 0;
    if (link != 0) {
        const int64_t difference = PjSidePressure(sides.left) - PjSidePressure(sides.right);
        flow = face.flow + FxMulShiftS64((int64_t)link, difference, 48);
    }

    uint64_t fraction = 0;
    if (flow != 0) {
        const GasSide donor = PickSide(flow > 0, sides.left, sides.right);
        fraction = GfRawFraction(flow, SideDerived(donor).inverseMass);
    }

    g_faces.Store<int64_t>(FaceAddress(faceId) + FACE_FLOW, flow);
    g_faces.Store<uint64_t>(FaceAddress(faceId) + FACE_FRACTION, fraction);
}
