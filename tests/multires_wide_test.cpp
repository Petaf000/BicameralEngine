// multires_wide_test.cpp — 溢れを使う多重解像度の世界(CPU)の、影の引き戻し・頁を畳む・端数を帳簿へ返す・静かな葉の許容差の試験(T-0199)。
//   - 影: 上限に当たらない場面(本物の鎖の影)は溢れを使わない世界と毎刻みビット一致。16 種の親の影も、子の合計が親 × 8(溢れの成分まで)・
//     世界は影なしと一致
//   - 畳む: 12 種(溢れのある)セルの頁をビット単位で畳み、一様の値の溢れごと細かくする・粗くする・伝導で頁に広げる。保存は毎刻みビット単位
//   - ほぼ同じ頁と端数: 12 種のほぼ同じ頁を平均の値で畳み、溢れのある端数の枠を帳簿へ返す。保存は毎刻みビット単位
//   - 静かな葉: 9 種目からの成分の量の差も許容差で比べる(インラインの 8 種だけ同じでも粗くしない)
//   - 一様な 8 種の根(T-0276): 9 種目を作る規則だけが進む一様なブロックも、溢れを使う世界は頁に広げて 9 種目を作る
//   - 熱の和(T-0272): 1 成分ずつ足す形(MrThermalSums。GPU の伝導の段の溢れを持つセル)が上限の無い形の MrCellThermal とビット単位で同じ
// 表は反応しない 12 種の表(この試験だけのデータ)と、上限の試験の表(tests/reaction_limits_table.h)・燃焼の試験の表。
#include <algorithm>
#include <cstdint>
#include <format>
#include <string_view>
#include <vector>

