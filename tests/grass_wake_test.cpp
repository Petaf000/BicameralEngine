// grass_wake_test.cpp — セルに混ぜた小さい燃える物(草)を、当たった熱が集まった所だけ物として起こして物の熱で燃やす研究の試作(T-1097・D-645。CPU だけ)。
// 試験の中の仮の口(土台は変えない。CLAUDE.md §1.9)。物の熱は tests/object_heat_cpu.h(共有の式)。
// 場面: 地面のすぐ上の空気のセルに、乾いた草を面密度 σ(0.3 kg/m² の cellulose)で混ぜる(セルの量 = σ × セルの上の面の広さ。セルの細かさで濃さは同じ)。
//   虫眼鏡の焦点の代わりに、2 mm 角の升 3 × 3 に 4 W(真ん中 2.4・辺 0.3 × 4・角 0.1 × 4)を当てる(レンズの升は世界の µm の格子で、セルの細かさによらない)。
// 1 本目(当たる熱の流れで起こす): 升の熱の流れ(W/m²)が Q_WAKE(着火の温度で薄い草が両面から失う熱の流れの目安 10 kW/m²)以上の升を覆う
//   箱 7.8 mm のボクセル(薄い物・両面が外)を、物として起こす。量 = ボクセルの面の広さ × σ(物理量。セルの細かさによらない)、
//   空気はセルと同じ濃さ、温度はセルの温度。セルからは同じ量を引き、粗い側の 1 単位に満たない分は対の端数へ(保存はビット単位)。
// 確かめる: 起きる・着火する・0.5 m と 0.25 m のセルで着火の時刻が揃う・保存・燃え広がるか(記録)。比べ: 起こさずにセルへ当てる(燃え始めない)。
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "object_heat_cpu.h"
#include "sun_column_scene.h"

namespace {

    using namespace bicameral;
    namespace rx = bicameral::reaction;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    // --- 試験の値(公開側の試験データ。ゲームの中身ではない。D-006)---
    constexpr int32_t BODY_LEVEL = 6;  // 箱 7.8 mm
    constexpr int64_t CELL_UM = 500000;
    constexpr uint32_t FILM_UM = 3906;
    constexpr uint64_t GRASS_MICROMOLES_PER_M2 = 1850000;  // 乾いた草 0.3 kg/m²(cellulose 162 g/mol)
    constexpr uint64_t AIR_OXYGEN_UNITS = 1092000;
    constexpr uint64_t AIR_NITROGEN_UNITS = 4108000;
    constexpr uint32_t DEFAULT_DIFFUSIVITY_MM2_PER_S = 20;
    constexpr uint32_t GAS_CONDUCTIVITY_SCALE = 5000;
    constexpr uint32_t SOLID_CONDUCTIVITY_SCALE = 20000;
    constexpr uint64_t WORLD_SEED = 0x1097;
    constexpr uint32_t TICKS = 60 * 30;
    constexpr uint64_t Q_WAKE_WATTS_PER_M2 = 10000;
    constexpr int64_t SPOT_UM = 2000;  // レンズの升(LN_SPOT_UM と同じ 2 mm)
    // 升 3 × 3 の力(mW。真ん中・辺・角)
    constexpr std::array<int64_t, 9> SPOT_MILLIWATTS = {100, 300, 100, 300, 2400, 300, 100, 300, 100};
    constexpr int64_t IGNITION_MILLIWATTS = 500;
    constexpr int64_t BURNING_MILLIWATTS = 100;

    sim::BakedReactionTable MakeTable() {
        return test::MakeSunTable([](sim::ReactionTableDefinition& definition) {
            for (sim::SpeciesDefinition& species : definition.species) {
                if (species.phase == sim::SpeciesPhase::Gas)
                    species.thermalConductivity /= GAS_CONDUCTIVITY_SCALE;
                else if (species.name == "cellulose" || species.name == "carbon")
                    species.thermalConductivity /= SOLID_CONDUCTIVITY_SCALE;
            }
        });
    }

