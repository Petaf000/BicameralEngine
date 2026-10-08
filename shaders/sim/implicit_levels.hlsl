// implicit_levels.hlsl — 細かいレベルの熱の陰解法(方式②。ADR-0019)の多重格子の段・重みを GPU で作る(T-0134)。
// CPU リファレンスは engine/src/sim/implicit_conduction.cpp の BuildImplicitGrid(MakeCellLevel・Coarsen・FinishLevel)と、
// それを GPU の並びにする gpu_implicit.cpp の MakeNodes。作るものは GpuImplicit の節(ImGpuNode)・隣(ImGpuLink)・子の一覧と同じ並び。
// 入力は implicit_build.hlsl が作った系(セル・面・セルの面の一覧)と作業場のセルの座標(CellKeyWord)。
// 番号の付け方も CPU と同じにする(隣の順・子の順まで):
//   - 段 0 = セル。行 = セルの面の一覧の順(面の番号の昇順)。粗い側の行の係数は 面の係数 ÷ 8^差
//   - 親の番号 = 細かい段の節を番号の順に見て初めて出会う順(同じ親の鍵の節の最小の番号〔表に atomic の最小〕だけを数える接頭和)
//   - 親の行 = 子の隣を (子の番号, 隣の番号) の順に見て初めて出会う順(同じ (親, 隣の親) の最小の隣の番号の、その行の中の順位)
//   - 親の行の係数 = 子の隣の係数(÷ 8 は縮約した親)の和(128bit の繰り上げつきの atomic。和は順によらない)。縮約した親が端なら ÷ 2
//   - 段の数は値で決まる: 自分の重み < 1/2 の節があり、段の数が上限より少なく、縮約で節が減った時だけ次の段を作る(述語で回を飛ばす)
// 重み(係数 ÷ 対角、Q48)は 128bit の割り算(ImWeight。CPU と同じ関数)。
// 段の順: Clear → Cells → CellLinks →(回 d = 0, 1, … 上限 − 2: NodeHash → NodeFirst → ParentScanLocal → ParentScanGroups → Parents →
//   LinkHash → Count → RowScanLocal → RowScanGroups → Place → Sort → AddCoefficients → CoarseNodes → Finish)→ RoundEnd(回ごと。述語の外)
// 節の並び(長い行・色ごとの並び)・ImTail の境・間接の Dispatch は T-0135(GpuImplicit はまだ CPU の系の形で作る)。
#include "common/implicit_conduction.hlsli"

RWByteAddressBuffer g_system : register(u0);  // implicit_build の系(セル・面・面の一覧。読むだけ)
RWByteAddressBuffer g_build
    : register(u1);  // implicit_build の作業場(見出し・面の一覧の位置のセル・セルの座標。読むだけ)
RWByteAddressBuffer g_levels : register(u2);  // この段の作業場(語の並びは下の番地の関数)
RWStructuredBuffer<ImGpuNode> g_nodes : register(u3);
RWStructuredBuffer<ImGpuLink> g_links : register(u4);
RWStructuredBuffer<uint32_t> g_children
    : register(u5);  // 子の一覧(GpuImplicit の面の一覧の後ろ。番地は 2 × 面の数を引いたもの)
RWStructuredBuffer<uint64_t> g_predicate : register(u6);  // 回ごと: 0 でなければその回を飛ばす

cbuffer LvConstants : register(b0) {
    uint32_t g_maxCells;       // セルの上限(段の節の数の上限)
    uint32_t g_maxNodes;       // 全部の段の節の上限
    uint32_t g_maxLinks;       // 全部の段の隣の上限
    uint32_t g_levelLinks;     // 1 段の隣の上限(= 2 × 面の上限)
    uint32_t g_depth;          // この回で縮約する細かい段
    uint32_t g_maxLevels;      // 段の数の上限(CPU の MAX_GRID_LEVELS)
    uint32_t g_nodeTableMask;  // 親の鍵の表の大きさ − 1(2 の冪)
    uint32_t g_linkTableMask;  // (親, 隣の親) の表の大きさ − 1
    uint32_t g_keysWord;       // g_build のセルの座標の始まり(語)
    uint32_t g_listCellsWord;  // g_build の面の一覧の位置のセルの始まり(語)
    uint32_t g_facesByte;      // g_system の面の始まり
    uint32_t g_listsByte;      // g_system の面の一覧の始まり
};

static const uint32_t LV_THREADS = 64;
static const uint32_t LV_SCAN_THREADS = 1024;
static const uint32_t NONE = 0xFFFFFFFFu;
static const int64_t HALF_WEIGHT = (int64_t)1 << (IM_WEIGHT_SHIFT - 1);

// implicit_build.hlsl の見出し
static const uint32_t BUILD_HEADER_FACES = 2;
static const uint32_t BUILD_HEADER_OVERFLOW = 3;
static const uint32_t BUILD_HEADER_CELLS = 4;

