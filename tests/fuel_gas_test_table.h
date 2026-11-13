// fuel_gas_test_table.h — 試験の表に「熱分解で燃える気体を出す」行を足す(D-660。値と出典は T-1142 の作業ログ)。
// 標準の real_reactions の cellulose_pyrolysis は「炭 + 水蒸気」だけ。標準へ入れるかは T-1142「標準の物質へ入れるか」(仮で入れていない)。
// 試験の中だけの値で、ゲームの中身ではない(D-006)。使い手: object_shell_test(T-1142・T-1143)・gpu_probe_fire_test(T-1366)。
#pragma once

#include <algorithm>

#include "sim/reaction_table.h"

namespace bicameral::test {

    // --- 表: 熱分解で燃える気体を出す(D-660)。値と出典は T-1142 の作業ログ ---
    // セルロースの熱分解は Bradbury–Sakai–Shafizadeh(1979)の 2 本の枝を、活性セルロースを通さず直接に(600〜900 K では開始の段が 2 本の枝より速い)。
    //   揮発分(levoglucosan): A = 1.9e16 /min = 3.17e14 /s・E = 197.9 kJ/mol / 炭 + 気体: A = 7.9e11 /min = 1.32e10 /s・E = 153.1 kJ/mol
    // levoglucosan(気体): 燃焼熱(固体)−2832 kJ/mol と昇華熱 125.1 kJ/mol から生成エンタルピー −833 kJ/mol(NIST)。比熱は 300 K の推算 171 J/(mol·K)。
    // 混ざる速さ 7 mm²/s(Fuller の式の推算)・伝導率 15 mW/(m·K)(未確認)。気体の燃焼の速さ A = 1.4e9 m³/(mol·s)・E = 125 kJ/mol は**未確認**(試験の中だけ)
    inline void AddFuelGas(sim::ReactionTableDefinition& definition) {
        for (sim::RuleDefinition& rule : definition.rules) {
            if (rule.name == "cellulose_pyrolysis")
                rule.rate = {.preExponentialMantissa = 132, .preExponentialExponent10 = 8, .activationEnergy = 153100};
        }

        sim::SpeciesDefinition levoglucosan{
            .name = "levoglucosan",
            .composition = {{.element = "C", .count = 6}, {.element = "H", .count = 10}, {.element = "O", .count = 5}},
            .formationEnthalpy = -833000,
            .heatCapacity = 171000,
            .thermalConductivity = 15,
            .phase = sim::SpeciesPhase::Gas};
        levoglucosan.transport.diffusivityMm2PerS = 7;
        definition.species.push_back(levoglucosan);
        definition.rules.push_back(
            {.name = "cellulose_volatilization",
             .reactants = {{.species = "cellulose", .coefficient = 1, .firstOrder = true}},
             .products = {{.species = "levoglucosan", .coefficient = 1}},
             .rate = {.preExponentialMantissa = 317, .preExponentialExponent10 = 12, .activationEnergy = 197900}});
        definition.rules.push_back(
            {.name = "levoglucosan_combustion",
             .reactants = {{.species = "levoglucosan", .coefficient = 1, .firstOrder = true},
                           {.species = "oxygen", .coefficient = 6, .firstOrder = true}},
             .products = {{.species = "carbon_dioxide", .coefficient = 6},
                          {.species = "water_vapor", .coefficient = 5}},
             .rate = {.preExponentialMantissa = 14, .preExponentialExponent10 = 8, .activationEnergy = 125000}});
    }

    // CO を作らない表(記録だけ。T-1143): 骨格の中も流れる比べでは、流れが紙の冷たいボクセルへ全部の気体を少しずつ運び、紙のボクセルの成分が
    // cellulose・炭・levoglucosan・O2・N2・CO2・水・湯気・CO の 9 になってセルの成分の枠 8(RX_MAX_CELL_SPECIES。GPU の溢れ T-0176)を超える。
    // 枠を広げるのは土台なので広げず、その比べだけ CO を作る規則(炭の不完全燃焼・Boudouard ほか)を外した試験の表で回す
    // (骨格を壁にする既定の形では CO ありの表で枠に収まる)
    inline void AddFuelGasWithoutMonoxide(sim::ReactionTableDefinition& definition) {
        AddFuelGas(definition);
        std::erase_if(definition.rules, [](const sim::RuleDefinition& rule) {
            return std::ranges::any_of(rule.products,
                                       [](const sim::RuleTerm& term) { return term.species == "carbon_monoxide"; });
        });
    }

}  // namespace bicameral::test
