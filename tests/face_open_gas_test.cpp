// face_open_gas_test.cpp — 気体が面の開き具合の欄(face.blocked。T-0937・ADR-0420)を液体と同じ口 FoFaceOpen で読むか(T-0976)。
//
// 確かめること(T-0976 の合格):
//   1. 塞ぐ割合が全部 0 なら今とビット一致: 多重解像度の 1 段の気体(欄を登録して全部 0 / 欄なし)が毎刻み HashWholeNest・報告で同じ。
//      気体の箱(陽解法 StepGas・陰の圧力 StepGasProjection)も塞ぐ割合 0 の表と表なしで HashGas・帳簿が同じ
//   2. 細い溝の場面(face_opening_test と同じ区画・溝。地面の上は石の蓋で閉じる)で、池 A の熱い煙(CO2 の印・高い圧力)が溝 1 に沿って
//      池 B へ運ばれ、溝の外の地面の中(平行な行き止まりの溝 2 を含む)には煙も熱も入らない。欄なし(両側の空きの小さい方だけ)は
//      0.5 m のセルで 2 本の溝のセルがどちらも少し空くので、間の固い地面を抜けて溝 2 へ漏れる
//   3. 保存 0(Σ 保存量 − 帳簿が毎刻み一定。箱は物質・エネルギー・運動量 − 帳簿)・2 回流して同じ要約
//   4. 陽解法と陰の圧力の両方の道: 箱の真ん中の面を欄で塞ぐと、どちらの道でも右の半分の物質が 1 µmol も変わらない。
//      細い隙間(4 面)だけ開けると、どちらの道でも右へ流れる
// 熱の伝導は面の開き具合を読まない(形の塞ぐ割合はセル自身の固い物で、固い物は熱を通す。チケット T-0976 の仮)ので、ここでは回さない。
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

#include "common/cell_view.hlsli"
#include "common/face_opening.hlsli"
#include "gas_projection_scenes.h"
#include "multires_test_scene.h"
#include "reaction_test_table.h"
#include "sim/cell_view.h"
#include "sim/cell_volume.h"
#include "sim/face_opening.h"
#include "sim/gas_projection.h"
#include "sim/gas_reference.h"
#include "sim/multires_fields.h"
#include "sim/multires_gas.h"
#include "sim/multires_nest.h"
#include "sim/multires_state.h"
#include "sim/surface_detail.h"
#include "standard_world_physics.h"

using namespace bicameral;
using namespace bicameral::sim;
using namespace bicameral::surface;
namespace fo = bicameral::face_opening;
namespace gs = bicameral::gas_scenes;

namespace {

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    // --- 場面(face_opening_test と同じ区画 12 × 3 の柱・地面 1.0 m・池 A〔柱 (1, 1)〕と池 B〔柱 (10, 1)〕は 0.5 m の穴)---
    //   溝 1: z 0.65 m の線を池 A の中心の x から池 B の中心の x まで(深さ 10 cm・半径 10 cm)
    //   溝 2: z 1.35 m の線を x 1.75〜4.75 m(行き止まり。溝 1 との間の地面は固い)
    //   区画の地面の上の層(1.0〜1.5 m)は全部石の蓋: 気体の通り道は池 A → 溝 1 → 池 B と溝 2 だけ(上へ逃げない)
    constexpr uint32_t REGION_COLUMNS_X = 12;
    constexpr uint32_t REGION_COLUMNS_Z = 3;
    constexpr uint32_t GROUND_UM = 1000000;
    constexpr uint32_t POND_A_X = 1;
    constexpr uint32_t POND_B_X = 10;
    constexpr uint32_t GROOVE_Z = 1;
    constexpr uint32_t LID_LAYER = 2;
    constexpr uint64_t FULL_STONE = 21000000000;  // µmol(セルを満たす量の少し上)
    constexpr uint32_t AIR_TEMPERATURE = 300000;  // mK
    constexpr uint32_t SMOKE_TEMPERATURE = 450000;
    constexpr uint64_t SEED = 0x976;
    constexpr uint64_t TICKS = 600;  // 10 秒
    constexpr std::array<uint32_t, 3> WORLD_SIZE = {16, 8, 8};