#include "common/multires_activity.hlsli"
#include "common/multires_conduction.hlsli"
#include "core/log.h"
#include "core/singleton.h"
#include "multires_limits_scene.h"
#include "multires_test_scene.h"
#include "multires_wide_scene.h"
#include "reaction_test_table.h"
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

    // --- 反応しない 12 種の表と、その上限の無いセル ---

    constexpr uint32_t INERT_SPECIES = 12;
    constexpr uint64_t WIDE_SEED = 0x7769646501990000ull;
    constexpr int32_t COLD_MILLIKELVIN = 300000;
    constexpr int32_t HOT_MILLIKELVIN = 400000;
    constexpr uint64_t QUIET_RUN_TICKS = MR_QUIET_TICKS + 4;  // ちょうど静かになる刻み(17)を越える

    BakedReactionTable MakeInertTable() {
        ReactionTableDefinition definition;
        definition.elements = {{.name = "X", .atomicMass = 10000}};
        for (uint32_t i = 1; i <= INERT_SPECIES; ++i)
            definition.species.push_back(test::LimitsSpecies(std::format("s{:02}", i), -700 * static_cast<int64_t>(i)));

        return BakeReactionTable(definition).value();
    }

    // 12 種のセル。量は 1 mol + extra(i) µmol(物質 i ごと)
    template <typename Extra>
    RxWideCell MakeInertCell(const BakedReactionTable& table, int32_t temperatureMk, Extra extra) {
        std::vector<SpeciesAmount> amounts;
        for (uint32_t i = 1; i <= INERT_SPECIES; ++i)
            amounts.push_back({.species = table.SpeciesId(std::format("s{:02}", i)), .amount = 1000000 + extra(i)});

        return MakeWideReactionCell(table, amounts, temperatureMk);
    }

    RxWideCell MakeEvenCell(const BakedReactionTable& table, int32_t temperatureMk) {
        return MakeInertCell(table, temperatureMk, [](uint32_t) { return uint64_t{0}; });
    }

    // 溢れを使う世界に、頁を持つ根(セルは全部 cellOf(index))を置く
    template <typename CellOf>
    MultiresNest MakeWideRootNest(const BakedReactionTable& table, CellOf cellOf) {
        MultiresNest nest = MakeMultiresNest(test::MakeMultiresCapacity(table, 4, 0, 4));
        EnableWideCells(nest);
        std::vector<RxCell> rootCells(MR_BLOCK_CELLS, RxMakeEmptyCell(0));
        rootCells[1] = RxMakeEmptyCell(1);  // 一様でない根(頁を取る)。中身はすぐ書き直す
        const uint32_t root = PlaceRootBlock(nest, 0, 0, 0, rootCells);
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
            StoreWidePageCell(nest, nest.blocks[root].page, index, cellOf(index));

        return nest;
    }

    // レベル 1 の原点の子への要求
    void RequestLevelOne(MultiresNest& nest, uint32_t op) {
        const MrRequest request = test::MakeMultiresRequest(op, {.x = 1, .y = 1, .z = 1, .level = 1}, 1);
        SubmitRequests(nest, {&request, 1});
        ProcessRequests(nest);
    }

    // --- 影 ---

    // 上限に当たらない場面(本物の鎖の影)は、溢れを使う世界と使わない世界が毎刻みビット一致(影のセルを含む)
    void TestShadowWideMatchesInline(const BakedReactionTable& table) {
        MultiresNest narrow = test::MakeMultiresNestForTest(table);
        MultiresNest wide = test::MakeMultiresNestForTest(table);
        EnableWideCells(wide);
        int mismatchedTicks = 0;
        for (uint64_t tick = 0; tick < test::MULTIRES_END_TICK; ++tick) {
            test::StepMultiresScene(narrow, table, test::MultiresScenario::Shadow, tick);
            test::StepMultiresScene(wide, table, test::MultiresScenario::Shadow, tick);
            const auto sameCell = [](const RxCell& a, const RxCell& b) {
                return RxSameCell(a, b);
            };
            const bool same = HashRealLeaves(narrow) == HashRealLeaves(wide) &&
                              std::ranges::equal(DenseCellImage(narrow), DenseCellImage(wide), sameCell) &&
                              narrow.counters == wide.counters && narrow.ledger == wide.ledger;
            mismatchedTicks += same ? 0 : 1;
        }

        Expect(mismatchedTicks == 0, "影: 上限に当たらない場面は溢れを使わない世界と毎刻みビット一致");
    }

    // 影の子 2³ の合計が親 × 8(溢れの成分まで)と合わない親のセルの数。mostSpecies に影のセルの成分の最大を書く
    int CountWideShadowMismatches(const MultiresNest& nest, uint32_t shadowSlot, uint32_t& mostSpecies) {
        const MrBlock& shadow = nest.blocks[shadowSlot];
        int mismatches = 0;
        for (uint32_t local = 0; local < MR_OCTANT_CELLS; ++local) {
            const RxWideCell parent = LoadWideNestCell(nest, shadow.parent, MrOctantCell(shadow.parentOctant, local));
            int64_t energy = 0;
            std::vector<uint64_t> amounts(parent.speciesCount, 0);
            for (uint32_t j = 0; j < MR_CHILDREN_PER_CELL; ++j) {
                const RxWideCell child = LoadWideNestCell(nest, shadowSlot, MrChildCell(local, j));
                mostSpecies = std::max(mostSpecies, child.speciesCount);
                energy += child.energy;
                for (uint32_t p = 0; p < parent.speciesCount; ++p)
                    amounts[p] += MrAmountOf(child, parent.species[p]);
            }

            bool same = energy == parent.energy * MR_CHILDREN_PER_CELL;
            for (uint32_t p = 0; p < parent.speciesCount; ++p)
                same = same && amounts[p] == parent.amounts[p] * MR_CHILDREN_PER_CELL;

            mismatches += same ? 0 : 1;
        }

        return mismatches;
    }

    // 16 種(8 種ずつ違う子を粗くした)親の影: 引き戻しが溢れの成分を落とさず、世界は影なしと同じ
    void TestShadowWideOverflow() {
        const auto table = BakeReactionTable(test::MakeLimitsTestTable());
        Expect(table.has_value(), "上限の試験の表をベイクできる");
        if (!table)
            return;

        MultiresNest plain = test::MakeCoarsenFullNest(*table, 1);
        EnableWideCells(plain);
        for (uint64_t tick = 0; tick < test::COARSEN_FULL_TICKS; ++tick) {
            SubmitRequests(plain, test::CoarsenFullRequestsAt(tick));
            ProcessRequests(plain);
        }

        MultiresNest observed = plain;
        const uint32_t root = LookupBlock(observed, 0, 0, 0, 0);
        const uint32_t shadowSlot = observed.capacity.worldBlocks;
        RefineShadowChain(observed, root, shadowSlot, 1, test::CoarsenFullPoint(true));

        int differentTicks = 0;
        int mismatches = 0;
        uint32_t mostSpecies = 0;
        for (uint64_t tick = 0; tick < test::LIMITS_STEP_TICKS; ++tick) {
            StepNest(plain, *table, test::LIMITS_STEP_SEED, tick);
            StepNest(observed, *table, test::LIMITS_STEP_SEED, tick);
            PullBackShadowChain(observed, *table, shadowSlot, 1);
            differentTicks += HashRealLeaves(plain) == HashRealLeaves(observed) ? 0 : 1;
            mismatches += CountWideShadowMismatches(observed, shadowSlot, mostSpecies);
        }

        Log(Channel::Sim, Level::Info,
            "溢れのある影: 世界の食い違い {} 刻み・子の合計の食い違い {}・影のセルの最大の成分 {}・余裕が負 {}",
            differentTicks, mismatches, mostSpecies, observed.counters[MR_COUNTER_SHADOW_CLAMPED]);
        Expect(differentTicks == 0, "溢れのある影: 世界のハッシュ列が影なしと一致");
        Expect(mismatches == 0, "溢れのある影: 子の合計が親 × 8 と一致(溢れの成分まで)");
        Expect(mostSpecies > RX_MAX_CELL_SPECIES, "溢れのある影: 影のセルが 9 種以上を持つ");
    }

    // --- 畳む(ビット単位)・一様の値の溢れ ---

    // 刻み first から count 刻み、活性で刻んで畳む。保存量が毎刻み initial と同じか
    bool RunFoldTicks(MultiresNest& nest, const BakedReactionTable& table, uint64_t first, uint64_t count,
                      const MrFoldTolerance& tolerance, const ConservedTotals& initial) {
        bool conserved = true;
        for (uint64_t tick = first; tick < first + count; ++tick) {
            StepActive(nest, table, WIDE_SEED, tick);
            FoldQuietPages(nest, table, tick, tolerance);
            conserved = conserved && ComputeConservedTotals(nest, table, 1) == initial;
        }

        return conserved;
    }

    void TestFoldWideExact(const BakedReactionTable& table) {
        const RxWideCell value = MakeEvenCell(table, COLD_MILLIKELVIN);
        MultiresNest nest = MakeWideRootNest(table, [&](uint32_t) { return value; });
        const uint32_t root = LookupBlock(nest, 0, 0, 0, 0);
        const uint32_t page = nest.blocks[root].page;
        const ConservedTotals initial = ComputeConservedTotals(nest, table, 1);

        bool conserved = RunFoldTicks(nest, table, 0, QUIET_RUN_TICKS, MrExactFoldTolerance(), initial);
        Expect(nest.counters[MR_COUNTER_FOLDED] == 1 && MrIsUniform(nest.blocks[root]), "畳む: 溢れのある頁を畳む");
        Expect(RxSameCell(LoadWideNestCell(nest, root, 7), value), "畳む: 一様の値は溢れまで同じ");
        Expect(!PageHasOverflow(nest, page) && UsedWorldPages(nest) == 0, "畳む: 返した頁の溢れは空");

        // --- 一様の値の溢れごと細かくし、粗くして戻す(頁を使わない)---
        RequestLevelOne(nest, MR_REQUEST_REFINE);
        const uint32_t child = LookupBlock(nest, 1, 0, 0, 0);
        Expect(child != MR_NO_BLOCK && MrIsUniform(nest.blocks[child]) &&
                   RxSameCell(LoadWideNestCell(nest, child, 0), value),
               "畳む: 細かくした子は一様の値を溢れごと受け継ぐ");
        conserved = conserved && ComputeConservedTotals(nest, table, 1) == initial;

        RequestLevelOne(nest, MR_REQUEST_COARSEN);
        Expect(LookupBlock(nest, 1, 0, 0, 0) == MR_NO_BLOCK && MrIsUniform(nest.blocks[root]) &&
                   UsedWorldPages(nest) == 0 && RxSameCell(LoadWideNestCell(nest, root, 0), value),
               "畳む: 一様な子を粗くしても親は一様のまま(溢れまで比べる)");
        conserved = conserved && ComputeConservedTotals(nest, table, 1) == initial;
        Expect(conserved, "畳む: 元素とエネルギーが毎刻み同じ");
    }

    // 温度の違う一様の値(溢れあり)どうしの面に流れがあると、両方を溢れごと頁に広げて伝導する
    void TestExpandWide(const BakedReactionTable& table) {
        const RxWideCell cold = MakeEvenCell(table, COLD_MILLIKELVIN);
        const RxWideCell hot = MakeEvenCell(table, HOT_MILLIKELVIN);
        MultiresNest nest = MakeWideRootNest(table, [&](uint32_t) { return cold; });
        RequestLevelOne(nest, MR_REQUEST_REFINE);
        const uint32_t child = LookupBlock(nest, 1, 0, 0, 0);
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
            StoreWidePageCell(nest, nest.blocks[child].page, index, hot);  // 試験のための直接の書き換え

        const ConservedTotals initial = ComputeConservedTotals(nest, table, 1);
        bool conserved = RunFoldTicks(nest, table, 0, QUIET_RUN_TICKS, MrExactFoldTolerance(), initial);
        Expect(nest.counters[MR_COUNTER_FOLDED] == 2 && UsedWorldPages(nest) == 0, "広げる: 根と子を畳む");

        MultiresStepOptions options;
        options.conduction = true;
        for (uint64_t tick = QUIET_RUN_TICKS; tick < QUIET_RUN_TICKS + 4; ++tick) {
            StepNest(nest, table, WIDE_SEED, tick, options);
            conserved = conserved && ComputeConservedTotals(nest, table, 1) == initial;
        }

        const RxWideCell after = LoadWideNestCell(nest, child, MrCellIndex(MR_BLOCK_EDGE - 1, 0, 0));  // 根との面のセル
        Log(Channel::Sim, Level::Info,
            "溢れのある一様の値を広げる: 畳んだ {}・広げた {}・子のセルの成分 {}・エネルギー {} → {}",
            nest.counters[MR_COUNTER_FOLDED], nest.counters[MR_COUNTER_EXPANDED], after.speciesCount, hot.energy,
            after.energy);
        Expect(nest.counters[MR_COUNTER_EXPANDED] >= 2, "広げる: 流れのある一様なブロックを頁に広げる");
        Expect(after.speciesCount == INERT_SPECIES, "広げる: 頁のセルは溢れの成分まで持つ");
        Expect(after.energy < hot.energy, "広げる: 根との面のセルから熱が流れる");
        Expect(conserved, "広げる: 元素とエネルギーが毎刻み同じ");
    }

    // --- ほぼ同じ頁と端数(許容差つき)---

    // 12 種のほぼ同じ頁を平均の値で畳み、粗くしてできた溢れのある端数の枠を帳簿へ返す
    void TestFoldWideNear(const BakedReactionTable& table) {
        MultiresNest nest = MakeWideRootNest(table, [&](uint32_t index) {
            return MakeInertCell(table, COLD_MILLIKELVIN, [&](uint32_t i) { return uint64_t{(index + i) % 3}; });
        });
        RequestLevelOne(nest, MR_REQUEST_REFINE);
        const uint32_t child = LookupBlock(nest, 1, 0, 0, 0);
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {  // 8 で割り切れない量(粗くすると端数ができる)
            const RxWideCell cell = MakeInertCell(table, COLD_MILLIKELVIN,
                                                  [&](uint32_t i) { return uint64_t{(index * i) % 7}; });
            StoreWidePageCell(nest, nest.blocks[child].page, index, cell);
        }

        const ConservedTotals initial = ComputeConservedTotals(nest, table, 1);
        RequestLevelOne(nest, MR_REQUEST_COARSEN);
        const uint32_t root = LookupBlock(nest, 0, 0, 0, 0);
        const uint32_t fractionSlot = nest.blocks[root].fraction;
        Expect(FractionHasOverflow(nest, fractionSlot), "ほぼ同じ: 粗くした親の端数は溢れを持つ");
        bool conserved = ComputeConservedTotals(nest, table, 1) == initial;

        const MrFoldTolerance tolerance = {.temperatureMk = 5000, .amountShift = 8};
        conserved = conserved && RunFoldTicks(nest, table, 0, QUIET_RUN_TICKS, tolerance, initial);
        Log(Channel::Sim, Level::Info, "溢れのあるほぼ同じ頁: 畳んだ {}・一様の値の成分 {}",
            nest.counters[MR_COUNTER_FOLDED], LoadWideNestCell(nest, root, 0).speciesCount);
        Expect(nest.blocks[root].fraction == MR_NO_FRACTION && !FractionHasOverflow(nest, fractionSlot),
               "ほぼ同じ: 溢れのある端数の枠を帳簿へ返す(溢れは空)");
        Expect(nest.counters[MR_COUNTER_FOLDED] == 1 && MrIsUniform(nest.blocks[root]), "ほぼ同じ: 12 種の頁を畳む");
        Expect(LoadWideNestCell(nest, root, 0).speciesCount == INERT_SPECIES, "ほぼ同じ: 平均の値は 12 種");
        Expect(conserved, "ほぼ同じ: 元素とエネルギーが毎刻み同じ(余りと端数は帳簿へ)");
    }

    // --- 静かな葉の許容差 ---

    // レベル 1 の葉(セルは cellOf)を静かになるまで刻み、静かな葉を粗くする要求の数を返す
    template <typename CellOf>
    uint32_t CountQuietRequests(const BakedReactionTable& table, CellOf cellOf) {
        MultiresNest nest = MakeWideRootNest(table, [&](uint32_t) { return MakeEvenCell(table, COLD_MILLIKELVIN); });
        RequestLevelOne(nest, MR_REQUEST_REFINE);
        const uint32_t child = LookupBlock(nest, 1, 0, 0, 0);
        for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index)
            StoreWidePageCell(nest, nest.blocks[child].page, index, cellOf(index));

        const MrFoldTolerance tolerance = {.temperatureMk = 5000, .amountShift = 6};
        for (uint64_t tick = 0; tick < QUIET_RUN_TICKS; ++tick) {
            StepActive(nest, table, WIDE_SEED, tick);
            SubmitQuietCoarsenRequests(nest, table, tick, tolerance);
            ProcessRequests(nest);
        }

        return nest.counters[MR_COUNTER_QUIET_REQUESTS];
    }

    // 8 種までは同じで、9 種目からの量だけが組の中で大きく違う葉は粗くしない(同じなら粗くする)
    void TestQuietWide(const BakedReactionTable& table) {
        const uint32_t similar = CountQuietRequests(table, [&](uint32_t index) {
            return MakeInertCell(table, COLD_MILLIKELVIN, [&](uint32_t i) { return uint64_t{(index + i) % 3}; });
        });
        const uint32_t different = CountQuietRequests(table, [&](uint32_t index) {
            return MakeInertCell(table, COLD_MILLIKELVIN, [&](uint32_t i) {
                return i > RX_MAX_CELL_SPECIES && index % 2 == 0 ? uint64_t{2000000} : uint64_t{0};
            });
        });

        Log(Channel::Sim, Level::Info, "溢れのある静かな葉: ほぼ同じ葉の要求 {}・9 種目から違う葉の要求 {}", similar,
            different);
        Expect(similar == 1, "静かな葉: ほぼ同じ 12 種の葉を粗くする");
        Expect(different == 0, "静かな葉: 9 種目からの量の差も許容差で比べる");
    }

    // --- 一様なブロックの 9 種目(T-0276)---

    // 一様な 8 種の根で 9 種目を作る規則だけが進む場面(表と根は multires_wide_scene.h): 溢れを使う世界は一様な根も頁に広げて
    // 9 種目を作り、待たせない(伝導あり・なし)。溢れを使わない世界は今までどおり待たせる(一様のまま。D-448 の当座のふるまい)。
    // 保存は毎刻みビット単位
    void TestUniformMakesNinth() {
        const auto table = BakeReactionTable(test::MakeWideOverflowTable());
        Expect(table.has_value(), "大きい溢れの表をベイクできる");
        if (!table)
            return;

        for (const bool conduction : {false, true}) {
            for (const bool wide : {true, false}) {
                MultiresNest nest = test::MakeWideOverflowNest(*table);
                if (wide)
                    EnableWideCells(nest);

                const ConservedTotals initial = ComputeConservedTotals(nest, *table, 1);
                bool conserved = true;
                for (uint64_t tick = 0; tick < test::WIDE_OVERFLOW_TICKS; ++tick) {
                    StepNest(nest, *table, test::WIDE_OVERFLOW_SEED, tick, test::LimitsStepOptions(conduction));
                    conserved = conserved && ComputeConservedTotals(nest, *table, 1) == initial;
                }

                const uint32_t slot = LookupBlock(nest, 0, test::WIDE_OVERFLOW_UNIFORM_X, 0, 0);
                const uint32_t most = test::WideOverflowUniformRootSpecies(nest);
                Log(Channel::Sim, Level::Info,
                    "一様な 8 種の根(溢れ {}・伝導 {}): 成分の最大 {}・一様のまま {}・待たせた {}",
                    wide ? "あり" : "なし", conduction ? "あり" : "なし", most, MrIsUniform(nest.blocks[slot]),
                    nest.counters[MR_COUNTER_LIMIT_PRODUCTS]);
                if (wide) {
                    Expect(most > RX_MAX_CELL_SPECIES && nest.counters[MR_COUNTER_LIMIT_PRODUCTS] == 0,
                           "一様な 8 種の根: 溢れを使う世界は 9 種目を作る(待たせない)");
                } else if (!conduction) {
                    Expect(MrIsUniform(nest.blocks[slot]) && most == test::WIDE_OVERFLOW_START_SPECIES,
                           "一様な 8 種の根: 溢れを使わない世界は今までどおり待たせる");
                }

                Expect(conserved, "一様な 8 種の根: 元素とエネルギーが毎刻み同じ");
            }
        }
    }

    // --- 熱の和(T-0272)---

    // 12 種のセルの熱を、成分を 1 つずつ足す形(GPU の WideTailThermal と同じ順: インライン 8 → 溢れ)と上限の無い形で比べる
    void TestThermalSums(const BakedReactionTable& table) {
        for (const int32_t temperatureMk : {0, COLD_MILLIKELVIN, HOT_MILLIKELVIN}) {
            const RxWideCell cell = MakeInertCell(table, temperatureMk, [](uint32_t i) { return uint64_t{i} * 7919; });
            MrThermalSums sums = MrMakeThermalSums();
            for (uint32_t i = 0; i < cell.speciesCount; ++i)
                sums = MrAddThermalSpecies(table.View(), sums, cell.species[i], cell.amounts[i]);

            const MrThermal expected = MrCellThermal(table.View(), cell);
            const MrThermal summed = MrThermalFromSums(cell.energy, sums);
            Expect(summed.temperature == expected.temperature && summed.conductance == expected.conductance &&
                       summed.capacityLimit == expected.capacityLimit,
                   std::format("熱の和: {} mK の 12 種のセルが上限の無い形と同じ", temperatureMk));
        }
    }

    int Run() {
        const auto table = BakeReactionTable(MakeCombustionTestTable());
        if (!table) {
            Log(Channel::Sim, Level::Error, "試験の表をベイクできない: {}", table.error());
            return 1;
        }

        TestShadowWideMatchesInline(*table);
        TestShadowWideOverflow();

        const BakedReactionTable inert = MakeInertTable();
        TestFoldWideExact(inert);
        TestExpandWide(inert);
        TestFoldWideNear(inert);
        TestQuietWide(inert);
        TestUniformMakesNinth();
        TestThermalSums(inert);

        if (failureCount != 0) {
            Log(Channel::Sim, Level::Error, "multires_wide_test: FAILED ({} 件)", failureCount);
            return 1;
        }

        Log(Channel::Sim, Level::Info, "multires_wide_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
