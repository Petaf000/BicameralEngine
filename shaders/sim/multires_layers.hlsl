// multires_layers.hlsl — 層を合わせる段(T-1221。ADR-0481 の 5・20 §10.5): 刻みの段(反応)がセルの成分の量を書いた後、
// 層の頁のある頁のセルごとに、成分ごとの層を物質で結んで 層 = min(前の層, 今の量) に付け替える(CPU の StorePageCell の付け替えと同じ規則)。
// ルート署名は多重解像度のもの(multires_bindings.hlsli。sim/gpu_multires.cpp)で、外のバッファ u4 に層の置き場を結ぶ(並びは
// common/multires_layers.hlsli)。g_external0 = 層の頁の数(置き場の大きさ)。1 グループ = 層の頁 1 つの 64 セル、1 スレッド = セル 1 つ。
// セルは読むだけ。CPU リファレンスは sim::StoreDenseCellImage → nest_detail::StorePageCell(試験 gpu_multires_layers)。
#include "common/multires_layers.hlsli"
#include "sim/multires_bindings.hlsli"

RWStructuredBuffer<uint32_t> g_layers : register(u4);

// 持ち主の枠が今もその頁を持っているか(頁が返された・別の枠に配られたなら、その層の頁は全部 0 にする)
bool LayerPageOwned(uint32_t page, uint32_t owner) {
    if (owner >= g_blockCount)
        return false;

    const MrBlock block = g_blocks[owner];

    return block.kind != MR_BLOCK_UNUSED && block.page == page;
}

[numthreads(MR_LAYER_GROUP_THREADS, 1, 1)] void ReconcileLayers(uint3 groupId : SV_GroupID,
                                                                uint3 threadId : SV_GroupThreadID) {
    const uint32_t capacity = g_external0;
    const uint32_t layerPage = groupId.x / MR_LAYER_GROUPS_PER_PAGE;
    if (layerPage >= capacity)
        return;

    const uint32_t header = MrLayerHeaderAddress(layerPage);
    const uint32_t page = g_layers[header];
    if (page == MR_LAYER_NO_PAGE)
        return;

    // --- 今のセル(持ち主が頁を手放していれば空のセルとして読む = 層は全部 0)---
    const uint32_t index = ((groupId.x % MR_LAYER_GROUPS_PER_PAGE) * MR_LAYER_GROUP_THREADS) + threadId.x;
    const bool owned = LayerPageOwned(page, g_layers[header + 1]);
    RxCell cell = RxMakeEmptyCell(0);
    if (owned)
        cell = g_cells[PageCellAddress(page, index)];

    // --- 組ごとに付け替える(変わった組だけ書く)---
    FX_UNROLL
    for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) {
        const uint32_t address = MrLayerEntryAddress(capacity, layerPage, index, k);
        const uint32_t low = g_layers[address + 1];
        const uint32_t high = g_layers[address + 2];
        if ((low | high) == 0)
            continue;

        const uint64_t layer = ((uint64_t)high << 32) | low;
        const uint64_t next = MrLayerAfterWrite(cell, g_layers[address], layer);
        const uint32_t nextLow = (uint32_t)next;
        const uint32_t nextHigh = (uint32_t)(next >> 32);
        if (nextLow == low && nextHigh == high)
            continue;

        g_layers[address + 1] = nextLow;
        g_layers[address + 2] = nextHigh;
    }
}
