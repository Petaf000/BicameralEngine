// multires_tree.hlsl — 多重解像度の世界の木の管理の Compute の段(17 §5「木の管理」。T-0018。ADR-0016)。
// 1 刻みの要求の処理: (静かな葉を粗くするなら TreeQuiet。T-0101)→ TreeResolve → TreeSettle → TreeAllocate → (Work Graph: RefineNode の鎖・CoarsenRequestNode)→ TreeRelease
// → TreeClearIndex → TreeFillIndex(後ろの 2 つは索引を作り直す印がある時だけ働く)。呼ぶ順は engine/src/sim/gpu_multires.cpp の
// RecordProcessRequests。CPU リファレンスは engine/src/sim/multires_tree.cpp(同じ関数・同じ順)。
//
// 順番に依存しない理由: 解決と確定は要求ごとに自分の値だけを書き、取り合いは atomic の最小(小さい番号が勝つ)。
// 割り当てと解放は 1 グループの累積和で、要求の順に枠を配る・積む(ADR-0016)。数える欄は足し算。
#include "sim/multires_bindings.hlsli"

static const uint32_t TREE_THREADS = 64;

groupshared uint32_t gs_blockScan[MR_MAX_REQUESTS];
groupshared uint32_t gs_fractionScan[MR_MAX_REQUESTS];
groupshared uint32_t gs_grantedBlocks;
groupshared uint32_t gs_grantedFractions;
groupshared uint32_t gs_refineCount;
groupshared uint32_t gs_coarsenCount;

uint32_t RequestCount() {
    return g_counters[MR_COUNTER_REQUESTS];
}

// 2 つの値の累積和(含む)を 1 グループで。i = スレッドの番号
void InclusiveScan(uint32_t i, uint32_t blockValue, uint32_t fractionValue) {
    gs_blockScan[i] = blockValue;
    gs_fractionScan[i] = fractionValue;
    GroupMemoryBarrierWithGroupSync();
    for (uint32_t offset = 1; offset < MR_MAX_REQUESTS; offset <<= 1) {
        uint32_t blocks = gs_blockScan[i];
        uint32_t fractions = gs_fractionScan[i];
        if (i >= offset) {
            blocks += gs_blockScan[i - offset];
            fractions += gs_fractionScan[i - offset];
        }

        GroupMemoryBarrierWithGroupSync();
        gs_blockScan[i] = blocks;
        gs_fractionScan[i] = fractions;
        GroupMemoryBarrierWithGroupSync();
    }
}

// --- 0. 静かな葉を粗くする要求(T-0101。1 グループ。multires_activity.hlsli の MrWantsQuietCoarsen)---
// 外から渡された要求(数は MR_COUNTER_REQUESTS)の後ろに、世界の枠の順に足す。枠を MR_MAX_REQUESTS 個ずつ区切り、区切りの中の位置は累積和で決める。
// 一覧が一杯なら残りは足さずに数える(次の刻みにまた作られる)

[numthreads(MR_MAX_REQUESTS, 1, 1)] void TreeQuiet(uint32_t i : SV_GroupIndex) {
    const uint32_t mark = MrActivityMark(FX_U64(g_tickHigh, g_tickLow));
    const uint32_t base = RequestCount();
    uint32_t wanted = 0;  // ここまでの区切りで粗くしたい葉の数(グループで一様)
    for (uint32_t first = 0; first < g_worldBlocks; first += MR_MAX_REQUESTS) {
        const uint32_t slot = first + i;
        const bool wants = slot < g_worldBlocks && MrWantsQuietCoarsen(MakeTree(), slot, mark);
        InclusiveScan(i, wants ? 1u : 0u, 0u);

        const uint32_t position = base + wanted + gs_blockScan[i] - 1;
        if (wants && position < MR_MAX_REQUESTS)
            g_requests[position] = MrMakeQuietCoarsenRequest(g_blocks[slot]);

        wanted += gs_blockScan[MR_MAX_REQUESTS - 1];
        GroupMemoryBarrierWithGroupSync();  // 次の区切りが累積和を書き直す前に、全部が読み終える
    }

    if (i != 0)
        return;

    const uint32_t added = min(wanted, MR_MAX_REQUESTS - base);
    g_counters[MR_COUNTER_REQUESTS] = base + added;
    g_counters[MR_COUNTER_QUIET_REQUESTS] += added;
    g_counters[MR_COUNTER_QUIET_DEFERRED] += wanted - added;
}

