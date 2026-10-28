// multires_refine_cells.cpp — 世界の木を細かくする時のセルの中身を「置かれ方で配る」CPU リファレンス(T-1176。D-664・D-674・ADR-0469 の 2-1・
// 20 §10.5・17 §3.2)。multires_tree.cpp の RefineRequestLevel が、親の八分の一の 4³ セルを子 2³ ずつへ写した後に呼ぶ。
// データの流れ: 親のセル(成分 µmol・エネルギー mJ。親のレベルの単位)→ 物質の表(RxSpecies の相の印・モル体積)で sim::PlaceRefineCell
// (「数の空間」: 子の器 = レベル 0 のセルの体積 V0・配る量 = 親の数 × 8 = 子のレベルの単位)→ 子 8 つのセル(成分・エネルギー)。
// 寄せないセル(気体だけ・詰まり切った・据わる固体を含む・凝縮相が無い)は呼ぶ前の写し(SAME)のまま触らない = 今とビット単位で同じ。
// 端数(2^-64 単位)は子へ SAME のまま(1 単位未満なので形に効かない。保存は子 8 つの和 = 親 × 8 でビット単位)。
// エネルギーは子の温度が親と揃うように熱(µJ)を子の熱容量の重みで配る(D-674)。
// 傾きは同じ親のブロックの中の隣の列から(SfSurfaceSlopeQ16)。ブロックの外・子に覆われたセルは無いと読む(1 本目の仮。チケットの設計の詰め 6):
//   横が無い → 傾き 0・下が無い → 詰まり切る・上が無い → 空。1 刻みに 1 つの親は 1 つの要求だけが触るので、読む所は順番に依存しない。
// GPU(shaders/sim/multires_graph.hlsl の RefineLevel)はまだ SAME(T-1188)。それまで MultiresNest::refineCells は既定で空(使わない)。
// 成分ごとの層(T-1220。ADR-0481 の 5 の 4・ADR-0489): 層を持つ成分は「層 = 沈む側(凝縮相)」と「残り = 物質の置かれ方」の 2 つの物質に分けて配り、
// 子の層 = 子へ配った層の分(子の層の和 = 親の層 × 8・子の量の和 = 親 × 8)。層を使う世界(EnableCellLayers)では粒の残りは舞う粒(空きに同じ濃さ)。
// 層の無いセル(層を使わない世界)は前と同じ 1 つの物質のまま = ビット単位で同じ。
#include <algorithm>

#include "sim/multires_nest.h"
#include "sim/multires_nest_internal.h"
#include "sim/placement_refine.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;
using namespace bicameral::state_field;

namespace bicameral::sim {

    // レベル 0 のセルの体積(mm³)は気体の器の体積(cell_view.hlsli。pL = 10^-6 mm³)と同じ
    static_assert(MR_CELL_VOLUME_MM3 * 1000000 == CV_CELL_VOLUME_LEVEL_UNITS);

    namespace {

        // --- 物質の表 → 置かれ方の物質(相の印・モル体積。粒の詰まりは表に値が無いので 0 = 100 %。仮・Q173)---
        PlaceMaterial MaterialOf(const RxSpecies& species) {
            PlaceMaterial material;
            material.kind = SF_SETTLE_GAS;
            if ((species.flags & RX_SPECIES_ANCHORED) != 0)
                material.kind = SF_SETTLE_ANCHORED;
            else if ((species.flags & RX_SPECIES_GRAIN) != 0)
                material.kind = SF_SETTLE_GRAIN;
            else if ((species.flags & RX_SPECIES_LIQUID) != 0)
                material.kind = SF_SETTLE_LIQUID;

            material.molarVolumeMm3PerMol = species.molarVolume;

            return material;
        }

        // セルの成分を置かれ方の物質に。層を持つ成分(と層を使う世界の粒)は層の分と残りの 2 つに分ける(T-1220)
        struct CellMaterials {
            std::vector<PlaceMaterial> materials;
            std::vector<int64_t> amounts;      // 子のレベルの単位(親の数 × 8)
            std::vector<uint32_t> components;  // [物質] セルの成分の位置
            std::vector<uint8_t> layerParts;   // [物質] 層の分なら 1

