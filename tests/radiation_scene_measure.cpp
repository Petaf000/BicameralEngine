// radiation_scene_measure.cpp — 草原の延焼の場面(data/scenes/grass_fire_river)を、放射の段を入れた仮の世界の CPU リファレンスで流して測る
// (T-0585・ADR-0263)。ctest の radiation_scene は短い煙の試し(2 秒)。測る時は runner/radiation_scene_settings.txt に設定を書いて
// job.py test -Filter radiation_scene -Show(GPU の順番待ちを通らない)か、job.py run -Exe radiation_scene_measure -- <設定> ...。
//
// 設定(1 つの引数 = 1 回。カンマ区切り。書かない鍵は既定):
//   w=<川の幅のセル>(既定 4 = 2 m。向こう岸 x 44〜63 は変えず、手前の岸を伸び縮みさせる)・gap=1(川の代わりに草の無い土の隙間)
//   ac=<セルロースの a>・ak=<炭(carbon)の a>・as=<砂の a>・aw=<水の a>(mm²/mol。既定 0)・rad=0(放射の段を入れない)
//   seed=<向きの種>・sec=<上限の秒>(既定 60)
// 出力(1 行 1 回): 向こう岸に火が移った時刻(向こう岸の草が 1% 減った)・向こう岸に残った草・火が消えた時刻・1 刻みの時間(放射の段の分も)・
//   川の水(液体の water)の始めと終わり・終わりの湯気(water_vapor。燃焼の生成物も入る)。川の水が減った分が蒸発(潜熱を吸う。T-0611)。
// 設定はスレッドで並べて走らせる(それぞれ独立の世界。結果は決定的)。
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "script/reaction_table_loader.h"
#include "sim/probe_sim.h"

namespace {

    using namespace bicameral;
    using namespace bicameral::sim;
    namespace chr = std::chrono;

    constexpr uint32_t RIVER_FIRST_X = 40;  // 場面の川: x 40〜43
    constexpr uint32_t FAR_FIRST_X = 44;    // 向こう岸の草: x 44〜63・y 4
    constexpr uint32_t GRASS_Y = 4;
    constexpr uint32_t HOT_MK = 1000000;  // 火 = 1000 K 以上のセル
    constexpr uint64_t TICKS_PER_SECOND = 60;

    struct Setting {
        std::string text;
        uint32_t riverCells = 4;
        bool gap = false;
        bool radiation = true;
        uint32_t absorptionCellulose = 0;
        uint32_t absorptionCarbon = 0;
        uint32_t absorptionSilica = 0;
        uint32_t absorptionWater = 0;
        uint64_t seed = 0;
        uint64_t seconds = 60;
        uint32_t grassLayers = 1;  // 草の層の数(火の強さの代わり。y 4 から上へ重ねる。既定 1 = 場面のまま)
        bool trace = false;        // 1 秒ごとに火の数・手前の岸の前線と向こう岸の最初の列の最高温度を出す
    };

    struct Outcome {
        double farIgnitionSeconds = -1;
        double farLeftPercent = 0;
        double fireOutSeconds = -1;
        double millisecondsPerTick = 0;
        double radiationMillisecondsPerTick = 0;
        uint64_t pairsPerTick = 0;
        uint32_t hottestMk = 0;
        uint64_t riverWaterStart = 0;  // µmol(川の列の液体の水)
        uint64_t riverWaterEnd = 0;
        uint64_t vaporEnd = 0;  // µmol(世界全部の湯気)
    };