// --- この段の作業場の番地(語)-------------------------------------------------------------------
// 見出し: [0] 段の数・[1] 上限を超えた・段ごとの表(LV_DEPTH_STRIDE 語ずつ)
static const uint32_t LV_LEVEL_COUNT = 0;
static const uint32_t LV_OVERFLOW = 1;
static const uint32_t LV_DEPTH_BASE = 16;
static const uint32_t LV_DEPTH_STRIDE = 72;
static const uint32_t LV_NODE_OFFSET = 0;  // 段の節の始まり(全体の番号)
static const uint32_t LV_NODE_COUNT = 1;
static const uint32_t LV_LINK_OFFSET = 2;  // 段の隣の始まり(全体の番号)
static const uint32_t LV_LINK_COUNT = 3;
static const uint32_t LV_NEEDS = 4;   // 自分の重み < 1/2 の節がある
static const uint32_t LV_FINEST = 5;  // 最も細かいレベル(符号の bit を反転した値の最大)
static const uint32_t LV_KEPT = 6;    // この段がある
static const uint32_t LV_HEADER_WORDS = LV_DEPTH_BASE + 8 * LV_DEPTH_STRIDE;

// 節の欄(作業場の 64 バイト / 節): 座標・縮約した親か・熱容量・対角
struct LvNode {
    int64_t x;
    int64_t y;
    int64_t z;
    int32_t level;
    uint32_t lowered;
    uint64_t capacityHigh;
    uint64_t capacityLow;
    uint64_t diagonalHigh;
    uint64_t diagonalLow;
};

static const uint32_t LV_NODE_WORDS = 16;
// 節の語(10 語 / 節)。細かい段として: 最初の節・接頭和・親・表の位置。粗い段として: 子の数・子の始まり・行の数・行の始まり・置いた数 2 つ
static const uint32_t NW_FIRST = 0;
static const uint32_t NW_SCAN = 1;
static const uint32_t NW_PARENT = 2;
static const uint32_t NW_CHILD_COUNT = 3;
static const uint32_t NW_CHILD_START = 4;
static const uint32_t NW_ROW_COUNT = 5;
static const uint32_t NW_ROW_START = 6;
static const uint32_t NW_SLOT = 7;
static const uint32_t NW_CHILD_FILL = 8;
static const uint32_t NW_ROW_FILL = 9;
static const uint32_t NW_WORDS = 10;
// 隣の語(10 語 / 隣): 係数 128bit(下位の語から 4 つ)・持ち主の節・隣の節(全体の番号)・最初の隣・表の位置・親の行の隣・置いた細かい隣
static const uint32_t LW_COEFFICIENT = 0;
static const uint32_t LW_OWNER = 4;
static const uint32_t LW_NEIGHBOR = 5;
static const uint32_t LW_FIRST = 6;
static const uint32_t LW_SLOT = 7;
static const uint32_t LW_TARGET = 8;
static const uint32_t LW_RAW = 9;
static const uint32_t LW_WORDS = 10;

uint32_t DepthWord(uint32_t kind, uint32_t depth) {
    return LV_DEPTH_BASE + kind * LV_DEPTH_STRIDE + depth;
}

uint32_t NodeByte(uint32_t node) {
    return (LV_HEADER_WORDS + LV_NODE_WORDS * node) * 4;
}

uint32_t NodeWord(uint32_t node, uint32_t field) {
    return LV_HEADER_WORDS + LV_NODE_WORDS * g_maxNodes + NW_WORDS * node + field;
}

uint32_t LinkWord(uint32_t link, uint32_t field) {
    return LV_HEADER_WORDS + (LV_NODE_WORDS + NW_WORDS) * g_maxNodes + LW_WORDS * link + field;
}

uint32_t RawChildWord(uint32_t position) {
    return LinkWord(g_maxLinks, 0) + position;
}

uint32_t NodeTableWord(uint32_t slot) {
    return RawChildWord(g_maxNodes) + slot;
}

uint32_t LinkTableWord(uint32_t slot) {
    return NodeTableWord(g_nodeTableMask + 1) + slot;
}

uint32_t GroupWord(uint32_t group, uint32_t kind) {
    return LinkTableWord(g_linkTableMask + 1) + 2 * group + kind;
}

uint32_t Load(uint32_t word) {
    return g_levels.Load(word * 4);
}

void Store(uint32_t word, uint32_t value) {
    g_levels.Store(word * 4, value);
}

uint32_t BuildWord(uint32_t word) {
    return g_build.Load(word * 4);
}

// --- 段の数と欄 ---------------------------------------------------------------------------------

uint32_t NodeOffset(uint32_t depth) {
    return Load(DepthWord(LV_NODE_OFFSET, depth));
}

uint32_t NodeCount(uint32_t depth) {
    return Load(DepthWord(LV_NODE_COUNT, depth));
}

uint32_t LinkOffset(uint32_t depth) {
    return Load(DepthWord(LV_LINK_OFFSET, depth));
}

uint32_t LinkCount(uint32_t depth) {
    return Load(DepthWord(LV_LINK_COUNT, depth));
}

bool Kept(uint32_t depth) {
    return Load(DepthWord(LV_KEPT, depth)) != 0 && Load(LV_OVERFLOW) == 0;
}

uint32_t FaceCount() {
    return BuildWord(BUILD_HEADER_FACES);
}

LvNode LoadNode(uint32_t node) {
    return g_levels.Load<LvNode>(NodeByte(node));
}

FxU128 Capacity(LvNode node) {
    FxU128 value = {node.capacityHigh, node.capacityLow};
    return value;
}

FxU128 Diagonal(LvNode node) {
    FxU128 value = {node.diagonalHigh, node.diagonalLow};
    return value;
}

