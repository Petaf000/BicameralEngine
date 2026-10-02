// multires_test.cpp — 多重解像度の入れ子の細分(shaders/common/multires.hlsli・sim/multires_nest)の CPU のテスト(T-0017、17 §6 の基準 1〜2)。
//   - k = 0〜9 を 1 点の周りで入れ子に細かくし、50 刻み反応を進めて粗く戻す。全部の刻みで元素の数とエネルギーの合計がビット一致
//   - 2 回走らせて全部が一致(決定性)
//   - 21 段の鎖の往復では端数が落ちない。24 段では落ちる(端数 64bit の幅の確認。ADR-0015)
//   - 観察の影の鎖があってもなくても、世界(本物の葉)のハッシュ列が一致。影の子の合計は親 × 8 と一致
#include <cstdint>
#include <string_view>

#include "core/log.h"
#include "core/singleton.h"
#include "multires_test_scene.h"
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

    // --- 基準 1: 入れ子の細分の往復 ---

    uint64_t RunRealChain(const BakedReactionTable& table) {
        MultiresNest nest = test::MakeMultiresNestForTest(table);
        const auto finest = static_cast<int32_t>(test::MULTIRES_LEVELS);
        const ConservedTotals initial = ComputeConservedTotals(nest, table, finest);

        int mismatchedTicks = 0;
        for (uint64_t tick = 0; tick < test::MULTIRES_END_TICK; ++tick) {
            test::StepMultiresScene(nest, table, test::MultiresScenario::Real, tick);
            if (ComputeConservedTotals(nest, table, finest) != initial)
                ++mismatchedTicks;
        }

        Expect(mismatchedTicks == 0, "本物の鎖: 保存量の合計が全部の刻みで最初と一致");
        Expect(nest.counters[MR_COUNTER_LOST] == 0, "本物の鎖: 端数が落ちない");
        Expect(nest.counters[MR_COUNTER_OVERFLOW] == 0, "本物の鎖: 成分がインラインに収まる");
        Expect(nest.blocks[test::MULTIRES_REAL_SLOT].kind == MR_BLOCK_UNUSED, "本物の鎖: 戻した後は根だけ");
        Log(Channel::Sim, Level::Info, "本物の鎖(k = 0〜{}): 端数のブロック {} 個・合計の食い違い {} 刻み",
            test::MULTIRES_LEVELS, nest.counters[MR_COUNTER_FRACTION_BLOCKS], mismatchedTicks);

        return HashWholeNest(nest);
    }

    // levels 段の鎖を 1 刻目に作り、tickCount 刻み進めて戻す。落ちた端数の数を返す
    uint32_t RunDeepChain(const BakedReactionTable& table, uint32_t levels, uint64_t tickCount) {
        MultiresNest nest = MakeMultiresNest(levels + 1, 2 * levels);
        PlaceRootBlock(nest, 0, 0, 0, 0, 0, test::MakeMultiresRootCells(table));
        const auto finest = static_cast<int32_t>(levels);
        const ConservedTotals initial = ComputeConservedTotals(nest, table, finest);

        RefineChain(nest, 0, 1, levels, MR_BLOCK_REAL, test::MakeMultiresPoint(levels));
        for (uint64_t tick = 0; tick < tickCount; ++tick)
            StepNest(nest, table, test::MULTIRES_TEST_SEED, tick);

        CoarsenChain(nest, levels, levels);
        const bool same = ComputeConservedTotals(nest, table, finest) == initial;
        const uint32_t lost = nest.counters[MR_COUNTER_LOST];
        Log(Channel::Sim, Level::Info, "深い鎖 {} 段: 落ちた端数 {} 件・合計の一致 {}", levels, lost, same);
        if (lost == 0)
            Expect(same, "深い鎖: 端数が落ちなければ合計が一致");

        return lost;
    }

    // --- 基準 2: 見るだけなら変わらない ---

    struct ShadowFamilyCheck {
        bool sumsMatch = true;   // 子の合計が親 × 8
        bool hasDetail = false;  // 子どうしが違う
    };

    ShadowFamilyCheck CheckShadowFamily(const MultiresNest& nest, uint32_t slot, const RxCell& parent, uint32_t local) {
        const auto childAt = [&](uint32_t j) -> const RxCell& {
            return nest.cells[(size_t{slot} * MR_BLOCK_CELLS) + MrChildCell(local, j)];
        };

        ShadowFamilyCheck check;
        int64_t energy = 0;
        for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
            energy += childAt(j).energy;
            check.hasDetail |= HashReactionCell(childAt(j)) != HashReactionCell(childAt(0));
        }

        check.sumsMatch = energy == parent.energy * MR_CHILDREN_PER_CELL;
        for (uint32_t p = 0; p < parent.speciesCount; ++p) {
            uint64_t amount = 0;
            for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j)
                amount += MrAmountOf(childAt(j), parent.species[p]);

            check.sumsMatch &= amount == parent.amounts[p] * MR_CHILDREN_PER_CELL;
        }

        return check;
    }

    // 影の子 2³ の合計が親 × 8 と一致しない親のセルの数。detailCells には子どうしが違う親のセルの数を足す
    int CountShadowMismatches(const MultiresNest& nest, uint32_t& detailCells) {
        int mismatches = 0;
        for (uint32_t level = 0; level < test::MULTIRES_LEVELS; ++level) {
            const uint32_t slot = test::MULTIRES_SHADOW_SLOT + level;
            const MrBlock& shadow = nest.blocks[slot];
            for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
                const size_t parentAddress = (size_t{shadow.parent} * MR_BLOCK_CELLS) +
                                             MrOctantCell(shadow.parentOctant, local);
                const ShadowFamilyCheck check = CheckShadowFamily(nest, slot, nest.cells[parentAddress], local);
                mismatches += check.sumsMatch ? 0 : 1;
                detailCells += check.hasDetail ? 1 : 0;
            }
        }

        return mismatches;
    }

    void TestShadowLeavesWorldUnchanged(const BakedReactionTable& table) {
        MultiresNest plain = test::MakeMultiresNestForTest(table);
        MultiresNest observed = test::MakeMultiresNestForTest(table);
        int differentTicks = 0;
        int shadowMismatches = 0;
        uint32_t detailCells = 0;
        for (uint64_t tick = 0; tick < test::MULTIRES_END_TICK; ++tick) {
            test::StepMultiresScene(plain, table, test::MultiresScenario::None, tick);
            test::StepMultiresScene(observed, table, test::MultiresScenario::Shadow, tick);
            differentTicks += HashRealLeaves(plain) == HashRealLeaves(observed) ? 0 : 1;
            if (test::MultiresShadowExists(test::MultiresScenario::Shadow, tick))
                shadowMismatches += CountShadowMismatches(observed, detailCells);
        }

        Expect(differentTicks == 0, "影: 世界のハッシュ列が影なしと一致");
        Expect(shadowMismatches == 0, "影: 子の合計が親 × 8 と一致");
        Expect(observed.counters[MR_COUNTER_SHADOW_CLAMPED] == 0, "影: エネルギーの余裕が負にならない");
        Expect(detailCells > 0, "影: 細部が親と違う(子どうしが違う)");
        Log(Channel::Sim, Level::Info,
            "影の鎖: 世界のハッシュの食い違い {} 刻み・子の合計の食い違い {}・細部のある親のセル {}(延べ)",
            differentTicks, shadowMismatches, detailCells);
    }

    int Run() {
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table) {
            Log(Channel::Sim, Level::Error, "試験の表をベイクできない: {}", table.error());
            return 1;
        }

        const uint64_t first = RunRealChain(*table);
        const uint64_t second = RunRealChain(*table);
        Expect(first == second, "本物の鎖: 2 回の実行で全部が一致");
        Log(Channel::Sim, Level::Info, "本物の鎖の要約 {:016x}", first);

        Expect(RunDeepChain(*table, 21, 50) == 0, "21 段の往復で端数が落ちない");
        Expect(RunDeepChain(*table, 24, 50) > 0, "24 段では端数が落ちる(64bit の幅)");

        TestShadowLeavesWorldUnchanged(*table);

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "multires_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "multires_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
