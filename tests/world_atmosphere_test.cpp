// world_atmosphere_test.cpp — 基準の大気の高さの分布(T-0499・ADR-0182 の改訂・D-494)を CPU だけで確かめる。
// 見るもの: 高さの点の列のベイクの検査(高さの順・範囲・点ごとの合計)/ 点の間の直線(温度・割合の合計がちょうど 1・端の外は端の値)/
//           高さの静水圧(地球の分布で ISA の表と 0.2% 以内・重力 0 で一定・海面より下で上がる・2 回で同じ)/
//           パッケージの読み方(一様な書き方 = 高さ 0 の点 1 つ・標準の補助で重い物が下に溜まる点と点の上書き・一緒に書けない・低い順でない)/
//           気体の層の基準を層ごとの温度と成分で作る形(一様なら今の形とビット一致・層の圧力が高さの静水圧と 1 Pa 以内・
//           どの重力でも静止大気が動かない)。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "script/reaction_table_loader.h"
#include "sim/gas_reference.h"
#include "sim/world_atmosphere.h"
#include "standard_world_physics.h"

namespace {

    using namespace bicameral;
    using namespace bicameral::sim;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    bool Contains(std::string_view text, std::string_view part) {
        if (text.find(part) != std::string_view::npos)
            return true;

        std::printf("  '%.*s' に '%.*s' が無い\n", static_cast<int>(text.size()), text.data(),
                    static_cast<int>(part.size()), part.data());

        return false;
    }

    const fs::path STANDARD_DIR = fs::path(BICAMERAL_DATA_STANDARD_DIR);
    const fs::path ATMOSPHERE_PACKAGES_DIR = fs::path(BICAMERAL_ATMOSPHERE_PACKAGES_DIR);

    // 標準パッケージだけを足して読む(窓の --world blank --mix と同じ読み方)
    std::expected<script::LoadedReactionTable, std::string> LoadStandard(const std::vector<std::string>& names) {
        std::vector<fs::path> folders;
        for (const std::string& name : names)
            folders.push_back(STANDARD_DIR / name);

        return script::LoadReactionTable(
            {.basePackage = {}, .readPackageRoot = false, .addedPackages = std::move(folders)});
    }

    // tests/packages_atmosphere/<place> の <place>_world をゲーム本体として読む(依存は data/standard の棚から)
    std::expected<script::LoadedReactionTable, std::string> LoadWorld(const std::string& place) {
        return script::LoadReactionTable({.packageRoot = ATMOSPHERE_PACKAGES_DIR / place,
                                          .basePackage = place + "_world",
                                          .standardRoot = STANDARD_DIR});
    }

    uint32_t PartsOf(const std::vector<AtmosphereFraction>& fractions, uint32_t species) {
        const auto found = std::ranges::find(fractions, species, &AtmosphereFraction::species);
        return found == fractions.end() ? 0 : found->partsPerMillion;
    }

    uint64_t TotalParts(const std::vector<AtmosphereFraction>& fractions) {
        uint64_t total = 0;
        for (const AtmosphereFraction& fraction : fractions)
            total += fraction.partsPerMillion;

        return total;
    }

    // 2 つの点の大気(高さ 0 m・300 K・N2 80 % → 1 km・250 K・N2 60 %・O2 39.9 %・Ar 0.1 %)
    WorldAtmosphereDefinition TwoPoints() {
        return {.points = {{.label = "environment.atmosphere_profile[1]",
                            .heightMillimeters = 0,
                            .temperatureMilliKelvin = 300000,
                            .fractions = {{"nitrogen", 800000}, {"oxygen", 200000}}},
                           {.label = "environment.atmosphere_profile[2]",
                            .heightMillimeters = 1000000,
                            .temperatureMilliKelvin = 250000,
                            .fractions = {{"nitrogen", 600000}, {"oxygen", 399000}, {"argon", 1000}}}},
                .surfacePressurePascals = 100000};
    }

    // --- ベイクの検査 ---