FxU128 LoadCoefficient(uint32_t link) {
    const uint4 words = g_levels.Load4(LinkWord(link, LW_COEFFICIENT) * 4);
    FxU128 value = {((uint64_t)words.w << 32) | words.z, ((uint64_t)words.y << 32) | words.x};
    return value;
}

void StoreCoefficient(uint32_t link, FxU128 value) {
    g_levels.Store4(LinkWord(link, LW_COEFFICIENT) * 4, uint4((uint32_t)value.lo, (uint32_t)(value.lo >> 32),
                                                              (uint32_t)value.hi, (uint32_t)(value.hi >> 32)));
}

// 128bit の繰り上げつきの atomic の足し算(語ごとに足し、前の値から自分の桁あふれを知って上の語へ。和は順によらない)
void AddCoefficient(uint32_t link, FxU128 value) {
    const uint32_t parts[4] = {(uint32_t)value.lo, (uint32_t)(value.lo >> 32), (uint32_t)value.hi,
                               (uint32_t)(value.hi >> 32)};
    uint32_t carry = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        const uint64_t add = (uint64_t)parts[i] + carry;
        carry = 0;
        if (add == 0)
            continue;

        if ((add >> 32) != 0) {
            carry = 1;
            continue;
        }

        uint32_t original;
        g_levels.InterlockedAdd((LinkWord(link, LW_COEFFICIENT) + i) * 4, (uint32_t)add, original);
        carry = original + (uint32_t)add < original ? 1u : 0u;
    }

    FX_ASSERT(carry == 0);
}

void MarkOverflow() {
    Store(LV_OVERFLOW, 1);
}

uint32_t BiasLevel(int32_t level) {
    return ((uint32_t)level) ^ 0x80000000u;
}

int32_t Finest(uint32_t depth) {
    return (int32_t)(Load(DepthWord(LV_FINEST, depth)) ^ 0x80000000u);
}

// 段の節を書き終えた時: 重み < 1/2 と最も細かいレベル(次の回が使う)
void NoteLevelNode(uint32_t depth, int32_t level, int64_t selfWeight) {
    g_levels.InterlockedMax(DepthWord(LV_FINEST, depth) * 4, BiasLevel(level));
    if (selfWeight < HALF_WEIGHT)
        g_levels.InterlockedOr(DepthWord(LV_NEEDS, depth) * 4, 1u);
}

uint32_t ColorOf(int64_t x, int64_t y, int64_t z) {
    return (uint32_t)((x + y + z) & 1);
}

// --- 接頭和(グループの中。2 本を同時に。包含的)------------------------------------------------------
groupshared uint32_t gs_first[LV_SCAN_THREADS];
groupshared uint32_t gs_second[LV_SCAN_THREADS];

void ScanGroup(uint32_t thread) {
    for (uint32_t offset = 1; offset < LV_SCAN_THREADS; offset <<= 1) {
        GroupMemoryBarrierWithGroupSync();
        const uint32_t first = thread >= offset ? gs_first[thread - offset] : 0;
        const uint32_t second = thread >= offset ? gs_second[thread - offset] : 0;
        GroupMemoryBarrierWithGroupSync();
        gs_first[thread] += first;
        gs_second[thread] += second;
    }

    GroupMemoryBarrierWithGroupSync();
}

// グループの和(GroupWord)を排他的な接頭和に置き換え、全体の和を返す(1 スレッド)
uint2 ScanGroupSums(uint32_t count) {
    const uint32_t groups = (count + LV_SCAN_THREADS - 1) / LV_SCAN_THREADS;
    uint2 running = uint2(0, 0);
    for (uint32_t group = 0; group < groups; ++group) {
        const uint2 sums = uint2(Load(GroupWord(group, 0)), Load(GroupWord(group, 1)));
        Store(GroupWord(group, 0), running.x);
        Store(GroupWord(group, 1), running.y);
        running += sums;
    }

    return running;
}

// --- Clear: 見出し・表を 0 に、回の述語を全部「飛ばす」に(1 スレッド = 1 語)--------------------------------
[numthreads(LV_THREADS, 1, 1)] void LvClear(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t word = dispatch.x;
    if (word < g_maxLevels)
        g_predicate[word] = 1;

    if (word < LV_HEADER_WORDS)
        Store(word, 0);

    if (word < g_nodeTableMask + 1)
        Store(NodeTableWord(word), 0);

    if (word < g_linkTableMask + 1)
        Store(LinkTableWord(word), 0);
}

// --- 段 0(CPU の MakeCellLevel + FinishLevel)-------------------------------------------------------

// 面の一覧の 1 つ(面の番号 × 2 + 粗い側なら 1)から、隣のセルと自分の単位の係数
void ListEntry(uint32_t value, out uint32_t neighbor, out FxU128 coefficient) {
    const ImGpuFace face = g_system.Load<ImGpuFace>(g_facesByte + (value >> 1) * 32);
    const FxU128 fineCoefficient = {face.coefficientHigh, face.coefficientLow};
    const bool coarseSide = (value & 1u) != 0;
    neighbor = coarseSide ? face.fine : face.coarse;
    coefficient = ImWideShiftRight(fineCoefficient, coarseSide ? IM_ENERGY_BITS_PER_LEVEL * face.gap : 0u);
}

