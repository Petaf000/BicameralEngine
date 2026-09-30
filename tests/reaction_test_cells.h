// reaction_test_cells.h — 反応のテスト(tests/reaction_test.cpp・gpu_reaction_test.cpp)が同じセルを作るための道具(T-0014)。
// セル i の成分と温度は FxHash64 だけで決める(CPU と GPU のテストが同じ列を作れるように)。
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "sim/reaction_table.h"

namespace bicameral::test {

    constexpr uint64_t REACTION_TEST_SEED = 20261001;

    // いろいろな成分と温度のセル。物質はそれぞれ 6 割の確率で入り、量は 1e3〜1e8 µmol(桁も散らす)。温度は 300〜2800 K
    inline reaction::RxCell MakeVariedReactionCell(const sim::BakedReactionTable& table, uint32_t index) {
        using namespace bicameral::fx;

        std::vector<sim::SpeciesAmount> amounts;
        const auto speciesCount = static_cast<uint32_t>(table.species.size());
        for (uint32_t species = 1; species < speciesCount; ++species) {
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
