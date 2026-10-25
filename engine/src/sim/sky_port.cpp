// sky_port.cpp — 空との交換の口(sky_port.h。T-1145・ADR-0474)。掃きの本体は太陽の影の柱(sun_column)。
#include "sim/sky_port.h"

#include <format>

#include "common/cascade_light.hlsli"

namespace bicameral::sim {

    namespace mr = ::bicameral::multires;
    namespace rx = ::bicameral::reaction;

    namespace {

        constexpr uint32_t SHARE_ONE_Q16 = 1u << 16;
        constexpr uint32_t LENGTH_SHIFT = mr::SUN_LENGTH_SHIFT;
        constexpr int64_t DIRECTION_LIMIT = mr::SUN_DIRECTION_LIMIT;
        constexpr int64_t IRRADIANCE_LIMIT = 0xFFFFFFFFll;

        int64_t Absolute(int64_t value) {
            return value < 0 ? -value : value;
        }

        // 水平な面への強さ horizontal(mW/m²)のうち取り分 share を、向き d の光に垂直な面の強さへ: × share × |D| ÷ |d_y|
        std::expected<uint32_t, std::string> PerpendicularMilliwatts(uint64_t horizontal, const SkyDirection& sky) {
            const std::array<int64_t, 3> d = {sky.direction[0], sky.direction[1], sky.direction[2]};
            const auto squared = static_cast<uint64_t>((d[0] * d[0]) + (d[1] * d[1]) + (d[2] * d[2]));
            const uint64_t lengthQ15 = fx::FxSqrtU64(squared << (2 * LENGTH_SHIFT));
            const fx::FxU128 numerator = fx::FxMulU64Full(horizontal * sky.shareQ16, lengthQ15);
            const uint64_t divisor = static_cast<uint64_t>(Absolute(d[1])) << (LENGTH_SHIFT + 16);
            const uint64_t quotient = fx::FxDivU128By64(numerator, divisor).quotient;
            if (quotient > static_cast<uint64_t>(IRRADIANCE_LIMIT))
                return std::unexpected("空の向きの強さが 32bit を超える(差が大きすぎるか、向きが水平に近すぎる)");

            return static_cast<uint32_t>(quotient);
        }

        std::expected<void, std::string> CheckDirections(const std::vector<SkyDirection>& directions) {
            uint64_t shareSum = 0;
            for (const SkyDirection& sky : directions) {
                const bool large = Absolute(sky.direction[0]) > DIRECTION_LIMIT ||
                                   Absolute(sky.direction[1]) > DIRECTION_LIMIT ||
                                   Absolute(sky.direction[2]) > DIRECTION_LIMIT;
                if (sky.direction[1] >= 0 || large)
                    return std::unexpected("空の向きが下を向いていないか、成分が 32768 を超える");

                shareSum += sky.shareQ16;
            }

            if (shareSum != SHARE_ONE_Q16)
                return std::unexpected(std::format("空の向きの取り分の和が 2^16 でない({})", shareSum));

            return {};
        }

    }  // namespace

    std::vector<SkyDirection> DefaultSkyDirections() {
        constexpr uint32_t QUARTER = SHARE_ONE_Q16 / 4;

        return {{.direction = {1, -1, 1}, .shareQ16 = QUARTER},
                {.direction = {-1, -1, 1}, .shareQ16 = QUARTER},
                {.direction = {1, -1, -1}, .shareQ16 = QUARTER},
                {.direction = {-1, -1, -1}, .shareQ16 = QUARTER}};
    }

    uint32_t SkyBlackLongMilliwatts(uint32_t skyMk) {
        const uint64_t longBand = rx::RcBlackIrradianceMilliwatts(skyMk).longBand;

        return longBand > static_cast<uint64_t>(IRRADIANCE_LIMIT) ? static_cast<uint32_t>(IRRADIANCE_LIMIT)
                                                                  : static_cast<uint32_t>(longBand);
    }

    std::expected<std::vector<SkyPortSweep>, std::string> PlanSkyPort(const SkyPortParams& params) {
        if (const auto checked = CheckDirections(params.directions); !checked)
            return std::unexpected(checked.error());

        // --- 帯ごとの差(空 − 参照の黒体)---
        const rx::RcBands reference = rx::RcBlackIrradianceMilliwatts(params.referenceMk);
        const std::array<int64_t, 2> net = {
            static_cast<int64_t>(params.skyShortMilliwatts) - static_cast<int64_t>(reference.shortBand),
            static_cast<int64_t>(params.skyLongMilliwatts) - static_cast<int64_t>(reference.longBand)};

        // --- 向きごと × 正と負 ---
        std::vector<SkyPortSweep> sweeps;
        for (const bool negative : {false, true}) {
            std::array<uint64_t, 2> magnitude{};
            for (size_t band = 0; band < 2; ++band) {
                const bool take = negative ? net[band] < 0 : net[band] > 0;
                magnitude[band] = take ? static_cast<uint64_t>(Absolute(net[band])) : 0;
            }

            if (magnitude[0] == 0 && magnitude[1] == 0)
                continue;

            for (const SkyDirection& sky : params.directions) {
                const auto shortBand = PerpendicularMilliwatts(magnitude[0], sky);
                const auto longBand = PerpendicularMilliwatts(magnitude[1], sky);
                if (!shortBand || !longBand)
                    return std::unexpected(!shortBand ? shortBand.error() : longBand.error());

                if (*shortBand == 0 && *longBand == 0)
                    continue;

                sweeps.push_back({.light = {.direction = sky.direction,
                                            .irradianceShortMilliwatts = *shortBand,
                                            .irradianceLongMilliwatts = *longBand},
                                  .negative = negative});
            }
        }

        return sweeps;
    }

    std::expected<SkyPortResult, std::string> SweepSkyPort(const MultiresNest& nest, const BakedReactionTable& table,
                                                           const SkyPortParams& params, bool reverse) {
        const auto sweeps = PlanSkyPort(params);
        if (!sweeps)
            return std::unexpected(sweeps.error());

        SkyPortResult result;
        for (std::vector<uint64_t>& words : result.words)
            words.assign(SunColumnWordCount(nest), 0);

        const size_t count = sweeps->size();
        for (size_t step = 0; step < count; ++step) {
            const SkyPortSweep& sweep = (*sweeps)[reverse ? count - 1 - step : step];
            const auto sunParams = MakeSunParams(nest, sweep.light, params.rayLevel);
            if (!sunParams)
                return std::unexpected(sunParams.error());

            const std::vector<uint64_t> words = SweepSunColumn(nest, table, *sunParams, reverse);
            std::vector<uint64_t>& sum = result.words[sweep.negative ? 1 : 0];
            for (size_t word = 0; word < words.size(); ++word)
                sum[word] += words[word];

            ++result.sweeps;
        }

        return result;
    }

    int64_t SkyPortAbsorbed(const SkyPortResult& result, uint32_t slot, uint32_t index, uint32_t band) {
        const uint32_t word = mr::SunCellWord(slot, index, band, mr::SUN_KIND_ABSORBED);

        return static_cast<int64_t>(result.words[0][word]) - static_cast<int64_t>(result.words[1][word]);
    }

}  // namespace bicameral::sim
