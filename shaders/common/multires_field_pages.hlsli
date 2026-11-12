// multires_field_pages.hlsli — GPU の世界の登録した欄(セル・面の欄の疎な頁。CPU は sim/multires_nest.h の MultiresFieldStore)の
// 置き場の並びと頁の取り方、集めて書く段(ADR-0500 の 3・20 §10.7)の作業の並び(T-1252 段 2。ADR-0403・ADR-0532)。
// HLSL と C++ の両方でコンパイルする(fixed.hlsli の約束)。C++ は写す像を作る・読み戻した像を CPU の置き場へ戻す
// (sim/gpu_multires_fields.cpp)、GPU は集めて書く段(shaders/sim/multires_field_gather.hlsl)が読み書きする。
//
// 置き場 1 つ = 登録した欄 1 つ(uint32 の並び。セルの頁の数 cellPages・欄の頁の容量 capacity・値の数 components):
//   [頭 MF_HEADER_WORDS 語(下の MF_HEADER_*)]
//   [pageOf: セルの頁 → 欄の頁(MF_NO_PAGE = 無い = 全部 0 と読む)× cellPages]
//   [空きのスタック × capacity(CPU の freePages と同じ並び。上 = 空きの数 − 1 から取る)]
//   [欄の頁 × MR_BLOCK_CELLS × components × 2 語(int64 の下位・上位。頁の中は CPU の InPage と同じ = セル × 値の数 + 値)]
// 一様の値(枠 × 値の数)は GPU の世界の区画(下)で置き場の後ろに持つ(集めて書く段の試験の置き場 1 つは持たない)。
//
// GPU の世界の欄の区画(T-1252 段 2 の 3 本目。木の段の u4 の層の置き場の後ろ = 始まりの語は g_external2・欄の数は g_external3。
// 層を持たない世界は区画が u4 の始まり。sim/gpu_multires.cpp の BindLayers):
//   [欄の数 N][表 × N × MF_TABLE_WORDS(登録の番号・配り手の番号と印・置き場の始まり・一様の値の始まり。始まりは区画の始まりからの語)]
//   [欄ごと: 置き場(上の並び。MfStoreWords)][一様の値 × 枠 × 値の数 × 2 語(CPU の MultiresFieldStore::uniform と同じ並び)]
// GPU に載せる欄は、セル・面の疎な欄のうち配り手の欄(SfOwned)とこの刻みだけの欄(SfTickOnly)だけ(一般の欄は GPU の世界はまだ持たない)。
//
// 頁の番号の取り方(CPU と GPU で同じ。ADR-0500 の 3 (a)・ADR-0532): 1 つの段の中で頁の要る所を決まった順
// (集めて書く段は枠の順)に並べ、k 番目は空きのスタックの上から k 番目(k < 空きの数)、それより後は使った高さ + (k − 空きの数)。
// CPU の EnsurePage(空きの後ろから取る・無ければ頁の並びの後ろに足す)をその順に呼んだのと同じ番号になる。
// 取れる数 = min(容量, 予算〔0 = 決めない〕)− 使っている数(使っている数 = 高さ − 空きの数 = CPU の UsedPages)。
// CPU は容量の上限を持たないので、容量が尽きる場面は GPU だけ(試験は尽きない容量で比べ、尽きる場面は予算で確かめる)。
//
// 集めて書く段の作業(uint32 の並び。世界の枠の数 worldBlocks・作業のブロックの数 workBlocks):
//   [blockOf: 世界の枠 → 作業のブロック(MF_NO_WORK = 触っていない)× worldBlocks]
//   [枠ごとの段の結果(MF_GATHER_*)× worldBlocks]
//   [作業の値 × workBlocks × MR_BLOCK_CELLS × components × 2 語(CPU の NestGatherWork::values と同じ並び)]
#ifndef BICAMERAL_MULTIRES_FIELD_PAGES_HLSLI
#define BICAMERAL_MULTIRES_FIELD_PAGES_HLSLI

#include "multires.hlsli"

MR_NAMESPACE_BEGIN

FX_CONST uint32_t MF_NO_PAGE = 0xFFFFFFFFu;  // pageOf: 欄の頁が無い(CPU の MULTIRES_NO_FIELD_PAGE と同じ値)
FX_CONST uint32_t MF_NO_WORK = 0xFFFFFFFFu;  // 作業の blockOf: 触っていない

