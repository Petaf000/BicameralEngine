// implicit_conduct.hlsl — 細かいレベルの熱の陰解法(方式②。ADR-0019)の Compute の段(T-0117)。CPU リファレンス
// engine/src/sim/implicit_conduction.cpp の StepImplicit(V サイクルを誤差の見込みで止める形)と毎刻みビット一致する。式は common/implicit_conduction.hlsli。
// 呼ぶ順は engine/src/sim/gpu_implicit.cpp の RecordStep:
//   ImBegin → ImStart → (V サイクル: ImSmooth・ImRestrict・ImProlong の段の再帰 → ImConverged → ImCycleEnd)× 上限の回数
//   → ImFlows → (ImMark → ImLimitEnd → ImLimitFaces)× 安全網の上限の回数 → ImApply
// 回数は GPU が決める(V サイクルを止める・安全網を止める)。記録は上限の回数だけ積み、要らない回は述語(u8。SetPredication)で
// Dispatch ごと飛ばす。述語を使わない時(比べる計測)も、段は状態の語を見て空で抜ける。
// 1 スレッド = 1 節・1 セル・1 面。値の 2 本(A・B)は赤黒の色で読み書きを入れ替える(色 0: A → B、色 1: B → A。掃き出し 1 回で A に戻る)。
#include "common/implicit_conduction.hlsli"

// --- 結び付け(gpu_implicit.cpp の ROOT_LAYOUT と同じ順)---
// u0 セル / u1 面 / u2 番号の一覧(セルの面・節の子)/ u3 節 / u4 隣 /
// u5 64bit の作業場 [値 A × 節][値 B × 節][右辺 × 節][刻みの初めの温度 × セル][流れ × 面][陽解法の流れ × 面] /
// u6 状態の語 [IM_STATE_*][安全網の印 × セル] / u7 64bit の状態 [最低の温度・最高の温度・範囲を超えた最大(mK)] /
// u8 述語 [V サイクルを止めた・安全網を止めた](0 でなければ以降の回の Dispatch を飛ばす)
RWStructuredBuffer<ImGpuCell> g_cells : register(u0);
RWStructuredBuffer<ImGpuFace> g_faces : register(u1);
RWStructuredBuffer<uint32_t> g_lists : register(u2);
RWStructuredBuffer<ImGpuNode> g_nodes : register(u3);
RWStructuredBuffer<ImGpuLink> g_links : register(u4);
RWStructuredBuffer<int64_t> g_work : register(u5);
RWStructuredBuffer<uint32_t> g_state : register(u6);
RWStructuredBuffer<int64_t> g_wide : register(u7);
RWStructuredBuffer<uint64_t> g_predicate : register(u8);

cbuffer ImConstants : register(b0) {
    uint32_t g_nodeTotal;
    uint32_t g_cellCount;
    uint32_t g_faceCount;
    uint32_t g_levelOffset;     // この段の節の始まり(全体の番号)
    uint32_t g_levelCount;      // この段の節の数
    uint32_t g_color;           // ImSmooth の色
    uint32_t g_tolerance;       // mK(0 なら回数を固定)
    uint32_t g_slack;           // 安全網の余裕(mK)
    int32_t g_correctionScale;  // 直しの倍率(Q8)
};

static const uint32_t IM_THREADS = 64;

// 状態の語
static const uint32_t IM_STATE_DONE = 0;           // V サイクルを止めた
static const uint32_t IM_STATE_NOT_CONVERGED = 1;  // この回の判定で誤差の見込みを超えた節があった
static const uint32_t IM_STATE_CYCLES = 2;         // 回した V サイクルの数
static const uint32_t IM_STATE_LIMIT_DONE = 3;     // 安全網を止めた
static const uint32_t IM_STATE_CHANGED = 4;        // この回の安全網で新しく印を付けた
static const uint32_t IM_STATE_LIMIT_ROUNDS = 5;   // 安全網を繰り返した回数
static const uint32_t IM_STATE_LIMITED_CELLS = 6;  // 安全網で陽解法に戻したセルの数
static const uint32_t IM_STATE_WORDS = 8;