// Cells: セル = 段 0 の節(座標・熱容量・対角・自分の重み)。段 0 の見出し(1 スレッド = 1 セル)
[numthreads(LV_THREADS, 1, 1)] void LvCells(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t cellCount = BuildWord(BUILD_HEADER_CELLS);
    const uint32_t listCount = 2 * FaceCount();
    if (dispatch.x == 0) {
        const bool overflow = BuildWord(BUILD_HEADER_OVERFLOW) != 0 || cellCount > g_maxCells ||
                              cellCount > g_maxNodes || listCount > g_levelLinks || listCount > g_maxLinks;
        if (overflow)
            MarkOverflow();

        Store(LV_LEVEL_COUNT, 1);
        Store(DepthWord(LV_NODE_COUNT, 0), cellCount);
        Store(DepthWord(LV_LINK_COUNT, 0), listCount);
        Store(DepthWord(LV_KEPT, 0), overflow ? 0u : 1u);
    }

    const uint32_t cellId = dispatch.x;
    if (cellId >= cellCount || cellId >= g_maxCells || cellId >= g_maxNodes)
        return;

    const uint4 keyLow = g_build.Load4((g_keysWord + 8 * cellId) * 4);
    const uint4 keyHigh = g_build.Load4((g_keysWord + 8 * cellId + 4) * 4);
    const ImGpuCell cell = g_system.Load<ImGpuCell>(cellId * 40);

    LvNode node;
    node.level = (int32_t)keyLow.x;
    node.x = (int64_t)(((uint64_t)keyLow.z << 32) | keyLow.y);
    node.y = (int64_t)(((uint64_t)keyHigh.x << 32) | keyLow.w);
    node.z = (int64_t)(((uint64_t)keyHigh.z << 32) | keyHigh.y);
    node.lowered = 0;

    // --- 対角 = C + Σ 行の係数(行 = セルの面の一覧の順)---
    const FxU128 capacity = ImWide(cell.heatCapacity);
    FxU128 diagonal = capacity;
    for (uint32_t k = cell.faceStart; k < cell.faceEnd; ++k) {
        uint32_t neighbor;
        FxU128 coefficient;
        ListEntry(g_system.Load(g_listsByte + k * 4), neighbor, coefficient);
        diagonal = ImWideAdd(diagonal, coefficient);
    }

    node.capacityHigh = capacity.hi;
    node.capacityLow = capacity.lo;
    node.diagonalHigh = diagonal.hi;
    node.diagonalLow = diagonal.lo;
    g_levels.Store<LvNode>(NodeByte(cellId), node);

    ImGpuNode gpuNode;
    gpuNode.selfWeight = ImWeight(capacity, diagonal);
    gpuNode.restrictWeight = 0;
    gpuNode.linkStart = cell.faceStart;
    gpuNode.linkEnd = cell.faceEnd;
    gpuNode.parent = 0;
    gpuNode.color = ColorOf(node.x, node.y, node.z);
    gpuNode.childStart = 0;
    gpuNode.childEnd = 0;
    g_nodes[cellId] = gpuNode;
    NoteLevelNode(0, node.level, gpuNode.selfWeight);
}

    // CellLinks: 段 0 の隣(1 スレッド = 面の一覧の 1 つ)。最後のスレッドの組は回 0 の述語も
    [numthreads(LV_THREADS, 1, 1)] void LvCellLinks(uint3 dispatch : SV_DispatchThreadID) {
    if (dispatch.x == 0) {
        const bool next = Kept(0) && Load(DepthWord(LV_NEEDS, 0)) != 0 && 1 < g_maxLevels;
        g_predicate[0] = next ? 0 : 1;
    }

    const uint32_t k = dispatch.x;
    if (!Kept(0) || k >= LinkCount(0))
        return;

    const uint32_t owner = BuildWord(g_listCellsWord + k);
    uint32_t neighbor;
    FxU128 coefficient;
    ListEntry(g_system.Load(g_listsByte + k * 4), neighbor, coefficient);
    StoreCoefficient(k, coefficient);
    Store(LinkWord(k, LW_OWNER), owner);
    Store(LinkWord(k, LW_NEIGHBOR), neighbor);

    ImGpuLink link;
    link.weight = ImWeight(coefficient, Diagonal(LoadNode(owner)));
    link.neighbor = neighbor;
    link.padding = 0;
    g_links[k] = link;
}

// --- 回 d: 段 d を縮約して段 d + 1 を作る(CPU の Coarsen + FinishLevel)--------------------------------------

// 細かい段の節 node(全体の番号)の親の鍵(最も細かいレベルなら 1 つ粗いレベルの親のセル、ほかは自分)
LvNode ParentKey(uint32_t node) {
    LvNode key = LoadNode(node);
    const int32_t finest = Finest(g_depth);
    key.lowered = key.level == finest ? 1u : 0u;
    if (key.lowered != 0) {
        key.level = finest - 1;
        key.x >>= 1;
        key.y >>= 1;
        key.z >>= 1;
    }

    return key;
}

bool SameKey(LvNode a, LvNode b) {
    return a.level == b.level && a.x == b.x && a.y == b.y && a.z == b.z;
}

uint32_t HashWords(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t h = a * 0x9E3779B1u;
    h = (h ^ (h >> 15)) + b * 0x85EBCA77u;
    h = (h ^ (h >> 13)) + c * 0xC2B2AE3Du;
    h = (h ^ (h >> 16)) + d * 0x27D4EB2Fu;
    return h ^ (h >> 15);
}

