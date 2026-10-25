// sky_port.cpp — 空との交換の口(sky_port.h。T-1145・ADR-0474)。掃きの本体は太陽の影の柱(sun_column)。
#include "sim/sky_port.h"

#include <format>
#include <optional>

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

        // 今の刻みに掃く向き(回さないなら全部。回すなら tick mod n の 1 つを n 倍の取り分で)
        std::vector<SkyDirection> ActiveDirections(const SkyPortParams& params) {
            if (!params.rotateDirections || params.directions.empty())
                return params.directions;

            const auto count = static_cast<uint32_t>(params.directions.size());
            SkyDirection active = params.directions[params.tick % count];
            active.shareQ16 *= count;

            return {active};
        }

        // 帯ごとの差(空 − 参照の黒体。mW/m²)
        std::array<int64_t, 2> NetBands(const SkyPortParams& params) {
            const rx::RcBands reference = rx::RcBlackIrradianceMilliwatts(params.referenceMk);

            return {static_cast<int64_t>(params.skyShortMilliwatts) - static_cast<int64_t>(reference.shortBand),
                    static_cast<int64_t>(params.skyLongMilliwatts) - static_cast<int64_t>(reference.longBand)};
        }

        // 向き 1 つの、帯ごとの大きさ magnitude を運ぶ光(どちらの帯も 0 なら無し)
        std::expected<std::optional<SunLight>, std::string> DirectionLight(const SkyDirection& sky,
                                                                           const std::array<uint64_t, 2>& magnitude) {
            const auto shortBand = PerpendicularMilliwatts(magnitude[0], sky);
            const auto longBand = PerpendicularMilliwatts(magnitude[1], sky);
            if (!shortBand || !longBand)
                return std::unexpected(!shortBand ? shortBand.error() : longBand.error());

            if (*shortBand == 0 && *longBand == 0)
                return std::nullopt;

            return SunLight{.direction = sky.direction,
                            .irradianceShortMilliwatts = *shortBand,
                            .irradianceLongMilliwatts = *longBand};
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

        const std::array<int64_t, 2> net = NetBands(params);
        const std::vector<SkyDirection> directions = ActiveDirections(params);

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

            for (const SkyDirection& sky : directions) {
                const auto light = DirectionLight(sky, magnitude);
                if (!light)
                    return std::unexpected(light.error());

                if (light->has_value())
                    sweeps.push_back({.light = **light, .negative = negative});
            }
        }

        return sweeps;
    }

    std::expected<SkyPortWork, std::string> MakeSkyPortWork(const MultiresNest& nest, const SkyPortParams& params) {
        if (const auto checked = CheckDirections(params.directions); !checked)
            return std::unexpected(checked.error());

        if (params.directions.size() > mr::SKY_PORT_SWEEP_LIMIT)
            return std::unexpected(std::format("空の向きが {} を超える", mr::SKY_PORT_SWEEP_LIMIT));

        // --- 帯ごとの大きさと足す側(帯の差の符号)---
        const std::array<int64_t, 2> net = NetBands(params);
        SkyPortWork work;
        std::array<uint64_t, 2> magnitude{};
        for (size_t band = 0; band < 2; ++band) {
            magnitude[band] = static_cast<uint64_t>(Absolute(net[band]));
            work.bandSide[band] = net[band] < 0 ? 1u : 0u;
        }

        // --- 向きごとに掃き 1 本(両方の帯を運ぶ)---
        for (const SkyDirection& sky : ActiveDirections(params)) {
            const auto light = DirectionLight(sky, magnitude);
            if (!light)
                return std::unexpected(light.error());

            if (!light->has_value())
                continue;

            const auto sunParams = MakeSunParams(nest, **light, params.rayLevel);
            if (!sunParams)
                return std::unexpected(sunParams.error());

            work.sweeps.push_back(*sunParams);
        }

        return work;
    }

    std::expected<SkyPortResult, std::string> SweepSkyPort(const MultiresNest& nest, const BakedReactionTable& table,
                                                           const SkyPortParams& params, bool reverse) {
        const auto work = MakeSkyPortWork(nest, params);
        if (!work)
            return std::unexpected(work.error());

        SkyPortResult result;
        for (std::vector<uint64_t>& words : result.words)
            words.assign(SunColumnWordCount(nest), 0);

        // --- 向きごとに掃き、語を帯の側へ足す ---
        const size_t count = work->sweeps.size();
        for (size_t step = 0; step < count; ++step) {
            const mr::SunParams& sweep = work->sweeps[reverse ? count - 1 - step : step];
            const std::vector<uint64_t> words = SweepSunColumn(nest, table, sweep, reverse);
            for (size_t word = 0; word < words.size(); ++word) {
                const uint32_t side = work->bandSide[mr::SkyPortWordBand(static_cast<uint32_t>(word))];
                result.words[side][word] += words[word];
            }

            ++result.sweeps;
        }

        return result;
    }

    int64_t SkyPortAbsorbed(const SkyPortResult& result, uint32_t slot, uint32_t index, uint32_t band) {
        const uint32_t word = mr::SunCellWord(slot, index, band, mr::SUN_KIND_ABSORBED);

        return static_cast<int64_t>(result.words[0][word]) - static_cast<int64_t>(result.words[1][word]);
    }

}  // namespace bicameral::sim
