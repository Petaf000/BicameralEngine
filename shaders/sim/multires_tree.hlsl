// multires_tree.hlsl — 多重解像度の世界の木の管理の Compute の段(17 §5「木の管理」。T-0018。ADR-0016)。
// 1 刻みの要求の処理: (静かな葉を粗くするなら TreeQuiet。T-0101)→ TreeResolve → TreeSettle → TreeAllocate → (Work Graph: RefineNode の鎖・CoarsenRequestNode)→ TreeRelease
// → TreeClearIndex → TreeFillIndex(後ろの 2 つは索引を作り直す印がある時だけ働く)。
// 要求の処理の前: TreeFoldCheck → TreeFold(静かで一様・ほぼ同じになった頁を枠の順に畳む・端数の枠を返す。T-0103・T-0112)。
// 刻んだ後: TreeExpand(一様で反応が進むブロックに枠の順で頁を配る。T-0102)。呼ぶ順は engine/src/sim/gpu_multires.cpp の
// RecordProcessRequests。CPU リファレンスは engine/src/sim/multires_tree.cpp(同じ関数・同じ順)。
//
// 順番に依存しない理由: 解決と確定は要求ごとに自分の値だけを書き、取り合いは atomic の最小(小さい番号が勝つ)。
// 割り当てと解放は 1 グループの累積和で、要求の順に枠を配る・積む(ADR-0016)。数える欄は足し算。
#include "sim/multires_bindings.hlsli"

static const uint32_t TREE_THREADS = 64;

groupshared uint32_t gs_blockScan[MR_MAX_REQUESTS];
groupshared uint32_t gs_fractionScan[MR_MAX_REQUESTS];
groupshared uint32_t gs_pageScan[MR_MAX_REQUESTS];
groupshared uint32_t gs_grantedBlocks;
groupshared uint32_t gs_grantedFractions;
groupshared uint32_t gs_grantedPages;
groupshared uint32_t gs_refineCount;
groupshared uint32_t gs_coarsenCount;

uint32_t RequestCount() {
    return g_counters[MR_COUNTER_REQUESTS];
}

