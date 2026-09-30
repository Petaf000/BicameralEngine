// probe_tick.hlsl — 仮の 1 刻み(common/probe_sim.hlsli の規則)の単位と、描画用の抽出(T-0004・T-0012)。
//
// データの流れ(engine/src/sim/probe_sim.cpp がフレームの枠ごとのリストに毎フレーム記録する):
//   CPU がアップロードのバッファ(input)に見出しとコマンドを書き、そのフレームに投げる単位を順に記録する
//   (単位ごとに刻みの番号をルート定数に埋め込む。刻みはフレームをまたいでよい。ADR-0011)
//   → 適用 → 拡散 → 重さ × k → ハッシュ(HashBegin → HashCells)→ 次の刻み …
//   → (刻みの境界の状態があれば)Extract が抽出の 3 組のどれかに写す(描画が読む)
//   → イベントとハッシュの表を CPU へ読み戻す(CPU は待たずに数フレーム後に読む。06 §3)
// 入口ごとに別の .cso にする(shaders/CMakeLists.txt)。整数だけ(D-205)。
#include "common/debug_ring.hlsli"
#include "common/probe_sim.hlsli"

RWStructuredBuffer<uint32_t> world : register(u0);        // 2 世代 × PROBE_CELL_COUNT
RWByteAddressBuffer events : register(u1);                // 見出し + レコード(probe_sim.hlsli)
RWStructuredBuffer<uint32_t> extraction0 : register(u2);  // 描画用の抽出(3 組。描画は完成済みの最新を読む。06 §4)
RWStructuredBuffer<uint32_t> extraction1 : register(u3);
RWStructuredBuffer<uint32_t> busySink : register(u4);  // 重さの試験の計算結果の捨て場(誰も読まない)
RWStructuredBuffer<uint32_t> extraction2 : register(u5);
RWByteAddressBuffer hashes : register(u6);  // 刻みごとの状態のハッシュの表(probe_sim.hlsli)
ByteAddressBuffer input : register(t0);     // CPU が書くフレームの入力

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

// clang-format は属性つき([numthreads])の入口が続くと並べ崩すので、ここから下(入口だけ)は整形を止める
// clang-format off

// --- [0] コマンドの適用: 1 スレッド = 1 コマンド ---

[numthreads(PROBE_LINEAR_GROUP_SIZE, 1, 1)] void ApplyCommands(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t commandIndex = dispatchThreadId.x;
    if (commandIndex >= HeaderWord(PROBE_HEADER_COMMAND_COUNT)) return;

    const uint32_t address = PROBE_INPUT_COMMANDS_OFFSET + commandIndex * PROBE_COMMAND_BYTES;
    const uint4 head = input.Load4(address);  // targetTick の下位・上位、sequence、type | size
    const uint64_t tick = CurrentTick();
    const uint64_t targetTick = (uint64_t)head.x | ((uint64_t)head.y << 32);
    if (targetTick != tick || (head.w & 0xFFFFu) != PROBE_COMMAND_TYPE_POKE) return;

    const uint2 cell = input.Load2(address + 16);
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

// --- [最後] ハッシュ: S(t + 1) の要約を表の (t + 1) % 容量 番目へ(06 §2 段 9)---
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
