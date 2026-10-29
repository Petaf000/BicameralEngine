// multires_refine_halo.hlsl — 木を細かくする段の置かれ方(MR_REFINE_PLACE の世界。T-1188)のための「隣の写し」を作る Compute の段(T-1189。D-664・20 §10.5)。
// 要求の処理の TreeAllocate の後・Work Graph の適用の前に 1 回(gpu_multires.cpp の RecordProcessRequests。置かれ方の世界だけ)。
// 1 グループ = 要求 1 つ・1 スレッド = 写しのセル 1 つ(MR_HALO_CELLS 個。並びは common/multires_refine_halo.hlsli)。許可した細かくする要求の
// 鎖の段ごとに、親のブロックの八分の一の外を向く 3 面の 6 × 6 のセルの詰まりを、索引で同じレベルの本物のブロックから読んで u6 の
// g_overflowBase からの置き場(段 d の写しの位置 = state.blockBase − 1 − d)へ書く。CPU リファレンスは
// engine/src/sim/multires_refine_cells.cpp の SnapshotRefineHalos(同じ式。tests/gpu_tree_refine_placement_test.cpp でビット一致)。
// 読むのは刻みの前の木(適用の前)なので、同じ刻みの別の要求が隣を覆っても・粗くしても答えは変わらない(順番に依存しない)。
#include "sim/multires_bindings.hlsli"
#include "sim/multires_refine_place.hlsli"

static const uint32_t HALO_THREADS = 128;

// 親のブロック(レベル level・原点 origin)の中の座標 cell(外に出る)を含む同じレベルの本物のブロックのセルの詰まり(CPU の HaloFullness)
int32_t HaloFullness(int32_t level, int64_t originX, int64_t originY, int64_t originZ, int3 cell) {
    const int32_t edge = (int32_t)MR_BLOCK_EDGE;
    const int3 offset = int3(cell.x < 0 ? -edge : (cell.x >= edge ? edge : 0),
                             cell.y < 0 ? -edge : (cell.y >= edge ? edge : 0),
                             cell.z < 0 ? -edge : (cell.z >= edge ? edge : 0));
    const uint32_t slot = LookupBlock(level, originX + offset.x, originY + offset.y, originZ + offset.z);
    if (slot == MR_NO_BLOCK)
        return MR_HALO_ABSENT;

    const uint32_t index = MrCellIndex((uint32_t)(cell.x - offset.x), (uint32_t)(cell.y - offset.y),
                                       (uint32_t)(cell.z - offset.z));

    // 子はバッファから直に読む(読んだ構造体の配列を動的な添字で読むと局所の配列になる。T-0397・T-1233)
    if (g_blocks[slot].children[MrOctantOfCell(index)] != MR_NO_BLOCK)
        return MR_HALO_ABSENT;

    const MrBlock block = g_blocks[slot];
    const uint32_t address = MrIsUniform(block) ? slot : PageCellAddress(block.page, index);

    return (int32_t)PlaceFullnessAt(address);
}

[numthreads(HALO_THREADS, 1, 1)] void TreeRefineHalo(uint32_t h : SV_GroupIndex, uint3 group : SV_GroupID) {
    const uint32_t i = group.x;
    if (i >= g_counters[MR_COUNTER_REQUESTS] || h >= MR_HALO_CELLS)
        return;

    const MrRequest request = g_requests[i];
    const MrRequestState state = g_states[i];
    if (state.status != MR_STATUS_GRANTED || request.op != MR_REQUEST_REFINE || MrIsUniform(g_blocks[state.target]))
        return;

    const int32_t targetLevel = g_blocks[state.target].level;
    for (uint32_t depth = 0; depth < state.levels; ++depth) {
        // --- 鎖の段 d の親(レベル = 始まりの親 + d・原点は要求の点から)---
        const int32_t level = targetLevel + (int32_t)depth;
        const int64_t originX = MrBlockOriginOf(MrCoarserCoordinate(request.x, request.level, level));
        const int64_t originY = MrBlockOriginOf(MrCoarserCoordinate(request.y, request.level, level));
        const int64_t originZ = MrBlockOriginOf(MrCoarserCoordinate(request.z, request.level, level));
        const uint32_t octant = MrOctantBitOfPoint(originX, level, request.x, request.level) |
                                (MrOctantBitOfPoint(originY, level, request.y, request.level) << 1) |
                                (MrOctantBitOfPoint(originZ, level, request.z, request.level) << 2);
        const int3 cell = int3(MrHaloCellAxis(h, octant, 0), MrHaloCellAxis(h, octant, 1),
                               MrHaloCellAxis(h, octant, 2));
        const int32_t fullness = HaloFullness(level, originX, originY, originZ, cell);
        g_treeWords[RefineHaloAddress(state.blockBase - 1 - depth, h)] = (uint32_t)fullness;
    }
}
