// multires_layer_access.hlsli — 層の置き場(u4。並びは common/multires_layers.hlsli)を読み書きする段の道具(T-1240。20 §10.5)。
// 層を合わせる段(multires_layers.hlsl)・木の段(multires_tree.hlsl。頁を返す・畳む・細かくする要求に層の頁を割り当てる)・
// 細かくする鎖(multires_graph.hlsl。子の頁へ層を写す)が include する。
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

// 1 グループ(MR_MAX_REQUESTS スレッド。全部のスレッドが呼ぶ)で value の累積和(含む)を gs_layerScan に
void LayerInclusiveScan(uint32_t i, uint32_t value) {
    gs_layerScan[i] = value;
    GroupMemoryBarrierWithGroupSync();
    for (uint32_t offset = 1; offset < MR_MAX_REQUESTS; offset <<= 1) {
        uint32_t sum = gs_layerScan[i];
        if (i >= offset)
            sum += gs_layerScan[i - offset];

        GroupMemoryBarrierWithGroupSync();
        gs_layerScan[i] = sum;
        GroupMemoryBarrierWithGroupSync();
    }
}

// セルの頁 page に層の頁 layerPage を結ぶ(頁の索引と持ち主)
void AttachLayerPage(uint32_t page, uint32_t layerPage) {
    g_layers[MrLayerIndexAddress(page)] = layerPage;
    g_layers[MrLayerOwnerAddress(LayerPageCount(), layerPage)] = page;
}

// 層の頁のセル from の組を全部、層の頁のセル to へ写す(組は物質で結ぶので、同じ値のセルへはそのまま写せる)
void CopyLayerCell(uint32_t fromLayerPage, uint32_t fromIndex, uint32_t toLayerPage, uint32_t toIndex) {
    const uint32_t from = MrLayerEntryAddress(LayerPageCount(), LayerCapacity(), fromLayerPage, fromIndex, 0);
    const uint32_t to = MrLayerEntryAddress(LayerPageCount(), LayerCapacity(), toLayerPage, toIndex, 0);
    for (uint32_t word = 0; word < RX_MAX_CELL_SPECIES * MR_LAYER_ENTRY_WORDS; ++word)
        g_layers[to + word] = g_layers[from + word];
}

// --- 物質で結んで読む・一様の枠の層(T-1240 の 3・4 段目)---

// 組の並びの始まり base(層の頁の 1 セルか一様の枠 1 つ)の、物質 species の層(0 でない組。無ければ 0)を、セル cell と結んで
// min(層, 量)(物質がセルに無ければ 0。CPU の RebindLayers・LoadWidePageCellLayers と同じ)
uint64_t BoundLayerOf(uint32_t base, uint32_t species, RxCell cell) {
    uint64_t raw = 0;
    FX_UNROLL
    for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) {
        const uint32_t address = base + (k * MR_LAYER_ENTRY_WORDS);
        const bool nonzero = (g_layers[address + 1] | g_layers[address + 2]) != 0;
        if (g_layers[address] == species && nonzero)
            raw = FX_U64(g_layers[address + 2], g_layers[address + 1]);
    }

    return MrLayerAfterWrite(cell, species, raw);
}

// 組の並び from を cell と結んで(物質ごとに min(層, 量))to へ書く(from と to は同じでもよい)
void WriteBoundLayers(uint32_t from, RxCell cell, uint32_t to) {
    FX_UNROLL
    for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) {
        const uint32_t source = from + (k * MR_LAYER_ENTRY_WORDS);
        const uint32_t species = g_layers[source];
        const uint64_t bound = MrLayerAfterWrite(cell, species, FX_U64(g_layers[source + 2], g_layers[source + 1]));
        const uint32_t target = to + (k * MR_LAYER_ENTRY_WORDS);
        g_layers[target] = species;
        g_layers[target + 1] = (uint32_t)bound;
        g_layers[target + 2] = (uint32_t)(bound >> 32);
    }
}

// 組の並び base を 0 に(1 スレッドで)
void ClearLayerWords(uint32_t base) {
    for (uint32_t word = 0; word < MR_LAYER_CELL_WORDS; ++word)
        g_layers[base + word] = 0;
}

// 一様な枠 slot の層の組の始まり
uint32_t UniformLayerBase(uint32_t slot) {
    return MrLayerUniformAddress(LayerPageCount(), LayerCapacity(), slot, 0);
}

// 一様な枠 slot が 0 でない層を持つか(CPU の uniformLayers.Find != nullptr。層を持たない世界は false)
bool UniformHasLayers(uint32_t slot) {
    if (LayerCapacity() == 0)
        return false;

    const uint32_t base = UniformLayerBase(slot);
    bool any = false;
    FX_UNROLL
    for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) {
        const uint32_t address = base + (k * MR_LAYER_ENTRY_WORDS);
        any = any || (g_layers[address + 1] | g_layers[address + 2]) != 0;
    }

    return any;
}

// 1 グループ(MR_MAX_REQUESTS スレッド。全部のスレッドが呼ぶ)で、スレッド i の頁 page(返さないなら MR_NO_PAGE)の層の頁を
// スレッドの番号の順に空きのスタックへ積む(CPU は頁を返す順に ClearPageLayers。層の頁の番号は CPU に無いので順は結果に効かないが、
// 決まった順にする)。頁の索引・持ち主を外し、返した層の頁の中身を 0 にする
void ReleaseLayerPages(uint32_t i, uint32_t page) {
    const uint32_t layerPage = page != MR_NO_PAGE ? LayerPageOf(page) : MR_LAYER_NO_PAGE;
    const bool give = layerPage != MR_LAYER_NO_PAGE;
    const uint32_t pageCount = LayerPageCount();
    const uint32_t capacity = LayerCapacity();

    // --- 返す数の累積和(含む)と、今の空きの数 ---
    if (i == 0)
        gs_layerFreeTop = capacity > 0 ? g_layers[MrLayerFreeCountAddress(pageCount, capacity)] : 0u;

    LayerInclusiveScan(i, give ? 1u : 0u);

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
