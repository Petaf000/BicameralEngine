// face_opening_test.cpp — セルの面の開き具合の欄(T-0937。21 §2.7 の塞ぐ面・23 §7・D-615)を、多重解像度の 1 段の液体が直接読むか。
//
// 確かめること(T-0937 の合格):
//   1. セルより細い 10 cm の溝に沿って水が流れ、溝の外の地面の中には流れない。場面は R-T1 (4)(T-0879)の池 A → 溝 → 池 B に、
//      平行な行き止まりの溝(隣の列のセルの中。間の地面は固い)を足したもの。セルの空きの小さい方だけ(欄なし。今までの読み方)だと、
//      0.5 m のセルでは 2 本の溝のセルがどちらも少し空いているので、間の地面を抜けて隣の溝へ漏れる。欄を読むと漏れない
//   2. 開き具合 1 の面ばかり(塞ぐ割合 0)なら、欄を登録しない世界と毎刻みビット一致(状態の要約・報告・Σ 保存量 − 帳簿)
//   3. Σ 保存量 − 帳簿が毎刻み一定・2 回流して同じ要約・保存点から戻して欄を作り直すと同じ要約・0.5 m と 0.25 m のセルで
//      溝に沿って運ばれる量が同じ向き(割合を記録)
//   4. 粒が穴を通る口(S8。FoGrainFaceOpen)の値
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

#include "common/face_opening.hlsli"
#include "multires_test_scene.h"
#include "reaction_test_table.h"
#include "sim/cell_volume.h"
#include "sim/face_opening.h"
#include "sim/multires_fields.h"
#include "sim/multires_liquid.h"
#include "sim/multires_nest.h"
#include "sim/multires_state.h"
#include "sim/surface_detail.h"
#include "standard_world_physics.h"

using namespace bicameral;
using namespace bicameral::sim;
using namespace bicameral::surface;
namespace fo = bicameral::face_opening;

namespace {

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    // --- 場面(区画 12 × 3 の柱 = 6 m × 1.5 m・地面 1.0 m。池 A〔柱 (1, 1)〕と池 B〔柱 (10, 1)〕は 0.5 m の穴)---
    //   溝 1: z 0.65 m の線を池 A の中心の x から池 B の中心の x まで(深さ 10 cm・半径 10 cm。柱 6 の中心で折れ線に分ける)
    //   溝 2: z 1.35 m の線を x 1.75〜4.75 m(行き止まり。池につながらない。溝 1 との間の地面〔z 0.75〜1.25 m〕は固い)
    //   溝 2 の列(柱の z 2)の地面の上の層(1.0〜1.5 m)は石の蓋: 池 A から溢れて地面の上を広がった水が、上から溝 2 へ落ちる道を塞ぐ
    //   (上から落ちるのは正しい流れ。この試験は「地面の中を抜けて」隣の溝へ行かないことを見る)
    //   区画の外(世界の残り)は地面 1.0 m の平らな石
    constexpr uint32_t REGION_COLUMNS_X = 12;
    constexpr uint32_t REGION_COLUMNS_Z = 3;
    constexpr uint32_t GROUND_UM = 1000000;
    constexpr uint32_t POND_A_X = 1;
    constexpr uint32_t POND_B_X = 10;
    constexpr uint32_t GROOVE_Z = 1;
    constexpr uint32_t LID_Z = 2;                 // 溝 2 の列の地面の上の石の蓋
    constexpr uint64_t FULL_WATER = 6900000000;   // µmol(0.5 m 角の水の少し下。probe_liquid_test と同じ)
    constexpr uint64_t FULL_STONE = 21000000000;  // µmol(セルを満たす量の少し上)
    constexpr uint64_t NITROGEN = 4000000;
    constexpr uint64_t OXYGEN = 1000000;
    constexpr int32_t TEMPERATURE = 300000;
    constexpr uint64_t TICKS = 1800;  // 30 秒

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

