// radiation_reference_test.cpp — 放射の熱の交換の CPU リファレンス(T-0583・ADR-0252。shaders/common/radiation.hlsli・sim/radiation_reference)。
// CPU だけで走る。確かめること:
//   - 黒い平行な板の対が Stefan–Boltzmann の解析解(σAΔt·F·(T₁⁴ − T₂⁴))と合う。距離が離れると見え方の割合 F だけ弱まる
//   - 対の流れは入れ替えると符号だけ反転する(ビット単位)。熱は高い方から低い方へだけ(坂を下る。D-466)
//   - 多くの面・多くの刻みで、エネルギーの合計がビット単位で変わらない(保存。D-465)。対の並びを変えても同じ結果(決定性)
//   - 熱容量の小さい面でも温度の順序が入れ替わらない(行き過ぎの上限)。初めの最高・最低の温度の外へ出ない
//   - 物質の任意の欄 radiation_absorption_mm2_per_mol がパッケージ → ベイク → セルの吸収率(1 − e^−τ)まで通る。書かない物質は放射しない
// double を使うのは試験の参照値だけ(シミュのコードは整数だけ)。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "reaction_test_table.h"
#include "script/luau_sandbox.h"
#include "script/reaction_package.h"
#include "sim/radiation_reference.h"
#include "sim/reaction_table.h"

namespace {

    using namespace bicameral;
    using namespace bicameral::script;
    using namespace bicameral::sim;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    // --- 道具 ---

    constexpr uint32_t ONE_Q16 = 65536;
    constexpr double STEFAN_BOLTZMANN = 5.670374419e-8;                          // W/(m²K⁴)
    constexpr double SIGMA_AREA_TICK = STEFAN_BOLTZMANN * 0.25 / 60.0 * 1000.0;  // mJ/K⁴(面 0.25 m²・1/60 秒)
    constexpr uint64_t HUGE_CAPACITY = uint64_t{1} << 60;                        // 上限が効かない熱容量(nJ/K)

    // 参照値: σAΔt·w·(T₁⁴ − T₂⁴)(mJ)
    double ReferenceFlow(double hotKelvin, double coldKelvin, double weight) {
        return SIGMA_AREA_TICK * weight * (std::pow(hotKelvin, 4) - std::pow(coldKelvin, 4));
    }

    // 1 辺 s の正方形が距離 d で向かい合う時の見え方の割合(平行・同軸。Incropera の表の式)
    double ParallelSquaresView(double side, double distance) {
        const double x = side / distance;
        const double x2 = x * x;
        const double term1 = std::log(std::sqrt((1 + x2) * (1 + x2) / (1 + 2 * x2)));
        const double term2 = 2 * x * std::sqrt(1 + x2) * std::atan(x / std::sqrt(1 + x2));
        const double term3 = 2 * x * std::atan(x);

        return 2 / (std::numbers::pi * x2) * (term1 + term2 - term3);
    }

    RadiationSurface Surface(uint32_t temperatureMk, uint64_t capacity = HUGE_CAPACITY,
                             uint32_t absorptance = ONE_Q16) {
        return {
            .temperatureMk = temperatureMk, .heatCapacityNanojoulesPerKelvin = capacity, .absorptanceQ16 = absorptance};
    }

    uint32_t Q16(double value) {
        return static_cast<uint32_t>(value * ONE_Q16 + 0.5);
    }

    // --- 試験 ---

