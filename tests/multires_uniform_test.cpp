// multires_uniform_test.cpp — 一様なブロック(見出しの枠とセルの頁を分け、一様なら値 1 つ)の CPU のテスト(T-0102)。
// 場面は tests/multires_uniform_scene.h。確かめること:
//   - 一様な木箱の根は最初の刻みに頁に広がり、各セルを 1 つずつ刻む素直な答え(頁も木も使わない)と毎刻み論理のセルが一致する
//   - 一様な空気の根の中の鎖は頁を使わず(使っている頁は木箱の 2 つだけ)、静かになると畳まれて根だけに戻る
//   - 「世界 + 帳簿」の保存量が毎刻み最初とビット一致・2 回の実行で全部が一致
//   - 頁が 1 つなら、2 つ目の木箱は毎刻み頁が足りずに刻まれず、種に残り、値が変わらない(数える欄 MR_COUNTER_PAGE_SHORTAGE)
//   - 一様な親へ値の違う一様な子を粗くすると親が頁に広がり、八分の一は粗くした値・残りは親の値になる。同じ値なら一様のまま
#include <algorithm>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
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
        bool starvedSeeded = true;    // 枠 1 が毎刻み種に残った
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
            run.starvedSeeded = run.starvedSeeded && nest.seeds[1] != 0;
        }

        run.crateChanged = !MrSameCell(LoadNestCell(nest, 0, 0), crate);
        run.starvedChanged = !MrSameCell(LoadNestCell(nest, 1, 0), crate);
        run.finalRealBlocks = CountRealBlocks(nest);
        run.expanded = nest.counters[MR_COUNTER_EXPANDED];
        run.shortage = nest.counters[MR_COUNTER_PAGE_SHORTAGE];
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

        Log(Channel::Sim, Level::Info,
            "頁が足りる: {} 刻み・広げた頁 {}・使った頁の最大 {}(本物のブロックの最大 {})・要約 {:016x}",
            test::UNIFORM_TICKS, first.expanded, first.maxUsedPages, first.maxRealBlocks, first.digest);
    }

    // 頁が 1 つ: 2 つ目の木箱は刻まれずに種に残る
    void CheckShortage(const BakedReactionTable& table) {
        const UniformRun run = RunUniform(table, 1);
        Expect(run.crateChanged, "頁が 1 つ: 1 つ目の木箱は燃えた");
        Expect(!run.starvedChanged && run.starvedSeeded, "頁が 1 つ: 2 つ目の木箱は刻まれず、毎刻み種に残った");
        Expect(run.shortage == test::UNIFORM_TICKS, "頁が 1 つ: 毎刻み頁の不足を数えた");
        Expect(run.conservationMismatches == 0, "頁が 1 つ: 保存量が毎刻み最初とビット一致");

        Log(Channel::Sim, Level::Info, "頁が 1 つ: 頁の不足 {} 回・要約 {:016x}", run.shortage, run.digest);
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

    int Run() {
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table) {
            Log(Channel::Sim, Level::Error, "multires_uniform_test: FAILED(表を作れない)");
            return 1;
        }

        CheckAmple(*table);
        CheckShortage(*table);
        CheckExpandParent(*table);

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
