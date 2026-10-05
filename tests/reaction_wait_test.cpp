// reaction_wait_test.cpp — 遅い反応の待ちの丸め(reaction.hlsli の RxWaitTicks・RxStepCellWait。T-0105・D-429。研究)の CPU のテスト。
//   - 待ちの分布: いろいろな f で、待ちの平均 = 1 ÷ f・P(待ち = 1) = f(±5σ)。f について単調・級数と log2 の境目で続いている
//   - 引き直し: ブロックが別の理由で変わるたびに引き直しても、起きた回数が Σ f の ±5σ(幾何分布は記憶が無い)。
//     速さが変わった刻みに引き直さない(古い乱数・新しい f)場合の偏りも測って出す(引き直す理由の記録)
//   - 眠らせる: ブロック(8 セル)を待ちの最小の刻みまで評価しない場合と、全部を毎刻み評価する場合が、毎刻みビット一致(2^32 刻みをまたぐ)
//   - 何年分: 遅い熱分解のセルを起こす刻みだけで 10 年(約 1.9e10 刻み)進め、起きた回数が Σ f × 刻み の ±5σ
//   - 今の場面を待ちの丸めで: 閉じた木箱の燃焼 36,000 刻みの保存則・490 K の遅い熱分解・規則の並び・室温の木箱がいつ進むか
// 期待値の Σ f × 刻み は、状態ごとの 1 刻みの進み f(double。テストだけ)と、その状態でいた刻みの数の積の合計
// (起きた回数からこれを引いたものは平均 0、分散 Σ f(1 − f) × 刻み。温度や量が変わって f が動いても正しい基準になる)
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/singleton.h"
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

    BakedReactionTable BakeTestTable(bool reversed = false) {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        if (reversed)
            std::ranges::reverse(definition.rules);

        auto baked = BakeReactionTable(definition);
        if (!baked) {
            Log(Channel::Reaction, Level::Error, "試験の表をベイクできない: {}", baked.error());
            std::exit(1);
        }

        return std::move(*baked);
    }

    constexpr uint64_t TICKS_PER_YEAR = 60ull * 60 * 60 * 24 * 365;
    constexpr uint64_t WAIT_TEST_KEY = 0x7E57'0105u;

    double FractionValue(uint64_t fraction) {
        return std::ldexp(static_cast<double>(fraction), -64);
    }

    double RateValue(const RxRate& rate) {
        return static_cast<double>(rate.integerPart) + FractionValue(rate.fraction);
    }

    uint32_t RuleId(const BakedReactionTable& table, std::string_view name) {
        return static_cast<uint32_t>(std::ranges::find(table.ruleNames, name) - table.ruleNames.begin());
    }

    uint32_t TableKelvin(const BakedReactionTable& table, const RxCell& cell) {
        const RxThermal thermal = RxComputeThermal(table.View(), cell);
        const uint32_t kelvin = static_cast<uint32_t>(thermal.temperature) /
                                static_cast<uint32_t>(MILLIKELVIN_PER_KELVIN);

        return std::min(kelvin, RX_RATE_TABLE_KELVINS - 1);
    }

    // 規則 name のセル cell での 1 刻みの進みの期待値
    RxRate RuleRate(const BakedReactionTable& table, const RxCell& cell, std::string_view name) {
        const uint32_t rule = RuleId(table, name);

        return RxComputeRuleRate(table.View(), cell, rule, table.rules[rule], TableKelvin(table, cell)).rate;
    }

    // 期待値と分散を足していき、起きた回数と比べる
    struct Tally {
        double expected = 0.0;
        double variance = 0.0;
        double actual = 0.0;

        void Expose(double perTick, double ticks) {
            expected += perTick * ticks;
            variance += perTick * (1.0 - perTick) * ticks;
        }

        [[nodiscard]] double Sigma() const { return (actual - expected) / std::sqrt(std::max(variance, 1e-300)); }
    };

    // --- 待ちの分布 ---

    struct WaitStatistics {
        double mean = 0.0;
        uint32_t ones = 0;
        uint32_t never = 0;
    };

    WaitStatistics DrawWaits(uint64_t fraction, uint32_t draws) {
        WaitStatistics statistics;
        double sum = 0.0;
        for (uint32_t i = 0; i < draws; ++i) {
            const uint64_t ticks = RxWaitTicks(fraction, fx::FxHash64(test::REACTION_TEST_SEED, i, fraction, 3));
            if (ticks == RX_WAIT_NEVER) {
                ++statistics.never;
                continue;
            }

            sum += static_cast<double>(ticks);
            statistics.ones += ticks == 1 ? 1 : 0;
        }

        statistics.mean = sum / static_cast<double>(draws - statistics.never);

        return statistics;
    }

    void TestWaitDistribution() {
        constexpr uint32_t DRAWS = 65536;
        constexpr std::array<uint64_t, 9> FRACTIONS = {
            0xE666'6666'6666'6666u,    // 0.9
            0x8000'0000'0000'0000u,    // 1/2
            RX_WAIT_SERIES_LIMIT,      // 1/16(log2 の側)
            RX_WAIT_SERIES_LIMIT - 1,  // 1/16 の直前(級数の側)
            1ull << 57,                // 1/128
            1ull << 44,                // 2^-20
            1ull << 24,                // 2^-40
            1ull << 6,                 // 2^-58(待ちの平均は約 2.9e17 刻み = 1.5 億年)
            1ull,                      // 2^-64(ほとんど「起きない」)
        };

        Log(Channel::Reaction, Level::Info, "待ちの分布({} 回ずつ引く)", DRAWS);
        for (const uint64_t fraction : FRACTIONS) {
            const double f = FractionValue(fraction);
            const WaitStatistics statistics = DrawWaits(fraction, DRAWS);
            const auto counted = static_cast<double>(DRAWS - statistics.never);
            const double meanSigma = std::sqrt(1.0 - f) / f / std::sqrt(counted);
            const double meanZ = (statistics.mean - 1.0 / f) / meanSigma;
            const double onesZ = (static_cast<double>(statistics.ones) - f * DRAWS) / std::sqrt(f * (1.0 - f) * DRAWS);
            // 待ちが約 2^61 刻みを超える引きは「起きない」(reaction.hlsli)。多めに見積もるため 2^60 で数える
            const double neverExpected = std::exp(-std::ldexp(f, 60)) * DRAWS;
            Log(Channel::Reaction, Level::Info,
                "  f = {:.3e}: 平均 {:.6e}(期待 {:.6e}、{:+.2f}σ)・待ち 1 が {}({:+.2f}σ)・起きない {}(期待 {:.1f})", f,
                statistics.mean, 1.0 / f, meanZ, statistics.ones, onesZ, statistics.never, neverExpected);

            if (fraction == 1)
                continue;

            Expect(std::abs(meanZ) < 5.0, std::format("f = {:.3e} の待ちの平均が 1 ÷ f の ±5σ に入らない", f));
            Expect(std::abs(onesZ) < 5.0, std::format("f = {:.3e} の P(待ち = 1) が f の ±5σ に入らない", f));
            Expect(statistics.never <= neverExpected * 10.0 + 3.0,
                   std::format("f = {:.3e} で「起きない」が多すぎる", f));
        }

        // f = 2^-64 は待ちの平均が 2^64 刻みで、約 2^61 刻みで切るので大半(約 exp(−1/8) ≈ 88%)が「起きない」
        Expect(DrawWaits(1, DRAWS).never > DRAWS / 2, "f = 2^-64 が大半「起きない」にならない");
    }

    // 同じ乱数なら、f が大きいほど待ちは短いか同じ(級数の側と log2 の側それぞれ)。境目では 1 刻みまでしか違わない
    void TestWaitMonotonic() {
        constexpr std::array<uint64_t, 7> SERIES = {1ull,       1ull << 20, (1ull << 33) + 12345,    1ull << 40,
                                                    1ull << 52, 1ull << 59, RX_WAIT_SERIES_LIMIT - 1};
        constexpr std::array<uint64_t, 4> LOG2 = {RX_WAIT_SERIES_LIMIT, 1ull << 61, 1ull << 63, 0xFFFF'FFFF'FFFF'0000u};
        bool monotonic = true;
        uint64_t worstBoundary = 0;
        for (uint32_t i = 0; i < 4096; ++i) {
            const uint64_t hash = fx::FxHash64(test::REACTION_TEST_SEED, i, 0, 4);
            const auto check = [&](std::span<const uint64_t> fractions) {
                for (size_t k = 1; k < fractions.size(); ++k)
                    monotonic = monotonic && RxWaitTicks(fractions[k], hash) <= RxWaitTicks(fractions[k - 1], hash);
            };
            check(SERIES);
            check(LOG2);

            const uint64_t below = RxWaitTicks(RX_WAIT_SERIES_LIMIT - 1, hash);
            const uint64_t above = RxWaitTicks(RX_WAIT_SERIES_LIMIT, hash);
            worstBoundary = std::max(worstBoundary, below > above ? below - above : above - below);
        }

        Log(Channel::Reaction, Level::Info, "  単調: {}・境目(f = 1/16)の差の最大: {} 刻み", monotonic, worstBoundary);
        Expect(monotonic, "f を大きくしても待ちが短くならない");
        Expect(worstBoundary <= 1, "級数と log2 の境目で待ちが飛ぶ");
    }

    // --- 引き直し(表なし。1 セル 1 規則で、f を場面が決める)---

    enum class Redraw : uint8_t { OnChange, Stale };

    struct RenewalScene {
        std::string_view label;
        uint64_t fractionHigh;
        uint64_t fractionLow;
        uint64_t period;       // f は period 刻みごとに高い・低いを入れ替える(0 なら高いまま)
        uint32_t pokePer1024;  // 1 刻みにブロックが別の理由で変わる確率(1/1024 単位)
        Redraw redraw;
    };

    // 刻み 1〜ticks の各刻みに、セルが「待ちが来たら起きる」を cells 個で数える(起きたら、つつかれたら、f が変わったら引き直す)
    Tally RunRenewal(const RenewalScene& scene, uint32_t cells, uint64_t ticks) {
        Tally tally;
        for (uint32_t cell = 0; cell < cells; ++cell) {
            uint64_t changed = 0;
            uint64_t fraction = scene.fractionHigh;
            uint64_t wait = RxWaitTicks(fraction, fx::FxHashCombine(RxWaitSeed(1, changed, cell), WAIT_TEST_KEY));
            for (uint64_t tick = 1; tick <= ticks; ++tick) {
                // --- 前の刻みの間に起きた変化(f の切り替え・ほかのセルが変えた)---
                const bool low = scene.period != 0 && ((tick - 1) / scene.period) % 2 == 1;
                const uint64_t now = low ? scene.fractionLow : scene.fractionHigh;
                const bool poked = fx::FxRandomBelow(fx::FxHash64(2, tick, cell, 5), 1024) < scene.pokePer1024;
                const bool switched = now != fraction;
                fraction = now;
                if (poked || (switched && scene.redraw == Redraw::OnChange))
                    changed = tick - 1;

                if (poked || switched)
                    wait = RxWaitTicks(fraction, fx::FxHashCombine(RxWaitSeed(1, changed, cell), WAIT_TEST_KEY));

                // --- この刻み ---
                tally.Expose(FractionValue(fraction), 1.0);
                if (tick - changed < wait)
                    continue;

                tally.actual += 1.0;
                changed = tick;
                wait = RxWaitTicks(fraction, fx::FxHashCombine(RxWaitSeed(1, changed, cell), WAIT_TEST_KEY));
            }
        }

        return tally;
    }

    void TestRenewal() {
        constexpr uint32_t CELLS = 2048;
        constexpr uint64_t TICKS = 4096;
        constexpr uint64_t F64 = 1ull << 58;   // 1/64
        constexpr uint64_t F16 = 1ull << 60;   // 1/16
        constexpr uint64_t F512 = 1ull << 55;  // 1/512
        const std::array<RenewalScene, 6> scenes = {{
            {.label = "f 一定",
             .fractionHigh = F64,
             .fractionLow = F64,
             .period = 0,
             .pokePer1024 = 0,
             .redraw = Redraw::OnChange},
            {.label = "f 一定・1/8 の刻みでつつかれる",
             .fractionHigh = F64,
             .fractionLow = F64,
             .period = 0,
             .pokePer1024 = 128,
             .redraw = Redraw::OnChange},
            {.label = "f が 256 刻みごとに 1/16 ↔ 1/512(引き直す)",
             .fractionHigh = F16,
             .fractionLow = F512,
             .period = 256,
             .pokePer1024 = 0,
             .redraw = Redraw::OnChange},
            {.label = "f が 7 刻みごとに 1/16 ↔ 1/512(引き直す)",
             .fractionHigh = F16,
             .fractionLow = F512,
             .period = 7,
             .pokePer1024 = 0,
             .redraw = Redraw::OnChange},
            {.label = "f が 256 刻みごとに 1/16 ↔ 1/512(引き直さない = 古い乱数)",
             .fractionHigh = F16,
             .fractionLow = F512,
             .period = 256,
             .pokePer1024 = 0,
             .redraw = Redraw::Stale},
            {.label = "f が 7 刻みごとに 1/16 ↔ 1/512(引き直さない = 古い乱数)",
             .fractionHigh = F16,
             .fractionLow = F512,
             .period = 7,
             .pokePer1024 = 0,
             .redraw = Redraw::Stale},
        }};

        Log(Channel::Reaction, Level::Info, "引き直し({} セル × {} 刻み)", CELLS, TICKS);
        for (const RenewalScene& scene : scenes) {
            const Tally tally = RunRenewal(scene, CELLS, TICKS);
            Log(Channel::Reaction, Level::Info, "  {}: 期待 {:.1f} / 実際 {:.0f}({:+.2f}σ、{:+.2f}%)", scene.label,
                tally.expected, tally.actual, tally.Sigma(), 100.0 * (tally.actual / tally.expected - 1.0));
            if (scene.redraw == Redraw::OnChange)
                Expect(std::abs(tally.Sigma()) < 5.0, std::format("{}: 起きた回数が ±5σ に入らない", scene.label));
        }
    }

    // --- 眠らせる(表あり。ブロック = 8 セル。ブロックの中の 1 セルでも変われば、ブロックの changedTick がその刻みになる)---

    constexpr uint32_t BLOCK_CELLS = 8;
    constexpr uint32_t BLOCK_COUNT = 16;

    struct SleepWorld {
        std::vector<RxCell> cells;
        std::vector<uint64_t> changed;  // ブロックごと
        std::vector<uint64_t> wake;     // ブロックごと(眠らせる側だけ)
        uint64_t evaluatedBlocks = 0;
    };

    std::vector<SpeciesAmount> CelluloseInNitrogen(const BakedReactionTable& table) {
        return {{.species = table.SpeciesId("cellulose"), .amount = 1000000},
                {.species = table.SpeciesId("nitrogen"), .amount = 4000000}};
    }

    std::vector<SpeciesAmount> CrateAir(const BakedReactionTable& table, uint64_t cellulose) {
        return {{.species = table.SpeciesId("cellulose"), .amount = cellulose},
                {.species = table.SpeciesId("oxygen"), .amount = 983000},
                {.species = table.SpeciesId("nitrogen"), .amount = 3697000}};
    }

    // ブロック 0〜7: 遅い熱分解(440〜510 K)/ 8〜11: いろいろなセル(速い反応で毎刻み変わるもの)/ 12〜14: 室温の木箱 / 15: 600 K の木箱
    SleepWorld MakeSleepWorld(const BakedReactionTable& table, uint64_t firstTick) {
        SleepWorld world;
        for (uint32_t block = 0; block < BLOCK_COUNT; ++block) {
            for (uint32_t i = 0; i < BLOCK_CELLS; ++i) {
                const uint32_t index = (block * BLOCK_CELLS) + i;
                if (block < 8)
                    world.cells.push_back(MakeReactionCell(table, CelluloseInNitrogen(table),
                                                           static_cast<int32_t>((440 + (index * 7 % 70)) * 1000)));
                else if (block < 12)
                    world.cells.push_back(test::MakeVariedReactionCell(table, index));
                else
                    world.cells.push_back(
                        MakeReactionCell(table, CrateAir(table, 38600000), block < 15 ? 300000 : 600000));
            }
        }

        world.changed.assign(BLOCK_COUNT, firstTick - 1);
        world.wake.assign(BLOCK_COUNT, firstTick);

        return world;
    }

    // 刻み tick の初めに外から熱を入れる(1000 刻みごとに 1 つのブロックの 1 セル)。前の刻みに変わったとみなし、眠っていれば起こす
    void HeatFromOutside(SleepWorld& world, uint64_t tick, uint64_t firstTick) {
        const uint64_t step = tick - firstTick;
        if (step == 0 || step % 1000 != 0)
            return;

        const auto block = static_cast<uint32_t>((step / 1000) % BLOCK_COUNT);
        world.cells[size_t{block} * BLOCK_CELLS].energy += 1000000;
        world.changed[block] = tick - 1;
        world.wake[block] = tick;
    }

    // ブロックを刻む。変わったか(changedTick の更新)と、変わらなければ次に評価が要る刻みを返す
    uint64_t StepBlock(const BakedReactionTable& table, SleepWorld& world, uint32_t block, uint64_t tick) {
        bool changed = false;
        uint64_t wake = RX_WAIT_NEVER;
        for (uint32_t i = 0; i < BLOCK_CELLS; ++i) {
            const uint32_t index = (block * BLOCK_CELLS) + i;
            const RxWaitStep step = StepReactionCellWait(table, world.cells[index], test::REACTION_TEST_SEED, tick,
                                                         world.changed[block], index);
            changed = changed || HashReactionCell(step.cell) != HashReactionCell(world.cells[index]);
            wake = std::min(wake, step.wakeTick);
            world.cells[index] = step.cell;
        }

        world.evaluatedBlocks += 1;
        if (!changed)
            return wake;

        world.changed[block] = tick;

        return tick + 1;
    }

    void TestSleepMatchesFull(const BakedReactionTable& table) {
        constexpr uint64_t FIRST_TICK = (1ull << 32) - 3000;  // 2^32 刻みをまたぐ
        constexpr uint64_t TICKS = 6000;
        SleepWorld full = MakeSleepWorld(table, FIRST_TICK);
        SleepWorld sleeping = MakeSleepWorld(table, FIRST_TICK);
        bool same = true;
        bool wokeLate = false;
        uint64_t firstMismatch = 0;
        for (uint64_t tick = FIRST_TICK; tick < FIRST_TICK + TICKS; ++tick) {
            HeatFromOutside(full, tick, FIRST_TICK);
            HeatFromOutside(sleeping, tick, FIRST_TICK);

            for (uint32_t block = 0; block < BLOCK_COUNT; ++block)
                StepBlock(table, full, block, tick);

            for (uint32_t block = 0; block < BLOCK_COUNT; ++block) {
                if (sleeping.wake[block] > tick)
                    continue;

                sleeping.wake[block] = StepBlock(table, sleeping, block, tick);
                wokeLate = wokeLate || sleeping.wake[block] <= tick;
            }

            const bool equal = full.changed == sleeping.changed &&
                               std::ranges::equal(full.cells, sleeping.cells, {}, HashReactionCell, HashReactionCell);
            if (!equal && same)
                firstMismatch = tick;

            same = same && equal;
        }

        const uint64_t fullBlocks = full.evaluatedBlocks;
        Log(Channel::Reaction, Level::Info,
            "眠らせる: {} ブロック × {} 刻み(2^32 をまたぐ)で毎刻み一致 {}・評価したブロック 全部 {} / 眠らせる "
            "{}({:.1f}%)",
            BLOCK_COUNT, TICKS, same, fullBlocks, sleeping.evaluatedBlocks,
            100.0 * static_cast<double>(sleeping.evaluatedBlocks) / static_cast<double>(fullBlocks));
        Expect(same, std::format("眠らせると全部を評価した場合と違う(最初は刻み {})", firstMismatch));
        Expect(!wokeLate, "起こす刻みが今の刻み以前になった");
        Expect(sleeping.evaluatedBlocks < fullBlocks / 2, "眠らせても評価が減らない");
    }

    // --- 何年分(起こす刻みだけ評価する)---

    // 遅い熱分解の 1 刻みの進みが target 以上になる最初の温度(K)
    uint32_t KelvinForRate(const BakedReactionTable& table, double target) {
        for (uint32_t kelvin = 300; kelvin < 1000; ++kelvin) {
            const RxCell cell = MakeReactionCell(table, CelluloseInNitrogen(table),
                                                 static_cast<int32_t>(kelvin * 1000));
            if (RateValue(RuleRate(table, cell, "cellulose_pyrolysis")) >= target)
                return kelvin;
        }

        return 1000;
    }

    // 何年分の場面の 1 セル: 起こす刻みだけ評価して lastTick まで進める
    struct YearsRun {
        Tally tally;
        uint64_t evaluations = 0;
        bool singleUnit = true;
    };

    void RunCellYears(const BakedReactionTable& table, uint32_t kelvin, uint32_t index, uint64_t firstTick,
                      uint64_t lastTick, YearsRun& run) {
        RxCell cell = MakeReactionCell(table, CelluloseInNitrogen(table), static_cast<int32_t>(kelvin * 1000));
        uint64_t changed = firstTick - 1;
        uint64_t tick = firstTick;
        while (tick <= lastTick) {
            const RxRate rate = RuleRate(table, cell, "cellulose_pyrolysis");
            run.singleUnit = run.singleUnit && rate.integerPart == 0;
            const RxWaitStep step = StepReactionCellWait(table, cell, test::REACTION_TEST_SEED, tick, changed, index);
            ++run.evaluations;
            if (HashReactionCell(step.cell) == HashReactionCell(cell)) {
                // 次の評価は待ちの刻み。その手前の刻みは評価しない(全部を評価しても変わらない。TestSleepMatchesFull)
                tick = step.wakeTick;
                continue;
            }

            // 刻み changed + 1 〜 tick の間、この状態の f にさらされて、tick に 1 単位進んだ
            run.tally.Expose(FractionValue(rate.fraction), static_cast<double>(tick - changed));
            cell = step.cell;
            changed = tick;
            tick += 1;
        }

        const RxRate rate = RuleRate(table, cell, "cellulose_pyrolysis");
        run.tally.Expose(FractionValue(rate.fraction), static_cast<double>(lastTick - changed));
        const uint32_t cellulose = RxFindSlot(cell, table.SpeciesId("cellulose"));
        run.tally.actual += static_cast<double>(1000000 - cell.amounts[cellulose]);
    }

    void TestFastForwardYears(const BakedReactionTable& table) {
        constexpr uint32_t CELLS = 256;
        constexpr uint64_t YEARS = 10;
        constexpr uint64_t FIRST_TICK = 1;
        constexpr uint64_t LAST_TICK = FIRST_TICK + (YEARS * TICKS_PER_YEAR);
        const uint32_t kelvin = KelvinForRate(table, 4e-8);
        YearsRun run;
        for (uint32_t index = 0; index < CELLS; ++index)
            RunCellYears(table, kelvin, index, FIRST_TICK, LAST_TICK, run);

        const auto perCellTicks = static_cast<double>(LAST_TICK - FIRST_TICK + 1);
        const Tally& tally = run.tally;
        Log(Channel::Reaction, Level::Info,
            "何年分: {} K の熱分解(1 刻み約 {:.2e} µmol)を {} セル × {} 年({:.3e} 刻み)。期待 {:.1f} / 実際 "
            "{:.0f}({:+.2f}σ)・評価 {} 回(全部なら {:.3e} 回)",
            kelvin, tally.expected / perCellTicks / CELLS, CELLS, YEARS, perCellTicks, tally.expected, tally.actual,
            tally.Sigma(), run.evaluations, perCellTicks * CELLS);
        Expect(run.singleUnit, "何年分の場面の熱分解が 1 刻みに 1 単位以上進む(場面の温度が合っていない)");
        Expect(LAST_TICK > (1ull << 32), "何年分の場面が 2^32 刻みを超えない");
        Expect(tally.expected > 1000.0, "何年分の場面で反応がほとんど起きない");
        Expect(std::abs(tally.Sigma()) < 5.0, "何年分の遅い反応が Σ f × 刻み の ±5σ に入らない");
    }

    // --- 今の場面を待ちの丸めで ---

    // 1 セル = 1 ブロックとして count 刻み進める。毎刻み元素とエネルギーが初めと同じ・熱が負でないことを確かめ、
    // 規則 ruleName の Σ f × 刻み を数える
    struct SingleCellRun {
        RxCell cell;
        bool conserved = true;
        Tally tally;
    };

    SingleCellRun RunSingleCell(const BakedReactionTable& table, RxCell cell, uint64_t count, uint64_t cellId,
                                std::string_view ruleName) {
        const std::vector<uint64_t> elements = CountElements(table, cell);
        const int64_t energy = cell.energy;
        SingleCellRun run;
        uint64_t changed = 0;
        for (uint64_t tick = 1; tick <= count; ++tick) {
            run.tally.Expose(RateValue(RuleRate(table, cell, ruleName)), 1.0);
            const RxWaitStep step = StepReactionCellWait(table, cell, test::REACTION_TEST_SEED, tick, changed, cellId);
            if (HashReactionCell(step.cell) != HashReactionCell(cell))
                changed = tick;

            cell = step.cell;
            run.conserved = run.conserved && CountElements(table, cell) == elements && cell.energy == energy &&
                            step.thermal.heat >= 0;
        }

        run.cell = cell;

        return run;
    }

    void TestClosedCellBurn(const BakedReactionTable& table) {
        const RxCell cell = MakeReactionCell(table, CrateAir(table, 38600000), 600000);
        const SingleCellRun run = RunSingleCell(table, cell, 36000, 0, "cellulose_combustion");
        const uint32_t oxygen = RxFindSlot(run.cell, table.SpeciesId("oxygen"));
        const RxThermal thermal = RxComputeThermal(table.View(), run.cell);
        Log(Channel::Reaction, Level::Info, "閉じた木箱 600 K を 36,000 刻み: {:.1f} K・O2 {} µmol・保存 {}",
            thermal.temperature / 1000.0, oxygen == RX_NO_SLOT ? 0 : run.cell.amounts[oxygen], run.conserved);
        Expect(run.conserved, "閉じた木箱: 元素・エネルギーが保存されない / 熱が負");
        Expect(oxygen == RX_NO_SLOT || run.cell.amounts[oxygen] < 983000, "閉じた木箱: 燃えて O2 が減らない");
    }

    void TestSlowReaction(const BakedReactionTable& table) {
        const RxCell cell = MakeReactionCell(table, CelluloseInNitrogen(table), 490000);
        SingleCellRun run = RunSingleCell(table, cell, 36000, 7, "cellulose_pyrolysis");
        const uint32_t cellulose = table.SpeciesId("cellulose");
        run.tally.actual = static_cast<double>(1000000 - run.cell.amounts[RxFindSlot(run.cell, cellulose)]);
        Log(Channel::Reaction, Level::Info, "490 K の遅い熱分解を 36,000 刻み: 期待 {:.1f} / 実際 {:.0f}({:+.2f}σ)",
            run.tally.expected, run.tally.actual, run.tally.Sigma());
        Expect(run.conserved, "490 K の熱分解: 元素・エネルギーが保存されない");
        Expect(std::abs(run.tally.Sigma()) < 5.0, "490 K の遅い熱分解が ±5σ に入らない");
    }

    uint64_t RunVariedCellsWait(const BakedReactionTable& table, uint32_t cellCount, uint64_t tickCount) {
        uint64_t digest = 0;
        for (uint32_t index = 0; index < cellCount; ++index) {
            RxCell cell = test::MakeVariedReactionCell(table, index);
            uint64_t changed = 0;
            for (uint64_t tick = 1; tick <= tickCount; ++tick) {
                const RxWaitStep step = StepReactionCellWait(table, cell, test::REACTION_TEST_SEED, tick, changed,
                                                             index);
                if (HashReactionCell(step.cell) != HashReactionCell(cell))
                    changed = tick;

                cell = step.cell;
            }

            digest = fx::FxHashCombine(digest, HashReactionCell(cell));
        }

        return digest;
    }

    void TestRuleOrderIndependence(const BakedReactionTable& table) {
        const uint64_t original = RunVariedCellsWait(table, 256, 200);
        const uint64_t swapped = RunVariedCellsWait(BakeTestTable(true), 256, 200);
        Log(Channel::Reaction, Level::Info, "規則の並び(待ちの丸め): 元 {:016x} / 逆 {:016x}", original, swapped);
        Expect(original == swapped, "待ちの丸めで規則の並びで結果が変わる");
    }

    // 室温の木箱: D-424 の下限を外したので、いつかは進む。どの規則がいつ進むかを出す(遊びの目安。期待の待ち = 1 ÷ f)
    void TestRoomTemperature(const BakedReactionTable& table) {
        const RxCell cell = MakeReactionCell(table, CrateAir(table, 38600000), 300000);
        const RxWaitStep step = StepReactionCellWait(table, cell, test::REACTION_TEST_SEED, 1, 0, 3);
        Log(Channel::Reaction, Level::Info, "室温(300 K)の木箱:");
        for (const std::string_view name : {"cellulose_pyrolysis", "cellulose_combustion"}) {
            const double perTick = RateValue(RuleRate(table, cell, name));
            Log(Channel::Reaction, Level::Info, "  {}: 1 刻み {:.3e} µmol・期待の待ち {:.3e} 年", name, perTick,
                perTick > 0.0 ? 1.0 / perTick / static_cast<double>(TICKS_PER_YEAR) : 0.0);
        }

        const double wakeYears = step.wakeTick == RX_WAIT_NEVER
                                     ? 0.0
                                     : static_cast<double>(step.wakeTick) / static_cast<double>(TICKS_PER_YEAR);
        Log(Channel::Reaction, Level::Info, "  このセルが次に評価の要る刻み: {}({:.3e} 年後)",
            step.wakeTick == RX_WAIT_NEVER ? std::string("起きない") : std::to_string(step.wakeTick), wakeYears);
        Expect(HashReactionCell(step.cell) == HashReactionCell(cell), "室温の木箱が最初の刻みに変わる");
    }

    int Run() {
        TestWaitDistribution();
        TestWaitMonotonic();
        TestRenewal();

        const BakedReactionTable table = BakeTestTable();
        TestSleepMatchesFull(table);
        TestFastForwardYears(table);
        TestClosedCellBurn(table);
        TestSlowReaction(table);
        TestRuleOrderIndependence(table);
        TestRoomTemperature(table);

        if (failureCount != 0) {
            Log(Channel::Reaction, Level::Error, "reaction_wait_test: {} failure(s)", failureCount);
            return 1;
        }

        Log(Channel::Reaction, Level::Info, "reaction_wait_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