// 3 つの値(枠・端数・頁)の累積和(含む)を 1 グループで。i = スレッドの番号
void InclusiveScan(uint32_t i, uint32_t blockValue, uint32_t fractionValue, uint32_t pageValue) {
    gs_blockScan[i] = blockValue;
    gs_fractionScan[i] = fractionValue;
    gs_pageScan[i] = pageValue;
    GroupMemoryBarrierWithGroupSync();
    for (uint32_t offset = 1; offset < MR_MAX_REQUESTS; offset <<= 1) {
        uint32_t blocks = gs_blockScan[i];
        uint32_t fractions = gs_fractionScan[i];
        uint32_t pages = gs_pageScan[i];
        if (i >= offset) {
            blocks += gs_blockScan[i - offset];
            fractions += gs_fractionScan[i - offset];
            pages += gs_pageScan[i - offset];
        }

        GroupMemoryBarrierWithGroupSync();
        gs_blockScan[i] = blocks;
        gs_fractionScan[i] = fractions;
        gs_pageScan[i] = pages;
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
        InclusiveScan(i, wants ? 1u : 0u, 0u, 0u);

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

// --- 0'. 静かな葉を粗くできるか(T-0113。D-430。TreeQuiet の前。multires_activity.hlsli の MrCoarsenGroupWithin)---
// 1 グループ = 世界の枠 1 つ、スレッド = 子のセル 2×2×2 の組(64)。今の忙しさの印でまだ調べていない静かな葉だけ
// (グループで一様な分岐)。組のどれかが許容差を超えたら粗くできない。結果を見出しの quietCheck に(CPU の CheckQuietLeaves)

groupshared uint32_t gs_quietBlocked;  // 許容差を超えた組があった

// MrCoarsenGroupWithin の Cells の約束(頁のセルと温度)
struct QuietPageCells {
    uint32_t page;

    RxCell Cell(uint32_t index) { return g_cells[PageCellAddress(page, index)]; }

    int32_t Temperature(RxCell cell) { return RxComputeThermal(MakeTable(), cell).temperature; }
};

[numthreads(MR_OCTANT_CELLS, 1, 1)] void TreeQuietCheck(uint32_t group : SV_GroupIndex, uint3 id : SV_GroupID) {
    const uint32_t slot = id.x;
    if (slot >= g_worldBlocks)
        return;

    const uint32_t mark = MrActivityMark(FX_U64(g_tickHigh, g_tickLow));
    const MrBlock block = g_blocks[slot];
    if (!MrNeedsQuietCheck(block, mark))
        return;

    // --- 一様な葉はいつも粗くできる ---
    if (MrIsUniform(block)) {
        if (group == 0)
            g_blocks[slot].quietCheck = MrQuietCheckStamp(block.busyTick, true);

        return;
    }

    if (group == 0)
        gs_quietBlocked = 0;

    GroupMemoryBarrierWithGroupSync();
    QuietPageCells cells;
    cells.page = block.page;
    if (!MrCoarsenGroupWithin(cells, group, MrUnpackFoldTolerance(g_foldTolerance)))
        InterlockedOr(gs_quietBlocked, 1u);

    GroupMemoryBarrierWithGroupSync();
    if (group == 0)
        g_blocks[slot].quietCheck = MrQuietCheckStamp(block.busyTick, gs_quietBlocked == 0);
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
    InterlockedMin(g_treeWords[TreeClaimAddress(deepest)], i);

    return state;
}

// 子のブロック childSlot を親へ粗くしても、親の八分の一の 64 セルのどれも成分が入りきるか(T-0022。CPU の CoarsenFits。
// 1 スレッドで 64 セル。粗くする要求は少ないので並べない)
bool CoarsenFits(uint32_t childSlot) {
    const MrBlock child = g_blocks[childSlot];
    bool fits = true;
    for (uint32_t local = 0; local < MR_OCTANT_CELLS && fits; ++local) {
        MrChildren children;
        for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
            const uint32_t index = MrChildCell(local, j);
            children.cells[j] = LoadBlockCell(child, childSlot, index);
            children.fractions[j] = LoadFraction(child.fraction, index);
        }

        fits = MrCoarsenFits(children);
    }

    return fits;
}

MrRequestState ResolveCoarsen(MrRequest request, MrRequestState state, uint32_t i) {
    const uint32_t slot = LookupBlock(request.level, MrBlockOriginOf(request.x), MrBlockOriginOf(request.y),
                                      MrBlockOriginOf(request.z));
    if (slot == MR_NO_BLOCK || g_blocks[slot].parent == MR_NO_BLOCK || MrHasRealChild(g_blocks[slot])) {
        state.status = MR_STATUS_INVALID;
        return state;
    }

    // --- 成分が入りきらなければ粗くしない。静かな葉なら、今の忙しさの印では粗くできないことにする(毎刻み要求し直さない)---
    if (!CoarsenFits(slot)) {
        g_blocks[slot].quietCheck = MrQuietCheckStamp(g_blocks[slot].busyTick, false);
        state.status = MR_STATUS_SPECIES_FULL;
        return state;
    }

    const uint32_t parent = g_blocks[slot].parent;
    state.target = slot;
    state.claimSlot = parent;
    InterlockedMin(g_treeWords[TreeClaimAddress(parent)], i);

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
    const bool lost = g_treeWords[TreeClaimAddress(state.claimSlot)] != i;
    const bool targetTaken = coarsen && g_treeWords[TreeClaimAddress(state.target)] != MR_NO_CLAIM;
    if (lost || targetTaken) {
        state.status = MR_STATUS_CONFLICT;
    } else if (coarsen) {
        const uint32_t parent = g_blocks[state.target].parent;
        state.fractionNeed = g_blocks[parent].fraction == MR_NO_FRACTION ? 1u : 0u;
        state.pageNeed = MrCoarsenPageNeed(g_blocks[state.target], g_cells[state.target], g_blocks[parent],
                                           g_cells[parent]);
    } else {
        // 一様な親の鎖は全部一様(頁なし)、頁を持つ親の鎖は全部頁を持つ(T-0102)
        state.blockNeed = state.levels;
        state.fractionNeed = g_blocks[state.target].fraction != MR_NO_FRACTION ? state.levels : 0u;
        state.pageNeed = MrIsUniform(g_blocks[state.target]) ? 0u : state.levels;
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
    const uint32_t freePages = g_counters[MR_COUNTER_FREE_PAGES];
    MrRequestState state = MrMakeRequestState();
    state.status = MR_STATUS_INVALID;
    if (i < count)
        state = g_states[i];

    const bool pending = i < count && state.status == MR_STATUS_PENDING;
    if (i == 0) {
        gs_grantedBlocks = 0;
        gs_grantedFractions = 0;
        gs_grantedPages = 0;
        gs_refineCount = 0;
        gs_coarsenCount = 0;
    }

    InclusiveScan(i, pending ? state.blockNeed : 0u, pending ? state.fractionNeed : 0u, pending ? state.pageNeed : 0u);

    // --- 許可: 累積和が空きの数以下(許可は一覧の前から続く)---
    const uint32_t blockSum = gs_blockScan[i];
    const uint32_t fractionSum = gs_fractionScan[i];
    const uint32_t pageSum = gs_pageScan[i];
    const bool granted = pending && blockSum <= freeBlocks && fractionSum <= freeFractions && pageSum <= freePages;
    if (granted) {
        state.status = MR_STATUS_GRANTED;
        state.blockBase = freeBlocks - (blockSum - state.blockNeed);
        state.fractionBase = freeFractions - (fractionSum - state.fractionNeed);
        state.pageBase = freePages - (pageSum - state.pageNeed);
        InterlockedMax(gs_grantedBlocks, blockSum);
        InterlockedMax(gs_grantedFractions, fractionSum);
        InterlockedMax(gs_grantedPages, pageSum);
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
    g_counters[MR_COUNTER_FREE_PAGES] = freePages - gs_grantedPages;

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
    const uint32_t topPages = g_counters[MR_COUNTER_FREE_PAGES];
    MrRequestState state = MrMakeRequestState();
    state.status = MR_STATUS_INVALID;
    if (i < count)
        state = g_states[i];

    const bool granted = i < count && state.status == MR_STATUS_GRANTED;
    const uint32_t blockRelease = granted && state.releaseBlock != MR_NO_BLOCK ? 1u : 0u;
    const uint32_t fractionRelease = granted ? state.releaseCount : 0u;
    const uint32_t pageRelease = granted && state.releasePage != MR_NO_PAGE ? 1u : 0u;
    if (i == 0) {
        gs_grantedBlocks = 0;
        gs_grantedFractions = 0;
        gs_grantedPages = 0;
    }

    InclusiveScan(i, blockRelease, fractionRelease, pageRelease);

    if (i < count) {
        if (state.claimSlot != MR_NO_CLAIM)
            g_treeWords[TreeClaimAddress(state.claimSlot)] = MR_NO_CLAIM;

        InterlockedAdd(g_counters[MrStatusCounter(state.status)], 1u);
    }

    const uint32_t fractionBase = topFractions + gs_fractionScan[i] - fractionRelease;
    for (uint32_t k = 0; k < fractionRelease; ++k)
        g_freeFractions[fractionBase + k] = state.releases[k];

    if (blockRelease != 0)
        g_treeWords[topBlocks + gs_blockScan[i] - 1] = state.releaseBlock;

    if (pageRelease != 0)
        g_treeWords[FreePageAddress(topPages + gs_pageScan[i] - 1)] = state.releasePage;

    InterlockedMax(gs_grantedBlocks, gs_blockScan[i]);
    InterlockedMax(gs_grantedFractions, gs_fractionScan[i]);
    InterlockedMax(gs_grantedPages, gs_pageScan[i]);
    GroupMemoryBarrierWithGroupSync();
    if (i != 0)
        return;

    g_counters[MR_COUNTER_FREE_BLOCKS] = topBlocks + gs_grantedBlocks;
    g_counters[MR_COUNTER_FREE_FRACTIONS] = topFractions + gs_grantedFractions;
    g_counters[MR_COUNTER_FREE_PAGES] = topPages + gs_grantedPages;

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

    g_treeWords[TreeIndexAddress(i)] = MR_INDEX_EMPTY;
}

    [numthreads(TREE_THREADS, 1, 1)] void TreeFillIndex(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t slot = dispatchThreadId.x;
    if (g_counters[MR_COUNTER_REBUILD_INDEX] == 0 || slot >= g_worldBlocks)
        return;

    if (g_blocks[slot].kind == MR_BLOCK_REAL)
        IndexInsert(slot);
}

// --- 7. 頁に広げる(刻んだ後。1 グループ。T-0102。multires_nest.cpp の ExpandWantedPages)---
// 刻む段が MR_PAGE_WANTED を付けた世界のブロックに、枠の順で頁の空きのスタックの上から頁を配り、一覧(MR_GRAPH_INPUT_EXPAND_*)に書く。
// 頁が足りなければ一様に戻して数え、活性の一覧(u13 = 次の刻みの種)に足す。埋めて刻むのは StepExpanded / ExpandStepNode
// (伝導を入れた刻みでは ConductPrepare が埋め、ConductApply が刻む。配った・凍らせた印を伝導の作業場に書く。T-0107)。
// 伝導の小刻み(T-0109)では小刻みごとに呼ぶ。印は小刻みごと、種は 1 刻みに 1 回だけ足す(AppendSeedOnce)

[numthreads(MR_MAX_REQUESTS, 1, 1)] void TreeExpand(uint32_t i : SV_GroupIndex) {
    const uint32_t freePages = g_counters[MR_COUNTER_FREE_PAGES];
    uint32_t wanted = 0;  // ここまでの区切りで頁に広げたいブロックの数(グループで一様)
    for (uint32_t first = 0; first < g_worldBlocks; first += MR_MAX_REQUESTS) {
        const uint32_t slot = first + i;
        const bool wants = slot < g_worldBlocks && g_blocks[slot].page == MR_PAGE_WANTED;
        InclusiveScan(i, wants ? 1u : 0u, 0u, 0u);

        const uint32_t position = wanted + gs_blockScan[i] - 1;
        if (wants && position < freePages) {
            g_blocks[slot].page = g_treeWords[FreePageAddress(freePages - 1 - position)];
            g_graphInput.Store(MR_GRAPH_INPUT_EXPAND_RECORDS + 4 * (position + 1), slot);
            SetSubstepMark(slot, CONDUCT_MARK_EXPANDED);
        } else if (wants) {
            g_blocks[slot].page = MR_NO_PAGE;
            AppendSeedOnce(slot);
            SetSubstepMark(slot, CONDUCT_MARK_FROZEN);  // 伝導: この小刻みは刻まず、面の流れも 0(T-0107)
        }

        wanted += gs_blockScan[MR_MAX_REQUESTS - 1];
        GroupMemoryBarrierWithGroupSync();  // 次の区切りが累積和を書き直す前に、全部が読み終える
    }

    if (i != 0)
        return;

    const uint32_t granted = min(wanted, freePages);
    g_counters[MR_COUNTER_FREE_PAGES] = freePages - granted;
    g_counters[MR_COUNTER_EXPANDED] += granted;
    g_counters[MR_COUNTER_PAGE_SHORTAGE] += wanted - granted;
    g_graphInput.Store(MR_GRAPH_INPUT_EXPAND_RECORDS, MR_NO_BLOCK);  // レコードを 0 件にしない
    g_graphInput.Store(MR_GRAPH_INPUT_EXPAND_HEADER + 4, granted + 1);
}

// --- 7b. 伝導の端数の枠を配る(頁を配った後。1 グループ。T-0107。multires_conduction.cpp の AllocateConductionFractions)---
// 伝導で粗い側に端数が要る(CONDUCT_MARK_FRACTION)・凍らせていない・端数の枠の無い世界のブロックに、枠の順で空きのスタックの上から配る。
// 足りなければ数える(そのブロックへは整数の単位の倍数だけ送る。ADR-0017)。配った枠を空にするのは ConductPrepare。
// 伝導の一覧の数を Work Graph の見出し(流れ・足す・小刻みの終わり)にも写す(一覧は印の段で出来上がっている)。小刻みごとに呼ぶ(T-0109)

// 伝導で端数の枠が要る(印がある・凍らせていない・枠が無い)世界のブロックか
bool WantsConductionFraction(uint32_t slot) {
    return slot < g_worldBlocks && HasSubstepMark(slot, CONDUCT_MARK_FRACTION) &&
           !HasSubstepMark(slot, CONDUCT_MARK_FROZEN) && g_blocks[slot].fraction == MR_NO_FRACTION;
}

[numthreads(MR_MAX_REQUESTS, 1, 1)] void TreeFractions(uint32_t i : SV_GroupIndex) {
    const uint32_t freeFractions = g_counters[MR_COUNTER_FREE_FRACTIONS];
    uint32_t wanted = 0;  // ここまでの区切りで端数の枠が要るブロックの数(グループで一様)
    for (uint32_t first = 0; first < g_worldBlocks; first += MR_MAX_REQUESTS) {
        const uint32_t slot = first + i;
        const bool wants = WantsConductionFraction(slot);
        InclusiveScan(i, wants ? 1u : 0u, 0u, 0u);

        const uint32_t position = wanted + gs_blockScan[i] - 1;
        if (wants && position < freeFractions) {
            g_blocks[slot].fraction = g_freeFractions[freeFractions - 1 - position];
            SetSubstepMark(slot, CONDUCT_MARK_GRANTED);
        }

        wanted += gs_blockScan[MR_MAX_REQUESTS - 1];
        GroupMemoryBarrierWithGroupSync();  // 次の区切りが累積和を書き直す前に、全部が読み終える
    }

    if (i != 0)
        return;

    const uint32_t granted = min(wanted, freeFractions);
    g_counters[MR_COUNTER_FREE_FRACTIONS] = freeFractions - granted;
    g_counters[MR_COUNTER_FRACTION_SHORTAGE] += wanted - granted;

    // --- 伝導の一覧はここで出来上がっている(印の段の後)。Work Graph の見出し(流れ・足す・小刻みの終わり)に数を写す ---
    const uint32_t records = g_graphInput.Load(MR_GRAPH_INPUT_CONDUCT_HEADER + 4);
    g_graphInput.Store(MR_GRAPH_INPUT_CONDUCT_FLOWS_HEADER + 4, records);
    g_graphInput.Store(MR_GRAPH_INPUT_CONDUCT_APPLY_HEADER + 4, records);
    g_graphInput.Store(MR_GRAPH_INPUT_CONDUCT_END_HEADER + 4, records);
}

// --- 8. 頁を畳む(要求の処理の前。T-0103・T-0112。multires_activity.cpp の FoldQuietPages)---
// TreeFoldCheck(1 グループ = 世界の枠 1 つ、スレッド = セル): ちょうど静かになったブロックを調べ、取り合いの印 [枠] に
// 「畳む・返す」の印(MR_CLAIM_FOLD_* の組み合わせ)を書く(取り合いの印は要求の処理の中でしか使わず、その外では全部 MR_NO_CLAIM なので借りる)。
//   - 許容差つき(MrIsExactFold でない)なら、端数の枠を持つブロックの端数を全部帳簿の同じ段へ移す(枠を返すのは TreeFold)
//   - 一様(全部覆われている・許容差なし)ならビット単位の判定(T-0103)。ほぼ同じなら平均の値を一様の値に書き、切り捨ての余りを帳簿へ(T-0104)
// TreeFold(1 グループ)が枠の順に頁と端数の枠を空きのスタックに積み、印を MR_NO_CLAIM に戻す(番号を決定的にするため)。
// 帳簿は繰り上げつきの atomic の足し算(AddLedgerCarrying)。足した値の和も繰り上がりの回数も順によらないので、CPU の枠の順の足し算とビット一致する

static const uint32_t MR_CLAIM_FOLD_BASE = 0xFFFFFFF0u;  // 下位 3bit が印(MR_NO_CLAIM = 0xFFFFFFFF とは重ならない)
static const uint32_t MR_CLAIM_FOLD_COPY = 1;            // 畳む: 一様の値は頁の値のセル(FoldValue)
static const uint32_t MR_CLAIM_FOLD_WRITTEN = 2;         // 畳む: 一様の値は TreeFoldCheck が書いた(平均)
static const uint32_t MR_CLAIM_FOLD_RETURN = 4;          // 端数の枠を返す(端数は TreeFoldCheck が帳簿へ移した)

groupshared uint32_t gs_foldMismatch;

// 畳む時の一様の値(覆われていない最初のセル。全部覆われていれば空)
RxCell FoldValue(MrBlock block) {
    const uint32_t valueCell = MrFoldValueCell(block);
    if (valueCell == MR_BLOCK_CELLS)
        return RxMakeEmptyCell(0);

    return g_cells[PageCellAddress(block.page, valueCell)];
}

// 帳簿の段 ledgerLevel に bits(その段の単位 × 2^-64)を足す。桁あふれ(= 1 段粗い段の 2^61)は粗い段へ繰り上げる
// (multires_activity.cpp の AddLedgerBits と同じ結果。atomic の前の値で自分の足し算の桁あふれが分かる)
void AddLedgerCarrying(int32_t ledgerLevel, uint32_t column, uint64_t bits) {
    for (; bits != 0; --ledgerLevel) {
        const uint32_t address = MrLedgerAddress(ledgerLevel, column, g_ledgerColumns);
        if (address == MR_NO_BLOCK) {
            InterlockedAdd(g_counters[MR_COUNTER_LEDGER_OUTSIDE], 1u);
            return;
        }

        uint64_t before = 0;
        InterlockedAdd(g_ledger[address], bits, before);
        bits = before + bits < bits ? FX_U64(1u << 29, 0u) : 0;
    }
}

// 端数の枠のセル index の端数を帳簿の同じ段へ(ReturnFractionsToLedger の 1 セル分)
void ReturnFractionCell(MrBlock block, uint32_t index) {
    const MrFraction fraction = g_fractions[FractionAddress(block.fraction, index)];
    AddLedgerCarrying(block.level, 0, fraction.energy);
    for (uint32_t i = 0; i < fraction.speciesCount; ++i)
        AddLedgerCarrying(block.level, 1 + fraction.species[i], fraction.amounts[i]);
}

// ビット単位で一様か(全部のスレッドが呼ぶ。グループで一様な結果)
bool FoldsExactly(MrBlock block, uint32_t index) {
    if (index == 0)
        gs_foldMismatch = 0;

    GroupMemoryBarrierWithGroupSync();
    if (!MrFoldsCell(block, FoldValue(block), index, g_cells[PageCellAddress(block.page, index)]))
        InterlockedOr(gs_foldMismatch, 1u);

    GroupMemoryBarrierWithGroupSync();

    return gs_foldMismatch == 0;
}

// --- ほぼ同じかの集計(T-0112): CollectFoldStats と同じ MrFoldStats を、セル = スレッドの並列で作る ---
// 1 スレッドで 512 セルを MrAddFoldCell で足すと、大きい MrFoldStats を何度も写して数 ms かかった。集計は最小・最大・和だけで
// セルの順によらないので、ウェーブで縮約し、ウェーブの部分(受け取った順の番号に置く。順によらない)をスレッド 0 が合わせる。
// 和は 64bit を上下 32bit に分けて足す(ウェーブ 1 つ・512 セルでも桁あふれしない)ので、128bit の和が CPU とビット一致する。
// 成分は「自分のセルの、前に選んだ ID より大きい最小の ID」の最小を繰り返して ID の昇順に集める(9 個目があれば合わせた成分が多すぎる)

static const uint32_t FOLD_MAX_WAVES = MR_BLOCK_CELLS / 4;  // ウェーブは 4 レーン以上(D3D12)
static const uint32_t FOLD_NO_SPECIES = 0xFFFFFFFFu;

groupshared uint32_t gs_foldWaves;  // 書いたウェーブの部分の数
groupshared uint32_t gs_foldNextSpecies;
groupshared uint32_t gs_foldCount[FOLD_MAX_WAVES];
groupshared int32_t gs_foldLowTemperature[FOLD_MAX_WAVES];
groupshared int32_t gs_foldHighTemperature[FOLD_MAX_WAVES];
groupshared uint64_t gs_foldLow[FOLD_MAX_WAVES];
groupshared uint64_t gs_foldHigh[FOLD_MAX_WAVES];
groupshared uint64_t gs_foldSumLow[FOLD_MAX_WAVES];   // 下位 32bit の和
groupshared uint64_t gs_foldSumHigh[FOLD_MAX_WAVES];  // 上位 32bit の和(エネルギーは符号つき)

// ウェーブの部分を書く番号(ウェーブの最初のレーンだけが呼ぶ)
uint32_t TakeFoldWave() {
    uint32_t wave = 0;
    InterlockedAdd(gs_foldWaves, 1u, wave);

    return wave;
}

// 上位 32bit の和(符号つき)× 2^32 + 下位 32bit の和 を 128bit の (上, 下) にする
MrWide CombineFoldSum(int64_t high, uint64_t low) {
    MrWide sum = MrMakeWide();
    const uint64_t shifted = (uint64_t)high << 32;
    sum.hi = shifted + low;
    sum.top = (uint64_t)(high >> 32) + (sum.hi < low ? 1u : 0u);

    return sum;
}

// セルの成分のうち、物質 after より大きい最小の ID(after = FOLD_NO_SPECIES なら最小。無ければ FOLD_NO_SPECIES)
uint32_t NextCellSpecies(RxCell cell, uint32_t after) {
    uint32_t next = FOLD_NO_SPECIES;
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        const uint32_t id = cell.species[i];
        if ((after == FOLD_NO_SPECIES || id > after) && id < next)
            next = id;
    }

    return next;
}

// セルの物質 id の量(無ければ 0 で present = false)
uint64_t CellSpeciesAmount(RxCell cell, uint32_t id, out bool present) {
    present = false;
    uint64_t amount = 0;
    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
        if (cell.species[i] == id) {
            present = true;
            amount = cell.amounts[i];
        }
    }

    return amount;
}

// セルの数・温度・成分の合計の最大・エネルギーの和(スレッド 0 の stats に)
MrFoldStats CollectFoldCells(uint32_t index, RxCell cell, bool uncovered) {
    const int32_t temperature = uncovered ? RxComputeThermal(MakeTable(), cell).temperature : 0;
    uint64_t total = 0;
    for (uint32_t i = 0; uncovered && i < cell.speciesCount; ++i)
        total = total + cell.amounts[i] < total ? FX_U64(0xFFFFFFFFu, 0xFFFFFFFFu) : total + cell.amounts[i];

    const int64_t energy = uncovered ? cell.energy : 0;
    if (index == 0)
        gs_foldWaves = 0;

    GroupMemoryBarrierWithGroupSync();
    const uint32_t count = WaveActiveCountBits(uncovered);
    const int32_t lowTemperature = WaveActiveMin(uncovered ? temperature : 2147483647);
    const int32_t highTemperature = WaveActiveMax(uncovered ? temperature : -2147483647 - 1);
    const uint64_t highTotal = WaveActiveMax(total);
    const uint64_t energyLow = WaveActiveSum((uint64_t)energy & FX_LOW32_MASK);
    const int64_t energyHigh = WaveActiveSum(energy >> 32);
    if (WaveIsFirstLane()) {
        const uint32_t wave = TakeFoldWave();
        gs_foldCount[wave] = count;
        gs_foldLowTemperature[wave] = lowTemperature;
        gs_foldHighTemperature[wave] = highTemperature;
        gs_foldHigh[wave] = highTotal;
        gs_foldSumLow[wave] = energyLow;
        gs_foldSumHigh[wave] = (uint64_t)energyHigh;
    }

    GroupMemoryBarrierWithGroupSync();
    MrFoldStats stats = MrMakeFoldStats();
    if (index != 0)
        return stats;

    uint64_t sumLow = 0;
    int64_t sumHigh = 0;
    for (uint32_t wave = 0; wave < gs_foldWaves; ++wave) {
        stats.cellCount += gs_foldCount[wave];
        stats.lowTemperature = min(stats.lowTemperature, gs_foldLowTemperature[wave]);
        stats.highTemperature = max(stats.highTemperature, gs_foldHighTemperature[wave]);
        stats.highTotal = max(stats.highTotal, gs_foldHigh[wave]);
        sumLow += gs_foldSumLow[wave];
        sumHigh += (int64_t)gs_foldSumHigh[wave];
    }

    stats.energy = CombineFoldSum(sumHigh, sumLow);

    return stats;
}

// 成分 position(物質 id)のセルの数・最小・最大・128bit の和を stats に(スレッド 0 だけ)
MrFoldStats CollectFoldSpecies(MrFoldStats stats, uint32_t index, RxCell cell, bool uncovered, uint32_t position,
                               uint32_t id) {
    bool present = false;
    const uint64_t amount = uncovered ? CellSpeciesAmount(cell, id, present) : 0;
    if (index == 0)
        gs_foldWaves = 0;

    GroupMemoryBarrierWithGroupSync();
    const uint32_t count = WaveActiveCountBits(present);
    const uint64_t low = WaveActiveMin(present ? amount : FX_U64(0xFFFFFFFFu, 0xFFFFFFFFu));
    const uint64_t high = WaveActiveMax(amount);
    const uint64_t sumLow = WaveActiveSum(amount & FX_LOW32_MASK);
    const uint64_t sumHigh = WaveActiveSum(amount >> 32);
    if (WaveIsFirstLane()) {
        const uint32_t wave = TakeFoldWave();
        gs_foldCount[wave] = count;
        gs_foldLow[wave] = low;
        gs_foldHigh[wave] = high;
        gs_foldSumLow[wave] = sumLow;
        gs_foldSumHigh[wave] = sumHigh;
    }

    GroupMemoryBarrierWithGroupSync();
    if (index != 0)
        return stats;

    uint64_t totalLow = 0;
    uint64_t totalHigh = 0;
    stats.species[position] = id;
    stats.lowAmount[position] = FX_U64(0xFFFFFFFFu, 0xFFFFFFFFu);
    for (uint32_t wave = 0; wave < gs_foldWaves; ++wave) {
        stats.present[position] += gs_foldCount[wave];
        stats.lowAmount[position] = min(stats.lowAmount[position], gs_foldLow[wave]);
        stats.highAmount[position] = max(stats.highAmount[position], gs_foldHigh[wave]);
        totalLow += gs_foldSumLow[wave];
        totalHigh += gs_foldSumHigh[wave];
    }

    const MrWide sum = CombineFoldSum((int64_t)totalHigh, totalLow);
    stats.sumLow[position] = sum.hi;
    stats.sumHigh[position] = sum.top;

    return stats;
}

// ほぼ同じなら(スレッド 0 だけが返す値を使う)平均の値を一様の値に書き、切り捨ての余りを帳簿へ(CollectFoldStats → MrFoldStatsValue)
bool FoldsNearly(uint32_t slot, MrBlock block, uint32_t index, MrFoldTolerance tolerance) {
    const bool uncovered = !MrIsCoveredCell(block, index);
    const RxCell cell = g_cells[PageCellAddress(block.page, index)];
    MrFoldStats stats = CollectFoldCells(index, cell, uncovered);

    // --- 成分を ID の昇順に 1 つずつ(グループで一様なループ)---
    uint32_t last = FOLD_NO_SPECIES;
    for (uint32_t position = 0; position <= RX_MAX_CELL_SPECIES; ++position) {
        if (index == 0)
            gs_foldNextSpecies = FOLD_NO_SPECIES;

        GroupMemoryBarrierWithGroupSync();
        const uint32_t mine = uncovered ? NextCellSpecies(cell, last) : FOLD_NO_SPECIES;
        if (mine != FOLD_NO_SPECIES)
            InterlockedMin(gs_foldNextSpecies, mine);

        GroupMemoryBarrierWithGroupSync();
        const uint32_t next = gs_foldNextSpecies;
        GroupMemoryBarrierWithGroupSync();  // 次の回が書き直す前に、全部が読み終える
        if (next == FOLD_NO_SPECIES)
            break;

        if (position == RX_MAX_CELL_SPECIES) {
            stats.tooManySpecies = 1;
            break;
        }

        stats = CollectFoldSpecies(stats, index, cell, uncovered, position, next);
        stats.speciesCount = position + 1;
        last = next;
    }

    if (index != 0 || !MrFoldStatsWithin(stats, tolerance))
        return false;

    const MrFoldValue value = MrFoldStatsValue(stats);
    AddLedgerCarrying(block.level - 3, 0, value.energyRemainder << 55u);
    for (uint32_t i = 0; i < stats.speciesCount; ++i)
        AddLedgerCarrying(block.level - 3, 1 + stats.species[i], value.amountRemainders[i] << 55u);

    g_cells[slot] = value.cell;

    return true;
}

[numthreads(MR_BLOCK_CELLS, 1, 1)] void TreeFoldCheck(uint32_t index : SV_GroupIndex, uint3 group : SV_GroupID) {
    const uint32_t slot = group.x;
    if (slot >= g_worldBlocks)
        return;

    // --- ちょうど静かになったら、端数の枠の端数を帳簿へ(許容差つきの時だけ。グループで一様な分岐)---
    const uint32_t mark = MrActivityMark(FX_U64(g_tickHigh, g_tickLow));
    const MrFoldTolerance tolerance = MrUnpackFoldTolerance(g_foldTolerance);
    MrBlock block = g_blocks[slot];
    uint32_t flags = 0;
    if (!MrIsExactFold(tolerance) && MrWantsFractionReturn(block, mark)) {
        ReturnFractionCell(block, index);
        block.fraction = MR_NO_FRACTION;
        flags |= MR_CLAIM_FOLD_RETURN;
    }

    // --- 畳めるか: まずビット単位(値が変わらない)。違えば、許容差つきで覆われていないセルがあれば、ほぼ同じか(T-0125。
    //     FoldsExactly はグループで一様な結果)---
    if (MrWantsFoldCheck(block, mark)) {
        if (FoldsExactly(block, index))
            flags |= MR_CLAIM_FOLD_COPY;
        else if (!MrIsExactFold(tolerance) && MrFoldValueCell(block) != MR_BLOCK_CELLS)
            flags |= FoldsNearly(slot, block, index, tolerance) ? MR_CLAIM_FOLD_WRITTEN : 0u;
    }

    if (index == 0 && flags != 0)
        g_treeWords[TreeClaimAddress(slot)] = MR_CLAIM_FOLD_BASE | flags;
}

// 取り合いの印 [枠] の畳む・返すの印(無ければ 0)
uint32_t FoldClaim(uint32_t slot) {
    if (slot >= g_worldBlocks)
        return 0;

    const uint32_t claim = g_treeWords[TreeClaimAddress(slot)];

    return claim != MR_NO_CLAIM && (claim & ~7u) == MR_CLAIM_FOLD_BASE ? claim & 7u : 0u;
}

// 枠 slot を畳む: 一様の値を書き(COPY の時)、頁を空きのスタックの position に積む。ほぼ同じで畳んだ(WRITTEN)なら
// セルの値が変わった(反応の速さ f も変わる)ので、つついて tc を書き直す(CPU の FoldQuietPages。ADR-0018 追記 T-0125)
void FoldBlock(uint32_t slot, uint32_t claim, uint32_t position) {
    const MrBlock block = g_blocks[slot];
    if ((claim & MR_CLAIM_FOLD_COPY) != 0)
        g_cells[slot] = FoldValue(block);

    g_treeWords[FreePageAddress(position)] = block.page;
    g_blocks[slot].page = MR_NO_PAGE;
    if ((claim & MR_CLAIM_FOLD_WRITTEN) != 0)
        PokeBlock(slot);
}

// 調べた頁を畳み、端数の枠を返す(1 グループ。どちらも枠の順に空きのスタックへ。CPU は枠ごとに「返す → 畳む」)
[numthreads(MR_MAX_REQUESTS, 1, 1)] void TreeFold(uint32_t i : SV_GroupIndex) {
    const uint32_t freePages = g_counters[MR_COUNTER_FREE_PAGES];
    const uint32_t freeFractions = g_counters[MR_COUNTER_FREE_FRACTIONS];
    uint32_t folded = 0;    // ここまでの区切りで畳んだ数(グループで一様)
    uint32_t returned = 0;  // ここまでの区切りで返した端数の枠の数
    for (uint32_t first = 0; first < g_worldBlocks; first += MR_MAX_REQUESTS) {
        const uint32_t slot = first + i;
        const uint32_t claim = FoldClaim(slot);
        const bool folds = (claim & (MR_CLAIM_FOLD_COPY | MR_CLAIM_FOLD_WRITTEN)) != 0;
        const bool returns = (claim & MR_CLAIM_FOLD_RETURN) != 0;
        InclusiveScan(i, folds ? 1u : 0u, returns ? 1u : 0u, 0u);

        if (returns) {
            g_freeFractions[freeFractions + returned + gs_fractionScan[i] - 1] = g_blocks[slot].fraction;
            g_blocks[slot].fraction = MR_NO_FRACTION;
        }

        if (folds)
            FoldBlock(slot, claim, freePages + folded + gs_blockScan[i] - 1);

        if (claim != 0)
            g_treeWords[TreeClaimAddress(slot)] = MR_NO_CLAIM;

        folded += gs_blockScan[MR_MAX_REQUESTS - 1];
        returned += gs_fractionScan[MR_MAX_REQUESTS - 1];
        GroupMemoryBarrierWithGroupSync();  // 次の区切りが累積和を書き直す前に、全部が読み終える
    }

    if (i != 0)
        return;

    g_counters[MR_COUNTER_FREE_PAGES] = freePages + folded;
    g_counters[MR_COUNTER_FREE_FRACTIONS] = freeFractions + returned;
    g_counters[MR_COUNTER_FOLDED] += folded;
}
