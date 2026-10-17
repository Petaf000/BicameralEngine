// probe_phase_test.cpp — 水の相変化を釣り合いの規則で書く(T-0611・ADR-0272 → T-0623・ADR-0290)の試験。仮の世界の CPU リファレンス
// (ProbeReference に気体と液体の区画)で、標準パッケージ real_matter・real_reactions の水の 3 相の見本を走らせる。
//
// 確かめること:
//   1. 表と 1 セル: 釣り合いの規則と逆向き(~逆)がベイクされ、坂の警告が出ない。①融点がちょうど(275 K の氷 → 273.150 K で止まる)
//      ②釣り合えば眠る ③霧(水の無い空気の湯気が凝縮)④種なしで凍り始める(270 K の水だけ)・沸点のまわりの湯気が釣り合いの濃さ・
//      潜熱(物質の生成エンタルピーと比熱から)が融解 6.010 kJ/mol(273.15 K)・蒸発 44.004 kJ/mol(298.15 K)
//   2. 湯気: 熱い石の上の水たまりが湯気になり、湯気が気体の区画で上がる。湯気は冷たい池まで運ばれて凝縮する(池の水が増える)
//   3. 氷: 暖かい台の上の氷が溶け、溶けた水が液体の区画で台から流れ落ちる
//   4. 保存(湯気の場面は気体の区画だけと、液体の区画も重ねた時・氷の場面は気体と液体の区画): 毎刻み、エネルギー(セル + 気体の
//      運動エネルギーのぶん − 気体の帳簿 + 液体の位置と運動のエネルギー − 液体の帳簿。潜熱は化学のエネルギーに入っている)が
//      ビット単位で一定で、物質の無いセルはエネルギーを持たない(T-0646・ADR-0286)。1 秒ごとに元素の数 − 気体の帳簿も一定。
//      決定性: 2 回の実行でセル・気体・液体のハッシュが一致
//   6. 場所を取らない石(`--stone-without-density`。ctest の probe_phase_porous_stone。T-0654): 石の密度を書かない(場所を取らない固体)と、
//      水たまり・池・溶けた水が熱い石のセルへ染み込んで湯気になる。場面の結果(湯気の高さ・池・流れ落ち)は見ず、4 の保存だけを確かめる
//   5. 記録(試験ではなく表示): 静かな池の計算したブロックの数(釣り合ったセルは眠れる。ADR-0290)
// 表は試験の表(combustion_test。real_matter・real_reactions を写して熱伝導率に試験の倍率)+ 地球の環境 + 基準の大気(N2・O2)+ 石。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "reaction_test_table.h"
#include "sim/probe_gas.h"
#include "sim/probe_liquid.h"
#include "sim/probe_sim.h"
#include "sim/reaction_slope.h"
#include "standard_world_physics.h"

using namespace bicameral;
using namespace bicameral::sim;

namespace {