            void Push(const PlaceMaterial& material, uint64_t amount, uint32_t component, bool layerPart) {
                materials.push_back(material);
                amounts.push_back((int64_t)amount * 8);
                components.push_back(component);
                layerParts.push_back(layerPart ? 1 : 0);
            }
        };

        CellMaterials MaterialsOf(const MultiresNest& nest, std::span<const RxSpecies> table, const RxCell& cell,
                                  const MrCellLayers& layers) {
            CellMaterials result;
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                const PlaceMaterial material = MaterialOf(table[cell.species[i]]);
                const bool floats = nest.cellLayers.enabled && material.kind == SF_SETTLE_GRAIN;
                const uint64_t layer = layers.amounts[i];
                if (layer == 0 && !floats) {
                    result.Push(material, cell.amounts[i], i, false);  // 前と同じ 1 つの物質
                    continue;
                }

                // --- 層は沈む側(凝縮相)・残りは物質の置かれ方(層を使う世界の粒の残りは舞う粒 = 空きに同じ濃さ)---
                PlaceMaterial settled = material;
                if (settled.kind == SF_SETTLE_GAS)
                    settled.kind = SF_SETTLE_GRAIN;

                PlaceMaterial rest = material;
                if (floats)
                    rest.kind = SF_SETTLE_GAS;

                if (layer != 0)
                    result.Push(settled, layer, i, true);

                if (cell.amounts[i] > layer)
                    result.Push(rest, cell.amounts[i] - layer, i, false);
            }

            return result;
        }

        // --- 隣の読み方: 同じ親のブロックの中で、子に覆われていないセルの詰まり(Q16。器 = 子 8 つ)---
        struct Neighborhood {
            const MultiresNest& nest;
            std::span<const RxSpecies> table;
            uint32_t slot;
        };

        bool Present(const Neighborhood& around, int32_t x, int32_t y, int32_t z) {
            if (x < 0 || y < 0 || z < 0 || x >= (int32_t)MR_BLOCK_EDGE || y >= (int32_t)MR_BLOCK_EDGE ||
                z >= (int32_t)MR_BLOCK_EDGE)
                return false;

            const uint32_t index = MrCellIndex((uint32_t)x, (uint32_t)y, (uint32_t)z);

            return around.nest.blocks[around.slot].children[MrOctantOfCell(index)] == MR_NO_BLOCK;
        }

        int64_t FullnessAt(const Neighborhood& around, int32_t x, int32_t y, int32_t z) {
            const uint32_t index = MrCellIndex((uint32_t)x, (uint32_t)y, (uint32_t)z);
            const RxCell cell = nest_detail::LoadCellAt(around.nest, around.slot, index);
            const MrCellLayers layers = LoadNestCellLayers(around.nest, around.slot, index);
            const CellMaterials cellMaterials = MaterialsOf(around.nest, around.table, cell, layers);
            const SfSettleSum sum = PlaceSettleSum(cellMaterials.materials, cellMaterials.amounts);

            return SfSettleFullnessQ16(SfSettledVolume(sum), MR_CELL_VOLUME_MM3 * 8, sum.anchored);
        }

        // 列 (x, z) の段 y の表面(下が無い = 詰まり切る・上が無い = 空)
        int64_t SurfaceAt(const Neighborhood& around, int32_t x, int32_t y, int32_t z) {
            const int64_t below = Present(around, x, y - 1, z) ? FullnessAt(around, x, y - 1, z) : SF_SETTLE_FULL_Q16;
            const int64_t above = Present(around, x, y + 1, z) ? FullnessAt(around, x, y + 1, z) : 0;

            return SfColumnSurfaceQ16(below, FullnessAt(around, x, y, z), above);
        }

        // 軸 (dx, dz) の向きの傾き(両側の列が無ければ 0)
        int64_t SlopeAt(const Neighborhood& around, int32_t x, int32_t y, int32_t z, int32_t dx, int32_t dz) {
            const bool bothSides = Present(around, x - dx, y, z - dz) && Present(around, x + dx, y, z + dz);
            if (!bothSides)
                return 0;

            return SfSurfaceSlopeQ16(SurfaceAt(around, x - dx, y, z - dz), SurfaceAt(around, x, y, z),
                                     SurfaceAt(around, x + dx, y, z + dz), true);
        }