    void TestBakeProfile(const BakedReactionTable& table) {
        const auto baked = BakeWorldAtmosphere(TwoPoints(), table);
        EXPECT(baked && baked->points.size() == 2 && baked->points[1].fractions.size() == 3);

        auto unsorted = TwoPoints();
        unsorted.points[1].heightMillimeters = -1;
        const auto unsortedResult = BakeWorldAtmosphere(unsorted, table);
        EXPECT(!unsortedResult && Contains(unsortedResult.error(), "低い順") &&
               Contains(unsortedResult.error(), "atmosphere_profile[2]"));

        auto same = TwoPoints();
        same.points[1].heightMillimeters = 0;
        const auto sameResult = BakeWorldAtmosphere(same, table);
        EXPECT(!sameResult && Contains(sameResult.error(), "低い順"));

        auto far = TwoPoints();
        far.points[1].heightMillimeters = ATMOSPHERE_HEIGHT_LIMIT_MILLIMETERS + 1;
        const auto farResult = BakeWorldAtmosphere(far, table);
        EXPECT(!farResult && Contains(farResult.error(), "範囲"));

        auto shortSum = TwoPoints();
        shortSum.points[1].fractions[0].partsPerMillion = 590000;
        const auto shortResult = BakeWorldAtmosphere(shortSum, table);
        EXPECT(!shortResult && Contains(shortResult.error(), "atmosphere_profile[2]") &&
               Contains(shortResult.error(), "990000"));
    }

    // --- 点の間の直線 ---

    void TestSample(const BakedReactionTable& table) {
        const auto baked = BakeWorldAtmosphere(TwoPoints(), table);
        if (!baked)
            return;

        const uint32_t nitrogen = table.SpeciesId("nitrogen");
        const uint32_t oxygen = table.SpeciesId("oxygen");
        const uint32_t argon = table.SpeciesId("argon");

        // 端と端の外は端の値(上の点の物質は下の点に載らない)
        const AtmosphereSample bottom = SampleAtmosphere(*baked, -5000);
        EXPECT(bottom.temperatureMilliKelvin == 300000 && bottom.fractions == baked->points[0].fractions);
        EXPECT(SampleAtmosphere(*baked, 0) == bottom);
        EXPECT(PartsOf(bottom.fractions, argon) == 0);
        EXPECT(SampleAtmosphere(*baked, 2000000).fractions == baked->points[1].fractions);

        // 真ん中: ちょうど半分
        const AtmosphereSample middle = SampleAtmosphere(*baked, 500000);
        EXPECT(middle.temperatureMilliKelvin == 275000);
        EXPECT(PartsOf(middle.fractions, nitrogen) == 700000 && PartsOf(middle.fractions, oxygen) == 299500 &&
               PartsOf(middle.fractions, argon) == 500);

        // 割り切れない高さ: 切り捨てて、端数を余りの大きい順に配る(足してちょうど 1)
        const AtmosphereSample third = SampleAtmosphere(*baked, 333333);
        std::printf("sample at 333.333 m: T = %u mK, N2 %u, O2 %u, Ar %u ppm\n", third.temperatureMilliKelvin,
                    PartsOf(third.fractions, nitrogen), PartsOf(third.fractions, oxygen),
                    PartsOf(third.fractions, argon));
        EXPECT(third.temperatureMilliKelvin == 283334);
        EXPECT(TotalParts(third.fractions) == ATMOSPHERE_PARTS_PER_WHOLE);
        // N2 733333.4・O2 266333.267・Ar 333.333 → 切り捨てで 999999 → 余りがいちばん大きい N2 に 1 ppm
        EXPECT(PartsOf(third.fractions, nitrogen) == 733334 && PartsOf(third.fractions, oxygen) == 266333 &&
               PartsOf(third.fractions, argon) == 333);

        for (int64_t height = -10; height < 1000010; height += 7777)
            EXPECT(TotalParts(SampleAtmosphere(*baked, height).fractions) == ATMOSPHERE_PARTS_PER_WHOLE);
    }

    // --- 高さの静水圧(地球の分布)---

