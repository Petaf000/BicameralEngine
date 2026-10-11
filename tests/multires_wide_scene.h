// multires_wide_scene.h — 頁の溢れが前の当座の枠(1 面 1024 成分)より大きくなる場面(T-0236。gpu_multires_test が使う)。
// 表は元素 X 1 つの異性体 w01〜w11(公開側に置く試験データで、ゲームの中身ではない。D-006)。w01〜w08 が揃ったセルで
// w01 → w09・w02 → w10・w03 → w11(どれも 10 /s)が進み、1 セル 11 種(溢れ 3 種)になる = 1 頁の溢れが 512 × 3 = 1536 成分。
// 根を 2 つ置く: 頁を持つ根(セルごとに量が違う。全部を刻む段で足りなくなる)と、一様な根(頁に広げて刻む段で足りなくなる)。
// GPU は書く面の塊が足りない刻みで塊を配って同じ刻みのうちに刻み直し、CPU の上限の無い世界と毎刻みビット一致する。
// 一様な根は 9 種目を作る規則だけが進むので、溢れを使う世界は待たせた印で頁に広げて刻む(T-0276。それまでは一様のまま止まっていた)。
// T-0248: 1 セルが前の GPU の当座の上限(24 種)を超える場面(異性体 m01〜m32 の鎖)と、溢れの世界の費用を測る場面(空気だけ・成分の多いセル)
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
    constexpr int64_t WIDE_OVERFLOW_UNIFORM_X = 8;  // 一様な根の原点(頁を持つ根は 0)

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

    // 名前 nameOf(1)〜nameOf(8) のセル(uniform なら全部同じ量、でなければセルごとに 1〜2 mol)
    inline reaction::RxCell MakeWideOverflowCell(const sim::BakedReactionTable& table, uint32_t index, bool uniform,
                                                 std::string (*nameOf)(uint32_t) = WideOverflowName) {
        std::vector<sim::SpeciesAmount> amounts;
        for (uint32_t i = 1; i <= WIDE_OVERFLOW_START_SPECIES; ++i) {
            const uint64_t seed = fx::FxHash64(REACTION_TEST_SEED, uniform ? 0u : index, i, 0x236);
            const uint64_t amount = 1000000 + fx::FxRandomBelow(seed, 1000000);
            amounts.push_back({.species = table.SpeciesId(nameOf(i)), .amount = amount});
        }

        return sim::MakeReactionCell(table, amounts, LIMITS_TEMPERATURE_MILLIKELVIN);
    }

    // 頁を持つ根(x = 0)と一様な根(x = 8)
    inline sim::MultiresNest MakeWideOverflowNest(const sim::BakedReactionTable& table,
                                                  std::string (*nameOf)(uint32_t) = WideOverflowName) {
        sim::MultiresNest nest = sim::MakeMultiresNest(
            MakeMultiresCapacity(table, WIDE_OVERFLOW_WORLD_BLOCKS, 0, WIDE_OVERFLOW_WORLD_BLOCKS));
        for (const bool uniform : {false, true}) {
            std::vector<reaction::RxCell> cells(multires::MR_BLOCK_CELLS);
            for (uint32_t index = 0; index < cells.size(); ++index)
                cells[index] = MakeWideOverflowCell(table, index, uniform, nameOf);

            sim::PlaceRootBlock(nest, uniform ? WIDE_OVERFLOW_UNIFORM_X : 0, 0, 0, cells);
        }

        return nest;
    }

    // 一様な根(最初は w01〜w08 だけ)の刻むセルの成分の最大(溢れを含む)。9 以上なら 9 種目を作った(T-0276)
    inline uint32_t WideOverflowUniformRootSpecies(const sim::MultiresNest& nest) {
        const uint32_t slot = sim::LookupBlock(nest, 0, WIDE_OVERFLOW_UNIFORM_X, 0, 0);
        uint32_t most = 0;
        for (uint32_t index = 0; index < multires::MR_BLOCK_CELLS; ++index) {
            const uint32_t species = sim::LoadWideNestCell(nest, slot, index).speciesCount;
            most = species > most ? species : most;
        }

        return most;
    }

    // --- 1 セル 25 種以上(T-0248): 異性体 m01〜m32。m01〜m08 のセルで m(i) → m(i + 8)(i = 1〜24。どれも 10 /s)が鎖のように進み、
    //     1 刻みに 8 種ずつ増えて 4 刻み目から 1 セル 25 種以上になる(進む規則が 16 を超えると選ぶ = RX_LIMIT_CANDIDATES。CPU と同じ)---

    constexpr uint32_t WIDE_MANY_SPECIES = 32;
    constexpr uint32_t WIDE_MANY_OLD_LIMIT = 24;  // T-0176〜T-0236 の GPU の 1 セルの当座の上限
    // 試験の many の場面の刻み(T-0327: 表が GPU の上限〔96〕まであるので、1 セル 64 種〔T-0272 の上限〕を超えるまで刻む。20 刻みで 80 種)
    constexpr uint64_t WIDE_MANY_TICKS = 20;

    inline std::string WideManyName(uint32_t i) {
        return std::format("m{:02}", i);
    }

    // species = 物質の数(m01〜)。試験の場面は GPU の 1 セルの上限まで(T-0272。上限 64 なら 1 セル 33 種以上)、計測の場面は 32
    inline sim::ReactionTableDefinition MakeWideManyTable(uint32_t species = WIDE_MANY_SPECIES) {
        sim::ReactionTableDefinition definition;
        definition.elements = {{.name = "X", .atomicMass = 10000}};
        for (uint32_t i = 1; i <= species; ++i)
            definition.species.push_back(LimitsSpecies(WideManyName(i), -700 * static_cast<int64_t>(i)));

        for (uint32_t i = 1; i + WIDE_OVERFLOW_START_SPECIES <= species; ++i) {
            definition.rules.push_back(LimitsRule(std::format("wide_many_{}", i), WideManyName(i),
                                                  WideManyName(i + WIDE_OVERFLOW_START_SPECIES), 1, 1));
        }

        return definition;
    }

    // --- 費用を測る場面(T-0248。--measure-wide): 根を MEASURE_ROOTS 個(頁のセル 512 × 根)。どのセルも違う値(一様にならない)---

    constexpr uint32_t MEASURE_ROOTS_PER_AXIS = 4;
    constexpr uint32_t MEASURE_ROOTS = MEASURE_ROOTS_PER_AXIS * MEASURE_ROOTS_PER_AXIS * MEASURE_ROOTS_PER_AXIS;

    // 空気(酸素 21%・窒素 79%。燃焼の試験の表)。温度はセルごとに 295〜305 K(伝導が働く)
    inline reaction::RxCell MakeMeasureAirCell(const sim::BakedReactionTable& table, uint32_t root, uint32_t index) {
        const uint64_t seed = fx::FxHash64(REACTION_TEST_SEED, root, index, 0x248);
        const std::vector<sim::SpeciesAmount> air = {{.species = table.SpeciesId("oxygen"), .amount = 983000},
                                                     {.species = table.SpeciesId("nitrogen"), .amount = 3697000}};

        return sim::MakeReactionCell(table, air, 295000 + static_cast<int32_t>(fx::FxRandomBelow(seed, 10000)));
    }

    // many なら成分の多いセル(m01〜m08 から鎖で増える。表は MakeWideManyTable)、でなければ空気
    inline sim::MultiresNest MakeMeasureNest(const sim::BakedReactionTable& table, bool many) {
        sim::MultiresNest nest = sim::MakeMultiresNest(MakeMultiresCapacity(table, MEASURE_ROOTS, 0, MEASURE_ROOTS));
        for (uint32_t root = 0; root < MEASURE_ROOTS; ++root) {
            std::vector<reaction::RxCell> cells(multires::MR_BLOCK_CELLS);
            for (uint32_t index = 0; index < cells.size(); ++index) {
                const uint32_t varied = (root * multires::MR_BLOCK_CELLS) + index;
                cells[index] = many ? MakeWideOverflowCell(table, varied, false, WideManyName)
                                    : MakeMeasureAirCell(table, root, index);
            }

            const auto axis = [&](uint32_t shift) {
                return int64_t{(root / shift) % MEASURE_ROOTS_PER_AXIS} * 8;
            };
            sim::PlaceRootBlock(nest, axis(1), axis(MEASURE_ROOTS_PER_AXIS),
                                axis(MEASURE_ROOTS_PER_AXIS * MEASURE_ROOTS_PER_AXIS), cells);
        }

        return nest;
    }

}  // namespace bicameral::test
