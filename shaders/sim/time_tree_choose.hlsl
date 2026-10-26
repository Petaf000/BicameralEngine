// time_tree_choose.hlsl — 時間を飛ばす試作の刻みを選ぶ段(D-669 の窓・隣の差 1 段)の GPU 版(T-1112・T-1213)。
// CPU リファレンスは engine/src/sim/time_step_tree.cpp の WindowCaps(窓の格子の 3 回の最小)と ChooseExponents
// (窓の上限・隣との差 1 段・長すぎる歩の途中の単位は打ち切る)。入力は単位ごとの詰めた歩の記録(2 B。PackTimeTreeUnits。
// ビット 0〜3 e・4〜7 望みの e・8 歩の途中・9 触った)、出力は窓の上限・選んだ e と、出来事の後の詰めた記録。
// 段の順(engine/src/sim/gpu_time_tree_choose.cpp が積む。間は UAV の壁。1 出来事 = 4 Dispatch。区画が 2 つ以上なら + 3):
//   TtcWindowX(単位: 窓の上限を上限 maxExponent に・次の e の初め・掃く回の印を 0 に / 格子: 望みの e を記録から直に読んで x の半径の内の最小)
//   → TtcWindowY(y の最小)→ TtcWindowZ(z の最小をそのまま単位の上限へ InterlockedMin。集める段を兼ねる)
//   → TtcSweep × TTC_HEAD_PASSES(区画 = 4,096 単位を 1 グループが groupshared に持ち、区画の中で不動点まで回してから詰めた記録を書く。
//     区画が 1 つなら 1 回で終わり)→ TtcFixup(区画が 2 つ以上の時: 1 グループが、最後の回に変わった区画の隣の区画から順に、
//     変わる区画が無くなるまで 1 区画ずつ回す。述語や空の回を積まずに必ず不動点で終わる)。
// 隣との差 1 段は単調に減るだけの決まり(打ち切った単位の次の e は 触った ? 0 : min(e, cap) の上限まで)なので、どの順で回しても
// 同じ最大の不動点に落ちる(CPU の回す順とビット一致)。区画の外の隣は globallycoherent の g_ttcState から読む(古い値は大きいだけ)。
// T-1213: 前の形(1 単位 1 スレッドで掃く回を 128 積み、述語で飛ばす)は飛ばした回の壁だけで 1 出来事 0.086 ms かかった。
// ルートの SRV/UAV に GetDimensions を使わない(大きさはルート定数)。浮動小数点は使わない(04 §4)。
// 結び付け(gpu_time_tree_choose.cpp の ROOT_LAYOUT と同じ順)
RWStructuredBuffer<uint32_t> g_ttcCaps : register(u0);  // 単位ごと: 窓の上限
globallycoherent RWStructuredBuffer<uint32_t> g_ttcState
    : register(u1);                                      // 単位ごと: 今の e(下 8 bit)・歩の途中のまま・打ち切った
RWStructuredBuffer<uint32_t> g_ttcAfter : register(u2);  // 2 単位で 1 語: 出来事の後の詰めた記録
RWStructuredBuffer<uint32_t> g_ttcCounters
    : register(u3);  // 掃く回ごと: 変わった区画の数 / 回し切れなかった区画 / 区画の中の回の最大
RWStructuredBuffer<uint32_t> g_ttcWindowA : register(u4);  // 格子のセルごと(行き来する 2 つ)
RWStructuredBuffer<uint32_t> g_ttcWindowB : register(u5);
RWStructuredBuffer<uint32_t> g_ttcTileChanged : register(u6);   // 区画ごと: 最後に変わった回 + 1(0 = 変わっていない)
StructuredBuffer<uint32_t> g_ttcBefore : register(t0);          // 出来事ごとに 2 単位で 1 語: 選ぶ前の詰めた記録
StructuredBuffer<uint32_t> g_ttcGridUnits : register(t1);       // 格子のセル → 単位(無ければ TTC_NO_UNIT)
StructuredBuffer<uint32_t> g_ttcNeighborStarts : register(t2);  // 単位ごとの隣の始まり(単位の数 + 1)
StructuredBuffer<uint32_t> g_ttcNeighbors : register(t3);
StructuredBuffer<uint32_t> g_ttcTileNeighborStarts : register(t4);  // 区画ごとの隣の区画の始まり(区画の数 + 1)
StructuredBuffer<uint32_t> g_ttcTileNeighbors : register(t5);