static const uint32_t IM_WIDE_LOWEST = 0;
static const uint32_t IM_WIDE_HIGHEST = 1;
static const uint32_t IM_WIDE_WORST_EXCESS = 2;

// --- 作業場の番地 ---
uint32_t ValueA(uint32_t node) {
    return node;
}
uint32_t ValueB(uint32_t node) {
    return g_nodeTotal + node;
}
uint32_t Rhs(uint32_t node) {
    return (2 * g_nodeTotal) + node;
}
uint32_t Start(uint32_t cell) {
    return (3 * g_nodeTotal) + cell;
}
uint32_t Flow(uint32_t face) {
    return (3 * g_nodeTotal) + g_cellCount + face;
}
uint32_t ExplicitFlow(uint32_t face) {
    return (3 * g_nodeTotal) + g_cellCount + g_faceCount + face;
}

FxU128 FaceCoefficient(ImGpuFace face) {
    FxU128 coefficient = {face.coefficientHigh, face.coefficientLow};
    return coefficient;
}

// 右辺 + Σ 隣の重み × 値(values: 0 なら A、1 なら B)
int64_t Relax(uint32_t node, uint32_t values) {
    const ImGpuNode entry = g_nodes[node];
    int64_t sum = g_work[Rhs(node)];
    for (uint32_t k = entry.linkStart; k < entry.linkEnd; ++k) {
        const ImGpuLink link = g_links[k];
        const int64_t value = g_work[values == 0 ? ValueA(link.neighbor) : ValueB(link.neighbor)];
        sum += ImMulWeight(link.weight, value);
    }

    return sum;
}

bool CyclesDone() {
    return g_state[IM_STATE_DONE] != 0;
}

// --- 刻みの初め ---

// 状態を初めに戻す(1 スレッド)
[numthreads(1, 1, 1)] void ImBegin() {
    for (uint32_t i = 0; i < IM_STATE_WORDS; ++i)
        g_state[i] = 0;

    g_wide[IM_WIDE_LOWEST] = IM_INT64_MAX;
    g_wide[IM_WIDE_HIGHEST] = IM_INT64_MIN;
    g_wide[IM_WIDE_WORST_EXCESS] = 0;
    g_predicate[0] = 0;
    g_predicate[1] = 0;
}

    // 温度・右辺・初めの近似(= 刻みの初めの温度)と、全部のセルの温度の範囲
    [numthreads(IM_THREADS, 1, 1)] void ImStart(uint32_t cell : SV_DispatchThreadID) {
    if (cell >= g_cellCount)
        return;

    const ImGpuCell entry = g_cells[cell];
    const int64_t start = ImTemperature(entry.energy, entry.heatCapacity);
    g_work[Start(cell)] = start;
    g_work[ValueA(cell)] = start;
    g_work[ValueB(cell)] = start;
    g_work[Rhs(cell)] = ImMulWeight(g_nodes[cell].selfWeight, start);
    g_state[IM_STATE_WORDS + cell] = 0;

    InterlockedMin(g_wide[IM_WIDE_LOWEST], start);
    InterlockedMax(g_wide[IM_WIDE_HIGHEST], start);
}

// --- V サイクルの段 ---

// 赤黒の掃き出しの 1 色: 色が合う節は掃き出しの初めの値から計算し、ほかは写す(同じ色の隣も初めの値を読む。順に依存しない)
[numthreads(IM_THREADS, 1, 1)] void ImSmooth(uint32_t local : SV_DispatchThreadID) {
    if (local >= g_levelCount || CyclesDone())
        return;

    const uint32_t node = g_levelOffset + local;
    const uint32_t from = g_color;
    const int64_t current = g_work[from == 0 ? ValueA(node) : ValueB(node)];
    const int64_t next = g_nodes[node].color == g_color ? Relax(node, from) : current;
    g_work[from == 0 ? ValueB(node) : ValueA(node)] = next;
}

    // 残差を親へ(自分の D で重み付けた和 ÷ 親の D)。親の直しを 0 から。1 スレッド = 親の段(この段)の 1 節。子は 1 つ細かい段
    [numthreads(IM_THREADS, 1, 1)] void ImRestrict(uint32_t local : SV_DispatchThreadID) {
    if (local >= g_levelCount || CyclesDone())
        return;

    const uint32_t parent = g_levelOffset + local;
    const ImGpuNode entry = g_nodes[parent];
    int64_t sum = 0;
    for (uint32_t k = entry.childStart; k < entry.childEnd; ++k) {
        const uint32_t child = g_lists[k];
        const int64_t residual = Relax(child, 0) - g_work[ValueA(child)];
        sum += FxMulShiftS64(g_nodes[child].restrictWeight, residual, IM_WEIGHT_SHIFT);
    }

    g_work[Rhs(parent)] = sum;
    g_work[ValueA(parent)] = 0;
    g_work[ValueB(parent)] = 0;
}