    Setting ParseSetting(std::string_view text) {
        Setting setting;
        setting.text = std::string(text);
        size_t start = 0;
        while (start < text.size()) {
            const size_t end = std::min(text.find(',', start), text.size());
            const std::string_view item = text.substr(start, end - start);
            const size_t equals = item.find('=');
            if (equals != std::string_view::npos) {
                const std::string key(item.substr(0, equals));
                const uint64_t value = std::strtoull(std::string(item.substr(equals + 1)).c_str(), nullptr, 10);
                if (key == "w")
                    setting.riverCells = static_cast<uint32_t>(value);
                else if (key == "gap")
                    setting.gap = value != 0;
                else if (key == "rad")
                    setting.radiation = value != 0;
                else if (key == "ac")
                    setting.absorptionCellulose = static_cast<uint32_t>(value);
                else if (key == "ak")
                    setting.absorptionCarbon = static_cast<uint32_t>(value);
                else if (key == "as")
                    setting.absorptionSilica = static_cast<uint32_t>(value);
                else if (key == "aw")
                    setting.absorptionWater = static_cast<uint32_t>(value);
                else if (key == "seed")
                    setting.seed = value;
                else if (key == "sec")
                    setting.seconds = value;
                else if (key == "tall")
                    setting.grassLayers = static_cast<uint32_t>(value);
                else if (key == "trace")
                    setting.trace = value != 0;
            }

            start = end + 1;
        }

        return setting;
    }

    void SetAbsorption(BakedReactionTable& table, std::string_view name, uint32_t value) {
        for (size_t id = 0; id < table.speciesNames.size(); ++id) {
            if (table.speciesNames[id] == name && id < table.speciesRadiationAbsorption.size())
                table.speciesRadiationAbsorption[id] = value;
        }
    }

    // 列(x が同じセル全部)を写す
    void CopyColumn(std::vector<reaction::RxCell>& world, uint32_t from, uint32_t to) {
        for (uint32_t z = 0; z < PROBE_GRID_SIZE; ++z) {
            for (uint32_t y = 0; y < PROBE_GRID_SIZE; ++y)
                world[ProbeCellIndex(to, y, z)] = world[ProbeCellIndex(from, y, z)];
        }
    }

    // 川の幅を変える(向こう岸は x 44 から変えない)。gap なら川の列を草の無い土の列にする
    std::vector<reaction::RxCell> MakeWorld(const BakedReactionTable& table, const Setting& setting) {
        std::vector<reaction::RxCell> world = MakeProbeInitialWorld(table);
        const uint32_t riverStart = FAR_FIRST_X - setting.riverCells;
        for (uint32_t x = riverStart; x < RIVER_FIRST_X; ++x)
            CopyColumn(world, RIVER_FIRST_X, x);
        for (uint32_t x = RIVER_FIRST_X; x < riverStart; ++x)
            CopyColumn(world, RIVER_FIRST_X - 1, x);

        if (setting.gap) {
            for (uint32_t x = riverStart; x < FAR_FIRST_X; ++x) {
                CopyColumn(world, RIVER_FIRST_X - 1, x);
                for (uint32_t z = 0; z < PROBE_GRID_SIZE; ++z)
                    world[ProbeCellIndex(x, GRASS_Y, z)] = world[ProbeCellIndex(x, GRASS_Y + 1, z)];
            }
        }

        // 背の高い草: 草のある列(y 4 にセルロースがある列)の y 4 を上へ写す
        for (uint32_t layer = 1; layer < setting.grassLayers; ++layer) {
            for (uint32_t z = 0; z < PROBE_GRID_SIZE; ++z) {
                for (uint32_t x = 0; x < PROBE_GRID_SIZE; ++x) {
                    const bool grass = x < riverStart || x >= FAR_FIRST_X;
                    if (grass && !setting.gap)
                        world[ProbeCellIndex(x, GRASS_Y + layer, z)] = world[ProbeCellIndex(x, GRASS_Y, z)];
                }
            }
        }

        return world;
    }

    uint64_t FarGrass(const BakedReactionTable& table, std::span<const reaction::RxCell> cells) {
        uint32_t cellulose = UINT32_MAX;
        for (size_t id = 0; id < table.speciesNames.size(); ++id) {
            if (table.speciesNames[id] == "cellulose")
                cellulose = static_cast<uint32_t>(id);
        }

        uint64_t amount = 0;
        for (uint32_t z = 0; z < PROBE_GRID_SIZE; ++z) {
            for (uint32_t x = FAR_FIRST_X; x < PROBE_GRID_SIZE; ++x) {
                const reaction::RxCell& cell = cells[ProbeCellIndex(x, GRASS_Y, z)];
                for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                    if (cell.species[i] == cellulose)
                        amount += cell.amounts[i];
                }
            }
        }

