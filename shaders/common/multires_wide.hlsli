// multires_wide.hlsli — GPU の多重解像度の世界のセルの溢れ(成分の二段の GPU 版。T-0176・T-0236・02 §3・17 R-MULTI-4)の大きさと並び。
// C++(engine/src/sim/gpu_multires.cpp が VRAM を取り、写し・読み戻す)と HLSL(shaders/sim/multires_wide_step.hlsli が刻む)で共有する。
//
// セルは「インライン RX_MAX_CELL_SPECIES 個(u1 の頁。今と同じ RxCell)+ 頁ごとの溢れ」。溢れは u6(木の管理の uint32 の表)の後ろに置く:
//   [頁ごとの見出し MR_WIDE_HEADER_WORDS 語 × 頁の数][塊の置き場: 塊の数・空きの数・塊 × MR_WIDE_CHUNK_WORDS・空きのスタック]
// 頁の溢れは 2 面。面ごとの中身(論理の語)は [セルの溢れの始まり × (MR_BLOCK_CELLS + 1)][成分 × 溢れの数(物質 ID・µmol の下位・上位の 3 語)]
// で、溢れはセルの番号の順に詰める(CPU の MultiresOverflowArea と同じ並び。グループ内のプレフィックス和)。論理の語は MR_WIDE_CHUNK_WORDS 語の
// 塊に分けて、見出しの塊の表(面ごと)が置き場の塊の番号を持つ(T-0236: 使う分だけの VRAM。溢れの無い面は塊を持たず、空気だけの頁は見出しだけ)。
// 刻むブロックは今の面を読み、もう片方の面へ書いてから面を入れ替える(読む前に別のセルの溢れを上書きしないため)。
// 書く面の塊が足りなければ、印(要求)を付けて刻みの後の段(WideAllocate)が枠の順に塊を配り、WideRetry が同じ刻みのうちに
// 溢れるセルだけ刻み直す(T-0236。置き場が尽きた時だけ待たせて MR_COUNTER_LIMIT_PRODUCTS に数える)。
// 1 セルが GPU で持てる成分は RX_GPU_WIDE_SPECIES まで(超える生成物は待たせる。T-0248)。
#ifndef BICAMERAL_MULTIRES_WIDE_HLSLI
#define BICAMERAL_MULTIRES_WIDE_HLSLI

#include "multires.hlsli"

MR_NAMESPACE_BEGIN

FX_CONST uint32_t RX_GPU_WIDE_SPECIES = 24;  // GPU の刻みが 1 セルで持てる成分の数(インライン 8 + 溢れ 16)
FX_CONST uint32_t MR_WIDE_ENTRY_WORDS = 3;   // 物質 ID・µmol の下位・上位
// 面の溢れの成分の最大(全部のセルが RX_GPU_WIDE_SPECIES 種)と、面の論理の語の最大
FX_CONST uint32_t MR_WIDE_SIDE_ENTRIES = MR_BLOCK_CELLS * (RX_GPU_WIDE_SPECIES - RX_MAX_CELL_SPECIES);
FX_CONST uint32_t MR_WIDE_SIDE_WORDS = (MR_BLOCK_CELLS + 1) + (MR_WIDE_ENTRY_WORDS * MR_WIDE_SIDE_ENTRIES);

// --- 塊(T-0236)---
FX_CONST uint32_t MR_WIDE_CHUNK_WORDS = 1024;  // 4 KiB。最初の塊に始まりの表(513 語)と 170 成分
FX_CONST uint32_t MR_WIDE_SIDE_CHUNKS = (MR_WIDE_SIDE_WORDS + MR_WIDE_CHUNK_WORDS - 1) / MR_WIDE_CHUNK_WORDS;

// --- 頁の見出し: [今の面][要求][要求した刻みの busyTick の下位・上位][刻み直すセルの印 × 16][面 0 の塊の数・表][面 1 の塊の数・表] ---
FX_CONST uint32_t MR_WIDE_HEADER_SIDE = 0;
FX_CONST uint32_t MR_WIDE_HEADER_REQUEST = 1;
FX_CONST uint32_t MR_WIDE_HEADER_BUSY = 2;
FX_CONST uint32_t MR_WIDE_HEADER_MARKS = 4;
FX_CONST uint32_t MR_WIDE_MARK_WORDS = MR_BLOCK_CELLS / 32;
FX_CONST uint32_t MR_WIDE_HEADER_TABLES = MR_WIDE_HEADER_MARKS + MR_WIDE_MARK_WORDS;
FX_CONST uint32_t MR_WIDE_TABLE_WORDS = 1 + MR_WIDE_SIDE_CHUNKS;
FX_CONST uint32_t MR_WIDE_HEADER_WORDS = MR_WIDE_HEADER_TABLES + (2 * MR_WIDE_TABLE_WORDS);

