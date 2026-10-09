// multires_test.cpp — 多重解像度の木(shaders/common/multires.hlsli・multires_tree.hlsli・sim/multires_nest)の CPU のテスト
// (T-0017、17 §6 の基準 1〜2。T-0018、17 §5 の木の管理)。
//   - k = 0〜9 を 1 点の周りで入れ子に細かくし、50 刻み反応を進めて粗く戻す。全部の刻みで元素の数とエネルギーの合計がビット一致
//   - 2 回走らせて全部が一致(決定性)・戻した後は枠が全部空きに戻る
//   - 21 段の鎖の往復では端数が落ちない。24 段では落ちる(端数 64bit の幅の確認。ADR-0015)が、「世界 + 帳簿」は一致
//   - 観察の影の鎖があってもなくても、世界(本物の葉)のハッシュ列が一致。影の子の合計は親 × 8 と一致
//   - たくさんの要求(取り合い・枠が足りない・無効・索引の作り直し)の場面で、「世界 + 帳簿」・索引・枠の数が毎刻み合う
//   - 粗くすると成分が入りきらない子(8 種ずつ違う子。tests/multires_limits_scene.h)は粗くせず、元素とエネルギーがビット単位で同じ(T-0022)
//   - セルの溢れを使う世界(T-0187): 同じ場面で粗くするのを断らず・9 種目の生成物を待たせず、保存はビット単位。
//     上限に当たらない場面(本物の鎖)は溢れを使わない世界と毎刻みビット一致
#include <algorithm>
#include <cstdint>
#include <string_view>

