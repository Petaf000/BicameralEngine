// implicit_conduct.hlsl — 細かいレベルの熱の陰解法(方式②。ADR-0019)の Compute の段(T-0117)。CPU リファレンス
// engine/src/sim/implicit_conduction.cpp の StepImplicit(V サイクルを誤差の見込みで止める形)と毎刻みビット一致する。式は common/implicit_conduction.hlsli。
// 呼ぶ順は engine/src/sim/gpu_implicit.cpp の RecordStep:
//   ImPlanLevels → ImPlanArgs(段の形から長い行の節の一覧・ImTail の境・間接の Dispatch の引数を作る。T-0136)→ ImBegin → ImStart → (V サイクル: ImSmooth・ImRestrict・ImProlong の段の再帰〔小さい段から下は ImTail の 1 グループ〕
//   → ImConverged → ImCycleEnd)× 上限の回数
//   → ImFlows → (ImMark → ImLimitEnd → ImLimitFaces)× 安全網の上限の回数 → ImApply
// 回数は GPU が決める(V サイクルを止める・安全網を止める)。記録は上限の回数だけ積み、要らない回は述語(u8。SetPredication)で
// Dispatch ごと飛ばす。述語を使わない時(比べる計測)も、段は状態の語を見て空で抜ける。
// 1 スレッド = 1 節・1 セル・1 面。値の 2 本(A・B)は赤黒の色で読み書きを入れ替える(色 0: A → B、色 1: B → A。掃き出し 1 回で A に戻る)。
// 隣を足す段(ImSmooth・ImRestrict・ImConverged)は、段の節を 1 スレッド = 1 節で回し(長い行の節は飛ばす)、その後ろのグループで
// 長い行の節(隣が多い節。違うレベルの面の粗い側・粗い段)を 1 グループ = 1 節で、隣をグループのスレッドで分けて足す(T-0120)。
// 整数の和なので足す順によらず同じ値(04 R1)。段の形・長い行の節の一覧・Dispatch の大きさは GPU のバッファ(u9・u10)から読む(T-0136)。
// 定数の段(g_depth)は記録の上限(g_dispatchLevels)まで積み、その段が無い・下りが止まった段より下なら Dispatch の引数が 0 になる。
#include "common/implicit_conduction.hlsli"

// --- 結び付け(gpu_implicit.cpp の ROOT_LAYOUT と同じ順)---
// u0 セル / u1 面 / u2 番号の一覧(セルの面・節の子)/ u3 節 / u4 隣 /
// u5 64bit の作業場 [値 A × 節][値 B × 節][右辺 × 節][刻みの初めの温度 × セル][流れ × 面][陽解法の流れ × 面] /
// u6 状態の語 [IM_STATE_*][安全網の印 × セル] / u7 64bit の状態 [最低の温度・最高の温度・範囲を超えた最大(mK)] /
// u8 述語 [V サイクルを止めた・安全網を止めた](0 でなければ以降の回の Dispatch を飛ばす)/
// u9 計画(段の形・段の表・節の印・長い行の節の一覧。番地は common/implicit_conduction.hlsli の IM_PLAN_*)/
// u10 間接の Dispatch の引数(IM_SLOT_*。ImPlanArgs だけが書く。V サイクルの間は INDIRECT_ARGUMENT)
// 作業場の番地は上限(g_maxNodes・g_maxCells・g_maxFaces)で決める。数は計画から
RWStructuredBuffer<ImGpuCell> g_cells : register(u0);
RWStructuredBuffer<ImGpuFace> g_faces : register(u1);
RWStructuredBuffer<uint32_t> g_lists : register(u2);
RWStructuredBuffer<ImGpuNode> g_nodes : register(u3);
RWStructuredBuffer<ImGpuLink> g_links : register(u4);
RWStructuredBuffer<int64_t> g_work : register(u5);
RWStructuredBuffer<uint32_t> g_state : register(u6);
RWStructuredBuffer<int64_t> g_wide : register(u7);
RWStructuredBuffer<uint64_t> g_predicate : register(u8);
RWStructuredBuffer<uint32_t> g_plan : register(u9);
RWStructuredBuffer<uint32_t> g_args : register(u10);

