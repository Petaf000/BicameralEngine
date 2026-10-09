// multires_uniform_scene.h — 一様なブロックのテスト(tests/multires_uniform_test.cpp・gpu_multires_uniform_test.cpp)の場面(T-0102・T-0103)。
// 場面: レベル 0 の根 2×2×2 個。枠 0・1 の根は全部が 900 K の木箱(一様で、反応が進むので最初の刻みに頁に広がる。数刻みで燃え尽きて
// どのセルも同じ値になり、静かになった刻みに畳まれて頁を返す)、残りは 300 K の空気(一様で反応しない = 頁を持たない)。
// 刻み 0 に空気の根(枠 7)の中の 1 点を UNIFORM_CHAIN_DEPTH 段まで細かくする(一様な親の子は一様なので、鎖は頁を使わない)。
// 粗くするのは静かな葉の要求だけ(T-0101)なので、鎖は N + 1 刻みごとに 1 段ずつ畳まれる。
// 頁を 1 つにすると、枠 1 の木箱は枠 0 の木箱が畳まれて頁が返るまで、毎刻み頁が足りずに刻まれず種に残る(MR_COUNTER_PAGE_SHORTAGE)。
// (600 K の木箱〔T-0102 まで〕は、O2 が 1 単位だけ残って進まないセルとそうでないセルに分かれ、一様に戻らない。BACKLOG の「進める規則が
// あるのにセルが変わらない」と同じ)
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "common/multires_activity.hlsli"
#include "multires_activity_scene.h"

namespace bicameral::test {

    constexpr uint32_t UNIFORM_ROOT_EDGE = 2;
    constexpr uint32_t UNIFORM_ROOTS = UNIFORM_ROOT_EDGE * UNIFORM_ROOT_EDGE * UNIFORM_ROOT_EDGE;
    constexpr uint32_t UNIFORM_WORLD_BLOCKS = UNIFORM_ROOTS + 24;
    constexpr uint32_t UNIFORM_FRACTIONS = 16;
    constexpr uint32_t UNIFORM_CRATE_ROOTS = 2;  // 枠 0・1
    constexpr uint32_t UNIFORM_CHAIN_ROOT = 7;   // 原点 (8, 8, 8)
    constexpr int32_t UNIFORM_CHAIN_DEPTH = 6;
    constexpr uint64_t UNIFORM_TICKS = (UNIFORM_CHAIN_DEPTH + 1) * (multires::MR_QUIET_TICKS + 1) + 4;

    // 900 K の木箱(体積の 1 割がセルロース、残りが空気。燃え尽きるとどのセルも同じ値になる)
    inline reaction::RxCell MakeCrateCell(const sim::BakedReactionTable& table, int32_t millikelvin = 900000) {
        const std::vector<sim::SpeciesAmount> crate = {{.species = table.SpeciesId("cellulose"), .amount = 38600000},
                                                       {.species = table.SpeciesId("oxygen"), .amount = 983000},
                                                       {.species = table.SpeciesId("nitrogen"), .amount = 3697000}};

        return sim::MakeReactionCell(table, crate, millikelvin);
    }

    // 根 8 個(枠 r の原点は (r % 2, r / 2 % 2, r / 4) × 8。枠 crateRoots より前が木箱〔crateMillikelvin〕)。pages は世界の頁の数
    inline sim::MultiresNest MakeUniformNest(const sim::BakedReactionTable& table, uint32_t pages,
                                             uint32_t crateRoots = UNIFORM_CRATE_ROOTS,
                                             int32_t crateMillikelvin = 900000) {
        sim::MultiresCapacity capacity = MakeMultiresCapacity(table, UNIFORM_WORLD_BLOCKS, 0, UNIFORM_FRACTIONS);
        capacity.pages = pages;
        sim::MultiresNest nest = sim::MakeMultiresNest(capacity);
        const std::vector<reaction::RxCell> air(multires::MR_BLOCK_CELLS, MakeAirCell(table));
        const std::vector<reaction::RxCell> crate(multires::MR_BLOCK_CELLS, MakeCrateCell(table, crateMillikelvin));
        for (uint32_t root = 0; root < UNIFORM_ROOTS; ++root) {
            const int64_t x = root % UNIFORM_ROOT_EDGE;
            const int64_t y = (root / UNIFORM_ROOT_EDGE) % UNIFORM_ROOT_EDGE;
            const int64_t z = root / (UNIFORM_ROOT_EDGE * UNIFORM_ROOT_EDGE);
            sim::PlaceRootBlock(nest, x * 8, y * 8, z * 8, root < crateRoots ? crate : air);
        }

        return nest;
    }

