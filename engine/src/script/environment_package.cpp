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
            } else {
                return std::unexpected(
                    std::format("パッケージ {}: {}.{}: 知らない鍵", entry.package, ENVIRONMENT_CATEGORY, key));
            }
        }

        return world;
    }

}  // namespace bicameral::script
