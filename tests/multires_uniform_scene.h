// multires_uniform_scene.h — 一様なブロックのテスト(tests/multires_uniform_test.cpp・gpu_multires_uniform_test.cpp)の場面(T-0102)。
// 場面: レベル 0 の根 2×2×2 個。枠 0・1 の根は全部が燃え始めの木箱(一様で、反応が進むので最初の刻みに頁に広がる)、
// 残りは 300 K の空気(一様で反応しない = 頁を持たない)。刻み 0 に空気の根(枠 7)の中の 1 点を UNIFORM_CHAIN_DEPTH 段まで細かくする
// (一様な親の子は一様なので、鎖は頁を使わない)。粗くするのは静かな葉の要求だけ(T-0101)なので、鎖は N + 1 刻みごとに 1 段ずつ畳まれる。
// 頁を 1 つにすると、枠 1 の木箱は毎刻み頁が足りず、刻まれずに種に残る(MR_COUNTER_PAGE_SHORTAGE)。
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

    inline reaction::RxCell MakeCrateCell(const sim::BakedReactionTable& table) {
        return MakeMultiresRootCells(table)[multires::MrCellIndex(3, 4, 5)];
    }

    // 根 8 個(枠 r の原点は (r % 2, r / 2 % 2, r / 4) × 8)。pages は世界の頁の数
    inline sim::MultiresNest MakeUniformNest(const sim::BakedReactionTable& table, uint32_t pages) {
        sim::MultiresCapacity capacity = MakeMultiresCapacity(table, UNIFORM_WORLD_BLOCKS, 0, UNIFORM_FRACTIONS);
        capacity.pages = pages;
        sim::MultiresNest nest = sim::MakeMultiresNest(capacity);
        const std::vector<reaction::RxCell> air(multires::MR_BLOCK_CELLS, MakeAirCell(table));
        const std::vector<reaction::RxCell> crate(multires::MR_BLOCK_CELLS, MakeCrateCell(table));
        for (uint32_t root = 0; root < UNIFORM_ROOTS; ++root) {
            const int64_t x = root % UNIFORM_ROOT_EDGE;
            const int64_t y = (root / UNIFORM_ROOT_EDGE) % UNIFORM_ROOT_EDGE;
            const int64_t z = root / (UNIFORM_ROOT_EDGE * UNIFORM_ROOT_EDGE);
            sim::PlaceRootBlock(nest, x * 8, y * 8, z * 8, root < UNIFORM_CRATE_ROOTS ? crate : air);
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

    // 1 刻みの前半(CPU): 外からの要求 → 静かな葉を粗くする要求 → 要求の処理。GPU も同じ順。後半は StepActive
    inline void BeginUniformTick(sim::MultiresNest& nest, uint64_t tick) {
        sim::SubmitRequests(nest, UniformRequestsAt(tick));
        sim::SubmitQuietCoarsenRequests(nest, tick);
        sim::ProcessRequests(nest);
    }

    // 粗くした時に一様な親を頁に広げる場面(1 回だけの処理): 鎖を 2 段作り、2 段目の一様の値を変えてから粗くする。
    // energyDelta = 0 なら値が同じなので親は一様のまま。CPU の前半(変える前まで)を作る
    inline sim::MultiresNest MakeExpandParentNest(const sim::BakedReactionTable& table, int64_t energyDelta) {
        sim::MultiresNest nest = MakeUniformNest(table, UNIFORM_WORLD_BLOCKS);
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

}  // namespace bicameral::test
