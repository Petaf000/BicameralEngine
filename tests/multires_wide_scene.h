// multires_wide_scene.h — 頁の溢れが前の当座の枠(1 面 1024 成分)より大きくなる場面(T-0236。gpu_multires_test が使う)。
// 表は元素 X 1 つの異性体 w01〜w11(公開側に置く試験データで、ゲームの中身ではない。D-006)。w01〜w08 が揃ったセルで
// w01 → w09・w02 → w10・w03 → w11(どれも 10 /s)が進み、1 セル 11 種(溢れ 3 種)になる = 1 頁の溢れが 512 × 3 = 1536 成分。
// 根を 2 つ置く: 頁を持つ根(セルごとに量が違う。全部を刻む段で足りなくなる)と、一様な根(頁に広げて刻む段で足りなくなる)。
// GPU は書く面の塊が足りない刻みで塊を配って同じ刻みのうちに刻み直し、CPU の上限の無い世界と毎刻みビット一致する
#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "multires_test_scene.h"
#include "reaction_limits_table.h"

namespace bicameral::test {

    constexpr uint32_t WIDE_OVERFLOW_SPECIES = 11;
    constexpr uint32_t WIDE_OVERFLOW_START_SPECIES = 8;
    constexpr uint32_t WIDE_OVERFLOW_WORLD_BLOCKS = 4;
    constexpr uint64_t WIDE_OVERFLOW_TICKS = 8;
    constexpr uint64_t WIDE_OVERFLOW_SEED = 0x776964650236ull;

    inline std::string WideOverflowName(uint32_t i) {
        return std::format("w{:02}", i);
    }

    inline sim::ReactionTableDefinition MakeWideOverflowTable() {
        sim::ReactionTableDefinition definition;
        definition.elements = {{.name = "X", .atomicMass = 10000}};
        for (uint32_t i = 1; i <= WIDE_OVERFLOW_SPECIES; ++i)
            definition.species.push_back(LimitsSpecies(WideOverflowName(i), -700 * static_cast<int64_t>(i)));

        // --- 溢れを作る: w01〜w03 から 9〜11 種目(10 /s)---
        for (uint32_t i = 1; i <= WIDE_OVERFLOW_SPECIES - WIDE_OVERFLOW_START_SPECIES; ++i) {
            definition.rules.push_back(LimitsRule(std::format("wide_make_{}", i), WideOverflowName(i),
                                                  WideOverflowName(WIDE_OVERFLOW_START_SPECIES + i), 1, 1));
        }

        return definition;
    }

    // w01〜w08 のセル(uniform なら全部同じ量、でなければセルごとに 1〜2 mol)
    inline reaction::RxCell MakeWideOverflowCell(const sim::BakedReactionTable& table, uint32_t index, bool uniform) {
        std::vector<sim::SpeciesAmount> amounts;
        for (uint32_t i = 1; i <= WIDE_OVERFLOW_START_SPECIES; ++i) {
            const uint64_t seed = fx::FxHash64(REACTION_TEST_SEED, uniform ? 0u : index, i, 0x236);
            const uint64_t amount = 1000000 + fx::FxRandomBelow(seed, 1000000);
            amounts.push_back({.species = table.SpeciesId(WideOverflowName(i)), .amount = amount});
        }

        return sim::MakeReactionCell(table, amounts, LIMITS_TEMPERATURE_MILLIKELVIN);
    }

    inline sim::MultiresNest MakeWideOverflowNest(const sim::BakedReactionTable& table) {
        sim::MultiresNest nest = sim::MakeMultiresNest(
            MakeMultiresCapacity(table, WIDE_OVERFLOW_WORLD_BLOCKS, 0, WIDE_OVERFLOW_WORLD_BLOCKS));
        for (const bool uniform : {false, true}) {
            std::vector<reaction::RxCell> cells(multires::MR_BLOCK_CELLS);
            for (uint32_t index = 0; index < cells.size(); ++index)
                cells[index] = MakeWideOverflowCell(table, index, uniform);

            sim::PlaceRootBlock(nest, uniform ? 8 : 0, 0, 0, cells);
        }

        return nest;
    }

}  // namespace bicameral::test
