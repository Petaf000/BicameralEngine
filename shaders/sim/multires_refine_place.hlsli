// multires_refine_place.hlsli — 世界の木を細かくする時、寄せるセルの中身を「置かれ方で配る」GPU の段(T-1188。D-664・D-674・D-679・
// ADR-0469 の 2-1・20 §10.5・17 §3.2)。multires_graph.hlsl の RefineLevel が、親の八分の一の 4³ セルを子へ同じ数で写した後に呼ぶ
// (溢れを使わない変種だけ。溢れを使う世界は SAME のまま = CPU と同じ)。
// CPU リファレンスは engine/src/sim/multires_refine_cells.cpp の PlaceTreeCell・PlaceRefineOctantCells(同じ式・同じ順でビット単位で同じ。
// tests/gpu_tree_refine_placement_test.cpp)。回しは sim/placement_refine.hlsli、1 つずつの式は common/state_field.hlsli(CPU と 1 つの源)。
// データの流れ: 親の頁のセル(成分 µmol・エネルギー mJ)と物質の表(g_species の相の印・粒の詰まり・モル体積・比熱・H0)
//   → グループ共有に親のブロックの詰まり(1 スレッド = 親のセル t)→ 子のセル t(1 スレッド = 子のセル t)の成分とエネルギーを子の頁へ。
// 「数の空間」: 子 1 つの器 = レベル 0 のセルの体積 MR_CELL_VOLUME_MM3・配る量 = 親の数 × 8(子のレベルの単位)。
// 寄せないセル(気体だけ・詰まり切った・据わる固体を含む・凝縮相が無い)は写したまま触らない。端数の枠は SAME のまま。
// 隣は親のブロックの中と、ブロックの外は隣の写し(要求の処理の割り当ての後に sim/multires_refine_halo.hlsl が u6 の g_overflowBase から書く。
// 刻みの前の、索引で同じレベルの本物のブロックの詰まり。T-1189・common/multires_refine_halo.hlsli)。子に覆われたセルは写しの覆う子のセル 2³ の
// 和から(ブロックの中のものは写しの中を向く面。T-1314)。同じレベルのブロックが無い所は無いと読む(横 → 傾き 0・下 → 詰まり切る・上 → 空。
// T-1176 の仮のまま)。
// 書き換えた構造体の配列を動的な添字で読まない(T-0397): 親のセルの成分は毎回バッファから読み、子のセルはバッファへ直接書く。
// 64bit の引き算と同じ値どうしの比べを並べない(T-1078・T-1159): 差は 32bit の 2 つに分けて借りを手で回す(PrSubS64)。
// 成分ごとの層(T-1240 の 4 段目。CPU の MaterialsOf・ChildCells): 層を持つ成分は「層の分 = 沈む側(凝縮相)」と「残り = 物質の置かれ方」の
// 2 つの物質に分けて配り、子の層 = 子へ配った層の分。層を使う世界(層の置き場の印 MR_LAYER_FLAG_GRAIN)では粒の残りは舞う粒(空きに同じ濃さ)。
// 層の無いセル(層を使わない世界)は前と同じ 1 つの物質のまま(物質 m = 成分 m)= ビット単位で同じ。層は u4(sim/multires_layer_access.hlsli)から
// 今のセルと結んで読む(s_placeLayer = セルの層の組の始まり。無ければ MR_LAYER_NO_PAGE)。
#ifndef BICAMERAL_MULTIRES_REFINE_PLACE_HLSLI
#define BICAMERAL_MULTIRES_REFINE_PLACE_HLSLI

#include "common/multires_refine_halo.hlsli"
#include "common/state_field.hlsli"

static uint32_t s_placeAddress;  // 今のスレッドが読む親のセルの番地(g_cells)
static uint32_t s_placeHalo;     // 今の段の隣の写しの位置(MR_HALO_NONE なら写しを読まない)
static uint32_t s_placeOctant;   // 今の段の親の八分の一(写しの面の向き)
static uint32_t
    s_placeLayer;          // 今のスレッドが読む親のセルの層の組の始まり(g_layers。無ければ MR_LAYER_NO_PAGE。T-1240)
