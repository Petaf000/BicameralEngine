// multires_uniform_test.cpp — 一様なブロック(見出しの枠とセルの頁を分け、一様なら値 1 つ。T-0102)と、静かで一様になった頁を畳む(T-0103)
// CPU のテスト。場面は tests/multires_uniform_scene.h。確かめること:
//   - 一様な木箱の根は最初の刻みに頁に広がり、各セルを 1 つずつ刻む素直な答え(頁も木も使わない)と毎刻み論理のセルが一致する
//     (畳んだ後も一致する = 畳んでも論理のセルは変わらない)
//   - 一様な空気の根の中の鎖は頁を使わず(使っている頁は木箱の 2 つだけ)、静かになると畳まれて根だけに戻る
//   - 燃え尽きた木箱の根は静かになった刻みに畳まれ、使っている頁の数が最初(0)に戻る(T-0103)
//   - 「世界 + 帳簿」の保存量が毎刻み最初とビット一致・2 回の実行で全部が一致
//   - 頁が 1 つなら、2 つ目の木箱は 1 つ目が畳まれて頁が返るまで毎刻み頁が足りずに刻まれず、種に残る(MR_COUNTER_PAGE_SHORTAGE)。
//     頁が返った刻みに頁に広がって燃え、やがて畳まれる
//   - 一様な親へ値の違う一様な子を粗くすると親が頁に広がり、八分の一は粗くした値・残りは親の値になる。同じ値なら一様のまま
//   - 子に覆われた頁(覆われていないセルが同じ・覆われたセルは空)も畳まれ、論理のセルは変わらず、頁は枠の順に積まれる(T-0103)
//   - 600 K の木箱は燃え尽きてビット単位で一様に戻り畳まれる(T-0106 の後)。許容差つきでも結果は全部同じ(T-0104)
//   - ほぼ同じ頁(T-0104): 計器で測れない差の頁は平均の切り捨てで畳み、余りと端数の枠の端数を帳簿へ移して保存量がビット一致。
//     許容差を超える頁は畳まない。許容差なしなら今までどおり(畳まない・端数の枠を返さない)
#include <algorithm>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "multires_uniform_scene.h"
#include "sim/multires_nest.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::multires;
using namespace bicameral::reaction;
using namespace bicameral::sim;

namespace {

    int failureCount = 0;

    void Expect(bool condition, std::string_view text) {
        if (condition)
            return;

        Log(Channel::Sim, Level::Error, "FAILED: {}", text);
        ++failureCount;
    }

    constexpr uint32_t DENSE_ROOTS = test::UNIFORM_CHAIN_ROOT;  // 木を変えない根(枠 0〜6)

    // 素直な答え: 木を変えない根のセルを全部、1 つずつ刻む
    struct DenseRoots {
        std::vector<RxCell> cells;  // DENSE_ROOTS × 512

        static DenseRoots From(const MultiresNest& nest) {
            DenseRoots dense;
            for (uint32_t slot = 0; slot < DENSE_ROOTS; ++slot) {
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                    dense.cells.push_back(LoadNestCell(nest, slot, index));
            }

            return dense;
        }

        void Step(const MultiresNest& nest, const ReactionTableView& view, uint64_t tick) {
            for (uint32_t slot = 0; slot < DENSE_ROOTS; ++slot) {
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    RxCell& cell = cells[(size_t{slot} * MR_BLOCK_CELLS) + index];
                    cell = MrStepCell(view, cell, test::STRESS_SEED, tick, nest.blocks[slot], index);
                }
            }
        }

        [[nodiscard]] bool Matches(const MultiresNest& nest) const {
            for (uint32_t slot = 0; slot < DENSE_ROOTS; ++slot) {
                for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                    if (!MrSameCell(LoadNestCell(nest, slot, index), cells[(size_t{slot} * MR_BLOCK_CELLS) + index]))
                        return false;
                }
            }