        return amount;
    }

    // 列 x の範囲 [firstX, endX) にある物質 name の合計(µmol。全部の y・z)
    uint64_t AmountInColumns(const BakedReactionTable& table, std::span<const reaction::RxCell> cells,
                             std::string_view name, uint32_t firstX, uint32_t endX) {
        uint32_t wanted = UINT32_MAX;
        for (size_t id = 0; id < table.speciesNames.size(); ++id) {
            if (table.speciesNames[id] == name)
                wanted = static_cast<uint32_t>(id);
        }

        uint64_t amount = 0;
        for (uint32_t z = 0; z < PROBE_GRID_SIZE; ++z) {
            for (uint32_t y = 0; y < PROBE_GRID_SIZE; ++y) {
                for (uint32_t x = firstX; x < endX; ++x) {
                    const reaction::RxCell& cell = cells[ProbeCellIndex(x, y, z)];
                    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                        if (cell.species[i] == wanted)
                            amount += cell.amounts[i];
                    }
                }
            }
        }

        return amount;
    }

    Outcome Run(const BakedReactionTable& baseTable, const Setting& setting) {
        BakedReactionTable table = baseTable;
        SetAbsorption(table, "cellulose", setting.absorptionCellulose);
        SetAbsorption(table, "carbon", setting.absorptionCarbon);
        SetAbsorption(table, "silica", setting.absorptionSilica);
        SetAbsorption(table, "water", setting.absorptionWater);

        const std::vector<reaction::RxCell> world = MakeWorld(table, setting);
        ProbeReference reference(table, world);
        if (setting.radiation)
            reference.EnableRadiation({.seed = setting.seed});

        const uint64_t farStart = FarGrass(table, reference.State(0));
        const std::vector<ProbeCommand> ignite = {MakePokeCommand(0, 0, 4, GRASS_Y, 32)};
        const uint32_t riverStart = FAR_FIRST_X - setting.riverCells;
        Outcome outcome;
        outcome.riverWaterStart = AmountInColumns(table, reference.State(0), "water", riverStart, FAR_FIRST_X);
        chr::nanoseconds total{};
        uint64_t pairs = 0;
        const uint64_t lastTick = setting.seconds * TICKS_PER_SECOND;
        uint64_t tick = 0;
        for (; tick < lastTick; ++tick) {
            const auto before = chr::steady_clock::now();
            reference.Advance(tick,
                              tick == 0 ? std::span<const ProbeCommand>(ignite) : std::span<const ProbeCommand>());
            total += chr::steady_clock::now() - before;
            pairs += reference.LastRadiationReport().pairs;

            if ((tick + 1) % TICKS_PER_SECOND != 0)
                continue;

            const auto cells = reference.State(tick + 1);
            const auto caches = reference.Caches(tick + 1);
            uint32_t hot = 0;
            for (const reaction::HcThermalCache& cache : caches) {
                hot += cache.temperature >= HOT_MK ? 1 : 0;
                outcome.hottestMk = std::max(outcome.hottestMk, cache.temperature);
            }

            const uint64_t farAmount = FarGrass(table, cells);
            if (setting.trace) {
                const uint32_t front = FAR_FIRST_X - setting.riverCells - 1;
                uint32_t frontMk = 0;
                uint32_t farMk = 0;
                for (uint32_t z = 0; z < PROBE_GRID_SIZE; ++z) {
                    frontMk = std::max(frontMk, caches[ProbeCellIndex(front, GRASS_Y, z)].temperature);
                    farMk = std::max(farMk, caches[ProbeCellIndex(FAR_FIRST_X, GRASS_Y, z)].temperature);
                }

                const ProbeRadiationReport& report = reference.LastRadiationReport();
                std::printf(
                    "  %s %3llu 秒: 火 %5u セル・前線の最高 %7u mK・向こう岸の最初の列の最高 %7u mK・放射で動いた %llu "
                    "mJ・吸うセル %u\n",
                    setting.text.c_str(), static_cast<unsigned long long>((tick + 1) / TICKS_PER_SECOND), hot, frontMk,
                    farMk, static_cast<unsigned long long>(report.movedMilliJoules), report.absorbingCells);
            }
            const double seconds = static_cast<double>(tick + 1) / TICKS_PER_SECOND;
            if (outcome.farIgnitionSeconds < 0 && farAmount * 100 < farStart * 99)
                outcome.farIgnitionSeconds = seconds;
            if (hot == 0 && tick > 2 * TICKS_PER_SECOND) {
                outcome.fireOutSeconds = seconds;
                ++tick;
                break;
            }
        }

        outcome.farLeftPercent = 100.0 * static_cast<double>(FarGrass(table, reference.State(tick))) /
                                 static_cast<double>(farStart == 0 ? 1 : farStart);
        outcome.riverWaterEnd = AmountInColumns(table, reference.State(tick), "water", riverStart, FAR_FIRST_X);
        outcome.vaporEnd = AmountInColumns(table, reference.State(tick), "water_vapor", 0, PROBE_GRID_SIZE);
        outcome.millisecondsPerTick = chr::duration<double, std::milli>(total).count() / static_cast<double>(tick);
        outcome.pairsPerTick = pairs / (tick == 0 ? 1 : tick);

        return outcome;
    }

}  // namespace