    // 空気の根(枠 7。原点 (8, 8, 8))の中の 1 点を level まで細かくする / level のブロックを粗くする要求
    inline multires::MrRequest MakeUniformChainRequest(uint32_t op, int32_t level) {
        constexpr int64_t FINEST = (int64_t{8 + 3} << UNIFORM_CHAIN_DEPTH) + 5;
        const auto shift = static_cast<uint32_t>(UNIFORM_CHAIN_DEPTH - level);

        return multires::MrMakeRequest(op, level, FINEST >> shift, FINEST >> shift, FINEST >> shift);
    }

    inline std::vector<multires::MrRequest> UniformRequestsAt(uint64_t tick) {
        if (tick != 0)
            return {};

        return {MakeUniformChainRequest(multires::MR_REQUEST_REFINE, UNIFORM_CHAIN_DEPTH)};
    }

    // 1 刻みの前半(CPU): 外からの要求 → 静かで一様な頁を畳む(T-0103)→ 静かな葉を粗くする要求 → 要求の処理。GPU も同じ順。後半は StepActive
    inline void BeginUniformTick(sim::MultiresNest& nest, uint64_t tick) {
        sim::SubmitRequests(nest, UniformRequestsAt(tick));
        sim::FoldQuietPages(nest, tick);
        sim::SubmitQuietCoarsenRequests(nest, tick);
        sim::ProcessRequests(nest);
    }

    // 許容差つきで畳む版(ほぼ同じ頁も畳み、ちょうど静かになった端数の枠を返す。T-0104・T-0112)
    inline void BeginUniformTick(sim::MultiresNest& nest, const sim::BakedReactionTable& table, uint64_t tick,
                                 const multires::MrFoldTolerance& tolerance) {
        sim::SubmitRequests(nest, UniformRequestsAt(tick));
        sim::FoldQuietPages(nest, table, tick, tolerance);
        sim::SubmitQuietCoarsenRequests(nest, table, tick, tolerance);
        sim::ProcessRequests(nest);
    }

    // 粗くした時に一様な親を頁に広げる場面(1 回だけの処理): 鎖を 2 段作り、2 段目の一様の値を変えてから粗くする。
    // energyDelta = 0 なら値が同じなので親は一様のまま。CPU の前半(変える前まで)を作る
    inline sim::MultiresNest MakeExpandParentNest(const sim::BakedReactionTable& table, int64_t energyDelta,
                                                  uint32_t crateRoots = UNIFORM_CRATE_ROOTS) {
        sim::MultiresNest nest = MakeUniformNest(table, UNIFORM_WORLD_BLOCKS, crateRoots);
        const multires::MrRequest refine = MakeUniformChainRequest(multires::MR_REQUEST_REFINE, 2);
        sim::SubmitRequests(nest, std::span(&refine, 1));
        sim::ProcessRequests(nest);

        // --- 2 段目の一様の値を変える(テストのための直接の書き換え。保存量は合わなくなる)---
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            if (nest.blocks[slot].kind == multires::MR_BLOCK_REAL && nest.blocks[slot].level == 2)
                nest.cells[slot].energy += energyDelta;
        }