uint32_t HashKey(LvNode key) {
    const uint32_t mixed = HashWords((uint32_t)key.x, (uint32_t)key.y, (uint32_t)key.z, (uint32_t)key.level);
    return HashWords(mixed, (uint32_t)((uint64_t)key.x >> 32), (uint32_t)((uint64_t)key.y >> 32),
                     (uint32_t)((uint64_t)key.z >> 32));
}

bool InRound(uint32_t local) {
    return Kept(g_depth) && local < NodeCount(g_depth);
}

// NodeHash: 親の鍵の表に「その鍵の最小の節の番号 + 1」を入れる(1 スレッド = 細かい段の 1 節)
[numthreads(LV_THREADS, 1, 1)] void LvNodeHash(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t local = dispatch.x;
    if (!InRound(local))
        return;

    const uint32_t offset = NodeOffset(g_depth);
    const LvNode key = ParentKey(offset + local);
    uint32_t slot = HashKey(key) & g_nodeTableMask;
    for (uint32_t probe = 0; probe <= g_nodeTableMask; ++probe) {
        uint32_t previous;
        g_levels.InterlockedCompareExchange(NodeTableWord(slot) * 4, 0u, local + 1, previous);
        if (previous == 0)
            return;

        if (SameKey(ParentKey(offset + previous - 1), key)) {
            g_levels.InterlockedMin(NodeTableWord(slot) * 4, local + 1);
            return;
        }

        slot = (slot + 1) & g_nodeTableMask;
    }

    MarkOverflow();
}

    // NodeFirst: 自分の鍵の最小の節(初めて出会う節)と表の位置
    [numthreads(LV_THREADS, 1, 1)] void LvNodeFirst(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t local = dispatch.x;
    if (!InRound(local))
        return;

    const uint32_t offset = NodeOffset(g_depth);
    const LvNode key = ParentKey(offset + local);
    uint32_t slot = HashKey(key) & g_nodeTableMask;
    for (uint32_t probe = 0; probe <= g_nodeTableMask; ++probe) {
        const uint32_t stored = Load(NodeTableWord(slot));
        if (stored == 0)
            break;

        if (SameKey(ParentKey(offset + stored - 1), key)) {
            Store(NodeWord(offset + local, NW_FIRST), stored - 1);
            Store(NodeWord(offset + local, NW_SLOT), slot);
            return;
        }

        slot = (slot + 1) & g_nodeTableMask;
    }

    MarkOverflow();
}

// ParentScanLocal: 「初めて出会う節」の印のグループの中の排他的な接頭和
[numthreads(LV_SCAN_THREADS, 1, 1)] void LvParentScanLocal(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
    const uint32_t local = group.x * LV_SCAN_THREADS + thread.x;
    const bool inside = InRound(local);
    const uint32_t node = NodeOffset(g_depth) + local;
    const uint32_t first = inside && Load(NodeWord(node, NW_FIRST)) == local ? 1u : 0u;
    gs_first[thread.x] = first;
    gs_second[thread.x] = 0;
    ScanGroup(thread.x);

    if (inside)
        Store(NodeWord(node, NW_SCAN), gs_first[thread.x] - first);

    if (thread.x == LV_SCAN_THREADS - 1)
        Store(GroupWord(group.x, 0), gs_first[thread.x]);
}

    // ParentScanGroups: 親の数が決まる。減ったら段 d + 1 を置く(減らなければ CPU と同じく段を足さずに終わる。1 スレッド)
    [numthreads(1, 1, 1)] void LvParentScanGroups() {
    if (!Kept(g_depth))
        return;

    const uint32_t fineCount = NodeCount(g_depth);
    const uint32_t coarseCount = ScanGroupSums(fineCount).x;
    const uint32_t offset = NodeOffset(g_depth) + fineCount;
    if (coarseCount == fineCount)
        return;

    if (offset + coarseCount > g_maxNodes) {
        MarkOverflow();
        return;
    }

    Store(DepthWord(LV_NODE_OFFSET, g_depth + 1), offset);
    Store(DepthWord(LV_NODE_COUNT, g_depth + 1), coarseCount);
    Store(DepthWord(LV_KEPT, g_depth + 1), 1);
    Store(LV_LEVEL_COUNT, g_depth + 2);
}

uint32_t CoarseNode(uint32_t parent) {
    return NodeOffset(g_depth + 1) + parent;
}

// Parents: 親の番号。初めて出会う節が親の節(座標・縮約したか)を置き、数を 0 に。親の鍵の表を空に戻す
[numthreads(LV_THREADS, 1, 1)] void LvParents(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t local = dispatch.x;
    if (!InRound(local) || !Kept(g_depth + 1))
        return;

    const uint32_t node = NodeOffset(g_depth) + local;
    const uint32_t first = Load(NodeWord(node, NW_FIRST));
    const uint32_t firstNode = NodeOffset(g_depth) + first;
    const uint32_t parent = Load(NodeWord(firstNode, NW_SCAN)) + Load(GroupWord(first / LV_SCAN_THREADS, 0));
    Store(NodeWord(node, NW_PARENT), parent);
    if (first != local)
        return;

    LvNode coarse = ParentKey(node);
    coarse.capacityHigh = 0;
    coarse.capacityLow = 0;
    coarse.diagonalHigh = 0;
    coarse.diagonalLow = 0;
    const uint32_t coarseNode = CoarseNode(parent);
    g_levels.Store<LvNode>(NodeByte(coarseNode), coarse);
    Store(NodeWord(coarseNode, NW_CHILD_COUNT), 0);
    Store(NodeWord(coarseNode, NW_ROW_COUNT), 0);
    Store(NodeWord(coarseNode, NW_CHILD_FILL), 0);
    Store(NodeWord(coarseNode, NW_ROW_FILL), 0);
    Store(NodeTableWord(Load(NodeWord(node, NW_SLOT))), 0);
}