static bool s_placeSplit;  // 今のセルの物質を層の分と残りに分けるか(層がある・層を使う世界。偽なら物質 m = 成分 m)

// 隣の写しの番地(u6。並びは common/multires_refine_halo.hlsli)
uint32_t RefineHaloAddress(uint32_t halo, uint32_t h) {
    return g_overflowBase + (halo * MR_HALO_CELLS) + h;
}

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

// --- 層の分と残り(T-1240。CPU の MaterialsOf)---
// 物質の並びはセルを読む用意(PlaceUseCell)の時に 1 回だけ作り、32bit の語に詰めて持つ(T-1305: 前は物質を読むたびに成分と層の組を
// 前から数え直し、層のある置かれ方の要求の処理が層なしの 4〜9 倍だった)。局所の配列は持たない(T-0397)。
static const uint32_t PLACE_PART_WHOLE = 0;    // 前と同じ 1 つの物質
static const uint32_t PLACE_PART_SETTLED = 1;  // 層の分(沈む側)
static const uint32_t PLACE_PART_REST = 2;     // 残り(層を使う世界の粒は舞う粒)

static const uint32_t PLACE_ENTRY_PART_SHIFT = 3;  // 物質の印の語: 成分の位置 3 bit・分 2 bit・舞う粒 1 bit
static const uint32_t PLACE_ENTRY_FLOATS = 1u << 5;
static const uint32_t PLACE_ENTRY_BITS = 8;  // 物質 1 つの印の幅(語 1 つに 4 つ。物質は成分 8 × 2 = 16 まで)

static uint32_t s_placeLayerSlots;  // 成分 i の層の組の番号 + 1(4 bit ずつ。0 = 層の組が無い)
static uint32_t s_placeMaterials;   // 今のセルの物質の数
static uint32_t s_placeMap0;        // 物質 0〜3 の印
static uint32_t s_placeMap1;        // 物質 4〜7 の印
static uint32_t s_placeMap2;        // 物質 8〜11 の印
static uint32_t s_placeMap3;        // 物質 12〜15 の印

// 今のセルの成分 i の層(今の量と結ぶ。層が無ければ 0)
uint64_t PlaceLayerOf(uint32_t i) {
    if (s_placeLayer == MR_LAYER_NO_PAGE)
        return 0;

    const uint32_t slot = (s_placeLayerSlots >> (i * 4u)) & 0xFu;
    if (slot == 0)
        return 0;

    const uint32_t address = s_placeLayer + ((slot - 1u) * MR_LAYER_ENTRY_WORDS);
    const uint64_t raw = FX_U64(g_layers[address + 2], g_layers[address + 1]);
    const uint64_t amount = g_cells[s_placeAddress].amounts[i];

    return MrLayerLess(amount, raw) ? amount : raw;
}

// 成分 i が層を使う世界の粒か(残りは舞う粒)
bool PlaceFloats(uint32_t i) {
    return LayerGrainFloats() && PlaceMaterialOf(g_cells[s_placeAddress].species[i]).x == SF_SETTLE_GRAIN;
}

// 成分 i の物質の数(層の分・残り。0 の分は物質にしない)
uint32_t PlacePartCount(uint32_t i) {
    const uint64_t layer = PlaceLayerOf(i);
    const bool hasLayer = ((uint32_t)layer | (uint32_t)(layer >> 32)) != 0;
    if (!hasLayer && !PlaceFloats(i))
        return 1;

    const bool hasRest = MrLayerLess(layer, g_cells[s_placeAddress].amounts[i]);

    return (hasLayer ? 1u : 0u) + (hasRest ? 1u : 0u);
}

