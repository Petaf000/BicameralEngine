// sun_lens_test.cpp — 日の光を物で集める CPU リファレンス(sim/sun_lens。研究 R-R2・T-0878・23 §10 R-R2)。
//   1. 屈折の値の欄: パッケージ → ベイク(書かなければ 1 = 曲げない・1 未満は誤り)
//   2. 焦点の距離: 軸に近い光線の焦点が、形と屈折の値から薄いレンズの式と ±5 %(仮)。世界の中でも、レンズの高さを変えて
//      紙の面の強さが一番大きくなる高さが ±5 %
//   3. 燃え始め: 虫眼鏡の焦点で乾いた草・紙(cellulose)が燃え始める(溜めた熱の量で判定。ADR-0426 の仮の行)。レンズ無しでは燃えない
//   4. 細かさ: 0.5 m と 0.25 m の世界で燃え始めの時刻が ±10 %(仮)
//   5. 保存: 影の柱 入った = 吸った + 返した + 外へ + レンズに入った(ビット単位)・レンズの光線 入った = レンズが吸った・返した + 世界が吸った・返した + 外へ
//   6. 決定的: レンズの光線を逆の順に走らせても同じ・2 回で同じ
//   7. 費用: レンズが日陰・夜ならレンズの光線は 0 本。日なたの費用を測る
// 燃え続けるか(23 §6 の未確認)は、焦点の量と燃えた所の熱でセルの平均の温度がどれだけ動くかを出して記録する。
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
#include "sim/sun_lens.h"
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

    // --- 試験の値(公開側の試験データ。ゲームの中身ではない。D-006)---
    constexpr uint32_t LENS_REFRACTION_PERMILLE = 1500;  // ふつうのガラス
    constexpr int64_t LENS_RADIUS_UM = 200000;           // 両凸の球面 R = 0.2 m → 薄いレンズで f = R ÷ 2(n − 1) = 0.2 m
    constexpr int64_t LENS_APERTURE_UM = 50000;          // 口の直径 10 cm の虫眼鏡
    constexpr int64_t LENS_THICKNESS_UM = 14000;
    constexpr uint64_t LENS_MICROMOLES_PER_CELL = 26041666667;  // 2500 kg/m³ ÷ 12 g/mol × 0.125 m³
    constexpr int64_t GROUND_TOP_UM = 500000;                   // 紙の面(0.5 m)
    constexpr int64_t LENS_X_UM = 1100000;                      // 0.25 m のセルの境から離す
    constexpr int64_t LENS_Z_UM = 1150000;
    // 燃え始めの仮の行(ADR-0426): 燃え始める強さ 20 kW/m²・溜める時間 2 s(紙の臨界の熱の流れの目安。値は仮)
    constexpr uint32_t PAPER_IGNITION_WATTS = 20000;
    constexpr uint32_t PAPER_ACCUMULATE_MS = 2000;
    constexpr uint32_t IGNITION_TICK_LIMIT = 60 * 120;

    constexpr SunLight VERTICAL_SUN{.direction = {0, -1, 0},
                                    .irradianceShortMilliwatts = test::SUN_IRRADIANCE_SHORT,
                                    .irradianceLongMilliwatts = test::SUN_IRRADIANCE_LONG};
    constexpr SunLight SLANTED_SUN{.direction = {1, -6, 0},
                                   .irradianceShortMilliwatts = test::SUN_IRRADIANCE_SHORT,
                                   .irradianceLongMilliwatts = test::SUN_IRRADIANCE_LONG};
    constexpr SunLight NIGHT{.direction = {0, -1, 0}, .irradianceShortMilliwatts = 0, .irradianceLongMilliwatts = 0};

    BakedReactionTable MakeLensTable() {
        return test::MakeSunTable([](ReactionTableDefinition& definition) {
            SpeciesDefinition glass{.name = "sun_test_lens_glass",
                                    .composition = {{.element = "C", .count = 1}},
                                    .formationEnthalpy = 0,
                                    .heatCapacity = 8500,
                                    .thermalConductivity = 1000,
                                    .phase = SpeciesPhase::Solid,
                                    .density = 2500};
            glass.lightAbsorptionShort = 5;           // 0.5 m で τ ≈ 0.52(1.4 cm で 1.5 % 吸う)
            glass.lightAbsorptionLong = 4000000000u;  // 長い帯は通さない
            glass.lightReflectanceShortPermille = 0;
            glass.lightReflectanceLongPermille = 0;
            glass.lightRefractionPermille = LENS_REFRACTION_PERMILLE;
            definition.species.push_back(glass);
        });
    }

    SunLensShape MakeShape(const BakedReactionTable& table, int64_t heightAboveGroundUm,
                           int64_t apertureUm = LENS_APERTURE_UM) {
        return {.centreUm = {LENS_X_UM, GROUND_TOP_UM + heightAboveGroundUm, LENS_Z_UM},
                .apertureUm = apertureUm,
                .frontRadiusUm = LENS_RADIUS_UM,
                .backRadiusUm = LENS_RADIUS_UM,
                .thicknessUm = LENS_THICKNESS_UM,
                .axis = 1,
                .facing = 1,
                .species = table.SpeciesId("sun_test_lens_glass"),
                .micromolesPerCell = LENS_MICROMOLES_PER_CELL,
                .lensLevel = 9};
    }

    // 紙の地面(下の 0.5 m)。roof なら 2〜2.5 m に石の屋根(x・z が 0.5〜2 m。レンズを日陰に)
    test::SunMaterial PaperGround(int64_t x, int64_t y, int64_t z, bool roof) {
        if (y < 2)
            return test::SunMaterial::Paper;

        const bool underRoof = roof && y >= 8 && y < 10 && x >= 2 && x < 8 && z >= 2 && z < 8;

        return underRoof ? test::SunMaterial::Stone : test::SunMaterial::Air;
    }

    MultiresNest MakePaperWorld(const BakedReactionTable& table, int32_t level, bool roof = false) {
        const uint32_t roots = level == 0 ? 1u : 2u;

        return test::MakeSunWorld(table, level, roots, roots, roots,
                                  [roof](int64_t x, int64_t y, int64_t z) { return PaperGround(x, y, z, roof); });
    }

    SunLensResult Focus(const MultiresNest& nest, const BakedReactionTable& table, const SunLight& sun,
                        int32_t rayLevel, const SunLensShape& shape, bool reverse = false) {
        auto result = FocusSunLight(nest, table, sun, rayLevel, shape, reverse);
        if (!result) {
            std::printf("FAILED: 集められない: %s\n", result.error().c_str());
            ++failureCount;

            return {};
        }

        return std::move(*result);
    }

    // 燃え始めまでの刻み(強さ一定。届かなければ IGNITION_TICK_LIMIT)
    uint32_t TicksToIgnite(uint64_t fluxWatts) {
        uint64_t dose = 0;
        for (uint32_t tick = 1; tick <= IGNITION_TICK_LIMIT; ++tick) {
            dose = mr::LnHeatDoseStep(dose, fluxWatts, PAPER_ACCUMULATE_MS);
            if (mr::LnIgnites(dose, PAPER_IGNITION_WATTS, PAPER_ACCUMULATE_MS))
                return tick;
        }

        return IGNITION_TICK_LIMIT;
    }

    // --- 1. 屈折の値の欄 ---

    std::expected<BakedReactionTable, std::string> BakeWithMod(const std::string& init) {
        auto sandbox = LuauSandbox::Create(SandboxLimits{});
        if (!sandbox)
            return std::unexpected(sandbox.error());

        (*sandbox)->Seal();
        std::vector<PackageSource> packages = ReadCombustionTestPackages();
        PackageSource mod{.name = "lens_mod", .files = {}};
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

    std::string Species(const char* name, const char* fields) {
        return std::string(name) +
               " = { composition = { C = 1 }, formation_enthalpy_j_per_mol = 0, heat_capacity_mj_per_mol_k = 8500, "
               "thermal_conductivity_mw_per_m_k = 1000, phase = \"solid\", density_kg_per_m3 = 2000" +
               fields + " }";
    }

    void TestRefractionField() {
        const auto baked = BakeWithMod("return { species = { " +
                                       Species("ice_lens", ", light_refraction_permille = 1310") + ", " +
                                       Species("plain_rock", "") + " } }");
        if (!baked) {
            std::printf("FAILED: ベイクできない: %s\n", baked.error().c_str());
            ++failureCount;

            return;
        }

        EXPECT(baked->speciesRefractionQ16.size() == baked->speciesNames.size());
        EXPECT(baked->speciesRefractionQ16[baked->SpeciesId("ice_lens")] == 85852);  // 1.31 × 65536
        EXPECT(baked->speciesRefractionQ16[baked->SpeciesId("plain_rock")] == 65536);
        const auto low = BakeWithMod("return { species = { " +
                                     Species("bad_glass", ", light_refraction_permille = 999") + " } }");
        EXPECT(!low && low.error().find("light_refraction_permille") != std::string::npos);
    }

    // --- 2. 焦点の距離 ---

    // 軸から offset の光線がレンズを出て軸と交わる所の、レンズの中心からの距離(µm)
    double AxisCrossing(const mr::LnLens& lens, int64_t offsetUm) {
        const mr::LnVec through = mr::LnMake(lens.centreX + offsetUm, lens.centreY, lens.centreZ);
        const mr::LnRay ray = mr::LnTraceLens(lens, through, mr::LnUnitOf(0, -1, 0), 1, 0);
        if (ray.refracted == 0 || ray.direction.x == 0)
            return 0.0;

        const double along = -static_cast<double>(ray.position.x - lens.centreX) / static_cast<double>(ray.direction.x);

        return static_cast<double>(lens.centreY - ray.position.y) - (along * static_cast<double>(ray.direction.y));
    }

    double ThinLensFocalUm() {
        const double n = LENS_REFRACTION_PERMILLE / 1000.0;

        return 1.0 / ((n - 1.0) * (2.0 / static_cast<double>(LENS_RADIUS_UM)));
    }

    void TestFocalLength(const BakedReactionTable& table) {
        const auto lens = MakeSunLens(table, MakeShape(table, 200000));
        EXPECT(lens.has_value());
        if (!lens)
            return;

        const double thin = ThinLensFocalUm();
        const double n = LENS_REFRACTION_PERMILLE / 1000.0;
        const double thick = 1.0 / ((n - 1.0) * ((2.0 / LENS_RADIUS_UM) - ((n - 1.0) * LENS_THICKNESS_UM /
                                                                           (n * LENS_RADIUS_UM * LENS_RADIUS_UM))));
        const double paraxial = AxisCrossing(*lens, 2000);
        const double marginal = AxisCrossing(*lens, 45000);
        std::printf(
            "焦点(レンズの中心から): 軸に近い光線 %.1f mm・縁の光線 %.1f mm・薄いレンズの式 %.1f mm・厚いレンズの式 "
            "%.1f mm\n",
            paraxial / 1000.0, marginal / 1000.0, thin / 1000.0, thick / 1000.0);
        EXPECT(std::abs(paraxial - thin) / thin <= 0.05);

        // 世界の中: レンズの高さを 2 mm ずつ変え、紙の面の強さが一番大きい高さ。口の直径 5 cm(f/4)で ±5 %。
        // 口の直径 10 cm(f/2)は球面の収差で縁の光線が手前で集まるので、一番強い所が手前へ寄る(記録だけ)
        const MultiresNest nest = MakePaperWorld(table, 0);
        for (const int64_t aperture : {int64_t{25000}, LENS_APERTURE_UM}) {
            int64_t bestHeight = 0;
            uint64_t bestFlux = 0;
            for (int64_t height = 160000; height <= 240000; height += 2000) {
                const SunLensResult result = Focus(nest, table, VERTICAL_SUN, 0, MakeShape(table, height, aperture));
                const SunLensPeak peak = FindSunLensPeak(nest, result);
                if (peak.fluxWatts > bestFlux) {
                    bestFlux = peak.fluxWatts;
                    bestHeight = height;
                }
            }

            const double difference = std::abs(static_cast<double>(bestHeight) - thin) / thin;
            std::printf(
                "世界の中の焦点(口の直径 %lld cm): 紙の面の強さが一番大きい高さ %.1f cm(%llu "
                "W/m²)・薄いレンズの式との差 %.2f %%\n",
                static_cast<long long>(aperture / 5000), bestHeight / 10000.0,
                static_cast<unsigned long long>(bestFlux), 100.0 * difference);
            if (aperture != LENS_APERTURE_UM)
                EXPECT(difference <= 0.05);
        }
    }

    // --- 5. 保存 ---

    bool Conserves(const MultiresNest& nest, const SunLensResult& result) {
        bool ok = true;
        for (uint32_t band = 0; band < rx::LT_BANDS; ++band) {
            uint64_t caught = 0;
            for (const SunCaughtRay& ray : result.caught)
                caught += band == 0 ? ray.caughtShort : ray.caughtLong;

            const auto ledger = [&](uint32_t entry) {
                return result.columnWords[mr::SunLedgerWord(band, entry)];
            };
            ok = ok && ledger(mr::SUN_LEDGER_ENTERED) == ledger(mr::SUN_LEDGER_ABSORBED) +
                                                             ledger(mr::SUN_LEDGER_REFLECTED) +
                                                             ledger(mr::SUN_LEDGER_ESCAPED) + caught;
            ok = ok && result.lensEntered[band] == (caught << mr::LN_ENERGY_SHIFT);
            ok = ok && result.lensEntered[band] == result.lensAbsorbed[band] + result.lensReflected[band] +
                                                       result.worldAbsorbed[band] + result.worldReflected[band] +
                                                       result.escaped[band];

            uint64_t cellAbsorbed = 0;
            uint64_t cellReflected = 0;
            for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
                for (uint32_t index = 0; index < mr::MR_BLOCK_CELLS; ++index) {
                    cellAbsorbed += result.lensWords[mr::SunCellWord(slot, index, band, mr::SUN_KIND_ABSORBED)];
                    cellReflected += result.lensWords[mr::SunCellWord(slot, index, band, mr::SUN_KIND_REFLECTED)];
                }
            }

            ok = ok && cellAbsorbed == result.worldAbsorbed[band] && cellReflected == result.worldReflected[band];
        }

        return ok && result.unfinishedRays == 0;
    }

    double LensMillijoules(uint64_t units, int32_t rayLevel) {
        return static_cast<double>(units) / 65536.0 / std::pow(8.0, rayLevel);
    }

    // --- 3・4. 燃え始めと細かさ ---

    struct IgnitionRun {
        uint64_t peakFlux = 0;
        uint32_t ticks = 0;
        double caughtMj = 0.0;
    };

    IgnitionRun RunIgnition(const BakedReactionTable& table, int32_t level, const SunLight& sun, int64_t height) {
        const MultiresNest nest = MakePaperWorld(table, level);
        const SunLensResult result = Focus(nest, table, sun, level, MakeShape(table, height));
        EXPECT(Conserves(nest, result));
        const SunLensPeak peak = FindSunLensPeak(nest, result);
        const IgnitionRun run{.peakFlux = peak.fluxWatts,
                              .ticks = TicksToIgnite(peak.fluxWatts),
                              .caughtMj = LensMillijoules(result.lensEntered[0] + result.lensEntered[1], level)};
        std::printf(
            "燃え始め(%.2f m の世界・向き %d,%d,%d): レンズに入った %.1f mJ/刻み・光線 %llu 本・焦点の強さ %llu W/m²・"
            "燃え始めまで %u 刻み(%.2f s)\n",
            level == 0 ? 0.5 : 0.25, sun.direction[0], sun.direction[1], sun.direction[2], run.caughtMj,
            static_cast<unsigned long long>(result.lensRays), static_cast<unsigned long long>(run.peakFlux), run.ticks,
            run.ticks / 60.0);

        return run;
    }

    void TestIgnition(const BakedReactionTable& table) {
        const int64_t focus = static_cast<int64_t>(ThinLensFocalUm());
        const IgnitionRun coarse = RunIgnition(table, 0, VERTICAL_SUN, focus);
        const IgnitionRun fine = RunIgnition(table, 1, VERTICAL_SUN, focus);
        EXPECT(coarse.ticks < IGNITION_TICK_LIMIT && fine.ticks < IGNITION_TICK_LIMIT);
        const double timeDifference = std::abs(static_cast<double>(fine.ticks) - coarse.ticks) / coarse.ticks;
        const double caughtDifference = std::abs(fine.caughtMj - coarse.caughtMj) / coarse.caughtMj;
        std::printf("細かさ: 燃え始めの時刻の差 %.2f %%・焦点の強さの差 %.3f %%・レンズに入った量の差 %.4f %%\n",
                    100.0 * timeDifference,
                    100.0 * std::abs(static_cast<double>(fine.peakFlux) - static_cast<double>(coarse.peakFlux)) /
                        static_cast<double>(coarse.peakFlux),
                    100.0 * caughtDifference);
        EXPECT(timeDifference <= 0.10);

        // ピントを外した所(焦点の手前 4 cm。燃え始めまで長く、刻みの数で細かさの差が見える)
        const IgnitionRun coarseOff = RunIgnition(table, 0, VERTICAL_SUN, focus - 40000);
        const IgnitionRun fineOff = RunIgnition(table, 1, VERTICAL_SUN, focus - 40000);
        const double offDifference = std::abs(static_cast<double>(fineOff.ticks) - coarseOff.ticks) / coarseOff.ticks;
        std::printf("細かさ(ピントを外した所): 燃え始めの時刻の差 %.2f %%\n", 100.0 * offDifference);
        EXPECT(coarseOff.ticks > coarse.ticks && coarseOff.ticks < IGNITION_TICK_LIMIT && offDifference <= 0.10);

        // 斜めの太陽でも集まって燃える(保存は RunIgnition の中で確かめる)
        const IgnitionRun slanted = RunIgnition(table, 0, SLANTED_SUN, focus);
        EXPECT(slanted.ticks < IGNITION_TICK_LIMIT);

        // レンズの無い日なたの紙(日の光そのもの 1000 W/m²)は燃えない。ピントを外すと遅い
        const MultiresNest nest = MakePaperWorld(table, 0);
        const SunColumnResult plain = *SweepSunColumn(nest, table, VERTICAL_SUN, 0);
        SunLensResult bare;
        bare.params = plain.params;
        bare.columnWords = plain.words;
        uint64_t plainFlux = 0;
        test::ForEachSunLeafCell(nest, [&](uint32_t slot, uint32_t index, const mr::MrBlock&) {
            plainFlux = std::max(plainFlux, SunLensFluxWatts(nest, bare, slot, index, 1));
        });
        std::printf("レンズ無し: 紙の面の強さ %llu W/m²・燃え始めまで %u 刻み(上限 = 燃えない)\n",
                    static_cast<unsigned long long>(plainFlux), TicksToIgnite(plainFlux));
        EXPECT(plainFlux > 0 && TicksToIgnite(plainFlux) == IGNITION_TICK_LIMIT);

        // 燃え続けるか(23 §6 の未確認): 焦点に入る力と、燃えた所(焦点の 1 cm² の紙 0.1 mm)の熱で 0.5 m のセルの平均の温度が動く大きさ
        const double focusWatts = coarse.caughtMj * 60.0 / 1000.0;
        const double cellCapacity = 1157.0 * 211.0;          // J/K(cellulose 1157 mol × 211 J/(mol·K))
        const double burntJoules = 1.0e-8 * 1500.0 * 1.7e7;  // 1 cm² × 0.1 mm × 1500 kg/m³ × 17 MJ/kg
        std::printf(
            "燃え続けるか: 焦点の力 %.2f W でセルの平均は %.6f K/s・焦点の紙が燃え切った熱 %.1f J で %.5f K"
            "(セルの平均では燃え始めの温度に届かず、細かい火は 0.5 m のセルで薄まって消える見込み)\n",
            focusWatts, focusWatts / cellCapacity, burntJoules, burntJoules / cellCapacity);
    }

    // --- 6. 決定的 ---

    uint64_t Digest(const SunLensResult& result) {
        uint64_t digest = 0;
        for (const uint64_t word : result.lensWords)
            digest = fx::FxHashCombine(digest, word);

        for (const auto& [spot, energy] : result.hits) {
            for (const int64_t part : spot)
                digest = fx::FxHashCombine(digest, static_cast<uint64_t>(part));

            digest = fx::FxHashCombine(digest, energy);
        }

        return digest;
    }

    void TestDeterministic(const BakedReactionTable& table) {
        const MultiresNest nest = MakePaperWorld(table, 1);
        const SunLensShape shape = MakeShape(table, 200000);
        const SunLensResult forward = Focus(nest, table, SLANTED_SUN, 1, shape);
        const SunLensResult again = Focus(nest, table, SLANTED_SUN, 1, shape);
        const SunLensResult backward = Focus(nest, table, SLANTED_SUN, 1, shape, true);
        std::printf("決定的: 当たった面 %zu・要約 %016llx\n", forward.hits.size(),
                    static_cast<unsigned long long>(Digest(forward)));
        EXPECT(Digest(forward) == Digest(again) && Digest(forward) == Digest(backward));
        EXPECT(forward.lensEntered == backward.lensEntered && forward.escaped == backward.escaped);
    }

    // --- 7. 費用 ---

    void TestCost(const BakedReactionTable& table) {
        // 日陰(屋根の下)・夜はレンズの光線 0 本
        const MultiresNest roofed = MakePaperWorld(table, 0, true);
        const SunLensResult shaded = Focus(roofed, table, VERTICAL_SUN, 0, MakeShape(table, 200000));
        const MultiresNest open = MakePaperWorld(table, 0);
        const SunLensResult night = Focus(open, table, NIGHT, 0, MakeShape(table, 200000));
        std::printf("費用: 日陰のレンズの光線 %llu 本・夜 %llu 本\n", static_cast<unsigned long long>(shaded.lensRays),
                    static_cast<unsigned long long>(night.lensRays));
        EXPECT(shaded.lensRays == 0 && shaded.caught.empty() && night.lensRays == 0);

        for (int32_t level = 0; level <= 1; ++level) {
            const MultiresNest nest = MakePaperWorld(table, level);
            const auto params = MakeSunParams(nest, VERTICAL_SUN, level);
            const auto begin = std::chrono::steady_clock::now();
            const std::vector<uint64_t> plain = SweepSunColumn(nest, table, *params);
            const auto middle = std::chrono::steady_clock::now();
            const SunLensResult lens = Focus(nest, table, VERTICAL_SUN, level, MakeShape(table, 200000));
            const auto end = std::chrono::steady_clock::now();
            std::printf(
                "計測(CPU・1 スレッド・4 m 角): %.2f m の世界・影の柱だけ %.2f ms・レンズ込み %.2f ms(レンズの光線 "
                "%llu 本)\n",
                level == 0 ? 0.5 : 0.25, std::chrono::duration<double, std::milli>(middle - begin).count(),
                std::chrono::duration<double, std::milli>(end - middle).count(),
                static_cast<unsigned long long>(lens.lensRays));
            EXPECT(!plain.empty());
        }
    }

    // --- 誤り ---

    void TestRejects(const BakedReactionTable& table) {
        SunLensShape thin = MakeShape(table, 200000);
        thin.thicknessUm = 5000;
        EXPECT(!MakeSunLens(table, thin).has_value());

        const MultiresNest nest = MakePaperWorld(table, 0);
        const SunLight sideways{
            .direction = {-1, 0, 0}, .irradianceShortMilliwatts = 1000, .irradianceLongMilliwatts = 0};
        EXPECT(!FocusSunLight(nest, table, sideways, 0, MakeShape(table, 200000)).has_value());
    }

}  // namespace

int main() {
    const BakedReactionTable table = MakeLensTable();
    TestRefractionField();
    TestFocalLength(table);
    TestIgnition(table);
    TestDeterministic(table);
    TestCost(table);
    TestRejects(table);
    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);

        return 1;
    }

    std::printf("sun_lens: すべて通過\n");

    return 0;
}
