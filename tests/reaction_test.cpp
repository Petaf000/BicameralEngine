// reaction_test.cpp — 反応表のベイクと 1 セルの評価(shaders/common/reaction.hlsli)の CPU のテスト(T-0014、02 のテスト)。
//   - ベイクの検査が壊れた規則を必ず落とす / 反応熱と速度の表が定義どおり(double の参照値と比べる。double はテストだけ)
//   - 閉じた 1 セルで燃やして 36,000 刻み(10 分): 元素ごとの数とエネルギーが完全に一致し、熱が負にならない
//   - 取り合い: O2 が足りない / 吸熱の規則が熱を使い切る場面でも、ある量を超えて使わない
//   - 規則の並びを入れ替えても結果が同じ / 1 刻みの進行度が 1 未満の遅い反応が、待ちの丸めで期待どおりに進む
//   - 上限に当たる場面(tests/reaction_limits_table.h。T-0022): 9 種目の生成物を作る規則は待ち、進む規則が 17 個以上でも
//     捨てずに刻みごとに選ぶ。どちらも毎刻み元素とエネルギーがビット単位で保存される
//   セルは 1 セルだけのブロックとして待ちの丸め(ADR-0018)で進める(初めの状態 = 刻み 0 に変わった。刻みは 1 から)。
//   待ちの丸めそのものの試験は reaction_wait_test。今までの丸めと D-424 の下限の試験は T-0130 で消した
#include <cmath>
#include <cstdint>
#include <functional>
#include <ranges>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
#include "reaction_limits_table.h"
#include "reaction_test_cells.h"
#include "sim/reaction_table.h"
#include "sim/reaction_test_table.h"

using namespace bicameral;
using namespace bicameral::reaction;
using namespace bicameral::sim;

namespace {

    int failureCount = 0;

    void Expect(bool condition, std::string_view text) {
        if (condition)
            return;

        Log(Channel::Reaction, Level::Error, "FAILED: {}", text);
        ++failureCount;
    }

    BakedReactionTable BakeTestTable() {
        auto baked = BakeReactionTable(MakeCombustionTestTable());
        if (!baked) {
            Log(Channel::Reaction, Level::Error, "試験の表をベイクできない: {}", baked.error());
            std::exit(1);
        }

        return std::move(*baked);
    }

    double RateValue(uint64_t packed) {
        return std::ldexp(static_cast<double>(RxRateMantissa(packed)), RxRateExponent(packed));
    }

    uint32_t RuleId(const BakedReactionTable& table, std::string_view name) {
        return static_cast<uint32_t>(std::ranges::find(table.ruleNames, name) - table.ruleNames.begin());
    }

    std::string DescribeCell(const BakedReactionTable& table, const RxCell& cell) {
        const RxThermal thermal = RxComputeThermal(table.View(), cell);
        std::string text = std::format("{:.1f} K:", thermal.temperature / 1000.0);
        for (uint32_t i = 0; i < cell.speciesCount; ++i)
            text += std::format(" {}={}", table.speciesNames[cell.species[i]], cell.amounts[i]);

        return text;
    }

    // --- ベイク ---

    void ExpectBakeFails(std::string_view label, const std::function<void(ReactionTableDefinition&)>& breakTable) {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        breakTable(definition);
        const auto baked = BakeReactionTable(definition);
        Expect(!baked, std::format("ベイクが {} を落とさない", label));
        if (!baked)
            Log(Channel::Reaction, Level::Info, "  {} → {}", label, baked.error());
    }

    void TestBakeRejectsBrokenRules() {
        ExpectBakeFails("元素の釣り合い", [](auto& d) { d.rules[0].products[0].coefficient = 5; });
        ExpectBakeFails("知らない物質", [](auto& d) { d.rules[1].reactants[1].species = "ozone"; });
        ExpectBakeFails("知らない元素", [](auto& d) { d.species[0].composition[0].element = "X"; });
        ExpectBakeFails("係数 0", [](auto& d) { d.rules[2].reactants[0].coefficient = 0; });
        ExpectBakeFails("次数 1 の反応物なし", [](auto& d) { d.rules[0].reactants[0].firstOrder = false; });
        ExpectBakeFails("規則の名前の重なり", [](auto& d) { d.rules[1].name = d.rules[0].name; });
        ExpectBakeFails("物質の名前の重なり", [](auto& d) { d.species[1].name = d.species[0].name; });
        ExpectBakeFails("反応物の重なり", [](auto& d) { d.rules[2].reactants[1].species = "carbon"; });
    }