bool InLinkRound(uint32_t local) {
    return Kept(g_depth + 1) && local < LinkCount(g_depth);
}

// 細かい段の隣 local の (親, 隣の親)。同じ親なら NONE
uint2 LinkParents(uint32_t local) {
    const uint32_t link = LinkOffset(g_depth) + local;
    const uint32_t parent = Load(NodeWord(Load(LinkWord(link, LW_OWNER)), NW_PARENT));
    const uint32_t other = Load(NodeWord(Load(LinkWord(link, LW_NEIGHBOR)), NW_PARENT));
    return parent == other ? uint2(NONE, NONE) : uint2(parent, other);
}

// LinkHash: (親, 隣の親) の表に「その組の最小の隣の番号 + 1」を入れる(1 スレッド = 細かい段の 1 隣)
[numthreads(LV_THREADS, 1, 1)] void LvLinkHash(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t local = dispatch.x;
    if (!InLinkRound(local))
        return;

    const uint2 pair = LinkParents(local);
    if (pair.x == NONE)
        return;

    uint32_t slot = HashWords(pair.x, pair.y, 0x5bd1e995u, 0u) & g_linkTableMask;
    for (uint32_t probe = 0; probe <= g_linkTableMask; ++probe) {
        uint32_t previous;
        g_levels.InterlockedCompareExchange(LinkTableWord(slot) * 4, 0u, local + 1, previous);
        if (previous == 0)
            return;

        if (all(LinkParents(previous - 1) == pair)) {
            g_levels.InterlockedMin(LinkTableWord(slot) * 4, local + 1);
            return;
        }

        slot = (slot + 1) & g_linkTableMask;
    }

    MarkOverflow();
}

    // Count: 親の子の数(1 スレッド = 細かい段の 1 節)と、親の行の数(初めて出会う組だけ。1 スレッド = 細かい段の 1 隣)
    [numthreads(LV_THREADS, 1, 1)] void LvCount(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t local = dispatch.x;
    if (InRound(local) && Kept(g_depth + 1)) {
        const uint32_t node = NodeOffset(g_depth) + local;
        g_levels.InterlockedAdd(NodeWord(CoarseNode(Load(NodeWord(node, NW_PARENT))), NW_CHILD_COUNT) * 4, 1u);
    }

    if (!InLinkRound(local))
        return;

    const uint32_t link = LinkOffset(g_depth) + local;
    const uint2 pair = LinkParents(local);
    if (pair.x == NONE) {
        Store(LinkWord(link, LW_FIRST), NONE);
        return;
    }

    uint32_t slot = HashWords(pair.x, pair.y, 0x5bd1e995u, 0u) & g_linkTableMask;
    for (uint32_t probe = 0; probe <= g_linkTableMask; ++probe) {
        const uint32_t stored = Load(LinkTableWord(slot));
        if (stored == 0)
            break;

        if (all(LinkParents(stored - 1) == pair)) {
            Store(LinkWord(link, LW_FIRST), stored - 1);
            Store(LinkWord(link, LW_SLOT), slot);
            if (stored - 1 == local)
                g_levels.InterlockedAdd(NodeWord(CoarseNode(pair.x), NW_ROW_COUNT) * 4, 1u);

            return;
        }

        slot = (slot + 1) & g_linkTableMask;
    }

    MarkOverflow();
}

// RowScanLocal: 親ごとの子の数と行の数のグループの中の排他的な接頭和(1 スレッド = 粗い段の 1 節)
[numthreads(LV_SCAN_THREADS, 1, 1)] void LvRowScanLocal(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
    const uint32_t parent = group.x * LV_SCAN_THREADS + thread.x;
    const bool inside = Kept(g_depth + 1) && parent < NodeCount(g_depth + 1);
    const uint32_t node = inside ? CoarseNode(parent) : 0;
    const uint32_t children = inside ? Load(NodeWord(node, NW_CHILD_COUNT)) : 0;
    const uint32_t row = inside ? Load(NodeWord(node, NW_ROW_COUNT)) : 0;
    gs_first[thread.x] = children;
    gs_second[thread.x] = row;
    ScanGroup(thread.x);

    if (inside) {
        Store(NodeWord(node, NW_CHILD_START), gs_first[thread.x] - children);
        Store(NodeWord(node, NW_ROW_START), gs_second[thread.x] - row);
    }

    if (thread.x == LV_SCAN_THREADS - 1) {
        Store(GroupWord(group.x, 0), gs_first[thread.x]);
        Store(GroupWord(group.x, 1), gs_second[thread.x]);
    }
}

    // RowScanGroups: 段 d + 1 の隣の数と始まり(1 スレッド)
    [numthreads(1, 1, 1)] void LvRowScanGroups() {
    if (!Kept(g_depth + 1))
        return;

    const uint32_t links = ScanGroupSums(NodeCount(g_depth + 1)).y;
    const uint32_t offset = LinkOffset(g_depth) + LinkCount(g_depth);
    if (offset + links > g_maxLinks || links > g_levelLinks) {
        MarkOverflow();
        return;
    }

    Store(DepthWord(LV_LINK_OFFSET, g_depth + 1), offset);
    Store(DepthWord(LV_LINK_COUNT, g_depth + 1), links);
}