// 成分ごとの層の組の番号を探す(層の組を 1 回だけ読む。0 でない組のうち、同じ物質の最後の組 = 前の PlaceLayerOf と同じ)
void PlaceFindLayerSlots(uint32_t speciesCount) {
    FX_UNROLL
    for (uint32_t k = 0; k < RX_MAX_CELL_SPECIES; ++k) {
        const uint32_t address = s_placeLayer + (k * MR_LAYER_ENTRY_WORDS);
        const bool nonzero = (g_layers[address + 1] | g_layers[address + 2]) != 0;
        const uint32_t species = g_layers[address];
        for (uint32_t i = 0; i < speciesCount; ++i) {
            if (!nonzero || g_cells[s_placeAddress].species[i] != species)
                continue;

            const uint32_t shift = i * 4u;
            s_placeLayerSlots = (s_placeLayerSlots & ~(0xFu << shift)) | ((k + 1u) << shift);
        }
    }
}

// 物質 m の印を書く(語は 0 から足すだけ)
void PlaceSetMaterial(uint32_t m, uint32_t entry) {
    const uint32_t bits = entry << ((m & 3u) * PLACE_ENTRY_BITS);
    if (m < 4)
        s_placeMap0 |= bits;
    else if (m < 8)
        s_placeMap1 |= bits;
    else if (m < 12)
        s_placeMap2 |= bits;
    else
        s_placeMap3 |= bits;
}

// 物質 m の印(物質の数の外は成分 0 の 1 つの物質 = 前の PlacePartOf の既定)
uint32_t PlaceMaterialEntry(uint32_t m) {
    if (m >= s_placeMaterials)
        return 0;

    uint32_t word = s_placeMap3;
    if (m < 4)
        word = s_placeMap0;
    else if (m < 8)
        word = s_placeMap1;
    else if (m < 12)
        word = s_placeMap2;

    return (word >> ((m & 3u) * PLACE_ENTRY_BITS)) & 0xFFu;
}

// 今のセル(s_placeAddress・s_placeLayer)を読む用意: 層の分けが要るか・物質の並び
void PlaceUseCell(uint32_t address, uint32_t layerBase) {
    s_placeAddress = address;
    s_placeLayer = layerBase;
    s_placeSplit = layerBase != MR_LAYER_NO_PAGE || LayerGrainFloats();
    s_placeLayerSlots = 0;
    s_placeMap0 = 0;
    s_placeMap1 = 0;
    s_placeMap2 = 0;
    s_placeMap3 = 0;

    const uint32_t speciesCount = g_cells[address].speciesCount;
    s_placeMaterials = speciesCount;
    if (!s_placeSplit)
        return;

    if (layerBase != MR_LAYER_NO_PAGE)
        PlaceFindLayerSlots(speciesCount);

    // --- 成分ごとに [層の分][残り](0 の分は物質にしない。CPU の MaterialsOf と同じ順)---
    uint32_t m = 0;
    for (uint32_t i = 0; i < speciesCount; ++i) {
        const uint64_t layer = PlaceLayerOf(i);
        const bool hasLayer = ((uint32_t)layer | (uint32_t)(layer >> 32)) != 0;
        const bool floats = PlaceFloats(i);
        const uint32_t count = PlacePartCount(i);
        for (uint32_t k = 0; k < count; ++k) {
            uint32_t part = PLACE_PART_REST;
            if (!hasLayer && !floats)
                part = PLACE_PART_WHOLE;
            else if (hasLayer && k == 0)
                part = PLACE_PART_SETTLED;

            PlaceSetMaterial(m + k, i | (part << PLACE_ENTRY_PART_SHIFT) | (floats ? PLACE_ENTRY_FLOATS : 0u));
        }

        m += count;
    }

    s_placeMaterials = m;
}

// 物質 m の成分の位置(x)と分(y = PLACE_PART_*)
uint2 PlacePartOf(uint32_t m) {
    if (!s_placeSplit)
        return uint2(m, PLACE_PART_WHOLE);

    const uint32_t entry = PlaceMaterialEntry(m);

    return uint2(entry & 7u, (entry >> PLACE_ENTRY_PART_SHIFT) & 3u);
}

uint32_t PlaceMaterialCount() {
    return s_placeMaterials;
}

