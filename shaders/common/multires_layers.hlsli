// multires_layers.hlsli — GPU の世界(sim/gpu_multires)の、セルの成分ごとの層の量の置き場の並び(T-1221。ADR-0481 の 5・ADR-0489・20 §10.5)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。C++ は写す像を作る・読み戻した像を CPU の頁の層へ戻す(sim/gpu_multires_layers.cpp)、
// GPU は層を合わせる段(shaders/sim/multires_layers.hlsl)が読む。
//
// 置き場(uint32 の並び。層の頁の数 capacity):
//   [見出し: 層の頁ごとに 2 語(セルの頁・持ち主の枠)][層の頁 × MR_BLOCK_CELLS × RX_MAX_CELL_SPECIES × 3 語(物質・層の下位・上位)]
// 層の頁は層のある頁だけ持つ(疎)。1 セルの層は成分の並びの位置ではなく物質で結ぶ(CPU の付け替えと同じ。並びがずれても物質についていく)。
// 層を合わせる(CPU の StorePageCell の付け替えと同じ規則): 層 = min(前の層, 今のセルのその物質の量)。物質がセルに無ければ 0。
// 持ち主の枠がもうその頁を持っていなければ(頁が返された・別の枠に配られた)、層の頁は全部 0(頁を配ったら層は空。T-1220 の約束)。
#ifndef BICAMERAL_MULTIRES_LAYERS_HLSLI
#define BICAMERAL_MULTIRES_LAYERS_HLSLI

#include "multires.hlsli"

MR_NAMESPACE_BEGIN

FX_CONST uint32_t MR_LAYER_NO_PAGE = 0xFFFFFFFFu;  // 見出しのセルの頁: 使っていない層の頁
FX_CONST uint32_t MR_LAYER_HEADER_WORDS = 2;       // 層の頁ごと: セルの頁・持ち主の枠
FX_CONST uint32_t MR_LAYER_ENTRY_WORDS = 3;        // 物質・層の下位・上位
FX_CONST uint32_t MR_LAYER_PAGE_WORDS = MR_BLOCK_CELLS * RX_MAX_CELL_SPECIES * MR_LAYER_ENTRY_WORDS;
FX_CONST uint32_t MR_LAYER_GROUP_THREADS = 64;  // 層を合わせる段: 1 スレッド = セル 1 つ
FX_CONST uint32_t MR_LAYER_GROUPS_PER_PAGE = MR_BLOCK_CELLS / MR_LAYER_GROUP_THREADS;

// 置き場全体の語の数
FX_FN uint32_t MrLayerStoreWords(uint32_t capacity) {
    return capacity * (MR_LAYER_HEADER_WORDS + MR_LAYER_PAGE_WORDS);
}

// 層の頁 layerPage の見出しの先頭の語
FX_FN uint32_t MrLayerHeaderAddress(uint32_t layerPage) {
    return layerPage * MR_LAYER_HEADER_WORDS;
}

// 層の頁 layerPage のセル index の k 番目の組の先頭の語
FX_FN uint32_t MrLayerEntryAddress(uint32_t capacity, uint32_t layerPage, uint32_t index, uint32_t k) {
    const uint32_t entry = (index * RX_MAX_CELL_SPECIES) + k;

    return (capacity * MR_LAYER_HEADER_WORDS) + (layerPage * MR_LAYER_PAGE_WORDS) + (entry * MR_LAYER_ENTRY_WORDS);
}

// 64bit の a < b を 32bit の 2 つで比べる(シミュの HLSL で 64bit の比べを引き算と並べない約束。CLAUDE.md §4・T-1078)
FX_FN bool MrLayerLess(uint64_t a, uint64_t b) {
    const uint32_t aHigh = (uint32_t)(a >> 32);
    const uint32_t bHigh = (uint32_t)(b >> 32);
    if (aHigh != bHigh)
        return aHigh < bHigh;

    return (uint32_t)a < (uint32_t)b;
}

// 層を書かない道がセルを書いた後の層: 層 = min(前の層, 今のセルの物質 species の量)。物質がセルに無ければ 0
// (並びの位置は決まった数の回しで選ぶ。動的な添字で構造体の配列を読まない。T-0397)
FX_FN uint64_t MrLayerAfterWrite(RxCell cell, uint32_t species, uint64_t layer) {
    uint64_t amount = 0;
    FX_UNROLL
    for (uint32_t i = 0; i < RX_MAX_CELL_SPECIES; ++i) {
        if (i < cell.speciesCount && cell.species[i] == species)
            amount = cell.amounts[i];
    }

    return MrLayerLess(amount, layer) ? amount : layer;
}

MR_NAMESPACE_END

#endif  // BICAMERAL_MULTIRES_LAYERS_HLSLI
