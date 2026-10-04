// multires_activity_scene.h — 木の上の活性のテスト(tests/multires_activity_test.cpp・gpu_multires_activity_test.cpp)の場面と、
// 面の隣の総当たりの答え(T-0100)。
// 場面: レベル 0 の根 4×4×4 個(16m 角)。セルは 300 K の空気(反応しない = 静か)で、原点の角の 2×2×2 個の根だけセル (3, 4, 5) が
// 燃え始めの木箱。木箱の周りを、たくさんの要求の場面(T-0018)と同じ要求で細かく/粗くする(深さ 26 段まで)。
// 静かな根の 1 つ(反対の角)には観察の影の鎖(4 段)を置き、毎刻み引き戻す(観察の枠は活性に入れず全部刻む)。
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "common/multires_activity.hlsli"
#include "multires_test_scene.h"

namespace bicameral::test {

    constexpr uint32_t ACTIVITY_ROOT_EDGE = 4;
    constexpr uint32_t ACTIVITY_ROOTS = ACTIVITY_ROOT_EDGE * ACTIVITY_ROOT_EDGE * ACTIVITY_ROOT_EDGE;
    constexpr uint32_t ACTIVITY_WORLD_BLOCKS = ACTIVITY_ROOTS + 128;
    constexpr uint32_t ACTIVITY_FRACTIONS = 64;
    constexpr uint32_t ACTIVITY_SHADOW_LEVELS = 4;
    constexpr uint32_t ACTIVITY_SHADOW_PARENT = ACTIVITY_ROOTS - 1;  // 反対の角の根(要求が触らない)
    constexpr uint64_t ACTIVITY_SHADOW_TICK = 3;
    constexpr uint64_t ACTIVITY_SHADOW_END_TICK = 70;
    constexpr uint64_t ACTIVITY_TICKS = 120;  // STRESS_DRAIN_TICK(80)からは葉を粗くするだけ

    inline reaction::RxCell MakeAirCell(const sim::BakedReactionTable& table) {
        const std::vector<sim::SpeciesAmount> air = {{.species = table.SpeciesId("oxygen"), .amount = 1067000},
                                                     {.species = table.SpeciesId("nitrogen"), .amount = 4013000}};

        return sim::MakeReactionCell(table, air, 300000);
    }

    // 根 rootEdge³ 個(計測は大きい世界で)。世界の枠は根 + 128
    inline sim::MultiresNest MakeActivityNest(const sim::BakedReactionTable& table,
                                              uint32_t rootEdge = ACTIVITY_ROOT_EDGE) {
        const uint32_t roots = rootEdge * rootEdge * rootEdge;
        sim::MultiresNest nest = sim::MakeMultiresNest(
            MakeMultiresCapacity(table, roots + 128, ACTIVITY_SHADOW_LEVELS, ACTIVITY_FRACTIONS));
        const std::vector<reaction::RxCell> air(multires::MR_BLOCK_CELLS, MakeAirCell(table));
        std::vector<reaction::RxCell> crate = air;
        crate[multires::MrCellIndex(3, 4, 5)] = MakeMultiresRootCells(table)[multires::MrCellIndex(3, 4, 5)];

        for (uint32_t root = 0; root < roots; ++root) {
            const uint32_t x = root % rootEdge;
            const uint32_t y = (root / rootEdge) % rootEdge;
            const uint32_t z = root / (rootEdge * rootEdge);
            const bool corner = x < 2 && y < 2 && z < 2;
            sim::PlaceRootBlock(nest, int64_t{x} * 8, int64_t{y} * 8, int64_t{z} * 8, corner ? crate : air);
        }

        return nest;
    }

    // 影の鎖の点(影の親の根の真ん中のセルの中)
    inline sim::MultiresPoint ActivityShadowPoint() {
        constexpr int64_t CENTER = ((ACTIVITY_ROOT_EDGE - 1) * 8) + 4;
        constexpr int64_t SCALE = int64_t{1} << ACTIVITY_SHADOW_LEVELS;

        return {.x = (CENTER * SCALE) + 3,
                .y = (CENTER * SCALE) + 5,
                .z = (CENTER * SCALE) + 9,
                .level = static_cast<int32_t>(ACTIVITY_SHADOW_LEVELS)};
    }

    inline bool ActivityShadowExists(uint64_t tick) {
        return tick >= ACTIVITY_SHADOW_TICK && tick < ACTIVITY_SHADOW_END_TICK;
    }

    // 1 刻みの前半(CPU): 要求の処理と影の出来事。GPU も同じ順
    inline void BeginActivityTick(sim::MultiresNest& nest, uint64_t tick,
                                  std::span<const multires::MrRequest> requests) {
        const uint32_t shadowSlot = nest.capacity.worldBlocks;
        sim::SubmitRequests(nest, requests);
        sim::ProcessRequests(nest);
        if (tick == ACTIVITY_SHADOW_TICK)
            sim::RefineShadowChain(nest, ACTIVITY_SHADOW_PARENT, shadowSlot, ACTIVITY_SHADOW_LEVELS,
                                   ActivityShadowPoint());
        else if (tick == ACTIVITY_SHADOW_END_TICK)
            sim::RemoveShadowChain(nest, shadowSlot, ACTIVITY_SHADOW_LEVELS);
    }

