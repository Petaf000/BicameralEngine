// probe_tick.hlsl — 仮の 1 刻み(common/probe_sim.hlsli の規則)の単位と、コマンドキュー・描画用の抽出(T-0004・T-0012・T-0086)。
//
// データの流れ(engine/src/sim/probe_sim.cpp がフレームの枠ごとのリストに毎フレーム記録する):
//   CPU がアップロードのバッファ(input)に見出しと新しいコマンドを書く
//   → EnqueueCommands が GPU のコマンドキュー(commandQueue)の末尾に足す(コマンドは自分の刻みの適用の単位まで、ここで待つ)
//   → そのフレームに投げる単位を順に(単位ごとに刻みの番号をルート定数に埋め込む。刻みはフレームをまたいでよい。ADR-0011)
//      適用 → 拡散 → 重さ × k → 検査と出力(HashBegin → HashCells、FlushEvents)→ 次の刻み …
//   → (刻みの境界の状態があれば)Extract が抽出の 3 組のどれかに写す(描画が読む)
//   → イベントとハッシュの表を CPU へ読み戻す(CPU は待たずに数フレーム後に読む。06 §3)
// 入口ごとに別の .cso にする(shaders/CMakeLists.txt)。整数だけ(D-205)。
#include "common/debug_ring.hlsli"
#include "common/probe_sim.hlsli"

RWStructuredBuffer<uint32_t> world : register(u0);        // 2 世代 × PROBE_CELL_COUNT
RWByteAddressBuffer events : register(u1);                // イベントのリング: 見出し + レコード(probe_sim.hlsli)
RWStructuredBuffer<uint32_t> extraction0 : register(u2);  // 描画用の抽出(3 組。描画は完成済みの最新を読む。06 §4)
RWStructuredBuffer<uint32_t> extraction1 : register(u3);
RWStructuredBuffer<uint32_t> busySink : register(u4);  // 重さの試験の計算結果の捨て場(誰も読まない)
RWStructuredBuffer<uint32_t> extraction2 : register(u5);
RWByteAddressBuffer hashes : register(u6);        // 刻みごとの状態のハッシュの表(probe_sim.hlsli)
RWByteAddressBuffer commandQueue : register(u7);  // GPU のコマンドキュー(環状。probe_sim.hlsli)
RWByteAddressBuffer tickEvents : register(u8);    // 刻みの中のイベントの一時置き場(順不同。刻みの最後に並べてリングへ)
ByteAddressBuffer input : register(t0);           // CPU が書くフレームの入力

cbuffer UnitConstants : register(b0) {
    uint32_t tickLow;  // この単位の刻み(記録するときに埋め込む)
    uint32_t tickHigh;
    uint32_t argument;  // Extract: 書き先の組
};

// --- 共通 ---

uint32_t HeaderWord(uint32_t index) {
    return input.Load(PROBE_INPUT_HEADER_OFFSET + index * 4);
}

uint64_t CurrentTick() {
    return (uint64_t)tickLow | ((uint64_t)tickHigh << 32);
}

// 刻み t の始めの状態がある世代の先頭(ADR-0003)
uint32_t GenerationBase(uint64_t tick) {
    return (uint32_t)(tick & 1) * PROBE_CELL_COUNT;
}

// 状態 S(stateTick) のハッシュの表の場所(容量は 2 の冪なので、下位 32bit の剰余と同じ)
uint32_t HashEntryAddress(uint64_t stateTick) {
    return ((uint32_t)stateTick & (PROBE_HASH_CAPACITY - 1)) * PROBE_HASH_ENTRY_BYTES;
}

// キューの position 番目(通し番号。2^32 で一周)のコマンドの場所
uint32_t QueueRecordAddress(uint32_t position) {
    return PROBE_COMMAND_QUEUE_HEADER_BYTES + (position & (PROBE_COMMAND_QUEUE_CAPACITY - 1)) * PROBE_COMMAND_BYTES;
}