    // セルの量(レベル cellLevel の単位 = µmol × 8^cellLevel)の草と空気のセル
    rx::RxCell MakeGrassCell(const sim::BakedReactionTable& table, int32_t cellLevel) {
        const int64_t cellUm = CELL_UM >> cellLevel;
        // σ × セルの上の面(µm² → m² は ÷ 10^12)× 8^L
        const uint64_t grass = (GRASS_MICROMOLES_PER_M2 * static_cast<uint64_t>(cellUm * cellUm) / 1000000000000ull)
                               << (3 * cellLevel);
        const std::vector<sim::SpeciesAmount> amounts = {
            {.species = table.SpeciesId("cellulose"), .amount = grass},
            {.species = table.SpeciesId("oxygen"), .amount = AIR_OXYGEN_UNITS},
            {.species = table.SpeciesId("nitrogen"), .amount = AIR_NITROGEN_UNITS}};

        return sim::MakeReactionCell(table, amounts, test::SUN_ROOM_TEMPERATURE_MK);
    }

    // --- 1 本目: 当たる熱の流れで起こす ---

    struct Woken {
        test::OhWorld world;
        uint32_t side = 0;        // 起こした物の 1 辺のボクセルの数
        uint32_t wokenSpots = 0;  // Q_WAKE 以上の升
        uint64_t movedGrass = 0;  // セルから物へ移した草(セルの単位)
        int64_t scaleShift = 0;
        bool moved = true;
    };

    // 升 (i, j)(0〜2。焦点の真ん中が升 (1, 1) の真ん中)の真ん中の µm(焦点の真ん中 = 0)
    int64_t SpotCentreUm(uint32_t i) {
        return (static_cast<int64_t>(i) - 1) * SPOT_UM;
    }