// 要求の語: 0 = 無し。ビット 0〜7 = 書く面に要る塊の数、8 = 書く面、9 = 刻み直す(足りなかった)、10 = 配れた
FX_CONST uint32_t MR_WIDE_REQUEST_PRESENT = 1u << 31;
FX_CONST uint32_t MR_WIDE_REQUEST_RETRY = 1u << 9;
FX_CONST uint32_t MR_WIDE_REQUEST_GRANTED = 1u << 10;

FX_FN uint32_t MrWideMakeRequest(uint32_t side, uint32_t chunks, bool retry) {
    return MR_WIDE_REQUEST_PRESENT | (retry ? MR_WIDE_REQUEST_RETRY : 0u) | (side << 8) | chunks;
}

FX_FN uint32_t MrWideRequestSide(uint32_t request) {
    return (request >> 8) & 1u;
}

FX_FN uint32_t MrWideRequestChunks(uint32_t request) {
    return request & 0xffu;
}

// 溢れの成分 entries 個の面に要る塊の数(溢れが無ければ 0: 始まりの表も持たない = 全部 0 と読む)
FX_FN uint32_t MrWideChunksFor(uint32_t entries) {
    if (entries == 0)
        return 0;

    const uint32_t words = (MR_BLOCK_CELLS + 1) + (MR_WIDE_ENTRY_WORDS * entries);

    return (words + MR_WIDE_CHUNK_WORDS - 1) / MR_WIDE_CHUNK_WORDS;
}

// --- 置き場: [塊の数][空きの数][塊 × MR_WIDE_CHUNK_WORDS][空きのスタック × 塊の数] ---
FX_CONST uint32_t MR_WIDE_POOL_CAPACITY = 0;
FX_CONST uint32_t MR_WIDE_POOL_FREE = 1;
FX_CONST uint32_t MR_WIDE_POOL_CHUNKS = 2;

// 頁 page の見出しの先頭(u6 の語。base = 溢れの領域の始まり)
FX_FN uint32_t MrWideHeaderWord(uint32_t base, uint32_t page) {
    return base + (page * MR_WIDE_HEADER_WORDS);
}

// 面 side の塊の表の先頭([塊の数][塊の番号 × MR_WIDE_SIDE_CHUNKS])
FX_FN uint32_t MrWideTableWord(uint32_t base, uint32_t page, uint32_t side) {
    return MrWideHeaderWord(base, page) + MR_WIDE_HEADER_TABLES + (side * MR_WIDE_TABLE_WORDS);
}

// 置き場の先頭(pages = 頁の数)
FX_FN uint32_t MrWidePoolWord(uint32_t base, uint32_t pages) {
    return base + (pages * MR_WIDE_HEADER_WORDS);
}

// 塊 chunk の先頭
FX_FN uint32_t MrWideChunkWord(uint32_t pool, uint32_t chunk) {
    return pool + MR_WIDE_POOL_CHUNKS + (chunk * MR_WIDE_CHUNK_WORDS);
}

// 空きのスタックの先頭(capacity = 塊の数)
FX_FN uint32_t MrWideStackWord(uint32_t pool, uint32_t capacity) {
    return pool + MR_WIDE_POOL_CHUNKS + (capacity * MR_WIDE_CHUNK_WORDS);
}

// 溢れの領域の語の数
FX_FN uint64_t MrWideAreaWords(uint32_t pages, uint32_t capacity) {
    return ((uint64_t)pages * MR_WIDE_HEADER_WORDS) + MR_WIDE_POOL_CHUNKS +
           ((uint64_t)capacity * (MR_WIDE_CHUNK_WORDS + 1));
}

// 面の成分 entry の先頭(面の論理の語)
FX_FN uint32_t MrWideEntryWord(uint32_t entry) {
    return (MR_BLOCK_CELLS + 1) + (MR_WIDE_ENTRY_WORDS * entry);
}

MR_NAMESPACE_END

#endif  // BICAMERAL_MULTIRES_WIDE_HLSLI
