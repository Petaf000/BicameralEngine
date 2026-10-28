// phase_layer_scene.h — セルより小さい層の 2 つのエネルギー(shaders/common/phase_layer.hlsli。ADR-0486)の試験が共有する表とセルの作り方
// (tests/phase_layer_test.cpp の 1 列・tests/gpu_phase_layer_test.cpp の GPU とのビット一致。T-1160・T-1211)。
// 表は試験の表(combustion_test。real_matter・real_reactions を写したもの)+ 地球の環境 + 石 2 つ(試験の石・現実の石)
// + 気体だけの規則 1 つ(試験だけの燃える気体)。
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

#include "common/phase_layer.hlsli"
#include "reaction_test_table.h"
#include "sim/reaction_table.h"
#include "standard_world_physics.h"

namespace bicameral::test {

    // µmol(0.125 m³ の岩 ≒ 337 kg を C の 12 g/mol で。密度 2000 でセルを満たす。probe_phase と同じ)
    inline constexpr uint64_t PL_STONE_MICROMOL = 28000000000;
    // 0.125 m³ の空気(300 K・1 気圧で 5.08 mol)の N2・O2
    inline constexpr uint64_t PL_AIR_NITROGEN_MICROMOL = 4013000;
    inline constexpr uint64_t PL_AIR_OXYGEN_MICROMOL = 1067000;

    inline sim::BakedReactionTable MakePhaseLayerTable() {
        sim::ReactionTableDefinition definition = sim::MakeCombustionTestTable();
        definition.gasHeatCapacityAtConstantPressure = true;
        definition.environment = sim::StandardWorldPhysics();
        // 試験の石(伝導率 2000 W/(m·K)・ほぼ一様な温度の台)と現実の石(2.8 W/(m·K)。ρc ≈ 1.9 MJ/(m³·K))
        definition.species.push_back({.name = "stone",
                                      .composition = {{.element = "C", .count = 1}},
                                      .formationEnthalpy = 0,
                                      .heatCapacity = 8500,
                                      .thermalConductivity = 2000000,
                                      .phase = sim::SpeciesPhase::Solid,
                                      .density = 2000});
        definition.species.push_back({.name = "stone_real",
                                      .composition = {{.element = "C", .count = 1}},
                                      .formationEnthalpy = 0,
                                      .heatCapacity = 8500,
                                      .thermalConductivity = 2800,
                                      .phase = sim::SpeciesPhase::Solid,
                                      .density = 2000});
        // 気体だけの規則(T-1211 の 2 回目の反応を通す。ほかの場面に出ないように試験だけの気体): 2 燃える気体 + O2 → 2 CO2。
        // CO と同じ値の別名。Ea 100 kJ/mol・A 1.7 × 10^5 /s(1000 K で約 1 /s・300 K で約 10^-12 /s = 層の温度では進まない)
        definition.species.push_back({.name = "test_fuel_gas",
                                      .composition = {{.element = "C", .count = 1}, {.element = "O", .count = 1}},
                                      .formationEnthalpy = -110530,
                                      .heatCapacity = 29100,
                                      .thermalConductivity = 25});
        definition.rules.push_back(
            {.name = "test_fuel_gas_burn",
             .reactants = {{.species = "test_fuel_gas", .coefficient = 2, .firstOrder = true},
                           {.species = "oxygen", .coefficient = 1}},
             .products = {{.species = "carbon_dioxide", .coefficient = 2}},
             .rate = {.preExponentialMantissa = 17, .preExponentialExponent10 = 4, .activationEnergy = 100000}});
        auto baked = sim::BakeReactionTable(definition);
        if (!baked) {
            std::printf("表を作れない: %s\n", baked.error().c_str());
            std::exit(1);
        }

        return std::move(*baked);
    }

    // --- セルを作る(成分は ID の昇順。エネルギー = Σ 量 × H0 + 熱容量 × 温度)---
    struct PlAmount {
        std::string_view name;
        uint64_t amount;
    };

    inline int64_t PlPartEnergyMicrojoules(const sim::BakedReactionTable& table, const std::vector<PlAmount>& amounts,
                                           uint32_t temperatureMk) {
        int64_t energy = 0;
        for (const PlAmount& entry : amounts) {
            const reaction::RxSpecies species = table.View().Species(table.SpeciesId(entry.name));
            energy += (int64_t)entry.amount * species.h0;
            energy += fx::FxMulDivS64((int64_t)(entry.amount * species.heatCapacity), temperatureMk, 1000000);
        }

        return energy;
    }

    inline reaction::RxCell MakePhaseLayerCell(const sim::BakedReactionTable& table, std::vector<PlAmount> amounts,
                                               int64_t energyMj) {
        std::ranges::sort(amounts, [&](const PlAmount& a, const PlAmount& b) {
            return table.SpeciesId(a.name) < table.SpeciesId(b.name);
        });
        reaction::RxCell cell = reaction::RxMakeEmptyCell(energyMj);
        for (const PlAmount& entry : amounts) {
            cell.species[cell.speciesCount] = table.SpeciesId(entry.name);
            cell.amounts[cell.speciesCount] = entry.amount;
            ++cell.speciesCount;
        }

        return cell;
    }

}  // namespace bicameral::test
