// surface_detail_test.cpp — 表面の細かい形(研究 R-T1・T-0879。23 §7・§10)の CPU の試験と計測。
//
// 確かめること(23 §10 R-T1 の合格):
//   1. 物の当たりで関数を呼ぶ費用(物 100 個 × 底の 16 点。CPU 1 スレッド)。差分の候補 2 つ(一覧・格子)と差分なし
//   2. 溝の多い所(16 m 四方・溝 24 本・足跡 200 個)で面の開き具合を計算し直す費用(全部の柱 / 溝 1 本を彫った時に触れた柱だけ)
//   3. 整数の版と小数の版(描画。shaders/render/surface_detail_float.hlsli)のずれが 2 mm 以下(仮)
//   4. 10 cm の溝を掘ると、水が溝に沿って流れる(今の液体 sim/probe_liquid。開き具合 → セルの固い物の量)。溝が無ければ流れない
//   5. 差分を消すと自然の形に戻り、セーブと読み込みでビット一致(2 つの候補)。決定的(2 回作って同じハッシュ)
//   6. 2 つの候補が同じ形を作る(格子は 31.25 mm の点で彫った形を線形に補間するので、ずれを測る。記録)
// 引数: --measure-only(計測だけ。時間の表を出す)
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

#include "core/aliases.h"
#include "reaction_test_table.h"
#include "render/surface_detail_float.hlsli"
#include "sim/cell_volume.h"
#include "sim/probe_liquid.h"
#include "sim/probe_sim.h"
#include "sim/surface_detail.h"
#include "standard_world_physics.h"
#include "surface_detail_scene.h"

using namespace bicameral;
using namespace bicameral::sim;
using namespace bicameral::surface;
using namespace bicameral::test;

namespace {

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    using Clock = std::chrono::steady_clock;

