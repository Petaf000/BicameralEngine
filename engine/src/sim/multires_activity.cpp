// multires_activity.cpp — 多重解像度の木の上の活性の CPU リファレンス(multires_nest.h の StepActive。17 §5「活性」。T-0100)。
// 種(前の刻みに進める反応の規則があった・木の変更でつつかれた)とその面の隣(multires_activity.hlsli)に印を付け、印のあるブロックだけ刻む。
// GPU(shaders/sim/multires_graph.hlsl の ActivitySeedNode → WakeFaceNode → ActivityStepNode)は同じことを並列に行う。
// 印は「この刻みの印と交換して、前と違えば初めて」なので、どの順に起こしても刻むブロックの集合・数える欄・印は同じになる。
// 面をたどる再帰も、たどる道(どの種のどの面からか)は順に依存しないので、上限で止まった数も同じになる。
// 刻んでセルが変わったブロックと、木の変更でつつかれたブロックには忙しさの印(busyTick)を書き、印が古い本物の葉を
// 粗くする要求を作る(SubmitQuietCoarsenRequests。GPU は multires_tree.hlsl の TreeQuiet。T-0101)。
// 一様なブロック(T-0102)は値 1 つで反応が進むかを調べ、進むなら刻んだ後に枠の順で頁に広げて刻む(GPU は TreeExpand → ExpandStepNode)。
// 頁を持つブロックがちょうど静かになった刻みに一様なら、値 1 つに戻して枠の順に頁を返す(FoldQuietPages。GPU は TreeFoldCheck → TreeFold。T-0103)。
#include <algorithm>

