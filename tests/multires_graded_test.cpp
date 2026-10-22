// multires_graded_test.cpp — 木の細かくする・粗くする要求が、種からの距離で細かい所の半径と移り目の規則を読む CPU(sim/multires_graded。
// T-1008・ADR-0436)の試験。根はレベル −3(4 m のセル・32 m のブロック)、規則のいちばん細かいレベルは 0(0.5 m)。
// 1. 種が 1 つの世界: 種(点 P を含むいちばん細かい本物のブロック)からの距離で数刻み細かくすると規則の形に着く(もう要求が出ない)・
//    P の所は 0.5 m・保存量の合計が毎刻み同じ・2 回でハッシュ一致・半径を広げるとブロックが増える
// 2. 種の無い静かな世界: 規則を読む呼び出しを入れても入れなくても、刻み・静かな葉を粗くする木が毎刻みハッシュ一致(規則は何もしない)
// 3. 種を持ったまま刻む: 静かな葉を粗くする要求のうち規則に反するものを外すと、種の周りは 0.5 m のまま。外さないと粗くしては細かくし直す
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "multires_test_scene.h"
#include "reaction_test_table.h"
#include "sim/multires_graded.h"
#include "sim/multires_nest.h"

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

    constexpr int32_t ROOT_LEVEL = -3;
    constexpr int32_t FINEST_LEVEL = 0;
    constexpr uint32_t WORLD_BLOCKS = 800;
    constexpr std::array<int64_t, 3> POINT = {20, 37, 45};  // レベル 0 のセルの単位
    constexpr uint32_t GROW_TICKS = 64;
    constexpr uint64_t STEP_TICKS = 60;

    FineRegion SmallRegion(uint64_t radiusMm) {
        return {.radiusMm = radiusMm, .farLeafMm = 16000, .levelsPerDoubling = 1};
    }

    MultiresNest MakeNest(const BakedReactionTable& table) {
        MultiresCapacity capacity = test::MakeMultiresCapacity(table, WORLD_BLOCKS, 0, 4 * WORLD_BLOCKS);
        capacity.rootLevel = ROOT_LEVEL;
        MultiresNest nest = MakeMultiresNest(capacity);
        PlaceRootBlock(nest, 0, 0, 0, test::MakeMultiresRootCells(table));

        return nest;
    }

    // 点を含むいちばん細かい本物のブロック
    uint32_t DeepestSlot(const MultiresNest& nest, std::array<int64_t, 3> point) {
        for (int32_t level = FINEST_LEVEL; level >= ROOT_LEVEL; --level) {
            const auto shift = (uint32_t)(FINEST_LEVEL - level);
            const int64_t mask = ~(int64_t)(MR_BLOCK_EDGE - 1);
            const uint32_t slot = LookupBlock(nest, level, (point[0] >> shift) & mask, (point[1] >> shift) & mask,
                                              (point[2] >> shift) & mask);
            if (slot != MR_NO_BLOCK)
                return slot;
        }

        return MR_NO_BLOCK;
    }

    uint32_t UsedBlocks(const MultiresNest& nest) {
        return WORLD_BLOCKS - nest.counters[MR_COUNTER_FREE_BLOCKS];
    }

    // --- 1. 種が 1 つの世界で規則の形に育てる ---
    struct Grown {
        MultiresNest nest;
        uint32_t ticks = 0;  // 要求が出なくなった刻み(着かなければ GROW_TICKS)
        bool conserved = true;
    };

    Grown Grow(const BakedReactionTable& table, const FineRegion& region) {
        Grown grown{.nest = MakeNest(table)};
        MultiresNest& nest = grown.nest;
        const ConservedTotals initial = ComputeConservedTotals(nest, table, FINEST_LEVEL);
        grown.ticks = GROW_TICKS;
        for (uint32_t tick = 0; tick < GROW_TICKS; ++tick) {
            const std::array<uint32_t, 1> seeds = {DeepestSlot(nest, POINT)};
            const GradedRequestReport report = SubmitGradedRefineRequests(nest, region, FINEST_LEVEL, seeds);
            if (report.refines == 0 && report.deferred == 0) {
                grown.ticks = tick;
                break;
            }

            ProcessRequests(nest);
            grown.conserved = grown.conserved && ComputeConservedTotals(nest, table, FINEST_LEVEL) == initial;
        }

        return grown;
    }

    void TestGrow(const BakedReactionTable& table) {
        const Grown first = Grow(table, SmallRegion(4000));
        const Grown second = Grow(table, SmallRegion(4000));
        const Grown wide = Grow(table, SmallRegion(8000));
        const MultiresNest& nest = first.nest;
        const uint32_t deepest = DeepestSlot(nest, POINT);
        const uint32_t corner = DeepestSlot(nest, {63, 63, 63});
        Expect(first.ticks < GROW_TICKS, "種 1 つ: 規則の形に着かない");
        Expect(nest.blocks[deepest].level == FINEST_LEVEL, "種 1 つ: 種の所が 0.5 m でない");
        Expect(first.conserved, "種 1 つ: 細かくして保存量の合計が変わった");
        Expect(HashWholeNest(nest) == HashWholeNest(second.nest), "種 1 つ: 2 回でハッシュが違う");
        Expect(UsedBlocks(wide.nest) > UsedBlocks(nest), "種 1 つ: 半径を広げてもブロックが増えない");
        Log(Channel::Sim, Level::Info,
            "1. 種 1 つ(半径 4 m・遠くの葉 16 m): {} 刻みで着く・ブロック {}(半径 8 m で {})・種の所のレベル "
            "{}・遠い角のレベル {}・保存 {}",
            first.ticks, UsedBlocks(nest), UsedBlocks(wide.nest), nest.blocks[deepest].level, nest.blocks[corner].level,
            first.conserved ? "一致" : "崩れ");
    }

    // --- 2・3. 刻みながら(静かな葉を粗くする要求と、規則を読む呼び出し)---
    enum class Graded : uint8_t { None, RefineOnly, Both };

    struct StepRun {
        std::vector<uint64_t> hashes;
        uint32_t minBlocks = UINT32_MAX;
        uint32_t maxBlocks = 0;
        uint32_t removed = 0;  // 外した粗くする要求
        uint32_t refines = 0;
        int32_t seedLevelAtEnd = 0;
    };

    StepRun StepWithRule(const BakedReactionTable& table, MultiresNest nest, const FineRegion& region, bool holdSeed,
                         Graded graded) {
        StepRun run;
        for (uint64_t tick = 0; tick < STEP_TICKS; ++tick) {
            StepActive(nest, table, test::MULTIRES_TEST_SEED, tick);
            SubmitQuietCoarsenRequests(nest, tick);
            std::vector<uint32_t> seeds;
            if (holdSeed)
                seeds.push_back(DeepestSlot(nest, POINT));

            if (graded == Graded::Both)
                run.removed += FilterGradedCoarsenRequests(nest, region, FINEST_LEVEL, seeds);

            if (graded != Graded::None)
                run.refines += SubmitGradedRefineRequests(nest, region, FINEST_LEVEL, seeds).refines;

            ProcessRequests(nest);
            run.hashes.push_back(HashWholeNest(nest));
            run.minBlocks = std::min(run.minBlocks, UsedBlocks(nest));
            run.maxBlocks = std::max(run.maxBlocks, UsedBlocks(nest));
        }

        run.seedLevelAtEnd = nest.blocks[DeepestSlot(nest, POINT)].level;

        return run;
    }

    void TestSteps(const BakedReactionTable& table) {
        const FineRegion region = SmallRegion(4000);
        const Grown grown = Grow(table, region);

        // --- 2. 種の無い世界: 規則を読む呼び出しは何もしない ---
        const StepRun plain = StepWithRule(table, grown.nest, region, false, Graded::None);
        const StepRun ruled = StepWithRule(table, grown.nest, region, false, Graded::Both);
        Expect(plain.hashes == ruled.hashes, "種なし: 規則を読む呼び出しで木が変わった");
        Expect(ruled.removed == 0 && ruled.refines == 0, "種なし: 要求を足すか外した");
        Log(Channel::Sim, Level::Info, "2. 種なし: {} 刻みのハッシュ {}・ブロック {} → 最少 {}(静かな葉が粗くなる)",
            STEP_TICKS, plain.hashes == ruled.hashes ? "毎刻み一致" : "不一致", UsedBlocks(grown.nest),
            plain.minBlocks);

        // --- 3. 種を持ったまま: 外すと形を保つ・外さないと行き来する ---
        const StepRun held = StepWithRule(table, grown.nest, region, true, Graded::Both);
        const StepRun loose = StepWithRule(table, grown.nest, region, true, Graded::RefineOnly);
        const StepRun again = StepWithRule(table, grown.nest, region, true, Graded::Both);
        Expect(held.seedLevelAtEnd == FINEST_LEVEL, "種あり: 種の所が 0.5 m のままでない");
        Expect(held.hashes == again.hashes, "種あり: 2 回でハッシュが違う");
        Log(Channel::Sim, Level::Info,
            "3. 種あり: 外す = ブロック {}〜{}・外した要求 {}・足した要求 {}・種の所のレベル {} / 外さない = ブロック "
            "{}〜{}・足した要求 {}",
            held.minBlocks, held.maxBlocks, held.removed, held.refines, held.seedLevelAtEnd, loose.minBlocks,
            loose.maxBlocks, loose.refines);
    }

    int Run() {
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table) {
            Log(Channel::Sim, Level::Error, "試験の表をベイクできない: {}", table.error());
            return 1;
        }

        TestGrow(*table);
        TestSteps(*table);
        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "multires_graded_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "multires_graded_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