        // --- エネルギー: 熱を子の熱容量の重みで(D-674)。和は親 × 8(最後の重みのある子が残り)---
        std::array<int64_t, 8> ChildEnergies(std::span<const RxSpecies> table, const RxCell& parent,
                                             const std::array<RxCell, 8>& children) {
            std::array<int64_t, 8> energies{};
            energies.fill(parent.energy);

            int64_t chemical = 0;
            for (uint32_t i = 0; i < parent.speciesCount; ++i)
                chemical += (int64_t)parent.amounts[i] * table[parent.species[i]].h0;

            const int64_t heat = (((int64_t)RX_MICROJOULES_PER_MILLIJOULE * parent.energy) - chemical) * 8;
            std::array<uint64_t, 8> capacity{};
            uint64_t capacitySum = 0;
            uint32_t last = 0;
            for (uint32_t j = 0; j < 8; ++j) {
                for (uint32_t i = 0; i < children[j].speciesCount; ++i)
                    capacity[j] += children[j].amounts[i] * (uint64_t)table[children[j].species[i]].heatCapacity;

                capacitySum += capacity[j];
                last = capacity[j] > 0 ? j : last;
            }

            if (capacitySum == 0)
                return energies;  // 熱容量が無い(SAME)

            int64_t given = 0;
            for (uint32_t j = 0; j < 8; ++j) {
                if (j == last)
                    continue;

                int64_t childChemical = 0;
                for (uint32_t i = 0; i < children[j].speciesCount; ++i)
                    childChemical += (int64_t)children[j].amounts[i] * table[children[j].species[i]].h0;

                const int64_t childHeat = fx::FxMulDivS64(heat, capacity[j], capacitySum);
                energies[j] = fx::FxDivS64(childChemical + childHeat, (int64_t)RX_MICROJOULES_PER_MILLIJOULE);
                given += energies[j];
            }

            energies[last] = (parent.energy * 8) - given;

            return energies;
        }

        // --- 配った子のセル(成分は親の並びのまま、量 0 の成分を消して詰める = 同じ中身なら同じ並び)と子の層(層の分の和。T-1220)---
        struct ChildCellSet {
            std::array<RxCell, 8> cells{};
            std::array<MrCellLayers, 8> layers{};
        };

        ChildCellSet ChildCells(std::span<const RxSpecies> table, const RxCell& parent,
                                const CellMaterials& cellMaterials, const PlacedChildren& placed) {
            ChildCellSet children;
            for (uint32_t j = 0; j < 8; ++j) {
                RxCell& child = children.cells[j];
                child = RxMakeEmptyCell(0);
                const std::vector<int64_t>& amounts = placed.massMg[PlaceIndexOfMrChild(placed, j)];

                // --- 物質(層の分と残り)を成分へ足し戻す ---
                std::array<int64_t, RX_MAX_CELL_SPECIES> totals{};
                std::array<int64_t, RX_MAX_CELL_SPECIES> layerTotals{};
                for (size_t m = 0; m < cellMaterials.materials.size(); ++m) {
                    const uint32_t component = cellMaterials.components[m];
                    totals[component] += amounts[m];
                    if (cellMaterials.layerParts[m] != 0)
                        layerTotals[component] += amounts[m];
                }

                for (uint32_t i = 0; i < parent.speciesCount; ++i) {
                    if (totals[i] <= 0)
                        continue;

                    child.species[child.speciesCount] = parent.species[i];
                    child.amounts[child.speciesCount] = (uint64_t)totals[i];
                    children.layers[j].amounts[child.speciesCount] = (uint64_t)std::max<int64_t>(layerTotals[i], 0);
                    child.speciesCount += 1;
                }
            }

            const std::array<int64_t, 8> energies = ChildEnergies(table, parent, children.cells);
            for (uint32_t j = 0; j < 8; ++j)
                children.cells[j].energy = energies[j];

            return children;
        }

