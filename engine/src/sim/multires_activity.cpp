// multires_activity.cpp — 多重解像度の木の上の活性の CPU リファレンス(multires_nest.h の StepActive。17 §5「活性」。T-0100)。
// 種(前の刻みに進める反応の規則があった・木の変更でつつかれた)とその面の隣(multires_activity.hlsli)に印を付け、印のあるブロックだけ刻む。
// GPU(shaders/sim/multires_graph.hlsl の ActivitySeedNode → WakeFaceNode → ActivityStepNode)は同じことを並列に行う。
// 印は「この刻みの印と交換して、前と違えば初めて」なので、どの順に起こしても刻むブロックの集合・数える欄・印は同じになる。
// 面をたどる再帰も、たどる道(どの種のどの面からか)は順に依存しないので、上限で止まった数も同じになる。
// 刻んでセルが変わったブロックと、木の変更でつつかれたブロックには忙しさの印(busyTick)を書き、印が古い本物の葉を
// 粗くする要求を作る(SubmitQuietCoarsenRequests。GPU は multires_tree.hlsl の TreeQuiet。T-0101)。粗くするのは子のセル 2×2×2 の組ごとの差が
// 許容差の中の葉だけ(静かになって初めての時に 1 回調べる。CheckQuietLeaves。GPU は TreeQuietCheck。D-430・T-0113)。
// 一様なブロック(T-0102)は値 1 つで反応が進むかを調べ、進むなら刻んだ後に枠の順で頁に広げて刻む(GPU は TreeExpand → ExpandStepNode)。
// 頁を持つブロックがちょうど静かになった刻みに一様なら、値 1 つに戻して枠の順に頁を返す(FoldQuietPages。GPU は TreeFoldCheck → TreeFold。T-0103)。
// 許容差を渡すと、ほぼ同じ頁も平均の値で畳み、切り捨ての余りを帳簿へ移す(T-0104。GPU も同じ段で。T-0112)。
// 反応の丸めは既定で待ちの丸め(ADR-0018。T-0115): 種 = 見出しを全部なめて起こす刻み(wakeTick)が来たブロック + つつかれたブロック。
// 刻んだ後に変わったブロックは忙しさの印(= tc)を書いて次の刻みに起こし、評価して変わらなければ次に評価の要る刻みを書く(RecordWaitResults)。
#include <algorithm>
#include <optional>

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

        // --- 頁を畳む(T-0103・T-0104)---

        // ビット単位で一様なら、その値(覆われていないセルが全部同じ・覆われたセルは空)
        // (溢れのある頁は畳まない。値 1 つはインラインの形なので。T-0187)
        std::optional<RxCell> ExactFoldValue(MultiresNest& nest, uint32_t slot) {
            const MrBlock& block = nest.blocks[slot];
            if (PageHasOverflow(nest, block.page))
                return std::nullopt;

            const uint32_t valueCell = MrFoldValueCell(block);
            const RxCell value = valueCell < MR_BLOCK_CELLS ? CellAt(nest, slot, valueCell) : RxMakeEmptyCell(0);
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (!MrFoldsCell(block, value, index, CellAt(nest, slot, index)))
                    return std::nullopt;
            }

            return value;
        }

        // 値 1 つに戻して、頁を空きのスタックに積む(枠の順)
        void FoldPage(MultiresNest& nest, uint32_t slot, const RxCell& value) {
            MrBlock& block = nest.blocks[slot];
            FreePageAt(nest, nest.counters[MR_COUNTER_FREE_PAGES]++) = block.page;
            UniformAt(nest, slot) = value;
            block.page = MR_NO_PAGE;
            nest.counters[MR_COUNTER_FOLDED] += 1;
        }

        // 帳簿の段 ledgerLevel に bits(その段の単位 × 2^-64)を足す。桁あふれ(= その段の 1 単位 = 1 段粗い段の 2^61)は粗い段へ繰り上げる
        void AddLedgerBits(MultiresNest& nest, int32_t ledgerLevel, uint32_t column, uint64_t bits) {
            for (; bits != 0; --ledgerLevel) {
                const uint32_t address = MrLedgerAddress(ledgerLevel, column, nest.capacity.ledgerColumns);
                if (address == MR_NO_BLOCK) {
                    nest.counters[MR_COUNTER_LEDGER_OUTSIDE] += 1;
                    return;
                }

                uint64_t& value = nest.ledger[address];
                value += bits;
                bits = value < bits ? uint64_t{1} << 61u : 0;
            }
        }

        // レベル level の units 単位(units < 512。畳んだ平均の切り捨ての余り)を帳簿へ: 3 段粗い段の units × 2^55
        void AddFoldRemainder(MultiresNest& nest, int32_t level, uint32_t column, uint64_t units) {
            FX_ASSERT(units < MR_BLOCK_CELLS);
            AddLedgerBits(nest, level - 3, column, units << 55u);
        }

        // 端数の枠のセルの端数を全部帳簿へ移し(端数はそのレベルの単位 × 2^-64 = 帳簿の同じ段の値)、枠を空きのスタックへ返す(T-0104)
        void ReturnFractionsToLedger(MultiresNest& nest, uint32_t slot) {
            MrBlock& block = nest.blocks[slot];
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const MrFraction fraction = nest_detail::FractionAt(nest, block.fraction, index);
                AddLedgerBits(nest, block.level, 0, fraction.energy);
                for (uint32_t i = 0; i < fraction.speciesCount; ++i)
                    AddLedgerBits(nest, block.level, 1 + fraction.species[i], fraction.amounts[i]);
            }

            nest.freeFractions[nest.counters[MR_COUNTER_FREE_FRACTIONS]++] = block.fraction;
            block.fraction = MR_NO_FRACTION;
        }

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

        // --- 静かな葉を粗くできるか(T-0113。D-430)---

        // MrCoarsenGroupWithin の Cells の約束(頁か一様の値のセル・温度)。完全に同じの時は温度を求めないので table は無くてよい
        struct QuietLeafCells {
            const MultiresNest* nest = nullptr;
            const ReactionTableView* table = nullptr;
            uint32_t slot = 0;

            [[nodiscard]] RxCell Cell(uint32_t index) const { return LoadNestCell(*nest, slot, index); }

            [[nodiscard]] int32_t Temperature(const RxCell& cell) const {
                return RxComputeThermal(*table, cell).temperature;
            }
        };

        // 葉を粗くしても、子のセル 2×2×2 の組のどれも許容差の中か(一様ならいつも)
        bool LeafCoarsenable(const MultiresNest& nest, const ReactionTableView* table, uint32_t slot,
                             const MrFoldTolerance& tolerance) {
            if (MrIsUniform(nest.blocks[slot]))
                return true;

            const QuietLeafCells cells{.nest = &nest, .table = table, .slot = slot};
            for (uint32_t group = 0; group < MR_OCTANT_CELLS; ++group) {
                if (!MrCoarsenGroupWithin(cells, group, tolerance))
                    return false;
            }

            return true;
        }

        // 今の忙しさの印でまだ調べていない静かな葉を調べ、結果を見出しの quietCheck に置く(GPU の TreeQuietCheck。要求を作る前に全部)
        void CheckQuietLeaves(MultiresNest& nest, const ReactionTableView* table, uint64_t tick,
                              const MrFoldTolerance& tolerance) {
            const uint64_t mark = MrChangeMark(tick);
            for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
                MrBlock& block = nest.blocks[slot];
                if (MrNeedsQuietCheck(block, mark))
                    block.quietCheck = MrQuietCheckStamp(block.busyTick, LeafCoarsenable(nest, table, slot, tolerance));
            }
        }

        // 静かな葉を調べてから、粗くできる葉の要求を枠の順に足す(table は許容差つきの時だけ要る)
        void SubmitQuietRequests(MultiresNest& nest, const ReactionTableView* table, uint64_t tick,
                                 const MrFoldTolerance& tolerance) {
            CheckQuietLeaves(nest, table, tick, tolerance);

            const CpuTree tree{.nest = &nest};
            const uint64_t mark = MrChangeMark(tick);
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
    }  // namespace

    void nest_detail::WakeAround(MultiresNest& nest, uint32_t slot, uint32_t mark) {
        const CpuTree tree{.nest = &nest};
        const MrBlock block = nest.blocks[slot];
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

    void StepActive(MultiresNest& nest, const BakedReactionTable& table, uint64_t worldSeed, uint64_t tick,
                    const MultiresStepOptions& options) {
        const uint32_t worldBlocks = nest.capacity.worldBlocks;
        const uint32_t mark = MrActivityMark(tick);

        // --- 種とその面の隣に印を付ける(種は使い切る)。見出しを全部なめ、起こす刻みが来たブロックも種に(待ちの丸め。T-0115)。
        //     つつかれた印は刻む前に ResolvePokes が「この刻みの直前」の印にする ---
        const uint64_t changeMark = MrChangeMark(tick);
        for (uint32_t slot = 0; slot < worldBlocks; ++slot) {
            const MrBlock& block = nest.blocks[slot];
            const bool due = block.wakeTick <= changeMark;
            if ((nest.seeds[slot] != 0 || due) && block.kind == MR_BLOCK_REAL)
                nest_detail::WakeAround(nest, slot, mark);
        }

        std::ranges::fill(nest.seeds, uint8_t{0});

        // --- 印のある世界のブロックと、観察の枠の全部(活性に入れない。D-403)を刻む ---
        std::vector<uint8_t> stepped(nest.blocks.size(), 0);
        for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot)
            stepped[slot] = slot >= worldBlocks || nest.blocks[slot].activeTick == mark ? 1 : 0;

        const std::vector<nest_detail::BlockStepResult> results = nest_detail::StepBlocks(
            nest, table.View(), stepped, worldSeed, tick, options, mark);

        // --- 待ちの丸め: 変わったブロックは忙しさの印(= tc)を書いて次の刻みに起こし、評価したブロックは起こす刻みを書く ---
        nest_detail::RecordWaitResults(nest, results, tick);
    }

    void FoldQuietPages(MultiresNest& nest, uint64_t tick) {
        const uint64_t mark = MrChangeMark(tick);
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            if (!MrWantsFoldCheck(nest.blocks[slot], mark))
                continue;

            const std::optional<RxCell> value = ExactFoldValue(nest, slot);
            if (value)
                FoldPage(nest, slot, *value);
        }
    }

    void FoldQuietPages(MultiresNest& nest, const BakedReactionTable& table, uint64_t tick,
                        const MrFoldTolerance& tolerance) {
        if (MrIsExactFold(tolerance)) {
            FoldQuietPages(nest, tick);
            return;
        }

        const ReactionTableView view = table.View();
        const uint64_t mark = MrChangeMark(tick);
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            // --- ちょうど静かになったら、端数の枠を帳簿へ移して返す(畳めるかはその後で調べる)---
            //     (溢れのある端数の枠は返さない。T-0187)
            if (MrWantsFractionReturn(nest.blocks[slot], mark) &&
                !FractionHasOverflow(nest, nest.blocks[slot].fraction))
                ReturnFractionsToLedger(nest, slot);

            const MrBlock& block = nest.blocks[slot];
            if (!MrWantsFoldCheck(block, mark))
                continue;

            // --- ビット単位で一様ならそのまま畳む(値が変わらないのでつつかない。T-0125)。全部覆われていれば(覆われていないセルが無い)
            //     ビット単位の判定だけ ---
            const std::optional<RxCell> exact = ExactFoldValue(nest, slot);
            if (exact) {
                FoldPage(nest, slot, *exact);
                continue;
            }

            if (MrFoldValueCell(block) == MR_BLOCK_CELLS || PageHasOverflow(nest, block.page))
                continue;

            const MrFoldStats stats = CollectFoldStats(nest, view, slot);
            if (!MrFoldStatsWithin(stats, tolerance))
                continue;

            // --- 平均の値で畳み、切り捨ての余りを帳簿へ(合計はビット一致)---
            const MrFoldValue value = MrFoldStatsValue(stats);
            AddFoldRemainder(nest, block.level, 0, value.energyRemainder);
            for (uint32_t i = 0; i < stats.speciesCount; ++i)
                AddFoldRemainder(nest, block.level, 1 + stats.species[i], value.amountRemainders[i]);

            FoldPage(nest, slot, value.cell);

            // --- セルの値が変わった(反応の速さ f も変わる)ので、つついて tc を書き直す(古い乱数・新しい f にしない。ADR-0018 追記 T-0125)---
            nest_detail::PokeBlock(nest, slot);
        }
    }

    MrFoldStats CollectFoldStats(const MultiresNest& nest, const ReactionTableView& table, uint32_t slot) {
        const MrBlock& block = nest.blocks[slot];
        MrFoldStats stats = MrMakeFoldStats();
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
            if (MrIsCoveredCell(block, index))
                continue;

            const RxCell cell = LoadNestCell(nest, slot, index);
            stats = MrAddFoldCell(stats, cell, RxComputeThermal(table, cell).temperature);
        }

        return stats;
    }

    void SubmitQuietCoarsenRequests(MultiresNest& nest, uint64_t tick) {
        SubmitQuietRequests(nest, nullptr, tick, MrExactFoldTolerance());
    }

    void SubmitQuietCoarsenRequests(MultiresNest& nest, const BakedReactionTable& table, uint64_t tick,
                                    const MrFoldTolerance& tolerance) {
        const ReactionTableView view = table.View();
        SubmitQuietRequests(nest, MrIsExactFold(tolerance) ? nullptr : &view, tick, tolerance);
    }

    std::vector<uint32_t> WaitSeedSlots(const MultiresNest& nest, uint64_t tick) {
        std::vector<uint32_t> slots;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (block.kind == MR_BLOCK_REAL && (nest.seeds[slot] != 0 || block.wakeTick <= MrChangeMark(tick)))
                slots.push_back(slot);
        }

        return slots;
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
