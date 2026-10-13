// environment_package.cpp — 合わせた表の分類 environment を sim::WorldPhysics に読む(environment_package.h。T-0400・ADR-0176)。
// 読むだけで、値の意味(重力の向き・音速の基準からの係数)は器(world_physics.h・gas_reference.cpp)が決める。
// どの値も Luau の倍精度から誤差なしで整数に直せるものだけ通す(04 R7。reaction_package.cpp と同じ決まり)。
#include "script/environment_package.h"

#include <cmath>
#include <cstdint>
#include <format>

#include "sim/gas_reference.h"

namespace bicameral::script {

    namespace {

        constexpr int64_t EXACT_INTEGER_LIMIT = int64_t{1} << 53;  // 倍精度で誤差なしに表せる整数の上限

        // 整数で書いた数を読む(小数・[minimum, maximum] の外は誤り)
        std::expected<uint32_t, std::string> ReadValue(const TableEntry& entry, std::string_view key, int64_t minimum,
                                                       int64_t maximum) {
            const std::string path = std::format("{}.{}('{}' が書いた)", ENVIRONMENT_CATEGORY, key, entry.package);
            constexpr auto LIMIT = static_cast<double>(EXACT_INTEGER_LIMIT);
            const ScriptValue& value = entry.value;
            const bool exact = value.IsNumber() && value.number >= -LIMIT && value.number <= LIMIT &&
                               std::trunc(value.number) == value.number;
            if (!exact)
                return std::unexpected(std::format("{}: 整数で書く", path));

            const auto integer = static_cast<int64_t>(value.number);
            if (integer < minimum || integer > maximum)
                return std::unexpected(std::format("{}: {} は範囲 {}〜{} の外", path, integer, minimum, maximum));

            return static_cast<uint32_t>(integer);
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
                const auto gravity = ReadValue(entry, key, 0, fx::GRAVITY_MAX_MICROMETERS_PER_SECOND2);
                if (!gravity)
                    return std::unexpected(gravity.error());

                world.gravityMicrometersPerSecond2 = *gravity;
            } else if (key == ENVIRONMENT_SOUND_SPEED_KEY) {
                const auto soundSpeed = ReadValue(entry, key, 0, soundSpeedLimit);
                if (!soundSpeed)
                    return std::unexpected(soundSpeed.error());

                world.soundSpeedMillimetersPerSecond = *soundSpeed;
            } else {
                return std::unexpected(
                    std::format("{}.{}('{}' が書いた): 知らない鍵", ENVIRONMENT_CATEGORY, key, entry.package));
            }
        }

        return world;
    }

}  // namespace bicameral::script