    // 1 刻みの後半(CPU): 刻む(active なら活性だけ、でなければ全部)→ 影の引き戻し
    inline void EndActivityTick(sim::MultiresNest& nest, const sim::BakedReactionTable& table, uint64_t tick,
                                bool active) {
        if (active)
            sim::StepActive(nest, table, STRESS_SEED, tick);
        else
            sim::StepNest(nest, table, STRESS_SEED, tick);

        if (ActivityShadowExists(tick))
            sim::PullBackShadowChain(nest, table, nest.capacity.worldBlocks, ACTIVITY_SHADOW_LEVELS);
    }

    // --- 面の隣の総当たり(索引を使わない答え): ブロックの覆われていない八分の一の箱どうしが面で接するか ---

    struct OctantBox {
        int32_t level = 0;
        std::array<int64_t, 3> low{};  // そのレベルのセルの単位。一辺 4
    };

    inline std::vector<OctantBox> UncoveredBoxes(const multires::MrBlock& block) {
        std::vector<OctantBox> boxes;
        for (uint32_t octant = 0; octant < 8; ++octant) {
            if (block.children[octant] != multires::MR_NO_BLOCK)
                continue;

            boxes.push_back(
                {.level = block.level,
                 .low = {block.originX + (int64_t{octant & 1u} * 4), block.originY + (int64_t{(octant >> 1) & 1u} * 4),
                         block.originZ + (int64_t{(octant >> 2) & 1u} * 4)}});
        }

        return boxes;
    }

    // 2 つの箱が面で接するか(1 軸でちょうど接し、残りの 2 軸で正の長さだけ重なる)。細かい方のレベルにそろえて比べる
    inline bool BoxesShareFace(const OctantBox& a, const OctantBox& b) {
        const int32_t level = std::max(a.level, b.level);
        const auto shiftA = static_cast<uint32_t>(level - a.level);
        const auto shiftB = static_cast<uint32_t>(level - b.level);
        uint32_t touching = 0;
        uint32_t overlapping = 0;
        for (uint32_t axis = 0; axis < 3; ++axis) {
            const int64_t lowA = a.low[axis] << shiftA;
            const int64_t highA = lowA + (int64_t{4} << shiftA);
            const int64_t lowB = b.low[axis] << shiftB;
            const int64_t highB = lowB + (int64_t{4} << shiftB);
            if (highA == lowB || highB == lowA)
                ++touching;
            else if (std::min(highA, highB) > std::max(lowA, lowB))
                ++overlapping;
        }

        return touching == 1 && overlapping == 2;
    }

    // 2 つのブロックの覆われていない八分の一の箱のどれかどうしが面で接するか
    inline bool BlocksShareFace(std::span<const OctantBox> mine, std::span<const OctantBox> theirs) {
        return std::ranges::any_of(mine, [&](const OctantBox& a) {
            return std::ranges::any_of(theirs, [&](const OctantBox& b) { return BoxesShareFace(a, b); });
        });
    }

    // 種(本物のブロック)とその面の隣の集合(枠ごとに 0 / 1)
    inline std::vector<uint8_t> BruteForceScheduled(const sim::MultiresNest& nest, std::span<const uint32_t> seeds) {
        const uint32_t worldBlocks = nest.capacity.worldBlocks;
        std::vector<std::vector<OctantBox>> boxes(worldBlocks);
        for (uint32_t slot = 0; slot < worldBlocks; ++slot) {
            if (nest.blocks[slot].kind == multires::MR_BLOCK_REAL)
                boxes[slot] = UncoveredBoxes(nest.blocks[slot]);
        }

        std::vector<uint8_t> scheduled(worldBlocks, 0);
        for (const uint32_t seed : seeds) {
            if (nest.blocks[seed].kind != multires::MR_BLOCK_REAL)
                continue;

            scheduled[seed] = 1;
            for (uint32_t other = 0; other < worldBlocks; ++other) {
                if (scheduled[other] == 0 && other != seed && BlocksShareFace(boxes[seed], boxes[other]))
                    scheduled[other] = 1;
            }
        }

        return scheduled;
    }

    // この刻みに印を付けた(刻んだ)世界のブロックの集合
    inline std::vector<uint8_t> ScheduledAt(const sim::MultiresNest& nest, uint64_t tick) {
        std::vector<uint8_t> scheduled(nest.capacity.worldBlocks, 0);
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot)
            scheduled[slot] = nest.blocks[slot].activeTick == multires::MrActivityMark(tick) ? 1 : 0;

        return scheduled;
    }

}  // namespace bicameral::test
