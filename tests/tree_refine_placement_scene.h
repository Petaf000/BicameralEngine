// tree_refine_placement_scene.h — 世界の木を細かくする時のセルの中身の置かれ方(T-1176・T-1188)の試験の場面。CPU の試験
// (tests/tree_refine_placement_test.cpp)と GPU の試験(tests/gpu_tree_refine_placement_test.cpp)が同じ場面を使う。
// 根 1 つ(空気の世界)に、粒の層(下は詰まり切る・途中は半分・上は空気だけ)と、x に沿って高さの変わる粒の斜面と、液体の溜まりを置き、
// 八分の一を細かくする。物質の表は試験の表(MakeSpeciesChangedTestTable)の 2 つの物質を粒(ばらける固体)と液体に書き換えて使う
// (相の印・粒の詰まり・モル体積だけを読む)。
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "multires_test_scene.h"
#include "reaction_test_table.h"
#include "sim/multires_nest.h"

namespace bicameral::test::tree_place {

    using namespace bicameral::multires;
    using namespace bicameral::reaction;

    constexpr uint32_t GRAIN_MOLAR_VOLUME = 22640;   // mm³/mol(石英の砂 60 g/mol ÷ 2650 kg/m³)
    constexpr uint32_t LIQUID_MOLAR_VOLUME = 18000;  // mm³/mol(水)
    constexpr int32_t ROOM_MILLIKELVIN = 300000;

    struct Materials {
        sim::BakedReactionTable table;
        uint32_t grain = 0;
        uint32_t liquid = 0;
    };

    // packingPercent は粒の詰まり(D-679。印のビット 8〜14。0 = 100 %)
    inline Materials MakeMaterials(const sim::BakedReactionTable& baked, uint32_t packingPercent = 0) {
        Materials materials{.table = baked};
        materials.grain = baked.SpeciesId("ozone");
        materials.liquid = baked.SpeciesId("hydrogen");
        materials.table.species[materials.grain].flags = RX_SPECIES_GRAIN |
                                                         (packingPercent << RX_SPECIES_PACKING_SHIFT);
        materials.table.species[materials.grain].molarVolume = GRAIN_MOLAR_VOLUME;
        materials.table.species[materials.liquid].flags = RX_SPECIES_LIQUID;
        materials.table.species[materials.liquid].molarVolume = LIQUID_MOLAR_VOLUME;

        return materials;
    }

    // 器(レベル 0 のセル)の percent % を占める量(µmol)
    inline uint64_t AmountOfPercent(uint32_t percent, uint32_t molarVolume) {
        return (MR_CELL_VOLUME_MM3 / 100 * percent * 1000000) / molarVolume;
    }