// 物質 m の置かれ方(層の分は沈む側: 気体なら粒として。層を使う世界の粒の残りは気体 = 舞う粒)
uint3 PlaceMaterialAt(uint32_t m) {
    const uint2 part = PlacePartOf(m);
    uint3 material = PlaceMaterialOf(g_cells[s_placeAddress].species[part.x]);
    const bool floats = s_placeSplit && (PlaceMaterialEntry(m) & PLACE_ENTRY_FLOATS) != 0;
    if (part.y == PLACE_PART_SETTLED && material.x == SF_SETTLE_GAS)
        material.x = SF_SETTLE_GRAIN;
    else if (part.y == PLACE_PART_REST && floats)
        material.x = SF_SETTLE_GAS;

    return material;
}

// a − b(a ≥ b。32bit の 2 つに分けて借りを手で回す。T-1078)
uint64_t PlaceSubU64(uint64_t a, uint64_t b) {
    const uint32_t low = (uint32_t)a - (uint32_t)b;
    const uint32_t borrow = (uint32_t)a < (uint32_t)b ? 1u : 0u;
    const uint32_t high = (uint32_t)(a >> 32) - (uint32_t)(b >> 32) - borrow;

    return FX_U64(high, low);
}

// 物質 m の親の量(子のレベルの単位 = 親の数 × 8)
int64_t PlaceParentMassAt(uint32_t m) {
    const uint2 part = PlacePartOf(m);
    const uint64_t amount = g_cells[s_placeAddress].amounts[part.x];
    uint64_t mass = amount;
    if (part.y == PLACE_PART_SETTLED)
        mass = PlaceLayerOf(part.x);
    else if (part.y == PLACE_PART_REST)
        mass = PlaceSubU64(amount, PlaceLayerOf(part.x));

    return (int64_t)mass * 8;
}

#define PR_MATERIAL_COUNT PlaceMaterialCount()
#define PR_MATERIAL(m) PlaceMaterialAt(m)
#define PR_MATERIAL_MOLAR_VOLUME(m) g_species[g_cells[s_placeAddress].species[PlacePartOf(m).x]].molarVolume
#define PR_PARENT_MASS(m) PlaceParentMassAt(m)
#include "sim/placement_refine.hlsli"

// グループ共有 6 KiB(HW の Work Graph の再帰のノードの 1 グループの重さを小さく保つ。12 KiB の 4 つの欄では RTX の
// gpu_multires〔置かれ方を使わない木〕が DEVICE_HUNG になった。T-1188)。1 つの欄を段ごとに使い回し、入れ替わる所にバリアを置く:
//   gs_placeNarrow = 親のブロックのセルの詰まり(Q16。器 = 子 8 つ。無いセルは −1 を符号無しで)→ 子のセルの空きの体積(mm³)
//   gs_placeWide   = 子のセルの熱容量(nJ/K)→ 子のセルのエネルギー(mJ。残りを取る子は 0。符号付きを符号無しで)
// 詰まり・空きは 32bit に収まる(詰まりは 0〜2^16・空きは子の器以下)
groupshared uint32_t gs_placeNarrow[MR_BLOCK_CELLS];
groupshared uint64_t gs_placeWide[MR_BLOCK_CELLS];

// --- 隣の読み方(CPU の Present・FullnessAt・SurfaceAt・SlopeAt)---

// 親のセルの詰まり(器 = 子 8 つ。CPU の FullnessAt。層の分は沈む側。layerBase = セルの層の組の始まり)
int64_t PlaceFullnessAt(uint32_t address, uint32_t layerBase) {
    PlaceUseCell(address, layerBase);
    SfSettleSum sum = SfMakeSettleSum();
    for (uint32_t m = 0; m < PR_MATERIAL_COUNT; ++m)
        sum = PrAddMaterial(sum, m, PR_PARENT_MASS(m));

    return SfSettleFullnessQ16(SfSettledVolume(sum), MR_CELL_VOLUME_MM3 * 8, sum.anchored);
}

