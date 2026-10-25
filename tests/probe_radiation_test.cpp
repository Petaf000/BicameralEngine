// probe_radiation_test.cpp — 仮の世界の CPU リファレンス(sim::ProbeReference)の熱の放射の既定の段(RC。T-1113)を確かめる。
//
// 場面: サンプルの世界の表(MakeCombustionTestTable。real_matter の光の値を写している)の初めの世界(空気の中の木箱)で、
//       reaction_package_scene と同じ壁に火をつけて進める。
// 確かめること:
//   1. 段が既定で動く(光の値を書いた表)。光の値を 1 つも書かない表では流さず、段を切った世界と毎刻み同じ
//   2. 保存(ビット単位): Σ セルのエネルギー × 2^16 + Σ 持ち越し + Σ 外へ出た + Σ 丸め − Σ つつき × 2^16 が毎刻み一定
//   3. 温度が 0 K 以下になるセルが無い(陽の段の安定の見張り)
//   4. 決定的: 2 回走らせてハッシュの列が同じ
//   5. 放射が火の熱を運ぶ: 箱の中の空気を挟んで向かい合う壁(3 m 先。伝導では 1 秒で届かない)が、段を切った世界より熱い
// `--measure ticks` は比べずに、段あり / なしの燃え方(600 K 以上のセル・炭・CO2)と 1 刻みの時間を表示する(T-1113 の記録)。
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

#include "core/aliases.h"
#include "reaction_test_table.h"
#include "sim/probe_sim.h"

using namespace bicameral;
using namespace bicameral::sim;

namespace {

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr uint64_t TOTAL_TICKS = 60;
    constexpr uint32_t FIRE_X = 28;
    constexpr uint32_t FIRE_Y = 32;
    constexpr uint32_t FACING_X = 35;  // 木箱(data/packages/combustion_test/world_cells.luau: 28〜35・殻 1)の向かいの壁

    // reaction_package_scene と同じ(frame_loop の --auto-ignite の壁)
    std::vector<ProbeCommand> MakeCommands() {
        return {MakePokeCommand(0, 0, FIRE_X, FIRE_Y, PROBE_VIEW_Z),
                MakePokeCommand(0, 1, FIRE_X, FIRE_Y + 1, PROBE_VIEW_Z),
                MakePokeCommand(7, 2, FIRE_X, FIRE_Y, PROBE_VIEW_Z),
                MakePokeCommand(19, 3, FIRE_X, FIRE_Y - 1, PROBE_VIEW_Z)};
    }

    uint64_t CarrySum(const CascadeCarry& carry) {
        uint64_t sum = 0;
        for (const CascadeCarryEntry& entry : carry.entries)
            sum += entry.amount;

        return sum;
    }

    BakedReactionTable WithoutLight(ReactionTableDefinition definition) {
        for (SpeciesDefinition& species : definition.species) {
            species.radiationAbsorption = 0;
            species.lightAbsorptionShort.reset();
            species.lightAbsorptionLong.reset();
            species.lightReflectanceShortPermille.reset();
            species.lightReflectanceLongPermille.reset();
        }

        return *BakeReactionTable(definition);
    }

    // --- 進めて、毎刻みの保存とハッシュを取る ---

    struct Run {
        std::vector<uint64_t> hashes;
        bool conserved = true;
        bool ranEveryTick = true;
        bool ranAnyTick = false;
        uint32_t negativeCells = 0;
        uint32_t facingTemperature = 0;  // 最後の刻みの、火をつけた壁に向かい合う壁(箱の中の空気の 3 m 先)の温度(mK)
    };

    Run RunScene(const BakedReactionTable& table, bool radiation, uint64_t ticks) {
        ProbeReference reference(table);
        reference.SetCascadeRadiation({.enabled = radiation});
        const std::vector<ProbeCommand> commands = MakeCommands();

        Run run;
        uint64_t outside = 0;  // Σ 外へ出た + Σ 丸め − Σ つつき × 2^16(mod 2^64)
        const auto total = [&](uint64_t tick) {
            return (ProbeEnergySum(reference.State(tick)) << reaction::RC_ENERGY_SHIFT) +
                   CarrySum(reference.CascadeRadiationCarry()) + outside;
        };
        const uint64_t initial = total(0);
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            reference.Advance(tick, commands);
            const ProbeCascadeReport& report = reference.LastCascadeReport();
            outside += report.escaped + static_cast<uint64_t>(report.rounding) -
                       (reference.SourceEnergy() << reaction::RC_ENERGY_SHIFT);
            run.conserved = run.conserved && total(tick + 1) == initial;
            run.ranEveryTick = run.ranEveryTick && report.ran;
            run.ranAnyTick = run.ranAnyTick || report.ran;
            run.negativeCells += report.negativeCells;
            run.hashes.push_back(reference.StateHash(tick + 1));
        }

