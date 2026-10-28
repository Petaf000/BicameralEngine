// multires_refine_cells.cpp — 世界の木を細かくする時のセルの中身を「置かれ方で配る」CPU リファレンス(T-1176。D-664・D-674・ADR-0469 の 2-1・
// 20 §10.5・17 §3.2)。multires_tree.cpp の RefineRequestLevel が、親の八分の一の 4³ セルを子 2³ ずつへ写した後に呼ぶ。
// データの流れ: 親のセル(成分 µmol・エネルギー mJ。親のレベルの単位)→ 物質の表(RxSpecies の相の印・モル体積)で sim::PlaceRefineCell
// (「数の空間」: 子の器 = レベル 0 のセルの体積 V0・配る量 = 親の数 × 8 = 子のレベルの単位)→ 子 8 つのセル(成分・エネルギー)。
// 寄せないセル(気体だけ・詰まり切った・据わる固体を含む・凝縮相が無い)は呼ぶ前の写し(SAME)のまま触らない = 今とビット単位で同じ。
// 端数(2^-64 単位)は子へ SAME のまま(1 単位未満なので形に効かない。保存は子 8 つの和 = 親 × 8 でビット単位)。
// エネルギーは子の温度が親と揃うように熱(µJ)を子の熱容量の重みで配る(D-674)。
// 傾きは同じ親のブロックの中の隣の列から(SfSurfaceSlopeQ16)。ブロックの外・子に覆われたセルは無いと読む(1 本目の仮。チケットの設計の詰め 6):
//   横が無い → 傾き 0・下が無い → 詰まり切る・上が無い → 空。1 刻みに 1 つの親は 1 つの要求だけが触るので、読む所は順番に依存しない。
// GPU は shaders/sim/multires_refine_place.hlsli(multires_graph.hlsl の RefineLevel が呼ぶ。T-1188。tests/gpu_tree_refine_placement_test.cpp)。
// 世界の箱(MakeLabBoxNest)は既定で使う(D-664)。MakeMultiresNest だけで作る木は使わない(SAME)。
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

        // --- 物質の表 → 置かれ方の物質(相の印・モル体積・粒の詰まり〔印のビット 8〜14。D-679。0 は 100 %〕)---
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
            if (material.kind == SF_SETTLE_GRAIN)
                material.packedFractionPct = RxSpeciesPackingPercent(species.flags);

            return material;
        }

        struct CellMaterials {
            std::vector<PlaceMaterial> materials;
            std::vector<int64_t> amounts;  // 子のレベルの単位(親の数 × 8)
        };

        CellMaterials MaterialsOf(std::span<const RxSpecies> table, const RxCell& cell) {
            CellMaterials result;
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                result.materials.push_back(MaterialOf(table[cell.species[i]]));
                result.amounts.push_back((int64_t)cell.amounts[i] * 8);
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
            const RxCell cell = nest_detail::LoadCellAt(around.nest, around.slot,
                                                        MrCellIndex((uint32_t)x, (uint32_t)y, (uint32_t)z));
            const CellMaterials cellMaterials = MaterialsOf(around.table, cell);
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

        // --- 配った子のセル(成分は親の並びのまま、量 0 の成分を消して詰める = 同じ中身なら同じ並び)---
        std::array<RxCell, 8> ChildCells(std::span<const RxSpecies> table, const RxCell& parent,
                                         const PlacedChildren& placed) {
            std::array<RxCell, 8> children{};
            for (uint32_t j = 0; j < 8; ++j) {
                RxCell& child = children[j];
                child = RxMakeEmptyCell(0);
                const std::vector<int64_t>& amounts = placed.massMg[PlaceIndexOfMrChild(placed, j)];
                for (uint32_t i = 0; i < parent.speciesCount; ++i) {
                    if (amounts[i] <= 0)
                        continue;

                    child.species[child.speciesCount] = parent.species[i];
                    child.amounts[child.speciesCount] = (uint64_t)amounts[i];
                    child.speciesCount += 1;
                }
            }

            const std::array<int64_t, 8> energies = ChildEnergies(table, parent, children);
            for (uint32_t j = 0; j < 8; ++j)
                children[j].energy = energies[j];

            return children;
        }

    }  // namespace

    std::optional<PlacedChildren> PlaceTreeCell(const MultiresNest& nest, uint32_t parentSlot, uint32_t parentCell) {
        const std::span<const RxSpecies> table = nest.refineCells.species;
        if (table.empty() || nest.wideCells || MrIsUniform(nest.blocks[parentSlot]))
            return std::nullopt;

        // --- 寄せるか(寄せないセルは隣を読まずに抜ける)---
        const RxCell cell = nest_detail::LoadCellAt(nest, parentSlot, parentCell);
        CellMaterials cellMaterials = MaterialsOf(table, cell);
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
                const std::array<RxCell, 8> children = ChildCells(nest.refineCells.species, parent, *placed);
                for (uint32_t j = 0; j < 8; ++j)
                    StorePageCell(nest, childPage, MrChildCell(local, j), children[j]);
            }
        }

    }  // namespace nest_detail

    void EnableRefineCellPlacement(MultiresNest& nest, const BakedReactionTable& table) {
        nest.refineCells.species = table.species;

        // 欄(SF_FLAG_PLACE_*)は同じ置かれ方の重みで配る(親のセルを覆う前に呼ばれるので、セルと同じ答え)
        nest.refinePlacement = [](const MultiresNest& owner, uint32_t parentSlot, uint32_t parentCell,
                                  NestChildWeights& weights) {
            const std::optional<PlacedChildren> placed = PlaceTreeCell(owner, parentSlot, parentCell);
            if (!placed)
                return false;

            std::vector<PlaceMaterial> materials;
            const RxCell cell = nest_detail::LoadCellAt(owner, parentSlot, parentCell);
            for (uint32_t i = 0; i < cell.speciesCount; ++i)
                materials.push_back(MaterialOf(owner.refineCells.species[cell.species[i]]));

            const PlaceWeights placeWeights = PlaceChildWeights(materials, *placed, MR_CELL_VOLUME_MM3);
            for (uint32_t j = 0; j < 8; ++j) {
                const uint32_t index = PlaceIndexOfMrChild(*placed, j);
                weights.condensed[j] = placeWeights.condensed[index];
                weights.open[j] = placeWeights.open[index];
            }

            return true;
        };
    }

}  // namespace bicameral::sim
