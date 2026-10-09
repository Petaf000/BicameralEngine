// multires_quiet_scene.h — 静かなブロックを粗くするテスト(tests/multires_quiet_test.cpp・gpu_multires_quiet_test.cpp)の場面(T-0101)。
// 場面: 木の上の活性の場面(multires_activity_scene.h)と同じ根 4×4×4 個。燃え始めの木箱のセルの 6 点(根の角の 2×2×2)と、
// 反対の角の根の静かな空気の 4 点を QUIET_DEPTH 段まで細かくする(空気は 1 刻みに 1 点: 同じ根を取り合わないように)。
// 空気の点 1 は点 0 と同じ QUIET_DEPTH − 1 段のブロックの別の八分の一なので、葉どうしが兄弟になる。外からの要求はこれと
//   - 刻み QUIET_STUFF_TICK から 2 刻み: 無効な要求で一覧を埋める(静かな葉の要求が入らず次へ回る。回った兄弟は同じ刻みに静かになる)
//   - 刻み QUIET_REFINE_AGAIN_TICK: 空気の点 0 をもう一度細かくする(空いた枠を使い直す)
// だけで、粗くするのは全部「静かな葉を粗くする要求」(SubmitQuietCoarsenRequests / RecordQuietRequests)。
// 空気の鎖は作ってすぐ静かなので N + 1 刻みごとに 1 段ずつ畳まれ、木箱の鎖は燃えている間は残り、燃え尽きると畳まれる。
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "common/multires_activity.hlsli"
#include "multires_activity_scene.h"

namespace bicameral::test {

    constexpr int32_t QUIET_DEPTH = 6;
    constexpr uint32_t QUIET_CRATE_POINTS = 6;
    constexpr uint32_t QUIET_AIR_POINTS = 4;
    constexpr uint64_t QUIET_STUFF_TICK = multires::MR_QUIET_TICKS + 1;  // 空気の点 0 の葉が初めて静かになる刻み
    constexpr uint64_t QUIET_STUFF_TICKS = 2;
    constexpr uint64_t QUIET_REFINE_AGAIN_TICK = 150;
    constexpr int32_t QUIET_REFINE_AGAIN_DEPTH = 4;
    constexpr uint64_t QUIET_TICKS = 300;

    // 反対の角の根(原点 24, 24, 24)の空気のセルの中の点を、レベル level まで細かくする要求。点 0 と 1 は最も細かい
    // QUIET_DEPTH のレベルで隣どうし(同じ QUIET_DEPTH − 1 段のブロックの別の八分の一)、点 2・3 は根の別の八分の一
    inline multires::MrRequest MakeAirRequest(uint32_t point, int32_t level) {
        constexpr std::array<std::array<int64_t, 3>, QUIET_AIR_POINTS> FINEST = {
            {{(25 << QUIET_DEPTH) + 3, (25 << QUIET_DEPTH) + 3, (25 << QUIET_DEPTH) + 3},
             {(25 << QUIET_DEPTH) + 8 + 3, (25 << QUIET_DEPTH) + 3, (25 << QUIET_DEPTH) + 3},
             {(29 << QUIET_DEPTH) + 5, (25 << QUIET_DEPTH) + 5, (25 << QUIET_DEPTH) + 5},
             {(29 << QUIET_DEPTH) + 7, (29 << QUIET_DEPTH) + 7, (29 << QUIET_DEPTH) + 7}}};
        const auto shift = static_cast<uint32_t>(QUIET_DEPTH - level);

        return multires::MrMakeRequest(multires::MR_REQUEST_REFINE, level, FINEST[point][0] >> shift,
                                       FINEST[point][1] >> shift, FINEST[point][2] >> shift);
    }