    // セルの真実(D-615: 固い物の量)を形の固い物の割合から作る。液体は残りの空きに入る(T-0612)
    reaction::RxCell MakeGroundCell(const BakedReactionTable& table, uint32_t fillQ16, uint64_t water) {
        const uint32_t stone = table.SpeciesId("stone");
        uint64_t stoneAmount = FULL_STONE;
        if (fillQ16 < SD_ONE_Q16)
            stoneAmount = ((PROBE_CELL_VOLUME_PICOLITERS * fillQ16) >> 16) / table.speciesMolarVolume[stone];

        std::vector<SpeciesAmount> amounts = {{.species = table.SpeciesId("nitrogen"), .amount = NITROGEN},
                                              {.species = table.SpeciesId("oxygen"), .amount = OXYGEN}};
        if (stoneAmount != 0)
            amounts.push_back({.species = stone, .amount = stoneAmount});

        if (water != 0)
            amounts.push_back({.species = table.SpeciesId("liquid_water"), .amount = water});

        std::ranges::sort(amounts, {}, &SpeciesAmount::species);

        return MakeReactionCell(table, amounts, TEMPERATURE);
    }

    // --- 世界(多重解像度の 1 段。根のレベル level = 0〔0.5 m〕/ 1〔0.25 m〕)---

    enum class FaceMode : uint8_t {
        None,  // 欄を登録しない(今までの読み方: 両側のセルの空きの小さい方)
        Zero,  // 欄を登録し、区画を世界の外に置いて作る(塞ぐ割合が全部 0)
        Shape  // 欄を登録し、区画の形から作る
    };

    struct World {
        int32_t level = 0;
        uint32_t edgeUm = 0;
        std::array<uint32_t, 3> size{};  // セル(8 の倍数。y が上)
        MultiresNest nest;
        MultiresLiquid liquid;
        uint32_t field = MULTIRES_NO_FIELD;
    };

    // 区画の柱 (x, z) と層(0 = 地面の下・1 = 地面の層 0.5〜1.0 m・2 以上 = 地面の上)
    struct CellPlace {
        uint32_t columnX = 0;
        uint32_t columnZ = 0;
        uint32_t layer = 0;
        bool inRegion = false;
    };

    CellPlace PlaceOf(const World& world, int64_t x, int64_t y, int64_t z) {
        const uint32_t shift = (uint32_t)world.level;
        CellPlace place{
            .columnX = (uint32_t)(x >> shift), .columnZ = (uint32_t)(z >> shift), .layer = (uint32_t)(y >> shift)};
        place.inRegion = place.columnX < REGION_COLUMNS_X && place.columnZ < REGION_COLUMNS_Z;

        return place;
    }

    reaction::RxCell CellAt(const BakedReactionTable& table, const SurfaceDetailScene& scene, const World& world,
                            int64_t x, int64_t y, int64_t z) {
        const CellPlace place = PlaceOf(world, x, y, z);
        const int64_t bottomUm = y * (int64_t)world.edgeUm;
        if (!place.inRegion)
            return MakeGroundCell(table, bottomUm < (int64_t)GROUND_UM ? (uint32_t)SD_ONE_Q16 : 0u, 0);

        const SdCellOpenness open = SdCellOpennessAt(scene.Data(), scene.config, SD_MODE_LIST,
                                                     (uint32_t)(x * world.edgeUm), (uint32_t)(z * world.edgeUm),
                                                     bottomUm, world.edgeUm);
        if (place.layer == 2 && place.columnZ == LID_Z)
            return MakeGroundCell(table, (uint32_t)SD_ONE_Q16, 0);

        const bool pondA = place.columnX == POND_A_X && place.columnZ == GROOVE_Z && place.layer == 1;

        return MakeGroundCell(table, open.fillQ16, pondA ? FULL_WATER : 0);
    }