// --- 1. 解決 ---

MrRequestState ResolveRefine(MrRequest request, MrRequestState state, uint32_t i) {
    const uint32_t root = LookupBlock(g_rootLevel,
                                      MrBlockOriginOf(MrCoarserCoordinate(request.x, request.level, g_rootLevel)),
                                      MrBlockOriginOf(MrCoarserCoordinate(request.y, request.level, g_rootLevel)),
                                      MrBlockOriginOf(MrCoarserCoordinate(request.z, request.level, g_rootLevel)));
    if (root == MR_NO_BLOCK) {
        state.status = MR_STATUS_INVALID;
        return state;
    }

    // --- 子の番号をたどって、点を含む最も深い本物のブロックへ ---
    uint32_t deepest = root;
    for (uint32_t step = 0; step <= MR_MAX_LEVEL_SPAN; ++step) {
        const MrBlock block = g_blocks[deepest];
        if (block.level >= request.level)
            break;

        const uint32_t child = block.children[MrOctantOfPoint(block, request.x, request.y, request.z, request.level)];
        if (child == MR_NO_BLOCK)
            break;

        deepest = child;
    }

    const int32_t deepestLevel = g_blocks[deepest].level;
    if (deepestLevel == request.level) {
        state.status = MR_STATUS_ALREADY;
        return state;
    }

    state.target = deepest;
    state.claimSlot = deepest;
    state.levels = min((uint32_t)(request.level - deepestLevel), MR_MAX_CHAIN_LEVELS);
    InterlockedMin(g_claims[deepest], i);

    return state;
}

MrRequestState ResolveCoarsen(MrRequest request, MrRequestState state, uint32_t i) {
    const uint32_t slot = LookupBlock(request.level, MrBlockOriginOf(request.x), MrBlockOriginOf(request.y),
                                      MrBlockOriginOf(request.z));
    if (slot == MR_NO_BLOCK || g_blocks[slot].parent == MR_NO_BLOCK || MrHasRealChild(g_blocks[slot])) {
        state.status = MR_STATUS_INVALID;
        return state;
    }

    const uint32_t parent = g_blocks[slot].parent;
    state.target = slot;
    state.claimSlot = parent;
    InterlockedMin(g_claims[parent], i);

    return state;
}

[numthreads(TREE_THREADS, 1, 1)] void TreeResolve(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t i = dispatchThreadId.x;
    if (i >= RequestCount())
        return;

    const MrRequest request = g_requests[i];
    MrRequestState state = MrMakeRequestState();
    const int32_t span = request.level - g_rootLevel;
    if (span < 0 || span > (int32_t)MR_MAX_LEVEL_SPAN)
        state.status = MR_STATUS_INVALID;
    else if (request.op == MR_REQUEST_REFINE)
        state = ResolveRefine(request, state, i);
    else if (request.op == MR_REQUEST_COARSEN)
        state = ResolveCoarsen(request, state, i);
    else
        state.status = MR_STATUS_INVALID;

    g_states[i] = state;
}

    // --- 2. 確定 ---

    [numthreads(TREE_THREADS, 1, 1)] void TreeSettle(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t i = dispatchThreadId.x;
    if (i >= RequestCount())
        return;

    MrRequestState state = g_states[i];
    if (state.status != MR_STATUS_PENDING)
        return;

    const bool coarsen = g_requests[i].op == MR_REQUEST_COARSEN;
    const bool lost = g_claims[state.claimSlot] != i;
    const bool targetTaken = coarsen && g_claims[state.target] != MR_NO_CLAIM;
    if (lost || targetTaken) {
        state.status = MR_STATUS_CONFLICT;
    } else if (coarsen) {
        const uint32_t parent = g_blocks[state.target].parent;
        state.fractionNeed = g_blocks[parent].fraction == MR_NO_FRACTION ? 1u : 0u;
    } else {
        state.blockNeed = state.levels;
        state.fractionNeed = g_blocks[state.target].fraction != MR_NO_FRACTION ? state.levels : 0u;
    }

    g_states[i] = state;
}