    Woken WakeGrass(const sim::BakedReactionTable& table, const test::OhTables& tables, int32_t cellLevel) {
        Woken woken;
        rx::RxCell cell = MakeGrassCell(table, cellLevel);
        const int64_t voxelUm = CELL_UM >> BODY_LEVEL;

        // --- 起こす升: 熱の流れ = 力 ÷ 升の広さ(W/m²。mW ÷ 4 mm² = mW × 250。真ん中 600・辺 75・角 25 kW/m²)---
        // 起こすボクセル: 升の真ん中が入るボクセル(焦点の真ん中をボクセルの真ん中に置く。物の箱の中の座標 = 升の座標 ÷ 箱 + 真ん中)
        int64_t lowest = 0;
        int64_t highest = 0;
        for (uint32_t s = 0; s < SPOT_MILLIWATTS.size(); ++s) {
            if (static_cast<uint64_t>(SPOT_MILLIWATTS[s]) * 250 < Q_WAKE_WATTS_PER_M2)
                continue;

            ++woken.wokenSpots;
            for (const int64_t um : {SpotCentreUm(s % 3), SpotCentreUm(s / 3)}) {
                const int64_t voxel = (um + (voxelUm / 2) + (voxelUm * 64)) / voxelUm - 64;
                lowest = std::min(lowest, voxel);
                highest = std::max(highest, voxel);
            }
        }

        woken.side = static_cast<uint32_t>(highest - lowest + 1);
        const uint32_t count = woken.side * woken.side;

        // --- ボクセルの中身: 草 = σ × ボクセルの面の広さ × 8^6(セルの粗い単位の倍数に切る)・空気はセルと同じ濃さ ---
        const auto scaleShift = static_cast<uint32_t>(3 * (BODY_LEVEL - cellLevel));
        woken.scaleShift = scaleShift;
        const uint64_t grassVoxel = ((GRASS_MICROMOLES_PER_M2 * static_cast<uint64_t>(voxelUm * voxelUm) /
                                      1000000000000ull)
                                         << (3 * BODY_LEVEL) >>
                                     scaleShift)
                                    << scaleShift;
        const uint32_t cellulose = table.SpeciesId("cellulose");
        const std::vector<sim::SpeciesAmount> amounts = {
            {.species = cellulose, .amount = grassVoxel},
            {.species = table.SpeciesId("oxygen"), .amount = rx::OhAmountOf(cell, table.SpeciesId("oxygen"))},
            {.species = table.SpeciesId("nitrogen"), .amount = rx::OhAmountOf(cell, table.SpeciesId("nitrogen"))}};
        const int32_t cellTemperature = rx::RxComputeThermal(table.View(), cell).temperature;
        const rx::RxCell voxel = sim::MakeReactionCell(table, amounts, cellTemperature);

        test::OhWorld& world = woken.world;
        world.objects.push_back({.voxelBegin = 0,
                                 .voxelCount = count,
                                 .sizeX = woken.side,
                                 .sizeY = 1,
                                 .cell = 0,
                                 .bodyLevel = BODY_LEVEL,
                                 .cellLevel = cellLevel,
                                 .substeps = 1,
                                 .filmUm = FILM_UM});
        for (uint32_t z = 0; z < woken.side; ++z) {
            for (uint32_t x = 0; x < woken.side; ++x) {
                const uint32_t neighbours = (x > 0 ? 1u : 0u) | (x + 1 < woken.side ? 2u : 0u) | (z > 0 ? 16u : 0u) |
                                            (z + 1 < woken.side ? 32u : 0u);
                world.infos.push_back({.object = 0, .shape = neighbours | (2u << rx::OH_EXPOSED_SHIFT)});
                world.voxels.push_back(voxel);
                world.light.push_back(0);
            }
        }

        // --- セルから引く(粗い単位。1 単位に満たない分は対の端数を負に。保存: セル × 8^Δ + ボクセル + 端数 = 一定)---
        rx::OhFlow carry = rx::OhZeroFlow();
        const uint64_t grassTotal = grassVoxel * count;
        woken.movedGrass = grassTotal >> scaleShift;
        rx::OhAmountChange change = rx::OhAddAmount(cell, cellulose, -static_cast<int64_t>(woken.movedGrass));
        woken.moved = change.ok != 0;
        cell = change.cell;
        for (uint32_t g = 0; g < rx::OH_MAX_GASES; ++g) {
            const uint32_t species = tables.gases.species[g];
            const auto total = static_cast<int64_t>(rx::OhAmountOf(voxel, species) * count);
            if (species == 0 || total == 0)
                continue;

            const int64_t coarse = total >> scaleShift;
            change = rx::OhAddAmount(cell, species, -coarse);
            woken.moved = woken.moved && change.ok != 0;
            cell = change.cell;
            carry.amounts[g] = -(total - (coarse << scaleShift));
        }

        const int64_t energyTotal = voxel.energy * static_cast<int64_t>(count);
        const int64_t energyCoarse = energyTotal >> scaleShift;
        cell.energy -= energyCoarse;
        carry.energy = -(energyTotal - (energyCoarse << scaleShift));
        world.cells = {cell};
        world.ranges = {{.cell = 0, .objectBegin = 0, .objectCount = 1}};
        world.carries = {carry};

        // --- 光: 升の力を、升の真ん中が入るボクセルへ(草が短い帯を吸う割合で。1 刻み = 1/60 s、mW × 8^6 / 60 = ボクセルの単位の mJ)---
        const uint64_t absorbQ16 = 65536 - table.speciesLight[cellulose].reflectanceShortQ16;
        for (uint32_t s = 0; s < SPOT_MILLIWATTS.size(); ++s) {
            const int64_t x = (SpotCentreUm(s % 3) + (voxelUm / 2) + (voxelUm * 64)) / voxelUm - 64 - lowest;
            const int64_t z = (SpotCentreUm(s / 3) + (voxelUm / 2) + (voxelUm * 64)) / voxelUm - 64 - lowest;
            if (x < 0 || z < 0 || x >= woken.side || z >= woken.side)
                continue;

            const int64_t energy = (SPOT_MILLIWATTS[s] << (3 * BODY_LEVEL)) / 60;
            world.light[static_cast<size_t>(x + (woken.side * z))] += static_cast<int64_t>(
                (static_cast<uint64_t>(energy) * absorbQ16) >> 16);
        }

        test::FinishObjectHeatWorld(world);

        return woken;
    }

