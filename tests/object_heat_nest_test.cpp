// object_heat_nest_test.cpp — 物の熱(tests/object_heat_cpu.h)を多重解像度の世界(MultiresNest)のセルにつなぐ最初の形の CPU の試験(T-1077・ADR-0451・ADR-0459)。
// 1 刻み = 世界の刻み(StepNest)→ 囲むセルを世界から読む(LoadNestCell。変わったセルは物のブロックを起こす)→ 物の熱の刻み(StepObjectHeat)
//   → 変わったセルを世界へ書く(ExpandNestPage・StoreNestCell・PokeNestBlock。液体の 1 段〔T-0717〕と同じ「刻みの段の外から書く」口)。
// 場面: 4 m 角の箱(下 1 m が石、上は空気)の真ん中の空気のセルに燃えている小枝 3 本と紙 1 枚、+x の隣のセルに冷えた薪 1 本。
// 確かめる:
//   1. 保存: 世界(ComputeConservedTotals。帳簿込み)+ 物のボクセル + 対の端数 がエネルギーと元素で毎 60 刻み一定(物との受け渡しは世界の帳簿に載せていない。
//      載せるには帳簿の理由「物との受け渡し」が要る = 土台の共通部分。司令塔へ。T-1077 のチケット)
//   2. 決定的: 2 回で同じ
//   3. 世界のセルが物の熱を受け取る(燃えている所の空気が温まる)・冷えた薪のブロックは眠る
//   4. 0.5 m と 0.25 m の世界で、燃える固体の残りを並べる(記録だけ)
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "object_heat_cpu.h"
#include "sun_column_scene.h"

namespace {

    using namespace bicameral;
    namespace rx = bicameral::reaction;
    namespace mr = bicameral::multires;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    // --- 試験の値(公開側の試験データ。ゲームの中身ではない。D-006。gpu_object_heat_test と同じ物)---
    constexpr int32_t BODY_LEVEL = 6;
    constexpr uint32_t FILM_UM = 3906;
    constexpr uint64_t FULL_CELLULOSE_UNITS = 1157000000;
    constexpr uint64_t AIR_OXYGEN_UNITS = 1092000;
    constexpr uint64_t AIR_NITROGEN_UNITS = 4108000;
    constexpr uint32_t DEFAULT_DIFFUSIVITY_MM2_PER_S = 20;
    constexpr uint32_t GAS_CONDUCTIVITY_SCALE = 5000;
    constexpr uint32_t SOLID_CONDUCTIVITY_SCALE = 20000;
    constexpr int32_t BURNING_MILLIKELVIN = 900000;
    constexpr uint64_t WORLD_SEED = 0x1077;
    constexpr uint32_t TICKS = 600;

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

    // --- 物(箱の並び。外に面するボクセルの数を形の語に)---

    struct Shape {
        uint32_t sizeX;
        uint32_t sizeY;
        uint32_t sizeZ;
        uint64_t celluloseUnits;
        bool sheet;
    };

    constexpr Shape PAPER{27, 1, 38, (FULL_CELLULOSE_UNITS * 100 << BODY_LEVEL) / 500000, true};
    constexpr Shape TWIG{2, 2, 38, (FULL_CELLULOSE_UNITS * 128) / 1000, false};
    constexpr Shape LOG{10, 10, 51, (FULL_CELLULOSE_UNITS * 400) / 1000, false};

