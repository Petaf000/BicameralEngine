// multires_layers.hlsli — GPU の世界(sim/gpu_multires)の、セルの成分ごとの層の量の置き場の並び(T-1221・T-1240。ADR-0481 の 5・ADR-0489・20 §10.5)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。C++ は写す像を作る・読み戻した像を CPU の頁の層へ戻す(sim/gpu_multires_layers.cpp)、
// GPU は層を合わせる段(shaders/sim/multires_layers.hlsl)と木の段(shaders/sim/multires_tree.hlsl。頁を返す時に層の頁も返す)が読む。
//
// 置き場(uint32 の並び。セルの頁の数 pageCount = 観察の枠の頁 + 世界の頁・層の頁の数 capacity。T-1240 で見出し 2 語から変えた):
//   [頁の索引: セルの頁ごとの層の頁の番号(無ければ MR_LAYER_NO_PAGE)× pageCount]
//   [層の頁の持ち主: 層の頁ごとのセルの頁(空いていれば MR_LAYER_NO_PAGE)× capacity]
//   [層の頁の空きのスタック × capacity][空きの数 1 語][足りなかった数 1 語(層の頁が尽きて断った要求・影の段。計器)]
//   [世界の印 1 語(MR_LAYER_FLAG_*。写す時に CPU が書く。T-1240 の 4 段目)]
//   [要求ごとの層の頁の割り当ての始まり × MR_MAX_REQUESTS(1 刻みの中だけ。TreeAllocate が書き、細かくする鎖の段 d は上から d 番目を使う)]
//   [層の頁 × MR_LAYER_PAGE_WORDS(MR_BLOCK_CELLS × RX_MAX_CELL_SPECIES × 3 語: 物質・層の下位・上位)]
//   [一様の枠の層 × 枠の数(slotCount = 世界の枠 + 観察の枠)× MR_LAYER_CELL_WORDS(層の頁の 1 セルと同じ組 8 つ。T-1240 の 3・4 段目)]
// 一様の枠の層は枠ごとに決まった場所(取る・返すが無い = 尽きない。CPU の MultiresUniformLayerStore は層のある枠だけ組を持つが、
// GPU は枠 1 つ 96 B を層の頁を持つ世界〔capacity > 0〕だけ持つ。仮・T-1240)。組の層が全部 0 = 層の無い枠(CPU の Find が nullptr)。
// 一様の枠の層も物質で結び、読む時に今の一様の値と結んで 層 = min(層, 量)(CPU の LoadUniformLayers と同じ)。
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

FX_CONST uint32_t MR_LAYER_NO_PAGE = 0xFFFFFFFFu;                                    // 頁の索引・持ち主: 無い
FX_CONST uint32_t MR_LAYER_ENTRY_WORDS = 3;                                          // 物質・層の下位・上位
FX_CONST uint32_t MR_LAYER_CELL_WORDS = RX_MAX_CELL_SPECIES * MR_LAYER_ENTRY_WORDS;  // 1 セル(一様の枠 1 つ)の組
FX_CONST uint32_t MR_LAYER_PAGE_WORDS = MR_BLOCK_CELLS * MR_LAYER_CELL_WORDS;
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

// 世界の印の語(MR_LAYER_FLAG_GRAIN = 層を使う世界〔EnableCellLayers〕: 粒の物質の層でない分は舞う粒。CPU の cellLayers.enabled)
FX_CONST uint32_t MR_LAYER_FLAG_GRAIN = 1;

FX_FN uint32_t MrLayerFlagsAddress(uint32_t pageCount, uint32_t capacity) {
    return pageCount + (2 * capacity) + 2;
}

// 要求 request の層の頁の割り当ての始まり(空きのスタックの位置。PoppedPage と同じ数え方)の語
FX_FN uint32_t MrLayerRequestAddress(uint32_t pageCount, uint32_t capacity, uint32_t request) {
    return pageCount + (2 * capacity) + 3 + request;
}

// 層の頁の始まりの語
FX_FN uint32_t MrLayerPagesBase(uint32_t pageCount, uint32_t capacity) {
    return pageCount + (2 * capacity) + 3 + MR_MAX_REQUESTS;
}

// 一様の枠の層の始まりの語
FX_FN uint32_t MrLayerUniformBase(uint32_t pageCount, uint32_t capacity) {
    return MrLayerPagesBase(pageCount, capacity) + (capacity * MR_LAYER_PAGE_WORDS);
}

// 置き場全体の語の数(slotCount = 枠の数。層を持たない世界〔capacity = 0〕は一様の枠の層も持たない)
FX_FN uint32_t MrLayerStoreWords(uint32_t pageCount, uint32_t capacity, uint32_t slotCount) {
    const uint32_t uniformWords = capacity > 0 ? slotCount * MR_LAYER_CELL_WORDS : 0u;

    return MrLayerUniformBase(pageCount, capacity) + uniformWords;
}

// 一様な枠 slot の k 番目の組の先頭の語
FX_FN uint32_t MrLayerUniformAddress(uint32_t pageCount, uint32_t capacity, uint32_t slot, uint32_t k) {
    return MrLayerUniformBase(pageCount, capacity) + (slot * MR_LAYER_CELL_WORDS) + (k * MR_LAYER_ENTRY_WORDS);
}

// 層の頁 layerPage のセル index の k 番目の組の先頭の語
FX_FN uint32_t MrLayerEntryAddress(uint32_t pageCount, uint32_t capacity, uint32_t layerPage, uint32_t index,
                                   uint32_t k) {
    return MrLayerPagesBase(pageCount, capacity) + (layerPage * MR_LAYER_PAGE_WORDS) + (index * MR_LAYER_CELL_WORDS) +
           (k * MR_LAYER_ENTRY_WORDS);
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
