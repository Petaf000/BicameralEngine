// multires_tree.hlsl — 多重解像度の世界の木の管理の Compute の段(17 §5「木の管理」。T-0018。ADR-0016)。
// 1 刻みの要求の処理: (静かな葉を粗くするなら TreeQuiet。T-0101)→ TreeResolve → TreeSettle → TreeAllocate → (Work Graph: RefineNode の鎖・CoarsenRequestNode)→ TreeRelease
// → TreeClearIndex → TreeFillIndex(後ろの 2 つは索引を作り直す印がある時だけ働く)。
// 溢れを使う世界(MR_WIDE_CELLS の変種。T-0237)は TreeAllocate の後に TreeWideReserve(細かくする子の頁に溢れの塊を付ける)、
// 要求の処理と畳むの後に TreeWideTrim(端数の枠が今の面の溢れに要らない塊を返す。T-0347)。
// 要求の処理の前: TreeFoldCheck → TreeFold(静かで一様・ほぼ同じになった頁を枠の順に畳む・端数の枠を返す。T-0103・T-0112)。
// 刻んだ後: TreeExpand(一様で反応が進むブロックに枠の順で頁を配る。T-0102)。呼ぶ順は engine/src/sim/gpu_multires.cpp の
// RecordProcessRequests。CPU リファレンスは engine/src/sim/multires_tree.cpp(同じ関数・同じ順)。
//
// 順番に依存しない理由: 解決と確定は要求ごとに自分の値だけを書き、取り合いは atomic の最小(小さい番号が勝つ)。
// 割り当てと解放は 1 グループの累積和で、要求の順に枠を配る・積む(ADR-0016)。数える欄は足し算。
#include "sim/multires_bindings.hlsli"
#include "sim/multires_field_access.hlsli"  // 欄の区画(u4 の層の置き場の後ろ。配り手の欄の頁を取る・返す。T-1252)
#include "sim/multires_layer_access.hlsli"  // 層の置き場(u4。頁を返す時に層の頁も返す。T-1240)
#ifdef MR_WIDE_CELLS
#include "sim/multires_wide_access.hlsli"

// 溢れの領域 area(頁か端数の枠)の今の面の溢れの合計
uint32_t WideTotal(uint32_t area) {
    return WideRange(area, WideCurrentSide(area), MR_BLOCK_CELLS - 1).y;
}
#endif

static const uint32_t TREE_THREADS = 64;

groupshared uint32_t gs_blockScan[MR_MAX_REQUESTS];
groupshared uint32_t gs_fractionScan[MR_MAX_REQUESTS];
groupshared uint32_t gs_pageScan[MR_MAX_REQUESTS];
groupshared uint32_t gs_grantedBlocks;
groupshared uint32_t gs_grantedFractions;
groupshared uint32_t gs_grantedPages;
groupshared uint32_t gs_refineCount;
groupshared uint32_t gs_coarsenCount;
groupshared uint32_t gs_grantedLayers;

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
        [unroll] for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
            const uint32_t index = MrChildCell(local, j);
            children.cells[j] = LoadBlockCell(child, childSlot, index);
            children.fractions[j] = LoadFraction(child.fraction, index);
        }

        fits = MrCoarsenFits(children);
    }

    return fits;
}

#ifdef MR_WIDE_CELLS
// 子のブロック childSlot を粗くした親の八分の一の 64 セルのどれも、0 でない層を持つ物質が 8 種までか(1 スレッドで 64 セル。
// 層は刻みの後に合わせてある = 量で結んだ値なので、置き場の 0 でない組を物質で数える。一様な子は子の値の層だけなので入る。T-1240 の 7 の 5)
bool CoarsenLayersFit(uint32_t childSlot) {
    const MrBlock child = g_blocks[childSlot];
    const uint32_t layerPage = MrIsUniform(child) ? MR_LAYER_NO_PAGE : LayerPageOf(child.page);
    if (layerPage == MR_LAYER_NO_PAGE)
        return true;

    bool fits = true;
    for (uint32_t local = 0; local < MR_OCTANT_CELLS && fits; ++local) {
        uint32_t seen[RX_MAX_CELL_SPECIES];
        FX_UNROLL
        for (uint32_t m = 0; m < RX_MAX_CELL_SPECIES; ++m)
            seen[m] = 0;

        uint32_t count = 0;
        for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
            const uint32_t base = MrLayerEntryAddress(LayerPageCount(), LayerCapacity(), layerPage,
                                                      MrChildCell(local, j), 0);
            for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) {
                const uint32_t address = base + (k * MR_LAYER_ENTRY_WORDS);
                if ((g_layers[address + 1] | g_layers[address + 2]) == 0)
                    continue;

                // 前に数えたか(決まった数の回しで選ぶ。動的な添字で書かない。T-0397)
                const uint32_t species = g_layers[address];
                bool known = false;
                FX_UNROLL
                for (uint32_t m = 0; m < RX_MAX_CELL_SPECIES; ++m)
                    known = known || (m < count && seen[m] == species);

                if (known)
                    continue;

                FX_UNROLL
                for (uint32_t m = 0; m < RX_MAX_CELL_SPECIES; ++m) {
                    if (m == count)
                        seen[m] = species;
                }

                count += 1;
            }
        }

        fits = count <= RX_MAX_CELL_SPECIES;
    }

    return fits;
}
#endif

MrRequestState ResolveCoarsen(MrRequest request, MrRequestState state, uint32_t i) {
    const uint32_t slot = LookupBlock(request.level, MrBlockOriginOf(request.x), MrBlockOriginOf(request.y),
                                      MrBlockOriginOf(request.z));
    if (slot == MR_NO_BLOCK || g_blocks[slot].parent == MR_NO_BLOCK || MrHasRealChild(g_blocks[slot])) {
        state.status = MR_STATUS_INVALID;
        return state;
    }

    // --- 成分が入りきらなければ粗くしない。静かな葉なら、今の忙しさの印では粗くできないことにする(毎刻み要求し直さない)。
    //     溢れを使う世界(multires_tree_wide_resolve)はいつも入りきる(CPU の ResolveCoarsen。T-0329)---
#ifndef MR_WIDE_CELLS
    if (!CoarsenFits(slot)) {
        g_blocks[slot].quietCheck = MrQuietCheckStamp(g_blocks[slot].busyTick, false);
        state.status = MR_STATUS_SPECIES_FULL;
        return state;
    }
#else
    // 層を持つ物質が粗いセル 1 つで 9 種以上になるなら断って数える(GPU の層の組は 1 セル 8 つ。D-689・T-1240 の 7 の 5。
    // 溢れを使わない世界は粗くした親が 8 種に入らなければ上で断るので起きない)
    if (!CoarsenLayersFit(slot)) {
        CountLayerShortage(1);
        state.status = MR_STATUS_NO_SPACE;
        return state;
    }
#endif

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

#ifdef MR_WIDE_CELLS
// 一様の値の溢れの成分(今の面のセル 0 の範囲)の層が子と親で同じか。溢れの数が違えば違う(並びの位置ごとに物質で結んで比べる。
// CPU の CoarsenKeepsUniformLayers は上限の無い形の並びを比べる。T-1240 の 7 の 5)
bool SameTailLayers(uint32_t childSlot, RxCell childValue, LayerTail childTail, uint32_t parentSlot, RxCell parentValue,
                    LayerTail parentTail) {
    const bool childWide = childValue.speciesCount == RX_MAX_CELL_SPECIES && childTail.area != MR_NO_PAGE;
    const bool parentWide = parentValue.speciesCount == RX_MAX_CELL_SPECIES && parentTail.area != MR_NO_PAGE;
    const uint2 childRange = childWide ? WideRange(childTail.area, childTail.side, 0) : uint2(0, 0);
    const uint2 parentRange = parentWide ? WideRange(parentTail.area, parentTail.side, 0) : uint2(0, 0);
    if (childRange.y - childRange.x != parentRange.y - parentRange.x)
        return false;

    for (uint32_t n = 0; n < childRange.y - childRange.x; ++n) {
        const uint32_t childWord = MrWideEntryWord(childRange.x + n);
        const uint32_t parentWord = MrWideEntryWord(parentRange.x + n);
        const uint32_t childSpecies = g_treeWords[WideWord(childTail.area, childTail.side, childWord)];
        const uint32_t parentSpecies = g_treeWords[WideWord(parentTail.area, parentTail.side, parentWord)];
        const uint64_t a = BoundLayerOfAt(UniformLayerBase(childSlot), childSpecies, childValue, childTail);
        const uint64_t b = BoundLayerOfAt(UniformLayerBase(parentSlot), parentSpecies, parentValue, parentTail);
        if ((uint32_t)a != (uint32_t)b || (uint32_t)(a >> 32) != (uint32_t)(b >> 32))
            return false;
    }

    return true;
}
#endif

// 一様な子 childSlot を一様な親へ粗くしても、親の一様の値の層が同じか(層の無い世界はいつも true。CPU の
// CoarsenKeepsUniformLayers: 今の値と結んだ層の並びを比べる。64bit の比べは 32bit の 2 つで。T-1240)
bool CoarsenKeepsUniformLayers(uint32_t childSlot, uint32_t parentSlot) {
    if (!UniformHasLayers(childSlot) && !UniformHasLayers(parentSlot))
        return true;

    const RxCell childValue = g_cells[childSlot];
    const RxCell parentValue = g_cells[parentSlot];
#ifdef MR_WIDE_CELLS
    // 溢れを使う世界(multires_tree_wide_settle): 溢れの成分の層も比べる(値は同じ = 溢れも同じ並び。T-1240 の 7 の 5)
    const LayerTail childTail = UniformLayerTail(childSlot);
    const LayerTail parentTail = UniformLayerTail(parentSlot);
    if (!SameTailLayers(childSlot, childValue, childTail, parentSlot, parentValue, parentTail))
        return false;
#else
    const LayerTail childTail = NoLayerTail();
    const LayerTail parentTail = NoLayerTail();
#endif
    bool same = childValue.speciesCount == parentValue.speciesCount;
    FX_UNROLL
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        if (i < childValue.speciesCount && i < parentValue.speciesCount) {
            const uint64_t a = BoundLayerOfAt(UniformLayerBase(childSlot), childValue.species[i], childValue,
                                              childTail);
            const uint64_t b = BoundLayerOfAt(UniformLayerBase(parentSlot), parentValue.species[i], parentValue,
                                              parentTail);
            same = same && (uint32_t)a == (uint32_t)b && (uint32_t)(a >> 32) == (uint32_t)(b >> 32);
        }
    }

    return same;
}

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

        // 一様の値の層が違っても頁に広げる(同じ値の子 8 つの層の和 ÷ 8 = 子の層。T-1240)
        if (state.pageNeed == 0 && MrIsUniform(g_blocks[parent]) && !CoarsenKeepsUniformLayers(state.target, parent))
            state.pageNeed = 1;

        // 載せた欄の値が違っても頁に広げる(CPU の CoarsenKeepsUniform。T-0711・T-1252)
        if (state.pageNeed == 0 && MrIsUniform(g_blocks[parent]) && !CoarsenKeepsUniformCarried(state.target, parent))
            state.pageNeed = 1;
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