// 刻みの中のイベントを一時置き場へ(順不同。並べるのは刻みの最後の FlushEvents)。溢れた分は書かない(書こうとした数だけ数える)
void EmitTickEvent(uint32_t type, uint32_t place) {
    uint32_t slot;
    tickEvents.InterlockedAdd(0, 1, slot);
    if (slot >= PROBE_TICK_EVENT_CAPACITY) return;
    tickEvents.Store2(PROBE_TICK_EVENT_HEADER_BYTES + slot * PROBE_TICK_EVENT_RECORD_BYTES, uint2(type, place));
}

// 1 つのコマンドを適用する(適用の単位の 1 スレッドが番号順に呼ぶ)。commandHead = 語 [0..3]、address = キューの中の場所
void ApplyCommand(uint64_t tick, uint4 commandHead, uint32_t address) {
    if ((commandHead.w & 0xFFFFu) != PROBE_COMMAND_TYPE_POKE) return;
    const uint2 cell = commandQueue.Load2(address + 16);
    DEBUG_ASSERT(cell.x < PROBE_GRID_SIZE && cell.y < PROBE_GRID_SIZE, DebugFormat::ProbePokeOutOfRange, cell.x,
                 cell.y);
    if (cell.x >= PROBE_GRID_SIZE || cell.y >= PROBE_GRID_SIZE) return;

    const uint32_t index = GenerationBase(tick) + ProbeCellIndex(cell.x, cell.y);
    world[index] = max(world[index], PROBE_POKE_AMOUNT);
    EmitTickEvent(PROBE_EVENT_POKE_APPLIED, ProbePokePlace(cell.x, cell.y));
}

// --- 刻みの最後にイベントを並べる(bitonic sort。1 グループ = PROBE_TICK_EVENT_CAPACITY スレッド)---
groupshared uint64_t g_eventKeys[PROBE_TICK_EVENT_CAPACITY];
groupshared uint32_t g_ringBase;

