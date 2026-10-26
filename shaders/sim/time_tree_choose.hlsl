// time_tree_choose.hlsl — 時間を飛ばす試作の刻みを選ぶ段(D-669 の窓・隣の差 1 段)の GPU 版(T-1112)。
// CPU リファレンスは engine/src/sim/time_step_tree.cpp の WindowCaps(窓の格子の 3 回の最小)と ChooseExponents
// (窓の上限・隣との差 1 段・長すぎる歩の途中の単位は打ち切る)。入力は単位ごとの詰めた歩の記録(2 B。PackTimeTreeUnits。
// ビット 0〜3 e・4〜7 望みの e・8 歩の途中・9 触った)、出力は窓の上限・選んだ e と、出来事の後の詰めた記録。
// 段の順(engine/src/sim/gpu_time_tree_choose.cpp が積む。間は UAV の壁):
//   TtcInit(単位: 窓の上限を上限 maxExponent に・次の e の初め・掃く回の印を 0 に)→ TtcScatter(格子: 望みの e を置く)
//   → TtcMinAxis × 3(x・y・z の順に半径の内の最小。端の外は数えない)→ TtcGather(格子: 単位の格子のセルの最小)
//   → TtcSweep × 回数(単位: 上限 = min(窓の上限, 隣の今の e + 1)。今の e が上限より長ければ縮める・歩の途中なら打ち切る)
//   → TtcPack(2 単位で 1 語: 出来事の後の詰めた記録)。
// 隣との差 1 段は単調に減るだけの決まり(打ち切った単位の次の e は 触った ? 0 : min(e, cap) の上限まで)なので、どの順で回しても
// 同じ最大の不動点に落ちる(CPU の回す順とビット一致)。掃く回は前の回に変わった単位が無ければ何もしない(g_ttcCounters)。
// ルートの SRV/UAV に GetDimensions を使わない(大きさはルート定数)。浮動小数点は使わない(04 §4)。
// 結び付け(gpu_time_tree_choose.cpp の ROOT_LAYOUT と同じ順)
RWStructuredBuffer<uint32_t> g_ttcCaps : register(u0);  // 単位ごと: 窓の上限
globallycoherent RWStructuredBuffer<uint32_t> g_ttcState
    : register(u1);                                         // 単位ごと: 今の e(下 8 bit)・歩の途中のまま・打ち切った
RWStructuredBuffer<uint32_t> g_ttcAfter : register(u2);     // 2 単位で 1 語: 出来事の後の詰めた記録
RWStructuredBuffer<uint32_t> g_ttcCounters : register(u3);  // 掃く回ごと: 変わった単位の数
RWStructuredBuffer<uint32_t> g_ttcWindowA : register(u4);   // 格子のセルごと(行き来する 2 つ)
RWStructuredBuffer<uint32_t> g_ttcWindowB : register(u5);
StructuredBuffer<uint32_t> g_ttcBefore : register(t0);          // 出来事ごとに 2 単位で 1 語: 選ぶ前の詰めた記録
StructuredBuffer<uint32_t> g_ttcGridUnits : register(t1);       // 格子のセル → 単位(無ければ TTC_NO_UNIT)
StructuredBuffer<uint32_t> g_ttcNeighborStarts : register(t2);  // 単位ごとの隣の始まり(単位の数 + 1)
StructuredBuffer<uint32_t> g_ttcNeighbors : register(t3);

cbuffer TimeTreeChooseConstants : register(b0) {
    uint32_t g_ttcUnits;
    uint32_t g_ttcGridX;
    uint32_t g_ttcGridY;
    uint32_t g_ttcGridZ;
    uint32_t g_ttcRadius;
    uint32_t g_ttcMaxExponent;
    uint32_t g_ttcCap;         // この刻みで始めてよい最も長い e
    uint32_t g_ttcPass;        // TtcMinAxis の軸(0〜2)・TtcSweep の回の番号
    uint32_t g_ttcBeforeWord;  // この出来事の選ぶ前の記録の始まり(語)
};

static const uint32_t TTC_NO_UNIT = 0xFFFFFFFFu;
static const uint32_t TTC_MAX_PASSES = 128;  // gpu_time_tree_choose.h の MAX_SWEEPS

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

// --- 単位: 窓の上限を上限に・次の e の初め(歩の途中なら今の e・閉じた単位は望みを cap まで)・掃く回の印 ---
[numthreads(64, 1, 1)] void TtcInit(uint32_t unit : SV_DispatchThreadID) {
    if (unit < TTC_MAX_PASSES)
        g_ttcCounters[unit] = 0;

    if (unit >= g_ttcUnits)
        return;

    const uint32_t record = TtcBefore(unit);
    g_ttcCaps[unit] = g_ttcMaxExponent;
    if ((record & TTC_RECORD_OPEN) != 0)
        g_ttcState[unit] = (record & TTC_RECORD_EXPONENT_MASK) | TTC_STATE_MIDSTEP;
    else
        g_ttcState[unit] = min(TtcDesired(record), g_ttcCap);
}

    // --- 格子: 望みの e を置く(単位の無いセルは上限)---
    [numthreads(64, 1, 1)] void TtcScatter(uint32_t cell : SV_DispatchThreadID) {
    if (cell >= g_ttcGridX * g_ttcGridY * g_ttcGridZ)
        return;

    const uint32_t unit = g_ttcGridUnits[cell];
    g_ttcWindowA[cell] = unit == TTC_NO_UNIT ? g_ttcMaxExponent : TtcDesired(TtcBefore(unit));
}