    // --- 場面: 八分の一 0(x・y・z が 0〜3)の中に層・斜面・溜まり ---
    inline RxCell SceneCell(const Materials& materials, uint32_t index) {
        const uint32_t x = MrCellX(index);
        const uint32_t y = MrCellY(index);
        const uint32_t z = MrCellZ(index);
        std::vector<sim::SpeciesAmount> amounts = {
            {.species = materials.table.SpeciesId("oxygen"), .amount = 1067000},
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

        std::ranges::sort(amounts, {}, &sim::SpeciesAmount::species);

        return sim::MakeReactionCell(materials.table, amounts, ROOM_MILLIKELVIN);
    }

    struct World {
        sim::MultiresNest nest;
        uint32_t root = MR_NO_BLOCK;
        uint32_t child = MR_NO_BLOCK;
    };

    // level = 細かくする要求のレベル(1 = 根の八分の一 0 を 1 段・2 = その子の八分の一 0 までの 2 段の鎖)
    inline World MakeWorld(const Materials& materials, bool placed, bool refined = true, int32_t level = 1) {
        World world{.nest = sim::MakeMultiresNest(MakeMultiresCapacity(materials.table, 16, 0, 16))};
        std::vector<RxCell> cells;
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
            cells.push_back(SceneCell(materials, index));

        world.root = sim::PlaceRootBlock(world.nest, 0, 0, 0, cells);
        if (placed)
            sim::EnableRefineCellPlacement(world.nest, materials.table);

        if (!refined)
            return world;

        const MrRequest refine = MrMakeRequest(MR_REQUEST_REFINE, level, 1, 1, 1);
        sim::SubmitRequests(world.nest, std::span(&refine, 1));
        sim::ProcessRequests(world.nest);
        for (uint32_t slot = 0; slot < world.nest.capacity.worldBlocks; ++slot) {
            if (world.nest.blocks[slot].kind == MR_BLOCK_REAL && world.nest.blocks[slot].level == 1)
                world.child = slot;
        }

        return world;
    }

    // --- 端の場面(T-1189): 隣のブロックの列を読む ---
    // 根 A(原点 0)の右に根 B(x = 8)、下に根 C(y = −8)と D(x = 8・y = −8)。A・B の段 y = 0 は粒の斜面(詰まりが世界の x とともに
    // 5 % → 95 % に上がる)、上は空気。C・D の一番上の段 y = 7 は詰まりが x とともに 95 % → 20 % に下がる粒、下は詰まり切る
    // (A の端の列 x = 7 の表面は x とともに上がる = 傾きが出る)。要求 R1 は A の八分の一 1(x = 4〜7。B と C に接する)を、
    // R2 は B の八分の一 0(A に接する)を細かくする。B・C が無ければ A の端の列は「隣が無い」(傾き 0・下は詰まり切る)と読む。
    inline RxCell EdgeSlopeCell(const Materials& materials, int64_t worldX, uint32_t y, bool below) {
        std::vector<sim::SpeciesAmount> amounts = {
            {.species = materials.table.SpeciesId("oxygen"), .amount = 1067000},
            {.species = materials.table.SpeciesId("nitrogen"), .amount = 4013000}};
        uint32_t grainPercent = 0;
        if (below)
            grainPercent = y == 7 ? 95 - (uint32_t)(worldX * 5) : 100;
        else if (y == 0)
            grainPercent = 5 + (uint32_t)(worldX * 6);

        if (grainPercent != 0)
            amounts.push_back(
                {.species = materials.grain, .amount = AmountOfPercent(grainPercent, GRAIN_MOLAR_VOLUME)});

        std::ranges::sort(amounts, {}, &sim::SpeciesAmount::species);

        return sim::MakeReactionCell(materials.table, amounts, ROOM_MILLIKELVIN);
    }

    inline std::vector<RxCell> EdgeRootCells(const Materials& materials, int64_t originX, bool below) {
        std::vector<RxCell> cells;
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
            cells.push_back(EdgeSlopeCell(materials, originX + MrCellX(index), MrCellY(index), below));

        return cells;
    }

    // R1 = A の八分の一 1・R2 = B の八分の一 0(どちらもレベル 1 の点)
    inline MrRequest EdgeRequestA() {
        return MrMakeRequest(MR_REQUEST_REFINE, 1, 9, 1, 1);
    }

    inline MrRequest EdgeRequestB() {
        return MrMakeRequest(MR_REQUEST_REFINE, 1, 17, 1, 1);
    }

    // neighbors = B・C・D を置くか。requests = 処理する要求(空なら細かくしない)。child = R1 の子(レベル 1・原点 x = 8)
    inline World MakeEdgeWorld(const Materials& materials, bool neighbors, std::span<const MrRequest> requests) {
        World world{.nest = sim::MakeMultiresNest(MakeMultiresCapacity(materials.table, 16, 0, 16))};
        world.root = sim::PlaceRootBlock(world.nest, 0, 0, 0, EdgeRootCells(materials, 0, false));
        if (neighbors) {
            sim::PlaceRootBlock(world.nest, 8, 0, 0, EdgeRootCells(materials, 8, false));
            sim::PlaceRootBlock(world.nest, 0, -8, 0, EdgeRootCells(materials, 0, true));
            sim::PlaceRootBlock(world.nest, 8, -8, 0, EdgeRootCells(materials, 8, true));
        }

        sim::EnableRefineCellPlacement(world.nest, materials.table);
        if (requests.empty())
            return world;

        sim::SubmitRequests(world.nest, requests);
        sim::ProcessRequests(world.nest);
        world.child = sim::LookupBlock(world.nest, 1, 8, 0, 0);

        return world;
    }

}  // namespace bicameral::test::tree_place