    // 保存の合計(ボクセルの単位): セル × 8^Δ + ボクセル + 端数 − 入った光(エネルギー)、元素
    std::vector<int64_t> Totals(const sim::BakedReactionTable& table, const test::OhTables& tables,
                                const test::OhWorld& world, int64_t scaleShift) {
        const size_t elementCount = table.elementNames.size();
        std::vector<int64_t> totals(1 + elementCount, 0);
        totals[0] = (world.cells[0].energy << scaleShift) - world.sunIn;
        const std::vector<uint64_t> cellCounts = sim::CountElements(table, world.cells[0]);
        for (size_t e = 0; e < elementCount; ++e)
            totals[1 + e] += static_cast<int64_t>(cellCounts[e]) << scaleShift;

        for (const rx::RxCell& voxel : world.voxels) {
            totals[0] += voxel.energy;
            const std::vector<uint64_t> counts = sim::CountElements(table, voxel);
            for (size_t e = 0; e < elementCount; ++e)
                totals[1 + e] += static_cast<int64_t>(counts[e]);
        }

        for (const rx::OhFlow& carry : world.carries) {
            totals[0] += carry.energy;
            for (uint32_t g = 0; g < rx::OH_MAX_GASES; ++g) {
                const uint32_t species = tables.gases.species[g];
                for (size_t e = 0; species != 0 && e < elementCount; ++e)
                    totals[1 + e] += carry.amounts[g] * table.speciesElements[(species * elementCount) + e];
            }
        }

        return totals;
    }

    struct Burn {
        uint32_t voxels = 0;
        uint32_t wokenSpots = 0;
        uint32_t ignitionTick = 0;
        uint32_t lastBurningTick = 0;
        uint32_t burntVoxels = 0;
        int32_t peakMilliKelvin = 0;
        bool conserved = true;
        bool moved = true;
        double movedGrams = 0.0;
    };

    Burn RunBurn(const sim::BakedReactionTable& table, const test::OhTables& tables, int32_t cellLevel) {
        Woken woken = WakeGrass(table, tables, cellLevel);
        test::OhWorld& world = woken.world;
        const test::OhCpuTable shared{.view = table.View(), .objectSpecies = tables.species};
        const sim::ReactionTableView view = table.View();
        const std::vector<int64_t> initial = Totals(table, tables, world, woken.scaleShift);
        const uint32_t cellulose = table.SpeciesId("cellulose");
        const uint32_t carbon = table.SpeciesId("carbon");
        const uint64_t fuelStart = 6 * rx::OhAmountOf(world.voxels[0], cellulose);
        Burn burn{.voxels = static_cast<uint32_t>(world.voxels.size()),
                  .wokenSpots = woken.wokenSpots,
                  .moved = woken.moved,
                  .movedGrams = static_cast<double>(woken.movedGrass) * 162.0 / 1.0e6 /
                                static_cast<double>(int64_t{1} << (3 * cellLevel))};
        for (uint32_t tick = 1; tick <= TICKS; ++tick) {
            std::vector<int64_t> before(world.voxels.size());
            for (size_t i = 0; i < world.voxels.size(); ++i)
                before[i] = rx::RxChemicalEnergy(view, world.voxels[i]);

            test::StepObjectHeat(shared, tables.gases, world, true, WORLD_SEED, tick);
            for (size_t i = 0; i < world.voxels.size(); ++i) {
                const int64_t released = before[i] - rx::RxChemicalEnergy(view, world.voxels[i]);
                const int64_t milliwatts = (released * 60 / 1000) >> (3 * BODY_LEVEL);
                if (burn.ignitionTick == 0 && milliwatts >= IGNITION_MILLIWATTS)
                    burn.ignitionTick = tick;

                if (milliwatts >= BURNING_MILLIWATTS)
                    burn.lastBurningTick = tick;

                burn.peakMilliKelvin = std::max(burn.peakMilliKelvin,
                                                rx::RxComputeThermal(view, world.voxels[i]).temperature);
            }

            burn.conserved = burn.conserved && !world.failed &&
                             Totals(table, tables, world, woken.scaleShift) == initial;
        }

        for (const rx::RxCell& voxel : world.voxels)
            burn.burntVoxels += (6 * rx::OhAmountOf(voxel, cellulose)) + rx::OhAmountOf(voxel, carbon) <= fuelStart / 2
                                    ? 1u
                                    : 0u;

        return burn;
    }

