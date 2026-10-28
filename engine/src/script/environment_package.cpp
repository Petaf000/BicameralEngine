// environment_package.cpp — 合わせた表の分類 environment を sim::WorldPhysics に読む(environment_package.h。T-0400・ADR-0176)。
// 読むだけで、値の意味(重力の向き・音速の基準からの係数)は器(world_physics.h・gas_reference.cpp)が決める。
// どの値も Luau の倍精度から誤差なしで整数に直せるものだけ通す(04 R7。reaction_package.cpp と同じ決まり)。
#include "script/environment_package.h"

#include <array>
#include <cstdint>
#include <format>
#include <iterator>

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

        // 成分の表(物質 → ppm)を名前のまま読む(物質の名前と合計はベイクが確かめる)
        std::expected<std::vector<sim::AtmosphereFractionDefinition>, std::string> ReadComposition(
            const ScriptValue& value, const std::string& path) {
            if (auto table = RequireTable(value, path); !table)
                return std::unexpected(table.error());

            std::vector<sim::AtmosphereFractionDefinition> fractions;
            for (const ScriptField& field : value.fields) {
                if (!field.key.IsString())
                    return std::unexpected(std::format("{}: 鍵は物質の名前", path));

                const auto parts = ReadInteger(field.value, std::format("{}.{}", path, field.key.text), 0,
                                               sim::ATMOSPHERE_PARTS_PER_WHOLE);
                if (!parts)
                    return std::unexpected(parts.error());

                fractions.push_back({.species = field.key.text, .partsPerMillion = static_cast<uint32_t>(*parts)});
            }

            return fractions;
        }

        // 高さの 1 つの点 { height_mm, temperature_mk, composition }
        std::expected<sim::AtmospherePointDefinition, std::string> ReadProfilePoint(const ScriptValue& value,
                                                                                    const std::string& path) {
            static constexpr std::string_view FIELDS[] = {"height_mm", "temperature_mk", "composition"};
            if (auto record = CheckRecord(value, FIELDS, path); !record)
                return std::unexpected(record.error());

            const auto height = ReadIntegerField(value, "height_mm", path, -sim::ATMOSPHERE_HEIGHT_LIMIT_MILLIMETERS,
                                                 sim::ATMOSPHERE_HEIGHT_LIMIT_MILLIMETERS);
            if (!height)
                return std::unexpected(height.error());

            const auto temperature = ReadIntegerField(value, "temperature_mk", path, 1,
                                                      ATMOSPHERE_TEMPERATURE_MAX_MILLIKELVIN);
            if (!temperature)
                return std::unexpected(temperature.error());

            const auto composition = RequireField(value, "composition", path);
            if (!composition)
                return std::unexpected(composition.error());

            auto fractions = ReadComposition(**composition, path + ".composition");
            if (!fractions)
                return std::unexpected(fractions.error());

            return sim::AtmospherePointDefinition{.label = path,
                                                  .heightMillimeters = *height,
                                                  .temperatureMilliKelvin = static_cast<uint32_t>(*temperature),
                                                  .fractions = std::move(*fractions)};
        }

        // environment.atmosphere_profile = { {…}, {…} }(書いた順。鍵は 1, 2, 3…。高さの順はベイクが確かめる)
        std::expected<std::vector<sim::AtmospherePointDefinition>, std::string> ReadProfile(const TableEntry& entry) {
            const std::string path = std::format("{}.{}", ENVIRONMENT_CATEGORY, ENVIRONMENT_ATMOSPHERE_PROFILE_KEY);
            const auto fail = [&](const std::string& message) {
                return std::unexpected(std::format("パッケージ {}: {}", entry.package, message));
            };
            if (!entry.value.IsTable() || entry.value.fields.empty())
                return fail(std::format("{}: 点の列(1, 2, 3…)で書く", path));

            std::vector<sim::AtmospherePointDefinition> points;
            for (const ScriptField& field : entry.value.fields) {
                if (!field.key.IsNumber() || field.key.number != static_cast<double>(points.size() + 1))
                    return fail(std::format("{}: 点の列(1, 2, 3…)で書く", path));

                auto point = ReadProfilePoint(field.value, std::format("{}[{}]", path, points.size() + 1));
                if (!point)
                    return fail(point.error());

                points.push_back(std::move(*point));
            }

            return points;
        }

        // 一様な大気(T-0373 の書き方): environment.atmosphere_temperature_mk と分類 atmosphere = 高さ 0 の点 1 つ。
        // 片方だけでも点を作り、無い方はベイクが「成分が無い」「温度が無い」で止める
        std::expected<sim::AtmospherePointDefinition, std::string> ReadUniform(
            const TableEntry* temperature, const std::map<std::string, TableEntry>* composition) {
            sim::AtmospherePointDefinition point;
            if (temperature != nullptr) {
                const auto value = ReadValue(std::string(ENVIRONMENT_ATMOSPHERE_TEMPERATURE_KEY), *temperature, 1,
                                             ATMOSPHERE_TEMPERATURE_MAX_MILLIKELVIN);
                if (!value)
                    return std::unexpected(value.error());

                point.temperatureMilliKelvin = *value;
            }

            if (composition == nullptr)
                return point;

            for (const auto& [species, entry] : *composition) {
                const auto parts = ReadInteger(entry.value, std::format("{}.{}", ATMOSPHERE_CATEGORY, species), 0,
                                               sim::ATMOSPHERE_PARTS_PER_WHOLE);
                if (!parts)
                    return std::unexpected(std::format("パッケージ {}: {}", entry.package, parts.error()));

                point.fractions.push_back({.species = species, .partsPerMillion = static_cast<uint32_t>(*parts)});
            }

            return point;
        }

        // 世界の端の 1 面("open" か "wall")
        std::expected<sim::WorldEdge, std::string> ReadEdge(const ScriptValue& value, const std::string& path) {
            if (value.IsString() && value.text == "open")
                return sim::WorldEdge::Open;

            if (value.IsString() && value.text == "wall")
                return sim::WorldEdge::Wall;

            return std::unexpected(std::format("{}: \"open\" か \"wall\" で書く", path));
        }

        // environment.world_edges = { top, bottom, sides }(T-0710・土台の決定 5)。書かない面は wall
        std::expected<sim::WorldEdges, std::string> ReadEdges(const TableEntry& entry) {
            const std::string path = std::format("{}.{}", ENVIRONMENT_CATEGORY, ENVIRONMENT_WORLD_EDGES_KEY);
            static constexpr std::string_view FIELDS[] = {"top", "bottom", "sides"};
            if (auto record = CheckRecord(entry.value, FIELDS, path); !record)
                return std::unexpected(std::format("パッケージ {}: {}", entry.package, record.error()));

            sim::WorldEdges edges;
            sim::WorldEdge* const targets[] = {&edges.top, &edges.bottom, &edges.sides};
            for (size_t i = 0; i < std::size(FIELDS); ++i) {
                const ScriptValue* value = entry.value.Find(FIELDS[i]);
                if (value == nullptr)
                    continue;

                const auto edge = ReadEdge(*value, std::format("{}.{}", path, FIELDS[i]));
                if (!edge)
                    return std::unexpected(std::format("パッケージ {}: {}", entry.package, edge.error()));

                *targets[i] = *edge;
            }

            return edges;
        }

        // environment.gas_pressure_speed = "sound" か "instant"(T-0804・D-526 ②。docs/design/20 §1.1)
        std::expected<sim::GasPressureSpeed, std::string> ReadPressureSpeed(const TableEntry& entry) {
            if (entry.value.IsString() && entry.value.text == "sound")
                return sim::GasPressureSpeed::Sound;

            if (entry.value.IsString() && entry.value.text == "instant")
                return sim::GasPressureSpeed::Instant;

            return std::unexpected(std::format("パッケージ {}: {}.{}: \"sound\"(本物の音速)か \"instant\"(最速)で書く",
                                               entry.package, ENVIRONMENT_CATEGORY,
                                               ENVIRONMENT_GAS_PRESSURE_SPEED_KEY));
        }

        // environment.cell_layers = true / false(成分ごとの層を使う世界。T-1241・ADR-0489 の 3)
        std::expected<bool, std::string> ReadCellLayers(const TableEntry& entry) {
            if (entry.value.IsBoolean())
                return entry.value.boolean;

            return std::unexpected(std::format("パッケージ {}: {}.{}: true(層を使う世界)か false(使わない)で書く",
                                               entry.package, ENVIRONMENT_CATEGORY, ENVIRONMENT_CELL_LAYERS_KEY));
        }

        // environment.balance_cycles = { gas_pressure, liquid_pressure, support }(T-0955)。書かない法則は既定の 2
        std::expected<sim::BalanceCycles, std::string> ReadBalanceCycles(const TableEntry& entry) {
            const std::string path = std::format("{}.{}", ENVIRONMENT_CATEGORY, ENVIRONMENT_BALANCE_CYCLES_KEY);
            static constexpr std::string_view FIELDS[] = {"gas_pressure", "liquid_pressure", "support"};
            if (auto record = CheckRecord(entry.value, FIELDS, path); !record)
                return std::unexpected(std::format("パッケージ {}: {}", entry.package, record.error()));

            sim::BalanceCycles cycles;
            uint32_t* const targets[] = {&cycles.gasPressure, &cycles.liquidPressure, &cycles.support};
            for (size_t i = 0; i < std::size(FIELDS); ++i) {
                const ScriptValue* value = entry.value.Find(FIELDS[i]);
                if (value == nullptr)
                    continue;

                const auto count = ReadInteger(*value, std::format("{}.{}", path, FIELDS[i]), 1,
                                               sim::BALANCE_CYCLES_MAX);
                if (!count)
                    return std::unexpected(std::format("パッケージ {}: {}", entry.package, count.error()));

                *targets[i] = static_cast<uint32_t>(*count);
            }

            return cycles;
        }

        // environment.fine_region = { radius_mm, far_leaf_mm, levels_per_doubling }(T-0954)。書かない欄は既定
        std::expected<sim::FineRegion, std::string> ReadFineRegion(const TableEntry& entry) {
            const std::string path = std::format("{}.{}", ENVIRONMENT_CATEGORY, ENVIRONMENT_FINE_REGION_KEY);
            static constexpr std::string_view FIELDS[] = {"radius_mm", "far_leaf_mm", "levels_per_doubling"};
            if (auto record = CheckRecord(entry.value, FIELDS, path); !record)
                return std::unexpected(std::format("パッケージ {}: {}", entry.package, record.error()));

            sim::FineRegion region;
            const auto lengthMax = static_cast<int64_t>(sim::FINE_REGION_LENGTH_MAX_MM);
            const std::array<int64_t, 3> minimums = {0, 1, 1};
            const std::array<int64_t, 3> maximums = {lengthMax, lengthMax, sim::FINE_REGION_LEVELS_PER_DOUBLING_MAX};
            std::array<int64_t, 3> values = {static_cast<int64_t>(region.radiusMm),
                                             static_cast<int64_t>(region.farLeafMm), region.levelsPerDoubling};
            for (size_t i = 0; i < std::size(FIELDS); ++i) {
                const ScriptValue* value = entry.value.Find(FIELDS[i]);
                if (value == nullptr)
                    continue;

                const auto read = ReadInteger(*value, std::format("{}.{}", path, FIELDS[i]), minimums[i], maximums[i]);
                if (!read)
                    return std::unexpected(std::format("パッケージ {}: {}", entry.package, read.error()));

                values[i] = *read;
            }

            region.radiusMm = static_cast<uint64_t>(values[0]);
            region.farLeafMm = static_cast<uint64_t>(values[1]);
            region.levelsPerDoubling = static_cast<uint32_t>(values[2]);

            return region;
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
            } else if (key == ENVIRONMENT_GAS_EDDY_VISCOSITY_KEY) {
                const auto viscosity = ReadValue(key, entry, 0,
                                                 static_cast<int64_t>(gas::GF_VISCOSITY_LIMIT_MM2_PER_S));
                if (!viscosity)
                    return std::unexpected(viscosity.error());

                world.gasEddyViscosityMm2PerS = *viscosity;
            } else if (key == ENVIRONMENT_WORLD_EDGES_KEY) {
                const auto edges = ReadEdges(entry);
                if (!edges)
                    return std::unexpected(edges.error());

                world.edges = *edges;
            } else if (key == ENVIRONMENT_GAS_PRESSURE_SPEED_KEY) {
                const auto speed = ReadPressureSpeed(entry);
                if (!speed)
                    return std::unexpected(speed.error());

                world.gasPressureSpeed = *speed;
            } else if (key == ENVIRONMENT_CELL_LAYERS_KEY) {
                const auto layers = ReadCellLayers(entry);
                if (!layers)
                    return std::unexpected(layers.error());

                world.cellLayers = *layers;
            } else if (key == ENVIRONMENT_BALANCE_CYCLES_KEY) {
                const auto cycles = ReadBalanceCycles(entry);
                if (!cycles)
                    return std::unexpected(cycles.error());

                world.balanceCycles = *cycles;
            } else if (key == ENVIRONMENT_FINE_REGION_KEY) {
                const auto region = ReadFineRegion(entry);
                if (!region)
                    return std::unexpected(region.error());

                world.fineRegion = *region;
            } else if (key == ENVIRONMENT_ATMOSPHERE_TEMPERATURE_KEY || key == ENVIRONMENT_ATMOSPHERE_PRESSURE_KEY ||
                       key == ENVIRONMENT_ATMOSPHERE_PROFILE_KEY) {
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
        const TableEntry* temperature = nullptr;
        const TableEntry* profile = nullptr;

        // --- 分類 environment の大気の値(地面の圧力・一様な大気の温度・高さの点の列)---
        if (const auto environment = packages.tables.find(std::string(ENVIRONMENT_CATEGORY));
            environment != packages.tables.end()) {
            for (const auto& [key, entry] : environment->second) {
                if (key == ENVIRONMENT_ATMOSPHERE_TEMPERATURE_KEY) {
                    temperature = &entry;
                } else if (key == ENVIRONMENT_ATMOSPHERE_PROFILE_KEY) {
                    profile = &entry;
                } else if (key == ENVIRONMENT_ATMOSPHERE_PRESSURE_KEY) {
                    const auto pressure = ReadValue(key, entry, 1, ATMOSPHERE_PRESSURE_MAX_PASCALS);
                    if (!pressure)
                        return std::unexpected(pressure.error());

                    atmosphere.surfacePressurePascals = *pressure;
                }
            }
        }

        const auto uniform = packages.tables.find(std::string(ATMOSPHERE_CATEGORY));
        const bool hasUniform = temperature != nullptr || uniform != packages.tables.end();
        if (profile != nullptr && hasUniform)
            return std::unexpected(
                std::format("パッケージ {}: {}.{}(高さの点の列)と一様な大気({}・{}.{})は一緒に書けない(どちらか一方)",
                            profile->package, ENVIRONMENT_CATEGORY, ENVIRONMENT_ATMOSPHERE_PROFILE_KEY,
                            ATMOSPHERE_CATEGORY, ENVIRONMENT_CATEGORY, ENVIRONMENT_ATMOSPHERE_TEMPERATURE_KEY));

        if (profile != nullptr) {
            auto points = ReadProfile(*profile);
            if (!points)
                return std::unexpected(points.error());

            atmosphere.points = std::move(*points);
        } else if (hasUniform) {
            auto point = ReadUniform(temperature, uniform == packages.tables.end() ? nullptr : &uniform->second);
            if (!point)
                return std::unexpected(point.error());

            atmosphere.points.push_back(std::move(*point));
        }

        return atmosphere;
    }

}  // namespace bicameral::script