// 要求 i(待ちのもの)が要る層の頁の数(T-1240。層を持たない世界は 0)。
// 細かくする: 親(鎖の最初の段)の頁が層の頁を持てば鎖の段の全部(どの段も親から写す)。一様な親の子は一様(一様の枠の層を写す)。
// 粗くする: 層の頁を持たない親の頁(広げるなら新しい頁)に層が要る時 1(子に層がある・広げる一様な親に一様の枠の層がある)
uint32_t RequestLayerNeed(uint32_t i, MrRequestState state) {
    if (LayerCapacity() == 0)
        return 0;

    const MrBlock target = g_blocks[state.target];
    if (g_requests[i].op == MR_REQUEST_REFINE)
        return !MrIsUniform(target) && LayerPageOf(target.page) != MR_LAYER_NO_PAGE ? state.levels : 0u;

    const MrBlock parent = g_blocks[target.parent];
    const bool childLayered = MrIsUniform(target) ? UniformHasLayers(state.target)
                                                  : LayerPageOf(target.page) != MR_LAYER_NO_PAGE;
    if (!MrIsUniform(parent))
        return childLayered && LayerPageOf(parent.page) == MR_LAYER_NO_PAGE ? 1u : 0u;

    if (state.pageNeed == 0)
        return 0;

    return childLayered || UniformHasLayers(target.parent) ? 1u : 0u;
}

// --- 3b. 配り手の欄の頁(T-1252 段 2。CPU の AllocateOwnerPages: 欄ごとに許可した要求の順に取る。ADR-0532 の 2・3)---

// 要求 i(待ちのもの)の欄の頁が、載せた欄の全部で取れるか(要求の順の累積和 ≤ 取れる数。取れる数は容量だけで決める =
// CPU の木の段は予算を見ない)。全部のスレッドが呼ぶ(中で 1 グループの累積和)
bool CarriedPagesFit(uint32_t i, MrRequestState state, bool pending) {
    bool fit = true;
    const uint32_t count = CarriedFieldCount();
    for (uint32_t f = 0; f < count; ++f) {
        uint32_t need = 0;  // ?: は両側を読むので if で(待ちでない要求の枠を読まない)
        if (pending && CarriedOwned(f))
            need = CarriedOwnerNeed(f, i, state);

        LayerInclusiveScan(i, need);
        const uint32_t available = MfPagesAvailable(CarriedHeader(f, MF_HEADER_CAPACITY), 0u,
                                                    CarriedHeader(f, MF_HEADER_FREE_COUNT),
                                                    CarriedHeader(f, MF_HEADER_TOP));
        fit = fit && gs_layerScan[i] <= available;
        GroupMemoryBarrierWithGroupSync();  // 次の欄の累積和で gs_layerScan を書き直す前に全部が読み終える
    }

    return fit;
}

// 段の中で k 番目に取る欄の頁(空きのスタックの上から k 番目・その後は高さの上。?: は両側を読むので if で)
uint32_t TakenCarriedPage(uint32_t field, uint32_t k, uint32_t freeCount, uint32_t top) {
    if (MfTakesFreePage(k, freeCount))
        return CarriedFreePage(field, MfFreePosition(k, freeCount));

    return MfFreshPage(k, freeCount, top);
}

// 許可した要求 i の欄の頁を、欄ごとに要求の順の k 番目 = 空きのスタックの上から k 番目・その後は高さの上で取り、
// セルの頁へ結んで 0 で埋める(CPU の TakeOwnerPage = EnsurePage)。全部のスレッドが呼ぶ
void TakeCarriedPages(uint32_t i, MrRequestState state, bool granted) {
    const uint32_t count = CarriedFieldCount();
    for (uint32_t f = 0; f < count; ++f) {
        uint32_t need = 0;
        if (granted && CarriedOwned(f))
            need = CarriedOwnerNeed(f, i, state);

        const uint32_t freeCount = CarriedHeader(f, MF_HEADER_FREE_COUNT);
        const uint32_t top = CarriedHeader(f, MF_HEADER_TOP);
        LayerInclusiveScan(i, need);  // 中のバリアで、全部のスレッドが need を数え終えてから結ぶ
        const uint32_t first = gs_layerScan[i] - need;
        const uint32_t total = gs_layerScan[MR_MAX_REQUESTS - 1];
        for (uint32_t d = 0; d < need; ++d) {
            const uint32_t k = first + d;
            SetCarriedPageOf(f, CarriedOwnerCellPage(i, state, d), TakenCarriedPage(f, k, freeCount, top));
        }

        // --- 取った頁を 0 に(グループで分けて書く。total はグループで一様)---
        const uint32_t words = MR_BLOCK_CELLS * CarriedComponents(f) * 2u;
        for (uint32_t n = 0; n < total; ++n) {
            const uint32_t base = CarriedValueAddress(f, TakenCarriedPage(f, n, freeCount, top), 0, 0);
            for (uint32_t word = i; word < words; word += MR_MAX_REQUESTS)
                g_layers[base + word] = 0;
        }

        DeviceMemoryBarrierWithGroupSync();  // 全部が空きの数・高さ・空きのスタックを読み終えてから書き直す
        if (i == 0 && total > 0) {
            const uint32_t fromFree = total < freeCount ? total : freeCount;
            g_layers[CarriedStoreBase(f) + MF_HEADER_FREE_COUNT] = freeCount - fromFree;
            g_layers[CarriedStoreBase(f) + MF_HEADER_TOP] = top + (total - fromFree);
        }

        DeviceMemoryBarrierWithGroupSync();
    }
}

