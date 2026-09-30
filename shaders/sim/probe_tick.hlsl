// probe_tick.hlsl — 仮の 1 刻み(common/probe_sim.hlsli の規則)の compute の単位と、コマンドキュー・描画用の抽出(T-0004・T-0012・T-0086・T-0005)。
//
// データの流れ(engine/src/sim/probe_sim.cpp がフレームの枠ごとのリストに毎フレーム記録する):
//   CPU がアップロードのバッファ(input)に見出しと新しいコマンドを書く
//   → EnqueueCommands が GPU のコマンドキュー(commandQueue)の末尾に足す(コマンドは自分の刻みの適用の単位まで、ここで待つ)
//   → そのフレームに投げる単位を順に(単位ごとに刻みの番号をルート定数に埋め込む。刻みはフレームをまたいでよい。ADR-0011)
//      適用(ApplyCommands)→ 伝導(Work Graph。sim/probe_conduct.hlsl)→ 重さ × k → 検査と出力(HashCells、FlushEvents)→ 次の刻み …
//   → (刻みの境界の状態があれば)Extract が全部のセルと活性の印を抽出の 3 組のどれかに写す(描画が読む。T-0015)
//   → イベントとハッシュの表を CPU へ読み戻す(CPU は待たずに数フレーム後に読む。06 §3)
// 入口ごとに別の .cso にする(shaders/CMakeLists.txt)。整数だけ(D-205)。バッファの結び方は sim/probe_bindings.hlsli。
#include "sim/probe_bindings.hlsli"

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
// つつき: 熱を足し、そのブロックを刻み t の活性の一覧へ(伝導の Work Graph が、そのブロックと隣を起こす)
void ApplyCommand(uint64_t tick, uint4 commandHead, uint32_t address) {
    if ((commandHead.w & 0xFFFFu) != PROBE_COMMAND_TYPE_POKE) return;
    const uint3 cell = commandQueue.Load3(address + 16);
    const bool inside = all(cell < PROBE_GRID_SIZE);
    DEBUG_ASSERT(inside, DebugFormat::ProbePokeOutOfRange, cell.x, cell.y, cell.z);
    if (!inside) return;

    const uint32_t index = GenerationBase(tick) + ProbeCellIndex(cell.x, cell.y, cell.z);
    world[index] = ProbeAddHeat(world[index], PROBE_POKE_AMOUNT);
    AppendActiveBlock((uint32_t)(tick & 1), ProbeBlockOfCell(cell.x, cell.y, cell.z));
    EmitTickEvent(PROBE_EVENT_POKE_APPLIED, ProbePokePlace(cell.x, cell.y, cell.z));
}

// 活性の一覧の見出し(D3D12_NODE_GPU_INPUT)の、数以外(入口・レコードのアドレス・間隔)を書く。アドレスは CPU が入力の見出しで渡す
void WriteActiveListDescriptor(RWByteAddressBuffer list, uint32_t parity) {
    const uint32_t entrypoint = HeaderWord(PROBE_HEADER_CONDUCT_ENTRYPOINT);
    const uint32_t addressLow = HeaderWord(PROBE_HEADER_ACTIVE_LIST_ADDRESS + parity * 2);
    const uint32_t addressHigh = HeaderWord(PROBE_HEADER_ACTIVE_LIST_ADDRESS + parity * 2 + 1);
    // レコードは見出しの直後(64bit のアドレスに見出しの大きさを足す。下位の繰り上がりも)
    const uint32_t recordsLow = addressLow + PROBE_ACTIVE_LIST_HEADER_BYTES;
    const uint32_t recordsHigh = addressHigh + (recordsLow < addressLow ? 1u : 0u);
    list.Store(PROBE_ACTIVE_LIST_ENTRYPOINT * 4, entrypoint);
    list.Store4(PROBE_ACTIVE_LIST_ADDRESS * 4, uint4(recordsLow, recordsHigh, 4, 0));
}

// 一覧を「先頭の何もしない 1 件だけ」にする(レコードの数を 0 にしない。probe_sim.hlsli)
void ResetActiveList(RWByteAddressBuffer list) {
    list.Store(PROBE_ACTIVE_LIST_COUNT * 4, 1);
    list.Store(PROBE_ACTIVE_LIST_HEADER_BYTES, PROBE_NO_BLOCK);
}