    void TestBlackPlatesMatchStefanBoltzmann() {
        // 1500 K と 300 K の黒い板が全部見えている(F = 1): 1,194,180.85 mJ/刻み
        const RadiationPair full{.first = 0, .second = 1, .viewQ16 = ONE_Q16, .shareQ16 = ONE_Q16};
        const int64_t flow = RadiationPairFlow(Surface(1500000), Surface(300000), full);
        std::printf("黒い板 1500 K → 300 K(F = 1): %lld mJ/刻み(解析解 1194180.85)\n", static_cast<long long>(flow));
        EXPECT(std::abs(static_cast<double>(flow) - 1194180.85) < 1194180.85 * 1e-4);

        // 温度の組を広く: 相対誤差 1e-3 以内(流れが 100 mJ 以上の所)
        double worst = 0;
        for (const uint32_t hot : {320000u, 600000u, 900000u, 1200000u, 2000000u, 4000000u, 20000000u}) {
            for (const uint32_t cold : {0u, 1000u, 273150u, 300000u, 310000u, 590000u}) {
                if (cold >= hot)
                    continue;

                const double expected = ReferenceFlow(hot / 1000.0, cold / 1000.0, 1.0);
                const int64_t actual = RadiationPairFlow(Surface(hot), Surface(cold), full);
                if (expected < 100)
                    continue;

                // 流れの mJ への切り捨て(1 mJ 未満)を除いた相対誤差
                const double error = std::max(0.0, std::abs(static_cast<double>(actual) - expected) - 1.0);
                worst = std::max(worst, error / expected);
            }
        }
        std::printf("温度の組の最大の相対誤差(1 mJ の切り捨てを除く): %.2e\n", worst);
        EXPECT(worst < 1e-4);
    }

    void TestDistanceThroughViewFactor() {
        // 0.5 m 角の黒い面が 0.5〜8 m 離れて向かい合う。弱まり方は見え方の割合だけで決まる
        int64_t previous = INT64_MAX;
        for (const double distance : {0.5, 1.0, 2.0, 4.0, 8.0}) {
            const double view = ParallelSquaresView(0.5, distance);
            const RadiationPair pair{.first = 0, .second = 1, .viewQ16 = Q16(view), .shareQ16 = ONE_Q16};
            const int64_t flow = RadiationPairFlow(Surface(1200000), Surface(300000), pair);
            const double expected = ReferenceFlow(1200, 300, Q16(view) / static_cast<double>(ONE_Q16));
            std::printf("距離 %.1f m: F = %.5f・流れ %lld mJ/刻み(参照 %.1f)\n", distance, view,
                        static_cast<long long>(flow), expected);
            EXPECT(flow < previous);
            EXPECT(std::abs(static_cast<double>(flow) - expected) <= expected * 1e-3 + 2);
            previous = flow;
        }
    }

    void TestAntisymmetricAndDownhill() {
        std::mt19937_64 random(28);
        std::uniform_int_distribution<uint32_t> temperature(0, 6000000);
        std::uniform_int_distribution<uint32_t> fraction(0, ONE_Q16);
        std::uniform_int_distribution<uint64_t> capacity(1, uint64_t{1} << 40);
        int mismatches = 0;
        int uphill = 0;
        for (int trial = 0; trial < 200000; ++trial) {
            const RadiationSurface a = Surface(temperature(random), capacity(random), fraction(random));
            const RadiationSurface b = Surface(trial % 7 == 0 ? a.temperatureMk : temperature(random), capacity(random),
                                               fraction(random));
            const RadiationPair forward{
                .first = 0, .second = 1, .viewQ16 = fraction(random), .shareQ16 = fraction(random)};
            const RadiationPair backward{
                .first = 1, .second = 0, .viewQ16 = forward.viewQ16, .shareQ16 = forward.shareQ16};
            const int64_t there = RadiationPairFlow(a, b, forward);
            const int64_t back = RadiationPairFlow(b, a, backward);
            if (there != -back)
                ++mismatches;

            // 坂を下る: 流れの向きは温度の差の向きと同じ(等しければ 0)
            const bool wrong = (a.temperatureMk > b.temperatureMk && there < 0) ||
                               (a.temperatureMk < b.temperatureMk && there > 0) ||
                               (a.temperatureMk == b.temperatureMk && there != 0);
            if (wrong)
                ++uphill;
        }
        std::printf("入れ替えの食い違い %d・坂を上った対 %d(20 万組)\n", mismatches, uphill);
        EXPECT(mismatches == 0);
        EXPECT(uphill == 0);
    }

