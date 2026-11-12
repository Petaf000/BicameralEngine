// multires_field_gather_tree.hlsl — GPU の世界(sim/gpu_multires)の集めて書く段(T-1252 段 2 の 6 本目。ADR-0500 の 3・ADR-0532 の 4・8)。
// 書き手(光の持ち越し T-1227・源の項 T-1228)が刻みの中で作業(u5。並びは common/multires_field_pages.hlsli の「作業」)に足した値を、
// 欄の区画(u4 の層の置き場の後ろ。始まり g_external2・載せた欄の数 g_external3)の欄 g_external0 へ書く。段の中身は
// sim/multires_field_gather.hlsli(置き場 1 つの試験の段と同じ)。CPU リファレンスは sim/multires_fields.cpp の GatherNestField(同じ順):
//   GatherTreeMark(g_external1 = 1)  作業のある一様な枠に MR_PAGE_WANTED(CPU の ExpandGatheredBlocks)
//   TreeExpand(木の段。GpuMultires::RecordGather が BindLayers の定数で投げる)  枠の順に頁を配る・層と載せた欄も広げる
//   GatherTreeFill                    配った頁を一様の値で埋める(CPU の FillPageFromUniform のセル。層は TreeExpand が写した)
//   GatherTreeMark(g_external1 = 0)  段の結果を付け直す(頁が足りず一様のままの枠は WANTED = 報告の cellPageShortage)
//   GatherTreeAssign                  枠の順のプレフィックス和で欄の頁を取る
//   GatherTreeWrite                   作業を欄へ足して 0 に・書いた枠をつつく(CPU の PokeBlock = この刻みの種)
// 溢れを使う世界はまだ(RecordGather が断る。一様の値の溢れを埋める段が要る)。
#include "sim/multires_bindings.hlsli"
#include "sim/multires_field_access.hlsli"

RWStructuredBuffer<uint32_t> g_gatherWork : register(u5);

uint32_t GatherEntry() {
    return g_external0;
}

uint32_t GatherStoreWord(uint32_t address) {
    return g_layers[CarriedStoreBase(GatherEntry()) + address];
}

void SetGatherStoreWord(uint32_t address, uint32_t value) {
    g_layers[CarriedStoreBase(GatherEntry()) + address] = value;
}

uint32_t GatherWorkWord(uint32_t address) {
    return g_gatherWork[address];
}

void SetGatherWorkWord(uint32_t address, uint32_t value) {
    g_gatherWork[address] = value;
}

uint32_t GatherCellPage(uint32_t slot) {
    return g_blocks[slot].page;
}

uint32_t GatherWorldBlocks() {
    return g_worldBlocks;
}

#include "sim/multires_field_gather.hlsli"

// --- 印(1 グループ = 世界の枠 1 つ)---
[numthreads(GATHER_THREADS, 1, 1)] void GatherTreeMark(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID) {
    const uint32_t slot = groupId.x;
    const uint32_t state = GatherMarkState(slot, threadId.x);
    if (threadId.x != 0)
        return;

    SetGatherWorkWord(GatherStateAddress(slot), state);
    if (g_external1 != 0 && state == MF_GATHER_WANTED && g_blocks[slot].page == MR_NO_PAGE)
        g_blocks[slot].page = MR_PAGE_WANTED;
}

    // --- 配った頁を一様の値で埋める(1 グループ = TreeExpand の一覧のレコード g + 1。グループの数は世界の枠の数 = 一覧の上限)---
    [numthreads(GATHER_THREADS, 1, 1)] void GatherTreeFill(uint3 groupId : SV_GroupID,
                                                           uint3 threadId : SV_GroupThreadID) {
    const uint32_t record = groupId.x + 1;
    if (record >= ExpandRecordCount())
        return;

    const uint32_t slot = ExpandRecord(record);
    const MrBlock block = g_blocks[slot];
    const uint32_t index = threadId.x;
    g_cells[PageCellAddress(block.page, index)] = MrUniformCell(block, g_cells[slot], index);
}

// --- 頁を取る(1 グループ)。広げた枠の数も報告へ(CPU の NestGatherReport::expanded)---
[numthreads(GATHER_THREADS, 1, 1)] void GatherTreeAssign(uint3 threadId : SV_GroupThreadID) {
    GatherAssignPages(threadId.x);
    if (threadId.x == 0)
        SetGatherStoreWord(MF_HEADER_EXPANDED, ExpandRecordCount() - 1u);
}

    // --- 書く(1 グループ = 世界の枠 1 つ・1 スレッド = セル 1 つ)---
    [numthreads(GATHER_THREADS, 1, 1)] void GatherTreeWrite(uint3 groupId : SV_GroupID,
                                                            uint3 threadId : SV_GroupThreadID) {
    const uint32_t slot = groupId.x;
    if (GatherWriteCell(slot, threadId.x) && threadId.x == 0)
        PokeBlock(slot);
}