    double MillisecondsSince(Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    const char* ModeName(uint32_t mode) {
        return mode == SD_MODE_LIST ? "一覧(候補 B)" : (mode == SD_MODE_GRID ? "格子(候補 A)" : "差分なし");
    }

    // --- 1: 当たりの費用 ---
    void TestContactCost() {
        std::printf("1. 物の当たりで関数を呼ぶ費用(物 %u 個 × 底の %u 点)\n", BODY_COUNT,
                    BODY_POINT_SIDE * BODY_POINT_SIDE);
        const SurfaceDetailScene scene = MakeBusyScene();
        const SdSceneData data = scene.Data();
        const std::vector<SdProbePoint> points = MakeBodyPoints();
        constexpr uint32_t REPEAT = 50;

        for (const uint32_t mode : {SD_MODE_NATURAL, SD_MODE_LIST, SD_MODE_GRID}) {
            uint64_t hash = 0;
            const auto start = Clock::now();
            for (uint32_t repeat = 0; repeat < REPEAT; ++repeat) {
                for (const SdProbePoint& probe : points) {
                    const SdContact contact = SdContactAt(data, scene.config, mode, probe.xUm, probe.zUm, probe.yUm);
                    hash = fx::FxHashCombine(hash, (uint64_t)contact.penetrationUm ^ (uint64_t)contact.slopeXQ16);
                }
            }

            const double perStep = MillisecondsSince(start) / REPEAT;
            std::printf("  %s: %.3f ms/刻み(%.2f µs/点・地面の高さ 3 回/点)ハッシュ %016llx\n", ModeName(mode), perStep,
                        perStep * 1000.0 / (double)points.size(), (unsigned long long)hash);
        }
    }

    // --- 2: 開き具合を計算し直す費用 ---
    std::vector<SdColumnOpenness> ComputeAllOpenness(const SurfaceDetailScene& scene, uint32_t mode) {
        const SdSceneData data = scene.Data();
        std::vector<SdColumnOpenness> result(scene.ColumnCount());
        for (uint32_t column = 0; column < scene.ColumnCount(); ++column) {
            result[column] = SdColumnOpennessAt(data, scene.config, mode, column % scene.config.columnsX,
                                                column / scene.config.columnsX, OPENNESS_BOTTOM_UM);
        }

        return result;
    }

    uint64_t HashOpenness(std::span<const SdColumnOpenness> values) {
        uint64_t hash = 0;
        for (const SdColumnOpenness& value : values) {
            for (uint32_t layer = 0; layer < SD_COLUMN_LAYERS; ++layer) {
                hash = fx::FxHashCombine(hash, value.fillQ16[layer] | ((uint64_t)value.openXQ16[layer] << 32));
                hash = fx::FxHashCombine(hash, value.openZQ16[layer] | ((uint64_t)value.openTopQ16[layer] << 32));
            }
        }

        return hash;
    }

    void TestOpennessCost() {
        std::printf("2. 溝の多い所(16 m 四方・溝 %u 本・足跡 %u 個)で面の開き具合を計算し直す費用(柱ごとに 4 セル)\n",
                    BUSY_GROOVES, BUSY_FOOTPRINTS);
        const SurfaceDetailScene scene = MakeBusyScene();
        uint32_t maxRegistered = 0;
        for (uint32_t column = 0; column < scene.ColumnCount(); ++column)
            maxRegistered = std::max(maxRegistered, scene.carveBegin[column + 1] - scene.carveBegin[column]);

        std::printf("  彫った形 %zu 件・柱ごとの登録 %zu 件(1 柱の最大 %u)・格子のある柱 %zu\n", scene.carves.size(),
                    scene.registered.size(), maxRegistered, scene.gridNodes.size() / SD_GRID_NODE_COUNT);

        for (const uint32_t mode : {SD_MODE_NATURAL, SD_MODE_LIST, SD_MODE_GRID}) {
            const auto start = Clock::now();
            const std::vector<SdColumnOpenness> values = ComputeAllOpenness(scene, mode);
            const double milliseconds = MillisecondsSince(start);
            std::printf("  %s: 全部の柱 %u 本 %.1f ms(%.1f µs/柱)ハッシュ %016llx\n", ModeName(mode),
                        scene.ColumnCount(), milliseconds, milliseconds * 1000.0 / scene.ColumnCount(),
                        (unsigned long long)HashOpenness(values));
        }

        // 溝 1 本を彫った時に計算し直す柱(触れた柱だけ)の費用
        SurfaceDetailScene carved = scene;
        const SdCarve groove = {
            .ax = 2100000, .az = 8100000, .bx = 6100000, .bz = 8100000, .radiusUm = 100000, .depthUm = 100000};
        const auto start = Clock::now();
        EnsureCarved(carved, groove);
        const SdSceneData data = carved.Data();
        uint32_t touched = 0;
        for (uint32_t columnZ = (groove.az - groove.radiusUm) / SD_CELL_UM;
             columnZ <= (groove.bz + groove.radiusUm) / SD_CELL_UM; ++columnZ) {
            for (uint32_t columnX = (groove.ax - groove.radiusUm) / SD_CELL_UM;
                 columnX <= (groove.bx + groove.radiusUm) / SD_CELL_UM; ++columnX) {
                (void)SdColumnOpennessAt(data, carved.config, SD_MODE_LIST, columnX, columnZ, OPENNESS_BOTTOM_UM);
                ++touched;
            }
        }

        std::printf("  溝 1 本(4 m)を彫る(一覧に足す + 登録の作り直し + 格子 + 触れた柱 %u 本の開き具合): %.2f ms\n",
                    touched, MillisecondsSince(start));
    }

    // --- 3: 整数の版と小数の版のずれ ---
    struct Drift {
        double maxMm = 0;
        double meanMm = 0;
        uint32_t worstX = 0;
        uint32_t worstZ = 0;
    };

    Drift MeasureDrift(const SurfaceDetailScene& scene, uint32_t mode, uint32_t step) {
        const SdSceneData data = scene.Data();
        Drift drift;
        double sum = 0;
        uint64_t count = 0;
        const uint32_t size = scene.config.columnsX * SD_CELL_UM;
        for (uint32_t z = step / 3; z < size; z += step) {
            for (uint32_t x = step / 7; x < size; x += step) {
                const double integer = (double)SdGroundHeightUm(data, scene.config, mode, x, z) * 0.001;
                const double real = (double)SdfGroundHeightM(data, scene.config, mode, (float)x * 0.000001f,
                                                             (float)z * 0.000001f) *
                                    1000.0;
                const double difference = std::abs(integer - real);
                sum += difference;
                ++count;
                if (difference <= drift.maxMm)
                    continue;

                drift.maxMm = difference;
                drift.worstX = x;
                drift.worstZ = z;
            }
        }

        drift.meanMm = sum / (double)count;
        return drift;
    }

    void TestDrift() {
        std::printf("3. 整数の版と小数の版(描画)のずれ(合格 2 mm 以下。仮)\n");
        SurfaceDetailScene scene = MakeBusyScene();
        for (const uint32_t smooth : {0u, 1u}) {
            scene.config.baseSmooth = smooth;
            for (const uint32_t mode : {SD_MODE_NATURAL, SD_MODE_LIST, SD_MODE_GRID}) {
                const Drift drift = MeasureDrift(scene, mode, 7919);  // 約 8 mm ごと(素数の幅で格子の点とずらす)
                std::printf("  下地 %s・%s: 最大 %.4f mm(%u, %u µm)・平均 %.5f mm\n", smooth != 0 ? "滑らか" : "平ら",
                            ModeName(mode), drift.maxMm, drift.worstX, drift.worstZ, drift.meanMm);
                EXPECT(drift.maxMm <= 2.0);
            }
        }

        // 6: 2 つの候補が同じ形か(格子は 31.25 mm の点の線形の補間なので、彫った形の縁と底でずれる)
        scene.config.baseSmooth = 0;
        const SdSceneData data = scene.Data();
        double maxDifference = 0;
        const uint32_t size = scene.config.columnsX * SD_CELL_UM;
        for (uint32_t z = 1301; z < size; z += 4999) {
            for (uint32_t x = 2203; x < size; x += 4999) {
                const int64_t list = SdGroundHeightUm(data, scene.config, SD_MODE_LIST, x, z);
                const int64_t grid = SdGroundHeightUm(data, scene.config, SD_MODE_GRID, x, z);
                maxDifference = std::max(maxDifference, (double)(list > grid ? list - grid : grid - list) * 0.001);
            }
        }

        std::printf("6. 一覧と格子の形の差(格子の補間の誤差): 最大 %.3f mm\n", maxDifference);
    }

    // --- 5: 差分を消す・セーブと読み込み・決定性 ---
    void TestDiffSaveLoad() {
        std::printf("5. 差分を消すと自然の形に戻る・セーブと読み込みでビット一致・決定的\n");
        constexpr uint32_t SAMPLES = 16;
        const SurfaceDetailScene scene = MakeBusyScene();
        const SurfaceDetailScene again = MakeBusyScene();
        const uint64_t natural = HashSurfaceHeights(scene, SD_MODE_NATURAL, SAMPLES);

        for (const uint32_t mode : {SD_MODE_LIST, SD_MODE_GRID}) {
            const uint64_t carved = HashSurfaceHeights(scene, mode, SAMPLES);
            EXPECT(carved != natural);
            EXPECT(carved == HashSurfaceHeights(again, mode, SAMPLES));

            const std::vector<uint8_t> saved = SaveSurfaceDiff(scene, mode);
            SurfaceDetailScene loaded = scene;
            ClearSurfaceDiff(loaded);
            EXPECT(HashSurfaceHeights(loaded, mode, SAMPLES) == natural);
            EXPECT(LoadSurfaceDiff(loaded, mode, saved).has_value());
            EXPECT(HashSurfaceHeights(loaded, mode, SAMPLES) == carved);
            EXPECT(HashOpenness(ComputeAllOpenness(loaded, mode)) == HashOpenness(ComputeAllOpenness(scene, mode)));
            std::printf("  %s: セーブの差分 %zu バイト・形のハッシュ %016llx(自然 %016llx)\n", ModeName(mode),
                        saved.size(), (unsigned long long)carved, (unsigned long long)natural);
        }
    }

    // --- 4: 掘った溝に水が流れるか ---
    //   区画 12 × 4 × 3 セル(6 m × 2 m × 1.5 m。y が上)。地面 1.0 m(柱の高さ)・池 A(x 1)と池 B(x 10)は 0.5 m の穴(z 1)。
    //   溝: z 1 の柱の中心の線を x 1 の中心から x 10 の中心まで、深さ 10 cm・幅 20 cm。A に水を満たす(1 セル)。
    //   セルの固い物(石)の量 = 関数の開き具合の fill(セルの体積の割合)。液体は固い物の残りの空きに入る(T-0612)。
    constexpr std::array<uint32_t, 3> WATER_REGION_MIN = {24, 0, 30};
    constexpr std::array<uint32_t, 3> WATER_REGION_SIZE = {12, 4, 3};
    constexpr uint32_t POND_A_X = 1;
    constexpr uint32_t POND_B_X = 10;
    constexpr uint32_t GROOVE_Z = 1;
    constexpr uint64_t FULL_WATER = 6900000000;  // µmol(0.5 m 角の水の少し下。probe_liquid_test と同じ)
    constexpr uint64_t NITROGEN = 4000000;
    constexpr uint64_t OXYGEN = 1000000;
    constexpr uint64_t FULL_STONE = 21000000000;  // µmol(セルを満たす量の少し上。probe_liquid_test と同じ)
    constexpr int32_t TEMPERATURE = 300000;

    BakedReactionTable MakeWaterTable() {
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

    SurfaceDetailScene MakeWaterScene(bool groove, bool relief) {
        SdConfig config = BusyConfig();
        config.columnsX = WATER_REGION_SIZE[0];
        config.columnsZ = WATER_REGION_SIZE[2];
        if (!relief)
            config.relief = {};

        SurfaceDetailScene scene = MakeSurfaceDetailScene(config, BUSY_GROUND_UM);
        scene.columnHeights[POND_A_X + (GROOVE_Z * config.columnsX)] = SD_HALF_CELL_UM * 2;
        scene.columnHeights[POND_B_X + (GROOVE_Z * config.columnsX)] = SD_HALF_CELL_UM * 2;
        if (groove) {
            // 4.5 m の溝は線分の上限(約 4.2 m)を超えるので、柱 5 の中心で折れ線に分ける。つなぎ目は 2 本の深さが足されて
            // 深くなる(一覧の形の今の制限。T-0938)ので、開き具合を記録する面(柱 5 の + x)から離して置く
            const uint32_t z = (GROOVE_Z * SD_CELL_UM) + SD_HALF_CELL_UM;
            const uint32_t beginX = (POND_A_X * SD_CELL_UM) + SD_HALF_CELL_UM;
            const uint32_t endX = (POND_B_X * SD_CELL_UM) + SD_HALF_CELL_UM;
            const uint32_t middleX = (5 * SD_CELL_UM) + SD_HALF_CELL_UM;
            EnsureCarved(scene, {.ax = beginX, .az = z, .bx = middleX, .bz = z, .radiusUm = 100000, .depthUm = 100000});
            EnsureCarved(scene, {.ax = middleX, .az = z, .bx = endX, .bz = z, .radiusUm = 100000, .depthUm = 100000});
        }

        return scene;
    }

    reaction::RxCell MakeGroundCell(const BakedReactionTable& table, uint32_t fillQ16, uint64_t water) {
        const uint32_t stone = table.SpeciesId("stone");
        uint64_t stoneAmount = FULL_STONE;
        if (fillQ16 < SD_ONE_Q16) {
            const uint64_t volume = (PROBE_CELL_VOLUME_PICOLITERS * fillQ16) >> 16;
            stoneAmount = volume / table.speciesMolarVolume[stone];
        }

        std::vector<SpeciesAmount> amounts = {{.species = table.SpeciesId("nitrogen"), .amount = NITROGEN},
                                              {.species = table.SpeciesId("oxygen"), .amount = OXYGEN}};
        if (stoneAmount != 0)
            amounts.push_back({.species = stone, .amount = stoneAmount});

        if (water != 0)
            amounts.push_back({.species = table.SpeciesId("liquid_water"), .amount = water});

        rng::sort(amounts, {}, &SpeciesAmount::species);
        return MakeReactionCell(table, amounts, TEMPERATURE);
    }

    struct WaterResult {
        uint64_t pondB = 0;         // µmol
        uint64_t groove = 0;        // x 2〜9 の z 1
        uint64_t beside = 0;        // z 0・2
        uint64_t besideGround = 0;  // そのうち地面の層(y 1。溝の横の地面の中)
        uint64_t above = 0;         // 地面の上の層(y 2 以上。区画の全部)
        uint64_t total = 0;
        uint32_t grooveOpenQ16 = 0;  // 溝の通る y 1 のセルの開き(+ x の面)
        uint32_t besideOpenQ16 = 0;  // 溝の横の y 1 のセル
    };

    WaterResult RunWater(bool groove, bool relief, uint64_t ticks) {
        const BakedReactionTable table = MakeWaterTable();
        const SurfaceDetailScene scene = MakeWaterScene(groove, relief);
        const uint32_t mode = SD_MODE_LIST;
        const std::vector<SdColumnOpenness> openness = [&] {
            const SdSceneData data = scene.Data();
            std::vector<SdColumnOpenness> values(scene.ColumnCount());
            for (uint32_t column = 0; column < scene.ColumnCount(); ++column)
                values[column] = SdColumnOpennessAt(data, scene.config, mode, column % scene.config.columnsX,
                                                    column / scene.config.columnsX, 0);
            return values;
        }();

        auto created = ProbeLiquidField::Create(table, {.minCell = WATER_REGION_MIN, .sizeCells = WATER_REGION_SIZE});
        EXPECT(created.has_value());
        ProbeLiquidField field = std::move(*created);
        std::vector<reaction::RxCell> cells = MakeBlankProbeWorld();
        const uint32_t width = WATER_REGION_SIZE[0];
        const uint32_t depth = WATER_REGION_SIZE[2];
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const uint32_t x = region % width;
            const uint32_t z = (region / width) % depth;
            const uint32_t y = region / (width * depth);
            const SdColumnOpenness& column = openness[x + (z * width)];
            const bool pondA = x == POND_A_X && z == GROOVE_Z && y == 1;
            cells[field.GridIndex(region)] = MakeGroundCell(table, column.fillQ16[y], pondA ? FULL_WATER : 0);
        }

        WaterResult result;
        result.grooveOpenQ16 = openness[5 + (GROOVE_Z * width)].openXQ16[1];
        result.besideOpenQ16 = openness[5].openXQ16[1];
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            std::vector<uint32_t> changed;
            (void)field.Step(table, cells, tick, changed);
        }

        const uint32_t water = table.SpeciesId("liquid_water");
        for (uint32_t region = 0; region < field.RegionCellCount(); ++region) {
            const uint32_t x = region % width;
            const uint32_t z = (region / width) % depth;
            const uint32_t y = region / (width * depth);
            const reaction::RxCell& cell = cells[field.GridIndex(region)];
            for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                if (cell.species[i] != water)
                    continue;

                result.total += cell.amounts[i];
                if (y >= 2)
                    result.above += cell.amounts[i];

                if (z != GROOVE_Z && y == 1)
                    result.besideGround += cell.amounts[i];

                if (z != GROOVE_Z)
                    result.beside += cell.amounts[i];
                else if (x == POND_B_X)
                    result.pondB += cell.amounts[i];
                else if (x > POND_A_X && x < POND_B_X)
                    result.groove += cell.amounts[i];
            }
        }