    BakedReactionTable MakeTable() {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        definition.environment = StandardWorldPhysics();
        definition.atmosphere = {
            .points = {{.heightMillimeters = 0,
                        .temperatureMilliKelvin = AIR_TEMPERATURE,
                        .fractions = {{.species = "nitrogen", .partsPerMillion = 790000},
                                      {.species = "oxygen", .partsPerMillion = 210000}}}},
            .surfacePressurePascals = 101325,
        };
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

    void EnsureCarved(SurfaceDetailScene& scene, const SdCarve& carve) {
        if (!CarveSurface(scene, carve))
            std::abort();
    }

    SurfaceDetailScene MakeGrooveScene() {
        SdConfig config{};
        config.seed = 0x5EED0937u;
        config.columnsX = REGION_COLUMNS_X;
        config.columnsZ = REGION_COLUMNS_Z;
        SurfaceDetailScene scene = MakeSurfaceDetailScene(config, GROUND_UM);
        scene.columnHeights[POND_A_X + (GROOVE_Z * REGION_COLUMNS_X)] = SD_CELL_UM;
        scene.columnHeights[POND_B_X + (GROOVE_Z * REGION_COLUMNS_X)] = SD_CELL_UM;

        const uint32_t beginX = (POND_A_X * SD_CELL_UM) + SD_HALF_CELL_UM;
        const uint32_t middleX = (6 * SD_CELL_UM) + SD_HALF_CELL_UM;
        const uint32_t endX = (POND_B_X * SD_CELL_UM) + SD_HALF_CELL_UM;
        EnsureCarved(scene,
                     {.ax = beginX, .az = 650000, .bx = middleX, .bz = 650000, .radiusUm = 100000, .depthUm = 100000});
        EnsureCarved(scene,
                     {.ax = middleX, .az = 650000, .bx = endX, .bz = 650000, .radiusUm = 100000, .depthUm = 100000});
        EnsureCarved(
            scene, {.ax = 1750000, .az = 1350000, .bx = 4750000, .bz = 1350000, .radiusUm = 100000, .depthUm = 100000});

        return scene;
    }

    // --- 世界(多重解像度の 1 段・根のレベル 0。端は全部 wall)---

    enum class FaceMode : uint8_t {
        None,  // 欄を登録しない(今までの読み方: 両側の空きの小さい方)
        Zero,  // 欄を登録し、区画を世界の外に置いて作る(塞ぐ割合が全部 0)
        Shape  // 欄を登録し、区画の形から作る
    };

    struct World {
        MultiresNest nest;
        MultiresGas gas;
        uint32_t field = MULTIRES_NO_FIELD;
    };

    struct CellPlace {
        uint32_t columnX = 0;
        uint32_t columnZ = 0;
        uint32_t layer = 0;
        bool inRegion = false;
    };

    CellPlace PlaceOf(int64_t x, int64_t y, int64_t z) {
        CellPlace place{.columnX = (uint32_t)x, .columnZ = (uint32_t)z, .layer = (uint32_t)y};
        place.inRegion = place.columnX < REGION_COLUMNS_X && place.columnZ < REGION_COLUMNS_Z;

        return place;
    }

    // セルを作る道具(石の量 = 形の固い物の割合。気体 = 層の基準の大気 × 気体の空き。池 A は熱い煙で高い圧力)
    struct CellMaker {
        const BakedReactionTable& table;
        const SurfaceDetailScene& scene;
        std::vector<multires::CvSpeciesPhase> phases;
        std::vector<reaction::RxCell> layers;  // 層ごとの基準の大気

        [[nodiscard]] uint64_t StoneAmount(uint32_t fillQ16) const {
            if (fillQ16 >= SD_ONE_Q16)
                return FULL_STONE;

            const uint32_t stone = table.SpeciesId("stone");

            return ((PROBE_CELL_VOLUME_PICOLITERS * fillQ16) >> 16) / table.speciesMolarVolume[stone];
        }

        [[nodiscard]] uint32_t GasOpen(uint64_t stoneAmount) const {
            const std::array<SpeciesAmount, 1> amounts = {
                SpeciesAmount{.species = table.SpeciesId("stone"), .amount = stoneAmount}};
            const reaction::RxCell solid = MakeReactionCell(table, amounts, (int32_t)AIR_TEMPERATURE);
            const CellViewTable viewTable = {.reaction = table.View(), .phases = phases};

            return multires::MakeCellView(viewTable, solid, 0, 0).gasOpen;
        }

        [[nodiscard]] reaction::RxCell Make(uint32_t fillQ16, uint32_t layer, bool smoke) const {
            const uint64_t stoneAmount = fillQ16 == 0 ? 0 : StoneAmount(fillQ16);
            const uint32_t open = stoneAmount == 0 ? GAS_OPEN_FULL : GasOpen(stoneAmount);
            std::vector<SpeciesAmount> amounts;
            uint64_t airTotal = 0;
            const reaction::RxCell& air = layers[layer];
            for (uint32_t i = 0; i < air.speciesCount; ++i) {
                const uint64_t amount = (air.amounts[i] * open) >> 16;
                airTotal += amount;
                if (amount != 0)
                    amounts.push_back({.species = air.species[i], .amount = amount});
            }

            // 煙: 空気 + 同じ量の CO2 を 450 K で(約 3 倍の圧力。広がった煙が溝の先の池 B まで届く量)
            if (smoke)
                amounts.push_back({.species = table.SpeciesId("carbon_dioxide"), .amount = airTotal});

            if (stoneAmount != 0)
                amounts.push_back({.species = table.SpeciesId("stone"), .amount = stoneAmount});

            std::ranges::sort(amounts, {}, &SpeciesAmount::species);

            return MakeReactionCell(table, amounts, (int32_t)(smoke ? SMOKE_TEMPERATURE : AIR_TEMPERATURE));
        }

        [[nodiscard]] reaction::RxCell At(int64_t x, int64_t y, int64_t z) const {
            const CellPlace place = PlaceOf(x, y, z);
            if (!place.inRegion)
                return place.layer < LID_LAYER ? Make((uint32_t)SD_ONE_Q16, place.layer, false)
                                               : Make(0, place.layer, false);

            if (place.layer == LID_LAYER)
                return Make((uint32_t)SD_ONE_Q16, place.layer, false);

            if (place.layer > LID_LAYER)
                return Make(0, place.layer, false);

            const SdCellOpenness open = SdCellOpennessAt(scene.Data(), scene.config, SD_MODE_LIST,
                                                         (uint32_t)(x * SD_CELL_UM), (uint32_t)(z * SD_CELL_UM),
                                                         y * (int64_t)SD_CELL_UM, SD_CELL_UM);
            const bool pondA = place.columnX == POND_A_X && place.columnZ == GROOVE_Z && place.layer == 1;

            return Make(open.fillQ16, place.layer, pondA);
        }
    };

    std::unique_ptr<World> MakeWorld(const BakedReactionTable& table, const SurfaceDetailScene& scene, FaceMode mode) {
        const uint32_t blocks = (WORLD_SIZE[0] / 8) * (WORLD_SIZE[1] / 8) * (WORLD_SIZE[2] / 8);
        MultiresNest nest = MakeMultiresNest(test::MakeMultiresCapacity(table, blocks, 0, 1));
        nest.edges.world = {.rootLevel = 0,
                            .lowX = 0,
                            .lowY = 0,
                            .lowZ = 0,
                            .highX = WORLD_SIZE[0],
                            .highY = WORLD_SIZE[1],
                            .highZ = WORLD_SIZE[2],
                            .top = multires::WE_WALL,
                            .bottom = multires::WE_WALL,
                            .sides = multires::WE_WALL};
        const uint32_t field = mode == FaceMode::None ? MULTIRES_NO_FIELD : RegisterFaceBlockedField(nest);
        EXPECT(mode == FaceMode::None || field != MULTIRES_NO_FIELD);
        auto gas = MultiresGas::Create(nest, table, {.randomSeed = SEED, .floorHeightMillimeters = 0});
        if (!gas) {
            std::printf("気体を作れない: %s\n", gas.error().c_str());
            std::exit(1);
        }

        auto world = std::make_unique<World>(World{.nest = std::move(nest), .gas = *gas, .field = field});
        CellMaker maker{.table = table, .scene = scene, .phases = MakeSpeciesPhases(table), .layers = {}};
        for (uint32_t layer = 0; layer < WORLD_SIZE[1]; ++layer)
            maker.layers.push_back(world->gas.ReferenceCell(world->nest, table, layer));

        for (uint32_t bz = 0; bz < WORLD_SIZE[2]; bz += 8) {
            for (uint32_t by = 0; by < WORLD_SIZE[1]; by += 8) {
                for (uint32_t bx = 0; bx < WORLD_SIZE[0]; bx += 8) {
                    std::vector<reaction::RxCell> cells(multires::MR_BLOCK_CELLS);
                    for (uint32_t index = 0; index < cells.size(); ++index)
                        cells[index] = maker.At(bx + multires::MrCellX(index), by + multires::MrCellY(index),
                                                bz + multires::MrCellZ(index));

                    PlaceRootBlock(world->nest, bx, by, bz, cells);
                }
            }
        }

        if (mode != FaceMode::None) {
            // Zero は区画を世界の外(x 1000 セル先)に置く = 全部の面が開き具合 1
            const SurfacePlacement placement = {.originX = mode == FaceMode::Zero ? 1000 : 0};
            EXPECT(DeriveFaceBlocked(world->nest, world->field, scene, SD_MODE_LIST, placement));
        }

        return world;
    }

    // --- 煙と熱の行き先 ---

    struct SmokeResult {
        uint64_t pondB = 0;   // 池 B の CO2(µmol)
        uint64_t groove = 0;  // 溝 1 の列(柱 x 2〜9・z 1)の地面の層の CO2
        uint64_t beside = 0;  // 地面の層のうち溝 1 の列の外(溝 2 を含む)の CO2
        uint64_t total = 0;
        // 熱(µJ)= 見立ての熱(エネルギー − 化学のエネルギー)+ 運動のエネルギーの欄。CO2 の化学のエネルギーは数えない
        int64_t besideHeat = 0;  // 溝 1 の列の外の地面の層
        int64_t pondBHeat = 0;
    };

    SmokeResult MeasureSmoke(const BakedReactionTable& table, const World& world) {
        const uint32_t smoke = table.SpeciesId("carbon_dioxide");
        const std::vector<multires::CvSpeciesPhase> phases = MakeSpeciesPhases(table);
        const CellViewTable viewTable = {.reaction = table.View(), .phases = phases};
        const uint32_t kinetic = world.gas.Fields().kinetic;
        SmokeResult result;
        for (uint32_t slot = 0; slot < world.nest.capacity.worldBlocks; ++slot) {
            const multires::MrBlock& block = world.nest.blocks[slot];
            if (block.kind != multires::MR_BLOCK_REAL)
                continue;

            for (uint32_t index = 0; index < multires::MR_BLOCK_CELLS; ++index) {
                const reaction::RxCell cell = LoadNestCell(world.nest, slot, index);
                uint64_t amount = 0;
                for (uint32_t i = 0; i < cell.speciesCount; ++i)
                    amount += cell.species[i] == smoke ? cell.amounts[i] : 0;

                const CellPlace place = PlaceOf(block.originX + multires::MrCellX(index),
                                                block.originY + multires::MrCellY(index),
                                                block.originZ + multires::MrCellZ(index));
                result.total += amount;
                if (!place.inRegion || place.layer != 1)
                    continue;

                const int64_t heat = multires::MakeCellView(viewTable, cell, 0, 0).heat +
                                     (LoadNestField(world.nest, kinetic, slot, index, 0) * 1000);
                if (place.columnZ == GROOVE_Z && place.columnX == POND_B_X) {
                    result.pondB += amount;
                    result.pondBHeat += heat;
                } else if (place.columnZ == GROOVE_Z && place.columnX > POND_A_X && place.columnX < POND_B_X)
                    result.groove += amount;
                else if (place.columnZ != GROOVE_Z) {
                    result.beside += amount;
                    result.besideHeat += heat;
                }
            }
        }

        return result;
    }

    double Share(uint64_t part, uint64_t total) {
        return total == 0 ? 0.0 : (double)part / (double)total;
    }

    // --- 刻む(Σ 保存量 − 帳簿を毎刻み確かめ、毎刻みの要約を束ねる)---

    struct RunResult {
        uint64_t digest = 0;
        uint32_t drifts = 0;  // Σ 保存量 − 帳簿が変わった刻み
        std::vector<uint64_t> hashes;
        std::vector<MultiresGasStepReport> reports;
    };

    RunResult Run(const BakedReactionTable& table, World& world, uint64_t ticks) {
        RunResult result;
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            const ConservedTotals before = ComputeConservedTotals(world.nest, table, 0);
            std::vector<uint32_t> changedSlots;
            result.reports.push_back(world.gas.Step(world.nest, table, tick, changedSlots));
            if (ComputeConservedTotals(world.nest, table, 0) != before)
                ++result.drifts;

            const uint64_t hash = HashWholeNest(world.nest);
            result.hashes.push_back(hash);
            result.digest = fx::FxHashCombine(result.digest, hash);
        }

        return result;
    }

