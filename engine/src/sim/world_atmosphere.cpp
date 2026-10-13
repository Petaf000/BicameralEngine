// world_atmosphere.cpp — 基準の大気のベイク(world_atmosphere.h。T-0373・ADR-0182)。物質の名前を ID に直し、割合と値を確かめる。
#include "sim/world_atmosphere.h"

#include <algorithm>
#include <format>

#include "sim/reaction_table.h"

namespace bicameral::sim {

    namespace {

        // 成分が無いのに温度か圧力だけ書いた / 成分があるのに温度か圧力が無い
        std::expected<void, std::string> CheckValuesPresent(const WorldAtmosphereDefinition& definition) {
            const bool hasFractions = !definition.fractions.empty();
            const bool hasTemperature = definition.temperatureMilliKelvin != 0;
            const bool hasPressure = definition.surfacePressurePascals != 0;
            if (!hasFractions && (hasTemperature || hasPressure))
                return std::unexpected("基準の大気: 温度か地面の圧力があるのに成分(分類 atmosphere)が無い");

            if (hasFractions && !(hasTemperature && hasPressure))
                return std::unexpected(
                    "基準の大気: 成分があるのに environment.atmosphere_temperature_mk か "
                    "atmosphere_surface_pressure_pa が無い");

            return {};
        }

    }  // namespace

    std::expected<WorldAtmosphere, std::string> BakeWorldAtmosphere(const WorldAtmosphereDefinition& definition,
                                                                    const BakedReactionTable& table) {
        if (auto present = CheckValuesPresent(definition); !present)
            return std::unexpected(present.error());

        WorldAtmosphere atmosphere{.temperatureMilliKelvin = definition.temperatureMilliKelvin,
                                   .surfacePressurePascals = definition.surfacePressurePascals};
        if (definition.fractions.empty())
            return atmosphere;  // ブランク(基準の大気が無い)

        // --- 名前 → ID(知らない物質・重なり・割合 0 は誤り)---
        uint64_t total = 0;
        for (const AtmosphereFractionDefinition& entry : definition.fractions) {
            const uint32_t species = table.SpeciesId(entry.species);
            if (species == 0)
                return std::unexpected(std::format("atmosphere: 知らない物質 {}", entry.species));

            if (std::ranges::contains(atmosphere.fractions, species, &AtmosphereFraction::species))
                return std::unexpected(std::format("atmosphere: 物質 {} が 2 回ある", entry.species));

            if (entry.partsPerMillion == 0)
                return std::unexpected(std::format("atmosphere.{}: 割合が 0(書かないなら鍵ごと消す)", entry.species));

            atmosphere.fractions.push_back({.species = species, .partsPerMillion = entry.partsPerMillion});
            total += entry.partsPerMillion;
        }

        // --- 足して 1 ---
        if (total != ATMOSPHERE_PARTS_PER_WHOLE)
            return std::unexpected(std::format("atmosphere: 割合を足すと {} ppm({} ppm になるように書く)", total,
                                               ATMOSPHERE_PARTS_PER_WHOLE));

        std::ranges::sort(atmosphere.fractions, {}, &AtmosphereFraction::species);

        return atmosphere;
    }

}  // namespace bicameral::sim
