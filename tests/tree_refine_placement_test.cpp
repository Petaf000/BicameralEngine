// tree_refine_placement_test.cpp — 世界の木を細かくする時のセルの中身の置かれ方(T-1176。D-664・D-674)の CPU の試験。
// 根 1 つ(空気の世界)に、粒の層(下は詰まり切る・途中は半分・上は空気だけ)と、x に沿って高さの変わる粒の斜面と、液体の溜まりを置き、
// 八分の一を細かくして、置かれ方を使う世界(EnableRefineCellPlacement)と使わない世界(今の SAME)を比べる:
//   - 物質ごとの和は親 × 8・エネルギーの和は親 × 8(ビット単位)
//   - 寄せないセル(気体だけ・詰まり切った)は SAME の世界とビット単位で同じ
//   - 半分のセルは粒が下の子に・気体は上の子に多い。斜面のセルは高い側の上の子に粒が多い
//   - 子の温度は親と揃う(熱容量の重み。丸めは子 1 つ 1 mJ 以下)
//   - 2 回走らせて状態の要約が同じ
//   - 層(T-1220): 粒が全部層なら層の無い世界と同じ子(子の層 = 子の粒の量)・層を使う世界では子の粒と層の和が親 × 8・
//     舞う粒だけのセルは沈めない
// 物質の表は試験の表(MakeSpeciesChangedTestTable)の 2 つの物質を、粒(ばらける固体)と液体に書き換えて使う(相の印とモル体積だけを読む)。
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "common/fixed.hlsli"
#include "core/log.h"
#include "core/singleton.h"
#include "multires_activity_scene.h"
#include "reaction_test_table.h"
#include "sim/multires_nest.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::reaction;
using namespace bicameral::sim;

namespace {

    int failureCount = 0;

    void Expect(bool condition, std::string_view what) {
        if (condition)
            return;

        failureCount += 1;
        Log(Channel::Sim, Level::Error, "失敗: {}", what);
    }

    constexpr uint32_t GRAIN_MOLAR_VOLUME = 22640;   // mm³/mol(石英の砂 60 g/mol ÷ 2650 kg/m³)
    constexpr uint32_t LIQUID_MOLAR_VOLUME = 18000;  // mm³/mol(水)
    constexpr int32_t ROOM_MILLIKELVIN = 300000;

    struct Materials {
        BakedReactionTable table;
        uint32_t grain = 0;
        uint32_t liquid = 0;
    };

    Materials MakeMaterials(const BakedReactionTable& baked) {
        Materials materials{.table = baked};
        materials.grain = baked.SpeciesId("ozone");
        materials.liquid = baked.SpeciesId("hydrogen");
        materials.table.species[materials.grain].flags = RX_SPECIES_GRAIN;
        materials.table.species[materials.grain].molarVolume = GRAIN_MOLAR_VOLUME;
        materials.table.species[materials.liquid].flags = RX_SPECIES_LIQUID;
        materials.table.species[materials.liquid].molarVolume = LIQUID_MOLAR_VOLUME;

        return materials;
    }

    // 器(レベル 0 のセル)の percent % を占める量(µmol)
    uint64_t AmountOfPercent(uint32_t percent, uint32_t molarVolume) {
        return (MR_CELL_VOLUME_MM3 / 100 * percent * 1000000) / molarVolume;
    }

    // --- 場面: 八分の一 0(x・y・z が 0〜3)の中に層・斜面・溜まり ---
    RxCell SceneCell(const Materials& materials, uint32_t index) {
        const uint32_t x = MrCellX(index);
        const uint32_t y = MrCellY(index);
        const uint32_t z = MrCellZ(index);
        std::vector<SpeciesAmount> amounts = {{.species = materials.table.SpeciesId("oxygen"), .amount = 1067000},
                                              {.species = materials.table.SpeciesId("nitrogen"), .amount = 4013000}};
        uint32_t grainPercent = 0;
        uint32_t liquidPercent = 0;
        if (z < 2) {
            // 粒の層: 下 2 段は詰まり切る・y = 2 は半分
            grainPercent = y < 2 ? 100 : (y == 2 ? 50 : 0);
        } else if (z == 2) {
            // 粒の斜面: y = 1 の段の詰まりが x とともに上がる
            grainPercent = y == 0 ? 100 : (y == 1 ? 20 + (x * 20) : 0);
        } else if (y < 2) {
            // 液体の溜まり(下は粒と液体の混ざり)
            liquidPercent = y == 0 ? 30 : 60;
            grainPercent = y == 0 ? 50 : 0;
        }

        if (grainPercent != 0)
            amounts.push_back(
                {.species = materials.grain, .amount = AmountOfPercent(grainPercent, GRAIN_MOLAR_VOLUME)});

        if (liquidPercent != 0)
            amounts.push_back(
                {.species = materials.liquid, .amount = AmountOfPercent(liquidPercent, LIQUID_MOLAR_VOLUME)});

        std::ranges::sort(amounts, {}, &SpeciesAmount::species);

        return MakeReactionCell(materials.table, amounts, ROOM_MILLIKELVIN);
    }

