// probe_bindings.hlsli — 仮の刻みのシェーダー(probe_tick.hlsl の compute と probe_conduct.hlsl の Work Graph)が共有する
// バッファの結び方と、刻み・世代・表の場所の計算。ルート署名は engine/src/sim/probe_sim.cpp の ROOT_LAYOUT と同じ順。
// Work Graph のノードも同じグローバルのルート署名で動く(ノードごとのローカルのルート署名は使わない)。
#ifndef BICAMERAL_PROBE_BINDINGS_HLSLI
#define BICAMERAL_PROBE_BINDINGS_HLSLI

#include "common/debug_ring.hlsli"
#include "common/graph_trace.hlsli"  // u2 space1(連鎖のトレース。T-0087)
#include "common/probe_sim.hlsli"
#include "common/probe_world.hlsli"
#include "common/work_graph_stats.hlsli"  // u1 space1(ノードのカウンタ。T-0008)

// --- バッファ(ROOT_LAYOUT の順)---
RWStructuredBuffer<RxCell> cells : register(u0);          // 2 世代 × PROBE_CELL_COUNT(成分 + エネルギー。T-0089)
RWByteAddressBuffer events : register(u1);                // イベントのリング: 見出し + レコード(probe_sim.hlsli)
RWStructuredBuffer<uint32_t> extraction0 : register(u2);  // 描画用の抽出(3 組。中身は probe_sim.hlsli。06 §4)
RWStructuredBuffer<uint32_t> extraction1 : register(u3);
RWStructuredBuffer<uint32_t> busySink : register(u4);  // 重さの試験の計算結果の捨て場(誰も読まない)
RWStructuredBuffer<uint32_t> extraction2 : register(u5);
RWByteAddressBuffer hashes : register(u6);        // 刻みごとの状態のハッシュの表(probe_sim.hlsli)
RWByteAddressBuffer commandQueue : register(u7);  // GPU のコマンドキュー(環状。probe_sim.hlsli)
RWByteAddressBuffer tickEvents : register(u8);    // 刻みの中のイベントの一時置き場(順不同。刻みの最後に並べてリングへ)
RWByteAddressBuffer activeList0 : register(u9);   // 活性の一覧(偶数の刻み)。伝導の間は GPU の入力(読むだけ)の状態
RWByteAddressBuffer activeList1 : register(u10);  // 活性の一覧(奇数の刻み)
RWStructuredBuffer<uint32_t> blockSchedule : register(u11);  // ブロックごとの予定の印(最後に予定した刻み + 1)
RWStructuredBuffer<HcThermalCache> thermal
    : register(u12);                     // 2 世代 × PROBE_CELL_COUNT(セルの熱のキャッシュ。cells と同じ並び)
ByteAddressBuffer input : register(t0);  // CPU が書くフレームの入力
StructuredBuffer<RxSpecies> reactionSpecies : register(t1);  // 反応の表(ベイクしたもの。sim/reaction_table.h)
StructuredBuffer<RxRule> reactionRules : register(t2);
StructuredBuffer<uint32_t> reactionRuleIndex : register(t3);
ByteAddressBuffer reactionRates : register(t4);

cbuffer UnitConstants : register(b0) {
    uint32_t tickLow;  // この単位の刻み(記録するときに埋め込む)
    uint32_t tickHigh;
    uint32_t argument;  // Extract: 書き先の組
};

// reaction.hlsli の Table の約束(表の読み方)
struct ProbeReactionTable {
    uint32_t unused;

    RxSpecies Species(uint32_t id) { return reactionSpecies[id]; }

    RxRule Rule(uint32_t id) { return reactionRules[id]; }

    uint32_t RuleIndex(uint32_t position) { return reactionRuleIndex[position]; }

    uint64_t Rate(uint32_t rule, uint32_t kelvin) {
        return reactionRates.Load<uint64_t>((rule * RX_RATE_TABLE_KELVINS + kelvin) * 8);
    }
};

ProbeReactionTable ReactionTable() {
    ProbeReactionTable table;
    table.unused = 0;

    return table;
}

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

// 状態 S(stateTick) のハッシュの表の欄の場所(容量は 2 の冪なので、下位 32bit の剰余と同じ)
uint32_t HashEntryAddress(uint64_t stateTick) {
    return ((uint32_t)stateTick & (PROBE_HASH_CAPACITY - 1)) * PROBE_HASH_ENTRY_BYTES;
}

// ブロックの座標(ProbeBlockIndex の逆)
uint3 BlockCoordinates(uint32_t block) {
    return uint3(block % PROBE_BLOCKS_PER_AXIS, (block / PROBE_BLOCKS_PER_AXIS) % PROBE_BLOCKS_PER_AXIS,
                 block / (PROBE_BLOCKS_PER_AXIS * PROBE_BLOCKS_PER_AXIS));
}

// 連鎖のトレースの範囲の箱にブロックが入るか(場所の単位はブロックの座標)
bool TraceWantsBlock(uint32_t block) {
    return GtWantsPlace(BlockCoordinates(block));
}

// 活性の一覧に 1 ブロック足す(parity = 刻みの偶奇)。容量は probe_sim.hlsli の約束で足りる(超えたら assert)
void AppendActiveBlock(uint32_t parity, uint32_t block) {
    uint32_t slot;
    if (parity == 0)
        activeList0.InterlockedAdd(PROBE_ACTIVE_LIST_COUNT * 4, 1, slot);
    else
        activeList1.InterlockedAdd(PROBE_ACTIVE_LIST_COUNT * 4, 1, slot);

    DEBUG_ASSERT(slot < PROBE_ACTIVE_LIST_CAPACITY, DebugFormat::ProbeActiveListFull, slot);
    if (slot >= PROBE_ACTIVE_LIST_CAPACITY)
        return;

    const uint32_t address = PROBE_ACTIVE_LIST_HEADER_BYTES + slot * 4;
    if (parity == 0)
        activeList0.Store(address, block);
    else
        activeList1.Store(address, block);
}

#endif  // BICAMERAL_PROBE_BINDINGS_HLSLI
