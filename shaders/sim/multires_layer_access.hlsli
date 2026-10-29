// multires_layer_access.hlsli — 層の置き場(u4。並びは common/multires_layers.hlsli)を読み書きする段の道具(T-1240。20 §10.5)。
// 層を合わせる段(multires_layers.hlsl)・木の段(multires_tree.hlsl。頁を返す・畳む・層の頁を配る)・細かくする鎖(multires_graph.hlsl)が include する。
// 木の段の間は u4 に層の置き場を結び、g_external0 = 層の頁の数(0 = 層を持たない世界: u4 は代わりのバッファなので読まない)・
// g_external1 = セルの頁の数(gpu_multires.cpp の BindLayers)。木の段のシェーダーはほかに u4・g_external* を使わない(2026-10-29 確かめた)。
// 頁を返す段は、その頁の層の頁も返す(頁を配ったら層は空。返す層の頁は中身を 0 にしてから積む = 取る側は 0 を前提にできる)。
#ifndef BICAMERAL_MULTIRES_LAYER_ACCESS_HLSLI
#define BICAMERAL_MULTIRES_LAYER_ACCESS_HLSLI

#include "common/multires_layers.hlsli"
#include "sim/multires_bindings.hlsli"

globallycoherent RWStructuredBuffer<uint32_t> g_layers : register(u4);

uint32_t LayerCapacity() {
    return g_external0;
}

uint32_t LayerPageCount() {
    return g_external1;
}

// セルの頁 page の層の頁(無ければ・層を持たない世界なら MR_LAYER_NO_PAGE)
uint32_t LayerPageOf(uint32_t page) {
    if (LayerCapacity() == 0 || page >= LayerPageCount())
        return MR_LAYER_NO_PAGE;

    return g_layers[MrLayerIndexAddress(page)];
}

// 層の頁の組 1 つ(物質・層の下位・上位)を写す
void CopyLayerEntry(uint32_t from, uint32_t to) {
    g_layers[to] = g_layers[from];
    g_layers[to + 1] = g_layers[from + 1];
    g_layers[to + 2] = g_layers[from + 2];
}

// 層の頁 layerPage のセル index の組を全部 0 に
void ClearLayerCell(uint32_t layerPage, uint32_t index) {
    const uint32_t base = MrLayerEntryAddress(LayerPageCount(), LayerCapacity(), layerPage, index, 0);
    for (uint32_t word = 0; word < RX_MAX_CELL_SPECIES * MR_LAYER_ENTRY_WORDS; ++word)
        g_layers[base + word] = 0;
}

// 層の頁が尽きて断った数を足す(計器。GPU だけ)
void CountLayerShortage(uint32_t count) {
    InterlockedAdd(g_layers[MrLayerShortageAddress(LayerPageCount(), LayerCapacity())], count);
}

groupshared uint32_t gs_layerScan[MR_MAX_REQUESTS];
groupshared uint32_t gs_layerFreeTop;

// 1 グループ(MR_MAX_REQUESTS スレッド。全部のスレッドが呼ぶ)で、スレッド i の頁 page(返さないなら MR_NO_PAGE)の層の頁を
// スレッドの番号の順に空きのスタックへ積む(CPU は頁を返す順に ClearPageLayers。層の頁の番号は CPU に無いので順は結果に効かないが、
// 決まった順にする)。頁の索引・持ち主を外し、返した層の頁の中身を 0 にする
void ReleaseLayerPages(uint32_t i, uint32_t page) {
    const uint32_t layerPage = page != MR_NO_PAGE ? LayerPageOf(page) : MR_LAYER_NO_PAGE;
    const bool give = layerPage != MR_LAYER_NO_PAGE;
    const uint32_t pageCount = LayerPageCount();
    const uint32_t capacity = LayerCapacity();

    // --- 返す数の累積和(含む)と、今の空きの数 ---
    gs_layerScan[i] = give ? 1u : 0u;
    if (i == 0)
        gs_layerFreeTop = capacity > 0 ? g_layers[MrLayerFreeCountAddress(pageCount, capacity)] : 0u;

    GroupMemoryBarrierWithGroupSync();
    for (uint32_t offset = 1; offset < MR_MAX_REQUESTS; offset <<= 1) {
        uint32_t sum = gs_layerScan[i];
        if (i >= offset)
            sum += gs_layerScan[i - offset];

        GroupMemoryBarrierWithGroupSync();
        gs_layerScan[i] = sum;
        GroupMemoryBarrierWithGroupSync();
    }

    const uint32_t top = gs_layerFreeTop;
    const uint32_t total = gs_layerScan[MR_MAX_REQUESTS - 1];
    if (give) {
        g_layers[MrLayerFreeAddress(pageCount, capacity, top + gs_layerScan[i] - 1)] = layerPage;
        g_layers[MrLayerIndexAddress(page)] = MR_LAYER_NO_PAGE;
        g_layers[MrLayerOwnerAddress(pageCount, layerPage)] = MR_LAYER_NO_PAGE;
    }

    DeviceMemoryBarrierWithGroupSync();

    // --- 返した層の頁の中身を 0 に(グループで分けて書く。total はグループで一様)---
    for (uint32_t n = 0; n < total; ++n) {
        const uint32_t returned = g_layers[MrLayerFreeAddress(pageCount, capacity, top + n)];
        const uint32_t base = MrLayerEntryAddress(pageCount, capacity, returned, 0, 0);
        for (uint32_t word = i; word < MR_LAYER_PAGE_WORDS; word += MR_MAX_REQUESTS)
            g_layers[base + word] = 0;
    }

    if (i == 0 && total > 0)
        g_layers[MrLayerFreeCountAddress(pageCount, capacity)] = top + total;

    DeviceMemoryBarrierWithGroupSync();  // 次に呼ぶ時に累積和と空きの数を書き直す前に、全部が読み終える
}

#endif  // BICAMERAL_MULTIRES_LAYER_ACCESS_HLSLI