    std::unique_ptr<World> MakeWorld(const BakedReactionTable& table, const SurfaceDetailScene& scene, int32_t level,
                                     FaceMode mode) {
        const uint32_t perColumn = 1u << (uint32_t)level;
        const uint32_t sizeX = ((REGION_COLUMNS_X * perColumn) + 7) & ~7u;
        const std::array<uint32_t, 3> size = {sizeX, 8, 8};
        const uint32_t blocks = (size[0] / 8) * (size[1] / 8) * (size[2] / 8);
        MultiresCapacity capacity = test::MakeMultiresCapacity(table, blocks, 0, 1);
        capacity.rootLevel = level;
        MultiresNest nest = MakeMultiresNest(capacity);
        const uint32_t field = mode == FaceMode::None ? MULTIRES_NO_FIELD : RegisterFaceBlockedField(nest);
        EXPECT(mode == FaceMode::None || field != MULTIRES_NO_FIELD);
        auto liquid = MultiresLiquid::Create(nest, table, {});
        if (!liquid) {
            std::printf("液体を作れない: %s\n", liquid.error().c_str());
            std::exit(1);
        }

        auto world = std::make_unique<World>(World{.level = level,
                                                   .edgeUm = SD_CELL_UM >> (uint32_t)level,
                                                   .size = size,
                                                   .nest = std::move(nest),
                                                   .liquid = *liquid,
                                                   .field = field});
        for (uint32_t bz = 0; bz < size[2]; bz += 8) {
            for (uint32_t by = 0; by < size[1]; by += 8) {
                for (uint32_t bx = 0; bx < size[0]; bx += 8) {
                    std::vector<reaction::RxCell> cells(multires::MR_BLOCK_CELLS);
                    for (uint32_t index = 0; index < cells.size(); ++index)
                        cells[index] = CellAt(table, scene, *world, bx + multires::MrCellX(index),
                                              by + multires::MrCellY(index), bz + multires::MrCellZ(index));

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

    // 世界のセル (x, y, z) の枠と番号
    std::array<uint32_t, 2> SlotOf(const World& world, uint32_t x, uint32_t y, uint32_t z) {
        const uint32_t slot = LookupBlock(world.nest, world.level, x & ~7u, y & ~7u, z & ~7u);

        return {slot, multires::MrCellIndex(x & 7u, y & 7u, z & 7u)};
    }

    // --- 水の行き先 ---

    struct WaterResult {
        uint64_t pondB = 0;         // 池 B の柱
        uint64_t groove = 0;        // 溝 1 の列(柱 x 2〜9・z 1)の地面の層
        uint64_t besideGround = 0;  // 地面の層のうち溝 1 の列の外(溝 2 を含む)
        uint64_t parallel = 0;      // そのうち溝 2 の柱(z 2・x 3〜9)
        uint64_t above = 0;         // 地面の上の層
        uint64_t total = 0;         // 全部(レベルの単位)
    };

    WaterResult MeasureWater(const BakedReactionTable& table, const World& world) {
        const uint32_t water = table.SpeciesId("liquid_water");
        WaterResult result;
        for (uint32_t slot = 0; slot < world.nest.capacity.worldBlocks; ++slot) {
            const multires::MrBlock& block = world.nest.blocks[slot];
            if (block.kind != multires::MR_BLOCK_REAL)
                continue;

            for (uint32_t index = 0; index < multires::MR_BLOCK_CELLS; ++index) {
                const reaction::RxCell cell = LoadNestCell(world.nest, slot, index);
                uint64_t amount = 0;
                for (uint32_t i = 0; i < cell.speciesCount; ++i)
                    amount += cell.species[i] == water ? cell.amounts[i] : 0;

                if (amount == 0)
                    continue;

                const CellPlace place = PlaceOf(world, block.originX + multires::MrCellX(index),
                                                block.originY + multires::MrCellY(index),
                                                block.originZ + multires::MrCellZ(index));
                result.total += amount;
                if (place.layer >= 2) {
                    result.above += amount;
                    continue;
                }

                if (place.columnZ == GROOVE_Z && place.columnX == POND_B_X)
                    result.pondB += amount;
                else if (place.columnZ == GROOVE_Z && place.columnX > POND_A_X && place.columnX < POND_B_X)
                    result.groove += amount;
                else if (place.columnZ != GROOVE_Z && place.layer == 1) {
                    result.besideGround += amount;
                    if (place.columnZ == 2 && place.columnX >= 3 && place.columnX <= 9)
                        result.parallel += amount;
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
        std::vector<MultiresLiquidStepReport> reports;
    };

    void Step(const BakedReactionTable& table, World& world, uint64_t tick, RunResult& result) {
        const ConservedTotals before = ComputeConservedTotals(world.nest, table, world.level);
        std::vector<uint32_t> changedSlots;
        result.reports.push_back(world.liquid.Step(world.nest, table, tick, changedSlots));
        if (ComputeConservedTotals(world.nest, table, world.level) != before)
            ++result.drifts;

        const uint64_t hash = HashWholeNest(world.nest);
        result.hashes.push_back(hash);
        result.digest = fx::FxHashCombine(result.digest, hash);
    }

    RunResult Run(const BakedReactionTable& table, World& world, uint64_t begin, uint64_t end) {
        RunResult result;
        for (uint64_t tick = begin; tick < end; ++tick)
            Step(table, world, tick, result);

        return result;
    }

    bool SameReport(const MultiresLiquidStepReport& a, const MultiresLiquidStepReport& b) {
        return a.wall == b.wall && a.gravity == b.gravity && a.friction == b.friction && a.dropped == b.dropped &&
               a.capped == b.capped && a.heat == b.heat && a.changedCells == b.changedCells &&
               a.limitedCells == b.limitedCells && a.movedMilligrams == b.movedMilligrams && a.edge == b.edge &&
               a.edgeMilligrams == b.edgeMilligrams;
    }

    // --- 1 と 3: 溝に沿って流れ、隣の溝へ漏れない(0.5 m と 0.25 m)---

    WaterResult TestGroove(const BakedReactionTable& table, const SurfaceDetailScene& scene, int32_t level) {
        std::printf("1. 溝に沿って流れる(セル %u mm・30 秒)\n", (SD_CELL_UM >> (uint32_t)level) / 1000);
        auto shaped = MakeWorld(table, scene, level, FaceMode::Shape);
        auto cellsOnly = MakeWorld(table, scene, level, FaceMode::None);

        // 溝 1 の列の地面の層のセル(柱 5)の + x の面と、溝 1 と溝 2 の間の + z の面(柱 5・溝 1 の側)
        const uint32_t perColumn = 1u << (uint32_t)level;
        const uint32_t grooveCellZ = (650000 / shaped->edgeUm);
        const auto [slot, index] = SlotOf(*shaped, (5 * perColumn) + perColumn - 1, (2 * perColumn) - 1, grooveCellZ);
        const uint32_t openX = fo::FoOpenLimit(LoadFaceBlockedQ16(shaped->nest, shaped->field, slot, index, 0));
        const uint32_t openZ = fo::FoOpenLimit(LoadFaceBlockedQ16(shaped->nest, shaped->field, slot, index, 2));
        std::printf("  溝のセルの + x の面の開き %.4f・+ z の面の開き %.4f\n", (double)openX / 65536.0,
                    (double)openZ / 65536.0);
        EXPECT(openX != 0 && openX < fo::FO_OPEN_FULL);

        const RunResult run = Run(table, *shaped, 0, TICKS);
        const RunResult old = Run(table, *cellsOnly, 0, TICKS);
        const WaterResult a = MeasureWater(table, *shaped);
        const WaterResult b = MeasureWater(table, *cellsOnly);
        for (const auto& [name, result] : {std::pair{"欄を読む", a}, std::pair{"セルの空きだけ(前)", b}}) {
            std::printf("  %s: 池 B %.4f・溝 %.4f・溝の外の地面 %.4f(隣の溝 %.4f)・地面の上 %.4f\n", name,
                        Share(result.pondB, result.total), Share(result.groove, result.total),
                        Share(result.besideGround, result.total), Share(result.parallel, result.total),
                        Share(result.above, result.total));
        }

        EXPECT(run.drifts == 0 && old.drifts == 0);
        EXPECT(a.total == (uint64_t)FULL_WATER << (3 * (uint32_t)level));
        EXPECT(a.pondB * 100 >= a.total);  // 30 秒で 1 % 以上が池 B へ
        EXPECT(a.besideGround == 0);       // 溝の外の地面の中へは流れない
        if (level == 0)
            EXPECT(b.parallel != 0);  // 欄なしは 0.5 m のセルで隣の溝へ漏れる(この欄が要る理由)

        // 決定的: もう 1 回作って流すと同じ要約
        auto again = MakeWorld(table, scene, level, FaceMode::Shape);
        EXPECT(Run(table, *again, 0, TICKS).digest == run.digest);

        return a;
    }

    // --- 2: 開き具合 1 の面ばかりなら、欄のない世界とビット一致 ---

    void TestAllOpen(const BakedReactionTable& table, const SurfaceDetailScene& scene) {
        std::printf("2. 開き具合 1 の面ばかり = 欄なしとビット一致(600 刻み)\n");
        for (const int32_t level : {0, 1}) {
            auto zero = MakeWorld(table, scene, level, FaceMode::Zero);
            auto none = MakeWorld(table, scene, level, FaceMode::None);
            const RunResult a = Run(table, *zero, 0, 600);
            const RunResult b = Run(table, *none, 0, 600);
            uint32_t differences = 0;
            for (uint64_t i = 0; i < a.hashes.size(); ++i)
                differences += a.hashes[i] == b.hashes[i] && SameReport(a.reports[i], b.reports[i]) ? 0 : 1;

            std::printf("  レベル %d: 違った刻み %u・要約の束 0x%016llx\n", level, differences,
                        (unsigned long long)a.digest);
            EXPECT(differences == 0 && a.drifts == 0);
            EXPECT(HashTruthLeaves(zero->nest) == HashTruthLeaves(none->nest));
        }
    }

    // --- 3: 保存点から戻すと導かれる値(欄)は 0。形から作り直せば続けた場合と同じ要約 ---

    void TestSavePoint(const BakedReactionTable& table, const SurfaceDetailScene& scene) {
        std::printf("3. 保存点から戻して欄を作り直す\n");
        auto world = MakeWorld(table, scene, 0, FaceMode::Shape);
        (void)Run(table, *world, 0, 300);
        const MultiresSaveState save = SaveNestState(world->nest);
        const RunResult continued = Run(table, *world, 300, 360);

        EXPECT(RestoreNestState(world->nest, save));
        const auto [slot, index] = SlotOf(*world, 5, 1, 1);
        EXPECT(LoadFaceBlockedQ16(world->nest, world->field, slot, index, 2) == 0);  // 導かれる値は保存点に入らない
        EXPECT(DeriveFaceBlocked(world->nest, world->field, scene, SD_MODE_LIST, {}));
        const RunResult restored = Run(table, *world, 300, 360);
        std::printf("  続けた 0x%016llx・戻して作り直した 0x%016llx\n", (unsigned long long)continued.digest,
                    (unsigned long long)restored.digest);
        EXPECT(continued.digest == restored.digest);
    }

    // --- 4: 粒が穴を通る口(S8。1 周目は口だけ)---

    void TestGrainPort() {
        std::printf("4. 粒が穴を通る口\n");
        EXPECT(fo::FoGrainFaceOpen(30000, 1000, 300, 50) == 30000);  // 300 µm の粒は 1 mm の穴の半分より小さい = 通る
        EXPECT(fo::FoGrainFaceOpen(30000, 1000, 600, 50) == 0);      // 600 µm は半分より大きい = 詰まる
        EXPECT(fo::FoGrainFaceOpen(30000, 0, 600, 50) == 30000);     // 穴の大きさを持たない面は制限しない
        EXPECT(fo::FoGrainFaceOpen(30000, 1000, 600, 0) == 30000);   // 比を書いていない世界は制限しない
        EXPECT(fo::FoFaceOpen(40000, 0) == 40000 && fo::FoFaceOpen(40000, 30000) == 35536);
    }

}  // namespace

int main() {
    const BakedReactionTable table = MakeTable();
    const SurfaceDetailScene scene = MakeGrooveScene();
    const WaterResult coarse = TestGroove(table, scene, 0);
    const WaterResult fine = TestGroove(table, scene, 1);
    std::printf("  0.5 m と 0.25 m: 池 B %.4f / %.4f・溝 %.4f / %.4f\n", Share(coarse.pondB, coarse.total),
                Share(fine.pondB, fine.total), Share(coarse.groove, coarse.total), Share(fine.groove, fine.total));
    EXPECT(coarse.pondB != 0 && fine.pondB != 0);  // 同じ向き(溝の先の池 B へ運ばれる)

    TestAllOpen(table, scene);
    TestSavePoint(table, scene);
    TestGrainPort();
    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("すべて通った\n");
    return 0;
}
