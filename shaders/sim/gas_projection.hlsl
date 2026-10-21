// gas_projection.hlsl — 気体の圧力の投影の 1 刻み(本物の音速の陰解法。T-0503・T-0842・T-0845)の、式を作る段・段の重み・当てる段を GPU の Compute で。
// CPU リファレンスは engine/src/sim/gas_projection.cpp の StepGasProjection(fixedCycles・外側の反復 1)と gas_reference.cpp の
// BeginGasPressure・FinishGasPressure・implicit_conduction.cpp の BuildImplicitGrid(段の重み)。式は gas_flow.hlsli・implicit_conduction.hlsli の
// 同じ関数を呼ぶ。毎刻みビット一致を tests/gpu_gas_projection_test.cpp(--t0845)が確かめる。呼ぶ順は engine/src/sim/gpu_gas_projection.cpp:
//   GasDerive(gas_step)→ GasProjectFace(面): K_f と F* → GasProjectCell(セル): 釣り合う家族の部品の系の段 0
//   (節 = セル〔C_i〕・面 = 箱の中の面〔K_f〕・開いた境界 = 外の x = 0 を固定した面・右辺の項 = E_i と −Σ F*。gas_projection.cpp の
//   MakeGasEquilibriumSystem と同じ系)→ 部品の段(shaders/sim/equilibrium.hlsl: 粗い段 → V サイクル → 残差の計器。T-0915)
//   → GasProjectPressure(セル): 解いた x を導く値の p̃ へ → GasFaceForce・GasApplyForce(gas_step)→ GasProjectFlow(面): F = F* + K_f (x_左 − x_右)
//   → GasOutflow 〜 GasTransfer(gas_step)
// 段 0 の式の仕上げ(対角・重み・右辺の割り方)は部品の関数(sim/equilibrium_core.hlsli・common/equilibrium.hlsli)。ここに残るのは式の作りだけ。
// 浮動小数点は使わない(04 §4)。
#define GAS_STEP_EXTRA_CONSTANTS                                                              \
    uint32_t g_pjMaxNodes;    /* GpuImplicit の節の上限(作業場の番地)*/                       \
    uint32_t g_pjMaxLinks;    /* 隣の上限(作業用の係数の番地)*/                               \
    uint32_t g_pjPerSoundLow; /* 式の C_i の 2^45 × 10^6 × T_地面 ÷ c̃²(GasPressurePerSound)*/ \
    uint32_t g_pjPerSoundHigh;                                                                \
    uint32_t g_pjStatePerSoundLow; /* E_i の V ÷ c_箱² の同じ値(箱の音速)*/                   \
    uint32_t g_pjStatePerSoundHigh;                                                           \
    uint32_t g_pjLinkLow; /* K(mg/µPa の Q48。GasPressureLinkCoefficient)*/                   \
    uint32_t g_pjLinkHigh;

#include "common/equilibrium.hlsli"
#include "common/implicit_conduction.hlsli"
#include "sim/gas_step.hlsl"

// 釣り合う家族の部品のバッファ(GpuEquilibrium。shaders/sim/equilibrium.hlsl の u0〜u4 と同じもの)
RWStructuredBuffer<ImGpuNode> g_eqNodes : register(u6);
RWStructuredBuffer<ImGpuLink> g_eqLinks : register(u7);
RWStructuredBuffer<uint32_t> g_eqLists : register(u8);     // 子の一覧(この段では読まない。部品の結び付けの並び)
RWStructuredBuffer<int64_t> g_eqWork : register(u9);       // 作業場: 値 A × 節・値 B × 節・右辺 × 節
RWStructuredBuffer<uint64_t> g_eqScratch : register(u10);  // 作業用の 128bit(common/equilibrium.hlsli の EqGpu*Word)

#define EQ_MAX_NODES g_pjMaxNodes
#define EQ_MAX_LINKS g_pjMaxLinks
#include "sim/equilibrium_core.hlsli"

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

// --- 段 P2: 部品の系の段 0(セル。gas_reference.cpp の BeginGasPressure・gas_projection.cpp の MakeGasEquilibriumSystem・
//     equilibrium.cpp の Diagonals・SourceRhs)---

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

    // --- 自分の 6 面: 出ていく F* の和・開いた境界の面の K(外の x = 0 を固定した面。自分だけの係数へ)・隣のセルとの面の K ---
    const CellFaceSet faces = CellFaceSetOf(index);
    int64_t divergence = 0;
    uint64_t boundary = 0;
    FxU128 coefficients[EQ_GPU_FACES_MAX];
    uint32_t neighbors[EQ_GPU_FACES_MAX];
    FX_UNROLL for (uint32_t f = 0; f < EQ_GPU_FACES_MAX; ++f) {
        coefficients[f] = ImWide(0);
        neighbors[f] = EQ_GPU_NO_NEIGHBOR;
        GasFaceSides sides;
        FaceSidesOf(faces.ids[f], sides);
        const GasGpuFace face = LoadFace(faces.ids[f]);
        const uint64_t link = face.fraction;
        if (link == 0)
            continue;

        divergence += faces.right[f] ? -face.flow : face.flow;
        const GasSide other = PickSide(faces.right[f], sides.left, sides.right);
        if (other.kind != SIDE_CELL) {
            boundary += link;
            continue;
        }

        coefficients[f] = ImWide(link);
        neighbors[f] = other.index;
    }

    // --- 部品の段 0(対角 D = C_i + 固定した面 + Σ K_f・重み)と右辺の項 ÷ D(値 × 係数 ÷ D。固定した面の値は 0 なので項も 0)---
    const FxU128 diagonal = EqGpuFinishNode(index, ImWide(capacity + boundary), neighbors, coefficients);
    const int64_t state = EqSourceTerm(EQ_ROUNDING_EXACT, statePressure, ImWide(stateCapacity), diagonal);
    const int64_t outflow = EqSourceTerm(EQ_ROUNDING_EXACT, -divergence, ImWide(FX_U64(0x10000u, 0u)), diagonal);
    EqGpuSetRhs(index, state + outflow);
}

    // --- 段 P4: 解いた x(値 A)を導く値の p̃ へ(面の力積が読む。gas_reference.cpp の FinishGasPressure の (1))---
    [numthreads(64, 1, 1)] void GasProjectPressure(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_cellCount)
        return;

    g_derived[index].scaledPressure = EqGpuValue(index);
}

// --- 段 P5: 面の流れ F = F* + K_f (x_左 − x_右)(開いた境界の外は x = 0。壁・閉じた面は 0)と抑える前の割合
//     (FinishGasPressure の (3)。GasFaceFlow の代わり)---
int64_t PjSidePressure(GasSide side) {
    if (side.kind != SIDE_CELL)
        return 0;

    return EqGpuValue(side.index);
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