        run.facingTemperature = reference.Caches(ticks)[ProbeCellIndex(FACING_X, FIRE_Y, PROBE_VIEW_Z)].temperature;

        return run;
    }

    void TestDefault(const BakedReactionTable& table) {
        const Run first = RunScene(table, true, TOTAL_TICKS);
        const Run second = RunScene(table, true, TOTAL_TICKS);
        const Run without = RunScene(table, false, TOTAL_TICKS);

        EXPECT(first.ranEveryTick);
        EXPECT(first.conserved);
        EXPECT(first.negativeCells == 0);
        EXPECT(first.hashes == second.hashes);
        EXPECT(!without.ranAnyTick);
        EXPECT(first.hashes != without.hashes);
        EXPECT(first.facingTemperature > without.facingTemperature);
        std::printf("向かいの壁(%llu 刻み): 放射あり %.3f K・なし %.3f K\n",
                    static_cast<unsigned long long>(TOTAL_TICKS), first.facingTemperature / 1000.0,
                    without.facingTemperature / 1000.0);
    }

    void TestNoLight(const ReactionTableDefinition& definition) {
        const BakedReactionTable table = WithoutLight(definition);
        const Run on = RunScene(table, true, TOTAL_TICKS / 4);
        const Run off = RunScene(table, false, TOTAL_TICKS / 4);

        EXPECT(!on.ranAnyTick);
        EXPECT(on.hashes == off.hashes);
    }

    // --- 測り(--measure)---

    void Measure(const BakedReactionTable& table, uint64_t ticks) {
        const uint32_t carbon = table.SpeciesId("carbon");
        const uint32_t dioxide = table.SpeciesId("carbon_dioxide");
        for (const bool radiation : {true, false}) {
            ProbeReference reference(table);
            reference.SetCascadeRadiation({.enabled = radiation});
            const std::vector<ProbeCommand> commands = MakeCommands();
            int64_t radiationMicroseconds = 0;
            for (uint64_t tick = 0; tick < ticks; ++tick) {
                const auto start = chr::steady_clock::now();
                reference.Advance(tick, commands);
                radiationMicroseconds += chr::duration_cast<chr::microseconds>(chr::steady_clock::now() - start)
                                             .count();
                if ((tick + 1) % 60 != 0)
                    continue;

                const auto state = reference.State(tick + 1);
                const auto caches = reference.Caches(tick + 1);
                uint32_t hot = 0;
                uint32_t hottest = 0;
                uint64_t carbonAmount = 0;
                uint64_t dioxideAmount = 0;
                for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index) {
                    hot += caches[index].temperature >= 600000 ? 1u : 0u;
                    hottest = std::max(hottest, caches[index].temperature);
                    carbonAmount += ProbeViewAmount(state[index], carbon);
                    dioxideAmount += ProbeViewAmount(state[index], dioxide);
                }

                const ProbeCascadeReport& report = reference.LastCascadeReport();
                std::printf(
                    "放射%s %3llu 秒: 600 K 以上 %5u  最高 %6.0f K  炭 %12llu  CO2 %12llu µmol  光線 %8llu  %.1f "
                    "ms/刻み\n",
                    radiation ? "あり" : "なし", static_cast<unsigned long long>((tick + 1) / 60), hot,
                    hottest / 1000.0, static_cast<unsigned long long>(carbonAmount),
                    static_cast<unsigned long long>(dioxideAmount), static_cast<unsigned long long>(report.rays),
                    radiationMicroseconds / 60000.0);
                radiationMicroseconds = 0;
            }
        }
    }

}  // namespace

int main(int argc, char** argv) {
    const ReactionTableDefinition definition = MakeCombustionTestTable();
    const auto table = BakeReactionTable(definition);
    if (!table) {
        std::printf("FAILED: 表を作れない: %s\n", table.error().c_str());
        return 1;
    }

    if (argc >= 3 && std::string_view(argv[1]) == "--measure") {
        Measure(*table, std::strtoull(argv[2], nullptr, 10));
        return 0;
    }

    TestDefault(*table);
    TestNoLight(definition);
    if (failureCount != 0)
        return 1;

    std::printf("probe_radiation: OK\n");

    return 0;
}
