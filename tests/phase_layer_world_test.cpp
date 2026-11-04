// phase_layer_world_test.cpp — 世界の木の伝導の当てる段で、混ざったセル(層 + 気体)の面を部分ごとに流す(T-1230 段 2 ①・ADR-0512 の 2)の CPU の試験。
// 根 1 つ(8³・レベル 0 = 0.5 m・端は壁)。y < 3 は石(330 K)、y = 3 は砂の層(石の粒)+ 空気の混ざったセル、y > 3 は空気(300 K)。
// 確かめること:
//   1. 相の層の欄があっても混ざったセルの無い世界は、欄の無い世界とセルのエネルギーがビット単位で同じ(当てる段の道が今と同じ)
//   2. 1 刻みの混ざったセルの変化が、面の式(phase_layer.hlsli の PlPartSideOf・PlPartFaceFlow・PlInnerFlowOf)で手で数えた値とビット単位で同じ
//      (レベル 0 の係数は 0.5 m のセルの式と同じ)。段違いの水面(x の偶奇で層の高さが違う)で横の面の斜めの組も通す
//   3. 保存量(ComputeConservedTotals)が刻みで変わらない・部分の熱が負にならない・層は気体より速く石の温度へ寄る
//   4. 八分の一を細かくした木(違うレベルの面)でも保存量が変わらず、部分の熱が負にならない
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "multires_test_scene.h"
#include "phase_layer_scene.h"
#include "sim/multires_fields.h"
#include "sim/multires_nest.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::reaction;
using namespace bicameral::sim;

namespace {

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    using Amount = test::PlAmount;

    constexpr uint64_t CELL_VOLUME_PL = 125000000000000ull;
    constexpr uint32_t STONE_MK = 330000;
    constexpr uint32_t LAYER_MK = 300000;
    constexpr uint32_t GAS_MK = 290000;
    constexpr uint32_t AIR_MK = 300000;
    constexpr int64_t SURFACE_Y = 3;
    constexpr MultiresStepOptions CONDUCTION = {.conduction = true};
    constexpr uint64_t SEED = 7;

    std::vector<Amount> AirFilling(const BakedReactionTable& table, const std::vector<Amount>& condensed) {
        uint64_t volume = 0;
        for (const Amount& entry : condensed)
            volume += entry.amount * table.View().Species(table.SpeciesId(entry.name)).molarVolume;

        const uint64_t open = volume < CELL_VOLUME_PL ? CELL_VOLUME_PL - volume : 0;
        std::vector<Amount> air = {{"nitrogen", test::PL_AIR_NITROGEN_MICROMOL},
                                   {"oxygen", test::PL_AIR_OXYGEN_MICROMOL}};
        for (Amount& entry : air)
            entry.amount = (uint64_t)fx::FxMulDivS64((int64_t)entry.amount, open, CELL_VOLUME_PL);

        return air;
    }

    struct SceneCell {
        RxCell cell;
        int64_t condensedEnergy = 0;
    };

    SceneCell MakeWhole(const BakedReactionTable& table, const std::vector<Amount>& amounts, uint32_t mk) {
        return {
            .cell = test::MakePhaseLayerCell(table, amounts, test::PlPartEnergyMicrojoules(table, amounts, mk) / 1000)};
    }

    // 砂の層(石の粒。反応しない)の厚さの割合(per mille)の混ざったセル
    SceneCell MakeMixed(const BakedReactionTable& table, uint32_t perMille) {
        const uint64_t molarVolume = table.View().Species(table.SpeciesId("stone_real")).molarVolume;
        const std::vector<Amount> sand = {{"stone_real", CELL_VOLUME_PL / 1000 * perMille / molarVolume}};
        const std::vector<Amount> gas = AirFilling(table, sand);
        const int64_t condensedMj = test::PlPartEnergyMicrojoules(table, sand, LAYER_MK) / 1000;
        const int64_t gasMj = test::PlPartEnergyMicrojoules(table, gas, GAS_MK) / 1000;
        std::vector<Amount> all = sand;
        all.insert(all.end(), gas.begin(), gas.end());

        return {.cell = test::MakePhaseLayerCell(table, all, condensedMj + gasMj), .condensedEnergy = condensedMj};
    }

    struct World {
        MultiresNest nest;
        uint32_t root = MR_NO_BLOCK;
        uint32_t field = MULTIRES_NO_FIELD;
    };