// --- 格子: 軸 g_ttcPass に沿って半径の内の最小(x: A → B・y: B → A・z: A → B)---
uint32_t TtcWindowRead(uint32_t cell) {
    return g_ttcPass == 1 ? g_ttcWindowB[cell] : g_ttcWindowA[cell];
}

[numthreads(64, 1, 1)] void TtcMinAxis(uint32_t cell : SV_DispatchThreadID) {
    if (cell >= g_ttcGridX * g_ttcGridY * g_ttcGridZ)
        return;

    const uint32_t x = cell % g_ttcGridX;
    const uint32_t y = (cell / g_ttcGridX) % g_ttcGridY;
    const uint32_t z = cell / (g_ttcGridX * g_ttcGridY);
    const uint32_t position = g_ttcPass == 0 ? x : (g_ttcPass == 1 ? y : z);
    const uint32_t length = g_ttcPass == 0 ? g_ttcGridX : (g_ttcPass == 1 ? g_ttcGridY : g_ttcGridZ);
    const uint32_t stride = g_ttcPass == 0 ? 1u : (g_ttcPass == 1 ? g_ttcGridX : g_ttcGridX * g_ttcGridY);
    const uint32_t low = position > g_ttcRadius ? position - g_ttcRadius : 0u;
    const uint32_t high = min(position + g_ttcRadius, length - 1);
    const uint32_t first = cell - (position * stride);

    uint32_t least = TtcWindowRead(first + (low * stride));
    for (uint32_t i = low + 1; i <= high; ++i)
        least = min(least, TtcWindowRead(first + (i * stride)));

    if (g_ttcPass == 1)
        g_ttcWindowA[cell] = least;
    else
        g_ttcWindowB[cell] = least;
}

    // --- 格子: 単位の格子のセルの最小(自分も入る)---
    [numthreads(64, 1, 1)] void TtcGather(uint32_t cell : SV_DispatchThreadID) {
    if (cell >= g_ttcGridX * g_ttcGridY * g_ttcGridZ)
        return;

    const uint32_t unit = g_ttcGridUnits[cell];
    if (unit == TTC_NO_UNIT)
        return;

    uint32_t previous = 0;
    InterlockedMin(g_ttcCaps[unit], g_ttcWindowB[cell], previous);
}

// --- 単位: 窓の上限と隣の差 1 段で縮める ---
// 1 回の Dispatch の中でも、隣の新しい値が見えれば続けて縮める(TTC_INNER_SWEEPS 回まで。g_ttcState は globallycoherent)。
// 打ち切った単位は奇数の刻み(cap 0)で e 0 になり、隣の歩の途中の単位を次々に打ち切る(火を付けた刻みで木の半分)ので、
// 1 回に 1 隣ずつでは木の差し渡しの回数が要る。値は減るだけなので、途中で古い値を読んでも着く不動点は同じ
static const uint32_t TTC_INNER_SWEEPS = 8;

// 上限まで縮める。変えたら true
bool TtcSweepOnce(uint32_t unit) {
    uint32_t limit = g_ttcCaps[unit];
    const uint32_t end = g_ttcNeighborStarts[unit + 1];
    for (uint32_t k = g_ttcNeighborStarts[unit]; k < end; ++k)
        limit = min(limit, (g_ttcState[g_ttcNeighbors[k]] & TTC_STATE_EXPONENT_MASK) + 1);

    const uint32_t state = g_ttcState[unit];
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

    g_ttcState[unit] = next | flags;

    return true;
}

[numthreads(64, 1, 1)] void TtcSweep(uint32_t unit : SV_DispatchThreadID) {
    if (unit >= g_ttcUnits)
        return;

    if (g_ttcPass > 0 && g_ttcCounters[g_ttcPass - 1] == 0)
        return;  // 前の回に変わった単位が無い = もう不動点

    bool changed = false;
    for (uint32_t inner = 0; inner < TTC_INNER_SWEEPS; ++inner) {
        if (!TtcSweepOnce(unit))
            break;

        changed = true;
        DeviceMemoryBarrier();
    }

    if (!changed)
        return;

    uint32_t previous = 0;
    InterlockedAdd(g_ttcCounters[g_ttcPass], 1u, previous);
}

// --- 2 単位で 1 語: 出来事の後の詰めた記録(歩の途中のままなら前のまま・始めた単位は e・望み・歩の途中)---
uint32_t TtcAfter(uint32_t unit) {
    const uint32_t record = TtcBefore(unit);
    const uint32_t state = g_ttcState[unit];
    if ((state & TTC_STATE_MIDSTEP) != 0)
        return record;

    const uint32_t exponent = record & TTC_RECORD_EXPONENT_MASK;
    const uint32_t restart = (record & TTC_RECORD_TOUCHED) != 0 ? 0u : exponent;
    const uint32_t desired = (state & TTC_STATE_TRUNCATED) != 0
                                 ? restart
                                 : ((record >> TTC_RECORD_DESIRED_SHIFT) & TTC_RECORD_EXPONENT_MASK);

    return (state & TTC_STATE_EXPONENT_MASK) | (desired << TTC_RECORD_DESIRED_SHIFT) | TTC_RECORD_OPEN;
}

[numthreads(64, 1, 1)] void TtcPack(uint32_t word : SV_DispatchThreadID) {
    const uint32_t first = word * 2;
    if (first >= g_ttcUnits)
        return;

    const uint32_t second = first + 1 < g_ttcUnits ? TtcAfter(first + 1) : 0u;
    g_ttcAfter[word] = TtcAfter(first) | (second << 16);
}