cbuffer TimeTreeChooseConstants : register(b0) {
    uint32_t g_ttcUnits;
    uint32_t g_ttcGridX;
    uint32_t g_ttcGridY;
    uint32_t g_ttcGridZ;
    uint32_t g_ttcRadius;
    uint32_t g_ttcMaxExponent;
    uint32_t g_ttcCap;         // この刻みで始めてよい最も長い e
    uint32_t g_ttcPass;        // TtcSweep の回の番号
    uint32_t g_ttcBeforeWord;  // この出来事の選ぶ前の記録の始まり(語)
};

static const uint32_t TTC_NO_UNIT = 0xFFFFFFFFu;
static const uint32_t TTC_HEAD_PASSES = 3;               // gpu_time_tree_choose.h の HEAD_SWEEPS(区画が 2 つ以上の時)
static const uint32_t TTC_PASSES = TTC_HEAD_PASSES + 1;  // 掃く回 + 仕上げ(gpu_time_tree_choose.h の PASSES)
static const uint32_t TTC_UNSETTLED_SLOT = TTC_PASSES;   // 回し切れなかった区画の数
static const uint32_t TTC_INNER_SLOT = TTC_PASSES + 1;   // 区画の中の回の最大
static const uint32_t TTC_VISITS_SLOT = TTC_PASSES + 2;  // 仕上げで回した区画の数
static const uint32_t TTC_COUNTERS = TTC_PASSES + 3;     // gpu_time_tree_choose.h の COUNTERS

// --- 区画(gpu_time_tree_choose.h の TILE_UNITS・TILE_THREADS)---
static const uint32_t TTC_TILE_THREADS = 1024;
static const uint32_t TTC_UNITS_PER_THREAD = 4;
static const uint32_t TTC_TILE_UNITS = TTC_TILE_THREADS * TTC_UNITS_PER_THREAD;  // 16 KB の groupshared
static const uint32_t TTC_MAX_INNER = 1024;  // 区画の中の回の上限(着かなければ TTC_UNSETTLED_SLOT に数える)
static const uint32_t TTC_MAX_TILES = 4096;  // gpu_time_tree_choose.h の MAX_TILES(仕上げの groupshared の印 512 B)
static const uint32_t TTC_DIRTY_WORDS = TTC_MAX_TILES / 32;
static const uint32_t TTC_MAX_VISITS = 65536;  // 仕上げで回す区画の上限(着かなければ TTC_UNSETTLED_SLOT に数える)

// --- 詰めた記録(time_step_tree.h の TIME_TREE_RECORD_*)---
static const uint32_t TTC_RECORD_EXPONENT_MASK = 0xFu;
static const uint32_t TTC_RECORD_DESIRED_SHIFT = 4;
static const uint32_t TTC_RECORD_OPEN = 1u << 8;
static const uint32_t TTC_RECORD_TOUCHED = 1u << 9;

// --- 今の e の語 ---
static const uint32_t TTC_STATE_EXPONENT_MASK = 0xFFu;
static const uint32_t TTC_STATE_MIDSTEP = 1u << 8;    // 歩の途中のまま(次の e は決めていない)
static const uint32_t TTC_STATE_TRUNCATED = 1u << 9;  // 歩の途中を打ち切った

uint32_t TtcBefore(uint32_t unit) {
    const uint32_t word = g_ttcBefore[g_ttcBeforeWord + (unit >> 1)];

    return (unit & 1) == 0 ? (word & 0xFFFFu) : (word >> 16);
}

uint32_t TtcDesired(uint32_t record) {
    return min((record >> TTC_RECORD_DESIRED_SHIFT) & TTC_RECORD_EXPONENT_MASK, g_ttcMaxExponent);
}

// 格子のセルに置く望みの e(単位の無いセルは上限)
uint32_t TtcCellDesired(uint32_t cell) {
    const uint32_t unit = g_ttcGridUnits[cell];

    return unit == TTC_NO_UNIT ? g_ttcMaxExponent : TtcDesired(TtcBefore(unit));
}

// --- 格子: 軸に沿った半径の内(端の外は数えない)の範囲 ---
struct TtcAxisSpan {
    uint32_t first;   // 軸の 0 番のセル
    uint32_t stride;  // 軸に沿って 1 つ進むセルの差
    uint32_t low;
    uint32_t high;
};

TtcAxisSpan TtcAxis(uint32_t cell, uint32_t axis) {
    const uint32_t x = cell % g_ttcGridX;
    const uint32_t y = (cell / g_ttcGridX) % g_ttcGridY;
    const uint32_t z = cell / (g_ttcGridX * g_ttcGridY);
    const uint32_t position = axis == 0 ? x : (axis == 1 ? y : z);
    const uint32_t length = axis == 0 ? g_ttcGridX : (axis == 1 ? g_ttcGridY : g_ttcGridZ);

    TtcAxisSpan span;
    span.stride = axis == 0 ? 1u : (axis == 1 ? g_ttcGridX : g_ttcGridX * g_ttcGridY);
    span.low = position > g_ttcRadius ? position - g_ttcRadius : 0u;
    span.high = min(position + g_ttcRadius, length - 1);
    span.first = cell - (position * span.stride);

    return span;
}