    // surface: 0 = 水面なし(y = 3 も空気)・1 = 平ら(30 %)・2 = 段違い(30 % / 60 %)
    World MakeWorld(const BakedReactionTable& table, bool withField, uint32_t surface) {
        World world{.nest = MakeMultiresNest(test::MakeMultiresCapacity(table, 16, 4, 16))};
        if (withField)
            world.field = EnablePhaseLayerEnergy(world.nest, table);

        const SceneCell stone = MakeWhole(table, {{"stone_real", test::PL_STONE_MICROMOL}}, STONE_MK);
        const SceneCell air = MakeWhole(table, AirFilling(table, {}), AIR_MK);
        const SceneCell even = MakeMixed(table, 300);
        const SceneCell odd = MakeMixed(table, surface == 2 ? 600 : 300);
        std::vector<RxCell> cells(MR_BLOCK_CELLS);
        std::vector<int64_t> condensed(MR_BLOCK_CELLS, 0);
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            const int64_t y = MrCellY(index);
            SceneCell chosen = y < SURFACE_Y ? stone : air;
            if (y == SURFACE_Y && surface != 0)
                chosen = (MrCellX(index) & 1u) == 0 ? even : odd;

            cells[index] = chosen.cell;
            condensed[index] = chosen.condensedEnergy;
        }

        world.root = PlaceRootBlock(world.nest, 0, 0, 0, cells);
        const bool expanded = ExpandNestPage(world.nest, world.root);
        EXPECT(expanded);
        if (withField) {
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                StoreNestField(world.nest, world.field, world.root, index, 0, condensed[index]);
        }