cbuffer ImConstants : register(b0) {
    uint32_t g_maxNodes;        // 節の上限(作業場の番地)
    uint32_t g_maxCells;        // セルの上限
    uint32_t g_maxFaces;        // 面の上限(番号の一覧の子の一覧はこの 2 倍の後ろ)
    uint32_t g_depth;           // この段(IM_TERMINAL_DEPTH なら計画の下りが止まる段)
    uint32_t g_dispatchLevels;  // 段ごとの Dispatch を積んだ段の数(これより深い段は ImTail に任せる)
    uint32_t g_color;           // ImSmooth の色
    uint32_t g_tolerance;       // mK(0 なら回数を固定)
    uint32_t g_slack;           // 安全網の余裕(mK)
    int32_t g_correctionScale;  // 直しの倍率(Q8)
    uint32_t g_tailMaxNodes;    // ImTail の境(GpuImplicitTuning)
    uint32_t g_tailMaxLinks;
    uint32_t g_coarsestTailMaxNodes;
    uint32_t g_maxLevels;  // 段の上限(ImPlanLevels のグループの数)
    uint32_t g_unused0;
    uint32_t g_unused1;
    uint32_t g_sweeps;  // 掃き出しの回数: 前 | 後 << 8 | 最も粗い段 << 16
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
    return g_maxNodes + node;
}
uint32_t Rhs(uint32_t node) {
    return (2 * g_maxNodes) + node;
}
uint32_t Start(uint32_t cell) {
    return (3 * g_maxNodes) + cell;
}
uint32_t Flow(uint32_t face) {
    return (3 * g_maxNodes) + g_maxCells + face;
}
uint32_t ExplicitFlow(uint32_t face) {
    return (3 * g_maxNodes) + g_maxCells + g_maxFaces + face;
}

