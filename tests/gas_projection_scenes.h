// gas_projection_scenes.h — 気体の陰の圧力の試験の場面(T-0845 の 5 場面。gpu_gas_projection_test から移した。値は同じ)。
// gpu_gas_projection_test(GPU と CPU の比べ)と equilibrium_test(釣り合う家族の部品へ移した CPU の比べ。T-0853)が使う。
// 閉じた箱(一辺 n。重さあり)の 熱い泡(+30 K・同じ圧力)/ 扉(x の半分が 2% 高い圧力)/ 強い爆発(真ん中 1 セル 5 倍の圧力・1500 K)/
// 孔のある壁(塞がった板に 1/2 と 1/256 の孔)/ 細孔の板(1 層を全部 1/256・下が 2% 高い)。
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "sim/gas_reference.h"
#include "sim/world_physics.h"
#include "standard_world_physics.h"

namespace bicameral::gas_scenes {

    // --- 試験の空気(gas_projection_test と同じ。N2 79% + O2 21% と煙の印)---
    inline constexpr uint32_t SPECIES_N2 = 0;
    inline constexpr uint32_t SPECIES_SMOKE = 2;
    inline constexpr std::array<uint32_t, 3> AIR_FRACTIONS_Q32 = {3393024163u, 901943132u, 0u};
    inline constexpr uint32_t AIR_TEMPERATURE = 293150;                   // mK
    inline constexpr uint64_t SURFACE_PRESSURE = 101325ull * 1000000ull;  // µPa
    inline constexpr uint32_t REAL_SOUND = 340000;                        // mm/s
    inline constexpr uint32_t BUBBLE_HEAT = 30000;                        // mK
    inline constexpr uint32_t BLAST_TEMPERATURE = 1500000;                // mK

    inline sim::GasConfig MakeAirConfig(uint32_t edge) {
        sim::GasConfig config;
        config.sizeX = edge;
        config.sizeY = edge;
        config.sizeZ = edge;
        config.speciesCount = 3;
        config.species[0] = {.molarMass = 28013, .heatCapacity = 20810, .formationEnergy = 0};  // N2
        config.species[1] = {.molarMass = 31999, .heatCapacity = 21064, .formationEnergy = 0};  // O2
        config.species[2] = {.molarMass = 28013, .heatCapacity = 20810, .formationEnergy = 0};  // 煙の印
        sim::ApplyWorldPhysics(sim::StandardWorldPhysics(), config);

        return config;
    }

    inline sim::GasBox MakeAirBox(uint32_t edge) {
        const sim::GasConfig config = MakeAirConfig(edge);
        return sim::MakeGasBox(
            config, sim::MakeHydrostaticReference(config, AIR_FRACTIONS_Q32, AIR_TEMPERATURE, SURFACE_PRESSURE));
    }

    // 同じ圧力のまま温度を変える(物質量を温度に反比例させる)
    inline void SetTemperature(sim::GasBox& box, uint32_t index, uint32_t temperature, uint64_t pressurePercent) {
        sim::GasPerSpecies<uint64_t> amounts = box.cells[index].amounts;
        for (uint64_t& amount : amounts)
            amount = amount * pressurePercent * AIR_TEMPERATURE / (100 * (uint64_t)temperature);

        box.cells[index] = sim::MakeGasCell(box.config, amounts, temperature);
    }

    // 壁 = x の真ん中の板を塞ぎ(空き 0)、孔 4 セル(空き 1/2 が 2・1/256 が 2)。左が 2% 高い圧力(T-0667)。
    // 板 = z = 一辺 ÷ 4 の層を全部 空き 1/256(細い孔の板)。下が 2% 高い圧力
    enum class Scene : uint8_t { Bubble, Door, Blast, Wall, Plate };