    // 面のエネルギー(mJ)と熱容量(mJ/K の整数倍)から温度(mK)を出す小さな世界
    struct PlateWorld {
        std::vector<int64_t> energies;             // mJ
        std::vector<int64_t> capacitiesPerKelvin;  // mJ/K
        std::vector<uint32_t> absorptances;        // Q16

        uint32_t TemperatureMk(size_t i) const {
            const int64_t milliKelvin = energies[i] * 1000 / capacitiesPerKelvin[i];
            return static_cast<uint32_t>(std::clamp<int64_t>(milliKelvin, 0, INT32_MAX));
        }

        std::vector<RadiationSurface> Surfaces() const {
            std::vector<RadiationSurface> surfaces;
            for (size_t i = 0; i < energies.size(); ++i)
                surfaces.push_back(
                    {.temperatureMk = TemperatureMk(i),
                     .heatCapacityNanojoulesPerKelvin = static_cast<uint64_t>(capacitiesPerKelvin[i]) * 1000000,
                     .absorptanceQ16 = absorptances[i]});

            return surfaces;
        }

        int64_t Total() const {
            int64_t total = 0;
            for (const int64_t energy : energies)
                total += energy;

            return total;
        }
    };

    // 面 count 個のあいだに対をばらまく。どの面の分け前の合計も 65536 以下
    std::vector<RadiationPair> RandomPairs(uint32_t count, uint32_t pairsPerSurface, std::mt19937_64& random) {
        std::vector<RadiationPair> pairs;
        std::uniform_int_distribution<uint32_t> pick(0, count - 1);
        std::uniform_int_distribution<uint32_t> view(0, ONE_Q16);
        std::vector<uint32_t> used(count, 0);
        const uint32_t share = ONE_Q16 / (2 * pairsPerSurface);
        while (pairs.size() < static_cast<size_t>(count) * pairsPerSurface / 2) {
            const uint32_t a = pick(random);
            const uint32_t b = pick(random);
            if (a == b || used[a] + share > ONE_Q16 || used[b] + share > ONE_Q16)
                continue;

            used[a] += share;
            used[b] += share;
            pairs.push_back({.first = a, .second = b, .viewQ16 = view(random), .shareQ16 = share});
        }

        return pairs;
    }

    void Step(PlateWorld& world, const std::vector<RadiationPair>& pairs) {
        const std::vector<int64_t> changes = ExchangeRadiation(world.Surfaces(), pairs);
        for (size_t i = 0; i < changes.size(); ++i)
            world.energies[i] += changes[i];
    }

    void TestConservationOrderAndBounds() {
        std::mt19937_64 random(583);
        constexpr uint32_t COUNT = 24;
        PlateWorld world;
        std::uniform_int_distribution<int64_t> kelvin(250, 2500);
        std::uniform_int_distribution<int64_t> capacity(1, 400);  // 小さい熱容量も混ぜて上限を効かせる
        std::uniform_int_distribution<uint32_t> absorptance(0, ONE_Q16);
        for (uint32_t i = 0; i < COUNT; ++i) {
            const int64_t c = capacity(random);
            world.capacitiesPerKelvin.push_back(c);
            world.energies.push_back(c * kelvin(random));
            world.absorptances.push_back(i % 5 == 0 ? 0 : absorptance(random));  // 放射しない面も混ぜる
        }

        const std::vector<RadiationPair> pairs = RandomPairs(COUNT, 6, random);
        std::vector<RadiationPair> shuffled = pairs;
        std::ranges::shuffle(shuffled, random);
        std::ranges::reverse(shuffled);

        uint32_t lowest = UINT32_MAX;
        uint32_t highest = 0;
        for (uint32_t i = 0; i < COUNT; ++i) {
            lowest = std::min(lowest, world.TemperatureMk(i));
            highest = std::max(highest, world.TemperatureMk(i));
        }

        PlateWorld other = world;
        const int64_t total = world.Total();
        bool conserved = true;
        bool same = true;
        bool bounded = true;
        for (int tick = 0; tick < 3600; ++tick) {
            Step(world, pairs);
            Step(other, shuffled);
            conserved = conserved && world.Total() == total;
            same = same && world.energies == other.energies;
            for (uint32_t i = 0; i < COUNT; ++i) {
                // 温度の切り捨て(energy × 1000 ÷ C)の分だけ ±1 mK の幅を見る
                const uint32_t t = world.TemperatureMk(i);
                bounded = bounded && t + 1 >= lowest && t <= highest + 1;
            }
        }

        uint32_t spreadAfter = 0;
        for (uint32_t i = 0; i < COUNT; ++i) {
            if (world.absorptances[i] == 0)
                continue;
            for (uint32_t j = 0; j < COUNT; ++j) {
                if (world.absorptances[j] != 0)
                    spreadAfter = std::max(
                        spreadAfter, world.TemperatureMk(i) - std::min(world.TemperatureMk(i), world.TemperatureMk(j)));
            }
        }
        std::printf(
            "24 面・72 対・3600 刻み: 保存 %s・並びによらない %s・初めの温度の範囲の中 %s(放射する面の温度差の最大 %u "
            "mK)\n",
            conserved ? "はい" : "いいえ", same ? "はい" : "いいえ", bounded ? "はい" : "いいえ", spreadAfter);
        EXPECT(conserved);
        EXPECT(same);
        EXPECT(bounded);
    }

