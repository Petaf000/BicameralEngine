// reaction_test_cells.h — 反応のテスト(tests/reaction_test.cpp・gpu_reaction_test.cpp)が同じセルを作るための道具(T-0014)。
// セル i の成分と温度は FxHash64 だけで決める(CPU と GPU のテストが同じ列を作れるように)。
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "sim/reaction_table.h"

namespace bicameral::test {

    constexpr uint64_t REACTION_TEST_SEED = 20261001;

    // どの規則にも出ない物質(反応しない。例: 標準パッケージの argon)か
    inline bool IsInertSpecies(const sim::BakedReactionTable& table, uint32_t species) {
        for (const reaction::RxRule& rule : table.rules) {
            for (uint32_t i = 0; i < rule.reactantCount; ++i) {
                if (rule.reactants[i] == species)
                    return false;
            }

            for (uint32_t i = 0; i < rule.productCount; ++i) {
                if (rule.products[i] == species)
                    return false;
            }
        }

        return true;
    }

    // セルに入れてよい物質: 全部。ただし物質がインラインのセルの数(RX_MAX_CELL_SPECIES)より多い表では、反応しない物質を
    // ID の小さい順に外して収める(試験の表は標準パッケージの argon で 9 種になった。T-0498。反応で 9 種目ができないように)
    inline std::vector<uint32_t> VariedCellSpecies(const sim::BakedReactionTable& table) {
        std::vector<uint32_t> candidates;
        for (uint32_t species = 1; species < static_cast<uint32_t>(table.species.size()); ++species)
            candidates.push_back(species);

        for (uint32_t species = 1;
             species < static_cast<uint32_t>(table.species.size()) && candidates.size() > reaction::RX_MAX_CELL_SPECIES;
             ++species) {
            if (IsInertSpecies(table, species))
                std::erase(candidates, species);
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