    bool SameReport(const MultiresGasStepReport& a, const MultiresGasStepReport& b) {
        return a.gasSpecies == b.gasSpecies && a.skippedCellOverflow == b.skippedCellOverflow &&
               a.changedCells == b.changedCells && a.restedCells == b.restedCells &&
               a.energyLimitedFaces == b.energyLimitedFaces && a.blockedCells == b.blockedCells &&
               a.partialCells == b.partialCells;
    }

    // --- 2・3: 溝に沿って煙と熱が運ばれ、溝の外の地面の中へ入らない ---

    void TestGroove(const BakedReactionTable& table, const SurfaceDetailScene& scene) {
        std::printf("2. 溝に沿って煙と熱が運ばれる(セル 500 mm・%llu 刻み)\n", (unsigned long long)TICKS);
        auto shaped = MakeWorld(table, scene, FaceMode::Shape);
        auto cellsOnly = MakeWorld(table, scene, FaceMode::None);
        const SmokeResult start = MeasureSmoke(table, *shaped);
        const RunResult run = Run(table, *shaped, TICKS);
        const RunResult old = Run(table, *cellsOnly, TICKS);
        const SmokeResult a = MeasureSmoke(table, *shaped);
        const SmokeResult b = MeasureSmoke(table, *cellsOnly);
        for (const auto& [name, result] : {std::pair{"欄を読む", a}, std::pair{"空きだけ(前)", b}}) {
            std::printf(
                "  %s: 煙 池 B %.4f・溝 %.4f・溝の外の地面 %.6f / 池 B の熱 %+lld µJ・溝の外の地面の熱 %+lld µJ\n",
                name, Share(result.pondB, result.total), Share(result.groove, result.total),
                Share(result.beside, result.total), (long long)(result.pondBHeat - start.pondBHeat),
                (long long)(result.besideHeat - start.besideHeat));
        }

        EXPECT(run.drifts == 0 && old.drifts == 0);
        EXPECT(a.total == start.total && start.total != 0);
        EXPECT(a.pondB != 0 && a.groove != 0);     // 溝に沿って池 B まで運ばれる
        EXPECT(a.pondBHeat > start.pondBHeat);     // 熱い煙の熱も池 B へ
        EXPECT(a.beside == 0);                     // 溝の外の地面の中へ煙は入らない
        EXPECT(a.besideHeat == start.besideHeat);  // 熱も入らない(気体が運ぶ熱。伝導はこの試験で回さない)
        EXPECT(b.beside != 0);                     // 欄なしは 0.5 m のセルで溝 2 へ漏れる(この欄が要る理由)

        // 決定的: もう 1 回作って流すと同じ要約
        auto again = MakeWorld(table, scene, FaceMode::Shape);
        EXPECT(Run(table, *again, TICKS).digest == run.digest);
    }