    void TestNoOvershootWithSmallCapacity() {
        // 熱容量 1 mJ/K の 2 枚(ほぼ何も持たない薄い膜)。上限が無ければ 1 刻みで行き過ぎる大きさ
        PlateWorld world;
        world.capacitiesPerKelvin = {1, 3};
        world.energies = {3000, 3 * 300};
        world.absorptances = {ONE_Q16, ONE_Q16};
        const std::vector<RadiationPair> pairs = {{.first = 0, .second = 1, .viewQ16 = ONE_Q16, .shareQ16 = ONE_Q16}};
        const int64_t total = world.Total();

        const uint64_t unlimited = reaction::RdPairCoefficient(3000000, 300000, uint64_t{1} << 32);
        const uint64_t limit = reaction::RdPairLimit(1000000, 3000000, ONE_Q16);
        std::printf("薄い膜: 上限なしの係数 %llu・上限 %llu\n", static_cast<unsigned long long>(unlimited),
                    static_cast<unsigned long long>(limit));
        EXPECT(unlimited > limit);  // 上限が効く場面であること

        bool ordered = true;
        bool monotone = true;
        uint32_t hotBefore = world.TemperatureMk(0);
        uint32_t coldBefore = world.TemperatureMk(1);
        for (int tick = 0; tick < 600; ++tick) {
            Step(world, pairs);
            const uint32_t hot = world.TemperatureMk(0);
            const uint32_t cold = world.TemperatureMk(1);
            ordered = ordered && hot + 1 >= cold;
            monotone = monotone && hot <= hotBefore && cold >= coldBefore;
            hotBefore = hot;
            coldBefore = cold;
        }
        std::printf("薄い膜 600 刻み: 順序 %s・単調 %s・最後 %u / %u mK\n", ordered ? "保つ" : "崩れた",
                    monotone ? "はい" : "いいえ", hotBefore, coldBefore);
        EXPECT(ordered && monotone);
        EXPECT(world.Total() == total);
        // 釣り合いへ近づいた(初めの差 2700 K。熱い膜は 1 mJ = 1 K の刻みでしか温度を持てないので、数 K で止まる)
        EXPECT(hotBefore - coldBefore <= 5000);
    }

    // --- パッケージの欄 → ベイク → セルの吸収率 ---