    // この刻みの外からの要求
    inline std::vector<multires::MrRequest> QuietRequestsAt(uint64_t tick) {
        std::vector<multires::MrRequest> requests;
        if (tick == 0) {
            for (uint32_t spot = 0; spot < QUIET_CRATE_POINTS; ++spot)
                requests.push_back(MakeHotspotRequest(spot, QUIET_DEPTH));
        }

        if (tick < QUIET_AIR_POINTS)
            requests.push_back(MakeAirRequest(static_cast<uint32_t>(tick), QUIET_DEPTH));

        // 根より粗いレベルは無効(数えるだけ)
        if (tick >= QUIET_STUFF_TICK && tick < QUIET_STUFF_TICK + QUIET_STUFF_TICKS)
            requests.assign(multires::MR_MAX_REQUESTS,
                            multires::MrMakeRequest(multires::MR_REQUEST_REFINE, -1, 0, 0, 0));

        if (tick == QUIET_REFINE_AGAIN_TICK)
            requests.push_back(MakeAirRequest(0, QUIET_REFINE_AGAIN_DEPTH));

        return requests;
    }

    // 1 刻みの前半(CPU): 外からの要求 → 静かで一様な頁を畳む → 静かな葉を粗くする要求 → 要求の処理。GPU も同じ順
    inline void BeginQuietTick(sim::MultiresNest& nest, uint64_t tick, std::span<const multires::MrRequest> requests) {
        sim::SubmitRequests(nest, requests);
        sim::FoldQuietPages(nest, tick);
        sim::SubmitQuietCoarsenRequests(nest, tick);
        sim::ProcessRequests(nest);
    }

    // 本物のブロックの数
    inline uint32_t CountRealBlocks(const sim::MultiresNest& nest) {
        uint32_t count = 0;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot)
            count += nest.blocks[slot].kind == multires::MR_BLOCK_REAL ? 1 : 0;