// --- 計画(u9。T-0136)---
uint32_t LevelCount() {
    return g_plan[IM_PLAN_LEVEL_COUNT];
}
uint32_t NodeOffset(uint32_t depth) {
    return g_plan[IM_PLAN_DEPTH_BASE + (IM_PLAN_NODE_OFFSET * IM_PLAN_DEPTH_STRIDE) + depth];
}
uint32_t NodeCount(uint32_t depth) {
    return g_plan[IM_PLAN_DEPTH_BASE + (IM_PLAN_NODE_COUNT * IM_PLAN_DEPTH_STRIDE) + depth];
}
uint32_t CellCount() {
    return NodeCount(0);
}
uint32_t FaceCount() {
    return g_plan[IM_PLAN_FACES];
}
uint32_t LevelWord(uint32_t depth, uint32_t field) {
    return IM_PLAN_LEVELS_BASE + (IM_PLAN_LEVEL_WORDS * depth) + field;
}
uint32_t FlagWord(uint32_t node) {
    return IM_PLAN_FLAGS_BASE + node;
}
// 段 depth の長い行の節の一覧(kind: 0・1 = 掃き出しの色、2 = 縮約)
uint32_t LongListWord(uint32_t depth, uint32_t kind) {
    return IM_PLAN_FLAGS_BASE + g_maxNodes + (3 * NodeOffset(depth)) + (kind * NodeCount(depth));
}
// 子の一覧の k 番(節の childStart は面の一覧〔面の数 × 2〕の後ろからの番号。番号の一覧には面の上限 × 2 の後ろに置く)
uint32_t ChildAt(uint32_t k) {
    return g_lists[(2 * g_maxFaces) + k - (2 * FaceCount())];
}
uint32_t ShortGroups(uint32_t count) {
    return (count + IM_THREADS - 1) / IM_THREADS;
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

// --- 隣が多い節をグループで足す(T-0120)---
groupshared int64_t gs_partial[IM_THREADS];

// グループの全部のスレッドの値の和(全部のスレッドが呼ぶ。全部に同じ値を返す)
int64_t GroupSum(int64_t value, uint32_t lane) {
    gs_partial[lane] = value;
    GroupMemoryBarrierWithGroupSync();
    for (uint32_t width = IM_THREADS / 2; width > 0; width >>= 1) {
        if (lane < width)
            gs_partial[lane] += gs_partial[lane + width];

        GroupMemoryBarrierWithGroupSync();
    }

    const int64_t total = gs_partial[0];
    GroupMemoryBarrierWithGroupSync();

    return total;
}

// Relax をグループで: 隣をスレッドで分けて足し、和を全部のスレッドに返す
int64_t RelaxGroup(uint32_t node, uint32_t values, uint32_t lane) {
    const ImGpuNode entry = g_nodes[node];
    int64_t partial = 0;
    for (uint32_t k = entry.linkStart + lane; k < entry.linkEnd; k += IM_THREADS) {
        const ImGpuLink link = g_links[k];
        const int64_t value = g_work[values == 0 ? ValueA(link.neighbor) : ValueB(link.neighbor)];
        partial += ImMulWeight(link.weight, value);
    }

    return g_work[Rhs(node)] + GroupSum(partial, lane);
}

static const uint32_t IM_KIND_RESTRICT = 2;
static const uint32_t IM_KIND_CONVERGED = 3;

// 段 depth のこのスレッドの節。前のグループは 1 スレッド = 1 節(長い行の節は飛ばす)、後ろのグループは長い行の節 1 つ(isLong = true。
// グループの中で同じ)。kind: 0・1 = 掃き出しの色(その色の長い節)、2 = 縮約(子の隣が多い親)、3 = 止める判定(色 0 → 色 1 の長い節)
bool OrderedNode(uint32_t depth, uint32_t kind, uint32_t group, uint32_t lane, out uint32_t node, out bool isLong) {
    const uint32_t count = NodeCount(depth);
    const uint32_t shortGroups = ShortGroups(count);
    isLong = group >= shortGroups;
    node = 0;
    if (isLong) {
        uint32_t index = group - shortGroups;
        uint32_t list = kind == IM_KIND_CONVERGED ? 0 : kind;
        if (kind == IM_KIND_CONVERGED && index >= g_plan[LevelWord(depth, 0)]) {
            index -= g_plan[LevelWord(depth, 0)];
            list = 1;
        }

        if (index >= g_plan[LevelWord(depth, list)])
            return false;

        node = g_plan[LongListWord(depth, list) + index];
        return true;
    }

    const uint32_t local = (group * IM_THREADS) + lane;
    if (local >= count)
        return false;

    node = NodeOffset(depth) + local;
    const uint32_t flags = g_plan[FlagWord(node)];
    if (kind == IM_KIND_RESTRICT)
        return (flags & 2u) == 0;

    const bool longRow = (flags & 1u) != 0 && (kind == IM_KIND_CONVERGED || g_nodes[node].color == kind);
    return !longRow;
}

// 定数の段(IM_TERMINAL_DEPTH なら下りが止まる段)
uint32_t ConstantDepth() {
    return g_depth == IM_TERMINAL_DEPTH ? g_plan[IM_PLAN_TERMINAL] : g_depth;
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
    if (cell >= CellCount())
        return;

    const ImGpuCell entry = g_cells[cell];
    const int64_t start = entry.startTemperature >= 0 ? entry.startTemperature
                                                      : ImTemperature(entry.energy, entry.heatCapacity);
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
[numthreads(IM_THREADS, 1, 1)] void ImSmooth(uint32_t3 group : SV_GroupID, uint32_t lane : SV_GroupIndex) {
    uint32_t node;
    bool isLong;
    if (CyclesDone() || !OrderedNode(ConstantDepth(), g_color, group.x, lane, node, isLong))
        return;

    const uint32_t from = g_color;
    const int64_t current = g_work[from == 0 ? ValueA(node) : ValueB(node)];
    const bool update = g_nodes[node].color == g_color;
    int64_t next = current;
    if (isLong) {
        const int64_t relaxed = RelaxGroup(node, from, lane);
        if (lane != 0)
            return;

        next = update ? relaxed : current;
    } else if (update)
        next = Relax(node, from);

    g_work[from == 0 ? ValueB(node) : ValueA(node)] = next;
}

    // 残差を親へ(自分の D で重み付けた和 ÷ 親の D)。親の直しを 0 から。1 スレッド = 親の段(この段)の 1 節。子は 1 つ細かい段
    [numthreads(IM_THREADS, 1, 1)] void ImRestrict(uint32_t3 group : SV_GroupID, uint32_t lane : SV_GroupIndex) {
    uint32_t parent;
    bool isLong;
    if (CyclesDone() || !OrderedNode(g_depth, IM_KIND_RESTRICT, group.x, lane, parent, isLong))
        return;

    const ImGpuNode entry = g_nodes[parent];
    int64_t sum = 0;
    for (uint32_t k = entry.childStart; k < entry.childEnd; ++k) {
        const uint32_t child = ChildAt(k);
        int64_t relaxed;
        if (isLong)
            relaxed = RelaxGroup(child, 0, lane);  // グループの中で同じ分岐(バリアを含む)
        else
            relaxed = Relax(child, 0);

        const int64_t residual = relaxed - g_work[ValueA(child)];
        sum += FxMulShiftS64(g_nodes[child].restrictWeight, residual, IM_WEIGHT_SHIFT);
    }

    if (lane != 0 && isLong)
        return;

    g_work[Rhs(parent)] = sum;
    g_work[ValueA(parent)] = 0;
    g_work[ValueB(parent)] = 0;
}

// 親の直しを子へそのまま(倍率を掛けて)足す
[numthreads(IM_THREADS, 1, 1)] void ImProlong(uint32_t local : SV_DispatchThreadID) {
    if (local >= NodeCount(g_depth) || CyclesDone())
        return;

    const uint32_t node = NodeOffset(g_depth) + local;
    const int64_t correction = g_work[ValueA(g_nodes[node].parent)];
    g_work[ValueA(node)] += FxMulShiftS64((int64_t)g_correctionScale, correction, IM_CORRECTION_SCALE_SHIFT);
}

// --- 小さい段から下の V サイクルを 1 グループで(T-0120)---
// 節の数が少ない段(g_tailDepth から最も粗い段まで)は、段ごとの Dispatch の待ちが計算より重い。1 グループで段の再帰と同じ順に回し、
// 段と段の間はグループのバリアにする(同じグループの書き込みはバリアの後に見える)。式と足す順は ImSmooth・ImRestrict・ImProlong と同じ。
static const uint32_t IM_TAIL_THREADS = 1024;

void TailSmooth(uint32_t depth, uint32_t sweeps, uint32_t lane) {
    const uint32_t begin = NodeOffset(depth);
    const uint32_t end = begin + NodeCount(depth);
    for (uint32_t sweep = 0; sweep < sweeps; ++sweep) {
        for (uint32_t color = 0; color < 2; ++color) {
            for (uint32_t node = begin + lane; node < end; node += IM_TAIL_THREADS) {
                const int64_t current = g_work[color == 0 ? ValueA(node) : ValueB(node)];
                int64_t next = current;
                if (g_nodes[node].color == color)
                    next = Relax(node, color);

                g_work[color == 0 ? ValueB(node) : ValueA(node)] = next;
            }

            DeviceMemoryBarrierWithGroupSync();
        }
    }
}

void TailRestrict(uint32_t depth, uint32_t lane) {
    const uint32_t end = NodeOffset(depth) + NodeCount(depth);
    for (uint32_t parent = NodeOffset(depth) + lane; parent < end; parent += IM_TAIL_THREADS) {
        const ImGpuNode entry = g_nodes[parent];
        int64_t sum = 0;
        for (uint32_t k = entry.childStart; k < entry.childEnd; ++k) {
            const uint32_t child = ChildAt(k);
            const int64_t residual = Relax(child, 0) - g_work[ValueA(child)];
            sum += FxMulShiftS64(g_nodes[child].restrictWeight, residual, IM_WEIGHT_SHIFT);
        }

        g_work[Rhs(parent)] = sum;
        g_work[ValueA(parent)] = 0;
        g_work[ValueB(parent)] = 0;
    }

    DeviceMemoryBarrierWithGroupSync();
}

void TailProlong(uint32_t depth, uint32_t lane) {
    const uint32_t end = NodeOffset(depth) + NodeCount(depth);
    for (uint32_t node = NodeOffset(depth) + lane; node < end; node += IM_TAIL_THREADS) {
        const int64_t correction = g_work[ValueA(g_nodes[node].parent)];
        g_work[ValueA(node)] += FxMulShiftS64((int64_t)g_correctionScale, correction, IM_CORRECTION_SCALE_SHIFT);
    }

    DeviceMemoryBarrierWithGroupSync();
}

// CPU の VCycle(計画の IM_PLAN_TAIL の段)と同じ順: 前の掃き出し → 縮約 → (下の段)→ 最も粗い段 → 直し → 後の掃き出し
[numthreads(IM_TAIL_THREADS, 1, 1)] void ImTail(uint32_t lane : SV_GroupIndex) {
    if (CyclesDone())
        return;

    const uint32_t preSmooth = g_sweeps & 0xFFu;
    const uint32_t postSmooth = (g_sweeps >> 8) & 0xFFu;
    const uint32_t coarsestSweeps = g_sweeps >> 16;
    const uint32_t last = LevelCount() - 1;
    const uint32_t tailDepth = g_plan[IM_PLAN_TAIL];
    for (uint32_t down = tailDepth; down < last; ++down) {
        TailSmooth(down, preSmooth, lane);
        TailRestrict(down + 1, lane);
    }

    TailSmooth(last, coarsestSweeps, lane);
    for (uint32_t up = last; up > tailDepth; --up) {
        TailProlong(up - 1, lane);
        TailSmooth(up - 1, postSmooth, lane);
    }
}

    // 新しい温度の誤差の見込み(残差 × D/C)が tolerance を超えるセルがあるか(ADR-0019 の 3)
    [numthreads(IM_THREADS, 1, 1)] void ImConverged(uint32_t3 group : SV_GroupID, uint32_t lane : SV_GroupIndex) {
    uint32_t cell;
    bool isLong;
    if (CyclesDone() || !OrderedNode(0, IM_KIND_CONVERGED, group.x, lane, cell, isLong))
        return;

    int64_t relaxed;
    if (isLong)
        relaxed = RelaxGroup(cell, 0, lane);
    else
        relaxed = Relax(cell, 0);

    if (lane != 0 && isLong)
        return;

    const int64_t residual = relaxed - g_work[ValueA(cell)];
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
    if (index >= FaceCount())
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
    if (cell >= CellCount() || g_state[IM_STATE_LIMIT_DONE] != 0)
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
    if (index >= FaceCount() || g_state[IM_STATE_LIMIT_DONE] != 0)
        return;

    const ImGpuFace face = g_faces[index];
    if (g_state[IM_STATE_WORDS + face.fine] != 0 || g_state[IM_STATE_WORDS + face.coarse] != 0)
        g_work[Flow(index)] = g_work[ExplicitFlow(index)];
}

    // 流れを両側に(同じレベル: 逆向きに同じ値 / 違うレベル: 粗い側は整数部 + 端数。ADR-0017)。面の番号の順に足す = CPU の ApplyFlows
    [numthreads(IM_THREADS, 1, 1)] void ImApply(uint32_t cell : SV_DispatchThreadID) {
    if (cell >= CellCount())
        return;

    ImGpuCell entry = g_cells[cell];
    MrEnergyDelta sum;
    sum.whole = entry.energy;
    sum.fraction = entry.fraction;
    for (uint32_t k = entry.faceStart; k < entry.faceEnd; ++k) {
        const uint32_t item = g_lists[k];
        const uint32_t index = item >> 1;
        const int64_t flow = g_work[Flow(index)];
        const ImGpuFace face = g_faces[index];
        const uint32_t gap = face.gap;
        const bool coarseSide = (item & 1u) != 0;
        if (gap == 0) {
            sum.whole += coarseSide ? flow : -flow;
            continue;
        }

        const MrCrossTransfer transfer = MrSplitCrossFlow(flow, gap, face.coarseFraction != 0);
        if (coarseSide)
            sum = MrAddEnergyDelta(sum, transfer.coarseDelta);
        else
            sum.whole += transfer.fineDelta;
    }

    entry.energy = sum.whole;
    entry.fraction = sum.fraction;
    g_cells[cell] = entry;
}

// --- V サイクルの計画(T-0136)----------------------------------------------------------------------
// 段の形(u9 の先頭。GPU が作った段か CPU の系)から、段ごとの長い行の節の一覧・ImTail の境・間接の Dispatch の引数を作る。
// CPU で作っていた形(gpu_implicit.cpp の旧 MakeOrders・FindTailDepth。T-0120)と同じ判定。長い行の節の一覧は番号の昇順(塊ごとの印の bit の順位)。
static const uint32_t IM_PLAN_THREADS = 1024;
static const uint32_t IM_PLAN_MASK_WORDS = IM_PLAN_THREADS / 32;

groupshared uint32_t gs_planBits[3 * IM_PLAN_MASK_WORDS];  // 種類ごと: この塊の長い行の節の印
groupshared uint32_t gs_planBase[3];                       // 種類ごと: 前の塊までの数
groupshared uint32_t gs_planMostLinks;

// 節の印を書き、長い行の種類の bit(bit k = 種類 k の一覧に入る)を返す
uint32_t PlanNode(uint32_t depth, uint32_t node) {
    const ImGpuNode entry = g_nodes[node];
    const uint32_t links = entry.linkEnd - entry.linkStart;
    InterlockedMax(gs_planMostLinks, links);

    uint32_t mostChildLinks = 0;
    if (depth != 0) {
        for (uint32_t k = entry.childStart; k < entry.childEnd; ++k) {
            const ImGpuNode child = g_nodes[ChildAt(k)];
            mostChildLinks = max(mostChildLinks, child.linkEnd - child.linkStart);
        }
    }

    const bool longRow = links > IM_LONG_ROW_LINKS;
    const bool longRestrict = mostChildLinks > IM_LONG_ROW_LINKS;
    g_plan[FlagWord(node)] = (longRow ? 1u : 0u) | (longRestrict ? 2u : 0u);

    uint32_t kinds = longRestrict ? (1u << IM_KIND_RESTRICT) : 0u;
    if (longRow)
        kinds |= 1u << entry.color;

    return kinds;
}

// この塊で lane より前にある種類 kind の印の数
uint32_t PlanRank(uint32_t kind, uint32_t lane) {
    uint32_t rank = 0;
    for (uint32_t w = 0; w < lane / 32; ++w)
        rank += countbits(gs_planBits[(kind * IM_PLAN_MASK_WORDS) + w]);

    const uint32_t below = (1u << (lane % 32)) - 1u;
    return rank + countbits(gs_planBits[(kind * IM_PLAN_MASK_WORDS) + (lane / 32)] & below);
}

// 1 グループ = 1 段: 節を 1024 ずつの塊で回し、長い行の節を種類ごとの一覧へ番号の昇順に置く
[numthreads(IM_PLAN_THREADS, 1, 1)] void ImPlanLevels(uint32_t3 group : SV_GroupID, uint32_t lane : SV_GroupIndex) {
    const uint32_t depth = group.x;
    if (depth >= LevelCount())
        return;

    const uint32_t offset = NodeOffset(depth);
    const uint32_t count = NodeCount(depth);
    if (lane < 3)
        gs_planBase[lane] = 0;

    if (lane == 0)
        gs_planMostLinks = 0;

    for (uint32_t base = 0; base < count; base += IM_PLAN_THREADS) {
        if (lane < 3 * IM_PLAN_MASK_WORDS)
            gs_planBits[lane] = 0;

        GroupMemoryBarrierWithGroupSync();

        // --- 印を立てる ---
        const uint32_t local = base + lane;
        const uint32_t kinds = local < count ? PlanNode(depth, offset + local) : 0u;
        for (uint32_t kind = 0; kind < 3; ++kind) {
            if (((kinds >> kind) & 1u) != 0)
                InterlockedOr(gs_planBits[(kind * IM_PLAN_MASK_WORDS) + (lane / 32)], 1u << (lane % 32));
        }

        GroupMemoryBarrierWithGroupSync();

        // --- 順位の番地に置く ---
        for (uint32_t kind = 0; kind < 3; ++kind) {
            if (((kinds >> kind) & 1u) != 0)
                g_plan[LongListWord(depth, kind) + gs_planBase[kind] + PlanRank(kind, lane)] = offset + local;
        }

        GroupMemoryBarrierWithGroupSync();

        if (lane < 3) {
            uint32_t total = 0;
            for (uint32_t w = 0; w < IM_PLAN_MASK_WORDS; ++w)
                total += countbits(gs_planBits[(lane * IM_PLAN_MASK_WORDS) + w]);

            gs_planBase[lane] += total;
        }

        GroupMemoryBarrierWithGroupSync();
    }

    GroupMemoryBarrierWithGroupSync();
    if (lane < 3)
        g_plan[LevelWord(depth, lane)] = gs_planBase[lane];

    if (lane == 0)
        g_plan[LevelWord(depth, IM_PLAN_MOST_LINKS)] = gs_planMostLinks;
}

void WriteArgs(uint32_t slot, uint32_t groups) {
    g_args[(3 * slot) + 0] = groups;
    g_args[(3 * slot) + 1] = 1;
    g_args[(3 * slot) + 2] = 1;
}

bool SmallLevel(uint32_t depth) {
    return NodeCount(depth) <= g_tailMaxNodes && g_plan[LevelWord(depth, IM_PLAN_MOST_LINKS)] <= g_tailMaxLinks;
}

// ImTail が受け持つ最初の段: そこから最も粗い段まで、どの段も節が tailMaxNodes 以下で隣が tailMaxLinks 以下。無くても最も粗い段の節が
// coarsestTailMaxNodes 以下ならその段から(T-0120)。段ごとの Dispatch を積んだ段(g_dispatchLevels)より深くは下りない(そこから先は ImTail)
uint32_t PlanTailDepth(uint32_t levels) {
    uint32_t tail = levels;
    while (tail > 0 && SmallLevel(tail - 1))
        --tail;

    if (tail == levels && NodeCount(levels - 1) <= g_coarsestTailMaxNodes)
        tail = levels - 1;

    if (min(tail, levels - 1) >= g_dispatchLevels)
        tail = g_dispatchLevels - 1;

    return tail;
}

// 1 スレッド: ImTail の境と、記録した全部の間接の Dispatch の引数(要らない段は 0 グループ)
[numthreads(1, 1, 1)] void ImPlanArgs() {
    const uint32_t levels = LevelCount();
    const uint32_t tail = levels == 0 ? 0 : PlanTailDepth(levels);
    const uint32_t terminal = levels == 0 ? 0 : min(tail, levels - 1);
    const bool useTail = tail < levels;
    g_plan[IM_PLAN_TAIL] = tail;
    g_plan[IM_PLAN_TERMINAL] = terminal;

    WriteArgs(IM_SLOT_CELLS, ShortGroups(CellCount()));
    WriteArgs(IM_SLOT_FACES, ShortGroups(FaceCount()));
    WriteArgs(IM_SLOT_CONVERGED, ShortGroups(CellCount()) + g_plan[LevelWord(0, 0)] + g_plan[LevelWord(0, 1)]);
    WriteArgs(IM_SLOT_TAIL, useTail ? 1u : 0u);
    for (uint32_t color = 0; color < 2; ++color) {
        const uint32_t groups = ShortGroups(NodeCount(terminal)) + g_plan[LevelWord(terminal, color)];
        WriteArgs(IM_SLOT_COARSEST + color, useTail || levels == 0 ? 0u : groups);
    }

    // --- 段ごと: 下りが止まる段より上だけ ---
    for (uint32_t depth = 0; depth < g_dispatchLevels; ++depth) {
        const uint32_t slot = IM_SLOT_DEPTH_BASE + (depth * IM_SLOT_DEPTH_STRIDE);
        const bool down = depth < terminal;
        for (uint32_t color = 0; color < 2; ++color) {
            const uint32_t groups = ShortGroups(NodeCount(depth)) + g_plan[LevelWord(depth, color)];
            WriteArgs(slot + IM_SLOT_SMOOTH + color, down ? groups : 0u);
        }

        const uint32_t restrictGroups = ShortGroups(NodeCount(depth + 1)) +
                                        g_plan[LevelWord(depth + 1, IM_PLAN_LONG_RESTRICT)];
        WriteArgs(slot + IM_SLOT_RESTRICT, down ? restrictGroups : 0u);
        WriteArgs(slot + IM_SLOT_PROLONG, down ? ShortGroups(NodeCount(depth)) : 0u);
    }
}
