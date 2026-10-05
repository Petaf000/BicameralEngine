// multires_peek.hlsl — 覗き窓(T-0096。17 §3 B): 仮の世界(common/probe_sim.hlsli の 64³)と多重解像度の影の鎖をつなぐ Compute 2 つ。
// ルート署名は多重解像度のもの(multires_bindings.hlsli。sim/gpu_multires.cpp)に、外のバッファ u4・u5 を足して使う。
//
// データの流れ(sim/probe_peek.cpp が、シミュのフレームのリストの抽出の後ろに記録する):
//   MirrorWorld:   世界のセル(u4。S(t) の世代)の 8³ のブロック → 入れ子の枠 0(世界の写し MR_BLOCK_MIRROR。影の鎖の根の親)
//   → (multires_graph.hlsl の細かくする・引き戻す、multires_step.hlsl の刻み)
//   → ExtractShadow: 影の鎖(枠 1〜)のセルの温度と見る物質の量 → 抽出(u5)の覗きの欄(並びは probe_sim.hlsli)。描画が読む
// 世界のバッファは読むだけ(D-403: 見るだけなら世界は変わらない)。CPU リファレンスは sim/probe_peek.cpp の同じ名前の関数。
#include "common/probe_world.hlsli"
#include "sim/multires_bindings.hlsli"

RWStructuredBuffer<RxCell> g_world : register(u4);         // 世界のセル(2 世代 × PROBE_CELL_COUNT)。読むだけ
RWStructuredBuffer<uint32_t> g_extraction : register(u5);  // 抽出(PROBE_EXTRACTION_WORDS)。覗きの欄だけに書く

static const uint32_t PEEK_MIRROR_SLOT = 0;  // 世界の写しの枠(sim/probe_peek.h の PEEK_MIRROR_SLOT)
static const uint32_t PEEK_GROUP_THREADS = 64;

// --- 世界の写し: g_external0 = 世代(刻み & 1)、g_external1〜3 = ブロックの原点 x・y・z(レベル 0 のセル、8 の倍数)---
[numthreads(PEEK_GROUP_THREADS, 1, 1)] void MirrorWorld(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t index = dispatchThreadId.x;
    if (index >= MR_BLOCK_CELLS)
        return;

    const uint32_t x = g_external1 + MrCellX(index);
    const uint32_t y = g_external2 + MrCellY(index);
    const uint32_t z = g_external3 + MrCellZ(index);
    const uint32_t source = (g_external0 * PROBE_CELL_COUNT) + ProbeCellIndex(x, y, z);
    const uint32_t page = MrObserverPage(PEEK_MIRROR_SLOT, g_worldBlocks);  // 観察の枠はいつも頁を持つ(T-0102)
    g_cells[PageCellAddress(page, index)] = g_world[source];

    if (index == 0)
        g_blocks[PEEK_MIRROR_SLOT] = MrMakeMirrorBlock(0, g_external1, g_external2, g_external3, page);
}

// --- 影の抽出: g_external0〜2 = 見る物質 3 つ、g_external3 = 影の鎖の最初の枠。1 スレッド = 段のセル 1 つ ---
// 段の数 = 最初の枠から続く影のブロックの数(覗いていなければ 0)
uint32_t CountShadowLevels() {
    uint32_t count = 0;
    for (uint32_t i = 0; i < PROBE_PEEK_MAX_LEVELS; ++i) {
        const uint32_t slot = g_external3 + i;
        if (slot >= g_blockCount || g_blocks[slot].kind != MR_BLOCK_SHADOW)
            break;

        count += 1;
    }

    return count;
}

[numthreads(PEEK_GROUP_THREADS, 1, 1)] void ExtractShadow(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint32_t level = dispatchThreadId.x / MR_BLOCK_CELLS;
    const uint32_t index = dispatchThreadId.x % MR_BLOCK_CELLS;
    const uint32_t levelCount = CountShadowLevels();
    if (dispatchThreadId.x == 0)
        g_extraction[PROBE_EXTRACTION_PEEK_OFFSET] = levelCount;

    if (level >= levelCount)
        return;

    // --- 段の見出し(段の最初のスレッド)---
    const MrBlock block = g_blocks[g_external3 + level];
    if (index == 0) {
        const uint32_t header = PROBE_EXTRACTION_PEEK_OFFSET + PROBE_PEEK_LEVEL_WORDS +
                                (level * PROBE_PEEK_LEVEL_WORDS);
        g_extraction[header] = (uint32_t)block.level;
        g_extraction[header + 1] = (uint32_t)(int32_t)block.originX;
        g_extraction[header + 2] = (uint32_t)(int32_t)block.originY;
        g_extraction[header + 3] = (uint32_t)(int32_t)block.originZ;
    }

    // --- セルの 4 語(世界の抽出と同じ: 温度と見る物質の量)---
    const RxCell cell = LoadBlockCell(block, g_external3 + level, index);
    const uint32_t base = PROBE_EXTRACTION_PEEK_CELL_OFFSET +
                          (((level * PROBE_PEEK_BLOCK_CELLS) + index) * PROBE_EXTRACTION_CELL_WORDS);
    g_extraction[base] = ProbeMakeCache(MakeTable(), cell).temperature;
    g_extraction[base + 1] = ProbeViewAmount(cell, g_external0);
    g_extraction[base + 2] = ProbeViewAmount(cell, g_external1);
    g_extraction[base + 3] = ProbeViewAmount(cell, g_external2);
}
