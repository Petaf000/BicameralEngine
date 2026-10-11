// multires_wide_access.hlsli — GPU の多重解像度の世界のセルの溢れ(頁の見出し・塊の表)を引く道具(T-0236 から分けた。T-0237)。
// 刻む段(multires_wide_step.hlsli)と、要求の処理(multires_tree.hlsl の TreeWideReserve・multires_graph.hlsl の溢れを使う変種)が使う。
// 並びは shaders/common/multires_wide.hlsli。溢れを使う変種(MR_WIDE_CELLS。ルート定数 g_overflowBase を宣言する)だけが include する。
#ifndef BICAMERAL_MULTIRES_WIDE_ACCESS_HLSLI
#define BICAMERAL_MULTIRES_WIDE_ACCESS_HLSLI

#include "common/multires_wide.hlsli"

// --- 頁の溢れ(u6 の後ろ。g_overflowBase。並びは shaders/common/multires_wide.hlsli)---

// 頁の数(観察の枠の頁 + 世界の頁。OverflowBaseWord = 世界の枠 × 2 + 索引 + 世界の頁)
uint32_t WidePageCount() {
    return (g_overflowBase - (2 * g_worldBlocks) - g_indexEntries) + (g_blockCount - g_worldBlocks);
}

uint32_t WidePoolWord() {
    return MrWidePoolWord(g_overflowBase, WidePageCount());
}

// 端数の枠 fractionSlot の溢れの領域の番号(T-0311。頁の後ろ)。下の道具の page は領域の番号(頁か端数の枠)
uint32_t WideFractionArea(uint32_t fractionSlot) {
    return WidePageCount() + fractionSlot;
}

// 領域 area の見出しの先頭(頁は u6 の溢れの領域の先頭から、端数の枠は空きのスタックの後ろから)
uint32_t WideAreaHeader(uint32_t area) {
    const uint32_t pages = WidePageCount();
    if (area < pages)
        return MrWideHeaderWord(g_overflowBase, area);

    const uint32_t pool = WidePoolWord();

    return MrWideFractionHeaderWord(pool, g_treeWords[pool + MR_WIDE_POOL_CAPACITY], area - pages);
}

uint32_t WideHeaderAt(uint32_t page, uint32_t field) {
    return WideAreaHeader(page) + field;
}

uint32_t WideCurrentSide(uint32_t page) {
    return g_treeWords[WideHeaderAt(page, MR_WIDE_HEADER_SIDE)];
}

// 面 side が持つ塊の数
uint32_t WideChunkCount(uint32_t page, uint32_t side) {
    return g_treeWords[MrWideTableAt(WideAreaHeader(page), side)];
}

// 面 side の論理の語 word の u6 の語(塊の表を引く。面が持つ塊の中であること)
uint32_t WideWord(uint32_t page, uint32_t side, uint32_t word) {
    const uint32_t table = MrWideTableAt(WideAreaHeader(page), side);
    const uint32_t chunk = g_treeWords[table + 1 + (word / MR_WIDE_CHUNK_WORDS)];

    return MrWideChunkWord(WidePoolWord(), chunk) + (word % MR_WIDE_CHUNK_WORDS);
}

// 面 side のセル index の溢れの [始まり, 終わり)(塊を持たない面は溢れが無い)
uint2 WideRange(uint32_t page, uint32_t side, uint32_t index) {
    if (WideChunkCount(page, side) == 0)
        return uint2(0, 0);

    return uint2(g_treeWords[WideWord(page, side, index)], g_treeWords[WideWord(page, side, index + 1)]);
}

// 頁の今の面を面 0 にして空にする(threads スレッドで分ける。同期は呼ぶ側。一様の値は溢れを持たない)。
// 前に使った頁の塊はそのまま持つ(次に書く時に要らなければ WideAllocate が返す)ので、塊を持つなら始まりの表を 0 にする
void ClearWideSideWords(uint32_t page, uint32_t thread, uint32_t threads) {
    if (WideChunkCount(page, 0) != 0) {
        for (uint32_t i = thread; i <= MR_BLOCK_CELLS; i += threads)
            g_treeWords[WideWord(page, 0, i)] = 0;
    }

    if (thread == 0)
        g_treeWords[WideHeaderAt(page, MR_WIDE_HEADER_SIDE)] = 0;
}

#endif  // BICAMERAL_MULTIRES_WIDE_ACCESS_HLSLI