    // ISA(U.S. Standard Atmosphere 1976)の表の圧力(Pa)と比べる(海面より下は等温の式の値)
    void ExpectNearIsa(const WorldAtmosphere& atmosphere, const BakedReactionTable& table, int64_t heightMillimeters,
                       double isaPascals) {
        const uint64_t pressure = AtmospherePressureMicroPascal(
            atmosphere, table, STANDARD_GRAVITY_MICROMETERS_PER_SECOND2, heightMillimeters);
        const double pascals = (double)pressure * 1e-6;
        const double error = (pascals - isaPascals) / isaPascals;
        std::printf("pressure at %lld m: %.2f Pa (ISA %.2f Pa, %+.4f %%)\n", (long long)(heightMillimeters / 1000),
                    pascals, isaPascals, error * 100.0);
        EXPECT(std::abs(error) < 2e-3);
    }

    void TestPressure() {
        const auto loaded = LoadStandard({"real_elements", "real_matter", "earth_environment"});
        EXPECT(loaded.has_value());
        if (!loaded) {
            std::printf("  %s\n", loaded.error().c_str());
            return;
        }

        const BakedReactionTable& table = loaded->table;
        const WorldAtmosphere& earth = table.atmosphere;
        EXPECT(earth.points.size() == 8);
        if (earth.Empty())
            return;

        EXPECT(AtmospherePressureMicroPascal(earth, table, STANDARD_GRAVITY_MICROMETERS_PER_SECOND2, 0) ==
               101325ull * 1000000);
        ExpectNearIsa(earth, table, -1000000,
                      114079.6);  // 一番低い点より下は端の温度のまま(等温の式 p0 exp(M̄ g h ÷ R T))
        ExpectNearIsa(earth, table, 11000000, 22632.06);
        ExpectNearIsa(earth, table, 20000000, 5474.89);
        ExpectNearIsa(earth, table, 32000000, 868.02);

        // 重力 0 なら一定・2 回で同じ・高いほど低い
        EXPECT(AtmospherePressureMicroPascal(earth, table, 0, 5000000) == 101325ull * 1000000);
        const uint64_t first = AtmospherePressureMicroPascal(earth, table, STANDARD_GRAVITY_MICROMETERS_PER_SECOND2,
                                                             8848000);
        EXPECT(first == AtmospherePressureMicroPascal(earth, table, STANDARD_GRAVITY_MICROMETERS_PER_SECOND2, 8848000));
        EXPECT(first < AtmospherePressureMicroPascal(earth, table, STANDARD_GRAVITY_MICROMETERS_PER_SECOND2, 8847500));
    }

    // --- パッケージの読み方 ---

    void TestPackages() {
        // 一様な書き方 = 高さ 0 の点 1 つ
        const auto uniform = LoadWorld("uniform");
        EXPECT(uniform.has_value());
        if (uniform) {
            const WorldAtmosphere& atmosphere = uniform->table.atmosphere;
            EXPECT(atmosphere.points.size() == 1 && atmosphere.surfacePressurePascals == 100000);
            EXPECT(!atmosphere.Empty() && atmosphere.points[0].heightMillimeters == 0 &&
                   atmosphere.points[0].temperatureMilliKelvin == 300000 && atmosphere.points[0].fractions.size() == 2);
        } else {
            std::printf("  %s\n", uniform.error().c_str());
        }

        // 標準の補助: 重い物(CO2)ほど下に溜まる点・温度の折れ目(11 km)を足す・海面の点の上書き
        const auto settled = LoadWorld("settled");
        EXPECT(settled.has_value());
        if (settled) {
            const BakedReactionTable& table = settled->table;
            const WorldAtmosphere& atmosphere = table.atmosphere;
            const uint32_t carbonDioxide = table.SpeciesId("carbon_dioxide");
            const std::array<int64_t, 5> heights = {0, 5000000, 10000000, 11000000, 15000000};
            EXPECT(atmosphere.points.size() == heights.size() && atmosphere.surfacePressurePascals == 101325);
            for (size_t i = 0; i < atmosphere.points.size() && i < heights.size(); ++i) {
                const AtmospherePoint& point = atmosphere.points[i];
                std::printf("settled point %zu: %lld m, %u mK, CO2 %u ppm\n", i,
                            (long long)(point.heightMillimeters / 1000), point.temperatureMilliKelvin,
                            PartsOf(point.fractions, carbonDioxide));
                EXPECT(point.heightMillimeters == heights[i]);
                EXPECT(TotalParts(point.fractions) == ATMOSPHERE_PARTS_PER_WHOLE);
                if (i > 0)
                    EXPECT(PartsOf(point.fractions, carbonDioxide) <
                           PartsOf(atmosphere.points[i - 1].fractions, carbonDioxide));
            }

            if (atmosphere.points.size() == heights.size()) {
                EXPECT(atmosphere.points[0].temperatureMilliKelvin == 300000);  // set_point で書き直した
                EXPECT(PartsOf(atmosphere.points[0].fractions, carbonDioxide) == 500000);
                EXPECT(atmosphere.points[3].temperatureMilliKelvin == 216650);  // ISA の対流圏界面
            }

            const auto again = LoadWorld("settled");
            EXPECT(again && again->tableVersion == settled->tableVersion);
        } else {
            std::printf("  %s\n", settled.error().c_str());
        }

        // 高さの点の列と一様な書き方は一緒に書けない
        const auto conflict = LoadWorld("conflict");
        EXPECT(!conflict && Contains(conflict.error(), "一緒に書けない"));

        // 低い順に並んでいない点の列はベイクで止まる
        const auto unsorted = LoadWorld("unsorted");
        EXPECT(!unsorted && Contains(unsorted.error(), "低い順"));
    }