// --- 単位: 窓の上限を上限に・次の e の初め(歩の途中なら今の e・閉じた単位は望みを cap まで)・掃く回の印 ---
void TtcInitUnit(uint32_t unit) {
    const uint32_t record = TtcBefore(unit);
    g_ttcCaps[unit] = g_ttcMaxExponent;
    if ((record & TTC_RECORD_OPEN) != 0)
        g_ttcState[unit] = (record & TTC_RECORD_EXPONENT_MASK) | TTC_STATE_MIDSTEP;
    else
        g_ttcState[unit] = min(TtcDesired(record), g_ttcCap);
}

// 初め + 窓の x(望みを記録から直に読む: A へ)
[numthreads(64, 1, 1)] void TtcWindowX(uint32_t id : SV_DispatchThreadID) {
    if (id < TTC_COUNTERS)
        g_ttcCounters[id] = 0;

    if (id < (g_ttcUnits + TTC_TILE_UNITS - 1) / TTC_TILE_UNITS)
        g_ttcTileChanged[id] = 0;

    if (id < g_ttcUnits)
        TtcInitUnit(id);

    if (id >= g_ttcGridX * g_ttcGridY * g_ttcGridZ)
        return;

    const TtcAxisSpan span = TtcAxis(id, 0);
    uint32_t least = TtcCellDesired(span.first + span.low);
    for (uint32_t i = span.low + 1; i <= span.high; ++i)
        least = min(least, TtcCellDesired(span.first + i));

    g_ttcWindowA[id] = least;
}

    // 窓の y(A → B)
    [numthreads(64, 1, 1)] void TtcWindowY(uint32_t cell : SV_DispatchThreadID) {
    if (cell >= g_ttcGridX * g_ttcGridY * g_ttcGridZ)
        return;

    const TtcAxisSpan span = TtcAxis(cell, 1);
    uint32_t least = g_ttcWindowA[span.first + (span.low * span.stride)];
    for (uint32_t i = span.low + 1; i <= span.high; ++i)
        least = min(least, g_ttcWindowA[span.first + (i * span.stride)]);

    g_ttcWindowB[cell] = least;
}

// 窓の z(B から)+ 集める: 単位の格子のセルの最小(自分も入る。InterlockedMin は順によらない)
[numthreads(64, 1, 1)] void TtcWindowZ(uint32_t cell : SV_DispatchThreadID) {
    if (cell >= g_ttcGridX * g_ttcGridY * g_ttcGridZ)
        return;

    const uint32_t unit = g_ttcGridUnits[cell];
    if (unit == TTC_NO_UNIT)
        return;

    const TtcAxisSpan span = TtcAxis(cell, 2);
    uint32_t least = g_ttcWindowB[span.first + (span.low * span.stride)];
    for (uint32_t i = span.low + 1; i <= span.high; ++i)
        least = min(least, g_ttcWindowB[span.first + (i * span.stride)]);

    uint32_t previous = 0;
    InterlockedMin(g_ttcCaps[unit], least, previous);
}

// --- 区画: 窓の上限と隣の差 1 段で縮める(groupshared の中で不動点まで)---
groupshared uint32_t s_ttcState[TTC_TILE_UNITS];
groupshared uint32_t s_ttcChanged[3];  // 区画の中の回ごとの「変わった」(3 つを回して使う。消すのは 2 回後)

// 区画の中の単位 local を上限まで縮める。変えたら true(区画の外の単位へも書き写す)
bool TtcSweepOnce(uint32_t tileFirst, uint32_t tileUnits, uint32_t local) {
    const uint32_t unit = tileFirst + local;
    uint32_t limit = g_ttcCaps[unit];
    const uint32_t end = g_ttcNeighborStarts[unit + 1];
    for (uint32_t k = g_ttcNeighborStarts[unit]; k < end; ++k) {
        const uint32_t neighbor = g_ttcNeighbors[k];
        const uint32_t inside = neighbor - tileFirst;  // 区画の前は折り返して大きくなる
        uint32_t state = 0;
        if (inside < tileUnits)
            state = s_ttcState[inside];
        else
            state = g_ttcState[neighbor];

        limit = min(limit, (state & TTC_STATE_EXPONENT_MASK) + 1);
    }

    const uint32_t state = s_ttcState[local];
    const uint32_t exponent = state & TTC_STATE_EXPONENT_MASK;
    if (exponent <= limit)
        return false;

    uint32_t next = limit;
    uint32_t flags = state & TTC_STATE_TRUNCATED;
    if ((state & TTC_STATE_MIDSTEP) != 0) {
        // 打ち切る: 次の e は 触った ? 0 : 打ち切った歩の e(TruncateUnit の望み)を cap まで
        const uint32_t record = TtcBefore(unit);
        const uint32_t restart = (record & TTC_RECORD_TOUCHED) != 0 ? 0u : (record & TTC_RECORD_EXPONENT_MASK);
        next = min(min(restart, g_ttcCap), limit);
        flags = TTC_STATE_TRUNCATED;
    }

    s_ttcState[local] = next | flags;
    g_ttcState[unit] = next | flags;

    return true;
}