            return true;
        }
    };

    uint32_t CountRealBlocks(const MultiresNest& nest) {
        uint32_t count = 0;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot)
            count += nest.blocks[slot].kind == MR_BLOCK_REAL ? 1 : 0;

        return count;
    }

    constexpr uint64_t NO_TICK = ~uint64_t{0};

    // 刻み tick の後に、枠 slot が頁を得た刻み・畳まれた刻み(初めての時だけ)を書く
    void NoteFold(const MultiresNest& nest, uint32_t slot, uint64_t tick, uint64_t& pageTick, uint64_t& foldTick) {
        const bool paged = !MrIsUniform(nest.blocks[slot]);
        if (paged && pageTick == NO_TICK)
            pageTick = tick;
        else if (!paged && pageTick != NO_TICK && foldTick == NO_TICK)
            foldTick = tick;
    }

    struct UniformRun {
        uint64_t digest = 0;
        uint32_t denseMismatches = 0;
        uint32_t conservationMismatches = 0;
        uint32_t maxUsedPages = 0;
        uint32_t maxRealBlocks = 0;
        uint32_t finalRealBlocks = 0;
        uint32_t expanded = 0;
        uint32_t shortage = 0;
        bool crateChanged = false;    // 枠 0 の木箱が変わった
        bool starvedChanged = false;  // 枠 1 の木箱が変わった
        bool starvedSeeded = true;    // 枠 1 が頁を得るまで毎刻み種に残った

        // --- 畳む(T-0103)---
        uint32_t folded = 0;
        uint32_t finalUsedPages = 0;
        uint64_t cratePageTick = NO_TICK;    // 枠 0 の木箱が初めて頁を得た刻み
        uint64_t crateFoldTick = NO_TICK;    // 枠 0 の木箱が畳まれた刻み
        uint64_t starvedPageTick = NO_TICK;  // 枠 1 の木箱が初めて頁を得た刻み
        uint64_t starvedFoldTick = NO_TICK;  // 枠 1 の木箱が畳まれた刻み
    };

    UniformRun RunUniform(const BakedReactionTable& table, uint32_t pages) {
        MultiresNest nest = test::MakeUniformNest(table, pages);
        const ReactionTableView view = table.View();
        const ConservedTotals initial = ComputeConservedTotals(nest, table, test::UNIFORM_CHAIN_DEPTH);
        const RxCell crate = test::MakeCrateCell(table);
        DenseRoots dense = DenseRoots::From(nest);

        UniformRun run;
        for (uint64_t tick = 0; tick < test::UNIFORM_TICKS; ++tick) {
            test::BeginUniformTick(nest, tick);
            StepActive(nest, table, test::STRESS_SEED, tick);
            dense.Step(nest, view, tick);

            run.denseMismatches += dense.Matches(nest) ? 0 : 1;
            run.conservationMismatches += ComputeConservedTotals(nest, table, test::UNIFORM_CHAIN_DEPTH) == initial ? 0
                                                                                                                    : 1;
            run.maxUsedPages = std::max(run.maxUsedPages, UsedWorldPages(nest));

            run.maxRealBlocks = std::max(run.maxRealBlocks, CountRealBlocks(nest));
            NoteFold(nest, 0, tick, run.cratePageTick, run.crateFoldTick);
            NoteFold(nest, 1, tick, run.starvedPageTick, run.starvedFoldTick);
            if (run.starvedPageTick == NO_TICK)
                run.starvedSeeded = run.starvedSeeded && nest.seeds[1] != 0;
        }

        run.crateChanged = !MrSameCell(LoadNestCell(nest, 0, 0), crate);
        run.starvedChanged = !MrSameCell(LoadNestCell(nest, 1, 0), crate);
        run.finalRealBlocks = CountRealBlocks(nest);
        run.expanded = nest.counters[MR_COUNTER_EXPANDED];
        run.shortage = nest.counters[MR_COUNTER_PAGE_SHORTAGE];
        run.folded = nest.counters[MR_COUNTER_FOLDED];
        run.finalUsedPages = UsedWorldPages(nest);
        run.digest = HashWholeNest(nest);

        return run;
    }

    // 頁が足りる: 素直な答えと一致・鎖は頁を使わない
    void CheckAmple(const BakedReactionTable& table) {
        const UniformRun first = RunUniform(table, test::UNIFORM_WORLD_BLOCKS);
        const UniformRun second = RunUniform(table, test::UNIFORM_WORLD_BLOCKS);
        Expect(first.digest == second.digest, "2 回の実行で全部が一致");
        Expect(first.denseMismatches == 0, "一様な根を刻んだ論理のセルが、1 つずつ刻む素直な答えと毎刻み一致");
        Expect(first.conservationMismatches == 0, "「世界 + 帳簿」の保存量が毎刻み最初とビット一致");
        Expect(first.expanded == test::UNIFORM_CRATE_ROOTS && first.maxUsedPages == test::UNIFORM_CRATE_ROOTS,
               "頁に広がったのは木箱の根 2 つだけ(空気の根と鎖は頁を使わない)");
        Expect(first.maxRealBlocks == test::UNIFORM_ROOTS + test::UNIFORM_CHAIN_DEPTH, "鎖を最後の段まで作った");
        Expect(first.finalRealBlocks == test::UNIFORM_ROOTS, "静かな鎖が畳まれて根だけに戻った");
        Expect(first.crateChanged && first.starvedChanged && first.shortage == 0, "木箱が両方燃えた・頁の不足なし");
        Expect(first.folded == test::UNIFORM_CRATE_ROOTS && first.finalUsedPages == 0 &&
                   first.crateFoldTick != NO_TICK && first.starvedFoldTick != NO_TICK,
               "燃え尽きた木箱の根が両方畳まれ、使っている頁の数が最初(0)に戻った");

        Log(Channel::Sim, Level::Info,
            "頁が足りる: {} 刻み・広げた頁 {}・使った頁の最大 {}(本物のブロックの最大 {})・畳んだ頁 {}(刻み {})・"
            "終わりの頁 {}・要約 {:016x}",
            test::UNIFORM_TICKS, first.expanded, first.maxUsedPages, first.maxRealBlocks, first.folded,
            first.crateFoldTick, first.finalUsedPages, first.digest);
    }

    // 頁が 1 つ: 2 つ目の木箱は、1 つ目が畳まれて頁が返るまで刻まれずに種に残る
    void CheckShortage(const BakedReactionTable& table) {
        const UniformRun run = RunUniform(table, 1);
        Expect(run.crateChanged && run.crateFoldTick != NO_TICK, "頁が 1 つ: 1 つ目の木箱は燃えて畳まれた");
        Expect(run.starvedSeeded && run.starvedPageTick == run.crateFoldTick,
               "頁が 1 つ: 2 つ目の木箱は頁が返るまで毎刻み種に残り、返った刻みに頁を得た");
        Expect(run.shortage == run.crateFoldTick, "頁が 1 つ: 頁が返るまで毎刻み頁の不足を数えた");
        Expect(run.starvedChanged && run.starvedFoldTick != NO_TICK && run.finalUsedPages == 0,
               "頁が 1 つ: 2 つ目の木箱も燃えて畳まれ、頁が空きに戻った");
        Expect(run.conservationMismatches == 0, "頁が 1 つ: 保存量が毎刻み最初とビット一致");

        Log(Channel::Sim, Level::Info,
            "頁が 1 つ: 頁の不足 {} 回・1 つ目が畳まれた刻み {}・2 つ目が畳まれた刻み {}・要約 {:016x}", run.shortage,
            run.crateFoldTick, run.starvedFoldTick, run.digest);
    }

    // 一様な親へ粗くする: 値が違えば親が頁に広がる
    void CheckExpandParent(const BakedReactionTable& table) {
        for (const int64_t delta : {int64_t{0}, int64_t{1000}}) {
            MultiresNest nest = test::MakeExpandParentNest(table, delta);
            uint32_t child = MR_NO_BLOCK;
            for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
                if (nest.blocks[slot].kind == MR_BLOCK_REAL && nest.blocks[slot].level == 2)
                    child = slot;
            }

            const uint32_t parent = nest.blocks[child].parent;
            const uint32_t octant = nest.blocks[child].parentOctant;
            const RxCell childValue = nest.cells[child];
            const RxCell parentValue = nest.cells[parent];
            const uint32_t pagesBefore = UsedWorldPages(nest);
            const MrRequest coarsen = test::ExpandParentCoarsenRequest();
            SubmitRequests(nest, std::span(&coarsen, 1));
            ProcessRequests(nest);

            MrChildren children{};
            for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
                children.cells[j] = childValue;
                children.fractions[j] = MrMakeEmptyFraction();
            }

            const RxCell coarsened = MrCoarsenCell(children).cell;
            bool cellsMatch = true;
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const RxCell expected = MrOctantOfCell(index) == octant ? coarsened : parentValue;
                cellsMatch = cellsMatch && MrSameCell(LoadNestCell(nest, parent, index), expected);
            }

            const bool expanded = !MrIsUniform(nest.blocks[parent]);
            const std::string label = std::format("一様な親へ粗くする(エネルギーの差 {})", delta);
            Expect(nest.counters[MR_COUNTER_GRANTED] == 2 && nest.blocks[child].kind == MR_BLOCK_UNUSED,
                   label + ": 粗くする要求が通った");
            Expect(cellsMatch, label + ": 親の八分の一は粗くした値、残りは親の値");
            Expect(expanded == (delta != 0) && UsedWorldPages(nest) == pagesBefore + (delta != 0 ? 1 : 0),
                   label + ": 値が違う時だけ親が頁に広がった");
        }
    }

    // 子に覆われた頁を畳む(T-0103): 1 段目(子がある)と 2 段目(葉)が同じ刻みに畳まれ、論理のセルは変わらない
    void CheckFoldCovered(const BakedReactionTable& table) {
        MultiresNest nest = test::MakeFoldCoveredNest(table);

        // --- 頁を持つ世界のブロック(枠の順)とその頁、全部の論理のセル ---
        std::vector<uint32_t> pages;
        bool hasCoveredPage = false;
        std::vector<RxCell> before;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (block.kind == MR_BLOCK_REAL && !MrIsUniform(block)) {
                pages.push_back(block.page);
                hasCoveredPage = hasCoveredPage || MrHasRealChild(block);
            }

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
                before.push_back(LoadNestCell(nest, slot, index));
        }

        // --- つついた刻み 0 から N + 1 刻み目まで ---
        const uint32_t freeBefore = nest.counters[MR_COUNTER_FREE_PAGES];
        uint64_t foldTick = NO_TICK;
        for (uint64_t tick = 0; tick < test::FOLD_COVERED_TICKS; ++tick) {
            test::BeginFoldCoveredTick(nest, tick);
            StepActive(nest, table, test::STRESS_SEED, tick);
            if (foldTick == NO_TICK && nest.counters[MR_COUNTER_FOLDED] != 0)
                foldTick = tick;
        }

        bool cellsSame = true;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                const RxCell& expected = before[(size_t{slot} * MR_BLOCK_CELLS) + index];
                cellsSame = cellsSame && MrSameCell(LoadNestCell(nest, slot, index), expected);
            }
        }

        bool stackOrder = nest.counters[MR_COUNTER_FREE_PAGES] == freeBefore + pages.size();
        for (size_t i = 0; i < pages.size() && stackOrder; ++i)
            stackOrder = nest.freeBlocks[nest.capacity.worldBlocks + freeBefore + i] == pages[i];

        Expect(pages.size() == 2 && hasCoveredPage,
               "子に覆われた頁を畳む: 場面の確認(頁を持つブロック 2 つ、1 つは子がある)");
        Expect(nest.counters[MR_COUNTER_FOLDED] == 2 && UsedWorldPages(nest) == 0 &&
                   foldTick == test::FOLD_COVERED_TICKS - 1,
               "子に覆われた頁を畳む: 2 つともちょうど静かになった刻みに畳まれた");
        Expect(cellsSame, "子に覆われた頁を畳む: 論理のセルは変わらない");
        Expect(stackOrder, "子に覆われた頁を畳む: 頁は枠の順に空きのスタックへ積まれた");
    }

    // --- ほぼ同じ頁を畳む(T-0104)---
    // 600 K の木箱(O2 が 1 単位だけ残るセルと残らないセルに分かれ、ビット単位では一様に戻らない。17 §5「頁を畳む」)を、
    // 許容差なしと許容差つきで進める。許容差つきでも「世界 + 帳簿」の保存量は毎刻みビット一致(余りは帳簿へ)
    struct NearFoldRun {
        uint32_t conservationMismatches = 0;
        uint32_t folded = 0;
        uint32_t finalUsedPages = 0;
        uint64_t crateFoldTick = NO_TICK;
        uint64_t cratePageTick = NO_TICK;
        uint32_t quietTicks = 0;  // 枠 0 の木箱が頁を持ち、静か(忙しさの印が N 刻みより古い)だった刻みの数
        uint64_t digest = 0;
    };

    constexpr uint64_t NEAR_FOLD_TICKS = 300;
    constexpr int32_t NEAR_FOLD_CRATE_MK = 600000;

    NearFoldRun RunNearFold(const BakedReactionTable& table, const MrFoldTolerance& tolerance) {
        MultiresNest nest = test::MakeUniformNest(table, test::UNIFORM_WORLD_BLOCKS, test::UNIFORM_CRATE_ROOTS,
                                                  NEAR_FOLD_CRATE_MK);
        const ConservedTotals initial = ComputeConservedTotals(nest, table, test::UNIFORM_CHAIN_DEPTH);
        NearFoldRun run;
        for (uint64_t tick = 0; tick < NEAR_FOLD_TICKS; ++tick) {
            SubmitRequests(nest, test::UniformRequestsAt(tick));
            FoldQuietPages(nest, table, tick, tolerance);
            SubmitQuietCoarsenRequests(nest, tick);
            ProcessRequests(nest);
            StepActive(nest, table, test::STRESS_SEED, tick);

            const MrBlock& crate = nest.blocks[0];
            run.quietTicks += !MrIsUniform(crate) && crate.busyTick != MR_BUSY_POKED &&
                                      MrActivityMark(tick + 1) - crate.busyTick > MR_QUIET_TICKS
                                  ? 1
                                  : 0;
            NoteFold(nest, 0, tick, run.cratePageTick, run.crateFoldTick);
            if (tick % 8 == 7 || tick + 1 == NEAR_FOLD_TICKS)
                run.conservationMismatches += ComputeConservedTotals(nest, table, test::UNIFORM_CHAIN_DEPTH) == initial
                                                  ? 0
                                                  : 1;
        }

        run.folded = nest.counters[MR_COUNTER_FOLDED];
        run.finalUsedPages = UsedWorldPages(nest);
        run.digest = HashWholeNest(nest);

        return run;
    }

    void CheckNearFold(const BakedReactionTable& table) {
        const NearFoldRun exact = RunNearFold(table, MrExactFoldTolerance());
        const MrFoldTolerance tolerance = {.temperatureMk = 100, .amountShift = 20};
        const NearFoldRun near = RunNearFold(table, tolerance);
        Expect(exact.conservationMismatches == 0, "600 K の木箱(許容差なし): 保存量が最初とビット一致");
        Expect(near.conservationMismatches == 0,
               "600 K の木箱(許容差つき): 余りを帳簿へ移しても保存量が最初とビット一致");
        Expect(exact.folded == test::UNIFORM_CRATE_ROOTS && exact.finalUsedPages == 0,
               "600 K の木箱: 燃え尽きてビット単位で一様に戻り、両方畳まれた(T-0106 の後)");
        Expect(near.digest == exact.digest,
               "600 K の木箱: ほぼ同じで畳む頁が無ければ、許容差つきでも許容差なしと全部が一致");

        for (const auto& [name, run] : {std::pair{"許容差なし", exact}, std::pair{"許容差 100 mK・量 >> 20", near}})
            Log(Channel::Sim, Level::Info,
                "600 K の木箱({}): {} 刻み・畳んだ頁 {}・最後の頁 {}・枠 0 が頁を得た刻み {}・畳まれた刻み "
                "{}・静かだった刻み {}",
                name, NEAR_FOLD_TICKS, run.folded, run.finalUsedPages, static_cast<int64_t>(run.cratePageTick),
                static_cast<int64_t>(run.crateFoldTick), run.quietTicks);
    }

    void CheckNearFoldUnit(const BakedReactionTable& table) {
        const MrFoldTolerance tolerance = test::NEAR_UNIT_TOLERANCE;
        test::NearFoldUnit exact = test::MakeNearFoldUnit(table);
        test::NearFoldUnit near = test::MakeNearFoldUnit(table);
        const ConservedTotals before = ComputeConservedTotals(near.nest, table, 0);
        const uint32_t freeFractions = near.nest.counters[MR_COUNTER_FREE_FRACTIONS];
        FoldQuietPages(exact.nest, table, test::NEAR_UNIT_TICK, MrExactFoldTolerance());
        FoldQuietPages(near.nest, table, test::NEAR_UNIT_TICK, tolerance);

        Expect(!MrIsUniform(exact.nest.blocks[0]) && !MrIsUniform(exact.nest.blocks[1]) &&
                   !MrIsUniform(exact.nest.blocks[2]) && exact.nest.blocks[2].fraction != MR_NO_FRACTION,
               "ほぼ同じ頁(許容差なし): どれも畳まず、端数の枠も返さない");
        Expect(ComputeConservedTotals(near.nest, table, 0) == before,
               "ほぼ同じ頁(許容差つき): 畳んで余りと端数を帳簿へ移しても保存量がビット一致");
        Expect(
            MrIsUniform(near.nest.blocks[0]) && !MrIsUniform(near.nest.blocks[1]) && MrIsUniform(near.nest.blocks[2]),
            "ほぼ同じ頁(許容差つき): 計器で測れない差の枠 0・2 は畳み、2 K 熱いセルのある枠 1 は畳まない");
        Expect(near.nest.blocks[2].fraction == MR_NO_FRACTION &&
                   near.nest.counters[MR_COUNTER_FREE_FRACTIONS] == freeFractions + 1,
               "ほぼ同じ頁(許容差つき): ちょうど静かになった端数の枠を返した");

        // --- 畳んだ値 = 平均の切り捨て ---
        int64_t energySum = 0;
        uint64_t oxygenSum = 0;
        const uint32_t oxygen = table.SpeciesId("oxygen");
        for (const RxCell& cell : near.nearCells) {
            energySum += cell.energy;
            for (uint32_t i = 0; i < cell.speciesCount; ++i)
                oxygenSum += cell.species[i] == oxygen ? cell.amounts[i] : 0;
        }

        const RxCell folded = LoadNestCell(near.nest, 0, 0);
        uint64_t foldedOxygen = 0;
        for (uint32_t i = 0; i < folded.speciesCount; ++i)
            foldedOxygen += folded.species[i] == oxygen ? folded.amounts[i] : 0;

        Expect(folded.energy == energySum / MR_BLOCK_CELLS && foldedOxygen == oxygenSum / MR_BLOCK_CELLS,
               "ほぼ同じ頁(許容差つき): 畳んだ値はエネルギーと O2 の平均の切り捨て");

        // --- 負のエネルギーの床の割り算(-11 ÷ 2 = -6 余り 1)---
        MrWide negative = MrMakeWide();
        negative = MrWideAdd(negative, static_cast<uint64_t>(int64_t{-5}), true, 0);
        negative = MrWideAdd(negative, static_cast<uint64_t>(int64_t{-6}), true, 0);
        const fx::FxDivResult division = MrFloorDivideEnergy(negative, 2);
        Expect(static_cast<int64_t>(division.quotient) == -6 && division.remainder == 1,
               "負のエネルギーの和の床の割り算と余り");
    }

    int Run() {
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table) {
            Log(Channel::Sim, Level::Error, "multires_uniform_test: FAILED(表を作れない)");
            return 1;
        }

        CheckAmple(*table);
        CheckShortage(*table);
        CheckExpandParent(*table);
        CheckFoldCovered(*table);
        CheckNearFold(*table);
        CheckNearFoldUnit(*table);

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "multires_uniform_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "multires_uniform_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