// 欄の頁が尽きて断った要求を数える(計器。GPU だけ。CPU は上限が無い)
void CountCarriedShortage() {
    InterlockedAdd(g_layers[CarriedStoreBase(0) + MF_HEADER_TREE_SHORTAGE], 1u);
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
    const uint32_t layerCapacity = LayerCapacity();
    const uint32_t layerPageCount = LayerPageCount();
    uint32_t freeLayers = 0;  // 層を持たない世界は u4 を読まない(?: は両側を読む)
    if (layerCapacity > 0)
        freeLayers = g_layers[MrLayerFreeCountAddress(layerPageCount, layerCapacity)];

    uint32_t layerNeed = 0;
    if (pending)
        layerNeed = RequestLayerNeed(i, state);

    if (i == 0) {
        gs_grantedBlocks = 0;
        gs_grantedFractions = 0;
        gs_grantedPages = 0;
        gs_grantedLayers = 0;
        gs_refineCount = 0;
        gs_coarsenCount = 0;
    }

    InclusiveScan(i, pending ? state.blockNeed : 0u, pending ? state.fractionNeed : 0u, pending ? state.pageNeed : 0u);
    LayerInclusiveScan(i, layerNeed);  // 層の頁(4 つ目の値。要求の順のプレフィックス和。T-1240)

    // --- 許可: 累積和が空きの数以下(許可は一覧の前から続く)。層の頁が尽きたら細かくしない・粗くしない(GPU だけ。仮【エンジン】T-1240)---
    const uint32_t blockSum = gs_blockScan[i];
    const uint32_t fractionSum = gs_fractionScan[i];
    const uint32_t pageSum = gs_pageScan[i];
    const uint32_t layerSum = gs_layerScan[i];
    const bool fits = pending && blockSum <= freeBlocks && fractionSum <= freeFractions && pageSum <= freePages;
    const bool layersFit = fits && layerSum <= freeLayers;
    if (fits && !layersFit)
        CountLayerShortage(1);

    // 配り手の欄の頁が尽きたら細かくしない・粗くしない(GPU だけ。層の頁と同じ扱い = 仮。T-1252)
    const bool carriedFit = CarriedPagesFit(i, state, pending);
    const bool granted = layersFit && carriedFit;
    if (layersFit && !carriedFit)
        CountCarriedShortage();

    if (i < count && layerCapacity > 0) {
        const bool layered = granted && layerNeed > 0;
        g_layers[MrLayerRequestAddress(layerPageCount, layerCapacity, i)] = layered
                                                                                ? freeLayers - (layerSum - layerNeed)
                                                                                : MR_LAYER_NO_PAGE;
    }

    if (granted) {
        state.status = MR_STATUS_GRANTED;
        state.blockBase = freeBlocks - (blockSum - state.blockNeed);
        state.fractionBase = freeFractions - (fractionSum - state.fractionNeed);
        state.pageBase = freePages - (pageSum - state.pageNeed);
        InterlockedMax(gs_grantedBlocks, blockSum);
        InterlockedMax(gs_grantedFractions, fractionSum);
        InterlockedMax(gs_grantedPages, pageSum);
        InterlockedMax(gs_grantedLayers, layerSum);
    } else if (pending) {
        state.status = MR_STATUS_NO_SPACE;
    }

    if (i < count)
        g_states[i] = state;

    TakeCarriedPages(i, state, granted);  // 許可の後に要求の順に(state.pageBase で取ったセルの頁が分かる)

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
    if (layerCapacity > 0)
        g_layers[MrLayerFreeCountAddress(layerPageCount, layerCapacity)] = freeLayers - gs_grantedLayers;

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

// 1 グループで、スレッド i の返すセルの頁 page(返さないなら MR_NO_PAGE)の載せた欄の頁を、欄ごとに要求の順に空きのスタックへ積む
// (CPU の ReleasePage: 頁の中身は消さない = 取る時に 0 で埋める)。全部のスレッドが呼ぶ
void ReleaseCarriedPages(uint32_t i, uint32_t page) {
    const uint32_t count = CarriedFieldCount();
    for (uint32_t f = 0; f < count; ++f) {
        const uint32_t fieldPage = CarriedPageOf(f, page);
        const bool give = fieldPage != MF_NO_PAGE;
        const uint32_t freeCount = CarriedHeader(f, MF_HEADER_FREE_COUNT);
        LayerInclusiveScan(i, give ? 1u : 0u);
        const uint32_t total = gs_layerScan[MR_MAX_REQUESTS - 1];
        if (give) {
            SetCarriedFreePage(f, freeCount + gs_layerScan[i] - 1, fieldPage);
            SetCarriedPageOf(f, page, MF_NO_PAGE);
        }

        DeviceMemoryBarrierWithGroupSync();
        if (i == 0 && total > 0)
            g_layers[CarriedStoreBase(f) + MF_HEADER_FREE_COUNT] = freeCount + total;

        DeviceMemoryBarrierWithGroupSync();
    }
}

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

    // 返す頁の層の頁も返す(CPU の ReleaseAll の ClearPageLayers。層を持たない世界は何もしない。T-1240)
    if (LayerCapacity() > 0)
        ReleaseLayerPages(i, pageRelease != 0 ? state.releasePage : MR_NO_PAGE);

    // 返す頁の載せた欄の頁も要求の順に返す(CPU の ReleaseAll の ReleaseOwnerPages と、Coarsen が返すこの刻みだけの欄の頁。T-1252)
    ReleaseCarriedPages(i, pageRelease != 0 ? state.releasePage : MR_NO_PAGE);

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

#ifdef MR_WIDE_CELLS
// 溢れを使う世界(T-0384): 頁に広げたブロックの頁の面 0 に、一様の値の溢れを覆われていないセル全部へ写す
// (multires_wait_step.hlsli の CopyUniformTails)ための塊を、一覧の順(枠の順)の累積和で空きのスタックの上から配る
// (TreeWideReserve と同じ型)。足りなければそのブロックには配らない(写す時に数える。置き場が尽きた時だけ)。前から持つ塊はそのまま使う

// 頁に広げるブロック slot の頁の面 0 に足す塊の数(attach なら stack の上から taken 番目より下を付ける)
uint32_t ReserveExpandTail(uint32_t slot, bool attach, uint32_t stack, uint32_t freeCount, uint32_t taken) {
    const MrBlock block = g_blocks[slot];
    const uint32_t entries = UniformTailCount(slot) * UncoveredCellCount(block);
    if (entries == 0)
        return 0;

    const uint32_t need = MrWideChunksFor(entries);
    const uint32_t table = MrWideTableAt(WideAreaHeader(block.page), 0);
    const uint32_t held = g_treeWords[table];
    if (need <= held)
        return 0;

    if (attach) {
        for (uint32_t j = held; j < need; ++j)
            g_treeWords[WideTableEntryWord(table, j)] = g_treeWords[stack + freeCount - 1 - (taken + (j - held))];

        g_treeWords[table] = need;
    }

    return need - held;
}

void ReserveExpandChunks(uint32_t i, uint32_t granted) {
    if (i == 0)
        gs_grantedBlocks = 0;

    AllMemoryBarrierWithGroupSync();  // 頁を配った枠(g_blocks)と一覧を全部のスレッドが読めるように

    const uint32_t pool = WidePoolWord();
    const uint32_t stack = MrWideStackWord(pool, g_treeWords[pool + MR_WIDE_POOL_CAPACITY]);
    const uint32_t freeCount = g_treeWords[pool + MR_WIDE_POOL_FREE];
    uint32_t taken = 0;  // ここまでの区切りで欲しい塊の数(グループで一様)
    for (uint32_t first = 0; first < granted; first += MR_MAX_REQUESTS) {
        const uint32_t position = first + i;
        const uint32_t slot = position < granted ? ExpandRecord(position + 1) : MR_NO_BLOCK;
        const uint32_t want = slot != MR_NO_BLOCK ? ReserveExpandTail(slot, false, stack, freeCount, 0) : 0u;
        InclusiveScan(i, want, 0, 0);

        // --- 累積和が空きに入るブロックだけ配る(累積和は増えるだけなので、配るのは一覧の前から続く)---
        const uint32_t sum = taken + gs_blockScan[i];
        if (want != 0 && sum <= freeCount) {
            ReserveExpandTail(slot, true, stack, freeCount, sum - want);
            InterlockedMax(gs_grantedBlocks, sum);
        }

        taken += gs_blockScan[MR_MAX_REQUESTS - 1];
        GroupMemoryBarrierWithGroupSync();  // 次の区切りが累積和を書き直す前に、全部が読み終える
    }

    if (i == 0)
        g_treeWords[pool + MR_WIDE_POOL_FREE] = freeCount - gs_grantedBlocks;
}
#endif

// --- 広げた頁の層(T-1240 の 3・4 段目。CPU は FillPageFromUniform)---
groupshared uint32_t gs_expandLayerSlots[MR_MAX_REQUESTS];
groupshared uint32_t gs_expandLayerPages[MR_MAX_REQUESTS];

// 1 区切り(全部のスレッドが呼ぶ): スレッド i が頁を配った一様な枠 slot(無ければ MR_NO_BLOCK)が一様の枠の層を持てば、枠の順に
// 層の頁を空きのスタックの上から取り、一様の枠の層を各セルの値と結んで写し、一様の枠の層を消す。taken は取ろうとした数(グループで一様)。
// 尽きたら数えて層なし(頁はもう配ったので断れない。CPU とずれる。layerPages を広げる分まで持つ約束。仮【エンジン】)
void ExpandLayers(uint32_t i, uint32_t slot, uint32_t freeLayers, inout uint32_t taken) {
    const uint32_t pageCount = LayerPageCount();
    const uint32_t capacity = LayerCapacity();
    const bool layered = slot != MR_NO_BLOCK && UniformHasLayers(slot);
    LayerInclusiveScan(i, layered ? 1u : 0u);

    const uint32_t count = gs_layerScan[MR_MAX_REQUESTS - 1];
    if (layered) {
        const uint32_t position = taken + gs_layerScan[i] - 1;
        uint32_t layerPage = MR_LAYER_NO_PAGE;
        if (position < freeLayers) {
            layerPage = g_layers[MrLayerFreeAddress(pageCount, capacity, freeLayers - 1 - position)];
            AttachLayerPage(g_blocks[slot].page, layerPage);
        } else {
            CountLayerShortage(1);
        }

        gs_expandLayerSlots[gs_layerScan[i] - 1] = slot;
        gs_expandLayerPages[gs_layerScan[i] - 1] = layerPage;
    }

    AllMemoryBarrierWithGroupSync();
    for (uint32_t n = 0; n < count; ++n) {
        const uint32_t expanded = gs_expandLayerSlots[n];
        const uint32_t layerPage = gs_expandLayerPages[n];
        const bool real = g_blocks[expanded].kind == MR_BLOCK_REAL;
        for (uint32_t index = i; index < MR_BLOCK_CELLS && layerPage != MR_LAYER_NO_PAGE; index += MR_MAX_REQUESTS) {
            // MrUniformCell と同じ(見出しを写すと子の並びを動的な添字で読むので、バッファから直に読む。T-0397)
            RxCell cell = g_cells[expanded];
            if (real && g_blocks[expanded].children[MrOctantOfCell(index)] != MR_NO_BLOCK)
                cell = RxMakeEmptyCell(0);

#ifdef MR_WIDE_CELLS
            const LayerTail tail = UniformLayerTail(expanded);  // 値の溢れの成分の層も(T-1240 の 7 の 5)
#else
            const LayerTail tail = NoLayerTail();
#endif
            WriteBoundLayersAt(UniformLayerBase(expanded), cell, tail,
                               MrLayerEntryAddress(pageCount, capacity, layerPage, index, 0));
        }
    }

    AllMemoryBarrierWithGroupSync();  // 全部が一様の枠の層を読み終えてから消す
    for (uint32_t n = 0; n < count && i < MR_LAYER_CELL_WORDS; ++n)
        g_layers[UniformLayerBase(gs_expandLayerSlots[n]) + i] = 0;

    taken += count;
    GroupMemoryBarrierWithGroupSync();  // 次の区切りが累積和と一覧を書き直す前に、全部が読み終える
}

// --- 広げた頁の載せた欄(CPU の ExpandWantedPages の ExpandPage。T-1252 段 2 の 5 本目)---
// 1 区切り(全部のスレッドが呼ぶ): スレッド i が頁を配った一様な枠 slot(無ければ MR_NO_BLOCK)が覆われていないセルを持ち、
// 欄の一様の値に 0 でないものがあれば、欄ごとに枠の順で 1 つ取り(空きの上から k 番目・その後は高さの上 = CPU の StorePage の EnsurePage)、
// 覆われていないセルに一様の値・覆われたセルに 0 を書く。一様の値は消さない(CPU と同じ)。尽きたら数えて頁なし(GPU だけ。CPU とずれる)
void ExpandCarried(uint32_t i, uint32_t slot) {
    const uint32_t count = CarriedFieldCount();
    bool open = false;  // 覆われていないセルがある
    if (slot != MR_NO_BLOCK) {
        const MrBlock block = g_blocks[slot];
        open = block.kind != MR_BLOCK_REAL || MrFoldValueCell(block) != MR_BLOCK_CELLS;
    }

    for (uint32_t f = 0; f < count; ++f) {
        bool need = false;
        if (open)
            need = CarriedUniformNonZero(f, slot);

        const uint32_t freeCount = CarriedHeader(f, MF_HEADER_FREE_COUNT);
        const uint32_t top = CarriedHeader(f, MF_HEADER_TOP);
        const uint32_t available = MfPagesAvailable(CarriedHeader(f, MF_HEADER_CAPACITY), 0u, freeCount, top);
        LayerInclusiveScan(i, need ? 1u : 0u);  // 中のバリアで、全部のスレッドが頭を読み終えてから
        const uint32_t total = gs_layerScan[MR_MAX_REQUESTS - 1];
        if (need) {
            const uint32_t k = gs_layerScan[i] - 1;
            uint32_t fieldPage = MF_NO_PAGE;
            if (k < available) {
                fieldPage = TakenCarriedPage(f, k, freeCount, top);
                SetCarriedPageOf(f, g_blocks[slot].page, fieldPage);
            }

            gs_expandLayerSlots[k] = slot;
            gs_expandLayerPages[k] = fieldPage;
        }

        AllMemoryBarrierWithGroupSync();
        const uint32_t components = CarriedComponents(f);
        for (uint32_t n = 0; n < total; ++n) {
            const uint32_t expanded = gs_expandLayerSlots[n];
            const uint32_t fieldPage = gs_expandLayerPages[n];
            const bool real = g_blocks[expanded].kind == MR_BLOCK_REAL;
            for (uint32_t index = i; index < MR_BLOCK_CELLS && fieldPage != MF_NO_PAGE; index += MR_MAX_REQUESTS) {
                // 覆われたか(子の並びはバッファから直に読む。T-0397)
                const bool covered = real && g_blocks[expanded].children[MrOctantOfCell(index)] != MR_NO_BLOCK;
                for (uint32_t c = 0; c < components; ++c) {
                    int64_t value = 0;
                    if (!covered)
                        value = LoadCarriedUniform(f, expanded, c);

                    StoreCarriedWords(CarriedValueAddress(f, fieldPage, index, c), value);
                }
            }
        }

        DeviceMemoryBarrierWithGroupSync();  // 全部が空きのスタックと頭を読み終えてから書き直す
        if (i == 0 && total > 0) {
            const uint32_t taken = min(total, available);
            const uint32_t fromFree = min(taken, freeCount);
            SetCarriedHeader(f, MF_HEADER_FREE_COUNT, freeCount - fromFree);
            SetCarriedHeader(f, MF_HEADER_TOP, top + (taken - fromFree));
            if (total > taken)
                CountCarriedPagesShort(total - taken);
        }

        DeviceMemoryBarrierWithGroupSync();
    }
}

[numthreads(MR_MAX_REQUESTS, 1, 1)] void TreeExpand(uint32_t i : SV_GroupIndex) {
    const uint32_t freePages = g_counters[MR_COUNTER_FREE_PAGES];
    uint32_t wanted = 0;  // ここまでの区切りで頁に広げたいブロックの数(グループで一様)
    uint32_t freeLayers = 0;
    if (LayerCapacity() > 0)
        freeLayers = g_layers[MrLayerFreeCountAddress(LayerPageCount(), LayerCapacity())];

    uint32_t layersTaken = 0;  // 広げた頁の層の頁(取ろうとした数。T-1240)
    for (uint32_t first = 0; first < g_worldBlocks; first += MR_MAX_REQUESTS) {
        const uint32_t slot = first + i;
        const bool wants = slot < g_worldBlocks && g_blocks[slot].page == MR_PAGE_WANTED;
        InclusiveScan(i, wants ? 1u : 0u, 0u, 0u);

        const uint32_t position = wanted + gs_blockScan[i] - 1;
        const bool granted = wants && position < freePages;
        if (granted) {
            g_blocks[slot].page = g_treeWords[FreePageAddress(freePages - 1 - position)];
            g_graphInput.Store(MR_GRAPH_INPUT_EXPAND_RECORDS + 4 * (position + 1), slot);
            SetSubstepMark(slot, CONDUCT_MARK_EXPANDED);
        } else if (wants) {
            g_blocks[slot].page = MR_NO_PAGE;
            AppendSeedOnce(slot);
            SetSubstepMark(slot, CONDUCT_MARK_FROZEN);  // 伝導: この小刻みは刻まず、面の流れも 0(T-0107)
        }

        if (LayerCapacity() > 0)
            ExpandLayers(i, granted ? slot : MR_NO_BLOCK, freeLayers, layersTaken);

        if (CarriedFieldCount() > 0)
            ExpandCarried(i, granted ? slot : MR_NO_BLOCK);

        wanted += gs_blockScan[MR_MAX_REQUESTS - 1];
        GroupMemoryBarrierWithGroupSync();  // 次の区切りが累積和を書き直す前に、全部が読み終える
    }
#ifdef MR_WIDE_CELLS
    ReserveExpandChunks(i, min(wanted, freePages));
#endif

    if (i != 0)
        return;

    const uint32_t granted = min(wanted, freePages);
    if (LayerCapacity() > 0)
        g_layers[MrLayerFreeCountAddress(LayerPageCount(), LayerCapacity())] = freeLayers -
                                                                               min(layersTaken, freeLayers);

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

// --- 載せた欄を畳む(CPU の FoldExact の ReleaseZeroPages・PageFoldable と FoldPage の欄の分。T-1252 段 2 の 5 本目)---
// TreeFoldCheck が、調べる刻みの頁の全部 0 の欄の頁に返す印(pageOf の上の bit。欄の頁の番号 < 容量 < 2^31)を付け(畳めなくても返す)、
// 欄の頁が一様(覆われていないセル = 値のセル・覆われたセル = 0)でなければ畳まない。畳むなら一様の値 = 値のセルの値。
// TreeFold が枠の順に、畳む頁と印の付いた頁の欄の頁を欄ごとに空きのスタックへ積み、pageOf を消す
static const uint32_t CARRIED_RELEASE_MARK = 0x80000000u;
static const uint32_t CARRIED_FOLD_DIFFER = 1;
static const uint32_t CARRIED_FOLD_NONZERO = 2;

// pageOf の語(印つき)から欄の頁(MF_NO_PAGE はそのまま)
uint32_t CarriedUnmarkedPage(uint32_t raw) {
    if (raw == MF_NO_PAGE)
        return MF_NO_PAGE;

    return raw & ~CARRIED_RELEASE_MARK;
}

// 載せた欄が一様か(全部のスレッドが呼ぶ・グループで一様な結果)。全部 0 の欄の頁には返す印を付ける
bool CarriedFoldable(uint32_t slot, MrBlock block, uint32_t index) {
    const uint32_t count = CarriedFieldCount();
    const uint32_t valueCell = MrFoldValueCell(block);
    const bool covered = block.kind == MR_BLOCK_REAL &&
                         g_blocks[slot].children[MrOctantOfCell(index)] != MR_NO_BLOCK;  // バッファから直に(T-0397)
    bool foldable = true;
    for (uint32_t f = 0; f < count; ++f) {
        const uint32_t fieldPage = CarriedPageOf(f, block.page);  // グループで一様
        if (fieldPage == MF_NO_PAGE)
            continue;

        // 64bit は 2 語の比べ(T-1078・T-1159)
        const uint32_t components = CarriedComponents(f);
        uint32_t mine = 0;
        for (uint32_t c = 0; c < components; ++c) {
            const uint32_t address = CarriedValueAddress(f, fieldPage, index, c);
            uint32_t expectedLow = 0;
            uint32_t expectedHigh = 0;
            if (!covered && valueCell != MR_BLOCK_CELLS) {
                const uint32_t valueAddress = CarriedValueAddress(f, fieldPage, valueCell, c);
                expectedLow = g_layers[valueAddress];
                expectedHigh = g_layers[valueAddress + 1];
            }

            if (g_layers[address] != expectedLow || g_layers[address + 1] != expectedHigh)
                mine |= CARRIED_FOLD_DIFFER;

            if ((g_layers[address] | g_layers[address + 1]) != 0u)
                mine |= CARRIED_FOLD_NONZERO;
        }

        const uint32_t flags = CarriedGroupOr(index, mine);
        if (index == 0 && (flags & CARRIED_FOLD_NONZERO) == 0)
            SetCarriedPageOf(f, block.page, fieldPage | CARRIED_RELEASE_MARK);

        foldable = foldable && (flags & CARRIED_FOLD_DIFFER) == 0;
    }

    return foldable;
}

// 畳む枠 slot の載せた欄の一様の値 = 値のセルの値(全部覆われていれば・欄の頁が無ければ 0。CPU の FoldPage の FoldValue)
void FoldCarriedUniform(uint32_t slot, MrBlock block) {
    const uint32_t count = CarriedFieldCount();
    const uint32_t valueCell = MrFoldValueCell(block);
    for (uint32_t f = 0; f < count; ++f) {
        uint32_t fieldPage = MF_NO_PAGE;
        if (valueCell != MR_BLOCK_CELLS)
            fieldPage = CarriedUnmarkedPage(g_layers[CarriedStoreBase(f) + MfPageOfAddress(block.page)]);

        const uint32_t components = CarriedComponents(f);
        for (uint32_t c = 0; c < components; ++c)
            StoreCarriedUniform(f, slot, c, LoadCarriedPage(f, fieldPage, valueCell, c));
    }
}

// 1 グループで、スレッド i の枠のセルの頁 page(頁を持たない・世界の外なら MR_NO_PAGE)の欄の頁を、畳む(folds)か返す印があれば
// 欄ごとに枠の順に空きのスタックへ積む(CPU の ReleasePage。中身は消さない)。全部のスレッドが呼ぶ
void ReleaseFoldedCarried(uint32_t i, uint32_t page, bool folds) {
    const uint32_t count = CarriedFieldCount();
    for (uint32_t f = 0; f < count; ++f) {
        uint32_t raw = MF_NO_PAGE;
        if (page != MR_NO_PAGE)
            raw = g_layers[CarriedStoreBase(f) + MfPageOfAddress(page)];

        const uint32_t fieldPage = CarriedUnmarkedPage(raw);
        const bool marked = fieldPage != raw;
        const bool give = fieldPage != MF_NO_PAGE && (folds || marked);
        const uint32_t freeCount = CarriedHeader(f, MF_HEADER_FREE_COUNT);
        LayerInclusiveScan(i, give ? 1u : 0u);
        const uint32_t total = gs_layerScan[MR_MAX_REQUESTS - 1];
        if (give) {
            SetCarriedFreePage(f, freeCount + gs_layerScan[i] - 1, fieldPage);
            SetCarriedPageOf(f, page, MF_NO_PAGE);
        }

        DeviceMemoryBarrierWithGroupSync();
        if (i == 0 && total > 0)
            SetCarriedHeader(f, MF_HEADER_FREE_COUNT, freeCount + total);

        DeviceMemoryBarrierWithGroupSync();
    }
}

// 畳む時の一様の値(覆われていない最初のセル。全部覆われていれば空)
RxCell FoldValue(MrBlock block) {
    const uint32_t valueCell = MrFoldValueCell(block);
    if (valueCell == MR_BLOCK_CELLS)
        return RxMakeEmptyCell(0);

    return g_cells[PageCellAddress(block.page, valueCell)];
}

// 帳簿の段 ledgerLevel に bits(その段の単位 × 2^-64)を足す。桁あふれ(= 1 段粗い段の 2^61)は粗い段へ繰り上げる
// (multires_activity.cpp の AddLedgerBits と同じ結果。atomic の前の値で自分の足し算の桁あふれが分かる)
// (世界の帳簿の「粗くした端数・出」の欄。T-0712)
void AddLedgerCarrying(int32_t ledgerLevel, uint32_t column, uint64_t bits) {
    AddWorldLedgerCarrying(WlSlot(WL_REASON_COARSEN_REMAINDER, WL_DIRECTION_OUT), ledgerLevel, column, bits);
}

// 端数の枠のセル index の端数を帳簿の同じ段へ(ReturnFractionsToLedger の 1 セル分)。
// 溢れを使う世界(multires_tree_wide_fold_check)は端数の枠の今の面の溢れの成分も返す(CPU は上限の無い形で読む。T-0347)
void ReturnFractionCell(MrBlock block, uint32_t index) {
    const MrFraction fraction = g_fractions[FractionAddress(block.fraction, index)];
    AddLedgerCarrying(block.level, 0, fraction.energy);
    [unroll] for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {  // 決まった数まで回して成分の数で抜ける(T-1266)
        if (i >= fraction.speciesCount)
            break;

        AddLedgerCarrying(block.level, 1 + fraction.species[i], fraction.amounts[i]);
    }

#ifdef MR_WIDE_CELLS
    const uint32_t area = WideFractionArea(block.fraction);
    const uint32_t side = WideCurrentSide(area);
    const uint2 range = WideRange(area, side, index);
    for (uint32_t entry = range.x; entry < range.y; ++entry) {
        const uint32_t word = MrWideEntryWord(entry);
        const uint64_t amount = FX_U64(g_treeWords[WideWord(area, side, word + 2)],
                                       g_treeWords[WideWord(area, side, word + 1)]);
        AddLedgerCarrying(block.level, 1 + g_treeWords[WideWord(area, side, word)], amount);
    }
#endif
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
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {  // 決まった数まで回して成分の数で抜ける(T-1266)
        if (i >= cell.speciesCount)
            break;

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
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {  // 決まった数まで回して成分の数で抜ける(T-1266)
        if (i >= cell.speciesCount)
            break;

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
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {  // 決まった数まで回して成分の数で抜ける(T-1266)
        if (!uncovered || i >= cell.speciesCount)
            break;

        total = total + cell.amounts[i] < total ? FX_U64(0xFFFFFFFFu, 0xFFFFFFFFu) : total + cell.amounts[i];
    }

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

    uint32_t presentCells = 0;
    uint64_t lowAmount = FX_U64(0xFFFFFFFFu, 0xFFFFFFFFu);
    uint64_t highAmount = 0;
    uint64_t totalLow = 0;
    uint64_t totalHigh = 0;
    for (uint32_t wave = 0; wave < gs_foldWaves; ++wave) {
        presentCells += gs_foldCount[wave];
        lowAmount = min(lowAmount, gs_foldLow[wave]);
        highAmount = max(highAmount, gs_foldHigh[wave]);
        totalLow += gs_foldSumLow[wave];
        totalHigh += gs_foldSumHigh[wave];
    }

    // 位置 position の欄だけに書く(動的な添字で書かない。T-1266。position の欄は 0 から始まるので、前の形の += と同じ)
    const MrWide sum = CombineFoldSum((int64_t)totalHigh, totalLow);
    [unroll] for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) {
        if (k != position)
            continue;

        stats.species[k] = id;
        stats.present[k] += presentCells;
        stats.lowAmount[k] = lowAmount;
        stats.highAmount[k] = max(stats.highAmount[k], highAmount);
        stats.sumLow[k] = sum.hi;
        stats.sumHigh[k] = sum.top;
    }

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
    [unroll] for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {  // 決まった数まで回して成分の数で抜ける(T-1266)
        if (i >= stats.speciesCount)
            break;

        AddLedgerCarrying(block.level - 3, 1 + stats.species[i], value.amountRemainders[i] << 55u);
    }

    g_cells[slot] = value.cell;

    return true;
}

// --- 層のある頁を畳む(T-1240 の 3 段目。CPU は multires_activity.cpp の FoldLayeredPage・FoldNear)---
static const uint32_t FOLD_LAYERS_NONZERO = 1;  // 0 でない層がある(平均では畳まない。CPU の PageHasLayers)
static const uint32_t FOLD_LAYERS_DIFFER = 2;   // 覆われていないセルの層が値のセルの層と違う(層ごとには畳めない)
static const uint32_t
    FOLD_LAYERS_FLOATING = 4;  // 層を使う世界で、覆われていないセルに舞う粒(粒の物質の 量 > 層)がある(畳まない)

groupshared uint32_t gs_foldLayerFlags;

// 層の頁 layerPage(無ければ MR_LAYER_NO_PAGE = 層は全部 0)を持つ頁の層の印(全部のスレッドが呼ぶ。グループで一様な結果)。
// 層は今のセルと結んで読む。舞う粒は層を使う世界(LayerGrainFloats)だけ見る(CPU の HasFloatingGrain・PageHasFloatingGrain)
uint32_t FoldLayerFlags(MrBlock block, uint32_t layerPage, uint32_t index) {
    if (index == 0)
        gs_foldLayerFlags = 0;

    GroupMemoryBarrierWithGroupSync();
    const RxCell cell = g_cells[PageCellAddress(block.page, index)];
    const bool layered = layerPage != MR_LAYER_NO_PAGE;
    const uint32_t base = layered ? MrLayerEntryAddress(LayerPageCount(), LayerCapacity(), layerPage, index, 0) : 0u;

    // 覆われたかは 1 回だけ読む(枠の子の並びを動的な添字で読むので、読む所ごとに局所の配列ができる。溢れの変種で増えた。T-0397)
    const bool covered = MrIsCoveredCell(block, index);
    uint32_t mine = 0;
    FX_UNROLL
    for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES && layered; ++k) {
        const uint32_t address = base + (k * MR_LAYER_ENTRY_WORDS);
        const uint64_t layer = MrLayerAfterWrite(cell, g_layers[address],
                                                 FX_U64(g_layers[address + 2], g_layers[address + 1]));
        if (((uint32_t)layer | (uint32_t)(layer >> 32)) != 0)
            mine |= FOLD_LAYERS_NONZERO;
    }

    // 舞う粒(覆われていないセルの粒の物質の 量 > 層)
    if (LayerGrainFloats() && !covered) {
        FX_UNROLL
        for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
            if (i < cell.speciesCount && (g_species[cell.species[i]].flags & RX_SPECIES_GRAIN) != 0) {
                uint64_t layer = 0;
                if (layered)
                    layer = BoundLayerOf(base, cell.species[i], cell);

                if (MrLayerLess(layer, cell.amounts[i]))
                    mine |= FOLD_LAYERS_FLOATING;
            }
        }
    }

    // 値のセルと同じ層か(CPU は覆われていないセルの層の並び〔今のセルの成分の順〕を値のセルの層の並びと比べる)
    const uint32_t valueCell = MrFoldValueCell(block);
    if (layered && !covered && valueCell != MR_BLOCK_CELLS) {
        const RxCell value = g_cells[PageCellAddress(block.page, valueCell)];
        const uint32_t valueBase = MrLayerEntryAddress(LayerPageCount(), LayerCapacity(), layerPage, valueCell, 0);
        if (cell.speciesCount != value.speciesCount)
            mine |= FOLD_LAYERS_DIFFER;

        FX_UNROLL
        for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
            if (i < cell.speciesCount && i < value.speciesCount) {
                const uint64_t a = BoundLayerOf(base, cell.species[i], cell);
                const uint64_t b = BoundLayerOf(valueBase, value.species[i], value);
                if ((uint32_t)a != (uint32_t)b || (uint32_t)(a >> 32) != (uint32_t)(b >> 32))
                    mine |= FOLD_LAYERS_DIFFER;
            }
        }
    }

    if (mine != 0)
        InterlockedOr(gs_foldLayerFlags, mine);

    GroupMemoryBarrierWithGroupSync();

    return gs_foldLayerFlags;
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
#ifdef MR_WIDE_CELLS
        // 返す枠の溢れを空に(全部のセルが読み終えてから。CPU の ClearFractionOverflow。塊は TreeWideTrim が返す。T-0347)
        AllMemoryBarrierWithGroupSync();
        ClearWideSideWords(WideFractionArea(block.fraction), index, MR_BLOCK_CELLS);
#endif
        block.fraction = MR_NO_FRACTION;
        flags |= MR_CLAIM_FOLD_RETURN;
    }

    // --- 畳めるか: まずビット単位(値が変わらない)。違えば、許容差つきで覆われていないセルがあれば、ほぼ同じか(T-0125。
    //     FoldsExactly はグループで一様な結果)。溢れを使う世界は、溢れのある頁を畳まない(GPU の一様の値はまだインラインだけで、
    //     畳むと溢れを落とす。仮。溢れまで畳むのは T-0312)---
    bool foldable = MrWantsFoldCheck(block, mark);

    // 載せた欄(T-1252): 全部 0 の欄の頁に返す印・欄が一様でなければ畳まない(CPU の FoldExact・FoldNear の PageFoldable。グループで一様な分岐)
    if (foldable && CarriedFieldCount() > 0) {
        if (!CarriedFoldable(slot, block, index))
            foldable = false;
    }

    // 層の頁を持つ頁は層まで一様な時だけ層ごと畳み(CPU の FoldLayeredPage)、0 でない層があれば平均では畳まない(FoldNear。T-1240)
    uint32_t layerPage = MR_LAYER_NO_PAGE;
    uint32_t layerFlags = 0;
    if (foldable)
        layerPage = LayerPageOf(block.page);

    if (foldable && (layerPage != MR_LAYER_NO_PAGE || LayerGrainFloats()))
        layerFlags = FoldLayerFlags(block, layerPage, index);
