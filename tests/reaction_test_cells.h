// reaction_test_cells.h — 反応のテスト(tests/reaction_test.cpp・gpu_reaction_test.cpp)が同じセルを作るための道具(T-0014)。
// セル i の成分と温度は FxHash64 だけで決める(CPU と GPU のテストが同じ列を作れるように)。
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "sim/reaction_table.h"

namespace bicameral::test {

    constexpr uint64_t REACTION_TEST_SEED = 20261001;

    // 物質が出る規則の数(反応物か生成物として。触媒のように両方に出ても 1 本と数える。どの規則にも出ない物質〔例: argon〕は 0)
    inline uint32_t RuleAppearances(const sim::BakedReactionTable& table, uint32_t species) {
        uint32_t count = 0;
        for (const reaction::RxRule& rule : table.rules) {
            bool appears = false;
            for (uint32_t i = 0; i < rule.reactantCount; ++i)
                appears = appears || rule.reactants[i] == species;

            for (uint32_t i = 0; i < rule.productCount; ++i)
                appears = appears || rule.products[i] == species;

            count += appears ? 1u : 0u;
        }

        return count;
    }

    // 物質の組が規則で閉じているか: 反応物が全部組の中にある規則は、生成物も全部組の中にある
    // (閉じた組から選んだセルは、反応を何刻み進めても組の外の物質を作らない)
    inline bool IsClosedUnderRules(const sim::BakedReactionTable& table, const std::vector<uint32_t>& candidates) {
        const auto inSet = [&](uint32_t species) {
            return std::ranges::find(candidates, species) != candidates.end();
        };
        for (const reaction::RxRule& rule : table.rules) {
            bool runs = true;
            for (uint32_t i = 0; i < rule.reactantCount; ++i)
                runs = runs && inSet(rule.reactants[i]);

            if (!runs)
                continue;

            for (uint32_t i = 0; i < rule.productCount; ++i) {
                if (!inSet(rule.products[i]))
                    return false;
            }
        }

        return true;
    }

    // セルに入れてよい物質: 全部。ただし物質がインラインのセルの数(RX_MAX_CELL_SPECIES)より多い表では、規則で閉じた組のまま
    // 収まるまで 1 つずつ外す(セルが反応で 9 種目を作らないように。T-0498・T-0611)。外すのは、外しても閉じたままの物質のうち
    // 出る規則が一番少ないもの(同じなら ID の小さい方)。反応しない物質(argon など。0 本)が先に外れる。
    // 試験の表(燃焼)は水の 3 相を足して反応する物質が 9 種になり、ice(融解・凝固の 2 本)が外れる
    // (ice を外しても水と湯気は残るので、蒸発・凝縮は試験に入る。water は ice の融解が作るので ice より先には外れない)
    inline std::vector<uint32_t> VariedCellSpecies(const sim::BakedReactionTable& table) {
        std::vector<uint32_t> candidates;
        for (uint32_t species = 1; species < static_cast<uint32_t>(table.species.size()); ++species)
            candidates.push_back(species);

        while (candidates.size() > reaction::RX_MAX_CELL_SPECIES) {
            uint32_t chosen = 0;
            uint32_t chosenAppearances = UINT32_MAX;
            for (const uint32_t species : candidates) {
                std::vector<uint32_t> trial = candidates;
                std::erase(trial, species);
                const uint32_t appearances = RuleAppearances(table, species);
                if (appearances >= chosenAppearances || !IsClosedUnderRules(table, trial))
                    continue;

                chosen = species;
                chosenAppearances = appearances;
            }

            if (chosen == 0)
                break;

            std::erase(candidates, chosen);
        }

        return candidates;
    }

    // いろいろな成分と温度のセル。物質はそれぞれ 6 割の確率で入り、量は 1e3〜1e8 µmol(桁も散らす)。温度は 300〜2800 K
    inline reaction::RxCell MakeVariedReactionCell(const sim::BakedReactionTable& table, uint32_t index) {
        using namespace bicameral::fx;

        std::vector<sim::SpeciesAmount> amounts;
        for (const uint32_t species : VariedCellSpecies(table)) {
            const uint64_t hash = FxHash64(REACTION_TEST_SEED, index, species, 1);
            if (FxRandomBelow(hash, 10) >= 6)
                continue;

            constexpr std::array<uint64_t, 6> SCALES = {1000, 10000, 100000, 1000000, 10000000, 100000000};
            const uint64_t scale = SCALES[FxRandomBelow(FxMix64(hash), static_cast<uint32_t>(SCALES.size()))];
            amounts.push_back({.species = species, .amount = scale + FxRandomBelow(FxMix64(hash + 1), 1000000)});
        }

        const uint32_t kelvin = 300 + FxRandomBelow(FxHash64(REACTION_TEST_SEED, index, 0, 2), 2500);

        return sim::MakeReactionCell(table, amounts, static_cast<int32_t>(kelvin * 1000));
    }

}  // namespace bicameral::test