// 親の直しを子へそのまま(倍率を掛けて)足す
[numthreads(IM_THREADS, 1, 1)] void ImProlong(uint32_t local : SV_DispatchThreadID) {
    if (local >= g_levelCount || CyclesDone())
        return;

    const uint32_t node = g_levelOffset + local;
    const int64_t correction = g_work[ValueA(g_nodes[node].parent)];
    g_work[ValueA(node)] += FxMulShiftS64((int64_t)g_correctionScale, correction, IM_CORRECTION_SCALE_SHIFT);
}

    // 新しい温度の誤差の見込み(残差 × D/C)が tolerance を超えるセルがあるか(ADR-0019 の 3)
    [numthreads(IM_THREADS, 1, 1)] void ImConverged(uint32_t cell : SV_DispatchThreadID) {
    if (cell >= g_cellCount || CyclesDone())
        return;

    const int64_t residual = Relax(cell, 0) - g_work[ValueA(cell)];
    if (ImExceedsTolerance(residual, g_nodes[cell].selfWeight, g_tolerance))
        InterlockedOr(g_state[IM_STATE_NOT_CONVERGED], 1u);
}

// V サイクル 1 回の終わり(1 スレッド): 数えて、収束していれば止める
[numthreads(1, 1, 1)] void ImCycleEnd() {
    if (CyclesDone())
        return;

    g_state[IM_STATE_CYCLES] += 1;
    if (g_tolerance != 0 && g_state[IM_STATE_NOT_CONVERGED] == 0) {
        g_state[IM_STATE_DONE] = 1;
        g_predicate[0] = 1;
    }

    g_state[IM_STATE_NOT_CONVERGED] = 0;
}

    // --- 面の流れと安全網 ---

    // 近似解からの流れと、刻みの初めの温度からの陽解法の流れ(安全網が使う)
    [numthreads(IM_THREADS, 1, 1)] void ImFlows(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_faceCount)
        return;

    const ImGpuFace face = g_faces[index];
    const FxU128 coefficient = FaceCoefficient(face);
    g_work[Flow(index)] = ImFaceFlow(coefficient, g_work[ValueA(face.fine)] - g_work[ValueA(face.coarse)]);

    const FxU128 limited = ImExplicitCoefficient(coefficient, g_cells[face.fine].heatCapacity,
                                                 g_cells[face.coarse].heatCapacity, face.gap);
    g_work[ExplicitFlow(index)] = ImFaceFlow(limited, g_work[Start(face.fine)] - g_work[Start(face.coarse)]);
}

// 流れを足した後のエネルギー(粗い側は整数部だけ。範囲の判定用。面の番号の順に頭打ちで足す = CPU の EnergiesAfter)
int64_t EnergyAfter(uint32_t cell) {
    const ImGpuCell entry = g_cells[cell];
    int64_t energy = entry.energy;
    for (uint32_t k = entry.faceStart; k < entry.faceEnd; ++k) {
        const uint32_t item = g_lists[k];
        const uint32_t index = item >> 1;
        const int64_t flow = g_work[Flow(index)];
        if ((item & 1u) == 0)
            energy = ImAddClamped(energy, -flow);
        else
            energy = ImAddClamped(energy, ImShiftRightSigned(flow, IM_ENERGY_BITS_PER_LEVEL * g_faces[index].gap));
    }

    return energy;
}

