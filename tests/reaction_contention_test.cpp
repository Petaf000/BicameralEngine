// reaction_contention_test.cpp — 反応物の取り合いで縮めた進行度の丸め(T-0106、D-431)の CPU のテスト。
//   同じセルを刻み(= 乱数)だけ変えて何度も評価し、規則ごとの「縮めた進行度」の合計を、丸める前の値(double)の合計と比べる。
//   切り捨てだと、少ししか進まない脇の反応(酸素不足の火の炭の燃焼・CO など)が毎刻み 0 になって消える。
//   確率的な丸め(reaction.hlsli の RxResolveContention)なら、脇の反応は期待値どおりに進む
//   (資源がほぼ尽きた場面だけ、大きい規則に数 % の偏りが残る。Report の説明)。
//   - 場面: 酸素不足の火(炭と木に少しの O2、900〜1500 K)・吸熱の規則が熱を取り合う・いろいろなセル
//   - どの刻みでも、縮めた後の消費はある量以下(熱は 1/8 まで)
//   切り捨ての結果(前の実装)もテストの中で計算して並べて出す(どれだけ消えていたかの記録。T-0106 の作業ログ)
//   望む進行度は待ちの丸め(T-0105。毎刻み変わるセル = 前の刻みに変わったとして評価。今までの丸めは T-0130 で消した)で作る
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
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

    BakedReactionTable BakeTestTable() {
        auto baked = BakeReactionTable(MakeCombustionTestTable());
        if (!baked) {
            Log(Channel::Reaction, Level::Error, "試験の表をベイクできない: {}", baked.error());
            std::exit(1);
        }

        return std::move(*baked);
    }

    // --- 丸める前の値 ---

    // 1 つの候補の縮めた進行度: 丸める前(double)と切り捨て(前の実装)
    struct Shrunk {
        double exact = 0.0;
        uint64_t floor = 0;
    };

    // extent × available ÷ demand
    Shrunk ShrinkBy(uint64_t extent, uint64_t available, uint64_t demand) {
        const uint64_t quotient = fx::FxDivU128By64(fx::FxMulU64Full(extent, available), demand).quotient;
        const double exact = static_cast<double>(extent) * static_cast<double>(available) / static_cast<double>(demand);

        return {.exact = exact, .floor = quotient};
    }

    // reaction.hlsli の RxResolveContention と同じ要求の合計から、候補ごとの縮めた進行度を出す
    std::vector<Shrunk> ExactShrunk(const BakedReactionTable& table, const RxCell& cell, const RxThermal& thermal,
                                    const RxCandidates& candidates) {
        // --- 要求の合計 ---
        std::array<uint64_t, RX_MAX_CELL_SPECIES> demand{};
        uint64_t heatDemand = 0;
        for (uint32_t c = 0; c < candidates.count; ++c) {
            const RxRule& rule = table.rules[candidates.rules[c]];
            for (uint32_t i = 0; i < rule.reactantCount; ++i)
                demand[RxFindSlot(cell, rule.reactants[i])] += candidates.extents[c] * rule.reactantCoefficients[i];

            if (rule.reactionEnthalpy > 0)
                heatDemand += candidates.extents[c] * static_cast<uint64_t>(rule.reactionEnthalpy);
        }

        // --- 縮める ---
        const uint64_t heatAvailable = thermal.heat > 0
                                           ? static_cast<uint64_t>(thermal.heat) >> RX_ENDOTHERMIC_HEAT_SHIFT
                                           : 0;
        std::vector<Shrunk> result(candidates.count);
        for (uint32_t c = 0; c < candidates.count; ++c) {
            const RxRule& rule = table.rules[candidates.rules[c]];
            const uint64_t extent = candidates.extents[c];
            Shrunk shrunk{.exact = static_cast<double>(extent), .floor = extent};
            const auto narrow = [&](uint64_t available, uint64_t total) {
                const Shrunk value = ShrinkBy(extent, available, total);
                shrunk.exact = std::min(shrunk.exact, value.exact);
                shrunk.floor = std::min(shrunk.floor, value.floor);
            };

            for (uint32_t i = 0; i < rule.reactantCount; ++i) {
                const uint32_t slot = RxFindSlot(cell, rule.reactants[i]);
                if (demand[slot] > cell.amounts[slot])
                    narrow(cell.amounts[slot], demand[slot]);
            }

            if (rule.reactionEnthalpy > 0 && heatDemand > heatAvailable)
                narrow(heatAvailable, heatDemand);

            result[c] = shrunk;
        }

        return result;
    }

    // --- 規則ごとの集計 ---

    struct RuleTally {
        double exact = 0.0;          // 丸める前の合計
        double variance = 0.0;       // 確率的な丸めの分散の合計(端数 f ごとに f(1 − f))
        uint64_t actual = 0;         // 今の実装の合計
        uint64_t floor = 0;          // 切り捨ての合計
        uint64_t shrunkTicks = 0;    // 縮められた刻み
        uint64_t vanishedTicks = 0;  // 縮めると 1 未満になった刻み(切り捨てなら 0 = 消える)
    };

    using Tallies = std::map<uint32_t, RuleTally>;

    // 縮めた後の消費が、どの資源もある量以下か
    bool WithinAvailable(const BakedReactionTable& table, const RxCell& cell, const RxThermal& thermal,
                         const RxCandidates& resolved) {
        std::array<uint64_t, RX_MAX_CELL_SPECIES> used{};
        uint64_t heatUsed = 0;
        for (uint32_t c = 0; c < resolved.count; ++c) {
            const RxRule& rule = table.rules[resolved.rules[c]];
            for (uint32_t i = 0; i < rule.reactantCount; ++i)
                used[RxFindSlot(cell, rule.reactants[i])] += resolved.extents[c] * rule.reactantCoefficients[i];

            if (rule.reactionEnthalpy > 0)
                heatUsed += resolved.extents[c] * static_cast<uint64_t>(rule.reactionEnthalpy);
        }

        bool within = true;
        for (uint32_t slot = 0; slot < cell.speciesCount; ++slot)
            within = within && used[slot] <= cell.amounts[slot];

        const uint64_t heatAvailable = thermal.heat > 0
                                           ? static_cast<uint64_t>(thermal.heat) >> RX_ENDOTHERMIC_HEAT_SHIFT
                                           : 0;
        return within && (heatUsed == 0 || heatUsed <= heatAvailable);
    }

    // 望む進行度(待ちの丸め。毎刻み変わるセル = 前の刻みに変わったとして評価するので、刻みごとに確率 f で 1 単位進む)
    RxCandidates CollectCandidates(const BakedReactionTable& table, const RxCell& cell, uint32_t kelvin, uint64_t tick,
                                   uint64_t cellId) {
        const uint64_t waitSeed = RxWaitSeed(test::REACTION_TEST_SEED, tick, cellId);

        return RxCollectCandidatesWait(table.View(), cell, kelvin, waitSeed, 1).candidates;
    }

    // セル cell を tickCount 回(刻みだけ変えて)評価し、規則ごとに足す。消費がある量を超えたら false
    bool Sample(const BakedReactionTable& table, const RxCell& cell, uint64_t cellId, uint64_t tickCount,
                Tallies& tallies) {
        const RxThermal thermal = RxComputeThermal(table.View(), cell);
        const uint32_t kelvin = static_cast<uint32_t>(thermal.temperature) /
                                static_cast<uint32_t>(MILLIKELVIN_PER_KELVIN);
        const uint32_t tableKelvin = std::min(kelvin, RX_RATE_TABLE_KELVINS - 1);
        bool within = true;
        for (uint64_t tick = 0; tick < tickCount; ++tick) {
            const uint64_t seed = RxRandomSeed(test::REACTION_TEST_SEED, tick, cellId);
            const RxCandidates candidates = CollectCandidates(table, cell, tableKelvin, tick, cellId);
            if (candidates.count == 0)
                continue;

            const RxCandidates resolved = RxResolveContention(table.View(), cell, thermal, candidates, seed);
            within = within && WithinAvailable(table, cell, thermal, resolved);

            const std::vector<Shrunk> shrunk = ExactShrunk(table, cell, thermal, candidates);
            for (uint32_t c = 0; c < candidates.count; ++c) {
                RuleTally& tally = tallies[candidates.rules[c]];
                const double fraction = shrunk[c].exact - std::floor(shrunk[c].exact);
                tally.exact += shrunk[c].exact;
                tally.variance += fraction * (1.0 - fraction);
                tally.actual += resolved.extents[c];
                tally.floor += shrunk[c].floor;
                const bool narrowed = shrunk[c].floor < candidates.extents[c];
                tally.shrunkTicks += narrowed ? 1 : 0;
                tally.vanishedTicks += narrowed && shrunk[c].exact < 1.0 ? 1 : 0;
            }
        }

        return within;
    }

    // 規則ごとに表示し、今の実装の合計が丸める前の合計に近いかを確かめる。切り捨てで半分も進まなかった規則(消えていた脇の反応)は
    // ±5σ、ほかは ±5σ + 合計の relativeTolerance。係数の大きい規則は、丸め上げに要る量が残り(端数の合計)に入らない刻みがあり、
    // 少し遅れる。資源がほぼ尽きた場面では、それと戻す丸め上げ(大きい規則に寄せる)で、大きい規則に数 % の偏りが残る
    // (残った資源は次の刻みに使われる。T-0106)
    void Report(const BakedReactionTable& table, std::string_view label, const Tallies& tallies, uint64_t tickCount,
                double relativeTolerance) {
        Log(Channel::Reaction, Level::Info, "  {}({} 刻み)", label, tickCount);
        for (const auto& [rule, tally] : tallies) {
            if (tally.shrunkTicks == 0)
                continue;

            const auto ticks = static_cast<double>(tickCount);
            const double floorRatio = tally.exact > 0.0 ? 100.0 * static_cast<double>(tally.floor) / tally.exact
                                                        : 100.0;
            const double actualRatio = tally.exact > 0.0 ? 100.0 * static_cast<double>(tally.actual) / tally.exact
                                                         : 100.0;
            Log(Channel::Reaction, Level::Info,
                "    {:<26} 1 刻み 期待 {:>12.4f} / 切り捨て {:>12.4f}({:5.1f}%)/ 丸め {:>12.4f}({:5.1f}%)"
                " 縮めた {} 刻み・うち 1 未満 {}",
                table.ruleNames[rule], tally.exact / ticks, static_cast<double>(tally.floor) / ticks, floorRatio,
                static_cast<double>(tally.actual) / ticks, actualRatio, tally.shrunkTicks, tally.vanishedTicks);

            const double error = std::abs(static_cast<double>(tally.actual) - tally.exact);
            const bool wasVanishing = static_cast<double>(tally.floor) < 0.5 * tally.exact;
            const double tolerance = wasVanishing ? 0.0 : relativeTolerance;
            const double allowed = 5.0 * std::sqrt(tally.variance) + tolerance * tally.exact + 1.0;
            Expect(error <= allowed, std::format("{}: {} の丸めが期待値からずれる(差 {:.1f}・許す {:.1f})", label,
                                                 table.ruleNames[rule], error, allowed));
        }
    }

    // --- 場面 ---

    // 木箱のセル(セルロース・炭・N2)に少しの O2
    RxCell StarvedFire(const BakedReactionTable& table, uint64_t carbon, uint64_t oxygen, int32_t milliKelvin) {
        const std::vector<SpeciesAmount> amounts = {{.species = table.SpeciesId("cellulose"), .amount = 38600000},
                                                    {.species = table.SpeciesId("carbon"), .amount = carbon},
                                                    {.species = table.SpeciesId("oxygen"), .amount = oxygen},
                                                    {.species = table.SpeciesId("nitrogen"), .amount = 3697000},
                                                    {.species = table.SpeciesId("carbon_dioxide"), .amount = 200000}};

        return MakeReactionCell(table, amounts, milliKelvin);
    }

    // 酸素不足の火の 9 つの場面。消費がある量を超えたら false
    bool SampleStarvedFires(const BakedReactionTable& table) {
        constexpr uint64_t TICKS = 20000;
        constexpr std::array<int32_t, 3> KELVINS = {900, 1200, 1500};
        constexpr std::array<uint64_t, 3> OXYGEN = {50, 2000, 100000};
        bool within = true;
        uint64_t cellId = 0;
        for (const int32_t kelvin : KELVINS) {
            for (const uint64_t oxygen : OXYGEN) {
                Tallies tallies;
                const RxCell cell = StarvedFire(table, 5000000, oxygen, kelvin * 1000);
                within = Sample(table, cell, cellId++, TICKS, tallies) && within;
                const double tolerance = oxygen < 100 ? 0.08 : 0.002;
                Report(table, std::format("酸素不足の火 {} K・O2 {} µmol", kelvin, oxygen), tallies, TICKS, tolerance);
            }
        }

        return within;
    }

    void TestStarvedFire(const BakedReactionTable& table) {
        Expect(SampleStarvedFires(table), "酸素不足の火: 縮めた後の消費がある量を超える");
    }

    // 吸熱の Boudouard が熱を使い切ろうとし、炭の燃焼が少しの O2 で進む
    void TestEndothermicContention(const BakedReactionTable& table) {
        constexpr uint64_t TICKS = 20000;
        const std::vector<SpeciesAmount> amounts = {
            {.species = table.SpeciesId("carbon"), .amount = 1000000000},
            {.species = table.SpeciesId("carbon_dioxide"), .amount = 1000000000},
            {.species = table.SpeciesId("oxygen"), .amount = 30},
        };
        Tallies tallies;
        const RxCell cell = MakeReactionCell(table, amounts, 2000000);
        Expect(Sample(table, cell, 100, TICKS, tallies), "吸熱: 縮めた後の消費がある量(熱の 1/8)を超える");
        Report(table, "吸熱 2000 K と少しの O2", tallies, TICKS, 0.002);
    }

    // いろいろなセル(reaction_test と同じ列)を 64 刻みずつ
    void TestVariedCells(const BakedReactionTable& table) {
        constexpr uint32_t CELLS = 4096;
        constexpr uint64_t TICKS = 64;
        Tallies tallies;
        bool within = true;
        for (uint32_t index = 0; index < CELLS; ++index)
            within = Sample(table, test::MakeVariedReactionCell(table, index), index, TICKS, tallies) && within;

        Expect(within, "いろいろなセル: 縮めた後の消費がある量を超える");
        Report(table, std::format("いろいろなセル {} 個", CELLS), tallies, TICKS * CELLS, 0.002);
    }

    int Run() {
        const BakedReactionTable table = BakeTestTable();
        TestStarvedFire(table);
        TestEndothermicContention(table);
        TestVariedCells(table);

        if (failureCount != 0) {
            Log(Channel::Reaction, Level::Error, "reaction_contention_test: {} failure(s)", failureCount);
            return 1;
        }

        Log(Channel::Reaction, Level::Info, "reaction_contention_test: OK");

        return 0;
    }

}  // namespace

int main() {
    const int exitCode = Run();
    SingletonFinalizer::Finalize();

    return exitCode;
}