    void AddObject(const sim::BakedReactionTable& table, test::OhWorld& world, const Shape& shape, uint32_t cell,
                   int32_t cellLevel, bool burning) {
        const auto objectIndex = static_cast<uint32_t>(world.objects.size());
        world.objects.push_back({.voxelBegin = static_cast<uint32_t>(world.voxels.size()),
                                 .voxelCount = shape.sizeX * shape.sizeY * shape.sizeZ,
                                 .sizeX = shape.sizeX,
                                 .sizeY = shape.sizeY,
                                 .cell = cell,
                                 .bodyLevel = BODY_LEVEL,
                                 .cellLevel = cellLevel,
                                 .substeps = 1,
                                 .filmUm = FILM_UM});
        world.carries.push_back(rx::OhZeroFlow());
        const std::vector<sim::SpeciesAmount> amounts = {
            {.species = table.SpeciesId("cellulose"), .amount = shape.celluloseUnits},
            {.species = table.SpeciesId("oxygen"), .amount = AIR_OXYGEN_UNITS},
            {.species = table.SpeciesId("nitrogen"), .amount = AIR_NITROGEN_UNITS}};
        const rx::RxCell hot = sim::MakeReactionCell(table, amounts, BURNING_MILLIKELVIN);
        const rx::RxCell cold = sim::MakeReactionCell(table, amounts, test::SUN_ROOM_TEMPERATURE_MK);
        for (uint32_t z = 0; z < shape.sizeZ; ++z) {
            for (uint32_t y = 0; y < shape.sizeY; ++y) {
                for (uint32_t x = 0; x < shape.sizeX; ++x) {
                    const std::array<bool, 6> inside = {x > 0, x + 1 < shape.sizeX, y > 0, y + 1 < shape.sizeY,
                                                        z > 0, z + 1 < shape.sizeZ};
                    uint32_t neighbours = 0;
                    uint32_t exposed = 0;
                    for (uint32_t direction = 0; direction < 6; ++direction) {
                        if (inside[direction])
                            neighbours |= 1u << direction;
                        else if (!shape.sheet || direction == 2 || direction == 3)
                            ++exposed;
                    }

                    world.infos.push_back(
                        {.object = objectIndex, .shape = neighbours | (exposed << rx::OH_EXPOSED_SHIFT)});
                    world.voxels.push_back(exposed != 0 && burning ? hot : cold);
                    world.light.push_back(0);
                }
            }
        }

        if (world.ranges.empty() || world.ranges.back().cell != cell)
            world.ranges.push_back({.cell = cell, .objectBegin = objectIndex, .objectCount = 0});

        ++world.ranges.back().objectCount;
    }

    // --- 世界のセルの口(T-1077 の最初の形)---

    struct NestCellRef {
        uint32_t slot;
        uint32_t index;
    };

    // 0.25 m の座標(x, y, z)を含むレベル level のセル(根の枠だけの世界。枠は z・y・x の順に置いた)
    NestCellRef CellAt(int32_t level, uint32_t roots, uint32_t quarterX, uint32_t quarterY, uint32_t quarterZ) {
        const auto shift = static_cast<uint32_t>(1 - level);
        const uint32_t x = quarterX >> shift;
        const uint32_t y = quarterY >> shift;
        const uint32_t z = quarterZ >> shift;

        return {.slot = (x / 8) + (roots * ((y / 8) + (roots * (z / 8)))),
                .index = mr::MrCellIndex(x % 8, y % 8, z % 8)};
    }

    // 世界から囲むセルを読む。世界の刻みで変わったセルは、物の外に面するブロックを起こす(変わった段 = 今の段)
    void LoadCells(const sim::MultiresNest& nest, std::span<const NestCellRef> refs, test::OhWorld& world) {
        for (size_t c = 0; c < refs.size(); ++c) {
            const rx::RxCell cell = sim::LoadNestCell(nest, refs[c].slot, refs[c].index);
            if (!rx::RxSameCell(cell, world.cells[c]))
                world.cellChangedPasses[c] = world.pass;

            world.cells[c] = cell;
        }
    }

    // 物の熱で変わったセルを世界へ書き、ブロックをつつく(次の刻みに反応と伝導が見る)
    bool StoreCells(sim::MultiresNest& nest, std::span<const NestCellRef> refs, const test::OhWorld& world,
                    std::span<const rx::RxCell> before) {
        bool stored = true;
        for (size_t c = 0; c < refs.size(); ++c) {
            if (rx::RxSameCell(world.cells[c], before[c]))
                continue;

            stored = stored && sim::ExpandNestPage(nest, refs[c].slot);
            sim::StoreNestCell(nest, refs[c].slot, refs[c].index, world.cells[c]);
            sim::PokeNestBlock(nest, refs[c].slot);
        }

        return stored;
    }

    // --- 保存の検査: 世界(2^-64 × レベル 6 の単位の 256bit)+ 物(レベル 6 の単位)---

    using Wide = sim::Wide256;

