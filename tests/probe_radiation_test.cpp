// probe_radiation_test.cpp — 仮の世界の CPU リファレンス(sim::ProbeReference)の熱の放射の既定の段(RC。T-1113)を確かめる。
//
// 場面: サンプルの世界の表(MakeCombustionTestTable。real_matter の光の値を写している)の初めの世界(空気の中の木箱)で、
//       reaction_package_scene と同じ壁に火をつけて進める。
// 確かめること:
//   1. 段が既定で動く。光の値を 1 つも書かない表でも流れる(D-670。書かない固体は D-610 の灰色で放つ)
//   2. 保存(ビット単位): Σ セルのエネルギー × 2^16 + Σ 持ち越し + Σ 外へ出た + Σ 丸め − Σ つつき × 2^16 が毎刻み一定
//   3. 温度が 0 K 以下になるセルが無い(陽の段の安定の見張り)
//   4. 決定的: 2 回走らせてハッシュの列が同じ
//   5. 放射が火の熱を運ぶ: 箱の中の空気を挟んで向かい合う壁(3 m 先。伝導では 1 秒で届かない)が、段を切った世界より熱い
//   6. 世界の外の扱い(T-1155・T-1164): 何もしない 300 K の世界は、外 = 0 K の空では面のブロックが毎刻み起き、
//      参照の温度(300 K。ADR-0461)の形では 1 刻み目から何も放たず全部眠る。参照の温度の形でも火の世界の保存(正 − 負の持ち越し)が
//      ビット単位で一定・決定的・火の熱は向かいの壁へ届く
//   7. 既定の外は環境の温度の物(D-671・ADR-0478): 参照の温度 = 表の基準の大気の高さ 0 の温度(試験の表は earth_environment の 288.15 K)。
//      基準の大気を書かない表は 0 K の空。基準の大気を 300 K にした表では、何もしない 300 K の世界が既定の値のまま全部眠る
// `--measure ticks` は比べずに、段あり / なしの燃え方(600 K 以上のセル・炭・CO2)と 1 刻みの時間を表示する(T-1113 の記録)。
// `--measure-outside ticks` は世界の外の扱い 2 つ(0 K の空 / 参照の温度)で、何もしない世界と火の世界の起きたブロック・光線・時間を表示する(T-1164)。
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
#include "sim/world_atmosphere.h"

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
    constexpr uint32_t WORLD_MK = 300000;  // 初めの世界の温度(world_cells.luau)= 参照の温度の形の参照

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

    // 参照の温度を決めた値にする(environmentReference = false。0 = 0 K の空)か、既定(表の基準の大気。ENVIRONMENT)
    constexpr uint32_t ENVIRONMENT = UINT32_MAX;

    ProbeCascadeOptions Options(bool radiation, uint32_t referenceMk) {
        ProbeCascadeOptions options{.enabled = radiation};
        if (referenceMk == ENVIRONMENT)
            return options;

        options.environmentReference = false;
        options.params.referenceMk = referenceMk;

        return options;
    }

    // 基準の大気の温度をどの点も temperatureMk にした表(成分と圧力はそのまま)
    BakedReactionTable WithAtmosphereTemperature(ReactionTableDefinition definition, uint32_t temperatureMk) {
        for (AtmospherePointDefinition& point : definition.atmosphere.points)
            point.temperatureMilliKelvin = temperatureMk;

        return *BakeReactionTable(definition);
    }

    Run RunScene(const BakedReactionTable& table, bool radiation, uint64_t ticks, uint32_t referenceMk = ENVIRONMENT) {
        ProbeReference reference(table);
        reference.SetCascadeRadiation(Options(radiation, referenceMk));
        const std::vector<ProbeCommand> commands = MakeCommands();

        Run run;
        uint64_t outside = 0;  // Σ 外へ出た + Σ 丸め − Σ つつき × 2^16(mod 2^64)
        const auto total = [&](uint64_t tick) {
            return (ProbeEnergySum(reference.State(tick)) << reaction::RC_ENERGY_SHIFT) +
                   CarrySum(reference.CascadeRadiationCarry()) - CarrySum(reference.CascadeRadiationCarryNegative()) +
                   outside;
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

        EXPECT(!ProbeTableWritesLight(table));
        EXPECT(on.ranEveryTick);  // D-670: 書かない表でも流す
        EXPECT(on.conserved);
        EXPECT(on.negativeCells == 0);
        EXPECT(on.hashes != off.hashes);  // 書かない木箱は灰色で放つ
    }

    // --- 世界の外の扱い(T-1164)---

    struct QuietRun {
        uint32_t lastScheduled = 0;  // 最後の刻みに予定したブロック
        uint64_t scheduledSum = 0;   // 予定したブロックの和
        uint64_t changedCells = 0;   // 放射でエネルギーが変わったセルの和
        uint64_t rays = 0;
        int64_t microseconds = 0;
    };

    QuietRun RunQuiet(const BakedReactionTable& table, bool radiation, uint32_t referenceMk, uint64_t ticks,
                      bool fire = false) {
        ProbeReference reference(table);
        reference.SetCascadeRadiation(Options(radiation, referenceMk));
        const std::vector<ProbeCommand> commands = fire ? MakeCommands() : std::vector<ProbeCommand>{};

        QuietRun run;
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            const auto start = chr::steady_clock::now();
            reference.Advance(tick, commands);
            run.microseconds += chr::duration_cast<chr::microseconds>(chr::steady_clock::now() - start).count();
            run.lastScheduled = reference.ScheduledBlocks();
            run.scheduledSum += reference.ScheduledBlocks();
            run.changedCells += reference.LastCascadeReport().changedCells;
            run.rays += reference.LastCascadeReport().rays;
        }

        return run;
    }

    void TestOutside(const BakedReactionTable& table) {
        constexpr uint64_t QUIET_TICKS = 20;
        const QuietRun space = RunQuiet(table, true, 0, QUIET_TICKS);  // 決めた値(0 K の空)
        const QuietRun referenced = RunQuiet(table, true, WORLD_MK, QUIET_TICKS);
        const QuietRun off = RunQuiet(table, false, 0, QUIET_TICKS);

        EXPECT(space.changedCells > 0);  // 0 K の空へ面が冷える(眠らない)
        EXPECT(space.lastScheduled > off.lastScheduled);
        EXPECT(referenced.changedCells == 0);  // 参照の温度の世界は何も放たない
        EXPECT(referenced.rays == 0);
        EXPECT(referenced.lastScheduled == off.lastScheduled);
        std::printf("何もしない 300 K の世界(%llu 刻み目): 予定したブロック 0 K の空 %u・参照の温度 %u・放射なし %u\n",
                    static_cast<unsigned long long>(QUIET_TICKS), space.lastScheduled, referenced.lastScheduled,
                    off.lastScheduled);

        const Run first = RunScene(table, true, TOTAL_TICKS, WORLD_MK);
        const Run second = RunScene(table, true, TOTAL_TICKS, WORLD_MK);
        const Run without = RunScene(table, false, TOTAL_TICKS);
        EXPECT(first.ranEveryTick);
        EXPECT(first.conserved);
        EXPECT(first.negativeCells == 0);
        EXPECT(first.hashes == second.hashes);
        EXPECT(first.facingTemperature > without.facingTemperature);
    }

    // --- 既定の外 = 環境の温度の物(D-671)---

    void TestEnvironmentDefault(const ReactionTableDefinition& definition, const BakedReactionTable& table) {
        constexpr uint64_t QUIET_TICKS = 20;

        // 試験の表(earth_environment の基準の大気)の参照の温度は高さ 0 の温度
        EXPECT(!table.atmosphere.Empty());
        const ProbeReference earth(table);
        EXPECT(earth.CascadeReferenceMk() == SampleAtmosphere(table.atmosphere, 0).temperatureMilliKelvin);
        EXPECT(ProbeCascadeReferenceMk({}, table) == earth.CascadeReferenceMk());
        std::printf("試験の表の参照の温度(基準の大気の高さ 0): %.2f K\n", earth.CascadeReferenceMk() / 1000.0);

        // 基準の大気を書かない表は 0 K の空
        ReactionTableDefinition bare = definition;
        bare.atmosphere = {};
        const BakedReactionTable bareTable = *BakeReactionTable(bare);
        EXPECT(ProbeReference(bareTable).CascadeReferenceMk() == 0);

        // 環境の温度 = 世界の温度(300 K)の表: 既定の値のまま何もしない世界が全部眠る
        const BakedReactionTable warm = WithAtmosphereTemperature(definition, WORLD_MK);
        EXPECT(ProbeReference(warm).CascadeReferenceMk() == WORLD_MK);
        const QuietRun quiet = RunQuiet(warm, true, ENVIRONMENT, QUIET_TICKS);
        const QuietRun off = RunQuiet(warm, false, 0, QUIET_TICKS);
        EXPECT(quiet.changedCells == 0);
        EXPECT(quiet.rays == 0);
        EXPECT(quiet.lastScheduled == off.lastScheduled);
        EXPECT(quiet.scheduledSum == off.scheduledSum);
        std::printf(
            "環境の温度 300 K の表・何もしない 300 K の世界: 予定したブロック 既定 %u・放射なし %u(和 %llu・%llu)\n",
            quiet.lastScheduled, off.lastScheduled, static_cast<unsigned long long>(quiet.scheduledSum),
            static_cast<unsigned long long>(off.scheduledSum));

        // 同じ表の火の世界: 保存・決定的・決めた値 300 K と同じ
        const Run first = RunScene(warm, true, TOTAL_TICKS);
        const Run second = RunScene(warm, true, TOTAL_TICKS);
        EXPECT(first.ranEveryTick);
        EXPECT(first.conserved);
        EXPECT(first.negativeCells == 0);
        EXPECT(first.hashes == second.hashes);
        EXPECT(first.hashes == RunScene(warm, true, TOTAL_TICKS, WORLD_MK).hashes);
    }

    void MeasureOutside(const BakedReactionTable& table, uint64_t ticks) {
        for (const bool fire : {false, true}) {
            for (const uint32_t referenceMk : {0u, WORLD_MK}) {
                const QuietRun run = RunQuiet(table, true, referenceMk, ticks, fire);
                std::printf(
                    "%s・外 %s: 予定したブロック 平均 %.1f・最後 %u  放射で変わったセル 平均 %.1f  光線 平均 %.0f  "
                    "%.1f ms/刻み\n",
                    fire ? "火の世界" : "何もしない世界", referenceMk == 0 ? "0 K の空" : "参照の温度 300 K",
                    static_cast<double>(run.scheduledSum) / static_cast<double>(ticks), run.lastScheduled,
                    static_cast<double>(run.changedCells) / static_cast<double>(ticks),
                    static_cast<double>(run.rays) / static_cast<double>(ticks),
                    static_cast<double>(run.microseconds) / 1000.0 / static_cast<double>(ticks));
            }
        }

        const QuietRun off = RunQuiet(table, false, 0, ticks);
        std::printf("何もしない世界・放射なし: 予定したブロック 平均 %.1f・最後 %u  %.1f ms/刻み\n",
                    static_cast<double>(off.scheduledSum) / static_cast<double>(ticks), off.lastScheduled,
                    static_cast<double>(off.microseconds) / 1000.0 / static_cast<double>(ticks));
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

    if (argc >= 3 && std::string_view(argv[1]) == "--measure-outside") {
        MeasureOutside(*table, std::strtoull(argv[2], nullptr, 10));
        return 0;
    }

    TestDefault(*table);
    TestNoLight(definition);
    TestOutside(*table);
    TestEnvironmentDefault(definition, *table);
    if (failureCount != 0)
        return 1;

    std::printf("probe_radiation: OK\n");

    return 0;
}