#include "common/multires_activity.hlsli"
#include "sim/multires_nest.h"
#include "sim/multires_nest_internal.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        using nest_detail::CellAt;
        using nest_detail::CpuTree;
        using nest_detail::FreePageAt;
        using nest_detail::UniformAt;

        // 刻む印を付ける。初めてなら数える
        void Schedule(MultiresNest& nest, uint32_t slot, uint32_t mark) {
            MrBlock& block = nest.blocks[slot];
            if (block.activeTick == mark)
                return;

            block.activeTick = mark;
            nest.counters[MR_COUNTER_SCHEDULED] += 1;
        }

        // 面の向き face に進んで入ったブロック(WakeFaceNode と同じ。remaining は残りの再帰の数)
        void WakeFace(MultiresNest& nest, uint32_t slot, uint32_t face, uint32_t remaining, uint32_t mark) {
            const MrBlock block = nest.blocks[slot];
            bool wakeSelf = false;
            for (uint32_t i = 0; i < MR_FACE_OCTANTS; ++i) {
                const uint32_t child = block.children[MrNearOctant(face, i)];
                if (child == MR_NO_BLOCK)
                    wakeSelf = true;
                else if (remaining == 0)
                    nest.counters[MR_COUNTER_WAKE_TOO_DEEP] += 1;
                else
                    WakeFace(nest, child, face, remaining - 1, mark);
            }

            if (wakeSelf)
                Schedule(nest, slot, mark);
        }

        // 種 1 つ: 自分と、(八分の一, 面) ごとの隣(ActivitySeedNode と同じ)
        void WakeSeed(MultiresNest& nest, uint32_t slot, uint32_t mark) {
            const CpuTree tree{.nest = &nest};
            const MrBlock block = nest.blocks[slot];
            if (block.busyTick == MR_BUSY_POKED)
                nest.blocks[slot].busyTick = mark;  // つつかれた刻みの印にする(T-0101)

            Schedule(nest, slot, mark);
            for (uint32_t check = 0; check < MR_WAKE_CHECKS; ++check) {
                const uint32_t face = check % MR_FACES;
                const MrWake wake = MrWakeAcross(tree, block, check / MR_FACES, face, nest.capacity.rootLevel);
                if (wake.schedule != MR_NO_BLOCK)
                    Schedule(nest, wake.schedule, mark);

                if (wake.descend != MR_NO_BLOCK)
                    WakeFace(nest, wake.descend, face, MR_MAX_WAKE_DEPTH, mark);
            }
        }

    }  // namespace

    void StepActive(MultiresNest& nest, const BakedReactionTable& table, uint64_t worldSeed, uint64_t tick,
                    const MultiresStepOptions& options) {
        const uint32_t worldBlocks = nest.capacity.worldBlocks;
        const uint32_t mark = MrActivityMark(tick);

        // --- 種とその面の隣に印を付ける(種は使い切る)---
        for (uint32_t slot = 0; slot < worldBlocks; ++slot) {
            if (nest.seeds[slot] != 0 && nest.blocks[slot].kind == MR_BLOCK_REAL)
                WakeSeed(nest, slot, mark);
        }

        std::ranges::fill(nest.seeds, uint8_t{0});

        // --- 印のある世界のブロックと、観察の枠の全部(活性に入れない。D-403)を刻む ---
        std::vector<uint8_t> stepped(nest.blocks.size(), 0);
        for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot)
            stepped[slot] = slot >= worldBlocks || nest.blocks[slot].activeTick == mark ? 1 : 0;

        const std::vector<nest_detail::BlockStepResult> results = nest_detail::StepBlocks(
            nest, table.View(), stepped, worldSeed, tick, options.conduction);

        // --- セルが変わったら忙しさの印(頁に広げたのも忙しい: 畳めるかを N 刻み後に調べる。T-0103)。
        //     進める規則があった・変わった(伝導。T-0019)なら次の種に ---
        for (uint32_t slot = 0; slot < worldBlocks; ++slot) {
            const nest_detail::BlockStepResult& result = results[slot];
            if (result.changed || result.expanded)
                nest.blocks[slot].busyTick = mark;

            if (result.possible || (options.conduction && result.changed))
                nest.seeds[slot] = 1;
        }
    }

    void FoldQuietPages(MultiresNest& nest, uint64_t tick) {
        const uint32_t mark = MrActivityMark(tick);
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            MrBlock& block = nest.blocks[slot];
            if (!MrWantsFoldCheck(block, mark))
                continue;

            // --- 一様か(覆われていないセルが全部同じ・覆われたセルは空)---
            const uint32_t valueCell = MrFoldValueCell(block);
            const RxCell value = valueCell < MR_BLOCK_CELLS ? CellAt(nest, slot, valueCell) : RxMakeEmptyCell(0);
            bool uniform = true;
            for (uint32_t index = 0; index < MR_BLOCK_CELLS && uniform; ++index)
                uniform = MrFoldsCell(block, value, index, CellAt(nest, slot, index));

            if (!uniform)
                continue;

            // --- 値 1 つに戻して、頁を空きのスタックに積む(枠の順)---
            FreePageAt(nest, nest.counters[MR_COUNTER_FREE_PAGES]++) = block.page;
            UniformAt(nest, slot) = value;
            block.page = MR_NO_PAGE;
            nest.counters[MR_COUNTER_FOLDED] += 1;
        }
    }

    void SubmitQuietCoarsenRequests(MultiresNest& nest, uint64_t tick) {
        const CpuTree tree{.nest = &nest};
        const uint32_t mark = MrActivityMark(tick);
        uint32_t& count = nest.counters[MR_COUNTER_REQUESTS];
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            if (!MrWantsQuietCoarsen(tree, slot, mark))
                continue;

            if (count == MR_MAX_REQUESTS) {
                nest.counters[MR_COUNTER_QUIET_DEFERRED] += 1;
                continue;
            }

            nest.requests[count++] = MrMakeQuietCoarsenRequest(nest.blocks[slot]);
            nest.counters[MR_COUNTER_QUIET_REQUESTS] += 1;
        }
    }

    std::vector<uint32_t> SeedSlots(const MultiresNest& nest) {
        std::vector<uint32_t> slots;
        for (uint32_t slot = 0; slot < nest.seeds.size(); ++slot) {
            if (nest.seeds[slot] != 0)
                slots.push_back(slot);
        }

        return slots;
    }

}  // namespace bicameral::sim