    Wide AddInt(Wide value, int64_t units) {
        // units × 2^64 を足す(語 0 が一番下。語 1 から足す。符号は上の語へ広げる)
        uint64_t carry = static_cast<uint64_t>(units);
        const uint64_t fill = units < 0 ? ~uint64_t{0} : 0;
        uint64_t extra = 0;
        for (size_t word = 1; word < value.size(); ++word) {
            const uint64_t add = word == 1 ? carry : fill;
            const uint64_t sum = value[word] + add + extra;
            extra = (sum < value[word] || (extra != 0 && sum == value[word])) ? 1 : 0;
            value[word] = sum;
        }

        return value;
    }

    struct Totals {
        Wide energy{};
        std::vector<Wide> elements;

        bool operator==(const Totals&) const = default;
    };

    Totals Measure(const sim::MultiresNest& nest, const sim::BakedReactionTable& table, const test::OhWorld& world) {
        const sim::ConservedTotals conserved = sim::ComputeConservedTotals(nest, table, BODY_LEVEL);
        Totals totals{.energy = conserved.energy, .elements = conserved.elements};
        int64_t energy = 0;
        std::vector<int64_t> elements(totals.elements.size(), 0);
        for (const rx::RxCell& voxel : world.voxels) {
            energy += voxel.energy;
            const std::vector<uint64_t> counts = sim::CountElements(table, voxel);
            for (size_t e = 0; e < elements.size(); ++e)
                elements[e] += static_cast<int64_t>(counts[e]);
        }

        for (const rx::OhFlow& carry : world.carries) {
            energy += carry.energy;
            for (uint32_t g = 0; g < rx::OH_MAX_GASES; ++g) {
                const uint32_t species = 0;  // 気体の番号 → 物質 ID は表から(下で足す)
                static_cast<void>(species);
            }
        }

        totals.energy = AddInt(totals.energy, energy);
        for (size_t e = 0; e < elements.size(); ++e)
            totals.elements[e] = AddInt(totals.elements[e], elements[e]);

        return totals;
    }

    // 対の端数の物質の分(気体の番号ごと)を元素に足す
    void AddCarryElements(const sim::BakedReactionTable& table, const test::OhTables& tables,
                          const test::OhWorld& world, Totals& totals) {
        const size_t elementCount = totals.elements.size();
        for (const rx::OhFlow& carry : world.carries) {
            for (uint32_t g = 0; g < rx::OH_MAX_GASES; ++g) {
                const uint32_t species = tables.gases.species[g];
                if (species == 0 || carry.amounts[g] == 0)
                    continue;

                for (size_t e = 0; e < elementCount; ++e) {
                    const int64_t count = carry.amounts[g] * table.speciesElements[(species * elementCount) + e];
                    totals.elements[e] = AddInt(totals.elements[e], count);
                }
            }
        }
    }

    // --- 場面 ---

    struct Run {
        uint64_t digest = 0;
        bool conserved = true;
        bool stored = true;
        int32_t fireAirStartMilliKelvin = 0;
        int32_t fireAirEndMilliKelvin = 0;
        int32_t logAirEndMilliKelvin = 0;
        uint64_t fuelStart = 0;
        uint64_t fuelEnd = 0;
        uint64_t logAwakeBlocks = 0;  // 後半 300 刻みで冷えた薪の回したブロック(段の和)
        size_t logBlocks = 0;
    };

    uint64_t FuelCarbon(const sim::BakedReactionTable& table, const test::OhWorld& world, uint32_t objects) {
        const uint32_t cellulose = table.SpeciesId("cellulose");
        const uint32_t carbon = table.SpeciesId("carbon");
        uint64_t total = 0;
        for (uint32_t o = 0; o < objects; ++o) {
            const rx::OhObject object = world.objects[o];
            for (uint32_t v = 0; v < object.voxelCount; ++v) {
                const rx::RxCell& cell = world.voxels[object.voxelBegin + v];
                total += (6 * rx::OhAmountOf(cell, cellulose)) + rx::OhAmountOf(cell, carbon);
            }
        }

        return total;
    }

