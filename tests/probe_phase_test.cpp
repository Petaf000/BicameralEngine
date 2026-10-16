// probe_phase_test.cpp — 水の相変化を反応表の規則で書く(T-0611・ADR-0272)の試験。仮の世界の CPU リファレンス
// (ProbeReference に気体と液体の区画)で、標準パッケージ real_matter・real_reactions の水の 3 相の見本を走らせる。
//
// 確かめること:
//   1. 表: 反応物が全部次数 0 の規則がベイクできる。融解と凝固の速さが 273〜274 K で入れ替わる・蒸発と凝縮の釣り合いの湯気が
//      飽和水蒸気の濃さ(373 K で 1 気圧 = 0.125 m³ に 4.07 mol)の 5% 以内・潜熱(物質の生成エンタルピーと比熱から)が
//      融解 6.010 kJ/mol(273.15 K)・蒸発 44.004 kJ/mol(298.15 K)
//   2. 湯気: 熱い石の上の水たまりが湯気になり、湯気が気体の区画で上がる。湯気は冷たい池まで運ばれて凝縮する(池の水が増える)
//   3. 氷: 暖かい台の上の氷が溶け、溶けた水が液体の区画で台から流れ落ちる
//   4. 保存(湯気の場面は気体の区画だけで元素だけ〔エネルギーは崩れる刻みを表示。チケットの「重なり」〕・氷の場面は気体と液体の区画で両方): 1 秒ごとに、元素の数 − 気体の帳簿・エネルギー(セル + 気体の運動エネルギーのぶん − 気体の帳簿。潜熱は化学のエネルギーに
//      入っている)がビット単位で一定。決定性: 2 回の実行でセル・気体・液体のハッシュが一致
//   5. 記録(試験ではなく表示): 水と湯気が釣り合ったセルは眠らない(蒸発と凝縮が毎刻み行き来する。ADR-0272 の注)・
//      湯気の場面に液体の区画も重ねると保存が崩れる刻み(T-0611 のチケットの「重なり」)
// 表は試験の表(combustion_test。real_matter・real_reactions を写して熱伝導率に試験の倍率)+ 地球の環境 + 基準の大気(N2・O2)+ 石。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "reaction_test_table.h"
#include "sim/probe_gas.h"
#include "sim/probe_liquid.h"
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

    constexpr int32_t AIR_TEMPERATURE = 300000;  // mK
    // µmol(0.125 m³ の岩 ≒ 337 kg を C の 12 g/mol で数えたもの。密度 2000 でセルを満たす)
    constexpr uint64_t STONE = 28000000000;
    constexpr uint64_t TICKS_PER_SECOND = 60;

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
        definition.species.push_back(
            {.name = "stone",
             .composition = {{.element = "C", .count = 1}},
             .formationEnthalpy = 0,
             .heatCapacity = 8500,
             .thermalConductivity = 2000000,
             .condensed = true,
             .density = 2000});  // セルを満たす石は液体と気体の壁(T-0593・ADR-0268。密度を書かない固体は場所を取らない)

        auto baked = BakeReactionTable(definition);
        if (!baked) {
            std::printf("表を作れない: %s\n", baked.error().c_str());
            std::exit(1);
        }

        return std::move(*baked);
    }

    uint32_t RuleId(const BakedReactionTable& table, std::string_view name) {
        const auto found = std::ranges::find(table.ruleNames, name);
        if (found == table.ruleNames.end()) {
            std::printf("規則 %.*s が無い\n", (int)name.size(), name.data());
            std::exit(1);
        }

        return (uint32_t)(found - table.ruleNames.begin());
    }

    // 速さの表の値(仮数 × 2^指数。1 刻みあたり)を倍精度に(試験の表示と比べだけ)
    double RateValue(const BakedReactionTable& table, uint32_t rule, uint32_t kelvin) {
        const uint64_t packed = table.View().Rate(rule, kelvin);
        const auto exponent = (int32_t)(uint32_t)(packed & 0xFFFFFFFFull);

        return std::ldexp((double)(packed >> 32), exponent);
    }

    // --- 1: 表 ---
    void TestTable(const BakedReactionTable& table) {
        std::printf("表\n");
        const uint32_t melting = RuleId(table, "ice_melting");
        const uint32_t freezing = RuleId(table, "water_freezing");
        const uint32_t evaporation = RuleId(table, "water_evaporation");
        const uint32_t condensation = RuleId(table, "water_condensation");
        EXPECT(table.rules[melting].firstOrderMask != 0 && table.rules[freezing].firstOrderMask != 0);
        EXPECT(table.rules[evaporation].firstOrderMask == 0);

        // 融点: 273 K までは凝固、274 K からは融解が速い(表は 1 K ごと)
        EXPECT(RateValue(table, melting, 273) < RateValue(table, freezing, 273));
        EXPECT(RateValue(table, melting, 274) > RateValue(table, freezing, 274));
        std::printf("  融解 ÷ 凝固: 272 K %.3f・273 K %.3f・274 K %.3f・275 K %.3f\n",
                    RateValue(table, melting, 272) / RateValue(table, freezing, 272),
                    RateValue(table, melting, 273) / RateValue(table, freezing, 273),
                    RateValue(table, melting, 274) / RateValue(table, freezing, 274),
                    RateValue(table, melting, 275) / RateValue(table, freezing, 275));

        // 釣り合いの湯気(µmol / セル)= 蒸発(µmol/刻み)÷ 凝縮(1/刻み)。飽和水蒸気の濃さ P_sat / RT × 0.125 m³ と比べる
        // 飽和水蒸気圧(IAPWS-95 の 298.15・323.15・373.15 K の値を、表の 1 K の温度へ Clausius–Clapeyron で直したもの)
        constexpr std::array<std::pair<uint32_t, double>, 3> SATURATION = {
            {{298, 3141.7}, {323, 12261.0}, {373, 100885.0}}};
        for (const auto& [kelvin, pascals] : SATURATION) {
            const double equilibrium = RateValue(table, evaporation, kelvin) / RateValue(table, condensation, kelvin);
            const double expected = pascals / (8.314462618 * kelvin) * 0.125 * 1e6;
            std::printf("  釣り合いの湯気 %u K: %.4f mol(飽和 %.4f mol)\n", kelvin, equilibrium * 1e-6,
                        expected * 1e-6);
            EXPECT(std::abs(equilibrium / expected - 1.0) < 0.05);
        }

        // 潜熱(mJ/mol)= h0 の差 × 1000 + 比熱の差 × 温度
        auto latent = [&](std::string_view from, std::string_view to, int64_t milliKelvin) {
            const reaction::RxSpecies a = table.species[table.SpeciesId(from)];
            const reaction::RxSpecies b = table.species[table.SpeciesId(to)];

            return ((b.h0 - a.h0) * 1000) + (((int64_t)b.heatCapacity - (int64_t)a.heatCapacity) * milliKelvin / 1000);
        };
        const int64_t fusion = latent("ice", "water", 273150);
        const int64_t vaporization = latent("water", "water_vapor", 298150);
        const int64_t boiling = latent("water", "water_vapor", 373150);
        std::printf("  潜熱: 融解 %lld mJ/mol(273.15 K)・蒸発 %lld(298.15 K)・%lld(373.15 K)\n", (long long)fusion,
                    (long long)vaporization, (long long)boiling);
        EXPECT(fusion > 6009000 && fusion < 6011000);
        EXPECT(vaporization > 44003000 && vaporization < 44005000);
    }

    // --- 区画とセル ---
    struct Region {
        std::array<uint32_t, 3> minCell;
        std::array<uint32_t, 3> sizeCells;
    };

    uint32_t GridIndex(const Region& region, uint32_t x, uint32_t y, uint32_t z) {
        return (region.minCell[0] + x) + (PROBE_GRID_SIZE * (region.minCell[1] + y)) +
               (PROBE_SLICE_CELL_COUNT * (region.minCell[2] + z));
    }

    struct CellSpec {
        int32_t temperature = AIR_TEMPERATURE;
        uint64_t stone = 0;
        uint64_t water = 0;
        uint64_t ice = 0;
    };

    // 基準の大気(300 K)を同じ圧力で温度に合わせた空気 + 中身
    reaction::RxCell MakeCell(const BakedReactionTable& table, const reaction::RxCell& air, const CellSpec& spec) {
        std::vector<SpeciesAmount> amounts;
        for (uint32_t i = 0; i < air.speciesCount; ++i)
            amounts.push_back({.species = air.species[i],
                               .amount = air.amounts[i] * (uint64_t)AIR_TEMPERATURE / (uint64_t)spec.temperature});

        const std::array<std::pair<const char*, uint64_t>, 3> extras = {
            {{"stone", spec.stone}, {"water", spec.water}, {"ice", spec.ice}}};
        for (const auto& [name, amount] : extras) {
            if (amount != 0)
                amounts.push_back({.species = table.SpeciesId(name), .amount = amount});
        }

        std::ranges::sort(amounts, {}, &SpeciesAmount::species);

        return MakeReactionCell(table, amounts, spec.temperature);
    }

    template <typename SpecOf>
    ProbeReference MakeReference(const BakedReactionTable& table, const Region& region, SpecOf specOf,
                                 bool liquid = true) {
        auto gas = ProbeGasField::Create(table, {.minCell = region.minCell, .sizeCells = region.sizeCells});
        if (!gas) {
            std::printf("気体の区画を作れない: %s\n", gas.error().c_str());
            std::exit(1);
        }

        std::vector<reaction::RxCell> cells = MakeBlankProbeWorld();
        for (uint32_t z = 0; z < region.sizeCells[2]; ++z) {
            for (uint32_t y = 0; y < region.sizeCells[1]; ++y) {
                for (uint32_t x = 0; x < region.sizeCells[0]; ++x)
                    cells[GridIndex(region, x, y, z)] = MakeCell(
                        table, gas->ReferenceCell(table, region.minCell[1] + y), specOf(x, y));
            }
        }

        ProbeReference reference(table, cells);
        EXPECT(reference.EnableGas({.minCell = region.minCell, .sizeCells = region.sizeCells, .randomSeed = 7})
                   .has_value());
        if (liquid)
            EXPECT(reference.EnableLiquid({.minCell = region.minCell, .sizeCells = region.sizeCells}).has_value());

        return reference;
    }

    // --- 見るもの ---
    // 区画の x の範囲・y の範囲の物質の量(µmol)
    uint64_t AmountIn(const BakedReactionTable& table, const ProbeReference& reference, uint64_t tick,
                      const Region& region, std::string_view name, std::array<uint32_t, 2> xRange,
                      std::array<uint32_t, 2> yRange) {
        const uint32_t species = table.SpeciesId(name);
        const std::span<const reaction::RxCell> cells = reference.State(tick);
        uint64_t total = 0;
        for (uint32_t z = 0; z < region.sizeCells[2]; ++z) {
            for (uint32_t y = yRange[0]; y < yRange[1]; ++y) {
                for (uint32_t x = xRange[0]; x < xRange[1]; ++x) {
                    const reaction::RxCell& cell = cells[GridIndex(region, x, y, z)];
                    for (uint32_t i = 0; i < cell.speciesCount; ++i) {
                        if (cell.species[i] == species)
                            total += cell.amounts[i];
                    }
                }
            }
        }

        return total;
    }

    // 物質の重心の高さ(区画の y。千分の 1 セル)
    int64_t CentroidMilliCells(const BakedReactionTable& table, const ProbeReference& reference, uint64_t tick,
                               const Region& region, std::string_view name) {
        uint64_t weight = 0;
        uint64_t moment = 0;
        for (uint32_t y = 0; y < region.sizeCells[1]; ++y) {
            const uint64_t layer = AmountIn(table, reference, tick, region, name, {0, region.sizeCells[0]},
                                            {y, y + 1}) /
                                   1000;
            weight += layer;
            moment += layer * ((uint64_t)y * 1000 + 500);
        }

        return weight == 0 ? -1 : (int64_t)(moment / weight);
    }

    // 保存の合計: 元素 − 気体の帳簿・エネルギー(セル + 気体の運動エネルギーのぶん − 気体の帳簿
    // + 液体の位置と運動のエネルギー − 液体の帳簿。液体の区画を載せた時。T-0610・ADR-0276)
    struct Totals {
        std::vector<uint64_t> elements;
        int64_t energy = 0;

        bool operator==(const Totals&) const = default;
    };

    Totals Sum(const BakedReactionTable& table, const ProbeReference& reference, uint64_t tick) {
        Totals totals;
        totals.elements.assign(table.elementNames.size(), 0);
        for (const reaction::RxCell& cell : reference.State(tick)) {
            if (cell.speciesCount == 0)
                continue;

            totals.energy += cell.energy;
            const std::vector<uint64_t> elements = CountElements(table, cell);
            for (size_t e = 0; e < elements.size(); ++e)
                totals.elements[e] += elements[e];
        }

        const ProbeGasField* gas = reference.GasField();
        for (size_t species = 0; species < gas->Ledger().amounts.size(); ++species) {
            for (size_t e = 0; e < totals.elements.size(); ++e)
                totals.elements[e] -= (uint64_t)gas->Ledger().amounts[species] *
                                      table.speciesElements[(species * table.elementNames.size()) + e];
        }

        totals.energy += gas->KineticEnergySum() - gas->Ledger().energy;
        if (const ProbeLiquidField* liquid = reference.LiquidField())
            totals.energy += liquid->PotentialEnergySum() + liquid->KineticEnergySum() - liquid->Ledger().energy;

        return totals;
    }

    uint64_t RunHash(const ProbeReference& reference, uint64_t tick) {
        return reference.StateHash(tick) ^ reference.GasField()->Hash() ^ (reference.LiquidField()->Hash() * 3);
    }

    // 1 秒ごとに保存を確かめながら進める。perSecond(tick) は 1 秒ごとの表示。
    // energyHolds = false なら、エネルギーの崩れは表示だけ(元素は必ず確かめる。湯気の場面の既知の崩れ。T-0611 の「重なり」)
    template <typename PerSecond>
    void Run(const BakedReactionTable& table, ProbeReference& reference, uint64_t ticks, PerSecond perSecond,
             bool energyHolds = true) {
        const Totals before = Sum(table, reference, 0);
        bool conserved = true;
        bool elementsConserved = true;
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            reference.Advance(tick, {});
            if ((tick + 1) % TICKS_PER_SECOND != 0)
                continue;

            const Totals now = Sum(table, reference, tick + 1);
            if (conserved && !(now == before)) {
                std::printf("  保存が崩れた(%llu 刻み): エネルギー %+lld mJ", (unsigned long long)(tick + 1),
                            (long long)(now.energy - before.energy));
                for (size_t e = 0; e < now.elements.size(); ++e)
                    std::printf("・%s %+lld", table.elementNames[e].c_str(),
                                (long long)(now.elements[e] - before.elements[e]));
                std::printf("・気体の報告 溢れ %d\n", reference.LastGasReport().skippedCellOverflow ? 1 : 0);
            }

            conserved = conserved && now == before;
            elementsConserved = elementsConserved && now.elements == before.elements;
            perSecond(tick + 1);
        }

        EXPECT(elementsConserved);
        EXPECT(conserved || !energyHolds);
    }

    // --- 2: 熱い石の上の水たまりが湯気になって上がり、冷たい池で凝縮する ---
    //   区画 12 × 8 × 3(6 m × 4 m × 1.5 m・閉じた壁)。y 0 は石(x 0〜3 は 700 K の熱い石・x 8〜11 は 275 K の冷たい石)。
    //   y 1: x 0〜3 に水たまり(300 mol/セル・360 K)・x 4 と x 7 は石の堤・x 8〜11 に冷たい池(3000 mol/セル・275 K)。ほかは空気
    constexpr Region STEAM_REGION = {.minCell = {26, 0, 30}, .sizeCells = {12, 8, 3}};
    constexpr uint64_t STEAM_TICKS = 1200;  // 20 秒

    CellSpec SteamCell(uint32_t x, uint32_t y) {
        if (y == 0)
            return {.temperature = x < 4 ? 700000 : (x >= 8 ? 275000 : AIR_TEMPERATURE), .stone = STONE};

        if (y == 1 && (x == 4 || x == 7))
            return {.stone = STONE};

        if (y == 1 && x < 4)
            return {.temperature = 360000, .water = 300000000};

        if (y == 1 && x >= 8)
            return {.temperature = 275000, .water = 3000000000};

        return {};
    }

    void TestSteam(const BakedReactionTable& table) {
        std::printf("湯気(熱い石の上の水たまり → 冷たい池)\n");
        const Region& region = STEAM_REGION;
        // 液体の区画は載せない(水たまりと池は石の堤で囲み、流れなくてよい。保存が崩れる刻みは ReportSteamBreak)
        ProbeReference reference = MakeReference(table, region, SteamCell, false);

        const uint64_t puddleBefore = AmountIn(table, reference, 0, region, "water", {0, 4}, {1, 2});
        const uint64_t pondBefore = AmountIn(table, reference, 0, region, "water", {8, 12}, {0, 8});
        uint64_t vaporHighMax = 0;
        int64_t centroidMax = -1;
        Run(
            table, reference, STEAM_TICKS,
            [&](uint64_t tick) {
                const uint64_t vapor = AmountIn(table, reference, tick, region, "water_vapor", {0, 12}, {0, 8});
                const uint64_t vaporHigh = AmountIn(table, reference, tick, region, "water_vapor", {0, 12}, {4, 8});
                const int64_t centroid = CentroidMilliCells(table, reference, tick, region, "water_vapor");
                const uint64_t puddle = AmountIn(table, reference, tick, region, "water", {0, 4}, {0, 8});
                const uint64_t pond = AmountIn(table, reference, tick, region, "water", {8, 12}, {0, 8});
                const uint64_t pondVapor = AmountIn(table, reference, tick, region, "water_vapor", {8, 12}, {0, 8});
                vaporHighMax = std::max(vaporHighMax, vaporHigh);
                centroidMax = std::max(centroidMax, centroid);
                if (tick % (TICKS_PER_SECOND * 4) == 0)
                    std::printf(
                        "  %2llu 秒: 水たまり %.2f mol・湯気 %.2f mol(y 4 以上 %.2f・重心 y %.2f)・池 %+.3f "
                        "mol(池の上の湯気 %.3f)\n",
                        (unsigned long long)(tick / TICKS_PER_SECOND), (double)puddle * 1e-6, (double)vapor * 1e-6,
                        (double)vaporHigh * 1e-6, (double)centroid * 1e-3, ((double)pond - (double)pondBefore) * 1e-6,
                        (double)pondVapor * 1e-6);
            },
            false);

        const uint64_t puddleAfter = AmountIn(table, reference, STEAM_TICKS, region, "water", {0, 4}, {0, 8});
        const uint64_t pondAfter = AmountIn(table, reference, STEAM_TICKS, region, "water", {8, 12}, {0, 8});
        // 水たまりの 3 割より多くが湯気になった(T-0593 のマージで石が気体を塞ぎ、熱い石の中を通る空気が水たまりを温めなくなった。
        // 20 秒で 3600 → 2428 mol。前は 1865 mol)
        EXPECT(puddleAfter < puddleBefore / 10 * 7);
        EXPECT(vaporHighMax > 0);        // 湯気が y 4(2 m)より上へ
        EXPECT(centroidMax > 2500);      // 湯気の重心が水たまり(y 1)より 1.5 セル以上高い
        EXPECT(pondAfter > pondBefore);  // 池の水が増えた(湯気が冷たい池で凝縮した)
    }

    // 記録(試験ではなく表示): 湯気の場面で、エネルギーが最初に崩れる刻みを 1 刻みずつ探す(T-0646 が使う。T-0611 のチケットの「重なり」)。
    //   liquid = false: 気体の区画だけ(TestSteam と同じ。1 秒ごとの確かめでは 901〜960 刻みのどこかで +54 kJ)。
    //   liquid = true: 液体の区画も重ねる(液体の位置と運動のエネルギーを数えれば〔T-0610〕660 刻みまで保存する)。
    //   崩れた刻みの気体の帳簿と運動エネルギーのぶんも出す(反応が気体の質量を変えた刻みとの突き合わせ用)
    void ReportSteamBreak(const BakedReactionTable& table, bool liquid, uint64_t firstChecked, uint64_t lastTick) {
        std::printf("記録: 湯気の場面(液体の区画 %s)で保存が崩れる刻み\n", liquid ? "あり" : "なし");
        ProbeReference reference = MakeReference(table, STEAM_REGION, SteamCell, liquid);
        const Totals before = Sum(table, reference, 0);
        for (uint64_t tick = 0; tick < lastTick; ++tick) {
            reference.Advance(tick, {});
            if (tick + 1 < firstChecked)
                continue;

            const Totals now = Sum(table, reference, tick + 1);
            if (!(now == before)) {
                std::printf("  %llu 刻み: エネルギー %+lld mJ・気体の帳簿 %lld・運動 %lld\n",
                            (unsigned long long)(tick + 1), (long long)(now.energy - before.energy),
                            (long long)reference.GasField()->Ledger().energy,
                            (long long)reference.GasField()->KineticEnergySum());
                return;
            }
        }

        std::printf("  %llu 刻みまで保存\n", (unsigned long long)lastTick);
    }

    // --- 3: 暖かい台の上の氷が溶けて流れ落ちる ---
    //   区画 12 × 6 × 3。y 0 は石(300 K)。x 0〜3 は台(石 y 1〜2・500 K)で、その上 y 3 に氷(400 mol/セル・270 K)。x 4〜11 は低い床
    constexpr Region ICE_REGION = {.minCell = {26, 10, 30}, .sizeCells = {12, 6, 3}};
    constexpr uint64_t ICE_TICKS = 1200;  // 20 秒

    CellSpec IceCell(uint32_t x, uint32_t y) {
        if (y == 0)
            return {.stone = STONE};

        if (x < 4 && y <= 2)
            return {.temperature = 500000, .stone = STONE};

        if (x < 4 && y == 3)
            return {.temperature = 270000, .ice = 400000000};

        return {};
    }

    void TestIce(const BakedReactionTable& table) {
        std::printf("氷(暖かい台の上 → 溶けて流れ落ちる)\n");
        const Region& region = ICE_REGION;
        ProbeReference reference = MakeReference(table, region, IceCell);

        const uint64_t iceBefore = AmountIn(table, reference, 0, region, "ice", {0, 12}, {0, 6});
        Run(table, reference, ICE_TICKS, [&](uint64_t tick) {
            if (tick % (TICKS_PER_SECOND * 3) != 0)
                return;

            const uint64_t ice = AmountIn(table, reference, tick, region, "ice", {0, 12}, {0, 6});
            const uint64_t plateau = AmountIn(table, reference, tick, region, "water", {0, 4}, {0, 6});
            const uint64_t floor = AmountIn(table, reference, tick, region, "water", {4, 12}, {0, 6});
            const uint64_t vapor = AmountIn(table, reference, tick, region, "water_vapor", {0, 12}, {0, 6});
            std::printf("  %2llu 秒: 氷 %.2f mol・台の水 %.2f・床の水 %.2f・湯気 %.3f mol\n",
                        (unsigned long long)(tick / TICKS_PER_SECOND), (double)ice * 1e-6, (double)plateau * 1e-6,
                        (double)floor * 1e-6, (double)vapor * 1e-6);
        });

        const uint64_t iceAfter = AmountIn(table, reference, ICE_TICKS, region, "ice", {0, 12}, {0, 6});
        const uint64_t floorAfter = AmountIn(table, reference, ICE_TICKS, region, "water", {4, 12}, {0, 6});
        EXPECT(iceAfter < iceBefore / 10);   // 9 割より多く溶けた
        EXPECT(floorAfter > iceBefore / 2);  // 溶けた水の半分より多くが台から流れ落ちた
    }

    // --- 4: 決定性 ---
    void TestDeterminism(const BakedReactionTable& table) {
        std::printf("決定性\n");
        std::array<uint64_t, 2> hashes = {};
        for (uint64_t& hash : hashes) {
            ProbeReference reference = MakeReference(table, ICE_REGION, IceCell);
            constexpr uint64_t TICKS = 240;
            for (uint64_t tick = 0; tick < TICKS; ++tick)
                reference.Advance(tick, {});

            hash = RunHash(reference, TICKS);
        }

        EXPECT(hashes[0] == hashes[1]);
    }

    // --- 5: 費用の記録(表示だけ): 静かな池のセルは眠るか ---
    void ReportQuietPond(const BakedReactionTable& table) {
        std::printf("静かな池(費用の記録)\n");
        const Region region = {.minCell = {26, 20, 30}, .sizeCells = {8, 3, 8}};
        ProbeReference reference = MakeReference(table, region, [](uint32_t, uint32_t y) {
            return y == 0 ? CellSpec{.stone = STONE} : (y == 1 ? CellSpec{.water = 3000000000} : CellSpec{});
        });

        constexpr uint64_t TICKS = 600;
        uint64_t scheduled = 0;
        for (uint64_t tick = 0; tick < TICKS; ++tick) {
            reference.Advance(tick, {});
            if (tick >= TICKS - TICKS_PER_SECOND)
                scheduled += reference.ScheduledBlocks();
        }

        std::printf("  10 秒目の 1 刻みあたりの計算したブロック %.1f(池は 8 × 8 セルの 1 層)\n",
                    (double)scheduled / (double)TICKS_PER_SECOND);
    }

}  // namespace

int main() {
    const BakedReactionTable table = MakeTable();
    TestTable(table);
    TestSteam(table);
    TestIce(table);
    TestDeterminism(table);
    ReportQuietPond(table);
    ReportSteamBreak(table, true, 480, 660);
    ReportSteamBreak(table, false, 900, 960);

    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("すべて通った\n");
    return 0;
}