    // --- 1: 塞ぐ割合が全部 0 なら欄のない世界とビット一致 ---

    void TestAllOpen(const BakedReactionTable& table, const SurfaceDetailScene& scene) {
        std::printf("1. 塞ぐ割合 0 ばかり = 欄なしとビット一致(多重解像度の気体・%llu 刻み)\n",
                    (unsigned long long)TICKS);
        auto zero = MakeWorld(table, scene, FaceMode::Zero);
        auto none = MakeWorld(table, scene, FaceMode::None);
        const RunResult a = Run(table, *zero, TICKS);
        const RunResult b = Run(table, *none, TICKS);
        uint32_t differences = 0;
        for (uint64_t i = 0; i < a.hashes.size(); ++i)
            differences += a.hashes[i] == b.hashes[i] && SameReport(a.reports[i], b.reports[i]) ? 0 : 1;

        std::printf("  違った刻み %u・要約の束 0x%016llx\n", differences, (unsigned long long)a.digest);
        EXPECT(differences == 0 && a.drifts == 0);
        EXPECT(HashTruthLeaves(zero->nest) == HashTruthLeaves(none->nest));
    }

    // --- 1・4: 気体の箱の陽解法と陰の圧力 ---

    enum class BoxPath : uint8_t { Explicit, Implicit };

