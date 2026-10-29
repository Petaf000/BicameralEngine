// multires_layers.hlsl — 層を合わせる段(T-1221・T-1240。ADR-0481 の 5・20 §10.5): 刻みの段(反応)がセルの成分の量を書いた後、
// 層の頁を持つ頁のセルごとに、成分ごとの層を物質で結んで 層 = min(前の層, 今の量) に付け替える(CPU の StorePageCell の付け替えと同じ規則)。
// ルート署名は多重解像度のもの(multires_bindings.hlsli。sim/gpu_multires.cpp)で、外のバッファ u4 に層の置き場を結ぶ(並びは
// common/multires_layers.hlsli・道具は sim/multires_layer_access.hlsli)。1 グループ = 層の頁 1 つの 64 セル、1 スレッド = セル 1 つ。
// 空いている層の頁(持ち主が無い)は何もしない(頁を返す段が層の頁も返す。T-1240)。
// セルは読むだけ。CPU リファレンスは sim::StoreDenseCellImage → nest_detail::StorePageCell(試験 gpu_multires_layers)。
#include "sim/multires_layer_access.hlsli"

[numthreads(MR_LAYER_GROUP_THREADS, 1, 1)] void ReconcileLayers(uint3 groupId : SV_GroupID,
                                                                uint3 threadId : SV_GroupThreadID) {
    const uint32_t capacity = LayerCapacity();
    const uint32_t pageCount = LayerPageCount();
    const uint32_t layerPage = groupId.x / MR_LAYER_GROUPS_PER_PAGE;
    if (layerPage >= capacity)
        return;

    const uint32_t page = g_layers[MrLayerOwnerAddress(pageCount, layerPage)];
    if (page == MR_LAYER_NO_PAGE)
        return;

    const uint32_t index = ((groupId.x % MR_LAYER_GROUPS_PER_PAGE) * MR_LAYER_GROUP_THREADS) + threadId.x;
    const RxCell cell = g_cells[PageCellAddress(page, index)];

    // --- 組ごとに付け替える(変わった組だけ書く)---
    FX_UNROLL
    for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) {
        const uint32_t address = MrLayerEntryAddress(pageCount, capacity, layerPage, index, k);
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
