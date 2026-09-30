// probe_tick.hlsl — T-0004 の仮の 1 刻み(common/probe_sim.hlsli の規則)と、描画用の抽出。
//
// データの流れ(engine/src/sim/probe_sim.cpp が記録するバッチのリスト):
//   CPU がアップロードのバッファ(batch)に見出しとコマンドを書き、刻みの数 n 用に記録したリストを投げる
//   → 刻み k = 0..n-1 ごとに ApplyCommands → Diffuse → Busy × 分けた数(k はリストに埋め込んだルート定数)
//   → Extract が最後の世代を抽出(描画が読む 3 組のどれか)に写す
//   → つつきを適用したことを events に追記(CPU は待たずに数フレーム後に読む。06 §3)
// 入口ごとに別の .cso にする(shaders/CMakeLists.txt)。整数だけ(D-205)。
#include "common/debug_ring.hlsli"
#include "common/probe_sim.hlsli"

RWStructuredBuffer<uint32_t> world : register(u0);        // 2 世代 × PROBE_CELL_COUNT
RWByteAddressBuffer events : register(u1);                // 見出し + レコード(probe_sim.hlsli)
RWStructuredBuffer<uint32_t> extraction0 : register(u2);  // 描画用の抽出(3 組。描画は完成済みの最新を読む。06 §4)
RWStructuredBuffer<uint32_t> extraction1 : register(u3);
RWStructuredBuffer<uint32_t> extraction2 : register(u5);
RWStructuredBuffer<uint32_t> busySink : register(u4);  // 重さの試験の計算結果の捨て場(誰も読まない)
ByteAddressBuffer batch : register(t0);                // CPU が書くバッチの入力

cbuffer TickSlot : register(b0) {
    uint32_t tickSlot;  // この Dispatch がバッチの何番目の刻みか(記録済みのリストに埋め込んだ定数)
};

// --- バッチの見出し ---

uint32_t HeaderWord(uint32_t index) {
    return batch.Load(PROBE_BATCH_HEADER_OFFSET + index * 4);
}

uint64_t FirstTick() {
    return (uint64_t)HeaderWord(PROBE_HEADER_FIRST_TICK_LOW) |
           ((uint64_t)HeaderWord(PROBE_HEADER_FIRST_TICK_HIGH) << 32);
}

uint64_t CurrentTick() {
    return FirstTick() + tickSlot;
}

// 刻み t の始めの状態がある世代の先頭(ADR-0003)
uint32_t GenerationBase(uint64_t tick) {
    return (uint32_t)(tick & 1) * PROBE_CELL_COUNT;
}

// --- (1) コマンドの適用: 1 スレッド = 1 コマンド ---

[numthreads(PROBE_COMMAND_GROUP_SIZE, 1, 1)] void ApplyCommands(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t commandIndex = dispatchThreadId.x;
    if (commandIndex >= HeaderWord(PROBE_HEADER_COMMAND_COUNT)) return;

    const uint32_t address = PROBE_BATCH_COMMANDS_OFFSET + commandIndex * PROBE_COMMAND_BYTES;
    const uint4 head = batch.Load4(address);  // targetTick の下位・上位、sequence、type | size
    const uint64_t tick = CurrentTick();
    const uint64_t targetTick = (uint64_t)head.x | ((uint64_t)head.y << 32);
    if (targetTick != tick || (head.w & 0xFFFFu) != PROBE_COMMAND_TYPE_POKE) return;

    const uint2 cell = batch.Load2(address + 16);
    DEBUG_ASSERT(cell.x < PROBE_GRID_SIZE && cell.y < PROBE_GRID_SIZE, DebugFormat::ProbePokeOutOfRange, cell.x,
                 cell.y);
    if (cell.x >= PROBE_GRID_SIZE || cell.y >= PROBE_GRID_SIZE) return;

    // max なので、同じセルへの複数のコマンドの順番に結果が依存しない(04 R2)
    uint32_t previous;
    InterlockedMax(world[GenerationBase(tick) + ProbeCellIndex(cell.x, cell.y)], PROBE_POKE_AMOUNT, previous);

    // 適用したことを CPU へ知らせる。溢れた分は書かない(書こうとした数 − 容量 = 落とした数)
    uint32_t slot;
    events.InterlockedAdd(0, 1, slot);
    if (slot < PROBE_EVENT_CAPACITY) {
        events.Store4(PROBE_EVENT_HEADER_BYTES + slot * PROBE_EVENT_WORDS * 4,
                      uint4((uint32_t)tick, (uint32_t)(tick >> 32), PROBE_EVENT_POKE_APPLIED, cell.x | (cell.y << 16)));
    }
}

    // --- (2) 拡散: 1 スレッド = 1 セル(gather。前の世代を読み、次の世代に書く)---

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
    const uint32_t value = ProbeDiffuseValue(self, left, right, up, down);
    world[next + cellIndex] = value;
}

// --- (3) 重さの試験(--sim-load・--sim-split。R-LOOP-2、T-0085)---
// 結果に入らない計算で刻みを重くする。1 刻みの重さを何個の Dispatch に分けて投げるかを試すため、前の Dispatch の結果
// (捨て場)から続けて計算する(分けた分だけ順につながる。実際の刻みを分けて投げるときと同じく、前が終わるまで次は始められない)。
// 見出しの繰り返し回数は 1 個あたり(0 なら何もしない)。捨て場は誰も読まない
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

    // --- 描画用の抽出: バッチの最後の状態を、抽出の 3 組のうち見出しが指す組へ写す ---

    [numthreads(PROBE_COMMAND_GROUP_SIZE, 1, 1)] void Extract(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t cellIndex = dispatchThreadId.x;
    if (cellIndex >= PROBE_CELL_COUNT) return;
    const uint64_t endTick = FirstTick() + HeaderWord(PROBE_HEADER_TICK_COUNT);
    const uint32_t value = world[GenerationBase(endTick) + cellIndex];
    const uint32_t target = HeaderWord(PROBE_HEADER_EXTRACTION_TARGET);
    if (target == 0) {
        extraction0[cellIndex] = value;
    } else if (target == 1) {
        extraction1[cellIndex] = value;
    } else {
        extraction2[cellIndex] = value;
    }
}
