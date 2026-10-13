// environment_package.cpp — 合わせた表の分類 environment を sim::WorldPhysics に読む(environment_package.h。T-0400・ADR-0176)。
// 読むだけで、値の意味(重力の向き・音速の基準からの係数)は器(world_physics.h・gas_reference.cpp)が決める。
// どの値も Luau の倍精度から誤差なしで整数に直せるものだけ通す(04 R7。reaction_package.cpp と同じ決まり)。
#include "script/environment_package.h"

#include <cstdint>
#include <format>

#include "script/package_fields.h"
#include "sim/gas_reference.h"

namespace bicameral::script {

    namespace {

        // 基準の大気の値の上限(器の決まり。温度はセルの温度と同じ int32 の mK、圧力は 10 億 Pa = 1 万気圧)
        constexpr int64_t ATMOSPHERE_TEMPERATURE_MAX_MILLIKELVIN = 2147483647;
        constexpr int64_t ATMOSPHERE_PRESSURE_MAX_PASCALS = 1000000000;

        // 1 つの値を整数で読む([minimum, maximum] の外は誤り)。誤りには、その鍵を書いたパッケージの名前を付ける
        std::expected<uint32_t, std::string> ReadValue(const std::string& key, const TableEntry& entry, int64_t minimum,
                                                       int64_t maximum) {
            const auto value = ReadInteger(entry.value, std::format("{}.{}", ENVIRONMENT_CATEGORY, key), minimum,
                                           maximum);
            if (!value)
                return std::unexpected(std::format("パッケージ {}: {}", entry.package, value.error()));

            return static_cast<uint32_t>(*value);
        }

    }  // namespace

    std::expected<sim::WorldPhysics, std::string> ReadWorldEnvironment(const PackageSetResult& packages) {
        sim::WorldPhysics world;
        const auto category = packages.tables.find(std::string(ENVIRONMENT_CATEGORY));
        if (category == packages.tables.end())
            return world;  // ブランクの世界(環境の値が無い。D-455)

        // 音速の基準の上限は、気体の既定の小刻みの安定の条件(ν ≤ 0.6。gas_reference.h)
        const auto soundSpeedLimit = static_cast<int64_t>(sim::GasSoundSpeedLimitMmPerS(sim::GasConfig{}.substeps));

        for (const auto& [key, entry] : category->second) {
            if (key == ENVIRONMENT_GRAVITY_KEY) {
                const auto gravity = ReadValue(key, entry, 0, fx::GRAVITY_MAX_MICROMETERS_PER_SECOND2);
                if (!gravity)
                    return std::unexpected(gravity.error());

                world.gravityMicrometersPerSecond2 = *gravity;
            } else if (key == ENVIRONMENT_SOUND_SPEED_KEY) {
                const auto soundSpeed = ReadValue(key, entry, 0, soundSpeedLimit);
                if (!soundSpeed)
                    return std::unexpected(soundSpeed.error());

                world.soundSpeedMillimetersPerSecond = *soundSpeed;
            } else if (key == ENVIRONMENT_ATMOSPHERE_TEMPERATURE_KEY || key == ENVIRONMENT_ATMOSPHERE_PRESSURE_KEY) {
                continue;  // 基準の大気の値(ReadWorldAtmosphere が読む)
            } else {
                return std::unexpected(
                    std::format("パッケージ {}: {}.{}: 知らない鍵", entry.package, ENVIRONMENT_CATEGORY, key));
            }
        }

        return world;
    }

    std::expected<sim::WorldAtmosphereDefinition, std::string> ReadWorldAtmosphere(const PackageSetResult& packages) {
        sim::WorldAtmosphereDefinition atmosphere;

        // --- 温度と地面の圧力(分類 environment の 2 つの鍵)---
        if (const auto environment = packages.tables.find(std::string(ENVIRONMENT_CATEGORY));
            environment != packages.tables.end()) {
            for (const auto& [key, entry] : environment->second) {
                if (key == ENVIRONMENT_ATMOSPHERE_TEMPERATURE_KEY) {
                    const auto temperature = ReadValue(key, entry, 1, ATMOSPHERE_TEMPERATURE_MAX_MILLIKELVIN);
                    if (!temperature)
                        return std::unexpected(temperature.error());

                    atmosphere.temperatureMilliKelvin = *temperature;
                } else if (key == ENVIRONMENT_ATMOSPHERE_PRESSURE_KEY) {
                    const auto pressure = ReadValue(key, entry, 1, ATMOSPHERE_PRESSURE_MAX_PASCALS);
                    if (!pressure)
                        return std::unexpected(pressure.error());

                    atmosphere.surfacePressurePascals = *pressure;
                }
            }
        }

        // --- 成分(分類 atmosphere。鍵 = 物質の名前)---
        const auto category = packages.tables.find(std::string(ATMOSPHERE_CATEGORY));
        if (category == packages.tables.end())
            return atmosphere;

        for (const auto& [species, entry] : category->second) {
            const auto parts = ReadInteger(entry.value, std::format("{}.{}", ATMOSPHERE_CATEGORY, species), 0,
                                           sim::ATMOSPHERE_PARTS_PER_WHOLE);
            if (!parts)
                return std::unexpected(std::format("パッケージ {}: {}", entry.package, parts.error()));

            atmosphere.fractions.push_back({.species = species, .partsPerMillion = static_cast<uint32_t>(*parts)});
        }

        return atmosphere;
    }

}  // namespace bicameral::script