#ifdef MR_WIDE_CELLS
    if (foldable)
        foldable = WideTotal(block.page) == 0;
#endif
    if (foldable) {
        const bool exact = FoldsExactly(block, index);
        if (exact && (layerFlags & (FOLD_LAYERS_DIFFER | FOLD_LAYERS_FLOATING)) == 0)
            flags |= MR_CLAIM_FOLD_COPY;
        else if (!MrIsExactFold(tolerance) && MrFoldValueCell(block) != MR_BLOCK_CELLS &&
                 (layerFlags & (FOLD_LAYERS_NONZERO | FOLD_LAYERS_FLOATING)) == 0)
            flags |= FoldsNearly(slot, block, index, tolerance) ? MR_CLAIM_FOLD_WRITTEN : 0u;
    }

    // --- 一様の枠の層: 層ごと畳むなら値のセルの層を今の値と結んで書く・平均で畳むなら 0(CPU の SetUniformLayers・
    //     ClearUniformLayers)。層の頁は TreeFold が頁と一緒に返す(T-1240)---
    const bool folds = (flags & (MR_CLAIM_FOLD_COPY | MR_CLAIM_FOLD_WRITTEN)) != 0;
    if (index == 0 && folds && LayerCapacity() > 0) {
        const uint32_t valueCell = MrFoldValueCell(block);
        if ((flags & MR_CLAIM_FOLD_COPY) != 0 && layerPage != MR_LAYER_NO_PAGE && valueCell != MR_BLOCK_CELLS)
            WriteBoundLayers(MrLayerEntryAddress(LayerPageCount(), LayerCapacity(), layerPage, valueCell, 0),
                             FoldValue(block), UniformLayerBase(slot));
        else
            ClearLayerWords(UniformLayerBase(slot));
    }

    if (index == 0 && folds && CarriedFieldCount() > 0)
        FoldCarriedUniform(slot, block);

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

        // 畳む頁の層の頁も返す(CPU の ReleaseFoldedPage の ClearPageLayers。層を持たない世界は何もしない。T-1240)
        if (LayerCapacity() > 0) {
            uint32_t foldedPage = MR_NO_PAGE;
            if (folds)
                foldedPage = g_blocks[slot].page;

            ReleaseLayerPages(i, foldedPage);
        }

        // 畳む頁と全部 0 の欄の頁の載せた欄の頁を返す(CPU の FoldExact の ReleaseZeroPages・FoldPage。T-1252)
        if (CarriedFieldCount() > 0) {
            uint32_t carriedPage = MR_NO_PAGE;
            if (slot < g_worldBlocks) {
                if (!MrIsUniform(g_blocks[slot]))
                    carriedPage = g_blocks[slot].page;
            }

            ReleaseFoldedCarried(i, carriedPage, folds);
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

#ifdef MR_WIDE_CELLS
// --- 溢れを使う世界: 細かくする子の頁・粗くする親の新しい面に溢れの塊を付ける(T-0237・T-0329。TreeAllocate の後・Work Graph の前。1 グループ)---
// 子の頁の面 0 に、親の八分の一のセルの溢れを写す(multires_graph.hlsl の RefineWideTails)ための塊と、粗くする親の頁・端数の枠の
// 新しい面(WriteCoarsenTails)の塊を、許可した要求の順の累積和で空きのスタックの上から配る(WideAllocate と同じ型)。
// 足りなければその要求には配らない(写す時に数える。置き場が尽きた時だけ)。どの塊を使うかは結果に出ない。前から持つ塊はそのまま使う。
// 粗くした溢れの数は粗くするまで分からないので、上限(セルごとに min(RX_GPU_WIDE_SPECIES, 子の成分の和) − インライン)で取る。
// 頁の余りは WriteCoarsenTails が要求の語で WideAllocate に返させる。端数の枠の余りは付けたまま(T-0347)

static const uint32_t MR_WIDE_RESERVE_NONE = 0;

// 細かくする要求の depth 段目の子の頁(または端数の枠)に写す溢れの数(一番上の親の領域 page〔頁か端数の枠。T-0311〕の今の面から。
// 子のセルを親のセルへたどる)
uint32_t RefineTailEntries(MrRequest request, MrRequestState state, uint32_t depth, uint32_t page) {
    uint32_t octants[MR_MAX_CHAIN_LEVELS];
    MrBlock block = g_blocks[state.target];
    for (uint32_t level = 0; level <= depth; ++level) {
        octants[level] = MrOctantOfPoint(block, request.x, request.y, request.z, request.level);
        block = MrMakeChildBlock(block, state.target, octants[level], MR_BLOCK_REAL, MR_NO_FRACTION, MR_NO_PAGE);
    }

    const uint32_t side = WideCurrentSide(page);
    uint32_t total = 0;
    for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
        uint32_t cell = index;
        for (uint32_t level = depth + 1; level > 0; --level)
            cell = MrParentCellOfChild(octants[level - 1], cell);

        const uint2 range = WideRange(page, side, cell);
        total += range.y - range.x;
    }

    return total;
}