// --- 置き場の頭の語 ---
FX_CONST uint32_t MF_HEADER_CELL_PAGES = 0;  // セルの頁の数(写す時に CPU が書く)
FX_CONST uint32_t MF_HEADER_CAPACITY = 1;    // 欄の頁の容量(同上)
FX_CONST uint32_t MF_HEADER_COMPONENTS = 2;  // 値の数(同上)
FX_CONST uint32_t MF_HEADER_BUDGET = 3;      // 取ってよい頁の数(0 = 決めない。CPU の pageBudget。D-468)
FX_CONST uint32_t MF_HEADER_FREE_COUNT = 4;  // 空きの数(CPU の freePages.size())
FX_CONST uint32_t MF_HEADER_TOP = 5;         // 使った高さ(CPU の pages.size() ÷ 頁の値の数)
// 集めて書く段の報告(CPU の NestGatherReport。段ごとに書き直す)
FX_CONST uint32_t MF_HEADER_WRITTEN = 6;   // 書いた枠(blocks)
FX_CONST uint32_t MF_HEADER_TAKEN = 7;     // 新しく取った欄の頁(pagesTaken)
FX_CONST uint32_t MF_HEADER_SHORTAGE = 8;  // 欄の頁が尽きて書かなかった枠(pageShortage。計器の誤り。値は作業に残る)
FX_CONST uint32_t MF_HEADER_WANTED = 9;    // 触った一様な枠(頁に広げてから書く。木の段の TreeExpand が広げる)
FX_CONST uint32_t MF_HEADER_TREE_SHORTAGE =
    10;  // 木の段で欄の頁が尽きて断った要求の数(GPU だけの計器。欄 0 の頭に足す。CPU は上限が無い)
FX_CONST uint32_t MF_HEADER_EXPANDED = 11;  // GPU の世界の集めて書く段が頁に広げた一様な枠(expanded。RecordGather)
FX_CONST uint32_t MF_HEADER_WORDS = 12;

// --- 集めて書く段の枠ごとの結果 ---
FX_CONST uint32_t MF_GATHER_NONE = 0;     // 触っていない・作業が全部 0
FX_CONST uint32_t MF_GATHER_TOUCHED = 1;  // 印の段の結果: 頁を持つ枠に作業がある(頁を取る段が下の 3 つに分ける)
FX_CONST uint32_t MF_GATHER_ADD = 2;      // 欄の頁がある: 足す
FX_CONST uint32_t MF_GATHER_NEW = 3;      // 欄の頁を新しく取った: 0 にしてから足す
FX_CONST uint32_t MF_GATHER_SHORT = 4;    // 欄の頁が尽きた: 書かない(作業に残る)
FX_CONST uint32_t MF_GATHER_WANTED = 5;   // 一様な枠: 頁に広げてから書く(この段では書かない)

// --- 置き場の並び ---

FX_FN uint32_t MfPageOfAddress(uint32_t cellPage) {
    return MF_HEADER_WORDS + cellPage;
}

FX_FN uint32_t MfFreeAddress(uint32_t cellPages, uint32_t position) {
    return MF_HEADER_WORDS + cellPages + position;
}

// 欄の頁 fieldPage のセル index の値 component の下位の語(上位は + 1)
FX_FN uint32_t MfValueAddress(uint32_t cellPages, uint32_t capacity, uint32_t components, uint32_t fieldPage,
                              uint32_t index, uint32_t component) {
    const uint32_t value = (((fieldPage * MR_BLOCK_CELLS) + index) * components) + component;

    return MF_HEADER_WORDS + cellPages + capacity + (value * 2u);
}

FX_FN uint32_t MfStoreWords(uint32_t cellPages, uint32_t capacity, uint32_t components) {
    return MF_HEADER_WORDS + cellPages + capacity + (capacity * MR_BLOCK_CELLS * components * 2u);
}

// --- 頁の取り方(CPU の EnsurePage を同じ順に呼んだのと同じ番号)---