    struct World {
        MultiresNest nest;
        uint32_t root = MR_NO_BLOCK;
        uint32_t child = MR_NO_BLOCK;
    };

    World MakeWorld(const Materials& materials, bool placed, bool refined = true) {
        World world{.nest = MakeMultiresNest(test::MakeMultiresCapacity(materials.table, 16, 0, 16))};
        std::vector<RxCell> cells;
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
            cells.push_back(SceneCell(materials, index));

        world.root = PlaceRootBlock(world.nest, 0, 0, 0, cells);
        if (placed)
            EnableRefineCellPlacement(world.nest, materials.table);

        if (!refined)
            return world;

        const MrRequest refine = MrMakeRequest(MR_REQUEST_REFINE, 1, 1, 1, 1);
        SubmitRequests(world.nest, std::span(&refine, 1));
        ProcessRequests(world.nest);
        for (uint32_t slot = 0; slot < world.nest.capacity.worldBlocks; ++slot) {
            if (world.nest.blocks[slot].kind == MR_BLOCK_REAL && world.nest.blocks[slot].level == 1)
                world.child = slot;
        }

        return world;
    }

    uint64_t AmountOf(const RxCell& cell, uint32_t species) {
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            if (cell.species[i] == species)
                return cell.amounts[i];
        }