        return nest;
    }

    inline multires::MrRequest ExpandParentCoarsenRequest() {
        return MakeUniformChainRequest(multires::MR_REQUEST_COARSEN, 2);
    }

    // 子に覆われた頁を畳む場面(T-0103。CPU の前半。木箱なし = 根は全部空気): MakeExpandParentNest(値を変えた)の 2 段目を粗くして 1 段目を頁に広げ、
    // もう一度 2 段目まで細かくする。1 段目は「覆われていないセル = 親の値・覆われたセル = 空」、2 段目は全部が粗くした値で、
    // どちらも頁を持ったまま一様。つついた刻み 0 から N + 1 刻み目(FOLD_COVERED_TICKS − 1)に両方畳まれる
    constexpr uint64_t FOLD_COVERED_TICKS = multires::MR_QUIET_TICKS + 2;

    inline sim::MultiresNest MakeFoldCoveredNest(const sim::BakedReactionTable& table) {
        sim::MultiresNest nest = MakeExpandParentNest(table, 1000, 0);
        const multires::MrRequest coarsen = ExpandParentCoarsenRequest();
        sim::SubmitRequests(nest, std::span(&coarsen, 1));
        sim::ProcessRequests(nest);

        const multires::MrRequest refine = MakeUniformChainRequest(multires::MR_REQUEST_REFINE, 2);
        sim::SubmitRequests(nest, std::span(&refine, 1));
        sim::ProcessRequests(nest);

        return nest;
    }

    // 子に覆われた頁を畳む場面の 1 刻みの前半(粗くする要求は作らない。後半は StepActive)
    inline void BeginFoldCoveredTick(sim::MultiresNest& nest, uint64_t tick) {
        sim::FoldQuietPages(nest, tick);
        sim::ProcessRequests(nest);
    }

    // --- ほぼ同じ頁を畳む(T-0104・T-0112。multires_uniform_test.cpp の CheckNearFoldUnit・gpu_multires_uniform_test.cpp)---
    // 根 3 つ(どれも頁を持つ): 枠 0 = 空気のセルのエネルギーと O2 が数単位ずつ違う(計器で測れない差)/ 枠 1 = 枠 0 の 1 セルだけ 2 K 熱い /
    // 枠 2 = 枠 0 と同じセルで、端数の枠を持つ。刻み NEAR_UNIT_TICK にちょうど静かになるよう忙しさの印を置き、1 回だけ畳む
    constexpr uint64_t NEAR_UNIT_TICK = 40;
    constexpr multires::MrFoldTolerance NEAR_UNIT_TOLERANCE = {.temperatureMk = 100, .amountShift = 20};

    struct NearFoldUnit {
        sim::MultiresNest nest;
        std::vector<reaction::RxCell> nearCells;
    };

    inline NearFoldUnit MakeNearFoldUnit(const sim::BakedReactionTable& table) {
        NearFoldUnit unit{.nest = sim::MakeMultiresNest(MakeMultiresCapacity(table, 4, 0, 4))};
        const reaction::RxCell air = MakeAirCell(table);
        const uint32_t oxygen = table.SpeciesId("oxygen");
        unit.nearCells.assign(multires::MR_BLOCK_CELLS, air);
        for (uint32_t index = 0; index < multires::MR_BLOCK_CELLS; index += 7) {
            unit.nearCells[index].energy += index % 300;
            unit.nearCells[index] = reaction::RxAddSpecies(unit.nearCells[index], oxygen, index % 4);
        }

        std::vector<reaction::RxCell> hot = unit.nearCells;
        const uint64_t heatCapacity = reaction::RxComputeThermal(table.View(), air).heatCapacity;  // nJ/K
        hot[100].energy += static_cast<int64_t>((2 * heatCapacity / 1000000) + 1);
        for (uint32_t root = 0; root < 3; ++root)
            sim::PlaceRootBlock(unit.nest, int64_t{root} * 8, 0, 0, root == 1 ? hot : unit.nearCells);

        // --- 枠 2 に端数の枠(エネルギーと O2 の端数を数セルに)---
        multires::MrBlock& owner = unit.nest.blocks[2];
        owner.fraction = unit.nest.freeFractions[--unit.nest.counters[multires::MR_COUNTER_FREE_FRACTIONS]];
        for (uint32_t index = 0; index < multires::MR_BLOCK_CELLS; ++index) {
            multires::MrFraction fraction = multires::MrMakeEmptyFraction();
            if (index % 5 == 0) {
                fraction.energy = 0xC000000000000000ull;
                fraction.speciesCount = 1;
                fraction.species[0] = oxygen;
                fraction.amounts[0] = 0x9000000000000000ull + index;
            }

            unit.nest.fractions[(size_t{owner.fraction} * multires::MR_BLOCK_CELLS) + index] = fraction;
        }

        for (uint32_t root = 0; root < 3; ++root)
            unit.nest.blocks[root].busyTick = multires::MrActivityMark(NEAR_UNIT_TICK) - (multires::MR_QUIET_TICKS + 1);

        return unit;
    }

}  // namespace bicameral::test