// 詰まり(無ければ MR_HALO_ABSENT)。ブロックの中はグループ共有、外は隣の写し
int32_t PlaceFullnessRaw(int32_t x, int32_t y, int32_t z) {
    const int32_t edge = (int32_t)MR_BLOCK_EDGE;
    if (x >= 0 && y >= 0 && z >= 0 && x < edge && y < edge && z < edge)
        return (int32_t)gs_placeNarrow[MrCellIndex((uint32_t)x, (uint32_t)y, (uint32_t)z)];

    if (s_placeHalo == MR_HALO_NONE)
        return MR_HALO_ABSENT;

    return (int32_t)g_treeWords[RefineHaloAddress(s_placeHalo, MrHaloIndexOf(x, y, z, s_placeOctant))];
}

// 親のブロックの子に覆われたセル t の詰まり(隣の写しの中を向く面。CPU の FullnessAt の覆われたセル。T-1314)
int32_t PlaceCoveredFullness(uint32_t t) {
    const int32_t x = (int32_t)MrCellX(t);
    const int32_t y = (int32_t)MrCellY(t);
    const int32_t z = (int32_t)MrCellZ(t);
    if (s_placeHalo == MR_HALO_NONE || !MrHaloInWindow(x, y, z, s_placeOctant))
        return MR_HALO_ABSENT;

    return (int32_t)g_treeWords[RefineHaloAddress(s_placeHalo, MrHaloIndexOf(x, y, z, s_placeOctant))];
}

bool PlacePresent(int32_t x, int32_t y, int32_t z) {
    return PlaceFullnessRaw(x, y, z) >= 0;
}