// 出来事の後の詰めた記録(歩の途中のままなら前のまま・始めた単位は e・望み・歩の途中)
uint32_t TtcAfter(uint32_t unit, uint32_t state) {
    const uint32_t record = TtcBefore(unit);
    if ((state & TTC_STATE_MIDSTEP) != 0)
        return record;

    const uint32_t exponent = record & TTC_RECORD_EXPONENT_MASK;
    const uint32_t restart = (record & TTC_RECORD_TOUCHED) != 0 ? 0u : exponent;
    const uint32_t desired = (state & TTC_STATE_TRUNCATED) != 0
                                 ? restart
                                 : ((record >> TTC_RECORD_DESIRED_SHIFT) & TTC_RECORD_EXPONENT_MASK);

    return (state & TTC_STATE_EXPONENT_MASK) | (desired << TTC_RECORD_DESIRED_SHIFT) | TTC_RECORD_OPEN;
}

// 区画の後の記録を書く(2 単位で 1 語。区画の始まりは偶数)
void TtcPackTile(uint32_t tileFirst, uint32_t tileUnits, uint32_t thread) {
    const uint32_t words = (tileUnits + 1) / 2;
    for (uint32_t word = thread; word < words; word += TTC_TILE_THREADS) {
        const uint32_t first = word * 2;
        const uint32_t low = TtcAfter(tileFirst + first, s_ttcState[first]);
        const uint32_t high = first + 1 < tileUnits ? TtcAfter(tileFirst + first + 1, s_ttcState[first + 1]) : 0u;
        g_ttcAfter[(tileFirst / 2) + word] = low | (high << 16);
    }
}

// 区画 tile を groupshared へ写し、区画の中で不動点まで回して(回の終わりの壁の後、全員が同じ印を読んで止まる)記録を詰める。
// 区画が変わったら true。inner = 回した回(変わらないと分かった回を含む)・settled = 上限の内に着いた
struct TtcTileResult {
    bool changed;
    bool settled;
    uint32_t inner;
};

TtcTileResult TtcSolveTile(uint32_t tile, uint32_t thread) {
    const uint32_t tileFirst = tile * TTC_TILE_UNITS;
    const uint32_t tileUnits = min(TTC_TILE_UNITS, g_ttcUnits - tileFirst);

    // --- 区画を groupshared へ(前の区画の詰めるが読み終えてから)---
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint32_t k = 0; k < TTC_UNITS_PER_THREAD; ++k) {
        const uint32_t local = thread + (k * TTC_TILE_THREADS);
        if (local < tileUnits)
            s_ttcState[local] = g_ttcState[tileFirst + local];
    }

    if (thread < 3)
        s_ttcChanged[thread] = 0;

    GroupMemoryBarrierWithGroupSync();

    // --- 区画の中で不動点まで ---
    TtcTileResult result;
    result.changed = false;
    result.settled = false;
    result.inner = 0;
    while (result.inner < TTC_MAX_INNER) {
        const uint32_t slot = result.inner % 3;
        bool changed = false;
        [unroll] for (uint32_t k = 0; k < TTC_UNITS_PER_THREAD; ++k) {
            const uint32_t local = thread + (k * TTC_TILE_THREADS);
            if (local < tileUnits && TtcSweepOnce(tileFirst, tileUnits, local))
                changed = true;
        }

        if (changed)
            s_ttcChanged[slot] = 1;

        GroupMemoryBarrierWithGroupSync();
        ++result.inner;
        if (s_ttcChanged[slot] == 0) {
            result.settled = true;
            break;
        }

        result.changed = true;
        if (thread == 0)
            s_ttcChanged[(slot + 2) % 3] = 0;  // 前の回の印(全員が壁の前に読み終えた。次に書くのは 2 回後)
    }

    TtcPackTile(tileFirst, tileUnits, thread);

    return result;
}