    void TestBakedValues(const BakedReactionTable& table) {
        // 反応熱 = 生成物の H0 − 反応物の H0。燃焼は発熱、Boudouard は吸熱
        const RxRule burn = table.rules[RuleId(table, "carbon_combustion")];
        const int64_t expected = table.species[table.SpeciesId("carbon_dioxide")].h0 -
                                 table.species[table.SpeciesId("carbon")].h0 -
                                 table.species[table.SpeciesId("oxygen")].h0;
        Expect(burn.reactionEnthalpy == expected && burn.reactionEnthalpy < 0, "炭の燃焼の反応熱");
        Expect(table.rules[RuleId(table, "boudouard")].reactionEnthalpy > 0, "Boudouard は吸熱");

        // 速度の表: double で計算した k(1 刻み、µmol とセルの体積に換算)と比べる。温度とともに増える
        const uint32_t rule = RuleId(table, "carbon_combustion");
        double maxRelativeError = 0.0;
        for (uint32_t kelvin = 300; kelvin < RX_RATE_TABLE_KELVINS; kelvin += 50) {
            const double rate = 3e8 * std::exp(-160000.0 / (8.314462618 * kelvin)) / 60.0 * (1e-6 / 0.125);
            const double baked = RateValue(table.View().Rate(rule, kelvin));
            maxRelativeError = std::max(maxRelativeError, std::abs(baked - rate) / rate);
            Expect(RateValue(table.View().Rate(rule, kelvin + 1)) >= baked, "速度が温度とともに増えない");
        }

        Log(Channel::Reaction, Level::Info, "速度の表と double の相対誤差の最大: {:.3e}", maxRelativeError);
        Expect(maxRelativeError < 1e-8, "速度の表が double の値からずれている");
    }

    // 1 セルだけのブロックを刻み 1 から tickCount 刻み進める(初めの状態 = 刻み 0 に変わった)
    RxCell Advance(const BakedReactionTable& table, const RxCell& cell, uint32_t tickCount, uint64_t cellId) {
        const RxLoneCell lone = AdvanceLoneReactionCell(table, RxMakeLoneCell(cell, 0), test::REACTION_TEST_SEED, 1,
                                                        tickCount, cellId);

        return lone.cell;
    }

    // --- 保存則 ---

    struct Conservation {
        std::vector<uint64_t> elements;
        int64_t energy = 0;
    };

    // count 刻み進めて、毎刻み元素とエネルギーが初めと同じ・熱が負でないことを確かめる。途中の様子を checkpoints の刻みで表示
    RxCell RunConserving(const BakedReactionTable& table, RxCell cell, uint64_t count, std::string_view label,
                         std::span<const uint64_t> checkpoints) {
        const Conservation initial{.elements = CountElements(table, cell), .energy = cell.energy};
        bool conserved = true;
        RxLoneCell lone = RxMakeLoneCell(cell, 0);
        for (uint64_t tick = 0; tick < count; ++tick) {
            if (std::ranges::find(checkpoints, tick) != checkpoints.end())
                Log(Channel::Reaction, Level::Info, "  {} 刻み {:>6}: {}", label, tick, DescribeCell(table, cell));

            lone = AdvanceLoneReactionCell(table, lone, test::REACTION_TEST_SEED, tick + 1, 1, 0);
            cell = lone.cell;
            const RxThermal thermal = RxComputeThermal(table.View(), cell);
            conserved = conserved && CountElements(table, cell) == initial.elements && cell.energy == initial.energy &&
                        thermal.heat >= 0;
        }

        Log(Channel::Reaction, Level::Info, "  {} 刻み {:>6}: {}", label, count, DescribeCell(table, cell));
        Expect(conserved, std::format("{}: 元素・エネルギーが保存されない / 熱が負", label));

        return cell;
    }

    std::vector<SpeciesAmount> CrateAir(const BakedReactionTable& table, uint64_t cellulose, uint64_t carbon) {
        // 木箱のセル: 体積の 1 割がセルロース(約 38.6 mol)、残りが空気(O2 約 0.98 mol・N2 約 3.7 mol)
        return {{.species = table.SpeciesId("cellulose"), .amount = cellulose},
                {.species = table.SpeciesId("carbon"), .amount = carbon},
                {.species = table.SpeciesId("oxygen"), .amount = 983000},
                {.species = table.SpeciesId("nitrogen"), .amount = 3697000}};
    }