// 一番上の親の領域 top(頁か端数の枠)の溢れを、段ごとの子の領域(fractions なら端数の枠、でなければ頁)の面 0 へ写すのに足す塊の数
// (attach なら stack の上から taken 番目より下を取って子の領域の面 0 の表に足す)
uint32_t ReserveTailChunks(MrRequest request, MrRequestState state, uint32_t top, bool fractions, bool attach,
                           uint32_t stack, uint32_t freeCount, uint32_t taken) {
    // --- 溢れの無い親は写すものが無い ---
    if (WideRange(top, WideCurrentSide(top), MR_BLOCK_CELLS - 1).y == 0)
        return MR_WIDE_RESERVE_NONE;

    uint32_t chunks = 0;
    for (uint32_t depth = 0; depth < state.levels; ++depth) {
        const uint32_t area = fractions ? WideFractionArea(PoppedFraction(state, depth)) : PoppedPage(state, depth);
        const uint32_t need = MrWideChunksFor(RefineTailEntries(request, state, depth, top));
        const uint32_t table = MrWideTableAt(WideAreaHeader(area), 0);
        const uint32_t held = g_treeWords[table];
        if (need <= held)
            continue;

        if (attach) {
            for (uint32_t j = held; j < need; ++j) {
                g_treeWords[WideTableEntryWord(table, j)] = g_treeWords[stack + freeCount - 1 - taken];
                taken += 1;
            }

            g_treeWords[table] = need;
        }

        chunks += need - held;
    }

    return chunks;
}