void SortEventKeys(uint32_t thread) {
    for (uint32_t size = 2; size <= PROBE_TICK_EVENT_CAPACITY; size <<= 1) {
        for (uint32_t stride = size >> 1; stride > 0; stride >>= 1) {
            GroupMemoryBarrierWithGroupSync();
            const uint32_t partner = thread ^ stride;
            if (partner > thread) {
                const uint64_t mine = g_eventKeys[thread];
                const uint64_t theirs = g_eventKeys[partner];
                const bool ascending = (thread & size) == 0;
                if ((mine > theirs) == ascending) {
                    g_eventKeys[thread] = theirs;
                    g_eventKeys[partner] = mine;
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
}

// clang-format は属性つき([numthreads])の入口が続くと並べ崩すので、ここから下(入口だけ)は整形を止める
// clang-format off

// --- フレームの始め: 新しいコマンドをキューの末尾へ(1 スレッド = 1 コマンド)---
// 足す場所(末尾)は CPU が見出しで渡す(足すのは CPU だけなので CPU が知っている)。容量を超えないことも CPU が守る
[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void EnqueueCommands(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t commandIndex = dispatchThreadId.x;
    const uint32_t count = HeaderWord(PROBE_HEADER_COMMAND_COUNT);
    const uint32_t base = HeaderWord(PROBE_HEADER_ENQUEUE_BASE);
    if (commandIndex == 0) {
        const uint32_t head = commandQueue.Load(PROBE_COMMAND_QUEUE_HEAD * 4);
        DEBUG_ASSERT(base + count - head <= PROBE_COMMAND_QUEUE_CAPACITY, DebugFormat::ProbeCommandQueueFull,
                     base + count - head);
        commandQueue.Store(PROBE_COMMAND_QUEUE_TAIL * 4, base + count);
    }
    if (commandIndex >= count) return;

    const uint32_t source = PROBE_INPUT_COMMANDS_OFFSET + commandIndex * PROBE_COMMAND_BYTES;
    const uint32_t destination = QueueRecordAddress(base + commandIndex);
    for (uint32_t offset = 0; offset < PROBE_COMMAND_BYTES; offset += 16) {
        commandQueue.Store4(destination + offset, input.Load4(source + offset));
    }
}

// --- [0] コマンドの適用: 1 スレッドがキューの先頭から番号順に(06 §3「同じ刻みの中は sequence の順」)---
// キューは (targetTick, sequence) の昇順なので、targetTick が今の刻みを超えたら止まる。今の刻みより前のもの(遅れて届いた)は捨てて知らせる。
// 順番に依存する本物のコマンドもこの形で決定的に適用できる。数が増えて 1 スレッドで重くなったら、種類ごとに分ける(T-0005 以降)
[numthreads(1, 1, 1)] void ApplyCommands() {
    const uint64_t tick = CurrentTick();
    const uint32_t tail = commandQueue.Load(PROBE_COMMAND_QUEUE_TAIL * 4);
    uint32_t head = commandQueue.Load(PROBE_COMMAND_QUEUE_HEAD * 4);
    for (uint32_t visited = 0; visited < PROBE_COMMAND_QUEUE_CAPACITY && head != tail; ++visited) {
        const uint32_t address = QueueRecordAddress(head);
        const uint4 commandHead = commandQueue.Load4(address);  // targetTick の下位・上位、sequence、type | size
        const uint64_t targetTick = (uint64_t)commandHead.x | ((uint64_t)commandHead.y << 32);
        if (targetTick > tick) break;
        if (targetTick == tick) {
            ApplyCommand(tick, commandHead, address);
        } else {
            DEBUG_ASSERT(false, DebugFormat::ProbeCommandLate, (uint32_t)targetTick, (uint32_t)tick);
            EmitTickEvent(PROBE_EVENT_COMMAND_LATE, commandHead.w & 0xFFFFu);
        }
        ++head;
    }
    commandQueue.Store(PROBE_COMMAND_QUEUE_HEAD * 4, head);
}

// --- [1] 拡散: 1 スレッド = 1 セル(gather。前の世代を読み、次の世代に書く)---
[numthreads(PROBE_GROUP_SIZE, PROBE_GROUP_SIZE, 1)] void Diffuse(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t x = dispatchThreadId.x;
    const uint32_t y = dispatchThreadId.y;
    const uint64_t tick = CurrentTick();
    const uint32_t current = GenerationBase(tick);
    const uint32_t next = GenerationBase(tick + 1);
    const uint32_t cellIndex = ProbeCellIndex(x, y);

    const uint32_t self = world[current + cellIndex];
    const uint32_t left = x > 0 ? world[current + cellIndex - 1] : 0;
    const uint32_t right = x + 1 < PROBE_GRID_SIZE ? world[current + cellIndex + 1] : 0;
    const uint32_t up = y > 0 ? world[current + cellIndex - PROBE_GRID_SIZE] : 0;
    const uint32_t down = y + 1 < PROBE_GRID_SIZE ? world[current + cellIndex + PROBE_GRID_SIZE] : 0;
    world[next + cellIndex] = ProbeDiffuseValue(self, left, right, up, down);
}

// --- [2 .. 2 + k) 重さの試験(--sim-load・--sim-split。R-LOOP-2)---
// 結果に入らない計算で刻みを重くする。前の単位の結果(捨て場)から続けて計算する(分けた分だけ順につながる。
// 実際の刻みを分けて投げるときと同じく、前が終わるまで次は始められない)。見出しの繰り返し回数は 1 個あたり
[numthreads(PROBE_GROUP_SIZE, PROBE_GROUP_SIZE, 1)] void Busy(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t iterations = HeaderWord(PROBE_HEADER_BUSY_ITERATIONS);
    if (iterations == 0) return;
    const uint32_t cellIndex = ProbeCellIndex(dispatchThreadId.x, dispatchThreadId.y);
    uint32_t hash = busySink[cellIndex] ^ cellIndex;
    for (uint32_t i = 0; i < iterations; ++i) {
        hash = hash * 1664525u + 1013904223u;
    }
    busySink[cellIndex] = hash;
}

// --- [最後] 検査と出力: S(t + 1) の要約を表の (t + 1) % 容量 番目へ(06 §2 段 9)---
// HashBegin(1 スレッド)が見出しを書いて和を 0 にし、UAV バリアの後に HashCells が全セルの寄与を足す

[numthreads(1, 1, 1)] void HashBegin() {
    const uint64_t stateTick = CurrentTick() + 1;
    hashes.Store4(HashEntryAddress(stateTick), uint4((uint32_t)stateTick, (uint32_t)(stateTick >> 32), 0, 0));
}

// 1 スレッド = 1 セル。wave の中で和を取り、代表の 1 レーンが 64bit の atomic で足す(和は順番に依存しない)
[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void HashCells(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t cellIndex = dispatchThreadId.x;
    const uint64_t stateTick = CurrentTick() + 1;
    const uint64_t contribution =
        cellIndex < PROBE_CELL_COUNT ? ProbeCellHash(cellIndex, world[GenerationBase(stateTick) + cellIndex]) : 0;
    const uint64_t waveSum = WaveActiveSum(contribution);
    if (WaveIsFirstLane()) {
        uint64_t original;
        hashes.InterlockedAdd64(HashEntryAddress(stateTick) + 8, waveSum, original);
    }
}

// 刻みの一時置き場のイベントをキー(種類・場所)で並べ、刻みの順にリングへ写して一時置き場を空にする(06 §3・§5)。
// 1 グループだけ起動する。リングの空きは代表の 1 スレッドがまとめて取る(刻みのイベントがリングの中で連続する)
[numthreads(PROBE_TICK_EVENT_CAPACITY, 1, 1)] void FlushEvents(uint3 groupThreadId : SV_GroupThreadID) {
    const uint32_t thread = groupThreadId.x;
    const uint32_t requested = tickEvents.Load(0);
    const uint32_t stored = min(requested, PROBE_TICK_EVENT_CAPACITY);
    const uint2 record = tickEvents.Load2(PROBE_TICK_EVENT_HEADER_BYTES + thread * PROBE_TICK_EVENT_RECORD_BYTES);
    g_eventKeys[thread] = thread < stored ? ProbeEventKey(record.x, record.y) : PROBE_U64(0xFFFFFFFFu, 0xFFFFFFFFu);
    SortEventKeys(thread);

    if (thread == 0) {
        uint32_t base;
        events.InterlockedAdd(PROBE_EVENT_HEADER_REQUESTED * 4, stored, base);
        g_ringBase = base;
        uint32_t previous;
        if (requested > stored) events.InterlockedAdd(PROBE_EVENT_HEADER_TICK_DROPPED * 4, requested - stored, previous);
        tickEvents.Store(0, 0);
    }
    GroupMemoryBarrierWithGroupSync();

    const uint32_t slot = g_ringBase + thread;
    if (thread >= stored || slot >= PROBE_EVENT_CAPACITY) return;
    const uint64_t tick = CurrentTick();
    const uint64_t key = g_eventKeys[thread];
    events.Store4(PROBE_EVENT_HEADER_BYTES + slot * PROBE_EVENT_WORDS * 4,
                  uint4((uint32_t)tick, (uint32_t)(tick >> 32), (uint32_t)(key >> 32), (uint32_t)key));
}

// --- 描画用の抽出: 刻み(ルート定数)の始めの状態を、抽出の 3 組のうち argument の組へ写す ---
[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void Extract(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t cellIndex = dispatchThreadId.x;
    if (cellIndex >= PROBE_CELL_COUNT) return;
    const uint32_t value = world[GenerationBase(CurrentTick()) + cellIndex];
    if (argument == 0) {
        extraction0[cellIndex] = value;
    } else if (argument == 1) {
        extraction1[cellIndex] = value;
    } else {
        extraction2[cellIndex] = value;
    }
}

// clang-format on