#include "core/log.h"
#include "core/singleton.h"
#include "multires_limits_scene.h"
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
        Expect(nest.counters[MR_COUNTER_FREE_BLOCKS] == test::MULTIRES_WORLD_BLOCKS - 1, "本物の鎖: 戻した後は根だけ");
        Expect(nest.counters[MR_COUNTER_FREE_FRACTIONS] +
                       (nest.blocks[test::MULTIRES_ROOT_SLOT].fraction != MR_NO_FRACTION ? 1u : 0u) ==
                   test::MULTIRES_FRACTION_CAPACITY,
               "本物の鎖: 端数の枠は根の分だけ使う");
        Expect(nest.counters[MR_COUNTER_GRANTED] == 1 + test::MULTIRES_LEVELS, "本物の鎖: 要求は全部適用");
        Expect(test::CountIndexMismatches(nest) == 0, "本物の鎖: 索引と枠の数が合う");
        Log(Channel::Sim, Level::Info, "本物の鎖(k = 0〜{}): 端数の枠の空き {}・合計の食い違い {} 刻み",
            test::MULTIRES_LEVELS, nest.counters[MR_COUNTER_FREE_FRACTIONS], mismatchedTicks);

        return HashWholeNest(nest);
    }

    // levels 段の鎖を 1 刻目に作り、tickCount 刻み進めて戻す。落ちた端数の数を返す
    uint32_t RunDeepChain(const BakedReactionTable& table, uint32_t levels, uint64_t tickCount) {
        MultiresNest nest = MakeMultiresNest(test::MakeMultiresCapacity(table, levels + 1, 0, 2 * levels));
        PlaceRootBlock(nest, 0, 0, 0, test::MakeMultiresRootCells(table));
        const auto finest = static_cast<int32_t>(levels);
        const ConservedTotals initial = ComputeConservedTotals(nest, table, finest);
        const MultiresPoint point = test::MakeMultiresPoint(levels);

        const MrRequest refine = test::MakeMultiresRequest(MR_REQUEST_REFINE, point, finest);
        SubmitRequests(nest, {&refine, 1});
        ProcessRequests(nest);
        for (uint64_t tick = 0; tick < tickCount; ++tick)
            StepNest(nest, table, test::MULTIRES_TEST_SEED, tick);

        // --- 1 刻み 1 段ずつ戻す ---
        for (auto level = finest; level > 0; --level) {
            const MrRequest coarsen = test::MakeMultiresRequest(MR_REQUEST_COARSEN, point, level);
            SubmitRequests(nest, {&coarsen, 1});
            ProcessRequests(nest);
        }

        const bool same = ComputeConservedTotals(nest, table, finest) == initial;
        const uint32_t lost = nest.counters[MR_COUNTER_LOST];
        uint64_t ledgerSum = 0;
        for (const uint64_t value : nest.ledger)
            ledgerSum += value;

        Log(Channel::Sim, Level::Info, "深い鎖 {} 段: 落ちた端数 {} 件・帳簿の合計 {}・世界 + 帳簿の一致 {}・適用 {}",
            levels, lost, ledgerSum, same, nest.counters[MR_COUNTER_GRANTED]);
        Expect(same, "深い鎖: 世界 + 帳簿の合計が一致");
        Expect(nest.counters[MR_COUNTER_GRANTED] == 1 + levels, "深い鎖: 要求は全部適用");
        Expect((lost == 0) == (ledgerSum == 0), "深い鎖: 落ちた端数は帳簿にある");
        Expect(nest.counters[MR_COUNTER_FREE_BLOCKS] == levels, "深い鎖: 戻した後は根だけ");

        return lost;
    }

    // --- 基準 2: 見るだけなら変わらない ---

    struct ShadowFamilyCheck {
        bool sumsMatch = true;   // 子の合計が親 × 8
        bool hasDetail = false;  // 子どうしが違う
    };

    ShadowFamilyCheck CheckShadowFamily(const MultiresNest& nest, uint32_t slot, const RxCell& parent, uint32_t local) {
        const auto childAt = [&](uint32_t j) {
            return LoadNestCell(nest, slot, MrChildCell(local, j));
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
                const RxCell parent = LoadNestCell(nest, shadow.parent, MrOctantCell(shadow.parentOctant, local));
                const ShadowFamilyCheck check = CheckShadowFamily(nest, slot, parent, local);
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

    // --- 木の管理: たくさんの要求 ---

    uint64_t RunStress(const BakedReactionTable& table) {
        MultiresNest nest = test::MakeStressNest(table);
        const ConservedTotals initial = ComputeConservedTotals(nest, table, test::STRESS_MAX_LEVEL);
        int mismatchedTicks = 0;
        uint32_t indexMismatches = 0;
        uint32_t maxUsed = 0;
        for (uint64_t tick = 0; tick < test::STRESS_TICKS; ++tick) {
            SubmitRequests(nest, test::MakeStressRequests(nest, tick));
            ProcessRequests(nest);
            StepNest(nest, table, test::STRESS_SEED, tick);
            mismatchedTicks += ComputeConservedTotals(nest, table, test::STRESS_MAX_LEVEL) == initial ? 0 : 1;
            indexMismatches += test::CountIndexMismatches(nest);
            maxUsed = std::max(maxUsed, test::STRESS_WORLD_BLOCKS - nest.counters[MR_COUNTER_FREE_BLOCKS]);
        }

        const auto& counters = nest.counters;
        Log(Channel::Sim, Level::Info,
            "たくさんの要求 {} 刻み: 適用 {}・済み {}・取り合い {}・枠不足 {}・無効 {}・落ちた端数 {}・帳簿の外 "
            "{}・使った枠の最大 {}/{}",
            test::STRESS_TICKS, counters[MR_COUNTER_GRANTED], counters[MR_COUNTER_ALREADY],
            counters[MR_COUNTER_CONFLICT], counters[MR_COUNTER_NO_SPACE], counters[MR_COUNTER_INVALID],
            counters[MR_COUNTER_LOST], counters[MR_COUNTER_LEDGER_OUTSIDE], maxUsed, test::STRESS_WORLD_BLOCKS);
        Expect(mismatchedTicks == 0, "たくさんの要求: 世界 + 帳簿の合計が毎刻み一致");
        Expect(indexMismatches == 0, "たくさんの要求: 索引と枠の数が毎刻み合う");
        Expect(counters[MR_COUNTER_INDEX_FULL] == 0 && counters[MR_COUNTER_LEDGER_OUTSIDE] == 0 &&
                   counters[MR_COUNTER_OVERFLOW] == 0,
               "たくさんの要求: 索引・帳簿・成分が溢れない");
        Expect(counters[MR_COUNTER_GRANTED] > 0 && counters[MR_COUNTER_CONFLICT] > 0 &&
                   counters[MR_COUNTER_NO_SPACE] > 0 && counters[MR_COUNTER_INVALID] > 0 &&
                   counters[MR_COUNTER_ALREADY] > 0 && counters[MR_COUNTER_LOST] > 0,
               "たくさんの要求: 適用・済み・取り合い・枠不足・無効・帳簿への落ち、を全部通る");

        return HashWholeNest(nest);
    }

    // --- 粗くすると成分が入りきらない(T-0022)---

    void TestCoarsenFull() {
        const auto table = BakeReactionTable(test::MakeLimitsTestTable());
        if (!table) {
            Expect(false, "上限の試験の表をベイクできる");
            return;
        }

        MultiresNest nest = test::MakeCoarsenFullNest(*table);
        Expect(nest.counters[MR_COUNTER_GRANTED] == 2, "入りきらない場面: 子を 2 つ作れた");
        const ConservedTotals initial = ComputeConservedTotals(nest, *table, 1);
        bool conserved = true;
        for (uint64_t tick = 0; tick < test::COARSEN_FULL_TICKS; ++tick) {
            SubmitRequests(nest, test::CoarsenFullRequestsAt(tick));
            ProcessRequests(nest);
            conserved = conserved && ComputeConservedTotals(nest, *table, 1) == initial;
        }

        Log(Channel::Sim, Level::Info, "入りきらない子を粗くする: 断った {}・適用 {}・捨てた成分 {}",
            nest.counters[MR_COUNTER_COARSEN_FULL], nest.counters[MR_COUNTER_GRANTED],
            nest.counters[MR_COUNTER_OVERFLOW]);
        Expect(conserved, "入りきらない場面: 元素とエネルギーが毎刻み同じ");
        Expect(nest.counters[MR_COUNTER_COARSEN_FULL] == 2, "入りきらない子を粗くする要求を 2 回とも断る");
        Expect(nest.counters[MR_COUNTER_GRANTED] == 3, "入りきる子は粗くする");
        Expect(nest.counters[MR_COUNTER_OVERFLOW] == 0, "粗くする時に成分を捨てない");
        Expect(LookupBlock(nest, 1, 0, 0, 0) != MR_NO_BLOCK && LookupBlock(nest, 1, 8, 0, 0) == MR_NO_BLOCK,
               "入りきらない子は残り、入りきる子は無くなる");
    }

    // 世界の刻みで上限に当たる(T-0163): 待たせる・選ぶを毎刻み数え、元素とエネルギーは毎刻み同じ
    void TestLimitsStep(bool conduction) {
        const auto table = BakeReactionTable(test::MakeLimitsTestTable());
        if (!table) {
            Expect(false, "上限の試験の表をベイクできる");
            return;
        }

        MultiresNest nest = test::MakeCoarsenFullNest(*table);
        const ConservedTotals initial = ComputeConservedTotals(nest, *table, 1);
        bool conserved = true;
        for (uint64_t tick = 0; tick < test::LIMITS_STEP_TICKS; ++tick) {
            StepNest(nest, *table, test::LIMITS_STEP_SEED, tick, test::LimitsStepOptions(conduction));
            conserved = conserved && ComputeConservedTotals(nest, *table, 1) == initial;
        }

        const uint32_t held = nest.counters[MR_COUNTER_LIMIT_PRODUCTS];
        const uint32_t limited = nest.counters[MR_COUNTER_LIMIT_CANDIDATES];
        Log(Channel::Sim, Level::Info,
            "世界の刻みで上限に当たる(伝導 {}): 待たせた (セル, 刻み) {}・選んだ (セル, 刻み) {}",
            conduction ? "あり" : "なし", held, limited);
        Expect(conserved, "上限に当たる世界の刻み: 元素とエネルギーが毎刻み同じ");
        Expect(held > 0, "上限に当たる世界の刻み: 9 種目の生成物を待たせた刻みを数える");
        Expect(limited > 0, "上限に当たる世界の刻み: 進む規則を選んだ刻みを数える");
    }

    // --- セルの溢れ(T-0187)---

    // 本物の葉のセルの成分の数の最大(溢れを含む)
    uint32_t MaxLeafSpecies(const MultiresNest& nest) {
        uint32_t most = 0;
        for (uint32_t slot = 0; slot < nest.blocks.size(); ++slot) {
            if (nest.blocks[slot].kind != MR_BLOCK_REAL)
                continue;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (MrIsSteppedCell(nest.blocks[slot], index))
                    most = std::max(most, LoadWideNestCell(nest, slot, index).speciesCount);
            }
        }

        return most;
    }

    // 入りきらない子も粗くする(断らない)。粗くした後の世界を刻んでも保存はビット単位
    void TestCoarsenFullWide() {
        const auto table = BakeReactionTable(test::MakeLimitsTestTable());
        if (!table) {
            Expect(false, "上限の試験の表をベイクできる");
            return;
        }

        MultiresNest nest = test::MakeCoarsenFullNest(*table);
        EnableWideCells(nest);
        const ConservedTotals initial = ComputeConservedTotals(nest, *table, 1);
        bool conserved = true;
        for (uint64_t tick = 0; tick < test::COARSEN_FULL_TICKS; ++tick) {
            SubmitRequests(nest, test::CoarsenFullRequestsAt(tick));
            ProcessRequests(nest);
            conserved = conserved && ComputeConservedTotals(nest, *table, 1) == initial;
        }

        const uint32_t coarsenedSpecies = MaxLeafSpecies(nest);
        for (uint64_t tick = 0; tick < test::LIMITS_STEP_TICKS; ++tick) {
            StepNest(nest, *table, test::LIMITS_STEP_SEED, tick, test::LimitsStepOptions(true));
            conserved = conserved && ComputeConservedTotals(nest, *table, 1) == initial;
        }

        Log(Channel::Sim, Level::Info,
            "溢れを使う世界で粗くする: 断った {}・適用 {}・粗くした後の最大の成分 {}・刻んだ後 {}",
            nest.counters[MR_COUNTER_COARSEN_FULL], nest.counters[MR_COUNTER_GRANTED], coarsenedSpecies,
            MaxLeafSpecies(nest));
        Expect(conserved, "溢れ: 粗くして刻んでも元素とエネルギーが毎刻み同じ");
        Expect(nest.counters[MR_COUNTER_COARSEN_FULL] == 0, "溢れ: 入りきらない子を粗くするのを断らない");
        // 刻み 0 の 2 つの要求は同じ親を取り合うので、先の豊かな子だけが粗くなる(もう 1 つは取り合いで後回し。刻み 1 には要求が無い)
        Expect(nest.counters[MR_COUNTER_GRANTED] == 3, "溢れ: 入りきらない豊かな子を粗くする");
        Expect(nest.counters[MR_COUNTER_OVERFLOW] == 0, "溢れ: 粗くする時に成分を捨てない");
        Expect(coarsenedSpecies == 2 * RX_MAX_CELL_SPECIES, "溢れ: 粗くした親のセルは 16 種(8 種ずつ違う子の和集合)");
        Expect(LookupBlock(nest, 1, 0, 0, 0) == MR_NO_BLOCK, "溢れ: 豊かな子は無くなる");
        Expect(nest.counters[MR_COUNTER_LIMIT_PRODUCTS] == 0, "溢れ: 刻んでも 9 種目の生成物を待たせない");
    }

    // 9 種目の生成物を待たせない(同じ場面を溢れを使う世界で刻む)
    void TestLimitsStepWide(bool conduction) {
        const auto table = BakeReactionTable(test::MakeLimitsTestTable());
        if (!table) {
            Expect(false, "上限の試験の表をベイクできる");
            return;
        }

        MultiresNest nest = test::MakeCoarsenFullNest(*table);
        EnableWideCells(nest);
        const ConservedTotals initial = ComputeConservedTotals(nest, *table, 1);
        bool conserved = true;
        for (uint64_t tick = 0; tick < test::LIMITS_STEP_TICKS; ++tick) {
            StepNest(nest, *table, test::LIMITS_STEP_SEED, tick, test::LimitsStepOptions(conduction));
            conserved = conserved && ComputeConservedTotals(nest, *table, 1) == initial;
        }

        const uint32_t most = MaxLeafSpecies(nest);
        Log(Channel::Sim, Level::Info, "溢れを使う世界の刻み(伝導 {}): 待たせた {}・選んだ {}・最大の成分 {}",
            conduction ? "あり" : "なし", nest.counters[MR_COUNTER_LIMIT_PRODUCTS],
            nest.counters[MR_COUNTER_LIMIT_CANDIDATES], most);
        Expect(conserved, "溢れの刻み: 元素とエネルギーが毎刻み同じ");
        Expect(nest.counters[MR_COUNTER_LIMIT_PRODUCTS] == 0, "溢れの刻み: 9 種目の生成物を待たせない");
        Expect(most > RX_MAX_CELL_SPECIES, "溢れの刻み: 9 種目の生成物ができる");
    }

    // 上限に当たらない場面(本物の鎖)は、溢れを使う世界と使わない世界が毎刻みビット一致
    void TestWideMatchesInline(const BakedReactionTable& table) {
        MultiresNest narrow = test::MakeMultiresNestForTest(table);
        MultiresNest wide = test::MakeMultiresNestForTest(table);
        EnableWideCells(wide);
        int mismatchedTicks = 0;
        for (uint64_t tick = 0; tick < test::MULTIRES_END_TICK; ++tick) {
            test::StepMultiresScene(narrow, table, test::MultiresScenario::Real, tick);
            test::StepMultiresScene(wide, table, test::MultiresScenario::Real, tick);
            const auto sameCell = [](const RxCell& a, const RxCell& b) {
                return RxSameCell(a, b);
            };
            const bool same = HashRealLeaves(narrow) == HashRealLeaves(wide) &&
                              std::ranges::equal(narrow.cells, wide.cells, sameCell) &&
                              narrow.counters == wide.counters && narrow.ledger == wide.ledger;
            mismatchedTicks += same ? 0 : 1;
        }

        Expect(mismatchedTicks == 0, "溢れ: 上限に当たらない場面は溢れを使わない世界と毎刻みビット一致");
        Expect(MaxLeafSpecies(wide) <= RX_MAX_CELL_SPECIES, "溢れ: 上限に当たらない場面は溢れを使わない");
    }

    int Run() {
        TestCoarsenFull();
        TestLimitsStep(false);
        TestLimitsStep(true);
        TestCoarsenFullWide();
        TestLimitsStepWide(false);
        TestLimitsStepWide(true);
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table) {
            Log(Channel::Sim, Level::Error, "試験の表をベイクできない: {}", table.error());
            return 1;
        }

        const uint64_t first = RunRealChain(*table);
        const uint64_t second = RunRealChain(*table);
        Expect(first == second, "本物の鎖: 2 回の実行で全部が一致");
        Log(Channel::Sim, Level::Info, "本物の鎖の要約 {:016x}", first);

        // 子どうしが違うのは燃えている途中(5 刻み)。燃え尽きると子はどれも同じになり(O2 を使い切る。T-0106 の前は
        // 取り合いの切り捨てで O2 が 0〜1 単位残って子が分かれていた)、端数が出ない
        Expect(RunDeepChain(*table, 21, 50) == 0, "21 段の往復で端数が落ちない");
        Expect(RunDeepChain(*table, 21, 5) == 0, "21 段の往復で端数が落ちない(燃えている途中)");
        Expect(RunDeepChain(*table, 24, 5) > 0, "24 段では端数が落ちる(64bit の幅)");

        TestShadowLeavesWorldUnchanged(*table);
        TestWideMatchesInline(*table);

        const uint64_t stressFirst = RunStress(*table);
        Expect(stressFirst == RunStress(*table), "たくさんの要求: 2 回の実行で全部が一致");
        Log(Channel::Sim, Level::Info, "たくさんの要求の要約 {:016x}", stressFirst);

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