// --- 粗くする要求(T-0329)---

// 子のセル index の成分の数(セルと端数の、インライン + 今の面の溢れ)
uint32_t ChildSpeciesCount(MrBlock child, uint32_t childSlot, uint32_t index) {
    uint32_t count = LoadBlockCell(child, childSlot, index).speciesCount;
    if (!MrIsUniform(child)) {
        const uint2 range = WideRange(child.page, WideCurrentSide(child.page), index);
        count += range.y - range.x;
    }

    if (child.fraction != MR_NO_FRACTION) {
        const uint32_t area = WideFractionArea(child.fraction);
        const uint2 range = WideRange(area, WideCurrentSide(area), index);
        count += g_fractions[FractionAddress(child.fraction, index)].speciesCount + (range.y - range.x);
    }

    return count;
}

// 粗くした親の八分の一の溢れの数の上限(セルごとに min(RX_GPU_WIDE_SPECIES, 子の成分の和) − インライン)
uint32_t CoarsenTailBound(MrBlock child, uint32_t childSlot) {
    uint32_t total = 0;
    for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
        uint32_t sum = 0;
        for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
            sum += ChildSpeciesCount(child, childSlot, MrChildCell(local, j));

        const uint32_t species = min(sum, RX_GPU_WIDE_SPECIES);
        total += species > RX_MAX_CELL_SPECIES ? species - RX_MAX_CELL_SPECIES : 0u;
    }

    return total;
}

