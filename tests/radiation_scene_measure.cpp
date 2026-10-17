// radiation_scene_measure.cpp — 草原の延焼の場面(data/scenes/grass_fire_river)を、仮の世界の CPU リファレンスで流して測る
// (放射の段 T-0585・ADR-0263。気体・液体の区画と草の燃える速さ・湿った岸 T-0697)。
// ctest の radiation_scene は短い煙の試し(2 秒)。測る時は runner/radiation_scene_settings.txt に設定を書いて
// job.py test -Filter radiation_scene -Show(GPU の順番待ちを通らない)か、job.py run -Exe radiation_scene_measure -- <設定> ...。
//
// 設定(1 つの引数 = 1 回。カンマ区切り。書かない鍵は既定):
//   w=<川の幅のセル>(既定 4 = 2 m。向こう岸 x 44〜63 は変えず、手前の岸を伸び縮みさせる)・gap=1(川の代わりに草の無い土の隙間)
//   ac=<セルロースの a>・ak=<炭(carbon)の a>・as=<砂の a>・aw=<水の a>(mm²/mol。既定 0)・rad=0(放射の段を入れない)
//   seed=<向きの種>・sec=<上限の秒>(既定 60)・tall=<草の層の数>・trace=1(1 秒ごとの経過)
//   gas=1(気体の区画: 川と両岸の上の空気 x 32〜51・y 4〜11・z 16〜47。T-0671 の場面の flow_regions と同じ。閉じた壁)・
//   gz=<奥行き>(気体の区画の z の奥行き。既定 32。z = 32 を中に縮める。CPU の気体の段が重いので)・gh=<高さ>(既定 8)・
//   gxb・gyb・gzb=<0 壁 / 1 周期 / 2 開いた>(区画の外の面。軸ごと。既定は全部壁)
//   liq=1(液体の区画: 川の列・y 1〜5。gap では入れない)
//   slow=<s>(草〔セルロース〕の 2 本の規則の速さを 1/2^s に。今の値は 1 セルが 1 秒足らずで燃え尽きる。T-0570 の文献の値の代わりの試し)
//   wet=<µmol>(川の両岸 4 列〔2 m〕の草のセルに液体の水を足す。湿った岸の試し。川から水が染みる道は無い)
// 出力(1 行 1 回): 向こう岸に火が移った時刻(向こう岸の草が 1% 減った)・向こう岸に残った草・火が消えた時刻・1 刻みの時間(放射の段の分も)・
//   川の水(液体の water)の始めと終わり・終わりの湯気(water_vapor。燃焼の生成物も入る)。川の水が減った分が蒸発(潜熱を吸う。T-0611)。
//   T-0697 で足した欄: 手前の草の 9 割が燃えた時刻(川に着いた目安)・前線(川の手前の列)と向こう岸の最初の列の最高温度・
//   前線の列が火(1000 K 以上)だった秒数・川の水面(y 4)の最高温度・川の上(y 5 以上)の湯気の最大・y 8(4 m)以上の CO2 の最大(煙が上がった)・
//   気体を流さなかった刻み(成分がインラインの数を超えた)。
// 設定はスレッドで並べて走らせる(それぞれ独立の世界。結果は決定的)。
#include <algorithm>
#include <array>
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
        bool gas = false;          // 気体の区画を載せる(T-0697)
        bool liquid = false;       // 液体の区画(川)を載せる
        uint32_t slowShift = 0;    // 草の規則の速さを 1/2^slowShift に
        uint64_t wetWater = 0;     // 岸の草のセルに足す液体の水(µmol)
        uint32_t gasDepth = 32;    // 気体の区画の z の奥行き(z = 32 を中に。CPU の気体の段は 50〜70 µs/セル/刻み)
        uint32_t gasHeight = 8;    // 気体の区画の高さ(y 4 から)
        std::array<GasBoundary, 3> gasBoundary = {GasBoundary::Wall, GasBoundary::Wall, GasBoundary::Wall};
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

        // --- T-0697 ---
        double nearBurnedSeconds = -1;    // 手前の草の 9 割が燃えた
        uint32_t frontPeakMk = 0;         // 川の手前の列(y 4)の最高
        uint32_t frontHotSeconds = 0;     // その列に 1000 K 以上のセルがあった秒数
        uint32_t farPeakMk = 0;           // 向こう岸の最初の列(y 4)の最高
        uint32_t riverPeakMk = 0;         // 川の水面(y 4)の最高
        uint64_t vaporOverRiverPeak = 0;  // µmol。川の列の y 5 以上の湯気の最大
        uint64_t smokeHighPeak = 0;       // µmol。y 8 以上の CO2 の最大
        uint64_t gasSkippedTicks = 0;     // 気体を流さなかった刻み
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
                else if (key == "gas")
                    setting.gas = value != 0;
                else if (key == "liq")
                    setting.liquid = value != 0;
                else if (key == "slow")
                    setting.slowShift = static_cast<uint32_t>(value);
                else if (key == "wet")
                    setting.wetWater = value;
                else if (key == "gh")
                    setting.gasHeight = static_cast<uint32_t>(
                        std::clamp<uint64_t>(value, 1, PROBE_GRID_SIZE - GRASS_Y));
                else if (key == "gxb" || key == "gyb" || key == "gzb")
                    setting.gasBoundary[key[1] - 'x'] = static_cast<GasBoundary>(std::min<uint64_t>(value, 2));
                else if (key == "gz")
                    setting.gasDepth = static_cast<uint32_t>(std::clamp<uint64_t>(value, 1, PROBE_GRID_SIZE));
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

    // 名前に cellulose を含む規則(熱分解・燃焼)の速さを 1/2^shift に(速度の表の値 = 仮数 × 2^指数 の指数を下げる)
    void SlowGrass(BakedReactionTable& table, uint32_t shift) {
        if (shift == 0 || table.rules.empty())
            return;

        const size_t kelvins = table.rates.size() / table.rules.size();
        for (size_t rule = 0; rule < table.ruleNames.size(); ++rule) {
            if (table.ruleNames[rule].find("cellulose") == std::string::npos)
                continue;

            for (size_t k = 0; k < kelvins; ++k) {
                uint64_t& rate = table.rates[(rule * kelvins) + k];
                const int32_t exponent = static_cast<int32_t>(static_cast<uint32_t>(rate & 0xffffffffULL));
                rate = (rate & ~0xffffffffULL) | static_cast<uint32_t>(exponent - static_cast<int32_t>(shift));
            }
        }
    }

    // セルに物質 name を amount µmol 足す(温度 temperatureMk でエネルギーを作り直す)
    void AddToCell(const BakedReactionTable& table, reaction::RxCell& cell, int32_t temperatureMk,
                   std::string_view name, uint64_t amount) {
        const uint32_t added = table.SpeciesId(name);
        std::vector<SpeciesAmount> amounts;
        bool found = false;
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            const bool same = cell.species[i] == added;
            found = found || same;
            amounts.push_back({.species = cell.species[i], .amount = cell.amounts[i] + (same ? amount : 0)});
        }
        if (!found)
            amounts.push_back({.species = added, .amount = amount});

        std::ranges::sort(amounts, {}, &SpeciesAmount::species);
        cell = MakeReactionCell(table, amounts, temperatureMk);
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

        // 湿った岸: 川の両岸 4 列の草のセル(草の層の全部)に液体の水
        if (setting.wetWater != 0 && !setting.gap) {
            constexpr int32_t SCENE_TEMPERATURE_MK = 300000;  // 場面の初めの温度(world_cells.luau)
            constexpr uint32_t WET_COLUMNS = 4;
            for (uint32_t x = riverStart - WET_COLUMNS; x < FAR_FIRST_X + WET_COLUMNS; ++x) {
                if (x >= riverStart && x < FAR_FIRST_X)
                    continue;

                for (uint32_t layer = 0; layer < setting.grassLayers; ++layer) {
                    for (uint32_t z = 0; z < PROBE_GRID_SIZE; ++z)
                        AddToCell(table, world[ProbeCellIndex(x, GRASS_Y + layer, z)], SCENE_TEMPERATURE_MK, "water",
                                  setting.wetWater);
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

    // 列 x の範囲 [firstX, endX)・y の範囲 [firstY, endY) にある物質 name の合計(µmol。全部の z)
    uint64_t AmountInColumns(const BakedReactionTable& table, std::span<const reaction::RxCell> cells,
                             std::string_view name, uint32_t firstX, uint32_t endX, uint32_t firstY = 0,
                             uint32_t endY = PROBE_GRID_SIZE) {
        uint32_t wanted = UINT32_MAX;
        for (size_t id = 0; id < table.speciesNames.size(); ++id) {
            if (table.speciesNames[id] == name)
                wanted = static_cast<uint32_t>(id);
        }

        uint64_t amount = 0;
        for (uint32_t z = 0; z < PROBE_GRID_SIZE; ++z) {
            for (uint32_t y = firstY; y < endY; ++y) {
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

    // 1 秒ごとに見るもの(T-0697): 手前の草・前線と向こう岸と川の水面の温度・川の上の湯気・高い所の煙
    void Observe(const BakedReactionTable& table, const Setting& setting, std::span<const reaction::RxCell> cells,
                 std::span<const reaction::HcThermalCache> caches, uint64_t ticks, uint64_t nearStart,
                 Outcome& outcome) {
        const uint32_t riverFirst = FAR_FIRST_X - setting.riverCells;
        const uint32_t front = riverFirst - 1;
        uint32_t frontMk = 0;
        for (uint32_t z = 0; z < PROBE_GRID_SIZE; ++z) {
            frontMk = std::max(frontMk, caches[ProbeCellIndex(front, GRASS_Y, z)].temperature);
            outcome.farPeakMk = std::max(outcome.farPeakMk,
                                         caches[ProbeCellIndex(FAR_FIRST_X, GRASS_Y, z)].temperature);
            for (uint32_t x = riverFirst; x < FAR_FIRST_X; ++x)
                outcome.riverPeakMk = std::max(outcome.riverPeakMk, caches[ProbeCellIndex(x, GRASS_Y, z)].temperature);
        }

        outcome.frontPeakMk = std::max(outcome.frontPeakMk, frontMk);
        outcome.frontHotSeconds += frontMk >= HOT_MK ? 1 : 0;

        const uint64_t nearAmount = AmountInColumns(table, cells, "cellulose", 0, riverFirst, GRASS_Y, GRASS_Y + 1);
        if (outcome.nearBurnedSeconds < 0 && nearAmount * 10 < nearStart)
            outcome.nearBurnedSeconds = static_cast<double>(ticks) / TICKS_PER_SECOND;

        const uint64_t vaporOverRiver = AmountInColumns(table, cells, "water_vapor", riverFirst, FAR_FIRST_X,
                                                        GRASS_Y + 1);
        outcome.vaporOverRiverPeak = std::max(outcome.vaporOverRiverPeak, vaporOverRiver);
        const uint64_t smokeHigh = AmountInColumns(table, cells, "carbon_dioxide", 0, PROBE_GRID_SIZE, GRASS_Y + 4);
        outcome.smokeHighPeak = std::max(outcome.smokeHighPeak, smokeHigh);
    }

    Outcome Run(const BakedReactionTable& baseTable, const Setting& setting) {
        BakedReactionTable table = baseTable;
        SetAbsorption(table, "cellulose", setting.absorptionCellulose);
        SetAbsorption(table, "carbon", setting.absorptionCarbon);
        SetAbsorption(table, "silica", setting.absorptionSilica);
        SetAbsorption(table, "water", setting.absorptionWater);
        SlowGrass(table, setting.slowShift);

        const std::vector<reaction::RxCell> world = MakeWorld(table, setting);
        ProbeReference reference(table, world);
        if (setting.radiation)
            reference.EnableRadiation({.seed = setting.seed});

        // 区画(T-0697。T-0671 の場面の flow_regions と同じ形。気体は閉じた壁)
        const uint32_t riverFirst = FAR_FIRST_X - setting.riverCells;
        if (setting.gas) {
            const auto enabled = reference.EnableGas({.minCell = {32, GRASS_Y, 32 - (setting.gasDepth / 2)},
                                                      .sizeCells = {20, setting.gasHeight, setting.gasDepth},
                                                      .boundary = setting.gasBoundary,
                                                      .randomSeed = 7});
            if (!enabled) {
                std::printf("気体の区画を載せられない: %s\n", enabled.error().c_str());
                std::exit(1);
            }
        }
        if (setting.liquid && !setting.gap) {
            const auto enabled = reference.EnableLiquid(
                {.minCell = {riverFirst, 1, 0}, .sizeCells = {setting.riverCells, 5, 64}});
            if (!enabled) {
                std::printf("液体の区画を載せられない: %s\n", enabled.error().c_str());
                std::exit(1);
            }
        }

        const uint64_t farStart = FarGrass(table, reference.State(0));
        const uint64_t nearStart = AmountInColumns(table, reference.State(0), "cellulose", 0, riverFirst, GRASS_Y,
                                                   GRASS_Y + 1);
        const std::vector<ProbeCommand> ignite = {MakePokeCommand(0, 0, 4, GRASS_Y, 32)};
        const uint32_t riverStart = riverFirst;
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
            if (setting.gas && reference.LastGasReport().skippedCellOverflow)
                ++outcome.gasSkippedTicks;

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
            Observe(table, setting, cells, caches, tick + 1, nearStart, outcome);
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
            "%llu/刻み・川の水 %.1f → %.1f mol・湯気 %.1f mol\n"
            "    手前の草 9 割 %5.0f 秒・前線 最高 %7u mK 火 %3u 秒・向こう岸の最初の列 最高 %7u mK・川の水面 最高 %7u "
            "mK・"
            "川の上の湯気 最大 %.2f mol・4 m 以上の CO2 最大 %.2f mol・気体を流さなかった刻み %llu\n",
            settings[i].text.c_str(), outcome.farIgnitionSeconds, outcome.farLeftPercent, outcome.fireOutSeconds,
            outcome.hottestMk, outcome.millisecondsPerTick, static_cast<unsigned long long>(outcome.pairsPerTick),
            static_cast<double>(outcome.riverWaterStart) * 1e-6, static_cast<double>(outcome.riverWaterEnd) * 1e-6,
            static_cast<double>(outcome.vaporEnd) * 1e-6, outcome.nearBurnedSeconds, outcome.frontPeakMk,
            outcome.frontHotSeconds, outcome.farPeakMk, outcome.riverPeakMk,
            static_cast<double>(outcome.vaporOverRiverPeak) * 1e-6, static_cast<double>(outcome.smokeHighPeak) * 1e-6,
            static_cast<unsigned long long>(outcome.gasSkippedTicks));
    }

    return 0;
}
