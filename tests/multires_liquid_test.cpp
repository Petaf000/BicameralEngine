// multires_liquid_test.cpp — 多重解像度の世界の 1 段の液体(sim/multires_liquid。T-0717)が、仮の世界の液体(sim/probe_liquid。
// 区画 = 世界いっぱい)と毎刻みビット単位で同じか(運ぶ計算の共通の土台の段 2 ② の答え合わせ。設計 20 §3)。
// 場面: 池に溜まる → 堤を消すと抜ける / ダムを壊す + 段の外で水を半分・2 倍(欄が物質に付く)/ 液体の無い世界(何も変えない)。
// 世界の広さ: 16³(区画も 16³。刻みは長く)・24 × 16 × 8(ダム)・仮の世界の格子いっぱいの 64³(レベル 0 のブロック 8³。1 刻み 1〜2 秒なので 30 + 30 刻み)。
// 比べるもの: 全部のセル・面の運動量・面の運動のエネルギー・位置のエネルギー・前の質量・帳簿・刻みの報告。
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "reaction_test_table.h"
#include "sim/multires_fields.h"
#include "sim/multires_liquid.h"
#include "sim/multires_nest.h"
#include "sim/multires_state.h"
#include "sim/probe_liquid.h"
#include "sim/probe_sim.h"
#include "standard_world_physics.h"

using namespace bicameral;
using namespace bicameral::sim;

namespace {

    int failureCount = 0;

    // 刻みの費用(場面ごと。仮の世界と多重解像度の 1 段の Step だけ。比べる時間は入れない)
    struct StepCost {
        int64_t probeNanoseconds = 0;
        int64_t nestNanoseconds = 0;
        uint64_t ticks = 0;
    };

    StepCost stepCost;

    void PrintCost(const char* scene) {
        const uint64_t ticks = stepCost.ticks == 0 ? 1 : stepCost.ticks;
        std::printf("  費用(%s・%llu 刻み): 仮の世界 %lld µs/刻み・多重解像度の 1 段 %lld µs/刻み\n", scene,
                    (unsigned long long)stepCost.ticks, (long long)(stepCost.probeNanoseconds / 1000 / (int64_t)ticks),
                    (long long)(stepCost.nestNanoseconds / 1000 / (int64_t)ticks));
        stepCost = {};
    }

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("  失敗(%d 行): %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr uint64_t FULL_WATER = 6900000000;  // µmol(0.5 m 角の水の少し下。probe_liquid_test と同じ)
    constexpr uint64_t NITROGEN = 4000000;
    constexpr uint64_t OXYGEN = 1000000;
    constexpr uint64_t STONE = 21000000000;
    constexpr int32_t TEMPERATURE = 300000;

    // 池の地形(probe_liquid_test の池と同じ形。x は場面の中の座標)
    constexpr uint32_t POND_WIDTH = 16;
    constexpr uint32_t BASIN_BEGIN = 4;
    constexpr uint32_t DAM_X = 10;

    enum class Kind : uint8_t { Air, Water, Stone };

    BakedReactionTable MakeTable() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        definition.environment = StandardWorldPhysics();
        definition.species.push_back({.name = "liquid_water",
                                      .composition = {{.element = "H", .count = 2}, {.element = "O", .count = 1}},
                                      .formationEnthalpy = -285830,
                                      .heatCapacity = 75300,
                                      .thermalConductivity = 600,
                                      .phase = SpeciesPhase::Liquid,
                                      .density = 1000});
        definition.species.push_back({.name = "stone",
                                      .composition = {{.element = "C", .count = 1}},
                                      .formationEnthalpy = 0,
                                      .heatCapacity = 8500,
                                      .thermalConductivity = 2000,
                                      .phase = SpeciesPhase::Solid,
                                      .density = 2000});

        auto baked = BakeReactionTable(definition);
        if (!baked) {
            std::printf("表を作れない: %s\n", baked.error().c_str());
            std::exit(1);
        }

        return std::move(*baked);
    }

