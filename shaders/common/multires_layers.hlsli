// multires_layers.hlsli — GPU の世界(sim/gpu_multires)の、セルの成分ごとの層の量の置き場の並び(T-1221・T-1240。ADR-0481 の 5・ADR-0489・20 §10.5)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。C++ は写す像を作る・読み戻した像を CPU の頁の層へ戻す(sim/gpu_multires_layers.cpp)、
// GPU は層を合わせる段(shaders/sim/multires_layers.hlsl)と木の段(shaders/sim/multires_tree.hlsl。頁を返す時に層の頁も返す)が読む。
//
// 置き場(uint32 の並び。セルの頁の数 pageCount = 観察の枠の頁 + 世界の頁・層の頁の数 capacity。T-1240 で見出し 2 語から変えた):
//   [頁の索引: セルの頁ごとの層の頁の番号(無ければ MR_LAYER_NO_PAGE)× pageCount]
//   [層の頁の持ち主: 層の頁ごとのセルの頁(空いていれば MR_LAYER_NO_PAGE)× capacity]
//   [層の頁の空きのスタック × capacity][空きの数 1 語][足りなかった数 1 語(層の頁が尽きて断った要求・影の段。計器)]
//   [要求ごとの層の頁の割り当ての始まり × MR_MAX_REQUESTS(1 刻みの中だけ。TreeAllocate が書き、細かくする鎖の段 d は上から d 番目を使う)]
//   [層の頁 × MR_LAYER_PAGE_WORDS(MR_BLOCK_CELLS × RX_MAX_CELL_SPECIES × 3 語: 物質・層の下位・上位)]
// 層の頁は層のある頁だけ持つ(疎)。1 セルの層は成分の並びの位置ではなく物質で結ぶ(CPU の付け替えと同じ。並びがずれても物質についていく)。
// 層を合わせる(CPU の StorePageCell の付け替えと同じ規則): 層 = min(前の層, 今のセルのその物質の量)。物質がセルに無ければ 0。
// 頁を返す段(TreeRelease・TreeFold)は層の頁も返す(頁を配ったら層は空。T-1220 の約束)。返した層の頁の中身は消さない:
// 層の頁を取る側が 512 セルの組を全部書く(T-1240 の 2 段目〔細かくする〕で守る約束)。
// 層の頁の番号は CPU に無い(CPU は頁ごとに持つ)ので、CPU とのビット一致は頁の番号で比べる(番号の取り方は結果に効かない)。
#ifndef BICAMERAL_MULTIRES_LAYERS_HLSLI
#define BICAMERAL_MULTIRES_LAYERS_HLSLI

#include "multires.hlsli"
#include "multires_tree.hlsli"

MR_NAMESPACE_BEGIN

FX_CONST uint32_t MR_LAYER_NO_PAGE = 0xFFFFFFFFu;  // 頁の索引・持ち主: 無い
FX_CONST uint32_t MR_LAYER_ENTRY_WORDS = 3;        // 物質・層の下位・上位
FX_CONST uint32_t MR_LAYER_PAGE_WORDS = MR_BLOCK_CELLS * RX_MAX_CELL_SPECIES * MR_LAYER_ENTRY_WORDS;
FX_CONST uint32_t MR_LAYER_GROUP_THREADS = 64;  // 層を合わせる段: 1 スレッド = セル 1 つ
FX_CONST uint32_t MR_LAYER_GROUPS_PER_PAGE = MR_BLOCK_CELLS / MR_LAYER_GROUP_THREADS;

// セルの頁 page の層の頁の番号の語
FX_FN uint32_t MrLayerIndexAddress(uint32_t page) {
    return page;
}

// 層の頁 layerPage の持ち主(セルの頁)の語
FX_FN uint32_t MrLayerOwnerAddress(uint32_t pageCount, uint32_t layerPage) {
    return pageCount + layerPage;
}

// 層の頁の空きのスタックの position 番目の語(上 = 空きの数 − 1 から取る)
FX_FN uint32_t MrLayerFreeAddress(uint32_t pageCount, uint32_t capacity, uint32_t position) {
    return pageCount + capacity + position;
}

// 層の頁の空きの数の語
FX_FN uint32_t MrLayerFreeCountAddress(uint32_t pageCount, uint32_t capacity) {
    return pageCount + (2 * capacity);
}

// 層の頁が尽きて断った数の語(GPU だけ。CPU は上限が無い)
FX_FN uint32_t MrLayerShortageAddress(uint32_t pageCount, uint32_t capacity) {
    return pageCount + (2 * capacity) + 1;
}

// 要求 request の層の頁の割り当ての始まり(空きのスタックの位置。PoppedPage と同じ数え方)の語
FX_FN uint32_t MrLayerRequestAddress(uint32_t pageCount, uint32_t capacity, uint32_t request) {
    return pageCount + (2 * capacity) + 2 + request;
}

// 層の頁の始まりの語
FX_FN uint32_t MrLayerPagesBase(uint32_t pageCount, uint32_t capacity) {
    return pageCount + (2 * capacity) + 2 + MR_MAX_REQUESTS;
}

// 置き場全体の語の数
FX_FN uint32_t MrLayerStoreWords(uint32_t pageCount, uint32_t capacity) {
    return MrLayerPagesBase(pageCount, capacity) + (capacity * MR_LAYER_PAGE_WORDS);
}

// 層の頁 layerPage のセル index の k 番目の組の先頭の語
FX_FN uint32_t MrLayerEntryAddress(uint32_t pageCount, uint32_t capacity, uint32_t layerPage, uint32_t index,
                                   uint32_t k) {
    const uint32_t entry = (index * RX_MAX_CELL_SPECIES) + k;

    return MrLayerPagesBase(pageCount, capacity) + (layerPage * MR_LAYER_PAGE_WORDS) + (entry * MR_LAYER_ENTRY_WORDS);
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