// 取れる頁の数(使っている数 = 高さ − 空きの数)
FX_FN uint32_t MfPagesAvailable(uint32_t capacity, uint32_t budget, uint32_t freeCount, uint32_t top) {
    const uint32_t used = top - freeCount;
    const uint32_t limit = budget == 0 ? capacity : (budget < capacity ? budget : capacity);

    return used < limit ? limit - used : 0u;
}

// 段の中で k 番目に頁の要る所が空きのスタックから取るか(偽なら新しい頁 = MfFreshPage)
FX_FN bool MfTakesFreePage(uint32_t k, uint32_t freeCount) {
    return k < freeCount;
}

// 空きのスタックから取る時の位置(上から k 番目)
FX_FN uint32_t MfFreePosition(uint32_t k, uint32_t freeCount) {
    return freeCount - 1u - k;
}

// 新しい頁の番号(k ≥ 空きの数)
FX_FN uint32_t MfFreshPage(uint32_t k, uint32_t freeCount, uint32_t top) {
    return top + (k - freeCount);
}

// --- GPU の世界の欄の区画(始まりからの語)---
FX_CONST uint32_t MF_TABLE_FIELD = 0;    // CPU の登録の番号(MultiresNest::fields の添字)
FX_CONST uint32_t MF_TABLE_OWNER = 1;    // 配り手の番号(SF_OWNER_*。配り手でなければ SF_NO_OWNER)
FX_CONST uint32_t MF_TABLE_STORE = 2;    // 置き場の始まり
FX_CONST uint32_t MF_TABLE_UNIFORM = 3;  // 一様の値の始まり
FX_CONST uint32_t
    MF_TABLE_LEDGER = 4;  // 保存量の欄の世界の帳簿の列の始まり(CPU の NestFieldLedgerBase + NestConservedIndex)
FX_CONST uint32_t MF_TABLE_WORDS = 5;
FX_CONST uint32_t MF_NO_LEDGER_COLUMN = 0xFFFFFFFFu;  // 表の帳簿の列: 保存量の欄でない

FX_FN uint32_t MfRegionTableAddress(uint32_t entry, uint32_t word) {
    return 1u + (entry * MF_TABLE_WORDS) + word;
}

// 一様の値(枠 slot の値 component)の下位の語(上位は + 1)
FX_FN uint32_t MfUniformAddress(uint32_t uniformStart, uint32_t components, uint32_t slot, uint32_t component) {
    return uniformStart + (((slot * components) + component) * 2u);
}

// 欄 1 つの置き場と一様の値の語の数
FX_FN uint32_t MfFieldEntryWords(uint32_t cellPages, uint32_t capacity, uint32_t components, uint32_t slots) {
    return MfStoreWords(cellPages, capacity, components) + (slots * components * 2u);
}

// 区画の語の数の上限(値の数の和 values までの欄。欄 1 つは値を 1 つ以上持つので欄の数 ≤ values)
FX_FN uint32_t MfRegionWordsBound(uint32_t cellPages, uint32_t capacity, uint32_t slots, uint32_t values) {
    const uint32_t perField = MF_TABLE_WORDS + MF_HEADER_WORDS + cellPages + capacity;

    return 1u + (values * perField) + (capacity * MR_BLOCK_CELLS * values * 2u) + (slots * values * 2u);
}

// --- 集めて書く段の作業の並び ---

FX_FN uint32_t MfWorkBlockOfAddress(uint32_t slot) {
    return slot;
}

FX_FN uint32_t MfWorkStateAddress(uint32_t worldBlocks, uint32_t slot) {
    return worldBlocks + slot;
}

FX_FN uint32_t MfWorkValueAddress(uint32_t worldBlocks, uint32_t components, uint32_t block, uint32_t index,
                                  uint32_t component) {
    const uint32_t value = (((block * MR_BLOCK_CELLS) + index) * components) + component;

    return (2u * worldBlocks) + (value * 2u);
}

FX_FN uint32_t MfWorkWords(uint32_t worldBlocks, uint32_t workBlocks, uint32_t components) {
    return (2u * worldBlocks) + (workBlocks * MR_BLOCK_CELLS * components * 2u);
}

MR_NAMESPACE_END

#endif  // BICAMERAL_MULTIRES_FIELD_PAGES_HLSLI