    reaction::RxCell MakeCell(const BakedReactionTable& table, Kind kind) {
        std::vector<SpeciesAmount> amounts = {{.species = table.SpeciesId("nitrogen"), .amount = NITROGEN},
                                              {.species = table.SpeciesId("oxygen"), .amount = OXYGEN}};
        if (kind == Kind::Water)
            amounts.push_back({.species = table.SpeciesId("liquid_water"), .amount = FULL_WATER});

        if (kind == Kind::Stone)
            amounts = {{.species = table.SpeciesId("stone"), .amount = STONE}};

        std::ranges::sort(amounts, {}, &SpeciesAmount::species);

        return MakeReactionCell(table, amounts, TEMPERATURE);
    }

    Kind PondKind(uint32_t x, uint32_t y, bool damOpen) {
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

    // --- 2 つの世界(仮の世界の格子 + 世界いっぱいの区画 / 多重解像度の 1 段)---

    using KindOf = std::function<Kind(uint32_t x, uint32_t y, uint32_t z)>;

    struct Worlds {
        std::array<uint32_t, 3> size{};  // セル(8 の倍数。y が上)
        std::vector<reaction::RxCell> grid;
        ProbeLiquidField probe;
        MultiresNest nest;
        MultiresLiquid liquid;
    };

    std::unique_ptr<Worlds> MakeWorlds(const BakedReactionTable& table, std::array<uint32_t, 3> size,
                                       const KindOf& kindOf) {
        auto probe = ProbeLiquidField::Create(table, {.minCell = {0, 0, 0}, .sizeCells = size});
        if (!probe) {
            std::printf("区画を作れない: %s\n", probe.error().c_str());
            std::exit(1);
        }

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
        auto liquid = MultiresLiquid::Create(nest, table, {});
        if (!liquid) {
            std::printf("液体を作れない: %s\n", liquid.error().c_str());
            std::exit(1);
        }

        auto worlds = std::make_unique<Worlds>(Worlds{.size = size,
                                                      .grid = MakeBlankProbeWorld(),
                                                      .probe = std::move(*probe),
                                                      .nest = std::move(nest),
                                                      .liquid = *liquid});
        for (uint32_t y = 0; y < size[1]; ++y) {
            for (uint32_t z = 0; z < size[2]; ++z) {
                for (uint32_t x = 0; x < size[0]; ++x)
                    worlds->grid[ProbeCellIndex(x, y, z)] = MakeCell(table, kindOf(x, y, z));
            }
        }

        for (uint32_t by = 0; by < size[1]; by += 8) {
            for (uint32_t bz = 0; bz < size[2]; bz += 8) {
                for (uint32_t bx = 0; bx < size[0]; bx += 8) {
                    std::vector<reaction::RxCell> cells(multires::MR_BLOCK_CELLS);
                    for (uint32_t index = 0; index < cells.size(); ++index) {
                        const uint32_t x = index % 8;
                        const uint32_t y = (index / 8) % 8;
                        const uint32_t z = index / 64;
                        cells[multires::MrCellIndex(x, y, z)] = worlds->grid[ProbeCellIndex(bx + x, by + y, bz + z)];
                    }

                    PlaceRootBlock(worlds->nest, bx, by, bz, cells);
                }
            }
        }

        return worlds;
    }

    // 両方の世界のセル (x, y, z) を同じ値に書き換える(段の外の変化: 堤を消す・水を減らす)
    void SetCell(Worlds& worlds, uint32_t x, uint32_t y, uint32_t z, const reaction::RxCell& cell) {
        worlds.grid[ProbeCellIndex(x, y, z)] = cell;
        const uint32_t slot = LookupBlock(worlds.nest, 0, x & ~7u, y & ~7u, z & ~7u);
        EXPECT(ExpandNestPage(worlds.nest, slot));
        StoreNestCell(worlds.nest, slot, multires::MrCellIndex(x & 7u, y & 7u, z & 7u), cell);
        PokeNestBlock(worlds.nest, slot);
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

    bool SameReport(const ProbeLiquidStepReport& a, const ProbeLiquidStepReport& b) {
        return a.wall == b.wall && a.gravity == b.gravity && a.friction == b.friction && a.dropped == b.dropped &&
               a.capped == b.capped && a.vanished == b.vanished && a.heat == b.heat &&
               a.potentialEntered == b.potentialEntered && a.cappedFaces == b.cappedFaces && a.wetCells == b.wetCells &&
               a.changedCells == b.changedCells && a.limitedCells == b.limitedCells &&
               a.overfilledCells == b.overfilledCells && a.movedMilligrams == b.movedMilligrams;
    }

    // 全部のセルと欄と帳簿を比べる。違えば最初の 1 つを書いて false
    bool Compare(const Worlds& worlds, const char* scene, uint64_t tick) {
        const auto size = worlds.size;
        const MultiresLiquidFields& fields = worlds.liquid.Fields();
        for (uint32_t y = 0; y < size[1]; ++y) {
            for (uint32_t z = 0; z < size[2]; ++z) {
                for (uint32_t x = 0; x < size[0]; ++x) {
                    const uint32_t region = x + (size[0] * (z + (size[2] * y)));
                    const uint32_t slot = LookupBlock(worlds.nest, 0, x & ~7u, y & ~7u, z & ~7u);
                    const uint32_t index = multires::MrCellIndex(x & 7u, y & 7u, z & 7u);
                    bool same = SameCell(worlds.grid[worlds.probe.GridIndex(region)],
                                         LoadNestCell(worlds.nest, slot, index));
                    for (uint32_t axis = 0; axis < 3 && same; ++axis) {
                        same = worlds.probe.FaceMomentum(axis, region) ==
                                   LoadNestField(worlds.nest, fields.momentum, slot, index, axis) &&
                               worlds.probe.FaceKinetic(axis, region) ==
                                   LoadNestField(worlds.nest, fields.kinetic, slot, index, axis);
                    }

                    same = same &&
                           worlds.probe.Potential(region) ==
                               LoadNestField(worlds.nest, fields.potential, slot, index, 0) &&
                           (int64_t)worlds.probe.MassBefore(region) ==
                               LoadNestField(worlds.nest, fields.massBefore, slot, index, 0);
                    if (!same) {
                        std::printf("  %s: 刻み %llu のセル (%u, %u, %u) が違う\n", scene, (unsigned long long)tick, x,
                                    y, z);
                        return false;
                    }
                }
            }
        }

        const ProbeLiquidLedger ledger = worlds.liquid.Ledger(worlds.nest);
        if (ledger.external != worlds.probe.Ledger().external || ledger.energy != worlds.probe.Ledger().energy) {
            std::printf("  %s: 刻み %llu の帳簿が違う\n", scene, (unsigned long long)tick);
            return false;
        }

        return true;
    }

    // 両方を ticks 刻み進め、毎刻み比べる。違えばそこで止めて false
    bool Run(const BakedReactionTable& table, Worlds& worlds, uint64_t ticks, const char* scene, uint64_t& tick) {
        for (uint64_t i = 0; i < ticks; ++i, ++tick) {
            std::vector<uint32_t> changed;
            std::vector<uint32_t> changedSlots;
            const auto start = std::chrono::steady_clock::now();
            const ProbeLiquidStepReport probe = worlds.probe.Step(table, worlds.grid, changed);
            // 保存の検査(T-0769): 液体の 1 刻みの前後で Σ 保存量 − 帳簿(エネルギー・元素・液体の欄の列)が変わらない
            const ConservedTotals before = ComputeConservedTotals(worlds.nest, table, worlds.nest.capacity.rootLevel);
            const auto middle = std::chrono::steady_clock::now();
            const ProbeLiquidStepReport nest = worlds.liquid.Step(worlds.nest, table, changedSlots);
            const auto end = std::chrono::steady_clock::now();
            if (ComputeConservedTotals(worlds.nest, table, worlds.nest.capacity.rootLevel) != before) {
                std::printf("  %s: 刻み %llu で Σ 保存量 − 帳簿が変わった\n", scene, (unsigned long long)tick);
                return false;
            }

            stepCost.probeNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(middle - start).count();
            stepCost.nestNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(end - middle).count();
            ++stepCost.ticks;
            if (!SameReport(probe, nest)) {
                std::printf("  %s: 刻み %llu の報告が違う(動いた %llu / %llu mg・変わったセル %u / %u)\n", scene,
                            (unsigned long long)tick, (unsigned long long)probe.movedMilligrams,
                            (unsigned long long)nest.movedMilligrams, probe.changedCells, nest.changedCells);
                return false;
            }

            if (!Compare(worlds, scene, tick))
                return false;
        }

        return true;
    }

    uint64_t WaterInX(const BakedReactionTable& table, const Worlds& worlds, uint32_t begin, uint32_t end) {
        const uint32_t water = table.SpeciesId("liquid_water");
        uint64_t total = 0;
        for (uint32_t y = 0; y < worlds.size[1]; ++y) {
            for (uint32_t z = 0; z < worlds.size[2]; ++z) {
                for (uint32_t x = begin; x < end; ++x) {
                    const uint32_t slot = LookupBlock(worlds.nest, 0, x & ~7u, y & ~7u, z & ~7u);
                    const reaction::RxCell cell = LoadNestCell(worlds.nest, slot,
                                                               multires::MrCellIndex(x & 7u, y & 7u, z & 7u));
                    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                        if (cell.species[i] == water)
                            total += cell.amounts[i];
                    }
                }
            }
        }

        return total;
    }

    // --- 1: 池に溜まる → 堤を消すと抜ける(16³。池は z の全部に)---
    void TestPondAndDrain(const BakedReactionTable& table, std::array<uint32_t, 3> size, uint32_t offsetX,
                          std::array<uint32_t, 2> pondZ, uint64_t settleTicks, uint64_t drainTicks) {
        std::printf("池に溜まる・堤を消すと抜ける(%u × %u × %u・%llu + %llu 刻み)\n", size[0], size[1], size[2],
                    (unsigned long long)settleTicks, (unsigned long long)drainTicks);
        const auto kindOf = [&](uint32_t x, uint32_t y, uint32_t z) {
            if (x < offsetX || x >= offsetX + POND_WIDTH || z < pondZ[0] || z >= pondZ[1])
                return y == 0 ? Kind::Stone : Kind::Air;

            return PondKind(x - offsetX, y, false);
        };
        auto worlds = MakeWorlds(table, size, kindOf);
        uint64_t tick = 0;
        EXPECT(Compare(*worlds, "池", tick));
        EXPECT(Run(table, *worlds, settleTicks, "池", tick));

        const uint32_t dam = offsetX + DAM_X;
        const uint64_t total = WaterInX(table, *worlds, 0, size[0]);
        std::printf("  溜まった: 堤の手前 %llu・堤の向こう %llu µmol(全体 %llu)\n",
                    (unsigned long long)WaterInX(table, *worlds, 0, dam),
                    (unsigned long long)WaterInX(table, *worlds, dam, size[0]), (unsigned long long)total);

        for (uint32_t z = pondZ[0]; z < pondZ[1]; ++z) {
            for (uint32_t y = 1; y <= 6; ++y)
                SetCell(*worlds, dam, y, z, MakeCell(table, Kind::Air));
        }

        EXPECT(Run(table, *worlds, drainTicks, "堤を消した池", tick));
        std::printf("  抜けた: 堤の手前 %llu・堤の向こう %llu µmol\n",
                    (unsigned long long)WaterInX(table, *worlds, 0, dam),
                    (unsigned long long)WaterInX(table, *worlds, dam, size[0]));
        EXPECT(WaterInX(table, *worlds, 0, size[0]) == total);
        PrintCost("池");
    }

    // --- 2: ダムを壊す + 段の外で水を半分・2 倍に(欄が物質に付く。T-0656)---
    void TestDamAndMassChange(const BakedReactionTable& table) {
        constexpr std::array<uint32_t, 3> SIZE = {24, 16, 8};
        std::printf("ダムを壊す・段の外で水を半分 / 2 倍(%u × %u × %u)\n", SIZE[0], SIZE[1], SIZE[2]);
        const auto kindOf = [](uint32_t x, uint32_t y, uint32_t) {
            if (y == 0)
                return Kind::Stone;

            return y <= 6 && x < 6 ? Kind::Water : Kind::Air;
        };
        auto worlds = MakeWorlds(table, SIZE, kindOf);
        const uint32_t water = table.SpeciesId("liquid_water");
        const auto scaleWater = [&](uint64_t numerator, uint64_t denominator) {
            for (uint32_t y = 0; y < SIZE[1]; ++y) {
                for (uint32_t z = 0; z < SIZE[2]; ++z) {
                    for (uint32_t x = 0; x < SIZE[0]; ++x) {
                        reaction::RxCell cell = worlds->grid[ProbeCellIndex(x, y, z)];
                        bool touched = false;
                        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                            if (cell.species[i] == water && cell.amounts[i] >= 2) {
                                cell.amounts[i] = cell.amounts[i] * numerator / denominator;
                                touched = true;
                            }
                        }

                        if (touched)
                            SetCell(*worlds, x, y, z, cell);
                    }
                }
            }
        };

        uint64_t tick = 0;
        EXPECT(Run(table, *worlds, 60, "ダム", tick));
        scaleWater(1, 2);
        EXPECT(Run(table, *worlds, 60, "水を半分にしたダム", tick));
        scaleWater(2, 1);
        EXPECT(Run(table, *worlds, 240, "水を戻したダム", tick));
        std::printf("  %llu 刻み: 右の半分 %llu µmol・運動のエネルギー %lld mJ\n", (unsigned long long)tick,
                    (unsigned long long)WaterInX(table, *worlds, SIZE[0] / 2, SIZE[0]),
                    (long long)worlds->liquid.KineticEnergySum(worlds->nest));
        PrintCost("ダム");

        // 保存点に戻して同じ刻みを進めると同じ状態(液体の欄は登録を回る保存点に入る。T-0711)
        const MultiresSaveState save = SaveNestState(worlds->nest);
        const auto runNest = [&] {
            for (uint32_t i = 0; i < 30; ++i) {
                std::vector<uint32_t> changedSlots;
                (void)worlds->liquid.Step(worlds->nest, table, changedSlots);
            }

            return HashWholeNest(worlds->nest);
        };
        const uint64_t first = runNest();
        EXPECT(RestoreNestState(worlds->nest, save));
        EXPECT(runNest() == first);
    }