    inline constexpr std::array<Scene, 5> SCENES = {Scene::Bubble, Scene::Door, Scene::Blast, Scene::Wall,
                                                    Scene::Plate};
    inline constexpr std::array<uint32_t, 2> SPEEDS = {0, REAL_SOUND};

    inline std::string_view SceneName(Scene scene) {
        switch (scene) {
            case Scene::Bubble: return "泡";
            case Scene::Door: return "扉";
            case Scene::Blast: return "爆発";
            case Scene::Wall: return "孔のある壁";
            case Scene::Plate: return "細孔の板";
        }

        return "";
    }

    // セルの空きを share にし、気体を基準 × share に減らす(固体が占めた分の空気は無い。T-0667)
    inline void Occupy(sim::GasBox& box, uint32_t index, uint32_t share) {
        if (box.openQ16.empty())
            box.openQ16.assign(box.cells.size(), sim::GAS_OPEN_FULL);

        box.openQ16[index] = share;
        sim::GasPerSpecies<uint64_t> amounts = box.cells[index].amounts;
        for (uint64_t& amount : amounts)
            amount = amount * share / sim::GAS_OPEN_FULL;

        box.cells[index] = sim::MakeGasCell(box.config, amounts, AIR_TEMPERATURE);
    }

    // 壁・板の空き(Q16。全部開いたセルは GAS_OPEN_FULL)
    inline uint32_t SceneShare(Scene scene, uint32_t edge, int32_t x, int32_t y, int32_t z) {
        const auto center = (int32_t)(edge / 2);
        if (scene == Scene::Plate)
            return z == center / 2 ? sim::GAS_OPEN_FULL / 256 : sim::GAS_OPEN_FULL;

        if (scene != Scene::Wall || x != center)
            return sim::GAS_OPEN_FULL;

        const bool holeY = y == center - 1 || y == center;
        if (!holeY || (z != center - 1 && z != center))
            return 0;

        return z == center - 1 ? sim::GAS_OPEN_FULL / 2 : sim::GAS_OPEN_FULL / 256;
    }

    inline sim::GasBox MakeScene(Scene scene, uint32_t edge) {
        sim::GasBox box = MakeAirBox(edge);
        const auto center = (int32_t)(edge / 2);
        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const auto x = (int32_t)(index % edge);
            const auto y = (int32_t)((index / edge) % edge);
            const auto z = (int32_t)(index / (edge * edge));
            if (scene == Scene::Bubble) {
                const int32_t dx = x - center;
                const int32_t dy = y - center;
                const int32_t dz = z - (center / 2);
                if ((dx * dx) + (dy * dy) + (dz * dz) <= 2)
                    SetTemperature(box, index, AIR_TEMPERATURE + BUBBLE_HEAT, 100);
            } else if (scene == Scene::Door || scene == Scene::Wall) {
                if (x < center)
                    SetTemperature(box, index, AIR_TEMPERATURE, 102);
            } else if (scene == Scene::Plate) {
                if (z < center / 2)
                    SetTemperature(box, index, AIR_TEMPERATURE, 102);
            } else if (x == center && y == center && z == center) {
                SetTemperature(box, index, BLAST_TEMPERATURE, 500);
                sim::GasCell& cell = box.cells[index];
                const uint64_t moved = cell.amounts[SPECIES_N2] / 2;
                cell.amounts[SPECIES_N2] -= moved;
                cell.amounts[SPECIES_SMOKE] += moved;
            }
        }

        if (scene != Scene::Wall && scene != Scene::Plate)
            return box;

        for (uint32_t index = 0; index < box.cells.size(); ++index) {
            const auto x = (int32_t)(index % edge);
            const auto y = (int32_t)((index / edge) % edge);
            const auto z = (int32_t)(index / (edge * edge));
            const uint32_t share = SceneShare(scene, edge, x, y, z);
            if (share != sim::GAS_OPEN_FULL)
                Occupy(box, index, share);
        }

        return box;
    }

}  // namespace bicameral::gas_scenes