int64_t PlaceFullnessOf(int32_t x, int32_t y, int32_t z) {
    return PlaceFullnessRaw(x, y, z);
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

// セルの層の組の始まり(層の頁 layerPage のセル index。層の頁が無ければ MR_LAYER_NO_PAGE)
uint32_t PlaceLayerBase(uint32_t layerPage, uint32_t index) {
    if (layerPage == MR_LAYER_NO_PAGE)
        return MR_LAYER_NO_PAGE;

    return MrLayerEntryAddress(LayerPageCount(), LayerCapacity(), layerPage, index, 0);
}

// 親のセルの配り方(隣の列の傾きから。今のセルを親のセルにする)
PrCell PlaceCellOf(MrBlock parent, uint32_t parentCell, uint32_t parentLayerPage) {
    const int32_t px = (int32_t)MrCellX(parentCell);
    const int32_t py = (int32_t)MrCellY(parentCell);
    const int32_t pz = (int32_t)MrCellZ(parentCell);
    const int64_t slopeX = PlaceSlopeAt(px, py, pz, 1, 0);
    const int64_t slopeZ = PlaceSlopeAt(px, py, pz, 0, 1);
    PlaceUseCell(PageCellAddress(parent.page, parentCell), PlaceLayerBase(parentLayerPage, parentCell));

    return PrMakeCell(false, slopeX, slopeZ, MR_CELL_VOLUME_MM3);
}

// --- エネルギー: 熱を子の熱容量の重みで(D-674。CPU の ChildEnergies)。和は親 × 8(最後の重みのある子が残り)---
int64_t PlaceChildEnergy(uint32_t parentAddress, uint32_t j, uint32_t local, uint32_t t, int64_t chemical) {
    s_placeAddress = parentAddress;
    const int64_t parentEnergy = g_cells[s_placeAddress].energy;
    int64_t parentChemical = 0;
    // 成分ごと(物質ではない。CPU の ChildEnergies。T-1305 の前は物質の数まで回し、層のあるセルで成分の並びの外を読んでいた)
    for (uint32_t i = 0; i < g_cells[s_placeAddress].speciesCount; ++i) {
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

// 細かくする段の置かれ方(グループの全部のスレッドが呼ぶ。1 スレッド = 子のセル t。子の頁のセル t は親と同じ数で写してある)。
// halo = この段の隣の写しの位置(T-1189。MR_HALO_NONE なら外は無い)。parentLayerPage・childLayerPage = 親・子の頁の層の頁
// (無ければ MR_LAYER_NO_PAGE。子の層の頁のセル t は親のセルの層を写してある。寄せたセルは配った層の分で書き直す。T-1240)
void PlaceRefineChildren(MrBlock parent, uint32_t parentSlot, uint32_t octant, uint32_t childPage, uint32_t halo,
                         uint32_t parentLayerPage, uint32_t childLayerPage, uint32_t t) {
    s_placeHalo = halo;
    s_placeOctant = octant;

    // --- 1. 親のブロックのセル t の詰まり。覆いは索引の見出しから(親を覆うのはこの段の後)。子に覆われたセルは隣の写しの
    // 中を向く面(覆う子のセル 2³ の和。T-1314)。窓の外・写しが無ければ無い ---
    int32_t fullness = -1;
    if (g_blocks[parentSlot].children[MrOctantOfCell(t)] == MR_NO_BLOCK)
        fullness = (int32_t)PlaceFullnessAt(PageCellAddress(parent.page, t), PlaceLayerBase(parentLayerPage, t));
    else
        fullness = PlaceCoveredFullness(t);

    gs_placeNarrow[t] = (uint32_t)fullness;
    GroupMemoryBarrierWithGroupSync();

    // --- 2. 子 t の配り方と空きの体積(気体の配り直しは同じ親の子 8 つの空きの和)---
    const uint2 index = PlaceChildIndex(t);
    const uint32_t j = index.x;
    const uint32_t local = index.y;
    const uint32_t parentAddress = PageCellAddress(parent.page, MrOctantCell(octant, local));
    const PrCell cell = PlaceCellOf(parent, MrOctantCell(octant, local), parentLayerPage);
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

    // --- 3. 子のセルの成分を書き(親の並びのまま、量 0 の成分を消して詰める = 同じ中身なら同じ並び)、熱容量と化学のエネルギーを数える。
    //     物質(層の分と残り)は成分へ足し戻し、層の分の和が子の層(CPU の ChildCells。T-1240)---
    const uint32_t address = PageCellAddress(childPage, t);
    const uint32_t childLayer = PlaceLayerBase(childLayerPage, t);
    if (cell.settled)
        g_cells[address] = RxMakeEmptyCell(0);

    if (cell.settled && childLayer != MR_LAYER_NO_PAGE)
        ClearLayerWords(childLayer);

    PlaceUseCell(parentAddress, PlaceLayerBase(parentLayerPage, MrOctantCell(octant, local)));
    uint64_t capacity = 0;
    int64_t chemical = 0;
    uint32_t written = 0;
    uint32_t m = 0;
    for (uint32_t i = 0; i < g_cells[s_placeAddress].speciesCount; ++i) {
        const uint32_t parts = s_placeSplit ? PlacePartCount(i) : 1u;
        int64_t mass = 0;
        int64_t layerMass = 0;
        for (uint32_t k = 0; k < parts; ++k) {
            const int64_t share = PlaceChildMass(cell, column, m + k, j, local, ownOpen, openSum);
            mass += share;
            if (s_placeSplit && PlacePartOf(m + k).y == PLACE_PART_SETTLED)
                layerMass += share;
        }

        m += parts;
        if (mass <= 0)
            continue;

        const uint32_t speciesId = g_cells[s_placeAddress].species[i];
        capacity += (uint64_t)mass * (uint64_t)g_species[speciesId].heatCapacity;
        chemical += mass * g_species[speciesId].h0;
        if (cell.settled) {
            g_cells[address].species[written] = speciesId;
            g_cells[address].amounts[written] = (uint64_t)mass;
        }

        if (cell.settled && childLayer != MR_LAYER_NO_PAGE && layerMass > 0) {
            const uint32_t entry = childLayer + (written * MR_LAYER_ENTRY_WORDS);
            g_layers[entry] = speciesId;
            g_layers[entry + 1] = (uint32_t)(uint64_t)layerMass;
            g_layers[entry + 2] = (uint32_t)((uint64_t)layerMass >> 32);
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