        // 親のセルの置かれ方の物質(層まで)
        CellMaterials ParentMaterials(const MultiresNest& nest, uint32_t parentSlot, uint32_t parentCell) {
            const RxCell cell = nest_detail::LoadCellAt(nest, parentSlot, parentCell);

            return MaterialsOf(nest, nest.refineCells.species, cell, LoadNestCellLayers(nest, parentSlot, parentCell));
        }

    }  // namespace

    std::optional<PlacedChildren> PlaceTreeCell(const MultiresNest& nest, uint32_t parentSlot, uint32_t parentCell) {
        const std::span<const RxSpecies> table = nest.refineCells.species;
        if (table.empty() || nest.wideCells || MrIsUniform(nest.blocks[parentSlot]))
            return std::nullopt;

        // --- 寄せるか(寄せないセルは隣を読まずに抜ける)---
        CellMaterials cellMaterials = ParentMaterials(nest, parentSlot, parentCell);
        const SfSettleSum sum = PlaceSettleSum(cellMaterials.materials, cellMaterials.amounts);
        if (!SfSettles(SfSettledVolume(sum), MR_CELL_VOLUME_MM3 * 8, sum.anchored))
            return std::nullopt;

        // --- 隣の列から傾き(x・z。重さの向きは −y)---
        const Neighborhood around{.nest = nest, .table = table, .slot = parentSlot};
        const auto x = (int32_t)MrCellX(parentCell);
        const auto y = (int32_t)MrCellY(parentCell);
        const auto z = (int32_t)MrCellZ(parentCell);
        PlaceCell place;
        place.massMg = std::move(cellMaterials.amounts);
        place.slopeXQ16 = SlopeAt(around, x, y, z, 1, 0);
        place.slopeZQ16 = SlopeAt(around, x, y, z, 0, 1);

        PlacedChildren placed = PlaceRefineCell(cellMaterials.materials, place, 2, 2, MR_CELL_VOLUME_MM3);
        if (!placed.settled)
            return std::nullopt;

        return placed;
    }

    namespace nest_detail {

        void PlaceRefineOctantCells(MultiresNest& nest, uint32_t parentSlot, uint32_t octant, uint32_t childPage) {
            if (nest.refineCells.species.empty() || nest.wideCells || MrIsUniform(nest.blocks[parentSlot]))
                return;

            for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                const uint32_t parentCell = MrOctantCell(octant, local);
                const std::optional<PlacedChildren> placed = PlaceTreeCell(nest, parentSlot, parentCell);
                if (!placed)
                    continue;

                const RxCell parent = LoadCellAt(nest, parentSlot, parentCell);
                const CellMaterials cellMaterials = ParentMaterials(nest, parentSlot, parentCell);
                const ChildCellSet children = ChildCells(nest.refineCells.species, parent, cellMaterials, *placed);
                for (uint32_t j = 0; j < 8; ++j)
                    StorePageCellWithLayers(nest, childPage, MrChildCell(local, j), children.cells[j],
                                            children.layers[j]);
            }
        }

    }  // namespace nest_detail

    void EnableCellLayers(MultiresNest& nest, const BakedReactionTable& table) {
        nest.cellLayers.enabled = true;
        nest.cellLayers.grain.assign(table.species.size(), 0);
        for (size_t i = 0; i < table.species.size(); ++i)
            nest.cellLayers.grain[i] = (table.species[i].flags & RX_SPECIES_GRAIN) != 0 ? 1 : 0;
    }

    void EnableRefineCellPlacement(MultiresNest& nest, const BakedReactionTable& table) {
        nest.refineCells.species = table.species;

        // 欄(SF_FLAG_PLACE_*)は同じ置かれ方の重みで配る(親のセルを覆う前に呼ばれるので、セルと同じ答え)
        nest.refinePlacement = [](const MultiresNest& owner, uint32_t parentSlot, uint32_t parentCell,
                                  NestChildWeights& weights) {
            const std::optional<PlacedChildren> placed = PlaceTreeCell(owner, parentSlot, parentCell);
            if (!placed)
                return false;

            const CellMaterials cellMaterials = ParentMaterials(owner, parentSlot,
                                                                parentCell);  // 層の分けまで同じ(T-1220)
            const PlaceWeights placeWeights = PlaceChildWeights(cellMaterials.materials, *placed, MR_CELL_VOLUME_MM3);
            for (uint32_t j = 0; j < 8; ++j) {
                const uint32_t index = PlaceIndexOfMrChild(*placed, j);
                weights.condensed[j] = placeWeights.condensed[index];
                weights.open[j] = placeWeights.open[index];
            }

            return true;
        };
    }

}  // namespace bicameral::sim