        return 0;
    }

    // 熱(µJ)と熱容量(nJ/K)
    struct Heat {
        int64_t heat = 0;
        uint64_t capacity = 0;
    };

    Heat HeatOf(const BakedReactionTable& table, const RxCell& cell) {
        Heat result{.heat = cell.energy * 1000};
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            result.heat -= (int64_t)cell.amounts[i] * table.species[cell.species[i]].h0;
            result.capacity += cell.amounts[i] * (uint64_t)table.species[cell.species[i]].heatCapacity;
        }

        return result;
    }

    // --- 1. 保存・SAME・温度 ---
    struct CellCheck {
        bool conserved = true;
        bool sameWhenNotPlaced = true;
        bool temperature = true;
        int placedCells = 0;
    };

    void CheckParentCell(const Materials& materials, const World& placed, const World& same, const World& probe,
                         uint32_t local, CellCheck& check) {
        const uint32_t parentCell = MrOctantCell(0, local);
        const RxCell parent = SceneCell(materials, parentCell);
        std::array<RxCell, 8> children{};
        bool sameAsSame = true;
        for (uint32_t j = 0; j < 8; ++j) {
            children[j] = LoadNestCell(placed.nest, placed.child, MrChildCell(local, j));
            sameAsSame &= MrSameCell(children[j], LoadNestCell(same.nest, same.child, MrChildCell(local, j)));
        }

        // --- 和は親 × 8 ---
        int64_t energy = 0;
        for (const RxCell& child : children)
            energy += child.energy;

        check.conserved &= energy == parent.energy * 8;
        for (uint32_t i = 0; i < parent.speciesCount; ++i) {
            uint64_t amount = 0;
            for (const RxCell& child : children)
                amount += AmountOf(child, parent.species[i]);

            check.conserved &= amount == parent.amounts[i] * 8;
        }

        const bool settles = PlaceTreeCell(probe.nest, probe.root, parentCell).has_value();  // 細かくする前の世界で
        check.placedCells += settles ? 1 : 0;
        if (!settles) {
            check.sameWhenNotPlaced &= sameAsSame;
            return;
        }

        // --- 子の温度は親と揃う: |熱_j − 熱 × C_j ÷ C| ≤ 1 mJ(最後の子は ≤ 7 mJ)---
        const Heat parentHeat = HeatOf(materials.table, parent);
        uint64_t capacitySum = 0;
        for (const RxCell& child : children)
            capacitySum += HeatOf(materials.table, child).capacity;

        for (const RxCell& child : children) {
            const Heat childHeat = HeatOf(materials.table, child);
            const int64_t expected = fx::FxMulDivS64(parentHeat.heat * 8, childHeat.capacity, capacitySum);
            const int64_t error = childHeat.heat - expected;
            check.temperature &= error >= -8000 && error <= 8000;
        }
    }

    void TestConservation(const Materials& materials, const World& placed, const World& same) {
        const World probe = MakeWorld(materials, true, false);
        CellCheck check;
        for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local)
            CheckParentCell(materials, placed, same, probe, local, check);

        Expect(check.conserved, "物質ごとの和とエネルギーの和は親 × 8");
        Expect(check.sameWhenNotPlaced, "寄せないセルは SAME の世界とビット単位で同じ");
        Expect(check.temperature, "子の温度は親と揃う(丸めは子 1 つ 1 mJ 程度)");
        Expect(check.placedCells > 0 && check.placedCells < (int)MR_OCTANT_CELLS, "寄せるセルと寄せないセルがある");
        Log(Channel::Sim, Level::Info, "置かれ方で配った親のセル {} / {}", check.placedCells, MR_OCTANT_CELLS);
    }

    // --- 2. 形: 半分のセルの粒は下・気体は上。斜面は高い側の上の子に粒が多い ---
    void TestShape(const Materials& materials, const World& placed) {
        const uint32_t oxygen = materials.table.SpeciesId("oxygen");

        // 半分の粒(x = 1・y = 2・z = 0)
        const uint32_t half = 1 + (2 * 4);
        const auto at = [&](uint32_t local, uint32_t j) {
            return LoadNestCell(placed.nest, placed.child, MrChildCell(local, j));
        };
        bool layered = true;
        for (uint32_t j = 0; j < 8; j += 2) {
            const bool low = ((j >> 1) & 1u) == 0;
            if (!low)
                continue;

            const RxCell bottom = at(half, j);
            const RxCell top = at(half, j | 2u);
            layered &= AmountOf(bottom, materials.grain) > AmountOf(top, materials.grain) &&
                       AmountOf(bottom, oxygen) < AmountOf(top, oxygen);
        }

        Expect(layered, "半分の粒は下の子に詰まり、気体は上の子へ多く入る");

        // 斜面(x = 1・y = 1・z = 2。左右の列は 20 % と 60 %。ここは 40 %): x の高い側の子(j の bit 0 が 1)に粒が多い
        const uint32_t slope = 1 + (1 * 4) + (2 * 16);
        uint64_t lowSide = 0;
        uint64_t highSide = 0;
        for (uint32_t j = 0; j < 8; ++j)
            ((j & 1u) == 0 ? lowSide : highSide) += AmountOf(at(slope, j), materials.grain);

        Expect(highSide > lowSide, "斜面のセルは高い側の子に粒が多い");
    }

    // --- 層(T-1220。ADR-0489): 層 = 沈む側・残り = 物質の置かれ方(層を使う世界の粒の残りは舞う粒)---

    // 置かれ方の世界の根の粒に 層 = 粒の量 × numerator ÷ 4 を書き(layersOn なら層を使う世界)、八分の一を細かくする
    World MakeLayeredWorld(const Materials& materials, uint64_t numerator, bool layersOn) {
        World world = MakeWorld(materials, true, false);
        if (layersOn)
            EnableCellLayers(world.nest, materials.table);

        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            const RxCell cell = LoadNestCell(world.nest, world.root, index);
            MrCellLayers layers;
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                if (cell.species[i] == materials.grain)
                    layers.amounts[i] = numerator == 4 ? cell.amounts[i] : cell.amounts[i] / 4 * numerator;
            }

            StoreNestCellWithLayers(world.nest, world.root, index, cell, layers);
        }

        const MrRequest refine = MrMakeRequest(MR_REQUEST_REFINE, 1, 1, 1, 1);
        SubmitRequests(world.nest, std::span(&refine, 1));
        ProcessRequests(world.nest);
        for (uint32_t slot = 0; slot < world.nest.capacity.worldBlocks; ++slot) {
            if (world.nest.blocks[slot].kind == MR_BLOCK_REAL && world.nest.blocks[slot].level == 1)
                world.child = slot;
        }

        return world;
    }

    // 子 8 つの粒の量と層の和(親のセル local)
    struct GrainSums {
        uint64_t amount = 0;
        uint64_t layer = 0;
    };

    GrainSums ChildGrainSums(const Materials& materials, const World& world, uint32_t local) {
        GrainSums sums;
        for (uint32_t j = 0; j < 8; ++j) {
            const uint32_t index = MrChildCell(local, j);
            const RxCell cell = LoadNestCell(world.nest, world.child, index);
            const MrCellLayers layers = LoadNestCellLayers(world.nest, world.child, index);
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                if (cell.species[i] != materials.grain)
                    continue;

                sums.amount += cell.amounts[i];
                sums.layer += layers.amounts[i];
            }
        }

        return sums;
    }

    void TestLayers(const Materials& materials, const World& placed) {
        // --- 層を使わない世界で粒が全部層なら、層の無い世界と同じ子(層 = 子の粒の量)---
        const World full = MakeLayeredWorld(materials, 4, false);
        uint32_t fullMismatches = 0;
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            const RxCell cell = LoadNestCell(full.nest, full.child, index);
            const MrCellLayers layers = LoadNestCellLayers(full.nest, full.child, index);
            bool same = MrSameCell(cell, LoadNestCell(placed.nest, placed.child, index));
            for (uint32_t i = 0; i < cell.speciesCount; ++i)
                same = same && layers.amounts[i] == (cell.species[i] == materials.grain ? cell.amounts[i] : 0);

            fullMismatches += same ? 0u : 1u;
        }

        Expect(fullMismatches == 0, "層: 粒が全部層なら子は層の無い世界と同じで、子の層 = 子の粒の量");

        // --- 層を使う世界: 半分が層・層の無い粒(舞う粒)---
        const World half = MakeLayeredWorld(materials, 2, true);
        const World floating = MakeLayeredWorld(materials, 0, true);
        const World probe = MakeLayeredWorld(materials, 2, true);
        uint32_t sumMismatches = 0;
        uint32_t floatMismatches = 0;
        for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
            const uint32_t parentCell = MrOctantCell(0, local);
            const RxCell parent = SceneCell(materials, parentCell);
            const uint64_t grain = AmountOf(parent, materials.grain);
            const GrainSums halfSums = ChildGrainSums(materials, half, local);
            const GrainSums floatSums = ChildGrainSums(materials, floating, local);
            sumMismatches += halfSums.amount == grain * 8 && halfSums.layer == grain / 4 * 2 * 8 ? 0u : 1u;
            sumMismatches += floatSums.amount == grain * 8 && floatSums.layer == 0 ? 0u : 1u;

            // 粒の半分の層(y = 2・z < 2)は舞う粒だけなら凝縮相が無いので寄せない(子は親と同じ数)
            const bool halfLayerCell = MrCellZ(parentCell) < 2 && MrCellY(parentCell) == 2;
            for (uint32_t j = 0; j < 8 && halfLayerCell; ++j) {
                const RxCell cell = LoadNestCell(floating.nest, floating.child, MrChildCell(local, j));
                floatMismatches += AmountOf(cell, materials.grain) == grain ? 0u : 1u;
            }
        }

        Expect(sumMismatches == 0, "層を使う世界: 子の粒の量の和 = 親 × 8・子の層の和 = 親の層 × 8");
        Expect(floatMismatches == 0, "層を使う世界: 舞う粒だけのセルは沈めない(子は親と同じ数)");
        Expect(CountLayerMisfits(half.nest) == 0 && CountLayerMisfits(floating.nest) == 0, "層: 0 ≤ 層 ≤ 量");
        Expect(HashWholeNest(half.nest) == HashWholeNest(probe.nest), "層: 2 回走らせて状態の要約が同じ");
        Expect(HashWholeNest(half.nest) != HashWholeNest(floating.nest), "層: 層の違いで子が変わる");
    }

    int Run() {
        const auto baked = BakeReactionTable(MakeSpeciesChangedTestTable());
        if (!baked) {
            Log(Channel::Sim, Level::Error, "試験の表をベイクできない: {}", baked.error());
            return 1;
        }

        const Materials materials = MakeMaterials(*baked);
        const World placed = MakeWorld(materials, true);
        const World same = MakeWorld(materials, false);
        Expect(placed.child != MR_NO_BLOCK && same.child == placed.child, "細かくした子がある");
        if (placed.child == MR_NO_BLOCK)
            return 1;

        TestConservation(materials, placed, same);
        TestShape(materials, placed);
        TestLayers(materials, placed);
        Expect(HashWholeNest(placed.nest) == HashWholeNest(MakeWorld(materials, true).nest),
               "2 回走らせて状態の要約が同じ");
        Expect(HashWholeNest(placed.nest) != HashWholeNest(same.nest), "置かれ方の世界は SAME の世界と違う");

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "tree_refine_placement_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "tree_refine_placement_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
