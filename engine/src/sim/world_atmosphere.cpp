// world_atmosphere.cpp — 基準の大気のベイク・高さの点の間の直線・高さの静水圧(world_atmosphere.h。T-0373・T-0499・ADR-0182)。
// ベイクは物質の名前を ID に直し、点ごとの割合と値・高さの順を確かめる。直線と静水圧は整数だけ(04 §4)。
#include "sim/world_atmosphere.h"

#include <algorithm>
#include <format>

#include "common/fixed.hlsli"
#include "common/units.hlsli"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    namespace {

        // 誤りの前置き: 一様な書き方(点 1 つ・label 空)は「基準の大気」、点の列は「基準の大気 environment.atmosphere_profile[i]」
        std::string Where(const AtmospherePointDefinition& point) {
            return point.label.empty() ? std::string("基準の大気") : std::format("基準の大気 {}", point.label);
        }

        // 点があるのに地面の圧力が無い / 圧力だけ書いた
        std::expected<void, std::string> CheckPressurePresent(const WorldAtmosphereDefinition& definition) {
            const bool hasPoints = !definition.points.empty();
            const bool hasPressure = definition.surfacePressurePascals != 0;
            if (!hasPoints && hasPressure)
                return std::unexpected(
                    "基準の大気: 地面の圧力があるのに成分(分類 atmosphere か environment.atmosphere_profile)が無い");

            if (hasPoints && !hasPressure)
                return std::unexpected("基準の大気: 成分があるのに environment.atmosphere_surface_pressure_pa が無い");

            return {};
        }

        // 1 つの点: 温度があること・名前 → ID(知らない物質・重なり・割合 0 は誤り)・足して 1
        std::expected<AtmospherePoint, std::string> BakePoint(const AtmospherePointDefinition& definition,
                                                              const BakedReactionTable& table) {
            const std::string where = Where(definition);
            if (definition.fractions.empty())
                return std::unexpected(
                    std::format("{}: 成分(分類 atmosphere か atmosphere_profile の composition)が無い", where));

            if (definition.temperatureMilliKelvin == 0)
                return std::unexpected(std::format(
                    "{}: 成分があるのに温度(environment.atmosphere_temperature_mk か temperature_mk)が無い", where));

            AtmospherePoint point{.heightMillimeters = definition.heightMillimeters,
                                  .temperatureMilliKelvin = definition.temperatureMilliKelvin};
            uint64_t total = 0;
            for (const AtmosphereFractionDefinition& entry : definition.fractions) {
                const uint32_t species = table.SpeciesId(entry.species);
                if (species == 0)
                    return std::unexpected(std::format("{}: 知らない物質 {}", where, entry.species));

                if (std::ranges::contains(point.fractions, species, &AtmosphereFraction::species))
                    return std::unexpected(std::format("{}: 物質 {} が 2 回ある", where, entry.species));

                if (entry.partsPerMillion == 0)
                    return std::unexpected(
                        std::format("{}.{}: 割合が 0(書かないなら鍵ごと消す)", where, entry.species));

                point.fractions.push_back({.species = species, .partsPerMillion = entry.partsPerMillion});
                total += entry.partsPerMillion;
            }

            if (total != ATMOSPHERE_PARTS_PER_WHOLE)
                return std::unexpected(std::format("{}: 割合を足すと {} ppm({} ppm になるように書く)", where, total,
                                                   ATMOSPHERE_PARTS_PER_WHOLE));

            std::ranges::sort(point.fractions, {}, &AtmosphereFraction::species);

            return point;
        }

        // 高さの範囲と順(厳密に増える)
        std::expected<void, std::string> CheckHeights(const WorldAtmosphereDefinition& definition) {
            for (size_t i = 0; i < definition.points.size(); ++i) {
                const AtmospherePointDefinition& point = definition.points[i];
                if (point.heightMillimeters < -ATMOSPHERE_HEIGHT_LIMIT_MILLIMETERS ||
                    point.heightMillimeters > ATMOSPHERE_HEIGHT_LIMIT_MILLIMETERS)
                    return std::unexpected(std::format("{}: 高さ {} mm が範囲(±{} mm)の外", Where(point),
                                                       point.heightMillimeters, ATMOSPHERE_HEIGHT_LIMIT_MILLIMETERS));

                if (i > 0 && point.heightMillimeters <= definition.points[i - 1].heightMillimeters)
                    return std::unexpected(std::format("{}: 高さ {} mm が前の点の高さ {} mm より高くない(低い順に書く)",
                                                       Where(point), point.heightMillimeters,
                                                       definition.points[i - 1].heightMillimeters));
            }

            return {};
        }

        // |difference| × numerator ÷ denominator(切り捨て)に difference の向きを付ける。numerator < denominator
        int64_t ScaleDifference(int64_t difference, uint64_t numerator, uint64_t denominator) {
            const uint64_t magnitude = difference < 0 ? (uint64_t)(-difference) : (uint64_t)difference;
            const auto scaled = (int64_t)fx::FxDivU128By64(fx::FxMulU64Full(magnitude, numerator), denominator)
                                    .quotient;

            return difference < 0 ? -scaled : scaled;
        }

        // 2 つの点の割合を重み (span − offset)・offset で混ぜる(切り捨て → 端数を余りの大きい順に配って足して 1)
        std::vector<AtmosphereFraction> BlendFractions(const AtmospherePoint& lower, const AtmospherePoint& upper,
                                                       uint64_t offset, uint64_t span) {
            struct Share {
                uint32_t species = 0;
                uint64_t parts = 0;
                uint64_t remainder = 0;
            };

            // --- 両方の点の物質を合わせる(無い側は 0 ppm)---
            std::vector<Share> shares;
            const auto add = [&](const AtmospherePoint& point, uint64_t weight) {
                for (const AtmosphereFraction& fraction : point.fractions) {
                    auto found = std::ranges::find(shares, fraction.species, &Share::species);
                    if (found == shares.end()) {
                        shares.push_back({.species = fraction.species});
                        found = shares.end() - 1;
                    }

                    found->parts += weight * fraction.partsPerMillion;  // ≤ 2^41 × 2^20(64bit に収まる)
                }
            };
            add(lower, span - offset);
            add(upper, offset);

            // --- 切り捨てと端数(余りの大きい順・同じなら物質 ID の小さい順に 1 ppm ずつ)---
            uint64_t total = 0;
            for (Share& share : shares) {
                share.remainder = share.parts % span;
                share.parts /= span;
                total += share.parts;
            }

            std::ranges::sort(shares, [](const Share& left, const Share& right) {
                return left.remainder != right.remainder ? left.remainder > right.remainder
                                                         : left.species < right.species;
            });
            for (size_t i = 0; total < ATMOSPHERE_PARTS_PER_WHOLE; ++i, ++total)
                ++shares[i].parts;

            std::vector<AtmosphereFraction> fractions;
            for (const Share& share : shares) {
                if (share.parts != 0)
                    fractions.push_back({.species = share.species, .partsPerMillion = (uint32_t)share.parts});
            }

            std::ranges::sort(fractions, {}, &AtmosphereFraction::species);

            return fractions;
        }

        // 1 段の圧力の比 Δp ÷ p = M̄ g Δz ÷ (R T) の Q32(M̄ = Σ ppm × M ÷ 10^6)。
        // 単位: Σ ppm × M(mg/mol)・g(µm/s²)・Δz(mm)・R(nJ/(mol·K))・T(mK)→ 比 = 和 × g × Δz ÷ (R × T × 10^9)
        uint64_t PressureRatioQ32(const AtmosphereSample& sample, const BakedReactionTable& table, uint64_t gravity,
                                  uint64_t step) {
            uint64_t massSum = 0;  // Σ ppm × M(≤ 10^6 × 2^32)
            for (const AtmosphereFraction& fraction : sample.fractions)
                massSum += (uint64_t)fraction.partsPerMillion * table.molarMasses[fraction.species];

            const uint64_t scaled = fx::FxDivU128By64(fx::FxMulU64Full(massSum, gravity * step),
                                                      fx::GAS_CONSTANT_NANOJOULES_PER_MOLE_KELVIN)
                                        .quotient;

            return fx::FxDivU128By64(fx::FxMulU64Full(scaled, (uint64_t)1 << 32),
                                     (uint64_t)sample.temperatureMilliKelvin * 1000000000)
                .quotient;
        }

        // p × 比(Q32)。比が 1 以上なら p
        uint64_t ScalePressure(uint64_t pressure, uint64_t ratioQ32) {
            if (ratioQ32 >= ((uint64_t)1 << 32))
                return pressure;

            return fx::FxShiftRightU128(fx::FxMulU64Full(pressure, ratioQ32), 32);
        }

    }  // namespace

    std::expected<WorldAtmosphere, std::string> BakeWorldAtmosphere(const WorldAtmosphereDefinition& definition,
                                                                    const BakedReactionTable& table) {
        if (auto present = CheckPressurePresent(definition); !present)
            return std::unexpected(present.error());

        if (auto heights = CheckHeights(definition); !heights)
            return std::unexpected(heights.error());

        WorldAtmosphere atmosphere{.surfacePressurePascals = definition.surfacePressurePascals};
        for (const AtmospherePointDefinition& entry : definition.points) {
            auto point = BakePoint(entry, table);
            if (!point)
                return std::unexpected(point.error());

            atmosphere.points.push_back(std::move(*point));
        }

        return atmosphere;  // 点が無ければブランク(基準の大気が無い)
    }

    AtmosphereSample SampleAtmosphere(const WorldAtmosphere& atmosphere, int64_t heightMillimeters) {
        const std::vector<AtmospherePoint>& points = atmosphere.points;

        // --- 端の点の外は端の値 ---
        if (heightMillimeters <= points.front().heightMillimeters)
            return {.temperatureMilliKelvin = points.front().temperatureMilliKelvin,
                    .fractions = points.front().fractions};

        if (heightMillimeters >= points.back().heightMillimeters)
            return {.temperatureMilliKelvin = points.back().temperatureMilliKelvin,
                    .fractions = points.back().fractions};

        // --- 挟む 2 つの点の間を直線で ---
        const auto upper = std::ranges::upper_bound(points, heightMillimeters, {}, &AtmospherePoint::heightMillimeters);
        const AtmospherePoint& high = *upper;
        const AtmospherePoint& low = *(upper - 1);
        const auto span = (uint64_t)(high.heightMillimeters - low.heightMillimeters);
        const auto offset = (uint64_t)(heightMillimeters - low.heightMillimeters);

        const int64_t difference = (int64_t)high.temperatureMilliKelvin - (int64_t)low.temperatureMilliKelvin;
        const int64_t temperature = (int64_t)low.temperatureMilliKelvin + ScaleDifference(difference, offset, span);

        return {.temperatureMilliKelvin = (uint32_t)temperature, .fractions = BlendFractions(low, high, offset, span)};
    }

    uint64_t AtmospherePressureMicroPascal(const WorldAtmosphere& atmosphere, const BakedReactionTable& table,
                                           uint32_t gravityMicrometersPerSecond2, int64_t heightMillimeters) {
        const int64_t base = atmosphere.points.front().heightMillimeters;
        uint64_t pressure = (uint64_t)atmosphere.surfacePressurePascals * 1000000;
        if (gravityMicrometersPerSecond2 == 0)
            return pressure;

        // --- 上へ: 段の下の端の温度と成分で、その段の重さの分だけ下げる(気体の層の基準と同じ形)---
        for (int64_t z = base; z < heightMillimeters && pressure != 0;) {
            const int64_t step = std::min(ATMOSPHERE_PRESSURE_STEP_MILLIMETERS, heightMillimeters - z);
            const uint64_t ratio = PressureRatioQ32(SampleAtmosphere(atmosphere, z), table,
                                                    gravityMicrometersPerSecond2, (uint64_t)step);
            pressure -= ScalePressure(pressure, ratio);
            z += step;
        }

        // --- 下へ: 段の下の端の温度と成分で、その段の重さの分だけ上げる(上限で止める)---
        for (int64_t z = base; z > heightMillimeters && pressure < ATMOSPHERE_PRESSURE_LIMIT_MICROPASCAL;) {
            const int64_t step = std::min(ATMOSPHERE_PRESSURE_STEP_MILLIMETERS, z - heightMillimeters);
            z -= step;
            const uint64_t ratio = PressureRatioQ32(SampleAtmosphere(atmosphere, z), table,
                                                    gravityMicrometersPerSecond2, (uint64_t)step);
            pressure = std::min(pressure + ScalePressure(pressure, ratio), ATMOSPHERE_PRESSURE_LIMIT_MICROPASCAL);
        }

        return pressure;
    }

}  // namespace bicameral::sim
