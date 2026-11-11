// multires_refine_halo.hlsl — 木を細かくする段の置かれ方(MR_REFINE_PLACE の世界。T-1188)のための「隣の写し」を作る Compute の段(T-1189。D-664・20 §10.5)。
// 要求の処理の TreeAllocate の後・Work Graph の適用の前に 1 回(gpu_multires.cpp の RecordProcessRequests。置かれ方の世界だけ)。
// 1 グループ = 要求 1 つ・1 スレッド = 写しのセル 1 つ(MR_HALO_CELLS 個。並びは common/multires_refine_halo.hlsli)。許可した細かくする要求の
// 鎖の段ごとに、親のブロックの八分の一の外のセル(P の外を向く 3 面と中を向く 3 面の 6 × 6)の詰まりを、索引で同じレベルの本物のブロック
// (子に覆われたセルは覆う子のセル 2³ の和。T-1314)から読んで u6 の
// g_overflowBase からの置き場(段 d の写しの位置 = state.blockBase − 1 − d)へ書く。CPU リファレンスは
// engine/src/sim/multires_refine_cells.cpp の SnapshotRefineHalos(同じ式。tests/gpu_tree_refine_placement_test.cpp でビット一致)。
// 読むのは刻みの前の木(適用の前)なので、同じ刻みの別の要求が隣を覆っても・粗くしても答えは変わらない(順番に依存しない)。
#include "sim/multires_bindings.hlsli"
#include "sim/multires_layer_access.hlsli"  // 層の置き場(u4。層の分は沈む側として詰まりに数える。T-1240)
#include "sim/multires_refine_place.hlsli"

static const uint32_t HALO_THREADS = 256;  // MR_HALO_CELLS(216)以上

// --- 子に覆われたセルの詰まり(T-1314。CPU の CoveredFullness: 子のセル 2³ の量を成分ごとに足し、子 8 つの器で読む。
// 局所の配列は持たず、成分ごとに子をバッファから読み直す〔T-0397〕。足す順は効かない)---

// 子のセルの番地(一様な枠は一様の値。覆われていないセルだけ読む)
uint32_t CoveredChildAddress(uint32_t childSlot, uint32_t childCell) {
    return MrIsUniform(g_blocks[childSlot]) ? childSlot : PageCellAddress(g_blocks[childSlot].page, childCell);
}

// 子のセルの層の組の始まり(CPU の LoadNestCellLayers)
uint32_t CoveredChildLayerBase(uint32_t childSlot, uint32_t childCell) {
    if (MrIsUniform(g_blocks[childSlot]))
        return UniformHasLayers(childSlot) ? UniformLayerBase(childSlot) : MR_LAYER_NO_PAGE;

    return PlaceLayerBase(LayerPageOf(g_blocks[childSlot].page), childCell);
}

// 子 j より前の子に成分 species があるか(成分ごとに 1 回だけ足す)
bool CoveredSpeciesSeen(uint32_t childSlot, uint32_t local, uint32_t j, uint32_t species) {
    bool seen = false;
    for (uint32_t k = 0; k < j; ++k) {
        const uint32_t address = CoveredChildAddress(childSlot, MrChildCell(local, k));
        for (uint32_t n = 0; n < g_cells[address].speciesCount; ++n)
            seen = seen || g_cells[address].species[n] == species;
    }

    return seen;
}

// 成分 1 つの子の量の和と層の和
struct CoveredTotal {
    uint64_t amount;
    uint64_t layer;
};

// 成分 species の子 j 以降の量の和と層の和
CoveredTotal CoveredSpeciesSum(uint32_t childSlot, uint32_t local, uint32_t j, uint32_t species) {
    uint64_t amount = 0;
    uint64_t layer = 0;
    for (uint32_t k = j; k < 8; ++k) {
        const uint32_t childCell = MrChildCell(local, k);
        const uint32_t address = CoveredChildAddress(childSlot, childCell);
        PlaceUseCell(address, CoveredChildLayerBase(childSlot, childCell));
        for (uint32_t n = 0; n < g_cells[address].speciesCount; ++n) {
            if (g_cells[address].species[n] != species)
                continue;

            amount += g_cells[address].amounts[n];
            layer += PlaceLayerOf(n);
        }
    }

    CoveredTotal total;
    total.amount = amount;
    total.layer = layer;

    return total;
}