    void TestClosedCellBurn(const BakedReactionTable& table) {
        constexpr std::array<uint64_t, 8> CHECKPOINTS = {0, 1, 10, 30, 60, 120, 600, 3600};
        const auto amounts = CrateAir(table, 38600000, 0);
        const RxCell cell = MakeReactionCell(table, amounts, 600000);
        const RxCell burnt = RunConserving(table, cell, 36000, "閉じた木箱 600 K", CHECKPOINTS);
        const uint32_t oxygen = RxFindSlot(burnt, table.SpeciesId("oxygen"));
        const bool oxidized = RxFindSlot(burnt, table.SpeciesId("carbon_dioxide")) != RX_NO_SLOT ||
                              RxFindSlot(burnt, table.SpeciesId("carbon_monoxide")) != RX_NO_SLOT;
        Expect(oxygen == RX_NO_SLOT || burnt.amounts[oxygen] < 983000, "燃えて O2 が減らない");
        Expect(oxidized, "燃えて CO2 も CO もできない");
    }

    // --- 取り合い ---

    // 熱い炭と木に少しの O2: 3 つの規則が O2 を取り合う。使った量はある量以下で、ほぼ使い切る
    void TestOxygenContention(const BakedReactionTable& table) {
        const auto amounts = CrateAir(table, 38600000, 50000000);
        const RxCell cell = MakeReactionCell(table, amounts, 1500000);
        const RxCell next = Advance(table, cell, 1, 0);
        const uint32_t oxygen = RxFindSlot(next, table.SpeciesId("oxygen"));
        const uint64_t left = oxygen == RX_NO_SLOT ? 0 : next.amounts[oxygen];
        Log(Channel::Reaction, Level::Info, "  O2 の取り合い: 983000 → {} µmol / {}", left, DescribeCell(table, next));
        Expect(left < 983000 / 100, "O2 の取り合いで使い切らない");
        Expect(CountElements(table, next) == CountElements(table, cell) && next.energy == cell.energy,
               "O2 の取り合いで保存されない");
    }

    // 吸熱の Boudouard が熱を使い切ろうとする場面: 熱を資源として縮めるので、熱は負にならない
    void TestEndothermicLimit(const BakedReactionTable& table) {
        const std::vector<SpeciesAmount> amounts = {
            {.species = table.SpeciesId("carbon"), .amount = 1000000000},
            {.species = table.SpeciesId("carbon_dioxide"), .amount = 1000000000},
        };
        const RxCell cell = MakeReactionCell(table, amounts, 2000000);
        constexpr std::array<uint64_t, 6> CHECKPOINTS = {0, 1, 2, 5, 10, 60};
        const RxCell cooled = RunConserving(table, cell, 600, "吸熱 2000 K", CHECKPOINTS);
        const RxThermal before = RxComputeThermal(table.View(), cell);
        const RxThermal after = RxComputeThermal(table.View(), cooled);
        const RxThermal firstTick = RxComputeThermal(table.View(), Advance(table, cell, 1, 0));
        Expect(after.temperature < before.temperature, "吸熱で温度が下がらない");
        Expect(firstTick.heat >= before.heat - (before.heat >> RX_ENDOTHERMIC_HEAT_SHIFT),
               "吸熱で 1 刻みに熱の 1/8 より多く使う");
        Expect(after.temperature > 300000, "吸熱で温度が落ちすぎる(陽的な評価の行き過ぎ)");
    }

    // --- 並び順 ---

    uint64_t RunVariedCells(const BakedReactionTable& table, uint32_t cellCount, uint32_t tickCount) {
        uint64_t digest = 0;
        for (uint32_t index = 0; index < cellCount; ++index) {
            const RxCell cell = Advance(table, test::MakeVariedReactionCell(table, index), tickCount, index);
            digest = fx::FxHashCombine(digest, HashReactionCell(cell));
        }

        return digest;
    }

    void TestRuleOrderIndependence(const BakedReactionTable& table) {
        ReactionTableDefinition reversed = MakeCombustionTestTable();
        std::ranges::reverse(reversed.rules);
        const auto reversedTable = BakeReactionTable(reversed);
        Expect(reversedTable.has_value(), "並びを変えた表をベイクできない");
        if (!reversedTable)
            return;

        const uint64_t original = RunVariedCells(table, 256, 200);
        const uint64_t swapped = RunVariedCells(*reversedTable, 256, 200);
        Log(Channel::Reaction, Level::Info, "  規則の並び: 元 {:016x} / 逆 {:016x}", original, swapped);
        Expect(original == swapped, "規則の並びで結果が変わる");
    }

    // --- 遅い反応 ---