// 刻み t の始め: 刻み t の一覧(つつきを足す先)の見出しを整え、刻み t + 1 の一覧を空にし、S(t + 1) の表の欄を用意する。
// 刻み t の一覧は刻み t − 1 の始めに空にしてある。刻み 0 だけは作った時の 0 件なので、ここで空にする
void BeginTick(uint64_t tick) {
    const uint32_t parity = (uint32_t)(tick & 1);
    if (parity == 0) {
        WriteActiveListDescriptor(activeList0, 0);
        WriteActiveListDescriptor(activeList1, 1);
        if (tick == 0) ResetActiveList(activeList0);
        ResetActiveList(activeList1);
    } else {
        WriteActiveListDescriptor(activeList1, 1);
        WriteActiveListDescriptor(activeList0, 0);
        ResetActiveList(activeList0);
    }
    const uint64_t stateTick = tick + 1;
    const uint32_t entry = HashEntryAddress(stateTick);
    hashes.Store4(entry, uint4((uint32_t)stateTick, (uint32_t)(stateTick >> 32), 0, 0));
    hashes.Store4(entry + PROBE_HASH_OFFSET_HEAT, uint4(0, 0, 0, 0));
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
    BeginTick(tick);
    const uint32_t tail = commandQueue.Load(PROBE_COMMAND_QUEUE_TAIL * 4);
    uint32_t head = commandQueue.Load(PROBE_COMMAND_QUEUE_HEAD * 4);
    WgGaugePeak(PROBE_STATS_GAUGE_COMMAND_QUEUE, tail - head);  // 待っている数(T-0008)
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

    // この刻みの活性の一覧はここで出来上がる(前の刻みの伝導 + この刻みのつつき)。長さの最大を計器へ(T-0008)
    const uint32_t countAddress = PROBE_ACTIVE_LIST_COUNT * 4;
    WgGaugePeak(PROBE_STATS_GAUGE_ACTIVE_LIST,
                (tick & 1) == 0 ? activeList0.Load(countAddress) : activeList1.Load(countAddress));
}

// --- [1] 伝導は Work Graph(sim/probe_conduct.hlsl)---

// --- [2 .. 2 + k) 重さの試験(--sim-load・--sim-split。R-LOOP-2)---
// 結果に入らない計算で刻みを重くする。前の単位の結果(捨て場)から続けて計算する(分けた分だけ順につながる。
// 実際の刻みを分けて投げるときと同じく、前が終わるまで次は始められない)。見出しの繰り返し回数は 1 個あたり
[numthreads(PROBE_GROUP_SIZE, PROBE_GROUP_SIZE, 1)] void Busy(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t iterations = HeaderWord(PROBE_HEADER_BUSY_ITERATIONS);
    if (iterations == 0) return;
    const uint32_t cellIndex = dispatchThreadId.y * PROBE_GRID_SIZE + dispatchThreadId.x;  // 捨て場は 1 つの面の大きさ
    uint32_t hash = busySink[cellIndex] ^ cellIndex;
    for (uint32_t i = 0; i < iterations; ++i) {
        hash = hash * 1664525u + 1013904223u;
    }
    busySink[cellIndex] = hash;
}

// --- [最後] 検査と出力: S(t + 1) の要約と熱の合計を表の (t + 1) % 容量 番目へ(06 §2 段 9)---
// 欄は刻みの適用の単位(BeginTick)が 0 にしてある。1 スレッド = 1 セル。wave の中で和を取り、代表の 1 レーンが 64bit の atomic で足す
// (和は順番に依存しない。04 R2)
[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void HashCells(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t cellIndex = dispatchThreadId.x;
    const uint64_t stateTick = CurrentTick() + 1;
    const bool inside = cellIndex < PROBE_CELL_COUNT;
    const uint32_t value = inside ? world[GenerationBase(stateTick) + cellIndex] : 0;
    const uint64_t hashSum = WaveActiveSum(inside ? ProbeCellHash(cellIndex, value) : 0);
    const uint64_t heatSum = WaveActiveSum((uint64_t)value);
    if (WaveIsFirstLane()) {
        const uint32_t entry = HashEntryAddress(stateTick);
        uint64_t original;
        hashes.InterlockedAdd64(entry + PROBE_HASH_OFFSET_HASH, hashSum, original);
        hashes.InterlockedAdd64(entry + PROBE_HASH_OFFSET_HEAT, heatSum, original);
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

// --- 描画用の抽出: 刻み(ルート定数)の始めの状態の全部のセルと、ブロックごとの活性の印を、抽出の 3 組のうち argument の組へ写す ---
// 活性の印: 予定の印(最後に計算した刻み + 1)が「この刻み」か「この刻み + 1」= 前の刻みか、途中まで進んだこの刻みで計算した。
// 刻みの境界(単位 0)で写すなら前の刻みの分だけになり、ハッシュの表の「計算したブロックの数」と同じ数になる。
void StoreExtraction(uint32_t index, uint32_t value) {
    if (argument == 0) {
        extraction0[index] = value;
    } else if (argument == 1) {
        extraction1[index] = value;
    } else {
        extraction2[index] = value;
    }
}

[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void Extract(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t cellIndex = dispatchThreadId.x;
    if (cellIndex >= PROBE_CELL_COUNT) return;
    StoreExtraction(cellIndex, world[GenerationBase(CurrentTick()) + cellIndex]);
    if (cellIndex >= PROBE_BLOCK_COUNT) return;
    const uint32_t mark = blockSchedule[cellIndex];
    const uint32_t tickLow32 = (uint32_t)CurrentTick();
    const bool computed = mark != 0 && (mark == tickLow32 || mark == tickLow32 + 1);
    StoreExtraction(PROBE_EXTRACTION_BLOCK_OFFSET + cellIndex, computed ? 1 : 0);
}

// clang-format on
