// multires_test_scene.h — 多重解像度のテスト(tests/multires_test.cpp・gpu_multires_test.cpp)が同じ場面を作り、同じ順に進めるための道具(T-0017・T-0018)。
// 場面: レベル 0 の根のブロック(8³ セル = 4m 角。成分と温度はいろいろ)の中の 1 点の周りを、刻み REFINE_TICK に 9 段(k = 1〜9、1mm)
// 細かくする要求を出し、COARSEN_TICK から 1 刻み 1 段ずつ粗くする要求で戻す。影の場面は同じ鎖を観察の枠に影として作り、
// 毎刻み親に引き戻し、COARSEN_TICK に捨てる。
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
    constexpr uint32_t MULTIRES_LEVELS = 9;                           // k = 1〜9(0.5m → 1mm)
    constexpr uint32_t MULTIRES_WORLD_BLOCKS = 16;                    // 根 + 本物の鎖 9 段 + 余り
    constexpr uint32_t MULTIRES_ROOT_SLOT = 0;                        // 最初に空きのスタックから取る枠
    constexpr uint32_t MULTIRES_SHADOW_SLOT = MULTIRES_WORLD_BLOCKS;  // 影の鎖は観察の枠 16〜24
    constexpr uint32_t MULTIRES_FRACTION_CAPACITY = 32;
    constexpr uint64_t MULTIRES_REFINE_TICK = 10;
    constexpr uint64_t MULTIRES_COARSEN_TICK = 60;  // ここから 1 刻み 1 段ずつ戻す(9 刻み)
    constexpr uint64_t MULTIRES_END_TICK = 70;

    inline sim::MultiresCapacity MakeMultiresCapacity(const sim::BakedReactionTable& table, uint32_t worldBlocks,
                                                      uint32_t observerBlocks, uint32_t fractions) {
        uint32_t indexEntries = 1;
        while (indexEntries < 2 * worldBlocks)
            indexEntries *= 2;

        return {.worldBlocks = worldBlocks,
                .observerBlocks = observerBlocks,
                .fractions = fractions,
                .pages = worldBlocks,  // 全部の枠が頁を持っても足りる(頁の不足は起こさない。T-0102)
                .indexEntries = indexEntries,
                .ledgerColumns = 1 + static_cast<uint32_t>(table.species.size())};
    }

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
        sim::MultiresNest nest = sim::MakeMultiresNest(
            MakeMultiresCapacity(table, MULTIRES_WORLD_BLOCKS, MULTIRES_LEVELS, MULTIRES_FRACTION_CAPACITY));
        sim::PlaceRootBlock(nest, 0, 0, 0, MakeMultiresRootCells(table));

        return nest;
    }

    inline bool MultiresShadowExists(MultiresScenario scenario, uint64_t tick) {
        return scenario == MultiresScenario::Shadow && tick >= MULTIRES_REFINE_TICK && tick < MULTIRES_COARSEN_TICK;
    }

    // 点(最も細かいレベル)を、それより粗い level の座標にした要求
    inline multires::MrRequest MakeMultiresRequest(uint32_t op, const sim::MultiresPoint& point, int32_t level) {
        const auto shift = static_cast<uint32_t>(point.level - level);

        return multires::MrMakeRequest(op, level, point.x >> shift, point.y >> shift, point.z >> shift);
    }

    // この刻みの要求(本物の鎖: REFINE_TICK に 9 段を 1 つの要求で・COARSEN_TICK から 1 刻み 1 段)
    inline std::vector<multires::MrRequest> MultiresRequestsAt(MultiresScenario scenario, uint64_t tick) {
        const sim::MultiresPoint point = MakeMultiresPoint(MULTIRES_LEVELS);
        if (scenario != MultiresScenario::Real)
            return {};

        if (tick == MULTIRES_REFINE_TICK)
            return {MakeMultiresRequest(multires::MR_REQUEST_REFINE, point, point.level)};

        if (tick >= MULTIRES_COARSEN_TICK && tick < MULTIRES_COARSEN_TICK + MULTIRES_LEVELS) {
            const auto level = static_cast<int32_t>(MULTIRES_LEVELS - (tick - MULTIRES_COARSEN_TICK));
            return {MakeMultiresRequest(multires::MR_REQUEST_COARSEN, point, level)};
        }

        return {};
    }

    // 1 刻み(CPU リファレンス): 要求の処理と影の出来事 → 反応 → 影の引き戻し。GPU(sim::GpuMultires)も同じ順に記録する
    inline void StepMultiresScene(sim::MultiresNest& nest, const sim::BakedReactionTable& table,
                                  MultiresScenario scenario, uint64_t tick,
                                  const sim::MultiresStepOptions& options = {}) {
        const sim::MultiresPoint point = MakeMultiresPoint(MULTIRES_LEVELS);
        sim::SubmitRequests(nest, MultiresRequestsAt(scenario, tick));
        sim::ProcessRequests(nest);
        if (tick == MULTIRES_REFINE_TICK && scenario == MultiresScenario::Shadow)
            sim::RefineShadowChain(nest, MULTIRES_ROOT_SLOT, MULTIRES_SHADOW_SLOT, MULTIRES_LEVELS, point);
        else if (tick == MULTIRES_COARSEN_TICK && scenario == MultiresScenario::Shadow)
            sim::RemoveShadowChain(nest, MULTIRES_SHADOW_SLOT, MULTIRES_LEVELS);

        sim::StepNest(nest, table, MULTIRES_TEST_SEED, tick, options);
        if (MultiresShadowExists(scenario, tick))
            sim::PullBackShadowChain(nest, table, MULTIRES_SHADOW_SLOT, MULTIRES_LEVELS);
    }

    // --- たくさんの要求の場面(T-0018): 根 2×2×2・ホットスポットの周りを細かく・葉を粗く・ときどき無効な要求。
    //     STRESS_DRAIN_TICK からは葉を粗くするだけ ---

    constexpr uint64_t STRESS_SEED = 20261004;
    constexpr uint32_t STRESS_WORLD_BLOCKS = 96;
    constexpr uint32_t STRESS_FRACTIONS = 48;
    constexpr uint32_t STRESS_REQUESTS_PER_TICK = 10;
    constexpr int32_t STRESS_MAX_LEVEL = 26;  // 21 段より深いので、粗くすると端数が帳簿へ落ちる
    constexpr uint32_t STRESS_HOTSPOTS = 6;
    constexpr uint64_t STRESS_DRAIN_TICK = 80;  // ここからは葉を全部粗くする要求だけ(深い鎖が戻り、端数が帳簿へ落ちる)
    constexpr uint64_t STRESS_TICKS = 140;

    inline sim::MultiresNest MakeStressNest(const sim::BakedReactionTable& table) {
        sim::MultiresNest nest = sim::MakeMultiresNest(
            MakeMultiresCapacity(table, STRESS_WORLD_BLOCKS, 0, STRESS_FRACTIONS));
        const std::vector<reaction::RxCell> cells = MakeMultiresRootCells(table);
        for (uint32_t root = 0; root < 8; ++root) {
            sim::PlaceRootBlock(nest, int64_t{root & 1u} * 8, int64_t{(root >> 1) & 1u} * 8,
                                int64_t{(root >> 2) & 1u} * 8, cells);
        }

        return nest;
    }

    // 子の無い本物の(根でない)ブロックの枠の一覧(枠の順)
    inline std::vector<uint32_t> RealLeafSlots(const sim::MultiresNest& nest) {
        std::vector<uint32_t> leaves;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const multires::MrBlock& block = nest.blocks[slot];
            if (block.kind == multires::MR_BLOCK_REAL && block.parent != multires::MR_NO_BLOCK &&
                !multires::MrHasRealChild(block))
                leaves.push_back(slot);
        }

        return leaves;
    }

    // ホットスポット spot(根のどれかの燃えている木箱のセル (3, 4, 5) の中の点)を、レベル level まで細かくする要求
    inline multires::MrRequest MakeHotspotRequest(uint64_t spot, int32_t level) {
        const uint64_t root = spot % 8;
        const auto inCrate = [&](uint64_t rootBit, uint64_t cell, uint32_t purpose) {
            const uint64_t low = fx::FxHash64(STRESS_SEED, 0, spot, purpose) & ((uint64_t{1} << STRESS_MAX_LEVEL) - 1);
            return static_cast<int64_t>((((rootBit * 8) + cell) << STRESS_MAX_LEVEL) | low);
        };
        const sim::MultiresPoint point = {.x = inCrate(root & 1u, 3, 2),
                                          .y = inCrate((root >> 1) & 1u, 4, 3),
                                          .z = inCrate((root >> 2) & 1u, 5, 4),
                                          .level = STRESS_MAX_LEVEL};

        return MakeMultiresRequest(multires::MR_REQUEST_REFINE, point, level);
    }

    // この刻みの要求(今の木から決める。CPU と GPU の木は同じなので、どちらから作っても同じ)。
    // 1/16 は無効(根より粗い)・6/16 はホットスポットを細かく・残りは葉を粗く(STRESS_DRAIN_TICK からは葉を全部粗く)
    inline std::vector<multires::MrRequest> MakeStressRequests(const sim::MultiresNest& nest, uint64_t tick) {
        const std::vector<uint32_t> leaves = RealLeafSlots(nest);
        const bool drain = tick >= STRESS_DRAIN_TICK;
        std::vector<multires::MrRequest> requests;
        for (uint32_t r = 0; r < STRESS_REQUESTS_PER_TICK; ++r) {
            const uint64_t hash = fx::FxHash64(STRESS_SEED, tick, r, 1);
            const uint32_t kind = drain ? 15u : static_cast<uint32_t>(hash & 15u);
            if (kind == 0)
                requests.push_back(multires::MrMakeRequest(multires::MR_REQUEST_REFINE, -1, 0, 0, 0));
            else if (kind < 7)
                requests.push_back(MakeHotspotRequest((hash >> 8) % STRESS_HOTSPOTS,
                                                      static_cast<int32_t>(1 + ((hash >> 16) % STRESS_MAX_LEVEL))));
            else if (!leaves.empty()) {
                const multires::MrBlock& leaf = nest.blocks[leaves[(drain ? r : (hash >> 8)) % leaves.size()]];
                requests.push_back(multires::MrMakeRequest(multires::MR_REQUEST_COARSEN, leaf.level, leaf.originX + 1,
                                                           leaf.originY + 2, leaf.originZ + 3));
            }
        }

        return requests;
    }

    // 索引の食い違い(本物のブロックが引けない数)と、使っている枠 + 空き が世界の枠の数と合わない時の差
    inline uint32_t CountIndexMismatches(const sim::MultiresNest& nest) {
        uint32_t mismatches = 0;
        uint32_t used = 0;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const multires::MrBlock& block = nest.blocks[slot];
            if (block.kind != multires::MR_BLOCK_REAL)
                continue;

            ++used;
            const uint32_t found = sim::LookupBlock(nest, block.level, block.originX, block.originY, block.originZ);
            mismatches += found == slot ? 0 : 1;
        }

        const uint32_t free = nest.counters[multires::MR_COUNTER_FREE_BLOCKS];
        mismatches += used + free == nest.capacity.worldBlocks ? 0 : 1;

        return mismatches;
    }

}  // namespace bicameral::test