        return world;
    }

    int64_t Ec(const World& world, uint32_t slot, uint32_t index) {
        return LoadNestPhaseLayerEnergy(world.nest, slot, index);
    }

    bool PartsNonNegative(const BakedReactionTable& table, const World& world) {
        for (uint32_t slot = 0; slot < world.nest.capacity.worldBlocks; ++slot) {
            const MrBlock& block = world.nest.blocks[slot];
            if (block.kind != MR_BLOCK_REAL || MrIsUniform(block))
                continue;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const RxCell cell = LoadNestCell(world.nest, slot, index);
                if (PlLayerOf(table.View(), cell).mixed == 0)
                    continue;

                const PlParts parts = PlPartsOf(table.View(), cell, Ec(world, slot, index));
                if (parts.condensed.heat < 0 || parts.gas.heat < 0)
                    return false;
            }
        }

        return true;
    }

    // --- 1 混ざったセルの無い世界は欄の有無でビット単位で同じ ---
    void TestNoMixedCells(const BakedReactionTable& table) {
        World plain = MakeWorld(table, false, 0);
        World fielded = MakeWorld(table, true, 0);
        for (uint64_t tick = 1; tick <= 30; ++tick) {
            StepNest(plain.nest, table, SEED, tick, CONDUCTION);
            StepNest(fielded.nest, table, SEED, tick, CONDUCTION);
        }

        bool same = true;
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
            same = same && LoadNestCell(plain.nest, plain.root, index).energy ==
                               LoadNestCell(fielded.nest, fielded.root, index).energy;

        EXPECT(same);
    }

    // --- 2 1 刻みの混ざったセルの変化 = 面の式で手で数えた値 ---
    struct Expected {
        int64_t energy = 0;
        int64_t condensed = 0;
    };

    Expected HandStep(const BakedReactionTable& table, const World& world, uint32_t index) {
        const RxCell self = LoadNestCell(world.nest, world.root, index);
        const int64_t selfEc = Ec(world, world.root, index);
        Expected expected{.energy = self.energy, .condensed = selfEc};
        const int64_t x = MrCellX(index);
        const int64_t y = MrCellY(index);
        const int64_t z = MrCellZ(index);
        for (uint32_t face = 0; face < MR_FACES; ++face) {
            const int64_t axis = face >> 1;
            const int64_t step = (face & 1u) != 0 ? 1 : -1;
            const int64_t nx = x + (axis == 0 ? step : 0);
            const int64_t ny = y + (axis == 1 ? step : 0);
            const int64_t nz = z + (axis == 2 ? step : 0);
            if (nx < 0 || ny < 0 || nz < 0 || nx >= MR_BLOCK_EDGE || ny >= MR_BLOCK_EDGE || nz >= MR_BLOCK_EDGE)
                continue;

            const uint32_t other = MrCellIndex(static_cast<uint32_t>(nx), static_cast<uint32_t>(ny),
                                               static_cast<uint32_t>(nz));
            const PlFaceFlow flow = PlPartFaceFlow(
                PlPartSideOf(table.View(), self, selfEc, face),
                PlPartSideOf(table.View(), LoadNestCell(world.nest, world.root, other), Ec(world, world.root, other),
                             face ^ 1u));
            expected.energy -= flow.total;
            expected.condensed -= flow.selfCondensed;
        }

        expected.condensed -= PlInnerFlowOf(table.View(), self, selfEc);

        return expected;
    }

    void TestHandStep(const BakedReactionTable& table, uint32_t surface) {
        World world = MakeWorld(table, true, surface);
        const uint32_t even = MrCellIndex(4, SURFACE_Y, 4);
        const uint32_t odd = MrCellIndex(3, SURFACE_Y, 4);
        const Expected expectedEven = HandStep(table, world, even);
        const Expected expectedOdd = HandStep(table, world, odd);
        StepNest(world.nest, table, SEED, 1, CONDUCTION);
        EXPECT(LoadNestCell(world.nest, world.root, even).energy == expectedEven.energy);
        EXPECT(Ec(world, world.root, even) == expectedEven.condensed);
        EXPECT(LoadNestCell(world.nest, world.root, odd).energy == expectedOdd.energy);
        EXPECT(Ec(world, world.root, odd) == expectedOdd.condensed);
    }

    // --- 3・4 保存・部分の熱・層が先に寄る ---
    void TestRun(const BakedReactionTable& table, bool refine) {
        World world = MakeWorld(table, true, 2);
        if (refine) {
            const MrRequest request = MrMakeRequest(MR_REQUEST_REFINE, 1, 1, 0, 1);
            SubmitRequests(world.nest, std::span(&request, 1));
            ProcessRequests(world.nest);
        }

        const int32_t finest = refine ? 1 : 0;
        const ConservedTotals before = ComputeConservedTotals(world.nest, table, finest);
        bool conserved = true;
        bool nonNegative = true;
        for (uint64_t tick = 1; tick <= 120; ++tick) {
            StepNest(world.nest, table, SEED, tick, CONDUCTION);
            conserved = conserved && ComputeConservedTotals(world.nest, table, finest) == before;
            nonNegative = nonNegative && PartsNonNegative(table, world);
        }

        EXPECT(conserved);
        EXPECT(nonNegative);
        // 水面の混ざったセルのうち根に残っている(覆われていない)もの
        uint32_t index = MR_BLOCK_CELLS;
        for (uint32_t candidate = 0; candidate < MR_BLOCK_CELLS && index == MR_BLOCK_CELLS; ++candidate) {
            const RxCell cell = LoadNestCell(world.nest, world.root, candidate);
            if (MrCellY(candidate) == SURFACE_Y && PlLayerOf(table.View(), cell).mixed != 0)
                index = candidate;
        }

        EXPECT(index != MR_BLOCK_CELLS);
        if (index == MR_BLOCK_CELLS)
            return;

        const RxCell cell = LoadNestCell(world.nest, world.root, index);
        const PlParts parts = PlPartsOf(table.View(), cell, Ec(world, world.root, index));
        std::printf("%s 120 刻み: 水面のセルの層 %d mK・気体 %d mK\n", refine ? "細かくした木" : "根だけ",
                    parts.condensed.temperature, parts.gas.temperature);
        // 層(初め 300 K)は下の石(330 K)へ寄り、気体(290 K)より暖かい
        EXPECT(parts.condensed.temperature > static_cast<int32_t>(LAYER_MK));
        EXPECT(parts.condensed.temperature > parts.gas.temperature);
    }

}  // namespace

int main() {
    const BakedReactionTable table = test::MakePhaseLayerTable();
    TestNoMixedCells(table);
    TestHandStep(table, 1);
    TestHandStep(table, 2);
    TestRun(table, false);
    TestRun(table, true);
    if (failureCount != 0) {
        std::printf("%d 件の失敗\n", failureCount);
        return 1;
    }

    std::printf("ok\n");
    return 0;
}
