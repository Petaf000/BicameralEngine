// reaction_limits_table.h — 上限に当たる場面の試験の反応表とセル(T-0022 の最初の段)。公開側に置く試験データで、ゲームの中身ではない(D-006)。
// reaction_test(CPU)と gpu_reaction_test --limits(GPU)が使う。
//   元素 X 1 つ。物質は X 1 個の異性体だけ(どの規則も元素の釣り合いが自明に取れる)。生成エンタルピーを少しずつ変え、発熱と吸熱の両方を含める。
//   - 込み合う場面(p01〜p08): p_i → p_j(i ≠ j)の 56 本。8 種が揃ったセルでは毎刻み 56 本が進もうとし、RX_MAX_CANDIDATES(16)を超える
//   - 入りきらない場面(q01〜q09): q01 → q09(9 種目を作る)と、q08 → q07(速い。q08 を使い切ると 1 枠空く)。
//     q01〜q08 が揃ったセルでは、q08 が無くなるまで q01 → q09 を待たせる(RX_LIMIT_PRODUCTS)
#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "reaction_test_cells.h"
#include "sim/reaction_table.h"

namespace bicameral::test {

    constexpr uint32_t LIMITS_CROWD_SPECIES = 8;
    constexpr uint32_t LIMITS_HELD_SPECIES = 9;
    constexpr int32_t LIMITS_TEMPERATURE_MILLIKELVIN = 300000;

    inline std::string LimitsCrowdName(uint32_t i) {
        return std::format("p{:02}", i);
    }

    inline std::string LimitsHeldName(uint32_t i) {
        return std::format("q{:02}", i);
    }

    inline sim::SpeciesDefinition LimitsSpecies(std::string name, int64_t formationEnthalpy) {
        return {.name = std::move(name),
                .composition = {{.element = "X", .count = 1}},
                .formationEnthalpy = formationEnthalpy,
                .heatCapacity = 30000,
                .thermalConductivity = 1000};
    }

    // from → to の一次反応(k = mantissa × 10^exponent10 /s、活性化エネルギー 0)
    inline sim::RuleDefinition LimitsRule(std::string name, std::string from, std::string to, uint64_t mantissa,
                                          int32_t exponent10) {
        return {
            .name = std::move(name),
            .reactants = {{.species = std::move(from), .coefficient = 1, .firstOrder = true}},
            .products = {{.species = std::move(to), .coefficient = 1, .firstOrder = false}},
            .rate = {.preExponentialMantissa = mantissa, .preExponentialExponent10 = exponent10, .activationEnergy = 0},
            .declaredReactionEnthalpy = std::nullopt};
    }

    inline sim::ReactionTableDefinition MakeLimitsTestTable() {
        sim::ReactionTableDefinition definition;
        definition.elements = {{.name = "X", .atomicMass = 10000}};
        for (uint32_t i = 1; i <= LIMITS_CROWD_SPECIES; ++i)
            definition.species.push_back(LimitsSpecies(LimitsCrowdName(i), -1000 * static_cast<int64_t>(i)));

        for (uint32_t i = 1; i <= LIMITS_HELD_SPECIES; ++i)
            definition.species.push_back(LimitsSpecies(LimitsHeldName(i), -500 * static_cast<int64_t>(i)));

        // --- 込み合う: 56 本(速さは 0.1〜0.5 /s。1 刻みに数百〜数千 µmol)---
        for (uint32_t i = 1; i <= LIMITS_CROWD_SPECIES; ++i) {
            for (uint32_t j = 1; j <= LIMITS_CROWD_SPECIES; ++j) {
                if (i == j)
                    continue;

                definition.rules.push_back(LimitsRule(std::format("crowd_{}_{}", i, j), LimitsCrowdName(i),
                                                      LimitsCrowdName(j), 1 + ((i * 8 + j) % 5), -1));
            }
        }

        // --- 入りきらない: 9 種目を作る規則(1 /s)と、枠を空ける規則(10 /s)---
        definition.rules.push_back(LimitsRule("held_make_ninth", LimitsHeldName(1), LimitsHeldName(9), 1, 0));
        definition.rules.push_back(LimitsRule("held_free_slot", LimitsHeldName(8), LimitsHeldName(7), 1, 1));

        return definition;
    }

    // 試験のセル: 偶数は込み合う(p01〜p08)、奇数は入りきらない(q01〜q08)。量はセルごとに 1〜2 mol
    inline reaction::RxCell MakeLimitsCell(const sim::BakedReactionTable& table, uint32_t index) {
        std::vector<sim::SpeciesAmount> amounts;
        const bool crowd = index % 2 == 0;
        const uint32_t count = crowd ? LIMITS_CROWD_SPECIES : LIMITS_HELD_SPECIES - 1;
        for (uint32_t i = 1; i <= count; ++i) {
            const std::string name = crowd ? LimitsCrowdName(i) : LimitsHeldName(i);
            const uint64_t amount = 1000000 + fx::FxRandomBelow(fx::FxHash64(REACTION_TEST_SEED, index, i, 3), 1000000);
            amounts.push_back({.species = table.SpeciesId(name), .amount = amount});
        }

        return sim::MakeReactionCell(table, amounts, LIMITS_TEMPERATURE_MILLIKELVIN);
    }

}  // namespace bicameral::test
