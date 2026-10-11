// multires_wide.hlsli — GPU の多重解像度の世界のセルの溢れ(成分の二段の GPU 版。T-0176・T-0236・02 §3・17 R-MULTI-4)の大きさと並び。
// C++(engine/src/sim/gpu_multires.cpp が VRAM を取り、写し・読み戻す)と HLSL(shaders/sim/multires_wide_step.hlsli が刻む)で共有する。
//
// セルは「インライン RX_MAX_CELL_SPECIES 個(u1 の頁。今と同じ RxCell)+ 頁ごとの溢れ」。溢れは u6(木の管理の uint32 の表)の後ろに置く:
//   [頁ごとの見出し MR_WIDE_HEADER_WORDS 語 × 頁の数][塊の置き場: 塊の数・空きの数・塊 × MR_WIDE_CHUNK_WORDS・空きのスタック]
// 頁の溢れは 2 面。面ごとの中身(論理の語)は [セルの溢れの始まり × (MR_BLOCK_CELLS + 1)][成分 × 溢れの数(物質 ID・µmol の下位・上位の 3 語)]
// で、溢れはセルの番号の順に詰める(CPU の MultiresOverflowArea と同じ並び。グループ内のプレフィックス和)。論理の語は MR_WIDE_CHUNK_WORDS 語の
// 塊に分けて、見出しの塊の表(面ごと)が置き場の塊の番号を持つ(T-0236: 使う分だけの VRAM。溢れの無い面は塊を持たず、空気だけの頁は見出しだけ)。
// 刻むブロックは今の面を読み、もう片方の面へ書いてから面を入れ替える(読む前に別のセルの溢れを上書きしないため)。
// 端数の枠も同じ形の溢れ(見出し・2 面・塊は同じ置き場)を持ち、見出しは空きのスタックの後ろに枠の順に並ぶ(T-0311)。
// 溢れの領域の番号(area)は、頁は頁の番号、端数の枠は「頁の数 + 枠の番号」。
// 書く面の塊が足りなければ、印(要求)を付けて刻みの後の段(WideAllocate)が枠の順に塊を配り、WideRetry が同じ刻みのうちに
// 溢れるセルだけ刻み直す(T-0236。置き場が尽きた時だけ待たせて MR_COUNTER_LIMIT_PRODUCTS に数える)。
// 1 セルが GPU で持てる成分は RX_GPU_WIDE_SPECIES まで(T-0248 で 24 → 32、T-0272 で 64、T-0327 で 96。表の物質が 96 以下なら
// CPU の上限の無い世界と食い違わない。超える生成物は待たせて MR_COUNTER_LIMIT_PRODUCTS に数える。32 を超えると HW の compute キューで
// 伝導の段が止まったのは、512 スレッドの段が上限の無い形のセルを持っていたため〔T-0290〕で、T-0272 で伝導の段から無くした。
// 上限の大きさの形を持つのは 64 スレッドの WideDeferred・WideRetry だけ〔T-0326〕。128 にすると、その形が 1 度も使われない場面でも HW の
// compute キューの刻みが終わらなくなった〔T-0327。T-0290 と同じくスレッドごとのスクラッチの大きさが疑わしい〕。見出しの大きさは上限に依らない
// 〔T-0327。塊の表の続きを最初の塊に置く〕。上限を無くす〔核をセルの形の道具で作業場から読む〕のは T-0362)。
#ifndef BICAMERAL_MULTIRES_WIDE_HLSLI
#define BICAMERAL_MULTIRES_WIDE_HLSLI

#include "multires.hlsli"

MR_NAMESPACE_BEGIN

FX_CONST uint32_t RX_GPU_WIDE_SPECIES = 96;  // GPU の刻みが 1 セルで持てる成分の数(インライン 8 + 溢れ 88。T-0327)
FX_CONST uint32_t MR_WIDE_ENTRY_WORDS = 3;   // 物質 ID・µmol の下位・上位
// 面の溢れの成分の最大(全部のセルが RX_GPU_WIDE_SPECIES 種)と、面の論理の語の最大
FX_CONST uint32_t MR_WIDE_SIDE_ENTRIES = MR_BLOCK_CELLS * (RX_GPU_WIDE_SPECIES - RX_MAX_CELL_SPECIES);
FX_CONST uint32_t MR_WIDE_SIDE_WORDS = (MR_BLOCK_CELLS + 1) + (MR_WIDE_ENTRY_WORDS * MR_WIDE_SIDE_ENTRIES);