    Run RunScene(const sim::BakedReactionTable& table, const test::OhTables& tables, int32_t level) {
        const uint32_t roots = level == 0 ? 1u : 2u;
        sim::MultiresNest nest = test::MakeSunWorld(table, level, roots, roots, roots, [](int64_t, int64_t y, int64_t) {
            return y < 4 ? test::SunMaterial::Stone : test::SunMaterial::Air;
        });
        const std::array<NestCellRef, 2> refs = {CellAt(level, roots, 8, 5, 8), CellAt(level, roots, 10, 5, 8)};
        test::OhWorld world;
        for (const NestCellRef& ref : refs)
            world.cells.push_back(sim::LoadNestCell(nest, ref.slot, ref.index));

        for (uint32_t i = 0; i < 3; ++i)
            AddObject(table, world, TWIG, 0, level, true);

        AddObject(table, world, PAPER, 0, level, true);
        AddObject(table, world, LOG, 1, level, false);
        test::FinishObjectHeatWorld(world);

        Run run;
        const test::OhCpuTable shared{.view = table.View(), .objectSpecies = tables.species};
        const sim::ReactionTableView view = table.View();
        run.fireAirStartMilliKelvin = rx::RxComputeThermal(view, world.cells[0]).temperature;
        run.fuelStart = FuelCarbon(table, world, 4);
        Totals initial = Measure(nest, table, world);
        AddCarryElements(table, tables, world, initial);
        uint64_t logAwakeAtHalf = 0;
        for (uint32_t tick = 1; tick <= TICKS; ++tick) {
            sim::StepNest(nest, table, WORLD_SEED, tick);
            LoadCells(nest, refs, world);
            const std::vector<rx::RxCell> before = world.cells;
            test::StepObjectHeat(shared, tables.gases, world, false, WORLD_SEED, tick);
            run.stored = run.stored && StoreCells(nest, refs, world, before) && !world.failed;
            if (tick == TICKS / 2)
                logAwakeAtHalf = world.awakeExchangeBlocks;

            if (tick % 60 != 0)
                continue;

            Totals now = Measure(nest, table, world);
            AddCarryElements(table, tables, world, now);
            run.conserved = run.conserved && now == initial;
        }

        run.fireAirEndMilliKelvin = rx::RxComputeThermal(view, world.cells[0]).temperature;
        run.logAirEndMilliKelvin = rx::RxComputeThermal(view, world.cells[1]).temperature;
        run.fuelEnd = FuelCarbon(table, world, 4);
        run.logBlocks = world.objects[4].blocksX * world.objects[4].blocksY * world.objects[4].blocksZ;
        run.logAwakeBlocks = world.awakeExchangeBlocks - logAwakeAtHalf;
        for (const rx::RxCell& voxel : world.voxels)
            run.digest = fx::FxHashCombine(run.digest, sim::HashReactionCell(voxel));

        for (const rx::RxCell& cell : sim::DenseCellImage(nest))
            run.digest = fx::FxHashCombine(run.digest, sim::HashReactionCell(cell));

        return run;
    }

    void Print(const char* label, const Run& run) {
        std::printf(
            "%s: 燃える物の囲むセル %.3f K → %.3f K・冷えた薪の囲むセル %.3f K・燃える固体の残り %.1f %%・"
            "後半 %u 刻みで回したブロック(全部の物)%llu・保存 %s・書き込み %s・要約 %016llx\n",
            label, run.fireAirStartMilliKelvin / 1000.0, run.fireAirEndMilliKelvin / 1000.0,
            run.logAirEndMilliKelvin / 1000.0,
            100.0 * static_cast<double>(run.fuelEnd) / static_cast<double>(std::max<uint64_t>(run.fuelStart, 1)),
            TICKS / 2, static_cast<unsigned long long>(run.logAwakeBlocks), run.conserved ? "0" : "崩れた",
            run.stored ? "OK" : "失敗", static_cast<unsigned long long>(run.digest));
    }

}  // namespace

int main() {
    const sim::BakedReactionTable table = MakeTable();
    const auto tables = test::MakeObjectHeatTables(table, DEFAULT_DIFFUSIVITY_MM2_PER_S);
    EXPECT(tables.has_value());
    if (!tables)
        return 1;

    const Run coarse = RunScene(table, *tables, 0);
    const Run again = RunScene(table, *tables, 0);
    const Run fine = RunScene(table, *tables, 1);
    Print("0.5 m の世界", coarse);
    Print("0.25 m の世界", fine);
    EXPECT(coarse.conserved && fine.conserved);
    EXPECT(coarse.stored && fine.stored);
    EXPECT(coarse.digest == again.digest);
    EXPECT(coarse.fireAirEndMilliKelvin > coarse.fireAirStartMilliKelvin);
    EXPECT(coarse.fuelEnd < coarse.fuelStart);
    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("object_heat_nest: 全部通った\n");

    return 0;
}