    // 箱(一辺 8・重さあり・閉じた)の左の半分が 2% 高い圧力。真ん中の面(x 3 と 4 の間)の塞ぎ方
    enum class Plate : uint8_t {
        NoTable,   // 表なし(空きも読まない。今までの道)
        Zero,      // 空き 1 の表 + 塞ぐ割合 0 の表
        ZeroOpen,  // 空き 1 の表だけ(塞ぐ割合の表なし)
        Closed,    // 真ん中の面を全部塞ぐ
        Slit       // 真ん中の面を塞ぎ、4 面(y 3〜4・z 3〜4)だけ開ける
    };

    constexpr uint32_t BOX_EDGE = 8;
    constexpr uint32_t BOX_TICKS = 60;

    sim::GasBox MakePlateBox(Plate plate) {
        sim::GasBox box = gs::MakeAirBox(BOX_EDGE);
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            if (index % BOX_EDGE < BOX_EDGE / 2)
                gs::SetTemperature(box, index, gs::AIR_TEMPERATURE, 102);
        }

        if (plate == Plate::NoTable)
            return box;

        box.openQ16.assign(box.cells.size(), GAS_OPEN_FULL);
        if (plate == Plate::ZeroOpen)
            return box;

        for (std::vector<uint32_t>& axis : box.faceBlockedQ16)
            axis.assign(box.cells.size(), 0);