// 領域 area の新しい面(今の面でない方)に、溢れ entries 個の分の塊を足す数(attach なら stack の上から taken 番目より下を付ける)
uint32_t ReserveSideChunks(uint32_t area, uint32_t entries, bool attach, uint32_t stack, uint32_t freeCount,
                           uint32_t taken) {
    const uint32_t need = MrWideChunksFor(entries);
    const uint32_t table = MrWideTableAt(WideAreaHeader(area), 1 - WideCurrentSide(area));
    const uint32_t held = g_treeWords[table];
    if (need <= held)
        return MR_WIDE_RESERVE_NONE;

    if (attach) {
        for (uint32_t j = held; j < need; ++j)
            g_treeWords[WideTableEntryWord(table, j)] = g_treeWords[stack + freeCount - 1 - (taken + (j - held))];

        g_treeWords[table] = need;
    }

    return need - held;
}

// 粗くする要求に足す塊の数(親の頁の新しい面、続けて親の端数の枠の新しい面。広げた頁・取った端数の枠は残りの溢れが無い)
uint32_t ReserveCoarsenChunks(MrRequestState state, bool attach, uint32_t stack, uint32_t freeCount, uint32_t taken) {
    const MrBlock child = g_blocks[state.target];
    const MrBlock parent = g_blocks[child.parent];
    const uint32_t bound = CoarsenTailBound(child, state.target);
    if (bound == 0)
        return MR_WIDE_RESERVE_NONE;

    uint32_t chunks = 0;
    const bool expand = state.pageNeed != 0;
    if (expand || !MrIsUniform(parent)) {
        const uint32_t page = expand ? PoppedPage(state, 0) : parent.page;
        const uint32_t rest = expand ? 0u : WideTotal(page);
        chunks += ReserveSideChunks(page, rest + bound, attach, stack, freeCount, taken);
    }

    const bool kept = parent.fraction != MR_NO_FRACTION;
    const uint32_t area = WideFractionArea(kept ? parent.fraction : PoppedFraction(state, 0));
    const uint32_t rest = kept ? WideTotal(area) : 0u;
    chunks += ReserveSideChunks(area, rest + bound, attach, stack, freeCount, taken + chunks);

    return chunks;
}