    void Print(const char* label, const Burn& burn) {
        std::printf(
            "%s: 起こした升 %u・ボクセル %u・移した草 %.3f g・着火 %u 刻み(%.2f s)・最後に燃えていた %u "
            "刻み・燃えたボクセル %u・"
            "一番熱い %.0f K・保存 %s・移す %s\n",
            label, burn.wokenSpots, burn.voxels, burn.movedGrams, burn.ignitionTick, burn.ignitionTick / 60.0,
            burn.lastBurningTick, burn.burntVoxels, burn.peakMilliKelvin / 1000.0, burn.conserved ? "0" : "崩れた",
            burn.moved ? "OK" : "失敗");
    }

    // 比べ: 起こさずに、同じ 4 W を草のセルへ均す(30 s。燃え始めない)
    void PrintMixed(const sim::BakedReactionTable& table, int32_t cellLevel) {
        rx::RxCell cell = MakeGrassCell(table, cellLevel);
        const sim::ReactionTableView view = table.View();
        const int32_t start = rx::RxComputeThermal(view, cell).temperature;
        int64_t total = 0;
        for (const int64_t milliwatts : SPOT_MILLIWATTS)
            total += milliwatts;

        int64_t carry = 0;
        for (uint32_t tick = 1; tick <= TICKS; ++tick) {
            const int64_t energy = (total << (3 * cellLevel)) + carry;
            cell.energy += energy / 60;
            carry = energy % 60;
            cell = sim::StepReactionCellWait(table, cell, WORLD_SEED, tick, tick - 1, 0).cell;
        }

        std::printf("比べ(起こさずにセルへ均す・%s): %.3f K → %.3f K\n", cellLevel == 0 ? "0.5 m" : "0.25 m",
                    start / 1000.0, rx::RxComputeThermal(view, cell).temperature / 1000.0);
    }

}  // namespace

int main() {
    const sim::BakedReactionTable table = MakeTable();
    const auto tables = test::MakeObjectHeatTables(table, DEFAULT_DIFFUSIVITY_MM2_PER_S);
    EXPECT(tables.has_value());
    if (!tables)
        return 1;

    const Burn coarse = RunBurn(table, *tables, 0);
    const Burn fine = RunBurn(table, *tables, 1);
    Print("1 本目(熱の流れで起こす)0.5 m の世界", coarse);
    Print("1 本目(熱の流れで起こす)0.25 m の世界", fine);
    PrintMixed(table, 0);
    PrintMixed(table, 1);
    EXPECT(coarse.conserved && fine.conserved && coarse.moved && fine.moved);
    EXPECT(coarse.ignitionTick != 0 && fine.ignitionTick != 0);
    const uint32_t gap = coarse.ignitionTick > fine.ignitionTick ? coarse.ignitionTick - fine.ignitionTick
                                                                 : fine.ignitionTick - coarse.ignitionTick;
    std::printf("着火の時刻の差 %.2f %%\n", 100.0 * gap / std::max(coarse.ignitionTick, 1u));
    EXPECT(gap * 10 <= std::max(coarse.ignitionTick, 1u));
    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("grass_wake: 全部通った\n");

    return 0;
}
