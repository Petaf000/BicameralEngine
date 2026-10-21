// sun_column_test.cpp — 太陽の影の柱の CPU リファレンス(sim/sun_column。T-0910・23 §10 R-R0)。
//   1. 光の値の欄: パッケージ → ベイク。書かない物質は固体・液体が灰色で通さない・気体が透明(D-610)。片方の帯だけなら同じ値
//   2. 壁の陰: 斜めの太陽で、壁の陰の地面は日の光をちょうど 0 受ける。日なたは受ける
//   3. 保存: 帯ごとに 入った = 吸った + 返した + 外へ(ビット単位)。セルの和 = 帳簿の吸った・返した
//   4. 細かさによらない: 0.5 m と 0.25 m の世界で、平らな地面が受ける量の和が ±1 %(仮)。真上と斜めの太陽
//   5. 温室: ガラスの屋根は短い帯を通し(中の床が受ける)、長い帯を吸う(中の床は長い帯を 0 受ける)
//   6. 多重解像度: 一部を細かくした木でも、陰は 0・保存はビット単位・地面の和は細かくしない木と ±0.5 %(仮)
//   7. 決定的: 光線を逆の順に走らせても同じ語・2 回で同じ
//   8. 誤り: 光線の細かさが木の最も細かいレベルより粗い・向きが 0・世界の箱が空
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "script/luau_sandbox.h"
#include "script/reaction_package.h"
#include "sun_column_scene.h"

namespace {

    using namespace bicameral;
    using namespace bicameral::script;
    using namespace bicameral::sim;
    namespace mr = bicameral::multires;
    namespace rx = bicameral::reaction;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr SunLight VERTICAL_SUN{.direction = {0, -1, 0},
                                    .irradianceShortMilliwatts = test::SUN_IRRADIANCE_SHORT,
                                    .irradianceLongMilliwatts = test::SUN_IRRADIANCE_LONG};
    // 光が +x へ進みながら下りる(高さ 3 に 2 の割合。陰は壁の +x 側に落ちる)
    constexpr SunLight SLANTED_SUN{.direction = {2, -3, 0},
                                   .irradianceShortMilliwatts = test::SUN_IRRADIANCE_SHORT,
                                   .irradianceLongMilliwatts = test::SUN_IRRADIANCE_LONG};
    // 3 軸とも斜め(z の向きにも)
    constexpr SunLight SKEW_SUN{.direction = {3, -5, 2},
                                .irradianceShortMilliwatts = test::SUN_IRRADIANCE_SHORT,
                                .irradianceLongMilliwatts = test::SUN_IRRADIANCE_LONG};

    SunColumnResult Sweep(const MultiresNest& nest, const BakedReactionTable& table, const SunLight& sun,
                          int32_t rayLevel) {
        auto result = SweepSunColumn(nest, table, sun, rayLevel);
        if (!result) {
            std::printf("FAILED: 掃けない: %s\n", result.error().c_str());
            ++failureCount;

            return {};
        }

        return std::move(*result);
    }

    uint64_t Ledger(std::span<const uint64_t> words, uint32_t band, uint32_t entry) {
        return words[mr::SunLedgerWord(band, entry)];
    }

    // 3. 保存: 帳簿が釣り合い、セルの和が帳簿と同じ
    bool Conserves(const MultiresNest& nest, std::span<const uint64_t> words) {
        bool ok = true;
        for (uint32_t band = 0; band < rx::LT_BANDS; ++band) {
            const uint64_t entered = Ledger(words, band, mr::SUN_LEDGER_ENTERED);
            const uint64_t absorbed = Ledger(words, band, mr::SUN_LEDGER_ABSORBED);
            const uint64_t reflected = Ledger(words, band, mr::SUN_LEDGER_REFLECTED);
            const uint64_t escaped = Ledger(words, band, mr::SUN_LEDGER_ESCAPED);
            uint64_t cellAbsorbed = 0;
            uint64_t cellReflected = 0;
            for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
                for (uint32_t index = 0; index < mr::MR_BLOCK_CELLS; ++index) {
                    cellAbsorbed += test::SunWord(words, slot, index, band, mr::SUN_KIND_ABSORBED);
                    cellReflected += test::SunWord(words, slot, index, band, mr::SUN_KIND_REFLECTED);
                }
            }

            ok = ok && entered != 0 && entered == absorbed + reflected + escaped && cellAbsorbed == absorbed &&
                 cellReflected == reflected;
        }

