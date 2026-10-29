// multires_refine_place.hlsli — 世界の木を細かくする時、寄せるセルの中身を「置かれ方で配る」GPU の段(T-1188。D-664・D-674・D-679・
// ADR-0469 の 2-1・20 §10.5・17 §3.2)。multires_graph.hlsl の RefineLevel が、親の八分の一の 4³ セルを子へ同じ数で写した後に呼ぶ
// (溢れを使わない変種だけ。溢れを使う世界は SAME のまま = CPU と同じ)。
// CPU リファレンスは engine/src/sim/multires_refine_cells.cpp の PlaceTreeCell・PlaceRefineOctantCells(同じ式・同じ順でビット単位で同じ。
// tests/gpu_tree_refine_placement_test.cpp)。回しは sim/placement_refine.hlsli、1 つずつの式は common/state_field.hlsli(CPU と 1 つの源)。
// データの流れ: 親の頁のセル(成分 µmol・エネルギー mJ)と物質の表(g_species の相の印・粒の詰まり・モル体積・比熱・H0)
//   → グループ共有に親のブロックの詰まり(1 スレッド = 親のセル t)→ 子のセル t(1 スレッド = 子のセル t)の成分とエネルギーを子の頁へ。
// 「数の空間」: 子 1 つの器 = レベル 0 のセルの体積 MR_CELL_VOLUME_MM3・配る量 = 親の数 × 8(子のレベルの単位)。
// 寄せないセル(気体だけ・詰まり切った・据わる固体を含む・凝縮相が無い)は写したまま触らない。端数の枠は SAME のまま。
// 隣は同じ親のブロックの中だけ(子に覆われたセル・ブロックの外は無いと読む: 横 → 傾き 0・下 → 詰まり切る・上 → 空。T-1176 の 1 本目の仮)。
// 書き換えた構造体の配列を動的な添字で読まない(T-0397): 親のセルの成分は毎回バッファから読み、子のセルはバッファへ直接書く。
// 64bit の引き算と同じ値どうしの比べを並べない(T-1078・T-1159): 差は 32bit の 2 つに分けて借りを手で回す(PrSubS64)。
#ifndef BICAMERAL_MULTIRES_REFINE_PLACE_HLSLI
#define BICAMERAL_MULTIRES_REFINE_PLACE_HLSLI

#include "common/state_field.hlsli"

static uint32_t s_placeAddress;  // 今のスレッドが読む親のセルの番地(g_cells)

// 物質の相の印 → 置かれ方の種類・密度(使わない。体積はモル体積から)・粒の詰まり(%。0 は 100)
uint3 PlaceMaterialOf(uint32_t speciesId) {
    const uint32_t flags = g_species[speciesId].flags;
    uint32_t kind = SF_SETTLE_GAS;
    if ((flags & RX_SPECIES_ANCHORED) != 0u)
        kind = SF_SETTLE_ANCHORED;
    else if ((flags & RX_SPECIES_GRAIN) != 0u)
        kind = SF_SETTLE_GRAIN;
    else if ((flags & RX_SPECIES_LIQUID) != 0u)
        kind = SF_SETTLE_LIQUID;

    const uint32_t packing = kind == SF_SETTLE_GRAIN ? RxSpeciesPackingPercent(flags) : 0u;

    return uint3(kind, 0u, packing);
}

#define PR_MATERIAL_COUNT g_cells[s_placeAddress].speciesCount
#define PR_MATERIAL(m) PlaceMaterialOf(g_cells[s_placeAddress].species[m])
#define PR_MATERIAL_MOLAR_VOLUME(m) g_species[g_cells[s_placeAddress].species[m]].molarVolume
#define PR_PARENT_MASS(m) ((int64_t)g_cells[s_placeAddress].amounts[m] * 8)
#include "sim/placement_refine.hlsli"

// グループ共有 6 KiB(HW の Work Graph の再帰のノードの 1 グループの重さを小さく保つ。12 KiB の 4 つの欄では RTX の
// gpu_multires〔置かれ方を使わない木〕が DEVICE_HUNG になった。T-1188)。1 つの欄を段ごとに使い回し、入れ替わる所にバリアを置く:
//   gs_placeNarrow = 親のブロックのセルの詰まり(Q16。器 = 子 8 つ。無いセルは −1 を符号無しで)→ 子のセルの空きの体積(mm³)
//   gs_placeWide   = 子のセルの熱容量(nJ/K)→ 子のセルのエネルギー(mJ。残りを取る子は 0。符号付きを符号無しで)
// 詰まり・空きは 32bit に収まる(詰まりは 0〜2^16・空きは子の器以下)
groupshared uint32_t gs_placeNarrow[MR_BLOCK_CELLS];
groupshared uint64_t gs_placeWide[MR_BLOCK_CELLS];

// --- 隣の読み方(CPU の Present・FullnessAt・SurfaceAt・SlopeAt)---