    // --- 3: 液体の無い世界は何も変えない(64³。頁に広げない)---
    void TestDryWorld(const BakedReactionTable& table) {
        std::printf("液体の無い世界(64³)\n");
        auto worlds = MakeWorlds(table, {PROBE_GRID_SIZE, PROBE_GRID_SIZE, PROBE_GRID_SIZE},
                                 [](uint32_t, uint32_t y, uint32_t) { return y == 0 ? Kind::Stone : Kind::Air; });
        const uint32_t pagesBefore = UsedWorldPages(worlds->nest);
        const uint64_t hashBefore = HashWholeNest(worlds->nest);
        uint64_t tick = 0;
        EXPECT(Run(table, *worlds, 3, "液体の無い世界", tick));
        EXPECT(UsedWorldPages(worlds->nest) == pagesBefore);
        EXPECT(HashWholeNest(worlds->nest) == hashBefore);
        stepCost = {};
    }

}  // namespace

int main() {
    const BakedReactionTable table = MakeTable();
    TestPondAndDrain(table, {16, 16, 16}, 0, {0, 16}, 1800, 1800);
    TestDamAndMassChange(table);
    TestDryWorld(table);
    // 仮の世界の格子いっぱい(区画 = 世界いっぱい)。池は probe_liquid_test と同じ所(x 24〜・z 30〜32)
    TestPondAndDrain(table, {PROBE_GRID_SIZE, PROBE_GRID_SIZE, PROBE_GRID_SIZE}, 24, {30, 33}, 30, 30);

    if (failureCount != 0) {
        std::printf("失敗 %d 件\n", failureCount);
        return 1;
    }

    std::printf("すべて通過\n");
    return 0;
}
