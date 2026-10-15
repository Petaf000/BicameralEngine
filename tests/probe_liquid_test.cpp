// probe_liquid_test.cpp — 仮の世界に液体を載せる CPU リファレンス(T-0609・ADR-0266。sim/probe_liquid)の試験。
//
// 確かめること:
//   1. 池に溜まる: 台の上に置いた水が低い方へ流れ落ち、くぼみ(池)に溜まって水面がそろう。堤の向こうへは勢いの跳ねだけ(0.1% 未満。T-0610)
//   2. 池を抜く: 堤(石のセル)を空気に置き換えると、池の水が低い土地へ流れ出る
//   3. 保存: 毎刻み、物質ごとの量・エネルギーの合計・面の運動量 − 帳簿(軸ごと)がビット単位で一定
//   4. 静かな池: 平らな水は 1 刻みも動かない(圧力の差と重さが整数で厳密に打ち消し合う)
//   5. 決定性: 2 回の実行で、セルと面の運動量・帳簿のハッシュが一致
//   6. 液体の無い世界は何も変わらない・液体に密度が無い表はベイクが断る・密度からモル体積
//   7. 世界の刻み(ProbeReference の (1c))に載せても、台の水が池へ流れ、水の量とエネルギーが格子の全体で保存される
//   8. ダムを壊す(T-0610): 運動量の移流で水が勢いで押し寄せ、向こうの壁を駆け上がって跳ね返る(移流なしと比べる)。
//      エネルギー(セル + 位置 + 運動 − 帳簿)が毎刻みビット単位・落ちた水が温まる・1 刻みの費用
// 表は試験の表(combustion_test)に、液体の水(liquid_water。phase = "liquid"・密度 1000)・石(stone。固体)・地球の環境の値を足したもの。
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "reaction_test_table.h"
#include "sim/probe_liquid.h"
#include "sim/probe_sim.h"
#include "standard_world_physics.h"

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

    // --- 区画(16 × 10 × 3 セル = 8 m × 5 m × 1.5 m。y が上)と地形(区画の中の x で)---
    //   x 0〜3: 台(石 y 0〜6。水を y 7〜8 に置く) / x 4〜9: 池(石 y 0〜3。池の底は y 4)/ x 10: 堤(石 y 0〜6)/
    //   x 11〜15: 低い土地(石 y 0)。池に入る水(24 セル)は堤の高さ(池の 3 層 = 54 セル)より少なく、低い土地(y 1〜3 の 54 セル)は池より低い
    constexpr std::array<uint32_t, 3> REGION_MIN = {24, 0, 30};
    constexpr std::array<uint32_t, 3> REGION_SIZE = {16, 10, 3};
    constexpr uint32_t BASIN_BEGIN = 4;
    constexpr uint32_t DAM_X = 10;

    constexpr uint64_t FULL_WATER = 6900000000;     // µmol(0.5 m 角の水 ≒ 6939 mol の少し下)
    constexpr uint64_t PARTIAL_WATER = 2000000000;  // µmol(世界の刻みの試験。セルの量の上限 T-0558 の前の値)
    constexpr uint64_t NITROGEN = 4000000;          // µmol(0.125 m³ の空気・300 K・1 気圧)
    constexpr uint64_t OXYGEN = 1000000;
    constexpr uint64_t STONE = 20000000;  // µmol
    constexpr int32_t TEMPERATURE = 300000;

    constexpr uint64_t SETTLE_TICKS = 1800;  // 30 秒(T-0610 で摩擦を弱めて 15 → 30 秒)
    constexpr uint64_t SETTLE_CHUNK = 300;
    constexpr uint64_t DRAIN_TICKS = 1800;  // 30 秒

    enum class Kind : uint8_t { Air, Water, Stone };

    BakedReactionTable MakeTable(bool withDensity = true) {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        definition.environment = StandardWorldPhysics();
        definition.species.push_back({.name = "liquid_water",
                                      .composition = {{.element = "H", .count = 2}, {.element = "O", .count = 1}},
                                      .formationEnthalpy = -285830,
                                      .heatCapacity = 75300,
                                      .thermalConductivity = 600,
                                      .liquid = true,
                                      .density = withDensity ? 1000u : 0u});
        definition.species.push_back({.name = "stone",
                                      .composition = {{.element = "C", .count = 1}},
                                      .formationEnthalpy = 0,
                                      .heatCapacity = 8500,
                                      .thermalConductivity = 2000,
                                      .condensed = true});

        auto baked = BakeReactionTable(definition);
        if (!baked) {
            std::printf("表を作れない: %s\n", baked.error().c_str());
            std::exit(1);
        }

        return std::move(*baked);
    }

    Kind TerrainKind(uint32_t x, uint32_t y, bool damOpen) {
        if (y == 0)
            return Kind::Stone;

        if (x < BASIN_BEGIN)
            return y <= 6 ? Kind::Stone : (y <= 8 ? Kind::Water : Kind::Air);

        if (x < DAM_X)
            return y <= 3 ? Kind::Stone : Kind::Air;

        if (x == DAM_X && !damOpen)
            return y <= 6 ? Kind::Stone : Kind::Air;

        return Kind::Air;
    }

    reaction::RxCell MakeCell(const BakedReactionTable& table, Kind kind, uint64_t water) {
        std::vector<SpeciesAmount> amounts = {{.species = table.SpeciesId("nitrogen"), .amount = NITROGEN},
                                              {.species = table.SpeciesId("oxygen"), .amount = OXYGEN}};
        if (kind == Kind::Water)
            amounts.push_back({.species = table.SpeciesId("liquid_water"), .amount = water});

        if (kind == Kind::Stone)
            amounts = {{.species = table.SpeciesId("stone"), .amount = STONE}};

        std::ranges::sort(amounts, {}, &SpeciesAmount::species);

        return MakeReactionCell(table, amounts, TEMPERATURE);
    }

    std::array<uint32_t, 3> RegionCoordinates(uint32_t region) {
        const uint32_t x = region % REGION_SIZE[0];
        const uint32_t z = (region / REGION_SIZE[0]) % REGION_SIZE[2];
        const uint32_t y = region / (REGION_SIZE[0] * REGION_SIZE[2]);

        return {x, y, z};
    }

    std::vector<reaction::RxCell> MakeWorld(const BakedReactionTable& table, const ProbeLiquidField& field,
                                            uint64_t water) {
        std::vector<reaction::RxCell> cells = MakeBlankProbeWorld();
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const auto at = RegionCoordinates(region);
            cells[field.GridIndex(region)] = MakeCell(table, TerrainKind(at[0], at[1], false), water);
        }

        return cells;
    }

    ProbeLiquidField MakeField(const BakedReactionTable& table) {
        auto field = ProbeLiquidField::Create(table, {.minCell = REGION_MIN, .sizeCells = REGION_SIZE});
        if (!field) {
            std::printf("区画を作れない: %s\n", field.error().c_str());
            std::exit(1);
        }

        return std::move(*field);
    }

    // --- 保存の検査(区画のセルの物質ごとの量・エネルギー・面の運動量 − 帳簿)---
    struct Totals {
        std::vector<uint64_t> amounts;
        int64_t energy = 0;  // mJ。セル + 位置 + 運動 − 帳簿(T-0610)
        std::array<int64_t, 3> momentum = {};

        bool operator==(const Totals&) const = default;
    };

    Totals MeasureTotals(const BakedReactionTable& table, const ProbeLiquidField& field,
                         std::span<const reaction::RxCell> cells) {
        Totals totals;
        totals.amounts.assign(table.species.size(), 0);
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const reaction::RxCell& cell = cells[field.GridIndex(region)];
            totals.energy += cell.energy;
            for (uint32_t i = 0; i < cell.speciesCount; ++i)
                totals.amounts[cell.species[i]] += cell.amounts[i];
        }

        totals.energy += field.PotentialEnergySum() + field.KineticEnergySum() - field.Ledger().energy;
        for (uint32_t axis = 0; axis < 3; ++axis)
            totals.momentum[axis] = field.MomentumSum(axis) - field.Ledger().external[axis];

        return totals;
    }

    // 区画の x の範囲 [begin, end) の液体の水の量(µmol)
    uint64_t WaterIn(const BakedReactionTable& table, const ProbeLiquidField& field,
                     std::span<const reaction::RxCell> cells, uint32_t begin, uint32_t end) {
        const uint32_t water = table.SpeciesId("liquid_water");
        uint64_t total = 0;
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const auto at = RegionCoordinates(region);
            if (at[0] < begin || at[0] >= end)
                continue;

            const reaction::RxCell& cell = cells[field.GridIndex(region)];
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                if (cell.species[i] == water)
                    total += cell.amounts[i];
            }
        }

        return total;
    }

    uint64_t CellsHash(const ProbeLiquidField& field, std::span<const reaction::RxCell> cells) {
        uint64_t hash = 14695981039346656037ull;
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const reaction::RxCell& cell = cells[field.GridIndex(region)];
            hash = (hash ^ (uint64_t)cell.energy) * 1099511628211ull;
            for (uint32_t i = 0; i < cell.speciesCount; ++i)
                hash = (hash ^ (cell.amounts[i] + cell.species[i])) * 1099511628211ull;
        }

        return hash;
    }

    // 走らせた間の合計(T-0610)
    struct RunTotals {
        int64_t dropped = 0;  // 行き場なし(軸の絶対値の和)
        int64_t capped = 0;   // 速さの上限
        uint64_t cappedFaces = 0;
        int64_t heat = 0;  // mJ
    };

    void Accumulate(RunTotals* totals, const ProbeLiquidStepReport& report) {
        if (totals == nullptr)
            return;

        for (uint32_t axis = 0; axis < 3; ++axis) {
            totals->dropped += report.dropped[axis] < 0 ? -report.dropped[axis] : report.dropped[axis];
            totals->capped += report.capped[axis] < 0 ? -report.capped[axis] : report.capped[axis];
        }

        totals->cappedFaces += report.cappedFaces;
        totals->heat += report.heat;
    }

    // 区画のセルのエネルギーの合計(mJ。熱 + 化学)
    int64_t CellEnergy(const ProbeLiquidField& field, std::span<const reaction::RxCell> cells) {
        int64_t total = 0;
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region)
            total += cells[field.GridIndex(region)].energy;

        return total;
    }

    // ticks 刻み進め、毎刻み保存を確かめる。最後の刻みの報告を返す
    ProbeLiquidStepReport Run(const BakedReactionTable& table, ProbeLiquidField& field,
                              std::vector<reaction::RxCell>& cells, uint64_t ticks, bool checkConservation,
                              RunTotals* runTotals = nullptr) {
        const Totals before = MeasureTotals(table, field, cells);
        ProbeLiquidStepReport report;
        bool conserved = true;
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            std::vector<uint32_t> changed;
            report = field.Step(table, cells, changed);
            Accumulate(runTotals, report);
            if (checkConservation && conserved && !(MeasureTotals(table, field, cells) == before)) {
                std::printf("  保存が崩れた刻み %llu\n", (unsigned long long)tick);
                conserved = false;
            }
        }

        EXPECT(conserved);

        return report;
    }

    // --- 1〜3: 池に溜まる・池を抜く・保存 ---
    void TestPoolAndDrain() {
        std::printf("池に溜まる・池を抜く\n");
        const BakedReactionTable table = MakeTable();
        ProbeLiquidField field = MakeField(table);
        std::vector<reaction::RxCell> cells = MakeWorld(table, field, FULL_WATER);
        const uint64_t total = WaterIn(table, field, cells, 0, REGION_SIZE[0]);

        const int64_t energyBefore = CellEnergy(field, cells);
        RunTotals settleTotals;
        ProbeLiquidStepReport settled;
        for (uint64_t done = SETTLE_CHUNK; done <= SETTLE_TICKS; done += SETTLE_CHUNK) {
            settled = Run(table, field, cells, SETTLE_CHUNK, true, &settleTotals);
            std::printf("  %llu 秒: 運動のエネルギー %lld mJ・最後の刻みに動いた %llu mg・堤の向こう %llu µmol\n",
                        (unsigned long long)(done / 60), (long long)field.KineticEnergySum(),
                        (unsigned long long)settled.movedMilligrams,
                        (unsigned long long)WaterIn(table, field, cells, DAM_X, REGION_SIZE[0]));
        }

        const int64_t heated = CellEnergy(field, cells) - energyBefore;
        std::printf(
            "  30 秒: 熱になった %lld mJ(落ちた位置のエネルギー %lld mJ・残りの運動 %lld mJ)・行き場なし %lld・"
            "速さの上限 %lld(%llu 面)\n",
            (long long)heated, (long long)(field.Ledger().energy - field.PotentialEnergySum()),
            (long long)field.KineticEnergySum(), (long long)settleTotals.dropped, (long long)settleTotals.capped,
            (unsigned long long)settleTotals.cappedFaces);
        EXPECT(heated > 0);
        EXPECT(heated == field.Ledger().energy - field.PotentialEnergySum() - field.KineticEnergySum());
        const uint64_t plateau = WaterIn(table, field, cells, 0, BASIN_BEGIN);
        const uint64_t basin = WaterIn(table, field, cells, BASIN_BEGIN, DAM_X);
        const uint64_t beyond = WaterIn(table, field, cells, DAM_X, REGION_SIZE[0]);
        std::printf(
            "  30 秒: 台 %.4f・池 %.4f・堤の向こう %llu µmol(全体 %llu)・最後の刻みに動いた %llu mg・縮めたセル %u\n",
            (double)plateau / (double)total, (double)basin / (double)total, (unsigned long long)beyond,
            (unsigned long long)total, (unsigned long long)settled.movedMilligrams, settled.limitedCells);
        EXPECT(WaterIn(table, field, cells, 0, REGION_SIZE[0]) == total);
        EXPECT(basin * 100 >= total * 95);
        // 移流(T-0610)で、台から池に落ちる水の勢いが堤を駆け上がり、最初の 10 秒にごく少し(2026-10-15 は全体の 0.027%)が越える(跳ね)
        EXPECT(beyond * 1000 <= total);

        // 水面がそろう: 池の列(x・z)ごとの水の量の差が平均の 2% 以内
        const uint32_t water = table.SpeciesId("liquid_water");
        std::vector<uint64_t> columns(REGION_SIZE[0] * REGION_SIZE[2], 0);
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const auto at = RegionCoordinates(region);
            const reaction::RxCell& cell = cells[field.GridIndex(region)];
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                if (cell.species[i] == water)
                    columns[at[0] + (REGION_SIZE[0] * at[2])] += cell.amounts[i];
            }
        }

        uint64_t lowest = UINT64_MAX;
        uint64_t highest = 0;
        for (uint32_t z = 0; z < REGION_SIZE[2]; ++z) {
            for (uint32_t x = BASIN_BEGIN; x < DAM_X; ++x) {
                lowest = std::min(lowest, columns[x + (REGION_SIZE[0] * z)]);
                highest = std::max(highest, columns[x + (REGION_SIZE[0] * z)]);
            }
        }

        std::printf("  池の列の水: 最小 %llu・最大 %llu µmol\n", (unsigned long long)lowest,
                    (unsigned long long)highest);
        EXPECT((highest - lowest) * 50 <= highest);

        // 池を抜く: 堤を空気に置き換える(面の運動量はそのまま)
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const auto at = RegionCoordinates(region);
            if (at[0] == DAM_X && at[1] >= 1 && at[1] <= 6)
                cells[field.GridIndex(region)] = MakeCell(table, Kind::Air, 0);
        }

        const ProbeLiquidStepReport drained = Run(table, field, cells, DRAIN_TICKS, true);
        const uint64_t basinAfter = WaterIn(table, field, cells, BASIN_BEGIN, DAM_X);
        const uint64_t lowland = WaterIn(table, field, cells, DAM_X, REGION_SIZE[0]);
        std::printf("  堤を消して 30 秒: 池 %.4f・低い土地 %.4f\n", (double)basinAfter / (double)total,
                    (double)lowland / (double)total);
        EXPECT(WaterIn(table, field, cells, 0, REGION_SIZE[0]) == total);
        EXPECT(basinAfter * 100 <= total * 5);
        EXPECT(lowland * 100 >= total * 90);

        std::printf(
            "  最後の刻みの外からの運動量(y): 壁 %lld・重さ %lld・摩擦 %lld・行き場なし %lld・帳簿の合計 %lld\n",
            (long long)drained.wall[1], (long long)drained.gravity[1], (long long)drained.friction[1],
            (long long)drained.dropped[1], (long long)field.Ledger().external[1]);
    }

    // --- 4: 静かな池(平らな 1 層の水)は動かない ---
    void TestStillPool() {
        std::printf("静かな池\n");
        const BakedReactionTable table = MakeTable();
        ProbeLiquidField field = MakeField(table);
        std::vector<reaction::RxCell> cells = MakeBlankProbeWorld();
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const auto at = RegionCoordinates(region);
            Kind kind = TerrainKind(at[0], at[1], false);
            if (at[0] < BASIN_BEGIN && kind == Kind::Water)
                kind = Kind::Air;

            if (at[0] >= BASIN_BEGIN && at[0] < DAM_X && at[1] == 4)
                kind = Kind::Water;

            cells[field.GridIndex(region)] = MakeCell(table, kind, FULL_WATER);
        }

        const std::vector<reaction::RxCell> before = cells;
        const ProbeLiquidStepReport report = Run(table, field, cells, 60, true);
        EXPECT(report.movedMilligrams == 0);
        EXPECT(CellsHash(field, cells) == CellsHash(field, before));
        for (uint32_t axis = 0; axis < 3; ++axis)
            EXPECT(field.MomentumSum(axis) == 0);
    }

    // --- 5: 決定性 ---
    void TestDeterminism() {
        std::printf("決定性\n");
        const BakedReactionTable table = MakeTable();
        std::array<uint64_t, 2> cellHashes = {};
        std::array<uint64_t, 2> fieldHashes = {};
        for (uint32_t run = 0; run < 2; ++run) {
            ProbeLiquidField field = MakeField(table);
            std::vector<reaction::RxCell> cells = MakeWorld(table, field, FULL_WATER);
            Run(table, field, cells, 300, false);
            cellHashes[run] = CellsHash(field, cells);
            fieldHashes[run] = field.Hash();
        }

        EXPECT(cellHashes[0] == cellHashes[1]);
        EXPECT(fieldHashes[0] == fieldHashes[1]);
    }

    // --- 6: 液体の無い世界・密度の無い液体・モル体積 ---
    void TestBlankAndBake() {
        std::printf("液体の無い世界・ベイク\n");
        const BakedReactionTable table = MakeTable();
        EXPECT(table.speciesMolarVolume[table.SpeciesId("liquid_water")] == 18015);
        EXPECT(table.speciesLiquid[table.SpeciesId("liquid_water")] == 1);
        EXPECT(table.speciesCondensed[table.SpeciesId("liquid_water")] == 1);
        EXPECT(table.speciesLiquid[table.SpeciesId("stone")] == 0);

        ReactionTableDefinition definition = MakeCombustionTestTable();
        definition.species.push_back({.name = "liquid_water",
                                      .composition = {{.element = "H", .count = 2}, {.element = "O", .count = 1}},
                                      .formationEnthalpy = -285830,
                                      .heatCapacity = 75300,
                                      .liquid = true});
        EXPECT(!BakeReactionTable(definition).has_value());

        ProbeLiquidField field = MakeField(table);
        std::vector<reaction::RxCell> cells = MakeBlankProbeWorld();
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region)
            cells[field.GridIndex(region)] = MakeCell(table, Kind::Air, 0);

        const std::vector<reaction::RxCell> before = cells;
        const ProbeLiquidStepReport report = Run(table, field, cells, 10, true);
        EXPECT(report.wetCells == 0);
        EXPECT(CellsHash(field, cells) == CellsHash(field, before));

        // 重力が 0 の表(ブランクの環境)は区画を断る
        ReactionTableDefinition blank = MakeCombustionTestTable();
        blank.environment = {};
        if (const auto baked = BakeReactionTable(blank))
            EXPECT(!ProbeLiquidField::Create(*baked, {.minCell = REGION_MIN, .sizeCells = REGION_SIZE}).has_value());
    }

    // --- 7: 世界の刻み(ProbeReference の (1c))---
    void TestProbeReference() {
        std::printf("世界の刻み\n");
        const BakedReactionTable table = MakeTable();
        const ProbeLiquidField shape = MakeField(table);
        const std::vector<reaction::RxCell> world = MakeWorld(table, shape, PARTIAL_WATER);
        ProbeReference reference(table, world);
        EXPECT(reference.EnableLiquid({.minCell = REGION_MIN, .sizeCells = REGION_SIZE}).has_value());

        const uint32_t water = table.SpeciesId("liquid_water");
        auto measure = [&](uint64_t tick) {
            std::pair<uint64_t, int64_t> totals = {0, 0};
            for (const reaction::RxCell& cell : reference.State(tick)) {
                totals.second += cell.energy;
                for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                    if (cell.species[i] == water)
                        totals.first += cell.amounts[i];
                }
            }

            return totals;
        };

        const auto before = measure(0);
        const uint64_t plateauBefore = WaterIn(table, shape, reference.State(0), 0, BASIN_BEGIN);
        constexpr uint64_t TICKS = 90;
        for (uint64_t tick = 0; tick < TICKS; ++tick)
            reference.Advance(tick, {});

        const uint64_t plateauAfter = WaterIn(table, shape, reference.State(TICKS), 0, BASIN_BEGIN);
        const uint64_t basinAfter = WaterIn(table, shape, reference.State(TICKS), BASIN_BEGIN, DAM_X);
        std::printf("  1.5 秒: 台 %llu → %llu・池 %llu µmol\n", (unsigned long long)plateauBefore,
                    (unsigned long long)plateauAfter, (unsigned long long)basinAfter);
        const ProbeLiquidField* liquid = reference.LiquidField();
        auto after = measure(TICKS);
        after.second += liquid->PotentialEnergySum() + liquid->KineticEnergySum() - liquid->Ledger().energy;
        std::printf("  熱になった %lld mJ\n", (long long)(measure(TICKS).second - before.second));
        EXPECT(after == before);
        EXPECT(plateauAfter < plateauBefore);
        EXPECT(basinAfter > 0);
    }

    // --- 8: ダムを壊す(T-0610)---
    //   区画 24 × 8 × 1(12 m × 4 m × 0.5 m。y が上)。床 y 0 は石。水は x 0〜5・y 1〜4(3 m × 2 m)。堤は t = 0 に消えたものとして置かない。
    //   向こうの壁は区画の端(x 23 の + 側)。落ち着いた水位は 24 セル ÷ 24 列 = 1 セル(0.5 m)
    constexpr std::array<uint32_t, 3> DAM_REGION_MIN = {8, 0, 8};
    constexpr std::array<uint32_t, 3> DAM_REGION_SIZE = {24, 8, 1};
    constexpr uint32_t DAM_WATER_END = 6;
    constexpr uint32_t DAM_WATER_TOP = 4;
    constexpr uint64_t DAM_TICKS = 360;  // 6 秒

    struct DamResult {
        uint64_t arrivalTick = 0;       // 向こうの壁の列に水が届いた刻み(全体の 1% を超えた)
        uint64_t peakTick = 0;          // 壁の列の水が最も多かった刻み
        uint64_t peakLevelPercent = 0;  // その時の壁の列の水(満ちたセル何個分 × 100)
        int64_t peakMomentum = 0;       // x の運動量の和の最大
        int64_t reboundMomentum = 0;    // 最大の後の x の運動量の和の最小(負なら跳ね返った)
        uint64_t finalLevelPercent = 0;
        RunTotals totals;
        int64_t nanosecondsPerTick = 0;
        uint64_t hash = 0;
    };

    DamResult RunDamBreak(const BakedReactionTable& table, const ProbeLiquidOptions& options, bool checkConservation) {
        auto created = ProbeLiquidField::Create(table, options);
        EXPECT(created.has_value());
        ProbeLiquidField field = std::move(*created);
        std::vector<reaction::RxCell> cells = MakeBlankProbeWorld();
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const uint32_t x = region % DAM_REGION_SIZE[0];
            const uint32_t y = region / DAM_REGION_SIZE[0];
            Kind kind = y == 0 ? Kind::Stone : Kind::Air;
            if (y >= 1 && y <= DAM_WATER_TOP && x < DAM_WATER_END)
                kind = Kind::Water;

            cells[field.GridIndex(region)] = MakeCell(table, kind, FULL_WATER);
        }

        const uint32_t water = table.SpeciesId("liquid_water");
        const auto wallColumn = [&] {
            uint64_t total = 0;
            for (uint32_t y = 0; y < DAM_REGION_SIZE[1]; ++y) {
                const reaction::RxCell&
                    cell = cells[field.GridIndex(DAM_REGION_SIZE[0] - 1 + (DAM_REGION_SIZE[0] * y))];
                for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                    if (cell.species[i] == water)
                        total += cell.amounts[i];
                }
            }

            return total * 100 / FULL_WATER;
        };

        DamResult result;
        const Totals before = MeasureTotals(table, field, cells);
        bool conserved = true;
        int64_t elapsed = 0;
        for (uint64_t tick = 1; tick <= DAM_TICKS; ++tick) {
            std::vector<uint32_t> changed;
            const auto start = std::chrono::steady_clock::now();
            const ProbeLiquidStepReport report = field.Step(table, cells, changed);
            elapsed += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                           .count();
            Accumulate(&result.totals, report);
            if (checkConservation && conserved && !(MeasureTotals(table, field, cells) == before)) {
                std::printf("  保存が崩れた刻み %llu\n", (unsigned long long)tick);
                conserved = false;
            }

            const uint64_t level = wallColumn();
            if (result.arrivalTick == 0 && level >= 24)  // 全体の水 24 セルの 1%
                result.arrivalTick = tick;

            if (level > result.peakLevelPercent) {
                result.peakLevelPercent = level;
                result.peakTick = tick;
            }

            const int64_t momentum = field.MomentumSum(0);
            if (momentum > result.peakMomentum) {
                result.peakMomentum = momentum;
                result.reboundMomentum = momentum;
            }

            result.reboundMomentum = std::min(result.reboundMomentum, momentum);
            result.finalLevelPercent = level;
        }

        EXPECT(conserved);
        result.nanosecondsPerTick = elapsed / (int64_t)DAM_TICKS;
        result.hash = CellsHash(field, cells) ^ field.Hash();

        return result;
    }

    void TestDamBreak() {
        std::printf("ダムを壊す\n");
        const BakedReactionTable table = MakeTable();
        struct Variant {
            const char* name;
            bool advection;
            uint32_t frictionShift;
        };

        const std::array<Variant, 4> variants = {{{"移流あり・摩擦 1/128(既定)", true, 7},
                                                  {"移流なし・摩擦 1/128", false, 7},
                                                  {"移流あり・摩擦 1/16(T-0609 の既定)", true, 4},
                                                  {"移流あり・摩擦 1/1024", true, 10}}};
        std::array<DamResult, 4> results;
        for (uint32_t i = 0; i < variants.size(); ++i) {
            ProbeLiquidOptions options = {.minCell = DAM_REGION_MIN, .sizeCells = DAM_REGION_SIZE};
            options.advection = variants[i].advection;
            options.frictionShift = variants[i].frictionShift;
            results[i] = RunDamBreak(table, options, true);
            const DamResult& r = results[i];
            std::printf(
                "  %s: 壁に届く %.2f 秒・壁際の最高 %.2f セル(%.2f 秒)・6 秒後 %.2f セル・x の運動量 最大 %lld → 最小 "
                "%lld・"
                "行き場なし %lld・速さの上限 %lld(%llu 面)・熱 %lld mJ・1 刻み %.1f µs\n",
                variants[i].name, (double)r.arrivalTick / 60.0, (double)r.peakLevelPercent / 100.0,
                (double)r.peakTick / 60.0, (double)r.finalLevelPercent / 100.0, (long long)r.peakMomentum,
                (long long)r.reboundMomentum, (long long)r.totals.dropped, (long long)r.totals.capped,
                (unsigned long long)r.totals.cappedFaces, (long long)r.totals.heat,
                (double)r.nanosecondsPerTick / 1000.0);
        }

        // 既定: 3 秒までに壁に届き、壁際で落ち着いた水位(1 セル)の 1.4 倍より高く駆け上がり、x の運動量が負に変わる(跳ね返る)。
        // 移流なしより速く届く。行き場なしは運動量の最大の 1e-6 未満
        const DamResult& chosen = results[0];
        EXPECT(chosen.arrivalTick != 0 && chosen.arrivalTick <= 180);
        EXPECT(chosen.arrivalTick < results[1].arrivalTick || results[1].arrivalTick == 0);
        EXPECT(chosen.peakLevelPercent >= 140);
        EXPECT(chosen.reboundMomentum < 0);
        EXPECT(chosen.totals.dropped * 1000000 <= chosen.peakMomentum);
        EXPECT(chosen.totals.heat > 0);

        // 決定性: もう 1 回
        ProbeLiquidOptions options = {.minCell = DAM_REGION_MIN, .sizeCells = DAM_REGION_SIZE};
        EXPECT(RunDamBreak(table, options, false).hash == results[0].hash);
    }

}  // namespace

int main() {
    TestBlankAndBake();
    TestStillPool();
    TestPoolAndDrain();
    TestDeterminism();
    TestProbeReference();
    TestDamBreak();

    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("すべて通った\n");
    return 0;
}
