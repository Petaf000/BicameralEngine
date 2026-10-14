// radiation_reference.cpp — 放射の熱の交換の CPU リファレンス(T-0583・ADR-0252)。説明は radiation_reference.h
#include "sim/radiation_reference.h"

#include "sim/reaction_table.h"

namespace bicameral::sim {

    namespace rx = ::bicameral::reaction;

    uint32_t CellAbsorptanceQ16(const BakedReactionTable& baked, const rx::RxCell& cell) {
        fx::FxU128 sum{};
        for (uint32_t i = 0; i < cell.speciesCount; ++i) {
            const uint32_t species = cell.species[i];
            if (species >= baked.speciesRadiationAbsorption.size())
                continue;

            sum = rx::RdAddAbsorption(sum, baked.speciesRadiationAbsorption[species], cell.amounts[i]);
        }

        return rx::RdAbsorptanceQ16(rx::RdOpticalDepthQ32(sum));
    }

    int64_t RadiationPairFlow(const RadiationSurface& first, const RadiationSurface& second,
                              const RadiationPair& pair) {
        // --- 係数: σAΔt·α·α·F·(T₁ + T₂)(T₁² + T₂²)と、行き過ぎの上限の小さい方(どちらも 2 つについて対称)---
        const uint64_t weight = rx::RdPairWeightQ32(first.absorptanceQ16, second.absorptanceQ16, pair.viewQ16);
        const uint64_t coefficient = rx::RdPairCoefficient(first.temperatureMk, second.temperatureMk, weight);
        const uint64_t limit = rx::RdPairLimit(first.heatCapacityNanojoulesPerKelvin,
                                               second.heatCapacityNanojoulesPerKelvin, pair.shareQ16);

        return rx::RdPairFlow(first.temperatureMk, second.temperatureMk, coefficient < limit ? coefficient : limit);
    }

    std::vector<int64_t> ExchangeRadiation(std::span<const RadiationSurface> surfaces,
                                           std::span<const RadiationPair> pairs) {
        std::vector<int64_t> changes(surfaces.size(), 0);
        for (const RadiationPair& pair : pairs) {
            if (pair.first >= surfaces.size() || pair.second >= surfaces.size() || pair.first == pair.second)
                continue;

            // 1 つの整数を片方から引き、もう片方に足す(保存はここで構造的に決まる)
            const int64_t flow = RadiationPairFlow(surfaces[pair.first], surfaces[pair.second], pair);
            changes[pair.first] -= flow;
            changes[pair.second] += flow;
        }

        return changes;
    }

}  // namespace bicameral::sim