    // --- 気体の層の基準(層ごとの温度と成分)---

    constexpr uint32_t MOON_GRAVITY = 1620000;
    constexpr int64_t BOX_BOTTOM_MILLIMETERS = 1000;  // 箱の床の高さ(一番低い点の高さ)

    GasConfig MakeAirConfig(uint32_t gravity) {
        GasConfig config;
        config.sizeX = 4;
        config.sizeY = 4;
        config.sizeZ = 16;
        config.speciesCount = 2;
        config.species[0] = {.molarMass = 28013, .heatCapacity = 20810, .formationEnergy = 0};  // N2
        config.species[1] = {.molarMass = 31999, .heatCapacity = 21064, .formationEnergy = 0};  // O2

        WorldPhysics world = StandardWorldPhysics();
        world.gravityMicrometersPerSecond2 = gravity;
        ApplyWorldPhysics(world, config);

        return config;
    }

    // 層 z の高さ(床 + z × 0.5 m)の基準の大気を、気体の物質の順(N2・O2)の Q32 にする
    std::vector<GasReferenceLayer> ProfileLayers(const GasConfig& config, const WorldAtmosphere& atmosphere,
                                                 const BakedReactionTable& table) {
        const std::array<uint32_t, 2> species = {table.SpeciesId("nitrogen"), table.SpeciesId("oxygen")};
        std::vector<GasReferenceLayer> layers;
        for (uint32_t z = 0; z < config.sizeZ; ++z) {
            const AtmosphereSample sample = SampleAtmosphere(
                atmosphere, BOX_BOTTOM_MILLIMETERS + (int64_t)z * ATMOSPHERE_PRESSURE_STEP_MILLIMETERS);
            GasReferenceLayer layer{.temperatureMilliKelvin = sample.temperatureMilliKelvin};
            for (size_t slot = 0; slot < species.size(); ++slot) {
                const uint64_t q32 = ((uint64_t)PartsOf(sample.fractions, species[slot]) << 32) /
                                     ATMOSPHERE_PARTS_PER_WHOLE;
                layer.fractionsQ32[slot] = (uint32_t)std::min<uint64_t>(q32, UINT32_MAX);
            }

            layers.push_back(layer);
        }

        return layers;
    }

    int64_t MaxSpeed(const GasBox& box) {
        int64_t speed = 0;
        for (const GasCell& cell : box.cells) {
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis) {
                const int64_t velocity = GasVelocity(box, cell, axis);
                speed = std::max(speed, velocity < 0 ? -velocity : velocity);
            }
        }

