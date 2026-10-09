// multires_limits_scene.h — 粗くすると成分が入りきらない場面(T-0022 の最初の段。multires_test・gpu_multires_test が同じ場面を作る)。
// 表は上限の試験の表(tests/reaction_limits_table.h)。根(レベル 0)のセルは込み合う(p01〜p08)と入りきらない(q01〜q08)が
// x の偶奇で交互。レベル 1 の子を 2 つ作り(八分の一 0 と 1)、八分の一 0 の子(豊かな子)は子のセルを x の偶奇で p と q に書き直す
// (2×2×2 の組ごとに 8 種ずつ違う子が 4 つずつ = 和集合 16 種)。もう 1 つの子は細かくしたまま(組の中は同じ = 入りきる)。
// 刻み 0 に両方を粗くする要求、刻み 1 に豊かな子をもう一度粗くする要求を出す。豊かな子は 2 回とも断られ(MR_STATUS_SPECIES_FULL)、
// もう 1 つは粗くなる。刻みは進めない(要求だけ)。同じ木を要求なしで刻むと、世界の刻みで上限に当たる(T-0163。LIMITS_STEP_*)
#pragma once

#include <cstdint>
#include <vector>

#include "multires_test_scene.h"
#include "reaction_limits_table.h"

namespace bicameral::test {

    constexpr uint64_t COARSEN_FULL_TICKS = 2;
    constexpr uint32_t COARSEN_FULL_WORLD_BLOCKS = 4;
    constexpr uint32_t COARSEN_FULL_FRACTIONS = 4;

    // レベル 1 の子の点(豊かな子は原点の八分の一、もう 1 つは x の隣の八分の一)
    inline sim::MultiresPoint CoarsenFullPoint(bool rich) {
        return {.x = rich ? 1 : 9, .y = 1, .z = 1, .level = 1};
    }

    inline sim::MultiresNest MakeCoarsenFullNest(const sim::BakedReactionTable& table) {
        sim::MultiresNest nest = sim::MakeMultiresNest(
            MakeMultiresCapacity(table, COARSEN_FULL_WORLD_BLOCKS, 0, COARSEN_FULL_FRACTIONS));
        std::vector<reaction::RxCell> rootCells(multires::MR_BLOCK_CELLS);
        for (uint32_t index = 0; index < rootCells.size(); ++index)
            rootCells[index] = MakeLimitsCell(table, index);

        sim::PlaceRootBlock(nest, 0, 0, 0, rootCells);

        // --- 子を 2 つ作る(同じ親を取り合うので 1 つずつ)---
        for (const bool rich : {true, false}) {
            const sim::MultiresPoint point = CoarsenFullPoint(rich);
            const multires::MrRequest refine = MakeMultiresRequest(multires::MR_REQUEST_REFINE, point, 1);
            sim::SubmitRequests(nest, {&refine, 1});
            sim::ProcessRequests(nest);
        }

        // --- 豊かな子のセルを書き直す(テストのための直接の書き換え。保存量は書いた後を基準にする)---
        const uint32_t slot = sim::LookupBlock(nest, 1, 0, 0, 0);
        if (slot == multires::MR_NO_BLOCK || multires::MrIsUniform(nest.blocks[slot]))
            return nest;  // 場面が作れていない(テストが子の数で確かめる)

        for (uint32_t index = 0; index < multires::MR_BLOCK_CELLS; ++index) {
            const size_t address = nest.blocks.size() + (size_t{nest.blocks[slot].page} * multires::MR_BLOCK_CELLS) +
                                   index;
            nest.cells[address] = MakeLimitsCell(table, index);
        }

        return nest;
    }

    // --- 世界の刻みで上限に当たる場面(T-0163): 同じ木(粗くする要求は出さない)を刻む。根と豊かな子の p のセルは毎刻み
    //     進む規則が 56 本(RX_LIMIT_CANDIDATES)、q のセルは q08 を使い切るまで q09 を作る規則を待たせる(RX_LIMIT_PRODUCTS)。
    //     伝導ありとなしの両方で刻み、上限の印の数(MR_COUNTER_LIMIT_*)も CPU と GPU で比べる ---
    constexpr uint64_t LIMITS_STEP_TICKS = 12;
    constexpr uint64_t LIMITS_STEP_SEED = 0x6c696d6974730163ull;

    inline sim::MultiresStepOptions LimitsStepOptions(bool conduction) {
        sim::MultiresStepOptions options;
        options.conduction = conduction;

        return options;
    }

    inline std::vector<multires::MrRequest> CoarsenFullRequestsAt(uint64_t tick) {
        std::vector<multires::MrRequest> requests = {
            MakeMultiresRequest(multires::MR_REQUEST_COARSEN, CoarsenFullPoint(true), 1)};
        if (tick == 0)
            requests.push_back(MakeMultiresRequest(multires::MR_REQUEST_COARSEN, CoarsenFullPoint(false), 1));

        return requests;
    }

}  // namespace bicameral::test