// 要求 i に足す塊の数(細かくする: 子の頁の分、続けて子の端数の枠の分。T-0311。粗くする: 親の新しい面。T-0329)
uint32_t ReserveRefineChunks(uint32_t i, bool attach, uint32_t stack, uint32_t freeCount, uint32_t taken) {
    if (i >= RequestCount())
        return MR_WIDE_RESERVE_NONE;

    const MrRequestState state = g_states[i];
    const MrRequest request = g_requests[i];
    if (state.status != MR_STATUS_GRANTED)
        return MR_WIDE_RESERVE_NONE;

    if (request.op == MR_REQUEST_COARSEN)
        return ReserveCoarsenChunks(state, attach, stack, freeCount, taken);

    if (request.op != MR_REQUEST_REFINE)
        return MR_WIDE_RESERVE_NONE;

    // --- 一様な親の頁は溢れを持たない(GPU の一様の値はインラインだけ)。端数の枠は一様な親でも持てる ---
    const MrBlock parent = g_blocks[state.target];
    uint32_t chunks = 0;
    if (!MrIsUniform(parent))
        chunks += ReserveTailChunks(request, state, parent.page, false, attach, stack, freeCount, taken);

    if (parent.fraction != MR_NO_FRACTION) {
        const uint32_t top = WideFractionArea(parent.fraction);
        chunks += ReserveTailChunks(request, state, top, true, attach, stack, freeCount, taken + chunks);
    }

    return chunks;
}

[numthreads(MR_MAX_REQUESTS, 1, 1)] void TreeWideReserve(uint32_t i : SV_GroupIndex) {
    const uint32_t pool = WidePoolWord();
    const uint32_t stack = MrWideStackWord(pool, g_treeWords[pool + MR_WIDE_POOL_CAPACITY]);
    const uint32_t freeCount = g_treeWords[pool + MR_WIDE_POOL_FREE];
    const uint32_t want = ReserveRefineChunks(i, false, stack, freeCount, 0);
    if (i == 0)
        gs_grantedBlocks = 0;

    InclusiveScan(i, want, 0, 0);

    // --- 累積和が空きに入る要求だけ配る(累積和は増えるだけなので、配るのは一覧の前から続く)---
    const uint32_t sum = gs_blockScan[i];
    if (want != 0 && sum <= freeCount) {
        ReserveRefineChunks(i, true, stack, freeCount, sum - want);
        InterlockedMax(gs_grantedBlocks, sum);
    }

    GroupMemoryBarrierWithGroupSync();
    if (i == 0)
        g_treeWords[pool + MR_WIDE_POOL_FREE] = freeCount - gs_grantedBlocks;
}

// --- 溢れを使う世界: 端数の枠の塊を返す(T-0347。要求の処理の後と、頁を畳んだ後。1 グループ)---
// 端数の枠は今の面の溢れに要る塊(MrWideChunksFor)だけを持つ。使っている枠(ブロックが持つ)は今の面の余りともう片方の面の塊を、
// 空きの枠(TreeRelease・TreeFold が返した。溢れは空)は全部の塊を、項目の順(ブロックの枠の順 → 空きのスタックの順)の累積和で
// 置き場の空きのスタックに積む。粗くした親の新しい面の上限で取った余り(T-0329)・覆った親の余り・返した枠の塊がここで戻る。
// どの塊を返すかは結果に出ない(CPU は塊を持たない)。刻みの段の WideAllocate は頁しか見ないので、端数の枠はここだけで返す

// 項目 item の端数の枠(item < g_blockCount はブロックの枠が持つ枠、その後ろは空きのスタック。無ければ MR_NO_FRACTION)
uint32_t TrimFractionAt(uint32_t item, uint32_t freeFractions) {
    if (item < g_blockCount)
        return g_blocks[item].fraction;

    const uint32_t position = item - g_blockCount;

    return position < freeFractions ? g_freeFractions[position] : MR_NO_FRACTION;
}

// 端数の枠 fractionSlot が返す塊の数(使っていなければ全部)。give なら返す塊を stack の position から積み、塊の表を詰める
uint32_t TrimFractionChunks(uint32_t fractionSlot, bool used, bool give, uint32_t stack, uint32_t position) {
    if (fractionSlot == MR_NO_FRACTION)
        return 0;

    const uint32_t area = WideFractionArea(fractionSlot);
    const uint32_t header = WideAreaHeader(area);
    const uint32_t current = g_treeWords[header + MR_WIDE_HEADER_SIDE];
    const uint32_t need = used ? MrWideChunksFor(WideTotal(area)) : 0u;
    uint32_t given = 0;
    for (uint32_t side = 0; side < 2; ++side) {
        const uint32_t table = MrWideTableAt(header, side);
        const uint32_t held = g_treeWords[table];
        const uint32_t keep = side == current ? min(held, need) : 0u;
        if (give && keep != held) {
            for (uint32_t j = keep; j < held; ++j)
                g_treeWords[stack + position + given + (j - keep)] = g_treeWords[WideTableEntryWord(table, j)];

            g_treeWords[table] = keep;
        }

        given += held - keep;
    }

    if (give && !used)
        g_treeWords[header + MR_WIDE_HEADER_SIDE] = 0;  // 空きの枠は面 0(塊なし = 空)

    return given;
}

[numthreads(MR_MAX_REQUESTS, 1, 1)] void TreeWideTrim(uint32_t i : SV_GroupIndex) {
    const uint32_t pool = WidePoolWord();
    const uint32_t stack = MrWideStackWord(pool, g_treeWords[pool + MR_WIDE_POOL_CAPACITY]);
    const uint32_t freeCount = g_treeWords[pool + MR_WIDE_POOL_FREE];
    const uint32_t freeFractions = g_counters[MR_COUNTER_FREE_FRACTIONS];
    const uint32_t items = g_blockCount + freeFractions;
    uint32_t given = 0;  // ここまでの区切りで返した塊の数(グループで一様)
    for (uint32_t first = 0; first < items; first += MR_MAX_REQUESTS) {
        const uint32_t item = first + i;
        const uint32_t fractionSlot = item < items ? TrimFractionAt(item, freeFractions) : MR_NO_FRACTION;
        const bool used = item < g_blockCount;
        const uint32_t want = TrimFractionChunks(fractionSlot, used, false, stack, 0);
        InclusiveScan(i, want, 0, 0);

        if (want != 0)
            TrimFractionChunks(fractionSlot, used, true, stack, freeCount + given + gs_blockScan[i] - want);

        given += gs_blockScan[MR_MAX_REQUESTS - 1];
        GroupMemoryBarrierWithGroupSync();  // 次の区切りが累積和を書き直す前に、全部が読み終える
    }

    if (i == 0 && given != 0)
        g_treeWords[pool + MR_WIDE_POOL_FREE] = freeCount + given;
}
#endif