int main(int argc, char** argv) {
    const auto loaded = script::LoadReactionTable(
        {.packageRoot = fs::path(BICAMERAL_DATA_SCENES_DIR) / "grass_fire_river",
         .requireGame = false,
         .standardRoot = fs::path(BICAMERAL_DATA_STANDARD_DIR)});
    if (!loaded) {
        std::printf("場面を読めない: %s\n", loaded.error().c_str());

        return 1;
    }

    // 設定: 引数 → 無ければ runner/radiation_scene_settings.txt(git の外。1 行 1 回。ランナーの test から測る道)→ 無ければ短い煙の試し
    std::vector<Setting> settings;
    for (int i = 1; i < argc; ++i)
        settings.push_back(ParseSetting(argv[i]));
    if (settings.empty()) {
        std::ifstream file(fs::path(BICAMERAL_SOURCE_DIR) / "runner" / "radiation_scene_settings.txt");
        for (std::string line; std::getline(file, line);) {
            if (!line.empty() && line[0] != '#')
                settings.push_back(ParseSetting(line));
        }
    }
    if (settings.empty())
        settings.push_back(ParseSetting("w=4,ac=500000,ak=100000,sec=2"));

    std::vector<Outcome> outcomes(settings.size());
    {
        std::vector<std::jthread> threads;
        for (size_t i = 0; i < settings.size(); ++i)
            threads.emplace_back([&, i] { outcomes[i] = Run(loaded->table, settings[i]); });
    }

    for (size_t i = 0; i < settings.size(); ++i) {
        const Outcome& outcome = outcomes[i];
        std::printf(
            "%-48s 向こう岸に火 %6.0f 秒・向こう岸の草 %6.2f%%・火が消えた %6.0f 秒・最高 %7u mK・%7.1f ms/刻み・対 "
            "%llu/刻み・川の水 %.1f → %.1f mol・湯気 %.1f mol\n",
            settings[i].text.c_str(), outcome.farIgnitionSeconds, outcome.farLeftPercent, outcome.fireOutSeconds,
            outcome.hottestMk, outcome.millisecondsPerTick, static_cast<unsigned long long>(outcome.pairsPerTick),
            static_cast<double>(outcome.riverWaterStart) * 1e-6, static_cast<double>(outcome.riverWaterEnd) * 1e-6,
            static_cast<double>(outcome.vaporEnd) * 1e-6);
    }

    return 0;
}