    // 490 K の熱分解は 1 刻みの進行度が 1 µmol 未満。待ちの丸めで、36,000 刻みの合計が期待値の ±5σ に入る
    void TestSlowReaction(const BakedReactionTable& table) {
        const uint32_t cellulose = table.SpeciesId("cellulose");
        const std::vector<SpeciesAmount> amounts = {{.species = cellulose, .amount = 1000000},
                                                    {.species = table.SpeciesId("nitrogen"), .amount = 4000000}};
        const double perTick = RateValue(table.View().Rate(RuleId(table, "cellulose_pyrolysis"), 490)) * 1e6;
        constexpr uint32_t TICKS = 36000;
        const RxCell cell = Advance(table, MakeReactionCell(table, amounts, 490000), TICKS, 7);

        const auto reacted = static_cast<double>(1000000 - cell.amounts[RxFindSlot(cell, cellulose)]);
        const double expected = perTick * TICKS;
        Log(Channel::Reaction, Level::Info, "  遅い反応: 1 刻み {:.4f} µmol、{} 刻みで期待 {:.1f} / 実際 {:.0f}",
            perTick, TICKS, expected, reacted);
        Expect(perTick < 1.0 && perTick > 0.001, "遅い反応の試験の温度が合っていない");
        Expect(std::abs(reacted - expected) < 5.0 * std::sqrt(expected), "遅い反応が期待どおりに進まない");
    }

    // --- 上限に当たる場面(T-0022 の最初の段)---

    BakedReactionTable BakeLimitsTable() {
        auto baked = BakeReactionTable(test::MakeLimitsTestTable());
        if (!baked) {
            Log(Channel::Reaction, Level::Error, "上限の試験の表をベイクできない: {}", baked.error());
            std::exit(1);
        }

        return std::move(*baked);
    }

    // 込み合うセル(偶数)と入りきらないセル(奇数)を 1 刻みずつ進め、毎刻みの保存と上限の印を確かめる
    void TestLimits() {
        constexpr uint32_t CELLS = 64;
        constexpr uint32_t TICKS = 400;
        const BakedReactionTable table = BakeLimitsTable();
        const uint32_t ninth = table.SpeciesId(test::LimitsHeldName(9));
        const uint32_t freed = table.SpeciesId(test::LimitsHeldName(8));
        bool conserved = true;
        uint32_t crowdLimited = 0;
        uint32_t heldCells = 0;
        uint32_t ninthMade = 0;
        uint64_t heldTicks = 0;
        for (uint32_t index = 0; index < CELLS; ++index) {
            const RxCell initial = test::MakeLimitsCell(table, index);
            const std::vector<uint64_t> elements = CountElements(table, initial);
            RxLoneCell lone = RxMakeLoneCell(initial, 0);
            for (uint32_t tick = 1; tick <= TICKS; ++tick) {
                lone = AdvanceLoneReactionCell(table, lone, test::REACTION_TEST_SEED, tick, 1, index);
                conserved = conserved && CountElements(table, lone.cell) == elements &&
                            lone.cell.energy == initial.energy && RxComputeThermal(table.View(), lone.cell).heat >= 0;
            }

            if (index % 2 == 0) {
                crowdLimited += lone.candidatesLimitTicks == TICKS && lone.cell.speciesCount == 8 ? 1u : 0u;
                continue;
            }

            heldCells += lone.productsHeldTicks > 0 && lone.productsHeldTicks < TICKS ? 1u : 0u;
            ninthMade += RxFindSlot(lone.cell, ninth) != RX_NO_SLOT && RxFindSlot(lone.cell, freed) == RX_NO_SLOT ? 1u
                                                                                                                  : 0u;
            heldTicks += lone.productsHeldTicks;
        }

        Log(Channel::Reaction, Level::Info,
            "  上限: 込み合う {}/{} が毎刻み選んで進み、入りきらない {}/{} が待った後に 9 種目を作った(待った刻み 平均 "
            "{})",
            crowdLimited, CELLS / 2, ninthMade, CELLS / 2, heldTicks / (CELLS / 2));
        Expect(conserved, "上限に当たる場面で元素かエネルギーが保存されない");
        Expect(crowdLimited == CELLS / 2, "進む規則が 17 個以上のセルで、毎刻み上限の印が立たない(か成分が消えた)");
        Expect(heldCells == CELLS / 2, "9 種目の生成物を作る規則が、待ってから進まない");
        Expect(ninthMade == CELLS / 2, "枠が空いた後に 9 種目の生成物ができない");
    }

    int Run() {
        TestLimits();
        TestBakeRejectsBrokenRules();
        const BakedReactionTable table = BakeTestTable();
        TestBakedValues(table);
        TestClosedCellBurn(table);
        TestOxygenContention(table);
        TestEndothermicLimit(table);
        TestRuleOrderIndependence(table);
        TestSlowReaction(table);
        Log(Channel::Reaction, Level::Info, "いろいろなセル 4096 × 400 刻みの要約: {:016x}",
            RunVariedCells(table, 4096, 400));

        if (failureCount != 0) {
            Log(Channel::Reaction, Level::Error, "reaction_test: {} failure(s)", failureCount);
            return 1;
        }

        Log(Channel::Reaction, Level::Info, "reaction_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