uint32_t ChildStart(uint32_t parent) {
    return Load(NodeWord(CoarseNode(parent), NW_CHILD_START)) + Load(GroupWord(parent / LV_SCAN_THREADS, 0));
}

uint32_t RowStart(uint32_t parent) {
    return Load(NodeWord(CoarseNode(parent), NW_ROW_START)) + Load(GroupWord(parent / LV_SCAN_THREADS, 1));
}

// Place: 子を親の子の一覧に、初めて出会う組を親の行に仮に置く(置く順は決まらない。次の Sort で番号の順に)
[numthreads(LV_THREADS, 1, 1)] void LvPlace(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t local = dispatch.x;
    if (InRound(local) && Kept(g_depth + 1)) {
        const uint32_t parent = Load(NodeWord(NodeOffset(g_depth) + local, NW_PARENT));
        uint32_t position;
        g_levels.InterlockedAdd(NodeWord(CoarseNode(parent), NW_CHILD_FILL) * 4, 1u, position);
        Store(RawChildWord(NodeOffset(g_depth) + ChildStart(parent) + position), local);
    }

    if (!InLinkRound(local))
        return;

    const uint32_t link = LinkOffset(g_depth) + local;
    if (Load(LinkWord(link, LW_FIRST)) != local)
        return;

    const uint32_t parent = LinkParents(local).x;
    uint32_t position;
    g_levels.InterlockedAdd(NodeWord(CoarseNode(parent), NW_ROW_FILL) * 4, 1u, position);
    Store(LinkWord(LinkOffset(g_depth + 1) + RowStart(parent) + position, LW_RAW), local);
}

    // Sort: 仮に置いた子・組を、同じ一覧の中で自分より小さい番号の数の位置へ。組は親の段の隣になる(係数は 0 から足す)
    [numthreads(LV_THREADS, 1, 1)] void LvSort(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t position = dispatch.x;
    if (!Kept(g_depth + 1) || Load(LV_OVERFLOW) != 0)
        return;

    const uint32_t fineOffset = NodeOffset(g_depth);
    if (position < NodeCount(g_depth)) {
        const uint32_t child = Load(RawChildWord(fineOffset + position));
        const uint32_t parent = Load(NodeWord(fineOffset + child, NW_PARENT));
        const uint32_t start = fineOffset + ChildStart(parent);
        const uint32_t end = start + Load(NodeWord(CoarseNode(parent), NW_CHILD_COUNT));
        uint32_t rank = start;
        for (uint32_t i = start; i < end; ++i) {
            if (Load(RawChildWord(i)) < child)
                ++rank;
        }

        g_children[rank] = fineOffset + child;
    }

    if (position >= LinkCount(g_depth + 1))
        return;

    const uint32_t coarseLinks = LinkOffset(g_depth + 1);
    const uint32_t fine = Load(LinkWord(coarseLinks + position, LW_RAW));
    const uint2 pair = LinkParents(fine);
    const uint32_t start = coarseLinks + RowStart(pair.x);
    const uint32_t end = start + Load(NodeWord(CoarseNode(pair.x), NW_ROW_COUNT));
    uint32_t rank = start;
    for (uint32_t i = start; i < end; ++i) {
        if (Load(LinkWord(i, LW_RAW)) < fine)
            ++rank;
    }

    Store(LinkWord(LinkOffset(g_depth) + fine, LW_TARGET), rank);
    StoreCoefficient(rank, ImWide((uint64_t)0));
    Store(LinkWord(rank, LW_OWNER), CoarseNode(pair.x));
    Store(LinkWord(rank, LW_NEIGHBOR), CoarseNode(pair.y));
}

// AddCoefficients: 細かい隣の係数(縮約した親なら ÷ 8)を親の行の隣へ足す。組の表を空に戻す(1 スレッド = 細かい段の 1 隣)
[numthreads(LV_THREADS, 1, 1)] void LvAddCoefficients(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t local = dispatch.x;
    if (!InLinkRound(local) || Load(LV_OVERFLOW) != 0)
        return;

    const uint32_t link = LinkOffset(g_depth) + local;
    const uint32_t first = Load(LinkWord(link, LW_FIRST));
    if (first == NONE)
        return;

    const uint32_t target = Load(LinkWord(LinkOffset(g_depth) + first, LW_TARGET));
    const uint32_t owner = Load(LinkWord(target, LW_OWNER));
    const uint32_t shift = LoadNode(owner).lowered != 0 ? IM_ENERGY_BITS_PER_LEVEL : 0;
    AddCoefficient(target, ImWideShiftRight(LoadCoefficient(link), shift));
    if (first == local)
        Store(LinkTableWord(Load(LinkWord(link, LW_SLOT))), 0);
}

