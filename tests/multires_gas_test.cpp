// multires_gas_test.cpp — 多重解像度の世界の 1 段の気体(sim/multires_gas。T-0720)が、仮の世界の気体(sim/probe_gas。区画 = 世界いっぱい・
// 陽解法)と毎刻みビット単位で同じことを確かめる。
//   1. 静かな大気: 20 刻みでセルが変わらず、動かない(閉じた端・開いた端)
//   2. 燃える木箱: 底の熱い木箱。毎刻み段の外で O2 の一部を CO2 に替えて熱を足し、N2 を少し除く(反応の代わり。気体の質量が増える・減る
//      = 欄が物質に付く〔ADR-0286〕の両方の道)。閉じた端
//   3. 熱い泡: 底の熱い湯気の塊が昇る。端は全部開いている(外は層の基準の大気。出入りは帳簿)
//   4. 静止の丸め(T-0723)を入れた熱い泡も同じ
//   5. 保存点に戻して進めると同じハッシュ
// 比べるもの: 全部のセル・運動量の欄・前の質量・運動エネルギーのぶんの和・帳簿(エネルギーと運動量)・刻みの報告。
// 仮の世界は y が上、多重解像度の世界は z が上なので、多重解像度の (x, y, z) = 仮の世界の (x, z, y)(気体の箱の軸と同じ)。
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "reaction_test_table.h"
#include "sim/multires_fields.h"
#include "sim/multires_gas.h"
#include "sim/multires_nest.h"
#include "sim/multires_state.h"
#include "sim/probe_gas.h"
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

    // --- 区画(仮の世界の格子の座標。y が上)= 多重解像度の世界の箱(2 × 2 × 3 ブロック)---
    constexpr std::array<uint32_t, 3> REGION_MIN = {24, 0, 24};
    constexpr std::array<uint32_t, 3> REGION_SIZE = {16, 24, 16};
    constexpr uint32_t WOOD_MIN = 30;  // x・z
    constexpr uint32_t WOOD_BOTTOM = 1;
    constexpr uint32_t WOOD_SIZE = 3;
    constexpr uint64_t WOOD_AMOUNT = 50000000;     // µmol
    constexpr int32_t WOOD_TEMPERATURE = 1000000;  // mK
    constexpr int32_t AIR_TEMPERATURE = 300000;    // mK
    constexpr uint64_t SEED = 12345;

    // 気体の箱の軸(x・y・z = 上)→ 仮の世界の格子の軸
    constexpr std::array<uint32_t, 3> GRID_AXIS_OF_GAS_AXIS = {0, 2, 1};

    BakedReactionTable MakeTable() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        definition.environment = StandardWorldPhysics();
        definition.atmosphere = {
            .points = {{.heightMillimeters = 0,
                        .temperatureMilliKelvin = (uint32_t)AIR_TEMPERATURE,
                        .fractions = {{.species = "nitrogen", .partsPerMillion = 790000},
                                      {.species = "oxygen", .partsPerMillion = 210000}}}},
            .surfacePressurePascals = 101325,
        };

        auto baked = BakeReactionTable(definition);
        if (!baked) {
            std::printf("表を作れない: %s\n", baked.error().c_str());
            std::exit(1);
        }

        return std::move(*baked);
    }

    enum class Scene : uint8_t {
        Quiet,
        Wood,
        Bubble,
    };

    bool InBlob(uint32_t x, uint32_t y, uint32_t z) {
        return x >= WOOD_MIN && x < WOOD_MIN + WOOD_SIZE && z >= WOOD_MIN && z < WOOD_MIN + WOOD_SIZE &&
               y >= WOOD_BOTTOM && y < WOOD_BOTTOM + WOOD_SIZE;
    }

    // 区画のセル(基準の大気。木箱・湯気の塊は同じ圧力で熱い)
    reaction::RxCell MakeSceneCell(const BakedReactionTable& table, const ProbeGasField& field, Scene scene, uint32_t x,
                                   uint32_t y, uint32_t z) {
        const reaction::RxCell air = field.ReferenceCell(table, y);
        if (scene == Scene::Quiet || !InBlob(x, y, z))
            return air;

        const bool steam = scene == Scene::Bubble;
        std::vector<SpeciesAmount> amounts;
        if (!steam)
            amounts.push_back({.species = table.SpeciesId("cellulose"), .amount = WOOD_AMOUNT});

        uint64_t airAmount = 0;
        for (uint32_t i = 0; i < air.speciesCount; ++i)
            airAmount += air.amounts[i];

        if (steam)
            amounts.push_back({.species = table.SpeciesId("water_vapor"),
                               .amount = airAmount * 9 / 10 * (uint64_t)AIR_TEMPERATURE / (uint64_t)WOOD_TEMPERATURE});

        for (uint32_t i = 0; i < air.speciesCount; ++i)
            amounts.push_back({.species = air.species[i],
                               .amount = air.amounts[i] * (steam ? 1u : 10u) / 10u * (uint64_t)AIR_TEMPERATURE /
                                         (uint64_t)WOOD_TEMPERATURE});

        std::ranges::sort(amounts, {}, &SpeciesAmount::species);

        return MakeReactionCell(table, amounts, WOOD_TEMPERATURE);
    }

    // --- 2 つの世界 ---
    struct Worlds {
        std::vector<reaction::RxCell> grid;
        ProbeGasField probe;
        MultiresNest nest;
        MultiresGas gas;
    };

    // 多重解像度の座標(x・y・z = 上)
    // 仮の世界の格子 (gx, gy〔上〕, gz) → 多重解像度の世界の座標(上は WE_UP_AXIS。横は WeAxisUpLast の順に格子の x・z)
    std::array<uint32_t, 3> NestPlace(uint32_t gx, uint32_t gy, uint32_t gz) {
        std::array<uint32_t, 3> place = {};
        place[multires::WeAxisUpLast(0)] = gx - REGION_MIN[0];
        place[multires::WeAxisUpLast(1)] = gz - REGION_MIN[2];
        place[multires::WE_UP_AXIS] = gy - REGION_MIN[1];

        return place;
    }

    std::unique_ptr<Worlds> MakeWorlds(const BakedReactionTable& table, Scene scene, bool open, uint32_t rest = 0) {
        const GasBoundary boundary = open ? GasBoundary::Open : GasBoundary::Wall;
        auto probe = ProbeGasField::Create(table, {.minCell = REGION_MIN,
                                                   .sizeCells = REGION_SIZE,
                                                   .boundary = {boundary, boundary, boundary},
                                                   .randomSeed = SEED,
                                                   .restSpeedUnits = rest});
        if (!probe) {
            std::printf("区画を作れない: %s\n", probe.error().c_str());
            std::exit(1);
        }

        const std::array<uint32_t, 3> size = NestPlace(REGION_MIN[0] + REGION_SIZE[0], REGION_MIN[1] + REGION_SIZE[1],
                                                       REGION_MIN[2] + REGION_SIZE[2]);
        const uint32_t blocks = (size[0] / 8) * (size[1] / 8) * (size[2] / 8);
        uint32_t indexEntries = 1;
        while (indexEntries < 2 * blocks)
            indexEntries *= 2;

        MultiresNest nest = MakeMultiresNest({.worldBlocks = blocks,
                                              .observerBlocks = 0,
                                              .fractions = 1,
                                              .pages = blocks,
                                              .indexEntries = indexEntries,
                                              .ledgerColumns = 1 + (uint32_t)table.species.size()});
        const uint32_t edge = open ? multires::WE_OPEN : multires::WE_WALL;
        nest.edges.world = {.rootLevel = 0,
                            .lowX = 0,
                            .lowY = 0,
                            .lowZ = 0,
                            .highX = size[0],
                            .highY = size[1],
                            .highZ = size[2],
                            .top = edge,
                            .bottom = edge,
                            .sides = edge};
        auto gas = MultiresGas::Create(nest, table,
                                       {.randomSeed = SEED, .floorHeightMillimeters = 0, .restSpeedUnits = rest});
        if (!gas) {
            std::printf("気体を作れない: %s\n", gas.error().c_str());
            std::exit(1);
        }

        auto worlds = std::make_unique<Worlds>(
            Worlds{.grid = MakeBlankProbeWorld(), .probe = std::move(*probe), .nest = std::move(nest), .gas = *gas});
        for (uint32_t y = REGION_MIN[1]; y < REGION_MIN[1] + REGION_SIZE[1]; ++y) {
            for (uint32_t z = REGION_MIN[2]; z < REGION_MIN[2] + REGION_SIZE[2]; ++z) {
                for (uint32_t x = REGION_MIN[0]; x < REGION_MIN[0] + REGION_SIZE[0]; ++x)
                    worlds->grid[ProbeCellIndex(x, y, z)] = MakeSceneCell(table, worlds->probe, scene, x, y, z);
            }
        }

        for (uint32_t bz = 0; bz < size[2]; bz += 8) {
            for (uint32_t by = 0; by < size[1]; by += 8) {
                for (uint32_t bx = 0; bx < size[0]; bx += 8) {
                    std::vector<reaction::RxCell> cells(multires::MR_BLOCK_CELLS);
                    for (uint32_t index = 0; index < cells.size(); ++index) {
                        const std::array<uint32_t, 3> place = {bx + (index % 8), by + ((index / 8) % 8),
                                                               bz + (index / 64)};
                        cells[index] = worlds->grid[ProbeCellIndex(REGION_MIN[0] + place[multires::WeAxisUpLast(0)],
                                                                   REGION_MIN[1] + place[multires::WE_UP_AXIS],
                                                                   REGION_MIN[2] + place[multires::WeAxisUpLast(1)])];
                    }

                    PlaceRootBlock(worlds->nest, bx, by, bz, cells);
                }
            }
        }

        return worlds;
    }

    // 仮の世界の格子のセル (gx, gy, gz) の多重解像度の枠と番号
    std::array<uint32_t, 2> NestCell(const Worlds& worlds, uint32_t gx, uint32_t gy, uint32_t gz) {
        const std::array<uint32_t, 3> place = NestPlace(gx, gy, gz);
        const uint32_t slot = LookupBlock(worlds.nest, 0, place[0] & ~7u, place[1] & ~7u, place[2] & ~7u);

        return {slot, multires::MrCellIndex(place[0] & 7u, place[1] & 7u, place[2] & 7u)};
    }

    bool SameCell(const reaction::RxCell& a, const reaction::RxCell& b) {
        if (a.energy != b.energy || a.speciesCount != b.speciesCount)
            return false;

        for (uint32_t i = 0; i < a.speciesCount; ++i) {
            if (a.species[i] != b.species[i] || a.amounts[i] != b.amounts[i])
                return false;
        }

        return true;
    }

    // 段の外の変化(反応の代わり): O2 の 1/8 を CO2 に替えて熱 40 J を足し、N2 の 1/64 を除く(両方の世界へ同じ値)
    reaction::RxCell Burn(const BakedReactionTable& table, const reaction::RxCell& cell) {
        const uint32_t oxygen = table.SpeciesId("oxygen");
        const uint32_t nitrogen = table.SpeciesId("nitrogen");
        const uint32_t carbonDioxide = table.SpeciesId("carbon_dioxide");
        std::vector<SpeciesAmount> amounts;
        uint64_t burned = 0;
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            uint64_t amount = cell.amounts[i];
            if (cell.species[i] == oxygen) {
                burned = amount / 8;
                amount -= burned;
            } else if (cell.species[i] == nitrogen) {
                amount -= amount / 64;
            }

            amounts.push_back({.species = cell.species[i], .amount = amount});
        }

        const auto found = std::ranges::find(amounts, carbonDioxide, &SpeciesAmount::species);
        if (found != amounts.end())
            found->amount += burned;
        else
            amounts.push_back({.species = carbonDioxide, .amount = burned});

        std::erase_if(amounts, [](const SpeciesAmount& amount) { return amount.amount == 0; });
        std::ranges::sort(amounts, {}, &SpeciesAmount::species);
        reaction::RxCell after = reaction::RxMakeEmptyCell(cell.energy + 40000);
        for (uint32_t i = 0; i < amounts.size(); ++i) {
            after.species[i] = amounts[i].species;
            after.amounts[i] = amounts[i].amount;
        }

        after.speciesCount = (uint32_t)amounts.size();

        return after;
    }

    void BurnWood(const BakedReactionTable& table, Worlds& worlds) {
        for (uint32_t y = WOOD_BOTTOM; y < WOOD_BOTTOM + WOOD_SIZE; ++y) {
            for (uint32_t z = WOOD_MIN; z < WOOD_MIN + WOOD_SIZE; ++z) {
                for (uint32_t x = WOOD_MIN; x < WOOD_MIN + WOOD_SIZE; ++x) {
                    reaction::RxCell& cell = worlds.grid[ProbeCellIndex(x, y, z)];
                    cell = Burn(table, cell);
                    const std::array<uint32_t, 2> at = NestCell(worlds, x, y, z);
                    EXPECT(ExpandNestPage(worlds.nest, at[0]));
                    StoreNestCell(worlds.nest, at[0], at[1], cell);
                    PokeNestBlock(worlds.nest, at[0]);
                }
            }
        }
    }

    // 全部のセル・欄・帳簿・報告が同じか
    bool SameWorlds(const Worlds& worlds, const ProbeGasStepReport& probe, const MultiresGasStepReport& nest) {
        bool same = probe.gasSpecies == nest.gasSpecies && probe.changedCells == nest.changedCells &&
                    probe.restedCells == nest.restedCells && probe.skippedCellOverflow == nest.skippedCellOverflow &&
                    probe.blockedCells == nest.blockedCells && probe.partialCells == nest.partialCells;
        const MultiresGasFields& fields = worlds.gas.Fields();
        for (uint32_t region = 0; region < worlds.probe.RegionCellCount() && same; ++region) {
            const uint32_t index = worlds.probe.GridIndex(region);
            const uint32_t gx = index % PROBE_GRID_SIZE;
            const uint32_t gy = (index / PROBE_GRID_SIZE) % PROBE_GRID_SIZE;
            const uint32_t gz = index / PROBE_SLICE_CELL_COUNT;
            const std::array<uint32_t, 2> at = NestCell(worlds, gx, gy, gz);
            same = SameCell(worlds.grid[index], LoadNestCell(worlds.nest, at[0], at[1]));
            for (uint32_t axis = 0; axis < 3; ++axis) {
                same = same && worlds.probe.Momentum()[region][axis] ==
                                   LoadNestField(worlds.nest, fields.momentum, at[0], at[1], axis);
            }

            same = same && (int64_t)worlds.probe.Mass()[region] ==
                               LoadNestField(worlds.nest, fields.massBefore, at[0], at[1], 0);
        }

        same = same && worlds.probe.KineticEnergySum() == worlds.gas.KineticEnergySum(worlds.nest);
        same = same && worlds.probe.Ledger().energy == worlds.gas.LedgerEnergy(worlds.nest);
        const std::array<int64_t, 3> momentum = worlds.gas.LedgerMomentum(worlds.nest);
        for (uint32_t axis = 0; axis < 3; ++axis)
            same = same && worlds.probe.Ledger().momentum[GRID_AXIS_OF_GAS_AXIS[axis]] == momentum[axis];

        return same;
    }

    struct RunResult {
        uint64_t firstMismatch = UINT64_MAX;  // 違った最初の刻み(無ければ UINT64_MAX)
        uint32_t changedCells = 0;            // 最後の刻みの変わったセル
        int64_t kinetic = 0;                  // 最後の運動エネルギーのぶんの和(mJ)
        int64_t probeMicroseconds = 0;
        int64_t nestMicroseconds = 0;
    };

    RunResult Run(const BakedReactionTable& table, Worlds& worlds, Scene scene, uint64_t ticks) {
        using Clock = std::chrono::steady_clock;
        RunResult result;
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            if (scene == Scene::Wood)
                BurnWood(table, worlds);

            std::vector<uint32_t> changed;
            const auto probeStart = Clock::now();
            const ProbeGasStepReport probe = worlds.probe.Step(table, worlds.grid, tick, changed);
            const auto nestStart = Clock::now();
            std::vector<uint32_t> changedSlots;
            const MultiresGasStepReport nest = worlds.gas.Step(worlds.nest, table, tick, changedSlots);
            const auto end = Clock::now();
            result.probeMicroseconds += std::chrono::duration_cast<std::chrono::microseconds>(nestStart - probeStart)
                                            .count();
            result.nestMicroseconds += std::chrono::duration_cast<std::chrono::microseconds>(end - nestStart).count();
            result.changedCells = nest.changedCells;
            if (!SameWorlds(worlds, probe, nest) && result.firstMismatch == UINT64_MAX) {
                result.firstMismatch = tick;
                std::printf("  刻み %llu で違う: 報告 仮 %u 種・%u セル / 多 %u 種・%u セル\n",
                            (unsigned long long)tick, probe.gasSpecies, probe.changedCells, nest.gasSpecies,
                            nest.changedCells);
            }
        }

        result.kinetic = worlds.gas.KineticEnergySum(worlds.nest);

        return result;
    }

    void TestScene(const BakedReactionTable& table, Scene scene, bool open, uint64_t ticks, const char* name,
                   uint32_t rest = 0) {
        const std::unique_ptr<Worlds> worlds = MakeWorlds(table, scene, open, rest);
        const RunResult result = Run(table, *worlds, scene, ticks);
        std::printf(
            "%s(%s・%llu 刻み): 毎刻みビット一致 %s・最後に変わったセル %u・運動エネルギー %lld mJ・"
            "1 刻み 仮 %lld µs / 多 %lld µs\n",
            name, open ? "開いた端" : "閉じた端", (unsigned long long)ticks,
            result.firstMismatch == UINT64_MAX ? "はい" : "いいえ", result.changedCells, (long long)result.kinetic,
            (long long)(result.probeMicroseconds / (int64_t)ticks),
            (long long)(result.nestMicroseconds / (int64_t)ticks));
        EXPECT(result.firstMismatch == UINT64_MAX);
        if (scene == Scene::Quiet) {
            EXPECT(result.changedCells == 0 && result.kinetic == 0);
        } else {
            EXPECT(result.kinetic > 0);
        }
    }

    // 保存点に戻して進めると同じハッシュ(登録した欄が保存点に入る)
    void TestSavePoint(const BakedReactionTable& table) {
        const std::unique_ptr<Worlds> worlds = MakeWorlds(table, Scene::Bubble, false);
        Run(table, *worlds, Scene::Bubble, 10);
        const MultiresSaveState save = SaveNestState(worlds->nest);
        const auto runNest = [&] {
            for (uint64_t tick = 10; tick < 20; ++tick) {
                std::vector<uint32_t> changedSlots;
                (void)worlds->gas.Step(worlds->nest, table, tick, changedSlots);
            }

            return HashWholeNest(worlds->nest);
        };
        const uint64_t first = runNest();
        EXPECT(RestoreNestState(worlds->nest, save));
        EXPECT(runNest() == first);
        std::printf("保存点に戻して 10 刻み: 同じハッシュ %016llx\n", (unsigned long long)first);
    }

}  // namespace

int main() {
    const BakedReactionTable table = MakeTable();
    TestScene(table, Scene::Quiet, false, 20, "静かな大気");
    TestScene(table, Scene::Quiet, true, 20, "静かな大気");
    TestScene(table, Scene::Wood, false, 60, "燃える木箱");
    TestScene(table, Scene::Bubble, true, 60, "熱い泡");
    TestScene(table, Scene::Bubble, false, 60, "熱い泡");
    TestScene(table, Scene::Bubble, false, 60, "熱い泡 + 静止の丸め 2^14 単位", 1u << 14);  // T-0723
    TestSavePoint(table);

    if (failureCount != 0) {
        std::printf("multires_gas_test: %d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("multires_gas_test: 全部通った\n");
    return 0;
}
