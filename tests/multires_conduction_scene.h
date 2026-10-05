// multires_conduction_scene.h — 木の上の熱の伝導のテスト(tests/multires_conduction_test.cpp・gpu_multires_conduction_test.cpp)の
// 鎖の場面(T-0019・T-0107)。根 2×2×2(300 K の空気・一様)の真ん中の角のすぐ内側に 6 段の鎖を細かくし、
// いちばん細かいブロック(一様)を 1500 K にして種にする。熱は細かい所から粗い所へ、レベルをまたいで根まで流れる。
#pragma once

#include <algorithm>
#include <climits>
#include <cstdint>
#include <span>
#include <vector>

#include "multires_test_scene.h"

namespace bicameral::test {

    constexpr int32_t CHAIN_LEVELS = 6;
    constexpr uint32_t CHAIN_ROOTS = 8;
    constexpr int64_t CHAIN_CORNER = int64_t{8} << CHAIN_LEVELS;  // 根の角 (8, 8, 8) をレベル 6 の単位で
    constexpr uint64_t CONDUCTION_SEED = 20261005;

    inline reaction::RxCell MakeConductionAir(const sim::BakedReactionTable& table, int32_t millikelvin) {
        const std::vector<sim::SpeciesAmount> air = {{.species = table.SpeciesId("oxygen"), .amount = 1067000},
                                                     {.species = table.SpeciesId("nitrogen"), .amount = 4013000}};

        return sim::MakeReactionCell(table, air, millikelvin);
    }

    // capacity の世界の枠は CHAIN_ROOTS + CHAIN_LEVELS 以上(端数の枠が 0 なら粗い側へは整数の単位の倍数だけ送る)
    inline sim::MultiresNest MakeChainNest(const sim::BakedReactionTable& table,
                                           const sim::MultiresCapacity& capacity) {
        sim::MultiresNest nest = sim::MakeMultiresNest(capacity);
        const std::vector<reaction::RxCell> air(multires::MR_BLOCK_CELLS, MakeConductionAir(table, 300000));
        for (uint32_t root = 0; root < CHAIN_ROOTS; ++root)
            sim::PlaceRootBlock(nest, int64_t{root & 1u} * 8, int64_t{(root >> 1) & 1u} * 8, int64_t{root >> 2} * 8,
                                air);

        const multires::MrRequest request = multires::MrMakeRequest(
            multires::MR_REQUEST_REFINE, CHAIN_LEVELS, CHAIN_CORNER + 1, CHAIN_CORNER + 2, CHAIN_CORNER + 3);
        sim::SubmitRequests(nest, std::span(&request, 1));
        sim::ProcessRequests(nest);

        // --- いちばん細かいブロック(一様)の値を熱くして、種にする ---
        const uint32_t finest = sim::LookupBlock(nest, CHAIN_LEVELS, CHAIN_CORNER, CHAIN_CORNER, CHAIN_CORNER);
        FX_ASSERT(finest != multires::MR_NO_BLOCK && multires::MrIsUniform(nest.blocks[finest]));
        nest.cells[finest] = MakeConductionAir(table, 1500000);
        nest.seeds[finest] = 1;

        return nest;
    }

    // --- 比べる道具(T-0019 のテストから移した。T-0108 のテストも使う)---

    // 世界(本物の葉)のセルの温度の最小と最大(mK)
    struct TemperatureRange {
        int32_t low = INT32_MAX;
        int32_t high = INT32_MIN;
    };

    inline TemperatureRange RealTemperatures(const sim::MultiresNest& nest, const sim::ReactionTableView& view) {
        TemperatureRange range;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const multires::MrBlock& block = nest.blocks[slot];
            if (block.kind != multires::MR_BLOCK_REAL)
                continue;

            for (uint32_t index = 0; index < multires::MR_BLOCK_CELLS; ++index) {
                if (!multires::MrIsSteppedCell(block, index))
                    continue;

                const int32_t temperature = reaction::RxComputeThermal(view, sim::LoadNestCell(nest, slot, index))
                                                .temperature;
                range.low = std::min(range.low, temperature);
                range.high = std::max(range.high, temperature);
            }
        }

        return range;
    }

    inline uint32_t CountFractionBlocks(const sim::MultiresNest& nest) {
        uint32_t count = 0;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot)
            count += nest.blocks[slot].kind == multires::MR_BLOCK_REAL &&
                             nest.blocks[slot].fraction != multires::MR_NO_FRACTION
                         ? 1
                         : 0;

        return count;
    }

    // 論理のセル・端数・帳簿が一致するか
    inline bool SameWorld(const sim::MultiresNest& a, const sim::MultiresNest& b) {
        for (uint32_t slot = 0; slot < a.blocks.size(); ++slot) {
            for (uint32_t index = 0; index < multires::MR_BLOCK_CELLS; ++index) {
                if (sim::HashReactionCell(sim::LoadNestCell(a, slot, index)) !=
                    sim::HashReactionCell(sim::LoadNestCell(b, slot, index)))
                    return false;
            }
        }

        return sim::HashRealLeaves(a) == sim::HashRealLeaves(b) && a.ledger == b.ledger;
    }

    // fractions = 端数の枠の数
    inline sim::MultiresNest MakeChainNest(const sim::BakedReactionTable& table, uint32_t fractions) {
        return MakeChainNest(
            table, MakeMultiresCapacity(table, CHAIN_ROOTS + static_cast<uint32_t>(CHAIN_LEVELS) + 2, 0, fractions));
    }

}  // namespace bicameral::test