        return count;
    }

    // --- 線の場面(T-0113。D-430。multires_quiet_test.cpp の RunLines・gpu_multires_quiet_test.cpp の RunLinesCompared)---
    // 空気の根 1 つ(枠 0。八分の一 7 の 1 セルだけエネルギーが 1 単位多いので頁を持つ)の八分の一 0〜3 を 1 段細かくし、
    // 葉 4 つの頁に x 方向の 1 セル幅の線(y = z = 3)などを書いて、静かになるのを待つ:
    //   葉 0 = 2 K 熱い線(計器で測れる。どの許容差でも粗くしない)
    //   葉 1 = エネルギーが 1 単位多い線 + 2 K 熱い 2×2×2 の組 1 つ(頁全体は畳めないが、組ごとは許容差の中 → 許容差つきなら粗くする。
    //          完全に同じでは粗くしない)
    //   葉 2 = 2 K 熱い 2×2×2 の組 1 つだけ(組の中はビット単位で同じ → いつも粗くする)
    //   葉 3 = O2 が 2 倍の線(測れる)
    // 葉 0 は粗くできないので、兄弟(八分の一の番号が大きい葉 1・2)に譲らない(T-0101 の兄弟の規則のままだと待ち続ける)
    constexpr uint32_t LINE_LEAVES = 4;
    constexpr uint64_t LINE_TICKS = (3 * (multires::MR_QUIET_TICKS + 1)) + 8;
    constexpr multires::MrFoldTolerance LINE_TOLERANCE = {.temperatureMk = 1,
                                                          .amountShift = 20};  // D-435(1 mK・約 100 万分の 1)

    // 葉 leaf がある八分の一の中の点(レベル 1)
    inline multires::MrRequest MakeLineLeafRequest(uint32_t leaf) {
        return multires::MrMakeRequest(multires::MR_REQUEST_REFINE, 1, ((leaf & 1) * 8) + 1,
                                       (((leaf >> 1) & 1) * 8) + 1, ((leaf >> 2) & 1) * 8 + 1);
    }

    // 世界の葉 leaf(八分の一 leaf の子)の枠
    inline uint32_t FindLineLeaf(const sim::MultiresNest& nest, uint32_t leaf) {
        return nest.blocks[0].children[leaf];
    }

    // 頁のセル(nest.cells の並びは [一様の値 × 枠][頁 × MR_BLOCK_CELLS])
    inline reaction::RxCell& LinePageCell(sim::MultiresNest& nest, uint32_t slot, uint32_t index) {
        return nest.cells[nest.blocks.size() + (size_t{nest.blocks[slot].page} * multires::MR_BLOCK_CELLS) + index];
    }

    // セルの物質 species の量(無ければ 0)
    inline uint64_t SpeciesAmountOf(const reaction::RxCell& cell, uint32_t species) {
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            if (cell.species[i] == species)
                return cell.amounts[i];
        }

        return 0;
    }

    // 2 K 熱くするエネルギー(セル cell の熱容量から)
    inline int64_t TwoKelvinEnergy(const sim::BakedReactionTable& table, const reaction::RxCell& cell) {
        const uint64_t heatCapacity = reaction::RxComputeThermal(table.View(), cell).heatCapacity;  // nJ/K

        return static_cast<int64_t>((2 * heatCapacity / 1000000) + 1);
    }

    inline sim::MultiresNest MakeLineNest(const sim::BakedReactionTable& table) {
        sim::MultiresNest nest = sim::MakeMultiresNest(MakeMultiresCapacity(table, 8, 0, 8));
        std::vector<reaction::RxCell> air(multires::MR_BLOCK_CELLS, MakeAirCell(table));
        air[multires::MR_BLOCK_CELLS - 1].energy += 1;
        sim::PlaceRootBlock(nest, 0, 0, 0, air);

        // --- 1 回の処理に 1 つの親は 1 つの要求しか通らないので、1 つずつ ---
        for (uint32_t leaf = 0; leaf < LINE_LEAVES; ++leaf) {
            const multires::MrRequest request = MakeLineLeafRequest(leaf);
            sim::SubmitRequests(nest, std::span(&request, 1));
            sim::ProcessRequests(nest);
        }

        // --- 葉の頁に線と組を書く(テストのための直接の書き換え。保存量は書いた後を基準にする)---
        const uint32_t oxygen = table.SpeciesId("oxygen");
        for (uint32_t leaf = 0; leaf < LINE_LEAVES; ++leaf) {
            const uint32_t slot = FindLineLeaf(nest, leaf);
            if (slot == multires::MR_NO_BLOCK || multires::MrIsUniform(nest.blocks[slot]))
                continue;  // 場面が作れていない(テストが LineNestReady で確かめる)

            const reaction::RxCell base = LinePageCell(nest, slot, 0);
            const int64_t hot = TwoKelvinEnergy(table, base);
            for (uint32_t x = 0; x < multires::MR_BLOCK_EDGE; ++x) {
                reaction::RxCell& cell = LinePageCell(nest, slot, multires::MrCellIndex(x, 3, 3));
                if (leaf == 0)
                    cell.energy += hot;
                else if (leaf == 1)
                    cell.energy += 1;
                else if (leaf == 3)
                    cell = reaction::RxAddSpecies(cell, oxygen, SpeciesAmountOf(cell, oxygen));
            }

            if (leaf != 1 && leaf != 2)
                continue;

            for (uint32_t member = 0; member < multires::MR_CHILDREN_PER_CELL; ++member)
                LinePageCell(nest, slot, multires::MrCoarsenGroupCell(0, member)).energy += hot;
        }

        return nest;
    }

    // 葉 4 つが頁を持って作れたか
    inline bool LineNestReady(const sim::MultiresNest& nest) {
        for (uint32_t leaf = 0; leaf < LINE_LEAVES; ++leaf) {
            const uint32_t slot = FindLineLeaf(nest, leaf);
            if (slot == multires::MR_NO_BLOCK || multires::MrIsUniform(nest.blocks[slot]))
                return false;
        }

        return true;
    }

    // 線の場面の 1 刻みの前半(CPU。後半は StepActive)。GPU も同じ順
    inline void BeginLineTick(sim::MultiresNest& nest, const sim::BakedReactionTable& table, uint64_t tick,
                              const multires::MrFoldTolerance& tolerance) {
        sim::FoldQuietPages(nest, table, tick, tolerance);
        sim::SubmitQuietCoarsenRequests(nest, table, tick, tolerance);
        sim::ProcessRequests(nest);
    }

    // 葉 leaf が粗くなっているか(八分の一に子が無い)
    inline bool LineLeafCoarsened(const sim::MultiresNest& nest, uint32_t leaf) {
        return FindLineLeaf(nest, leaf) == multires::MR_NO_BLOCK;
    }

    // 粗くするはずの葉(許容差つきなら 1・2、完全に同じなら 2)
    inline bool LineLeafShouldCoarsen(uint32_t leaf, bool exact) {
        return leaf == 2 || (leaf == 1 && !exact);
    }

}  // namespace bicameral::test
