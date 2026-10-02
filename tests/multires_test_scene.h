// multires_test_scene.h — 多重解像度のテスト(tests/multires_test.cpp・gpu_multires_test.cpp)が同じ場面を作り、同じ順に進めるための道具(T-0017)。
// 場面: レベル 0 の根のブロック(8³ セル = 4m 角。成分と温度はいろいろ)の中の 1 点の周りを、刻み REFINE_TICK に 9 段(k = 1〜9、1mm)
// 細かくし、COARSEN_TICK に戻す。影の場面は同じ鎖を観察の影として作り、毎刻み親に引き戻し、COARSEN_TICK に捨てる。
#pragma once

#include <cstdint>
#include <vector>

#include "reaction_test_cells.h"
#include "sim/multires_nest.h"

namespace bicameral::test {

    enum class MultiresScenario : uint8_t {
        None,    // 細かくしない
        Real,    // 本物の鎖(状態・介入による細分。世界に返る)
        Shadow,  // 観察の影の鎖(世界に返らない)
    };

    constexpr uint64_t MULTIRES_TEST_SEED = 20261003;
    constexpr uint32_t MULTIRES_LEVELS = 9;  // k = 1〜9(0.5m → 1mm)
    constexpr uint32_t MULTIRES_ROOT_SLOT = 0;
    constexpr uint32_t MULTIRES_REAL_SLOT = 1;                                       // 本物の鎖は枠 1〜9
    constexpr uint32_t MULTIRES_SHADOW_SLOT = MULTIRES_REAL_SLOT + MULTIRES_LEVELS;  // 影の鎖は枠 10〜18
    constexpr uint32_t MULTIRES_BLOCK_CAPACITY = MULTIRES_SHADOW_SLOT + MULTIRES_LEVELS;
    constexpr uint32_t MULTIRES_FRACTION_CAPACITY = 32;
    constexpr uint64_t MULTIRES_REFINE_TICK = 10;
    constexpr uint64_t MULTIRES_COARSEN_TICK = 60;
    constexpr uint64_t MULTIRES_END_TICK = 70;

    inline std::vector<reaction::RxCell> MakeMultiresRootCells(const sim::BakedReactionTable& table) {
        std::vector<reaction::RxCell> cells(multires::MR_BLOCK_CELLS);
        for (uint32_t index = 0; index < cells.size(); ++index)
            cells[index] = MakeVariedReactionCell(table, index);

        // 細かくする点のセルは燃え始めの木箱(600 K。体積の 1 割がセルロース、残りが空気)。k = 3 より細かい段は全部このセルから
        const std::vector<sim::SpeciesAmount> crate = {{.species = table.SpeciesId("cellulose"), .amount = 38600000},
                                                       {.species = table.SpeciesId("oxygen"), .amount = 983000},
                                                       {.species = table.SpeciesId("nitrogen"), .amount = 3697000}};
        cells[multires::MrCellIndex(3, 4, 5)] = sim::MakeReactionCell(table, crate, 600000);

        return cells;
    }

    // 根のセル (3, 4, 5) の中の、最も細かいレベルの 1 点(下位のビットはハッシュで散らす)
    inline sim::MultiresPoint MakeMultiresPoint(uint32_t levels) {
        const uint64_t hash = fx::FxHash64(MULTIRES_TEST_SEED, 0, 0, 3);
        const uint64_t mask = (uint64_t{1} << levels) - 1;
        const auto scatter = [&](uint64_t cell, uint32_t shift) {
            return static_cast<int64_t>((cell << levels) | ((hash >> shift) & mask));
        };

        return {.x = scatter(3, 0), .y = scatter(4, 21), .z = scatter(5, 42), .level = static_cast<int32_t>(levels)};
    }

    inline sim::MultiresNest MakeMultiresNestForTest(const sim::BakedReactionTable& table) {
        sim::MultiresNest nest = sim::MakeMultiresNest(MULTIRES_BLOCK_CAPACITY, MULTIRES_FRACTION_CAPACITY);
        sim::PlaceRootBlock(nest, MULTIRES_ROOT_SLOT, 0, 0, 0, 0, MakeMultiresRootCells(table));

        return nest;
    }

    inline bool MultiresShadowExists(MultiresScenario scenario, uint64_t tick) {
        return scenario == MultiresScenario::Shadow && tick >= MULTIRES_REFINE_TICK && tick < MULTIRES_COARSEN_TICK;
    }

    // 1 刻み(CPU リファレンス): 細分の出来事 → 反応 → 影の引き戻し。GPU(sim::GpuMultires)も同じ順に記録する
    inline void StepMultiresScene(sim::MultiresNest& nest, const sim::BakedReactionTable& table,
                                  MultiresScenario scenario, uint64_t tick) {
        const sim::MultiresPoint point = MakeMultiresPoint(MULTIRES_LEVELS);
        if (tick == MULTIRES_REFINE_TICK && scenario == MultiresScenario::Real)
            sim::RefineChain(nest, MULTIRES_ROOT_SLOT, MULTIRES_REAL_SLOT, MULTIRES_LEVELS, multires::MR_BLOCK_REAL,
                             point);
        else if (tick == MULTIRES_REFINE_TICK && scenario == MultiresScenario::Shadow)
            sim::RefineChain(nest, MULTIRES_ROOT_SLOT, MULTIRES_SHADOW_SLOT, MULTIRES_LEVELS, multires::MR_BLOCK_SHADOW,
                             point);
        else if (tick == MULTIRES_COARSEN_TICK && scenario == MultiresScenario::Real)
            sim::CoarsenChain(nest, MULTIRES_REAL_SLOT + MULTIRES_LEVELS - 1, MULTIRES_LEVELS);
        else if (tick == MULTIRES_COARSEN_TICK && scenario == MultiresScenario::Shadow)
            sim::RemoveShadowChain(nest, MULTIRES_SHADOW_SLOT, MULTIRES_LEVELS);

        sim::StepNest(nest, table, MULTIRES_TEST_SEED, tick);
        if (MultiresShadowExists(scenario, tick))
            sim::PullBackShadowChain(nest, table, MULTIRES_SHADOW_SLOT, MULTIRES_LEVELS);
    }

}  // namespace bicameral::test
