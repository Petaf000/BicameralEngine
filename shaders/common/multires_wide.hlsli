// multires_wide.hlsli — GPU の多重解像度の世界のセルの溢れ(成分の二段の GPU 版。T-0176・02 §3・17 R-MULTI-4)の大きさと並び。
// C++(engine/src/sim/gpu_multires.cpp が VRAM を取り、写し・読み戻す)と HLSL(shaders/sim/multires_wide_step.hlsli が刻む)で共有する。
//
// セルは「インライン RX_MAX_CELL_SPECIES 個(u1 の頁。今と同じ RxCell)+ 頁ごとの溢れ」。溢れは u6(木の管理の uint32 の表)の後ろに
// 頁ごとに MR_WIDE_PAGE_WORDS 語: [今の面(0 か 1)][面 0][面 1]。面は [セルの溢れの始まり × (MR_BLOCK_CELLS + 1)][成分 × MR_WIDE_PAGE_ENTRIES
// (物質 ID・µmol の下位・上位の 3 語)]。溢れはセルの番号の順に詰める(CPU の MultiresOverflowArea と同じ並び。グループ内のプレフィックス和)。
// 刻むブロックは今の面を読み、もう片方の面へ書いてから面を入れ替える(読む前に別のセルの溢れを上書きしないため)。
//
// 当座の上限(実装の細部として T-0176 で決めた値。HANDOFF「このチャットで決めたこと」): 頁ごとの溢れの枠は決まった数(MR_WIDE_PAGE_ENTRIES)で、
// 1 セルが GPU で持てる成分は RX_GPU_WIDE_SPECIES まで。どちらかを超える刻みは、そのセルを待たせて MR_COUNTER_LIMIT_PRODUCTS に数える
// (CPU の上限なしの世界と食い違う)。足りない時に大きい溢れを配って同じ刻みでやり直すのは T-0211。
#ifndef BICAMERAL_MULTIRES_WIDE_HLSLI
#define BICAMERAL_MULTIRES_WIDE_HLSLI

#include "multires.hlsli"

MR_NAMESPACE_BEGIN

FX_CONST uint32_t RX_GPU_WIDE_SPECIES = 24;     // GPU の刻みが 1 セルで持てる成分の数(インライン 8 + 溢れ 16)
FX_CONST uint32_t MR_WIDE_PAGE_ENTRIES = 1024;  // 頁ごとの溢れの成分の数(1 面。平均 1 セル 2 種まで)
FX_CONST uint32_t MR_WIDE_ENTRY_WORDS = 3;      // 物質 ID・µmol の下位・上位
FX_CONST uint32_t MR_WIDE_SIDE_WORDS = (MR_BLOCK_CELLS + 1) + (MR_WIDE_ENTRY_WORDS * MR_WIDE_PAGE_ENTRIES);
FX_CONST uint32_t MR_WIDE_PAGE_WORDS = 1 + (2 * MR_WIDE_SIDE_WORDS);

// 頁 page の溢れの先頭(u6 の語。base = 溢れの領域の始まり)
FX_FN uint32_t MrWidePageWord(uint32_t base, uint32_t page) {
    return base + (page * MR_WIDE_PAGE_WORDS);
}

// 頁 page の面 side の先頭
FX_FN uint32_t MrWideSideWord(uint32_t base, uint32_t page, uint32_t side) {
    return MrWidePageWord(base, page) + 1 + (side * MR_WIDE_SIDE_WORDS);
}

// 面の成分 entry の先頭(面の先頭から)
FX_FN uint32_t MrWideEntryWord(uint32_t entry) {
    return (MR_BLOCK_CELLS + 1) + (MR_WIDE_ENTRY_WORDS * entry);
}

MR_NAMESPACE_END

#endif  // BICAMERAL_MULTIRES_WIDE_HLSLI