        if (plate == Plate::Zero)
            return box;

        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const uint32_t x = index % BOX_EDGE;
            const uint32_t y = (index / BOX_EDGE) % BOX_EDGE;
            const uint32_t z = index / (BOX_EDGE * BOX_EDGE);
            const bool slit = plate == Plate::Slit && (y == 3 || y == 4) && (z == 3 || z == 4);
            if (x == (BOX_EDGE / 2) - 1 && !slit)
                box.faceBlockedQ16[0][index] = fo::FO_OPEN_FULL;
        }

        return box;
    }

    struct BoxRun {
        uint64_t hash = 0;
        bool conserved = true;
        uint64_t rightBefore = 0;  // 右の半分の物質量(µmol)
        uint64_t rightAfter = 0;
        sim::GasLedger ledger;
    };

    uint64_t RightAmount(const sim::GasBox& box) {
        uint64_t sum = 0;
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            if (index % BOX_EDGE < BOX_EDGE / 2)
                continue;

            for (uint32_t s = 0; s < box.config.speciesCount; ++s)
                sum += box.cells[index].amounts[s];
        }

        return sum;
    }

    BoxRun RunBox(Plate plate, BoxPath path) {
        sim::GasBox box = MakePlateBox(plate);
        const sim::GasTotals totals = sim::SumGas(box);
        sim::GasProjection state;
        sim::GasProjectionOptions options;
        options.pressure.soundSpeedMmPerS = gs::REAL_SOUND;
        options.fixedCycles = true;
        options.solver.cycles = 2;
        options.outerIterations = 1;
        options.equilibrium = true;
        BoxRun run{.rightBefore = RightAmount(box)};
        for (uint32_t tick = 0; tick < BOX_TICKS; ++tick) {
            if (path == BoxPath::Explicit)
                sim::StepGas(box);
            else
                (void)sim::StepGasProjection(box, state, options);

            const sim::GasTotals now = sim::SumGas(box);
            bool ok = now.amounts == totals.amounts && now.energy == totals.energy + box.ledger.energy;
            for (uint32_t axis = 0; axis < sim::GAS_AXES; ++axis)
                ok = ok && now.momentum[axis] == totals.momentum[axis] + box.ledger.momentum[axis];

            run.conserved = run.conserved && ok;
        }

        run.hash = sim::HashGas(box);
        run.rightAfter = RightAmount(box);
        run.ledger = box.ledger;

        return run;
    }

    bool SameLedger(const sim::GasLedger& a, const sim::GasLedger& b) {
        return a.energy == b.energy && a.momentum == b.momentum && a.amounts == b.amounts &&
               a.wallMomentum == b.wallMomentum;
    }

    void TestBoxPaths() {
        std::printf("4. 気体の箱の陽解法と陰の圧力(一辺 %u・%u 刻み)\n", BOX_EDGE, BOX_TICKS);
        for (const BoxPath path : {BoxPath::Explicit, BoxPath::Implicit}) {
            const char* name = path == BoxPath::Explicit ? "陽解法" : "陰の圧力";
            const BoxRun zero = RunBox(Plate::Zero, path);
            const BoxRun zeroOpen = RunBox(Plate::ZeroOpen, path);
            const BoxRun closed = RunBox(Plate::Closed, path);
            const BoxRun slit = RunBox(Plate::Slit, path);
            const BoxRun again = RunBox(Plate::Slit, path);
            std::printf("  %s: 右の半分の変化 塞いだ %+lld・隙間 %+lld・塞がない %+lld µmol\n", name,
                        (long long)(closed.rightAfter - closed.rightBefore),
                        (long long)(slit.rightAfter - slit.rightBefore),
                        (long long)(zero.rightAfter - zero.rightBefore));

            // 1: 塞ぐ割合 0 の表は表なしと同じ(空き 1 の表どうし)
            EXPECT(zero.hash == zeroOpen.hash && SameLedger(zero.ledger, zeroOpen.ledger));
            // 3・4: 保存・塞いだ面は通さない・隙間は通す・決定的
            EXPECT(zero.conserved && closed.conserved && slit.conserved);
            EXPECT(closed.rightAfter == closed.rightBefore);
            EXPECT(slit.rightAfter > slit.rightBefore);
            EXPECT(again.hash == slit.hash);
        }

        // 表なし(空きを読まない箱)と空き 1 の表は空きを読む式の数え方が違う(T-0593 から)ので比べない。保存だけ見る
        EXPECT(RunBox(Plate::NoTable, BoxPath::Explicit).conserved);
    }

}  // namespace

int main() {
    TestBoxPaths();
    const BakedReactionTable table = MakeTable();
    const SurfaceDetailScene scene = MakeGrooveScene();
    TestAllOpen(table, scene);
    TestGroove(table, scene);
    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("すべて通った\n");
    return 0;
}