// 親の段の隣の係数: 端の片方が縮約した親なら ÷ 2(そのレベルの離散化に合わせる。CPU の galerkin = false)
FxU128 CoarseCoefficient(uint32_t link) {
    const bool lowered = LoadNode(Load(LinkWord(link, LW_OWNER))).lowered != 0 ||
                         LoadNode(Load(LinkWord(link, LW_NEIGHBOR))).lowered != 0;
    const FxU128 coefficient = LoadCoefficient(link);
    return ImWideShiftRight(coefficient, lowered ? 1u : 0u);
}

// CoarseNodes: 親の熱容量(子の和。縮約した親なら ÷ 8)・対角・自分の重み・節(1 スレッド = 粗い段の 1 節)
[numthreads(LV_THREADS, 1, 1)] void LvCoarseNodes(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t parent = dispatch.x;
    if (!Kept(g_depth + 1) || parent >= NodeCount(g_depth + 1))
        return;

    const uint32_t node = CoarseNode(parent);
    LvNode coarse = LoadNode(node);
    const uint32_t shift = coarse.lowered != 0 ? IM_ENERGY_BITS_PER_LEVEL : 0;
    const uint32_t childStart = NodeOffset(g_depth) + ChildStart(parent);
    const uint32_t childEnd = childStart + Load(NodeWord(node, NW_CHILD_COUNT));
    FxU128 capacity = ImWide((uint64_t)0);
    for (uint32_t c = childStart; c < childEnd; ++c)
        capacity = ImWideAdd(capacity, ImWideShiftRight(Capacity(LoadNode(g_children[c])), shift));

    const uint32_t linkStart = LinkOffset(g_depth + 1) + RowStart(parent);
    const uint32_t linkEnd = linkStart + Load(NodeWord(node, NW_ROW_COUNT));
    FxU128 diagonal = capacity;
    for (uint32_t k = linkStart; k < linkEnd; ++k)
        diagonal = ImWideAdd(diagonal, CoarseCoefficient(k));

    coarse.capacityHigh = capacity.hi;
    coarse.capacityLow = capacity.lo;
    coarse.diagonalHigh = diagonal.hi;
    coarse.diagonalLow = diagonal.lo;
    g_levels.Store<LvNode>(NodeByte(node), coarse);

    ImGpuNode gpuNode;
    gpuNode.selfWeight = ImWeight(capacity, diagonal);
    gpuNode.restrictWeight = 0;
    gpuNode.linkStart = linkStart;
    gpuNode.linkEnd = linkEnd;
    gpuNode.parent = 0;
    gpuNode.color = ColorOf(coarse.x, coarse.y, coarse.z);
    gpuNode.childStart = 2 * FaceCount() + childStart;
    gpuNode.childEnd = 2 * FaceCount() + childEnd;
    g_nodes[node] = gpuNode;
    NoteLevelNode(g_depth + 1, coarse.level, gpuNode.selfWeight);
}

    // Finish: 親の段の隣の係数(÷ 2 の後を書き戻す)と重み(1 スレッド = 粗い段の 1 隣)、細かい段の節の親と縮約の重み(1 スレッド = 細かい段の 1 節)
    [numthreads(LV_THREADS, 1, 1)] void LvFinish(uint3 dispatch : SV_DispatchThreadID) {
    const uint32_t index = dispatch.x;
    if (!Kept(g_depth + 1))
        return;

    if (index < LinkCount(g_depth + 1)) {
        const uint32_t link = LinkOffset(g_depth + 1) + index;
        const FxU128 coefficient = CoarseCoefficient(link);
        const uint32_t owner = Load(LinkWord(link, LW_OWNER));
        StoreCoefficient(link, coefficient);  // 次の回は ÷ 2 の後の係数を縮約する(CPU の coarse.coefficients)

        ImGpuLink gpuLink;
        gpuLink.weight = ImWeight(coefficient, Diagonal(LoadNode(owner)));
        gpuLink.neighbor = Load(LinkWord(link, LW_NEIGHBOR));
        gpuLink.padding = 0;
        g_links[link] = gpuLink;
    }

    if (index >= NodeCount(g_depth))
        return;

    // 縮約の重み = 自分の D(親の単位)÷ 親の D。読むのは CoarseNodes が書いた対角だけ(係数を書き換える上の組とは別の語)
    const uint32_t node = NodeOffset(g_depth) + index;
    const uint32_t parentNode = CoarseNode(Load(NodeWord(node, NW_PARENT)));
    const LvNode parent = LoadNode(parentNode);
    const uint32_t shift = parent.lowered != 0 ? IM_ENERGY_BITS_PER_LEVEL : 0;
    g_nodes[node].parent = parentNode;
    g_nodes[node].restrictWeight = ImWeight(ImWideShiftRight(Diagonal(LoadNode(node)), shift), Diagonal(parent));
}

// RoundEnd: 次の回の述語(段 d + 1 があり、重み < 1/2 の節があり、段の数が上限より少ない時だけ回す。述語の外。1 スレッド)
[numthreads(1, 1, 1)] void LvRoundEnd() {
    const uint32_t next = g_depth + 1;
    if (next >= g_maxLevels)
        return;

    const bool run = Kept(next) && Load(DepthWord(LV_NEEDS, next)) != 0 && next + 1 < g_maxLevels;
    g_predicate[next] = run ? 0 : 1;
}