        return ok;
    }

    // 葉のセルのうち条件に合うものが受けた量(吸った + 返した。帯 band)の和。単位は光線の細かさ rayLevel のエネルギー
    template <typename Pick>
    uint64_t Received(const MultiresNest& nest, std::span<const uint64_t> words, uint32_t band, Pick pick) {
        uint64_t total = 0;
        test::ForEachSunLeafCell(nest, [&](uint32_t slot, uint32_t index, const mr::MrBlock& block) {
            if (!pick(test::SunQuarterCorner(block, index), block.level))
                return;

            total += test::SunWord(words, slot, index, band, mr::SUN_KIND_ABSORBED) +
                     test::SunWord(words, slot, index, band, mr::SUN_KIND_REFLECTED);
        });

        return total;
    }

    // 地面の一番上の層(0.25〜0.5 m。0.5 m の世界では地面のセル全部)
    bool IsGroundTop(const std::array<int64_t, 3>& quarter, int32_t level) {
        return level == 0 ? quarter[1] == 0 : quarter[1] == 1;
    }

    // --- 1. 光の値の欄 ---

    std::expected<BakedReactionTable, std::string> BakeWithMod(const std::string& init) {
        auto sandbox = LuauSandbox::Create(SandboxLimits{});
        if (!sandbox)
            return std::unexpected(sandbox.error());

        (*sandbox)->Seal();
        std::vector<PackageSource> packages = ReadCombustionTestPackages();
        PackageSource mod{.name = "light_mod", .files = {}};
        mod.files.emplace("package.luau", "return { format = 1, depends = { \"combustion_test\" } }");
        mod.files.emplace("init.luau", init);
        packages.push_back(std::move(mod));
        const auto loaded = LoadPackages(**sandbox, packages, PackageLoadOptions{.basePackage = "combustion_test"});
        if (!loaded)
            return std::unexpected(loaded.error());

        if (!loaded->rejected.empty())
            return std::unexpected(loaded->rejected.front().package + ": " + loaded->rejected.front().reason);

        const auto definition = ReadReactionTableDefinition(*loaded);
        if (!definition)
            return std::unexpected(definition.error());

        return BakeReactionTable(*definition);
    }

    std::string SolidSpecies(const char* name, const char* fields) {
        return std::string(name) +
               " = { composition = { C = 1 }, formation_enthalpy_j_per_mol = 0, heat_capacity_mj_per_mol_k = 8500, "
               "thermal_conductivity_mw_per_m_k = 1000, phase = \"solid\", density_kg_per_m3 = 2000" +
               fields + " }";
    }

    void TestLightFields() {
        const std::string init = "return { species = { " + SolidSpecies("plain_rock", "") + ", " +
                                 SolidSpecies("dark_glass",
                                              ", light_absorption_short_mm2_per_mol = 7, "
                                              "light_reflectance_long_permille = 250") +
                                 ", " + SolidSpecies("old_soot", ", radiation_absorption_mm2_per_mol = 900") + " } }";
        const auto baked = BakeWithMod(init);
        if (!baked) {
            std::printf("FAILED: ベイクできない: %s\n", baked.error().c_str());
            ++failureCount;

            return;
        }

        EXPECT(baked->speciesLight.size() == baked->speciesNames.size());
        const rx::LtSpeciesLight rock = baked->speciesLight[baked->SpeciesId("plain_rock")];
        const rx::LtSpeciesLight glass = baked->speciesLight[baked->SpeciesId("dark_glass")];
        const rx::LtSpeciesLight soot = baked->speciesLight[baked->SpeciesId("old_soot")];
        const rx::LtSpeciesLight oxygen = baked->speciesLight[baked->SpeciesId("oxygen")];
        const rx::LtSpeciesLight cellulose = baked->speciesLight[baked->SpeciesId("cellulose")];

        // 書かない固体: 灰色で通さない・照り返し 0.5。書かない気体: 透明(D-610)
        EXPECT(rock.absorptionShort == rx::LT_OPAQUE_ABSORPTION_MM2_PER_MOL &&
               rock.absorptionLong == rx::LT_OPAQUE_ABSORPTION_MM2_PER_MOL);
        EXPECT(rock.reflectanceShortQ16 == 32768 && rock.reflectanceLongQ16 == 32768);
        EXPECT(cellulose.absorptionShort == rx::LT_OPAQUE_ABSORPTION_MM2_PER_MOL);
        EXPECT(oxygen.absorptionShort == 0 && oxygen.absorptionLong == 0 && oxygen.reflectanceShortQ16 == 0);

        // 片方の帯だけ書いた: もう片方も同じ値(灰色)。照り返しも同じ
        EXPECT(glass.absorptionShort == 7 && glass.absorptionLong == 7);
        EXPECT(glass.reflectanceShortQ16 == 16384 && glass.reflectanceLongQ16 == 16384);

        // 前の欄(ADR-0252)だけ書いた: 両方の帯にその値・照り返しは書いた物質なので 0
        EXPECT(soot.absorptionShort == 900 && soot.absorptionLong == 900 && soot.reflectanceShortQ16 == 0);

        // 範囲の外は誤り
        const auto over = BakeWithMod("return { species = { " +
                                      SolidSpecies("bad_rock", ", light_reflectance_short_permille = 1001") + " } }");
        EXPECT(!over && over.error().find("light_reflectance_short_permille") != std::string::npos);
        const auto negative = BakeWithMod(
            "return { species = { " + SolidSpecies("bad_rock", ", light_absorption_long_mm2_per_mol = -1") + " } }");
        EXPECT(!negative && negative.error().find("light_absorption_long_mm2_per_mol") != std::string::npos);
    }

    // --- 2・3. 壁の陰と保存 ---

    void TestWallShadow(const BakedReactionTable& table) {
        for (const int32_t level : {0, 1}) {
            const uint32_t roots = level == 0 ? 1u : 2u;
            const MultiresNest nest = test::MakeSunWorld(table, level, roots, roots, roots, test::WallScene);
            const SunColumnResult result = Sweep(nest, table, SLANTED_SUN, level);
            if (result.words.empty())
                return;

            EXPECT(Conserves(nest, result.words));

            // 壁(x 1〜1.5 m・高さ 2.5 m まで)の +x 側 1.5〜2.5 m の地面は陰(どの光線も壁を通る。ちょうど 0)
            const auto shadow = [](const std::array<int64_t, 3>& q, int32_t cellLevel) {
                return IsGroundTop(q, cellLevel) && q[0] >= 6 && q[0] < 10;
            };
            const auto sunny = [](const std::array<int64_t, 3>& q, int32_t cellLevel) {
                return IsGroundTop(q, cellLevel) && q[0] >= 12;
            };
            for (uint32_t band = 0; band < rx::LT_BANDS; ++band) {
                const uint64_t inShadow = Received(nest, result.words, band, shadow);
                const uint64_t inSun = Received(nest, result.words, band, sunny);
                std::printf("壁の陰(レベル %d・帯 %u): 陰 %llu・日なた %llu\n", level, band,
                            static_cast<unsigned long long>(inShadow), static_cast<unsigned long long>(inSun));
                EXPECT(inShadow == 0 && inSun > 0);
            }
        }
    }

    // --- 4. 細かさによらない ---

    void TestResolutionConsistency(const BakedReactionTable& table) {
        const MultiresNest coarse = test::MakeSunWorld(table, 0, 1, 1, 1, test::FlatGround);
        const MultiresNest fine = test::MakeSunWorld(table, 1, 2, 2, 2, test::FlatGround);
        for (const SunLight& sun : {VERTICAL_SUN, SLANTED_SUN, SKEW_SUN}) {
            const SunColumnResult coarseResult = Sweep(coarse, table, sun, 0);
            const SunColumnResult fineResult = Sweep(fine, table, sun, 1);
            if (coarseResult.words.empty() || fineResult.words.empty())
                return;

            EXPECT(Conserves(coarse, coarseResult.words) && Conserves(fine, fineResult.words));
            for (uint32_t band = 0; band < rx::LT_BANDS; ++band) {
                // 地面全部(どの層でも)。0.25 m の世界の量は 1/8 mJ の単位 → mJ に
                const auto ground = [](const std::array<int64_t, 3>& q, int32_t) {
                    return q[1] < 2;
                };
                const double coarseMj = static_cast<double>(Received(coarse, coarseResult.words, band, ground));
                const double fineMj = static_cast<double>(Received(fine, fineResult.words, band, ground)) / 8.0;
                const double difference = std::abs(fineMj - coarseMj) / coarseMj;
                std::printf("細かさ(向き %d,%d,%d・帯 %u): 0.5 m %.1f mJ・0.25 m %.1f mJ・差 %.4f %%\n",
                            sun.direction[0], sun.direction[1], sun.direction[2], band, coarseMj, fineMj,
                            difference * 100.0);
                EXPECT(coarseMj > 0.0 && difference <= 0.01);
            }
        }
    }

    // --- 5. 温室 ---

    void TestGlassHouse(const BakedReactionTable& table) {
        for (const int32_t level : {0, 1}) {
            const uint32_t roots = level == 0 ? 1u : 2u;
            const MultiresNest nest = test::MakeSunWorld(table, level, roots, roots, roots, test::GlassHouse);
            const MultiresNest open = test::MakeSunWorld(table, level, roots, roots, roots, test::FlatGround);
            const SunColumnResult result = Sweep(nest, table, VERTICAL_SUN, level);
            const SunColumnResult openResult = Sweep(open, table, VERTICAL_SUN, level);
            if (result.words.empty() || openResult.words.empty())
                return;

            EXPECT(Conserves(nest, result.words));
            const auto innerFloor = [](const std::array<int64_t, 3>& q, int32_t cellLevel) {
                return IsGroundTop(q, cellLevel) && q[0] >= 4 && q[0] < 12 && q[2] >= 4 && q[2] < 12;
            };
            const auto roof = [](const std::array<int64_t, 3>& q, int32_t) {
                return q[1] >= 8 && q[1] < 10;
            };
            const uint64_t floorShort = Received(nest, result.words, rx::LT_BAND_SHORT, innerFloor);
            const uint64_t floorLong = Received(nest, result.words, rx::LT_BAND_LONG, innerFloor);
            const uint64_t roofLong = Received(nest, result.words, rx::LT_BAND_LONG, roof);
            const uint64_t openShort = Received(open, openResult.words, rx::LT_BAND_SHORT, innerFloor);
            // ガラス 0.5 m の短い帯の τ = 1 mm²/mol × 2 万 mol ÷ 0.25 m² = 0.08 → 床へ e^−0.08 ≈ 92.3 %
            const double through = static_cast<double>(floorShort) / static_cast<double>(openShort);
            std::printf("温室(レベル %d): 床 短 %llu(屋根なしの %.2f %%)・長 %llu / 屋根の長 %llu\n", level,
                        static_cast<unsigned long long>(floorShort), through * 100.0,
                        static_cast<unsigned long long>(floorLong), static_cast<unsigned long long>(roofLong));
            // 短い帯はほとんど床へ・長い帯は屋根が全部吸い、床は 0
            EXPECT(through > 0.91 && through < 0.935 && floorLong == 0 && roofLong > 0);
        }
    }

    // --- 6. 多重解像度 ---

    MultiresNest MakeRefinedWall(const BakedReactionTable& table) {
        MultiresNest nest = test::MakeSunWorld(table, 0, 1, 1, 1, test::WallScene, 1);
        // 根の八分の一 (0, 0, 0)(x・y・z が 0〜2 m。壁と陰の一部を含む)を 0.25 m に
        const mr::MrRequest refine = mr::MrMakeRequest(mr::MR_REQUEST_REFINE, 1, 2, 2, 2);
        SubmitRequests(nest, std::span(&refine, 1));
        ProcessRequests(nest);

        return nest;
    }

    void TestRefinedTree(const BakedReactionTable& table) {
        const MultiresNest refined = MakeRefinedWall(table);
        const MultiresNest plain = test::MakeSunWorld(table, 0, 1, 1, 1, test::WallScene, 1);
        EXPECT(refined.blocks[1].kind == mr::MR_BLOCK_REAL && refined.blocks[1].level == 1);

        // 光線の細かさが木の最も細かいレベルより粗いと断る(細かい葉を飛ばさない)
        EXPECT(!SweepSunColumn(refined, table, SLANTED_SUN, 0));

        for (const SunLight& sun : {SLANTED_SUN, SKEW_SUN}) {
            const SunColumnResult refinedResult = Sweep(refined, table, sun, 1);
            const SunColumnResult plainResult = Sweep(plain, table, sun, 1);
            if (refinedResult.words.empty() || plainResult.words.empty())
                return;

            EXPECT(Conserves(refined, refinedResult.words) && Conserves(plain, plainResult.words));
            const auto ground = [](const std::array<int64_t, 3>& q, int32_t) {
                return q[1] < 2;
            };
            const auto shadow = [](const std::array<int64_t, 3>& q, int32_t cellLevel) {
                return IsGroundTop(q, cellLevel) && q[0] >= 6 && q[0] < 10 && q[2] >= 2 && q[2] < 14;
            };
            for (uint32_t band = 0; band < rx::LT_BANDS; ++band) {
                const auto refinedGround = static_cast<double>(Received(refined, refinedResult.words, band, ground));
                const auto plainGround = static_cast<double>(Received(plain, plainResult.words, band, ground));
                const double difference = std::abs(refinedGround - plainGround) / plainGround;
                std::printf("多重解像度(向き %d,%d,%d・帯 %u): 細かくした木 %.0f・しない木 %.0f・差 %.4f %%\n",
                            sun.direction[0], sun.direction[1], sun.direction[2], band, refinedGround, plainGround,
                            difference * 100.0);
                EXPECT(difference <= 0.005);
                if (sun.direction[2] == 0)
                    EXPECT(Received(refined, refinedResult.words, band, shadow) == 0);
            }
        }
    }

    // --- 7. 決定的 ---

    void TestDeterministic(const BakedReactionTable& table) {
        const MultiresNest nest = MakeRefinedWall(table);
        const auto params = MakeSunParams(nest, SKEW_SUN, 1);
        EXPECT(params.has_value());
        if (!params)
            return;

        const std::vector<uint64_t> forward = SweepSunColumn(nest, table, *params);
        const std::vector<uint64_t> again = SweepSunColumn(nest, table, *params);
        const std::vector<uint64_t> backward = SweepSunColumn(nest, table, *params, true);
        uint64_t digest = 0;
        for (const uint64_t word : forward)
            digest = fx::FxHashCombine(digest, word);

        std::printf("決定的: 語 %zu・要約 %016llx\n", forward.size(), static_cast<unsigned long long>(digest));
        EXPECT(forward == again && forward == backward);
    }

    // --- 8. 誤り ---

    void TestRejects(const BakedReactionTable& table) {
        const MultiresNest nest = test::MakeSunWorld(table, 0, 1, 1, 1, test::FlatGround);
        EXPECT(!SweepSunColumn(nest, table, SunLight{.direction = {0, 0, 0}}, 0));
        EXPECT(!SweepSunColumn(nest, table, SunLight{.direction = {40000, -1, 0}}, 0));
        EXPECT(!SweepSunColumn(nest, table, VERTICAL_SUN, -1));

        MultiresNest empty = nest;
        empty.edges.world = {};
        EXPECT(!SweepSunColumn(empty, table, VERTICAL_SUN, 0));

        // 光の無い太陽: 何も入らない(帳簿は全部 0)
        const auto dark = SweepSunColumn(nest, table, SunLight{.direction = {0, -1, 0}}, 0);
        EXPECT(dark && std::ranges::all_of(dark->words, [](uint64_t word) { return word == 0; }));
    }

    // --- 計測(docs/perf.md): 64³ の地形(0.5 m)を真上と斜めの太陽で ---

    void MeasureTerrain(const BakedReactionTable& table) {
        const MultiresNest nest = test::MakeSunWorld(table, 0, 8, 8, 8, test::Terrain);
        for (const SunLight& sun : {VERTICAL_SUN, SKEW_SUN}) {
            for (const int32_t rayLevel : {0, 1}) {
                const auto params = MakeSunParams(nest, sun, rayLevel);
                if (!params)
                    continue;

                const auto begin = std::chrono::steady_clock::now();
                const std::vector<uint64_t> words = SweepSunColumn(nest, table, *params);
                const auto end = std::chrono::steady_clock::now();
                const double ms = std::chrono::duration<double, std::milli>(end - begin).count();
                EXPECT(Conserves(nest, words));
                std::printf("計測(CPU・1 スレッド): 64³ の地形・向き %d,%d,%d・光線の細かさ %d・光線 %u 本: %.2f ms\n",
                            sun.direction[0], sun.direction[1], sun.direction[2], rayLevel,
                            params->rayCountB * params->rayCountC, ms);
            }
        }
    }

}  // namespace

int main() {
    const BakedReactionTable table = test::MakeSunTable();
    TestLightFields();
    TestWallShadow(table);
    TestResolutionConsistency(table);
    TestGlassHouse(table);
    TestRefinedTree(table);
    TestDeterministic(table);
    TestRejects(table);
    MeasureTerrain(table);
    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);

        return 1;
    }

    std::printf("sun_column: すべて通過\n");

    return 0;
}