    int failureCount = 0;
    bool outcomeChecks = true;  // 場面の結果も確かめる(場所を取らない石の時は保存だけ。T-0654)

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)
#define EXPECT_OUTCOME(condition) Expect(!outcomeChecks || (condition), #condition, __LINE__)

    constexpr int32_t AIR_TEMPERATURE = 300000;  // mK
    // µmol(0.125 m³ の岩 ≒ 337 kg を C の 12 g/mol で数えたもの。密度 2000 でセルを満たす)
    constexpr uint64_t STONE = 28000000000;
    constexpr uint64_t TICKS_PER_SECOND = 60;

    // writtenHeatCapacity: 気体の比熱を書いた値のまま定積として読む(T-0597 より前と比べる測り。`--written-heat-capacity`)
    // stoneWithoutDensity: 石の密度を書かない(場所を取らない固体の中へ水が入る。T-0654 の再現。`--stone-without-density`)
    BakedReactionTable MakeTable(bool writtenHeatCapacity, bool stoneWithoutDensity) {
        ReactionTableDefinition definition = MakeCombustionTestTable();
        definition.gasHeatCapacityAtConstantPressure = !writtenHeatCapacity;
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
        if (stoneWithoutDensity)
            definition.species.back().density = 0;

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

    // 1 セルを n 刻み進める(セルだけのブロック。待ちの丸め)
    reaction::RxLoneCell AdvanceCell(const BakedReactionTable& table, const reaction::RxCell& cell, uint32_t ticks) {
        return AdvanceLoneReactionCell(table, reaction::RxMakeLoneCell(cell, 0), 11, 1, ticks, 5);
    }

    uint64_t CellAmount(const BakedReactionTable& table, const reaction::RxCell& cell, std::string_view name) {
        const uint32_t id = table.SpeciesId(name);
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            if (cell.species[i] == id)
                return cell.amounts[i];
        }

        return 0;
    }

    int32_t CellTemperature(const BakedReactionTable& table, const reaction::RxCell& cell) {
        return reaction::RxComputeThermal(table.View(), cell).temperature;
    }

    // 釣り合ったセルは眠る: 次の刻みに進む規則が無く、起こす刻みも無い
    bool Sleeps(const BakedReactionTable& table, const reaction::RxLoneCell& lone, uint64_t tick) {
        const reaction::RxWaitStep step = StepReactionCellWait(table, lone.cell, 11, tick, lone.changedTick, 5);

        return step.wakeTick == reaction::RX_WAIT_NEVER && reaction::RxSameCell(step.cell, lone.cell);
    }

    // 釣り合いの湯気の濃さ(mol / セル)。real_reactions の water_evaporation の balance と同じ式(試験の比べだけ倍精度)
    double BalanceVapor(int32_t milliKelvin) {
        const double kelvin = milliKelvin * 1e-3;

        return 32.688 * std::exp(-39975.0 / 8.314462618 * ((1.0 / kelvin) - (1.0 / 373.15))) * 0.125;
    }

    // --- 1: 表と 1 セルの釣り合い(T-0623・ADR-0290)---
    void TestTable(const BakedReactionTable& table) {
        std::printf("表と 1 セルの釣り合い\n");
        for (const char* name : {"ice_melting", "ice_melting~逆", "water_evaporation", "water_evaporation~逆"}) {
            const reaction::RxRule& rule = table.rules[RuleId(table, name)];
            EXPECT(rule.balanceKelvin != 0 && rule.firstOrderMask == 0);
        }

        for (const std::string& warning : table.warnings) {
            std::printf("  警告: %s\n", warning.c_str());
            EXPECT(warning.find("ice_melting") == std::string::npos);
            EXPECT(warning.find("water_evaporation") == std::string::npos);
        }

        constexpr uint32_t SETTLE_TICKS = 3600;  // 60 秒(近づく速さは 1 /s。距離の端数まで)
        constexpr int32_t MELTING_POINT = 273150;
        const uint32_t water = table.SpeciesId("water");
        const uint32_t ice = table.SpeciesId("ice");
        const uint32_t vapor = table.SpeciesId("water_vapor");
        const uint32_t nitrogen = table.SpeciesId("nitrogen");
        const uint32_t stone = table.SpeciesId("stone");

        // ① 融点がちょうど: 275 K の氷だけ → 熱の分だけ溶けて 273.150 K で止まる
        const reaction::RxLoneCell melted = AdvanceCell(
            table, MakeReactionCell(table, std::vector<SpeciesAmount>{{ice, 100000000}}, 275000), SETTLE_TICKS);
        std::printf("  氷 100 mol(275 K)→ %.3f K・氷 %.3f mol・水 %.3f mol\n",
                    CellTemperature(table, melted.cell) * 1e-3, CellAmount(table, melted.cell, "ice") * 1e-6,
                    CellAmount(table, melted.cell, "water") * 1e-6);
        EXPECT(std::abs(CellTemperature(table, melted.cell) - MELTING_POINT) <= 1);
        EXPECT(CellAmount(table, melted.cell, "ice") > 0 && CellAmount(table, melted.cell, "water") > 0);

        // ② 釣り合えば眠る
        EXPECT(Sleeps(table, melted, 1 + SETTLE_TICKS));

        // ④ 種が無くても凍り始める: 270 K の水だけ → 凍って 273.150 K で止まる
        const reaction::RxLoneCell frozen = AdvanceCell(
            table, MakeReactionCell(table, std::vector<SpeciesAmount>{{water, 100000000}}, 270000), SETTLE_TICKS);
        std::printf("  水 100 mol(270 K)→ %.3f K・氷 %.3f mol\n", CellTemperature(table, frozen.cell) * 1e-3,
                    CellAmount(table, frozen.cell, "ice") * 1e-6);
        EXPECT(std::abs(CellTemperature(table, frozen.cell) - MELTING_POINT) <= 1);
        EXPECT(CellAmount(table, frozen.cell, "ice") > 0);
        EXPECT(Sleeps(table, frozen, 1 + SETTLE_TICKS));

        // ③ 霧: 水の無い 300 K の空気(石で熱を受ける)に飽和を超える湯気 → 水ができ、湯気は釣り合いの濃さへ
        std::vector<SpeciesAmount> fogAmounts = {{nitrogen, 4000000}, {vapor, 1000000}, {stone, STONE}};
        std::ranges::sort(fogAmounts, {}, &SpeciesAmount::species);
        const reaction::RxLoneCell fog = AdvanceCell(table, MakeReactionCell(table, fogAmounts, AIR_TEMPERATURE),
                                                     SETTLE_TICKS);
        const double fogVapor = CellAmount(table, fog.cell, "water_vapor") * 1e-6;
        const double fogExpected = BalanceVapor(CellTemperature(table, fog.cell));
        std::printf("  湯気 1 mol の空気(300 K)→ 水 %.4f mol・湯気 %.4f mol(釣り合い %.4f)・%.3f K\n",
                    CellAmount(table, fog.cell, "water") * 1e-6, fogVapor, fogExpected,
                    CellTemperature(table, fog.cell) * 1e-3);
        EXPECT(CellAmount(table, fog.cell, "water") > 0);
        EXPECT(std::abs(fogVapor / fogExpected - 1.0) < 0.01);
        EXPECT(Sleeps(table, fog, 1 + SETTLE_TICKS));

        // 沸点のまわり: 373.15 K の水 → 湯気が釣り合いの濃さ(1 気圧 ≒ 4.086 mol / セル)へ。蒸発で冷えるので少し下
        std::vector<SpeciesAmount> boilAmounts = {{nitrogen, 1000000}, {water, 100000000}, {stone, STONE}};
        std::ranges::sort(boilAmounts, {}, &SpeciesAmount::species);
        const reaction::RxLoneCell boiled = AdvanceCell(table, MakeReactionCell(table, boilAmounts, 373150),
                                                        SETTLE_TICKS);
        const double boilVapor = CellAmount(table, boiled.cell, "water_vapor") * 1e-6;
        const double boilExpected = BalanceVapor(CellTemperature(table, boiled.cell));
        std::printf("  水 100 mol(373.15 K)→ 湯気 %.4f mol(釣り合い %.4f)・%.3f K\n", boilVapor, boilExpected,
                    CellTemperature(table, boiled.cell) * 1e-3);
        EXPECT(std::abs(boilVapor / boilExpected - 1.0) < 0.01);
        EXPECT(Sleeps(table, boiled, 1 + SETTLE_TICKS));

        // 潜熱(定圧。mJ/mol)= h0 の差 × 1000 + 定圧の比熱の差 × 温度。セルは体積一定なので、閉じたセルで湯気になる時に
        // 吸う熱は内部エネルギーの差 = 潜熱 − RT(298.15 K で 41.5 kJ/mol。気体の比熱が定積。T-0597)
        auto latent = [&](std::string_view from, std::string_view to, int64_t milliKelvin) {
            const uint32_t a = table.SpeciesId(from);
            const uint32_t b = table.SpeciesId(to);
            const auto capacityA = (int64_t)SpeciesPressureHeatCapacity(table, a);
            const auto capacityB = (int64_t)SpeciesPressureHeatCapacity(table, b);

            return ((table.species[b].h0 - table.species[a].h0) * 1000) +
                   ((capacityB - capacityA) * milliKelvin / 1000);
        };
        const int64_t fusion = latent("ice", "water", 273150);
        const int64_t vaporization = latent("water", "water_vapor", 298150);
        std::printf("  潜熱: 融解 %lld mJ/mol(273.15 K)・蒸発 %lld(298.15 K)\n", (long long)fusion,
                    (long long)vaporization);
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

    // セルの外に持つエネルギー: 気体の運動エネルギーのぶん − 気体の帳簿 + 液体の位置と運動のエネルギー − 液体の帳簿
    int64_t FieldEnergy(const ProbeReference& reference) {
        const ProbeGasField* gas = reference.GasField();
        int64_t energy = gas->KineticEnergySum() - gas->Ledger().energy;
        if (const ProbeLiquidField* liquid = reference.LiquidField())
            energy += liquid->PotentialEnergySum() + liquid->KineticEnergySum() - liquid->Ledger().energy;

        return energy;
    }

    Totals Sum(const BakedReactionTable& table, const ProbeReference& reference, uint64_t tick) {
        Totals totals;
        totals.elements.assign(table.elementNames.size(), 0);
        for (const reaction::RxCell& cell : reference.State(tick)) {
            totals.energy += cell.energy;
            if (cell.speciesCount == 0)
                continue;

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

        totals.energy += FieldEnergy(reference);

        return totals;
    }

    // 毎刻みの確かめ(元素を数えない軽い方): エネルギーの合計と、物質の無いのにエネルギーを持つセルの数(T-0646・ADR-0286)
    struct EnergyCheck {
        int64_t energy = 0;
        uint32_t emptyWithEnergy = 0;
    };

    EnergyCheck SumEnergy(const ProbeReference& reference, uint64_t tick) {
        EnergyCheck check;
        for (const reaction::RxCell& cell : reference.State(tick)) {
            check.energy += cell.energy;
            if (cell.speciesCount == 0 && cell.energy != 0)
                ++check.emptyWithEnergy;
        }

        check.energy += FieldEnergy(reference);

        return check;
    }

    uint64_t RunHash(const ProbeReference& reference, uint64_t tick) {
        return reference.StateHash(tick) ^ reference.GasField()->Hash() ^ (reference.LiquidField()->Hash() * 3);
    }

    // 保存を確かめながら進める: エネルギーと物質の無いセルは毎刻み、元素は 1 秒ごと。perSecond(tick) は 1 秒ごとの表示。
    // 崩れたら最初の刻みを表示する(どの刻みで崩れたかを見るため。T-0646)
    template <typename PerSecond>
    void Run(const BakedReactionTable& table, ProbeReference& reference, uint64_t ticks, PerSecond perSecond) {
        const Totals before = Sum(table, reference, 0);
        bool energyConserved = true;
        bool elementsConserved = true;
        uint32_t emptyWithEnergyMax = 0;
        for (uint64_t tick = 0; tick < ticks; ++tick) {
            reference.Advance(tick, {});

            const EnergyCheck check = SumEnergy(reference, tick + 1);
            if (energyConserved && check.energy != before.energy)
                std::printf("  エネルギーの保存が崩れた(%llu 刻み): %+lld mJ・気体の帳簿 %lld・運動 %lld\n",
                            (unsigned long long)(tick + 1), (long long)(check.energy - before.energy),
                            (long long)reference.GasField()->Ledger().energy,
                            (long long)reference.GasField()->KineticEnergySum());

            if (emptyWithEnergyMax == 0 && check.emptyWithEnergy != 0)
                std::printf("  物質の無いのにエネルギーを持つセル(%llu 刻み): %u\n", (unsigned long long)(tick + 1),
                            check.emptyWithEnergy);

            energyConserved = energyConserved && check.energy == before.energy;
            emptyWithEnergyMax = std::max(emptyWithEnergyMax, check.emptyWithEnergy);
            if ((tick + 1) % TICKS_PER_SECOND != 0)
                continue;

            const Totals now = Sum(table, reference, tick + 1);
            if (elementsConserved && now.elements != before.elements) {
                std::printf("  元素の保存が崩れた(%llu 刻み)", (unsigned long long)(tick + 1));
                for (size_t e = 0; e < now.elements.size(); ++e)
                    std::printf("・%s %+lld", table.elementNames[e].c_str(),
                                (long long)(now.elements[e] - before.elements[e]));
                std::printf("・気体の報告 溢れ %d\n", reference.LastGasReport().skippedCellOverflow ? 1 : 0);
            }

            elementsConserved = elementsConserved && now.elements == before.elements;
            perSecond(tick + 1);
        }

        EXPECT(elementsConserved);
        EXPECT(energyConserved);
        EXPECT(emptyWithEnergyMax == 0);
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

    // liquid: 液体の区画も重ねる(水たまりと池は石の堤で囲むので流れなくてよいが、気体と液体の両方の区画で保存を確かめる)
    void TestSteam(const BakedReactionTable& table, bool liquid) {
        std::printf("湯気(熱い石の上の水たまり → 冷たい池。液体の区画 %s)\n", liquid ? "あり" : "なし");
        const Region& region = STEAM_REGION;
        ProbeReference reference = MakeReference(table, region, SteamCell, liquid);

        const uint64_t puddleBefore = AmountIn(table, reference, 0, region, "water", {0, 4}, {1, 2});
        const uint64_t pondBefore = AmountIn(table, reference, 0, region, "water", {8, 12}, {0, 8});
        uint64_t vaporHighMax = 0;
        int64_t centroidMax = -1;
        Run(table, reference, STEAM_TICKS, [&](uint64_t tick) {
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
        });

        const uint64_t puddleAfter = AmountIn(table, reference, STEAM_TICKS, region, "water", {0, 4}, {0, 8});
        const uint64_t pondAfter = AmountIn(table, reference, STEAM_TICKS, region, "water", {8, 12}, {0, 8});
        // 水たまりの 3 割より多くが湯気になった(T-0593 のマージで石が気体を塞ぎ、熱い石の中を通る空気が水たまりを温めなくなった。
        // 20 秒で 3600 → 2428 mol。前は 1865 mol)
        EXPECT_OUTCOME(puddleAfter < puddleBefore / 10 * 7);
        EXPECT_OUTCOME(vaporHighMax > 0);        // 湯気が y 4(2 m)より上へ
        EXPECT_OUTCOME(centroidMax > 2500);      // 湯気の重心が水たまり(y 1)より 1.5 セル以上高い
        EXPECT_OUTCOME(pondAfter > pondBefore);  // 池の水が増えた(湯気が冷たい池で凝縮した)
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
        EXPECT_OUTCOME(iceAfter < iceBefore / 10);   // 9 割より多く溶けた
        EXPECT_OUTCOME(floorAfter > iceBefore / 2);  // 溶けた水の半分より多くが台から流れ落ちた
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

int main(int argc, char** argv) {
    const std::span<char*> args(argv, static_cast<size_t>(argc));
    const auto hasArgument = [&](std::string_view name) {
        return std::ranges::any_of(args.subspan(1), [&](const char* arg) { return std::string_view(arg) == name; });
    };
    const bool stoneWithoutDensity = hasArgument("--stone-without-density");
    const BakedReactionTable table = MakeTable(hasArgument("--written-heat-capacity"), stoneWithoutDensity);
    if (stoneWithoutDensity) {
        // --- 6: 場所を取らない石へ水が染み込む場面の保存だけ ---
        outcomeChecks = false;
        TestSteam(table, true);
        TestIce(table);
        if (failureCount != 0) {
            std::printf("%d 件失敗\n", failureCount);
            return 1;
        }

        std::printf("すべて通った\n");
        return 0;
    }

    TestTable(table);
    TestSteam(table, false);
    TestSteam(table, true);
    TestIce(table);
    TestDeterminism(table);
    ReportQuietPond(table);

    if (failureCount != 0) {
        std::printf("%d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("すべて通った\n");
    return 0;
}