// 刻みの初めの全部のセルの温度の範囲(上は 1 mK の切り上げ)の外に出るセルに印を付ける。最初の回は範囲を超えた最大も記録する
[numthreads(IM_THREADS, 1, 1)] void ImMark(uint32_t cell : SV_DispatchThreadID) {
    if (cell >= g_cellCount || g_state[IM_STATE_LIMIT_DONE] != 0)
        return;

    const uint64_t capacity = g_cells[cell].heatCapacity;
    const int64_t lowest = ImEnergyFor(capacity, g_wide[IM_WIDE_LOWEST] >> IM_TEMPERATURE_SHIFT);
    const int64_t highest = ImEnergyFor(capacity, (g_wide[IM_WIDE_HIGHEST] >> IM_TEMPERATURE_SHIFT) + 1);
    const int64_t over = ImOverrun(EnergyAfter(cell), lowest, highest);
    if (g_state[IM_STATE_LIMIT_ROUNDS] == 0)
        InterlockedMax(g_wide[IM_WIDE_WORST_EXCESS], ImExcessMillikelvin(over, capacity));

    if (g_state[IM_STATE_WORDS + cell] != 0 || over <= ImEnergyFor(capacity, (int64_t)g_slack))
        return;

    g_state[IM_STATE_WORDS + cell] = 1;
    InterlockedOr(g_state[IM_STATE_CHANGED], 1u);
    InterlockedAdd(g_state[IM_STATE_LIMITED_CELLS], 1u);
}

    // 安全網 1 回の終わり(1 スレッド): 新しい印が無ければ止める
    [numthreads(1, 1, 1)] void ImLimitEnd() {
    if (g_state[IM_STATE_LIMIT_DONE] != 0)
        return;

    if (g_state[IM_STATE_CHANGED] == 0) {
        g_state[IM_STATE_LIMIT_DONE] = 1;
        g_predicate[1] = 1;
    } else
        g_state[IM_STATE_LIMIT_ROUNDS] += 1;

    g_state[IM_STATE_CHANGED] = 0;
}

// 印の付いたセルの面を陽解法の流れに戻す
[numthreads(IM_THREADS, 1, 1)] void ImLimitFaces(uint32_t index : SV_DispatchThreadID) {
    if (index >= g_faceCount || g_state[IM_STATE_LIMIT_DONE] != 0)
        return;

    const ImGpuFace face = g_faces[index];
    if (g_state[IM_STATE_WORDS + face.fine] != 0 || g_state[IM_STATE_WORDS + face.coarse] != 0)
        g_work[Flow(index)] = g_work[ExplicitFlow(index)];
}

    // 流れを両側に(同じレベル: 逆向きに同じ値 / 違うレベル: 粗い側は整数部 + 端数。ADR-0017)。面の番号の順に足す = CPU の ApplyFlows
    [numthreads(IM_THREADS, 1, 1)] void ImApply(uint32_t cell : SV_DispatchThreadID) {
    if (cell >= g_cellCount)
        return;

    ImGpuCell entry = g_cells[cell];
    MrEnergyDelta sum;
    sum.whole = entry.energy;
    sum.fraction = entry.fraction;
    for (uint32_t k = entry.faceStart; k < entry.faceEnd; ++k) {
        const uint32_t item = g_lists[k];
        const uint32_t index = item >> 1;
        const int64_t flow = g_work[Flow(index)];
        const uint32_t gap = g_faces[index].gap;
        const bool coarseSide = (item & 1u) != 0;
        if (gap == 0) {
            sum.whole += coarseSide ? flow : -flow;
            continue;
        }

        const MrCrossTransfer transfer = MrSplitCrossFlow(flow, gap, true);
        if (coarseSide)
            sum = MrAddEnergyDelta(sum, transfer.coarseDelta);
        else
            sum.whole += transfer.fineDelta;
    }

    entry.energy = sum.whole;
    entry.fraction = sum.fraction;
    g_cells[cell] = entry;
}