// 足した成分 1 つの沈み固まった体積(CPU の CoveredSettleSum。層の分は沈む側・層を使う世界の粒の残りは舞う粒)
SfSettleSum CoveredAddSpecies(SfSettleSum sum, uint32_t species, CoveredTotal total) {
    const uint3 material = PlaceMaterialOf(species);
    const uint32_t molarVolume = g_species[species].molarVolume;
    const bool floats = LayerGrainFloats() && material.x == SF_SETTLE_GRAIN;
    const uint64_t amount = total.amount;
    const uint64_t layer = total.layer;
    const bool hasLayer = ((uint32_t)layer | (uint32_t)(layer >> 32)) != 0;
    if (!hasLayer && !floats)
        return SfAddSettleOf(sum, material.x, material.y, material.z, molarVolume, (int64_t)amount);

    const uint32_t settledKind = material.x == SF_SETTLE_GAS ? SF_SETTLE_GRAIN : material.x;
    const uint32_t restKind = floats ? SF_SETTLE_GAS : material.x;
    if (hasLayer)
        sum = SfAddSettleOf(sum, settledKind, material.y, material.z, molarVolume, (int64_t)layer);

    if (MrLayerLess(layer, amount))
        sum = SfAddSettleOf(sum, restKind, material.y, material.z, molarVolume, (int64_t)PlaceSubU64(amount, layer));

    return sum;
}

// 枠 slot のセル index(子に覆われている)の詰まり(子のセル 2³ は子のブロックの同じ八分の一にあるので、孫に覆われていれば無い)
int32_t CoveredFullness(uint32_t slot, uint32_t index) {
    const uint32_t childSlot = g_blocks[slot].children[MrOctantOfCell(index)];
    const uint32_t local = (MrCellX(index) & 3u) | ((MrCellY(index) & 3u) << 2) | ((MrCellZ(index) & 3u) << 4);
    if (g_blocks[childSlot].children[MrOctantOfCell(MrChildCell(local, 0))] != MR_NO_BLOCK)
        return MR_HALO_ABSENT;

    SfSettleSum sum = SfMakeSettleSum();
    for (uint32_t j = 0; j < 8; ++j) {
        const uint32_t address = CoveredChildAddress(childSlot, MrChildCell(local, j));
        for (uint32_t i = 0; i < g_cells[address].speciesCount; ++i) {
            const uint32_t species = g_cells[address].species[i];
            if (CoveredSpeciesSeen(childSlot, local, j, species))
                continue;

            sum = CoveredAddSpecies(sum, species, CoveredSpeciesSum(childSlot, local, j, species));
        }
    }

    return (int32_t)SfSettleFullnessQ16(SfSettledVolume(sum), MR_CELL_VOLUME_MM3 * 8, sum.anchored);
}

// 親のブロック(レベル level・原点 origin)の中の座標 cell(外に出るか P の別の八分の一)を含む同じレベルの本物のブロックのセルの詰まり
// (CPU の HaloFullness。子に覆われていれば CoveredFullness)
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
        return CoveredFullness(slot, index);

    const MrBlock block = g_blocks[slot];
    const bool uniformBlock = MrIsUniform(block);
    const uint32_t address = uniformBlock ? slot : PageCellAddress(block.page, index);

    // 層(CPU の CellFullness は LoadNestCellLayers: 一様な枠は一様の枠の層・頁は頁の層。T-1240)
    uint32_t layerBase = MR_LAYER_NO_PAGE;
    if (uniformBlock && UniformHasLayers(slot))
        layerBase = UniformLayerBase(slot);
    else if (!uniformBlock)
        layerBase = PlaceLayerBase(LayerPageOf(block.page), index);

    return (int32_t)PlaceFullnessAt(address, layerBase);
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