    std::expected<BakedReactionTable, std::string> BakeWithMod(const std::string& init) {
        auto sandbox = LuauSandbox::Create(SandboxLimits{});
        if (!sandbox)
            return std::unexpected(sandbox.error());

        (*sandbox)->Seal();
        std::vector<PackageSource> packages = ReadCombustionTestPackages();
        PackageSource mod{.name = "radiation_mod", .files = {}};
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

    reaction::RxCell CellOf(uint32_t species, uint64_t amountMicromoles) {
        reaction::RxCell cell{};
        cell.speciesCount = 1;
        cell.species[0] = species;
        cell.amounts[0] = amountMicromoles;

        return cell;
    }

    void TestPackageField() {
        // 草の束のような物(0.5 m²/mol)と、欄を書かない物
        const std::string init =
            "return { species = { "
            "dark_test = { composition = { H = 2 }, formation_enthalpy_j_per_mol = 0, heat_capacity_mj_per_mol_k = "
            "28836, "
            "thermal_conductivity_mw_per_m_k = 1000, radiation_absorption_mm2_per_mol = 500000 }, "
            "clear_test = { composition = { H = 2 }, formation_enthalpy_j_per_mol = 0, heat_capacity_mj_per_mol_k = "
            "28836, "
            "thermal_conductivity_mw_per_m_k = 1000 } } }";
        const auto baked = BakeWithMod(init);
        if (!baked) {
            std::printf("FAILED: ベイクできない: %s\n", baked.error().c_str());
            ++failureCount;

            return;
        }

        const uint32_t dark = baked->SpeciesId("dark_test");
        const uint32_t clear = baked->SpeciesId("clear_test");
        EXPECT(dark != 0 && clear != 0);
        EXPECT(baked->speciesRadiationAbsorption.size() == baked->speciesNames.size());
        EXPECT(baked->speciesRadiationAbsorption[dark] == 500000 && baked->speciesRadiationAbsorption[clear] == 0);

        // 1.2 mol → τ = 0.5 × 1.2 ÷ 0.25 = 2.4 → α = 1 − e^−2.4 = 0.90928
        const uint32_t grass = CellAbsorptanceQ16(*baked, CellOf(dark, 1200000));
        std::printf("草の束 1.2 mol の吸収率: %u / 65536(参照 %.1f)\n", grass, 0.9092820467 * ONE_Q16);
        EXPECT(std::abs(static_cast<double>(grass) - 0.9092820467 * ONE_Q16) <= 3);
        EXPECT(CellAbsorptanceQ16(*baked, CellOf(clear, 5000000)) == 0);
        EXPECT(CellAbsorptanceQ16(*baked, CellOf(dark, 0)) == 0);
        EXPECT(CellAbsorptanceQ16(*baked, CellOf(dark, uint64_t{1} << 40)) == ONE_Q16);  // 厚い塊は黒い

        // 吸収率は量について単調
        uint32_t previous = 0;
        bool monotone = true;
        for (uint64_t amount = 0; amount <= 40000000; amount += 50000) {
            const uint32_t value = CellAbsorptanceQ16(*baked, CellOf(dark, amount));
            monotone = monotone && value >= previous;
            previous = value;
        }
        EXPECT(monotone);

        // 試験の表(combustion_test)の物質は欄を書いていない → すべて 0(今までの世界は変わらない)
        bool allZero = true;
        for (uint32_t id = 0; id < baked->speciesNames.size(); ++id) {
            if (id != dark)
                allZero = allZero && baked->speciesRadiationAbsorption[id] == 0;
        }
        EXPECT(allZero);

        // 負の値は誤り
        const auto negative = BakeWithMod(
            "return { species = { bad_test = { composition = { H = 2 }, formation_enthalpy_j_per_mol = 0, "
            "heat_capacity_mj_per_mol_k = 28836, thermal_conductivity_mw_per_m_k = 1000, "
            "radiation_absorption_mm2_per_mol = -1 } } }");
        EXPECT(!negative && negative.error().find("radiation_absorption_mm2_per_mol") != std::string::npos);
    }

}  // namespace

int main() {
    TestBlackPlatesMatchStefanBoltzmann();
    TestDistanceThroughViewFactor();
    TestAntisymmetricAndDownhill();
    TestConservationOrderAndBounds();
    TestNoOvershootWithSmallCapacity();
    TestPackageField();
    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);

        return 1;
    }

    std::printf("radiation_reference: すべて通過\n");

    return 0;
}