void TtcCount(TtcTileResult result) {
    uint32_t previous = 0;
    if (!result.settled)
        InterlockedAdd(g_ttcCounters[TTC_UNSETTLED_SLOT], 1u, previous);

    InterlockedMax(g_ttcCounters[TTC_INNER_SLOT], result.inner, previous);
}

// 1 グループ = 1 区画。区画の中を回し、変わらなくなったら記録を詰める
[numthreads(TTC_TILE_THREADS, 1, 1)] void TtcSweep(uint32_t tile : SV_GroupID, uint32_t thread : SV_GroupIndex) {
    if (g_ttcPass > 0 && g_ttcCounters[g_ttcPass - 1] == 0)
        return;  // 前の回に変わった区画が無い = もう不動点(どのスレッドも同じ値を読む)

    const TtcTileResult result = TtcSolveTile(tile, thread);
    if (thread != 0)
        return;

    TtcCount(result);
    if (!result.changed)
        return;

    uint32_t previous = 0;
    InterlockedAdd(g_ttcCounters[g_ttcPass], 1u, previous);
    g_ttcTileChanged[tile] = g_ttcPass + 1;
}

// --- 仕上げ(1 グループ): 最後の回(g_ttcPass − 1)に変わった区画の隣を印し、印の付いた区画を 1 つずつ回す。
// 変わった区画は隣を印す。印が尽きたら不動点(最後の回に変わらなかった区画の隣の区画は、その回の終わりの値で既に着いている)---
groupshared uint32_t s_ttcDirty[TTC_DIRTY_WORDS];
groupshared uint32_t s_ttcNext;

void TtcMarkNeighbors(uint32_t tile, uint32_t thread) {
    const uint32_t end = g_ttcTileNeighborStarts[tile + 1];
    for (uint32_t k = g_ttcTileNeighborStarts[tile] + thread; k < end; k += TTC_TILE_THREADS) {
        const uint32_t neighbor = g_ttcTileNeighbors[k];
        uint32_t previous = 0;
        InterlockedOr(s_ttcDirty[neighbor >> 5], 1u << (neighbor & 31), previous);
    }
}

[numthreads(TTC_TILE_THREADS, 1, 1)] void TtcFixup(uint32_t thread : SV_GroupIndex) {
    if (g_ttcCounters[g_ttcPass - 1] == 0)
        return;  // 最後の回に変わった区画が無い = もう不動点

    const uint32_t tiles = (g_ttcUnits + TTC_TILE_UNITS - 1) / TTC_TILE_UNITS;
    for (uint32_t word = thread; word < TTC_DIRTY_WORDS; word += TTC_TILE_THREADS)
        s_ttcDirty[word] = 0;

    GroupMemoryBarrierWithGroupSync();
    for (uint32_t tile = 0; tile < tiles; ++tile) {
        if (g_ttcTileChanged[tile] == g_ttcPass)
            TtcMarkNeighbors(tile, thread);
    }

    uint32_t visits = 0;
    bool settled = false;
    while (visits < TTC_MAX_VISITS) {
        // --- 印の付いた最も小さい区画(全員が壁の後に同じ値を読む)---
        if (thread == 0)
            s_ttcNext = TTC_NO_UNIT;

        GroupMemoryBarrierWithGroupSync();
        if (thread < TTC_DIRTY_WORDS && s_ttcDirty[thread] != 0) {
            uint32_t previous = 0;
            InterlockedMin(s_ttcNext, (thread * 32) + firstbitlow(s_ttcDirty[thread]), previous);
        }

        GroupMemoryBarrierWithGroupSync();
        const uint32_t tile = s_ttcNext;
        if (tile == TTC_NO_UNIT) {
            settled = true;
            break;
        }

        GroupMemoryBarrierWithGroupSync();  // 全員が s_ttcNext を読んでから印を消す
        if (thread == 0)
            s_ttcDirty[tile >> 5] &= ~(1u << (tile & 31));

        const TtcTileResult result = TtcSolveTile(tile, thread);
        ++visits;
        if (thread == 0)
            TtcCount(result);

        if (result.changed)
            TtcMarkNeighbors(tile, thread);
    }

    if (thread != 0)
        return;

    uint32_t previous = 0;
    if (!settled)
        InterlockedAdd(g_ttcCounters[TTC_UNSETTLED_SLOT], 1u, previous);

    InterlockedAdd(g_ttcCounters[TTC_VISITS_SLOT], visits, previous);
    if (visits > 0)
        InterlockedAdd(g_ttcCounters[g_ttcPass], 1u, previous);
}