// 親のセルの詰まり(器 = 子 8 つ。CPU の FullnessAt)
int64_t PlaceFullnessAt(uint32_t address) {
    s_placeAddress = address;
    SfSettleSum sum = SfMakeSettleSum();
    for (uint32_t m = 0; m < PR_MATERIAL_COUNT; ++m)
        sum = PrAddMaterial(sum, m, PR_PARENT_MASS(m));

    return SfSettleFullnessQ16(SfSettledVolume(sum), MR_CELL_VOLUME_MM3 * 8, sum.anchored);
}

bool PlacePresent(int32_t x, int32_t y, int32_t z) {
    const int32_t edge = (int32_t)MR_BLOCK_EDGE;
    if (x < 0 || y < 0 || z < 0 || x >= edge || y >= edge || z >= edge)
        return false;

    return (int32_t)gs_placeNarrow[MrCellIndex((uint32_t)x, (uint32_t)y, (uint32_t)z)] >= 0;
}

int64_t PlaceFullnessOf(int32_t x, int32_t y, int32_t z) {
    return (int32_t)gs_placeNarrow[MrCellIndex((uint32_t)x, (uint32_t)y, (uint32_t)z)];
}

// 列 (x, z) の段 y の表面(下が無い = 詰まり切る・上が無い = 空)
int64_t PlaceSurfaceAt(int32_t x, int32_t y, int32_t z) {
    const int64_t below = PlacePresent(x, y - 1, z) ? PlaceFullnessOf(x, y - 1, z) : SF_SETTLE_FULL_Q16;
    const int64_t above = PlacePresent(x, y + 1, z) ? PlaceFullnessOf(x, y + 1, z) : 0;

    return SfColumnSurfaceQ16(below, PlaceFullnessOf(x, y, z), above);
}

// 軸 (dx, dz) の向きの傾き(両側の列が無ければ 0)
int64_t PlaceSlopeAt(int32_t x, int32_t y, int32_t z, int32_t dx, int32_t dz) {
    if (!PlacePresent(x - dx, y, z - dz) || !PlacePresent(x + dx, y, z + dz))
        return 0;

    return SfSurfaceSlopeQ16(PlaceSurfaceAt(x - dx, y, z - dz), PlaceSurfaceAt(x, y, z),
                             PlaceSurfaceAt(x + dx, y, z + dz), true);
}

// --- 子 j の物質 m の量(気体は子 8 つの空きの体積で配り直す。placement_refine_check.hlsl と同じ)---
// 128bit の割り算(FxMulDivS64)を書く所を少なくする: 他の子の分の和は回し([loop])で 1 か所に(HW の Work Graph のノードを軽く保つ)
int64_t PlaceChildMass(PrCell cell, PrColumn column, uint32_t m, uint32_t j, uint32_t local, uint64_t ownOpen,
                       uint64_t openSum) {
    const int64_t mass = PrChildMass(cell, column, m, j);
    if (!cell.settled || PR_MATERIAL(m).x != SF_SETTLE_GAS)
        return mass;

    const int64_t parentMass = PR_PARENT_MASS(m);
    int64_t othersGiven = 0;
    if (openSum != 0 && j == 7) {
        [loop] for (uint32_t k = 0; k < 7; ++k)
            othersGiven += FxMulDivS64(parentMass, gs_placeNarrow[MrChildCell(local, k)], openSum);
    }

    return PrGasShare(mass, parentMass, ownOpen, openSum, othersGiven, j);
}

// 子 t の番号 j(MrChildCell の j)と八分の一の中の親の番号 local
uint2 PlaceChildIndex(uint32_t t) {
    const uint32_t x = MrCellX(t);
    const uint32_t y = MrCellY(t);
    const uint32_t z = MrCellZ(t);

    return uint2((x & 1u) | ((y & 1u) << 1) | ((z & 1u) << 2), (x >> 1) | ((y >> 1) << 2) | ((z >> 1) << 4));
}

// 親のセルの配り方(隣の列の傾きから。s_placeAddress を親のセルにする)
PrCell PlaceCellOf(MrBlock parent, uint32_t parentCell) {
    const int32_t px = (int32_t)MrCellX(parentCell);
    const int32_t py = (int32_t)MrCellY(parentCell);
    const int32_t pz = (int32_t)MrCellZ(parentCell);
    const int64_t slopeX = PlaceSlopeAt(px, py, pz, 1, 0);
    const int64_t slopeZ = PlaceSlopeAt(px, py, pz, 0, 1);
    s_placeAddress = PageCellAddress(parent.page, parentCell);

    return PrMakeCell(false, slopeX, slopeZ, MR_CELL_VOLUME_MM3);
}