// --- 塊(T-0236)---
FX_CONST uint32_t MR_WIDE_CHUNK_WORDS = 1024;  // 4 KiB。最初の塊に塊の表の続き・始まりの表(513 語)と成分
// 面の塊の表は [塊の数][最初の塊の番号](見出し)+ 2 番目からの塊の番号(最初の塊の先頭 MR_WIDE_TABLE_SPILL 語)。
// 見出しの大きさが 1 セルの上限(RX_GPU_WIDE_SPECIES)に依らない(T-0327。前は見出しに表を丸ごと持ち、上限 64 で 192 語 = 768 B/頁)。
// 面の塊の数の最大 c は、表の続き(c − 1 語)と論理の語が c 塊に入る最小: 1024c ≥ (c − 1) + 論理の語 ⇔ c ≥ (論理の語 − 1) ÷ 1023
FX_CONST uint32_t MR_WIDE_SIDE_CHUNKS = (MR_WIDE_SIDE_WORDS - 1 + (MR_WIDE_CHUNK_WORDS - 2)) / (MR_WIDE_CHUNK_WORDS - 1);
FX_CONST uint32_t MR_WIDE_TABLE_SPILL = MR_WIDE_SIDE_CHUNKS - 1;  // 最初の塊の先頭に置く塊の表の続き(語)

// --- 頁の見出し: [今の面][要求][要求した刻みの busyTick の下位・上位][刻み直すセルの印 × 16][面 0 の塊の数・最初の塊][面 1 の塊の数・最初の塊] ---
FX_CONST uint32_t MR_WIDE_HEADER_SIDE = 0;
FX_CONST uint32_t MR_WIDE_HEADER_REQUEST = 1;
FX_CONST uint32_t MR_WIDE_HEADER_BUSY = 2;
FX_CONST uint32_t MR_WIDE_HEADER_MARKS = 4;
FX_CONST uint32_t MR_WIDE_MARK_WORDS = MR_BLOCK_CELLS / 32;
FX_CONST uint32_t MR_WIDE_HEADER_TABLES = MR_WIDE_HEADER_MARKS + MR_WIDE_MARK_WORDS;
FX_CONST uint32_t MR_WIDE_TABLE_WORDS = 2;
FX_CONST uint32_t MR_WIDE_HEADER_WORDS = MR_WIDE_HEADER_TABLES + (2 * MR_WIDE_TABLE_WORDS);

// 要求の語: 0 = 無し。ビット 0〜7 = 書く面に要る塊の数、8 = 書く面、9 = 刻み直す(足りなかった)、10 = 配れた、
// 11 = 伝導の段が印のセルの反応を後の段(WideDeferredPass)に回した(T-0272。塊の数・面は持たない。WideDeferredPass が消してから ② で書き直す)
FX_CONST uint32_t MR_WIDE_REQUEST_PRESENT = 1u << 31;
FX_CONST uint32_t MR_WIDE_REQUEST_RETRY = 1u << 9;
FX_CONST uint32_t MR_WIDE_REQUEST_GRANTED = 1u << 10;
FX_CONST uint32_t MR_WIDE_REQUEST_DEFERRED = 1u << 11;

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

    const uint32_t words = MR_WIDE_TABLE_SPILL + (MR_BLOCK_CELLS + 1) + (MR_WIDE_ENTRY_WORDS * entries);

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

// 面 side の塊の表の先頭([塊の数][最初の塊の番号]。2 番目からの番号は最初の塊の先頭。MrWideTableEntryInChunk)
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

// 端数の枠 fractionSlot の見出しの先頭(T-0311。空きのスタックの後ろ。capacity = 塊の数)
FX_FN uint32_t MrWideFractionHeaderWord(uint32_t pool, uint32_t capacity, uint32_t fractionSlot) {
    return MrWideStackWord(pool, capacity) + capacity + (fractionSlot * MR_WIDE_HEADER_WORDS);
}

// 見出しの先頭 header から、面 side の塊の表の先頭
FX_FN uint32_t MrWideTableAt(uint32_t header, uint32_t side) {
    return header + MR_WIDE_HEADER_TABLES + (side * MR_WIDE_TABLE_WORDS);
}

// 溢れの領域の語の数(fractions = 端数の枠の数。T-0311)
FX_FN uint64_t MrWideAreaWords(uint32_t pages, uint32_t capacity, uint32_t fractions) {
    return ((uint64_t)pages * MR_WIDE_HEADER_WORDS) + MR_WIDE_POOL_CHUNKS +
           ((uint64_t)capacity * (MR_WIDE_CHUNK_WORDS + 1)) + ((uint64_t)fractions * MR_WIDE_HEADER_WORDS);
}

// 面の論理の語 word の、塊を並べた中の位置(最初の塊の先頭は塊の表の続き。T-0327)
FX_FN uint32_t MrWideSideOffset(uint32_t word) {
    return MR_WIDE_TABLE_SPILL + word;
}

// 面の塊の表の j 番目(j ≥ 1)の、最初の塊の中の位置
FX_FN uint32_t MrWideTableEntryInChunk(uint32_t j) {
    return j - 1;
}

// 面の成分 entry の先頭(面の論理の語)
FX_FN uint32_t MrWideEntryWord(uint32_t entry) {
    return (MR_BLOCK_CELLS + 1) + (MR_WIDE_ENTRY_WORDS * entry);
}

MR_NAMESPACE_END

#endif  // BICAMERAL_MULTIRES_WIDE_HLSLI