// --- 3. 割り当て(1 グループ)と Work Graph の GPU の入力 ---

void StoreGraphHeader(uint32_t header, uint32_t entry, uint32_t recordCount, uint32_t recordOffset,
                      uint32_t strideBytes) {
    const uint64_t base = FX_U64(g_graphInputHigh, g_graphInputLow);
    const uint64_t records = base + recordOffset;
    g_graphInput.Store2(header, uint2(entry, recordCount));
    g_graphInput.Store4(header + 8, uint4((uint32_t)records, (uint32_t)(records >> 32), strideBytes, 0));
}

void StoreRefineRecord(uint32_t position, uint32_t request, uint32_t parentSlot, uint32_t levels) {
    const uint32_t address = MR_GRAPH_INPUT_REFINE_RECORDS + position * MR_REFINE_RECORD_BYTES;
    g_graphInput.Store3(address, uint3(request, parentSlot, 0));
    g_graphInput.Store3(address + 12, uint3(levels, MR_BLOCK_REAL, 0));
}

[numthreads(MR_MAX_REQUESTS, 1, 1)] void TreeAllocate(uint32_t i : SV_GroupIndex) {
    const uint32_t count = RequestCount();
    const uint32_t freeBlocks = g_counters[MR_COUNTER_FREE_BLOCKS];
    const uint32_t freeFractions = g_counters[MR_COUNTER_FREE_FRACTIONS];
    MrRequestState state = MrMakeRequestState();
    state.status = MR_STATUS_INVALID;
    if (i < count)
        state = g_states[i];

    const bool pending = i < count && state.status == MR_STATUS_PENDING;
    if (i == 0) {
        gs_grantedBlocks = 0;
        gs_grantedFractions = 0;
        gs_refineCount = 0;
        gs_coarsenCount = 0;
    }

    InclusiveScan(i, pending ? state.blockNeed : 0u, pending ? state.fractionNeed : 0u);

    // --- 許可: 累積和が空きの数以下(許可は一覧の前から続く)---
    const uint32_t blockSum = gs_blockScan[i];
    const uint32_t fractionSum = gs_fractionScan[i];
    const bool granted = pending && blockSum <= freeBlocks && fractionSum <= freeFractions;
    if (granted) {
        state.status = MR_STATUS_GRANTED;
        state.blockBase = freeBlocks - (blockSum - state.blockNeed);
        state.fractionBase = freeFractions - (fractionSum - state.fractionNeed);
        InterlockedMax(gs_grantedBlocks, blockSum);
        InterlockedMax(gs_grantedFractions, fractionSum);
    } else if (pending) {
        state.status = MR_STATUS_NO_SPACE;
    }

    if (i < count)
        g_states[i] = state;

    // --- GPU の入力の一覧(並びは結果に関係しない)---
    const bool refine = granted && g_requests[i].op == MR_REQUEST_REFINE;
    if (refine) {
        uint32_t position;
        InterlockedAdd(gs_refineCount, 1u, position);
        StoreRefineRecord(position, i, state.target, state.levels);
    } else if (granted) {
        uint32_t position;
        InterlockedAdd(gs_coarsenCount, 1u, position);
        g_graphInput.Store(MR_GRAPH_INPUT_COARSEN_RECORDS + position * 4, i);
    }

    GroupMemoryBarrierWithGroupSync();
    if (i != 0)
        return;

    g_counters[MR_COUNTER_FREE_BLOCKS] = freeBlocks - gs_grantedBlocks;
    g_counters[MR_COUNTER_FREE_FRACTIONS] = freeFractions - gs_grantedFractions;

    // レコードを 0 件にしない(WARP が固まる。何もしないレコードを 1 件)
    if (gs_refineCount == 0)
        StoreRefineRecord(0, MR_NO_BLOCK, 0, 0);

    if (gs_coarsenCount == 0)
        g_graphInput.Store(MR_GRAPH_INPUT_COARSEN_RECORDS, MR_NO_BLOCK);

    StoreGraphHeader(MR_GRAPH_INPUT_REFINE_HEADER, g_graphEntries & 0xFFFFu, max(gs_refineCount, 1u),
                     MR_GRAPH_INPUT_REFINE_RECORDS, MR_REFINE_RECORD_BYTES);
    StoreGraphHeader(MR_GRAPH_INPUT_COARSEN_HEADER, g_graphEntries >> 16, max(gs_coarsenCount, 1u),
                     MR_GRAPH_INPUT_COARSEN_RECORDS, 4);
}

    // --- 5. 解放(1 グループ): 返す枠を要求の順に積む・取り合いの印を消す・数える ---

    [numthreads(MR_MAX_REQUESTS, 1, 1)] void TreeRelease(uint32_t i : SV_GroupIndex) {
    const uint32_t count = RequestCount();
    const uint32_t topBlocks = g_counters[MR_COUNTER_FREE_BLOCKS];
    const uint32_t topFractions = g_counters[MR_COUNTER_FREE_FRACTIONS];
    MrRequestState state = MrMakeRequestState();
    state.status = MR_STATUS_INVALID;
    if (i < count)
        state = g_states[i];

    const bool granted = i < count && state.status == MR_STATUS_GRANTED;
    const uint32_t blockRelease = granted && state.releaseBlock != MR_NO_BLOCK ? 1u : 0u;
    const uint32_t fractionRelease = granted ? state.releaseCount : 0u;
    if (i == 0) {
        gs_grantedBlocks = 0;
        gs_grantedFractions = 0;
    }

    InclusiveScan(i, blockRelease, fractionRelease);

    if (i < count) {
        if (state.claimSlot != MR_NO_CLAIM)
            g_claims[state.claimSlot] = MR_NO_CLAIM;

        InterlockedAdd(g_counters[MrStatusCounter(state.status)], 1u);
    }

    const uint32_t fractionBase = topFractions + gs_fractionScan[i] - fractionRelease;
    for (uint32_t k = 0; k < fractionRelease; ++k)
        g_freeFractions[fractionBase + k] = state.releases[k];

    if (blockRelease != 0)
        g_freeBlocks[topBlocks + gs_blockScan[i] - 1] = state.releaseBlock;

    InterlockedMax(gs_grantedBlocks, gs_blockScan[i]);
    InterlockedMax(gs_grantedFractions, gs_fractionScan[i]);
    GroupMemoryBarrierWithGroupSync();
    if (i != 0)
        return;

    g_counters[MR_COUNTER_FREE_BLOCKS] = topBlocks + gs_grantedBlocks;
    g_counters[MR_COUNTER_FREE_FRACTIONS] = topFractions + gs_grantedFractions;

    // --- 墓石が表の 1/4 を超えたら作り直す ---
    const bool rebuild = g_counters[MR_COUNTER_TOMBSTONES] * 4 > g_indexEntries;
    g_counters[MR_COUNTER_REBUILD_INDEX] = rebuild ? 1u : 0u;
    if (rebuild)
        g_counters[MR_COUNTER_TOMBSTONES] = 0;

    g_counters[MR_COUNTER_REQUESTS] = 0;
}

// --- 6. 索引の作り直し(印がある時だけ)---

[numthreads(TREE_THREADS, 1, 1)] void TreeClearIndex(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t i = dispatchThreadId.x;
    if (g_counters[MR_COUNTER_REBUILD_INDEX] == 0 || i >= g_indexEntries)
        return;

    g_index[i] = MR_INDEX_EMPTY;
}

    [numthreads(TREE_THREADS, 1, 1)] void TreeFillIndex(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t slot = dispatchThreadId.x;
    if (g_counters[MR_COUNTER_REBUILD_INDEX] == 0 || slot >= g_worldBlocks)
        return;

    if (g_blocks[slot].kind == MR_BLOCK_REAL)
        IndexInsert(slot);
}