// --- エネルギー: 熱を子の熱容量の重みで(D-674。CPU の ChildEnergies)。和は親 × 8(最後の重みのある子が残り)---
int64_t PlaceChildEnergy(uint32_t parentAddress, uint32_t j, uint32_t local, uint32_t t, int64_t chemical) {
    s_placeAddress = parentAddress;
    const int64_t parentEnergy = g_cells[s_placeAddress].energy;
    int64_t parentChemical = 0;
    for (uint32_t i = 0; i < PR_MATERIAL_COUNT; ++i) {
        const uint32_t speciesId = g_cells[s_placeAddress].species[i];
        parentChemical += (int64_t)g_cells[s_placeAddress].amounts[i] * g_species[speciesId].h0;
    }

    const int64_t heat = PrSubS64((int64_t)RX_MICROJOULES_PER_MILLIJOULE * parentEnergy, parentChemical) * 8;
    const uint64_t ownCapacity = gs_placeWide[t];
    uint64_t capacitySum = 0;
    uint32_t last = 0;
    FX_UNROLL
    for (uint32_t k = 0; k < 8; ++k) {
        const uint64_t capacity = gs_placeWide[MrChildCell(local, k)];
        capacitySum += capacity;
        last = capacity > 0 ? k : last;
    }

    int64_t energy = parentEnergy;  // 熱容量が無い(SAME)
    if (capacitySum != 0 && j != last) {
        const int64_t childHeat = FxMulDivS64(heat, ownCapacity, capacitySum);
        energy = FxDivS64(chemical + childHeat, (int64_t)RX_MICROJOULES_PER_MILLIJOULE);
    }

    // 熱容量を全部のスレッドが読み終えてから、同じ欄にエネルギーを書く
    GroupMemoryBarrierWithGroupSync();
    gs_placeWide[t] = (uint64_t)(capacitySum != 0 && j == last ? 0 : energy);
    GroupMemoryBarrierWithGroupSync();
    if (capacitySum == 0 || j != last)
        return energy;

    int64_t given = 0;
    FX_UNROLL
    for (uint32_t k = 0; k < 8; ++k)
        given += (int64_t)gs_placeWide[MrChildCell(local, k)];

    return PrSubS64(parentEnergy * 8, given);
}

// 細かくする段の置かれ方(グループの全部のスレッドが呼ぶ。1 スレッド = 子のセル t。子の頁のセル t は親と同じ数で写してある)
void PlaceRefineChildren(MrBlock parent, uint32_t parentSlot, uint32_t octant, uint32_t childPage, uint32_t t) {
    // --- 1. 親のブロックのセル t の詰まり(子に覆われたセルは無い)。覆いは索引の見出しから(親を覆うのはこの段の後)---
    int32_t fullness = -1;
    if (g_blocks[parentSlot].children[MrOctantOfCell(t)] == MR_NO_BLOCK)
        fullness = (int32_t)PlaceFullnessAt(PageCellAddress(parent.page, t));

    gs_placeNarrow[t] = (uint32_t)fullness;
    GroupMemoryBarrierWithGroupSync();

    // --- 2. 子 t の配り方と空きの体積(気体の配り直しは同じ親の子 8 つの空きの和)---
    const uint2 index = PlaceChildIndex(t);
    const uint32_t j = index.x;
    const uint32_t local = index.y;
    const uint32_t parentAddress = PageCellAddress(parent.page, MrOctantCell(octant, local));
    const PrCell cell = PlaceCellOf(parent, MrOctantCell(octant, local));
    const PrColumn column = PrMakeColumn(cell, j & 1u, j >> 2);
    const uint64_t ownOpen = PrChildWeightsOf(cell, column, j).openMm3;

    // 詰まりを全部のスレッドが読み終えてから、同じ欄に空きの体積を書く
    GroupMemoryBarrierWithGroupSync();
    gs_placeNarrow[t] = (uint32_t)ownOpen;
    GroupMemoryBarrierWithGroupSync();

    uint64_t openSum = 0;
    FX_UNROLL
    for (uint32_t k = 0; k < 8; ++k)
        openSum += gs_placeNarrow[MrChildCell(local, k)];

    // --- 3. 子のセルの成分を書き(親の並びのまま、量 0 の成分を消して詰める = 同じ中身なら同じ並び)、熱容量と化学のエネルギーを数える ---
    const uint32_t address = PageCellAddress(childPage, t);
    if (cell.settled)
        g_cells[address] = RxMakeEmptyCell(0);

    s_placeAddress = parentAddress;
    uint64_t capacity = 0;
    int64_t chemical = 0;
    uint32_t written = 0;
    for (uint32_t m = 0; m < PR_MATERIAL_COUNT; ++m) {
        const int64_t mass = PlaceChildMass(cell, column, m, j, local, ownOpen, openSum);
        if (mass <= 0)
            continue;

        const uint32_t speciesId = g_cells[s_placeAddress].species[m];
        capacity += (uint64_t)mass * (uint64_t)g_species[speciesId].heatCapacity;
        chemical += mass * g_species[speciesId].h0;
        if (cell.settled) {
            g_cells[address].species[written] = speciesId;
            g_cells[address].amounts[written] = (uint64_t)mass;
        }

        written += 1;
    }

    gs_placeWide[t] = capacity;
    GroupMemoryBarrierWithGroupSync();

    // --- 4. エネルギー(寄せないセルは写したまま)---
    const int64_t energy = PlaceChildEnergy(parentAddress, j, local, t, chemical);
    if (!cell.settled)
        return;

    g_cells[address].energy = energy;
    g_cells[address].speciesCount = written;
}

#endif  // BICAMERAL_MULTIRES_REFINE_PLACE_HLSLI