        return result;
    }

    void TestWaterInGroove() {
        std::printf("4. 10 cm の溝を掘ると水が溝に沿って流れる(今の液体・30 秒)\n");
        constexpr uint64_t TICKS = 1800;
        const WaterResult grooved = RunWater(true, false, TICKS);
        const WaterResult flat = RunWater(false, false, TICKS);
        const WaterResult rough = RunWater(true, true, TICKS);
        for (const auto& [name, result] :
             {std::pair{"溝あり", grooved}, std::pair{"溝なし", flat}, std::pair{"溝あり + 肌理", rough}}) {
            std::printf(
                "  %s: 池 B %.4f・溝 %.4f・溝の横 %.4f(地面の中 %.4f)・地面の上の層 %.4f(全体 %llu µmol)・"
                "溝のセルの + x の開き %.4f・横 %.4f\n",
                name, (double)result.pondB / (double)result.total, (double)result.groove / (double)result.total,
                (double)result.beside / (double)result.total, (double)result.besideGround / (double)result.total,
                (double)result.above / (double)result.total, (unsigned long long)result.total,
                (double)result.grooveOpenQ16 / 65536.0, (double)result.besideOpenQ16 / 65536.0);
        }

        EXPECT(grooved.total == FULL_WATER && flat.total == FULL_WATER);
        EXPECT(grooved.pondB * 100 >= grooved.total);  // 30 秒で 1% 以上が池 B へ届く
        // 溝の横の地面の中(y 1)へは入らない。池 A から溢れて地面の上の層(y 2)へ出た水は 0.5 m のセルの平らな面を広がる(記録)
        EXPECT(grooved.besideGround == 0);
        EXPECT(flat.pondB == 0 && flat.groove == 0 && flat.beside == 0);
    }

}  // namespace

int main(int argc, char** argv) {
    const bool measureOnly = argc > 1 && std::string(argv[1]) == "--measure-only";
    TestContactCost();
    TestOpennessCost();
    if (!measureOnly) {
        TestDrift();
        TestDiffSaveLoad();
        TestWaterInGroove();
    }

    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("すべて通った\n");
    return 0;
}