        return speed;
    }

    bool SameCells(const std::vector<GasCell>& left, const std::vector<GasCell>& right) {
        if (left.size() != right.size())
            return false;

        for (size_t i = 0; i < left.size(); ++i) {
            if (left[i].amounts != right[i].amounts || left[i].energy != right[i].energy ||
                left[i].momentum != right[i].momentum)
                return false;
        }

        return true;
    }

    void TestGasReference(const BakedReactionTable& table) {
        // --- 一様な層の版は今の形とビット一致 ---
        const GasConfig earthConfig = MakeAirConfig(STANDARD_GRAVITY_MICROMETERS_PER_SECOND2);
        const std::array<uint32_t, 2> air = {3393024163u, 901943132u};
        const GasReferenceLayer uniformLayer{.temperatureMilliKelvin = 293150, .fractionsQ32 = {air[0], air[1], 0, 0}};
        const std::vector<GasReferenceLayer> uniform(earthConfig.sizeZ, uniformLayer);
        EXPECT(SameCells(MakeHydrostaticReference(earthConfig, air, 293150, 101325ull * 1000000),
                         MakeHydrostaticReference(earthConfig, uniform, 101325ull * 1000000)));

        // --- 高さの分布(床 1 m・280 K・N2 79 % → 9 m・320 K・N2 60 %。温度の逆転と成分の傾き)---
        const WorldAtmosphereDefinition definition{
            .points = {{.heightMillimeters = BOX_BOTTOM_MILLIMETERS,
                        .temperatureMilliKelvin = 280000,
                        .fractions = {{"nitrogen", 790000}, {"oxygen", 210000}}},
                       {.heightMillimeters = 9000,
                        .temperatureMilliKelvin = 320000,
                        .fractions = {{"nitrogen", 600000}, {"oxygen", 400000}}}},
            .surfacePressurePascals = 90000};
        const auto atmosphere = BakeWorldAtmosphere(definition, table);
        EXPECT(atmosphere.has_value());
        if (!atmosphere)
            return;

        for (const uint32_t gravity :
             {0u, MOON_GRAVITY, STANDARD_GRAVITY_MICROMETERS_PER_SECOND2, fx::GRAVITY_MAX_MICROMETERS_PER_SECOND2}) {
            const GasConfig config = MakeAirConfig(gravity);
            const uint64_t floorPressure = AtmospherePressureMicroPascal(*atmosphere, table, gravity,
                                                                         BOX_BOTTOM_MILLIMETERS);
            GasBox box = MakeGasBox(
                config, MakeHydrostaticReference(config, ProfileLayers(config, *atmosphere, table), floorPressure));

            // 層の圧力 = その高さの静水圧(気体の層の基準と高さの静水圧は同じ形の積み方。モル質量の丸めの差だけ)
            int64_t worst = 0;
            for (uint32_t z = 0; z < config.sizeZ; ++z) {
                const int64_t layer = DeriveGasCell(box, box.reference[z], z).pressure;
                const auto expected = (int64_t)AtmospherePressureMicroPascal(
                    *atmosphere, table, gravity,
                    BOX_BOTTOM_MILLIMETERS + (int64_t)z * ATMOSPHERE_PRESSURE_STEP_MILLIMETERS);
                worst = std::max(worst, std::abs(layer - expected));
            }

            // 静止大気はどの重力でも動かない(保存はビット単位)
            const GasTotals before = SumGas(box);
            for (uint32_t tick = 0; tick < 120; ++tick)
                StepGas(box);

            const GasTotals after = SumGas(box);
            std::printf(
                "profile atmosphere (gravity %u um/s^2): layer pressure vs column %lld uPa, "
                "max speed after 120 ticks = %lld\n",
                gravity, (long long)worst, (long long)MaxSpeed(box));
            EXPECT(worst <= 1000000);
            EXPECT(MaxSpeed(box) <= 1);
            EXPECT(after.amounts == before.amounts);
            EXPECT(after.energy == before.energy + box.ledger.energy);
        }
    }

}  // namespace

int main() {
    const auto real = LoadStandard({"real_elements", "real_matter"});
    if (!real) {
        std::printf("FAILED: 標準パッケージを読めない: %s\n", real.error().c_str());
        return 1;
    }

    TestBakeProfile(real->table);
    TestSample(real->table);
    TestPressure();
    TestPackages();
    TestGasReference(real->table);

    if (failureCount == 0)
        std::printf("world_atmosphere_test: all passed\n");

    return failureCount == 0 ? 0 : 1;
}
