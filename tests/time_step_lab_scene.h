// time_step_lab_scene.h — 研究 R-T2(T-0856)の試作の場面(sim/time_step_lab.h)。テストの側なので浮動小数点で物の値を決め、整数に直して渡す。
//   川: 一定の速さで流れる水の列(溶けた物 D と熱を運ぶ・乱れで混ざる・地面へ熱が逃げる)。底でゆっくり発酵する物(A → P)。
//   火: 動かない燃える物の列(着火の温度を超えると燃え、熱で隣へ燃え移る・空気へ熱が逃げる)。
// どちらも断面 1 m² の列で、セルの長さ dx(0.5 m・2 m)を変えて同じ物理の場面を作る(規則が細かさによらないかを見る)。
// 値: 容量 = c dx・面の伝導 = k ÷ dx・流れ = c v・地面 = h dx(断面 1 m²。どれも 1 刻みあたり)。
#pragma once

#include <cmath>
#include <cstdint>

#include "sim/time_step_lab.h"

namespace bicameral::test {

    inline constexpr double TICK_SECONDS = 1.0 / 60.0;
    inline constexpr double Q16 = 65536.0;
    inline constexpr double Q32 = 4294967296.0;
    inline constexpr double WATER_HEAT = 4.18e6;          // J/(m³·K) = mJ/(m³·mK)
    inline constexpr double WATER_MICROMOL_PPM = 5.55e4;  // 1 m³ の水の 100 万分の 1(µmol)

    inline uint64_t ToQ(double value, double scale) {
        return static_cast<uint64_t>(std::llround(value * scale));
    }

    inline int64_t ToQSigned(double value, double scale) {
        return static_cast<int64_t>(std::llround(value * scale));
    }

    // 1 つの量の列の値(SI。断面 1 m²)
    struct CarriedValues {
        double capacity = 0.0;      // 値 / y の単位 / m³
        double conductivity = 0.0;  // 値 / y の単位 · m / s
        double speed = 0.0;         // m/s
        double ground = 0.0;        // 値 / y の単位 / m³ / s
        double groundValue = 0.0;   // y
        double inflowValue = 0.0;   // y(流れ込みの水の値)
    };

    inline void SetCarried(sim::TimeStepCarried& law, const CarriedValues& values, uint32_t cells, double dx) {
        law.capacityQ16.assign(cells, ToQ(values.capacity * dx, Q16));
        law.groundQ16.assign(cells, ToQ(values.ground * dx * TICK_SECONDS, Q16));
        law.conductQ16.assign(cells - 1, ToQ(values.conductivity / dx * TICK_SECONDS, Q16));
        law.flowQ16.assign(cells - 1, ToQ(values.capacity * values.speed * TICK_SECONDS, Q16));
        law.outflowQ16 = ToQ(values.capacity * values.speed * TICK_SECONDS, Q16);
        law.inflowQ16 = ToQSigned(values.capacity * values.speed * TICK_SECONDS * values.inflowValue, Q16);
        law.groundValueQ16 = ToQSigned(values.groundValue, Q16);
    }

    // --- 川 ---
    struct RiverParameters {
        double cellMeters = 0.5;
        double lengthMeters = 64.0;
        double speed = 0.3;  // m/s
        double dispersion =
            1.0;  // 乱れで混ざる(川の縦の分散 m²/s。熱と D に同じ。2 m のセルの風上の数値の拡散 0.3 より大きく)
        double blockMeters = 8.0;          // ブロックの長さ(隣の差 1 段の決まりを同じ長さで比べる)
        double groundPerCubic = 2.0;       // 地面へ(W/(m³·K))
        double waterTemperature = 288.0;   // K(流れ込み)
        double startTemperature = 290.0;   // K(初め。暖かい水が下っていく)
        double frontMeters = 8.0;          // 初めの山の縁の幅
        double groundTemperature = 283.0;  // K
        double dissolvedPpm = 200.0;       // 流れ込みの D
        double startPpm = 260.0;
        double fermentMolPerCubic = 400.0;  // 底の発酵する物
        double fermentHalfLifeSeconds = 7200.0;
        double fermentJoulePerMol = 1000.0;
    };

    inline uint32_t CellCount(double lengthMeters, double cellMeters) {
        return static_cast<uint32_t>(std::llround(lengthMeters / cellMeters));
    }

    inline sim::TimeStepLabSetup MakeRiverSetup(const RiverParameters& river) {
        const double dx = river.cellMeters;
        const uint32_t cells = CellCount(river.lengthMeters, dx);
        sim::TimeStepLabSetup setup;
        setup.cellCount = cells;
        setup.blockCells = CellCount(river.blockMeters, dx);
        setup.inflowCell = 0;
        setup.outflowCell = cells - 1;

        // --- 熱(y = mK)と D(y = ppm)---
        CarriedValues heat;
        heat.capacity = WATER_HEAT;
        heat.conductivity = WATER_HEAT * river.dispersion;
        heat.speed = river.speed;
        heat.ground = river.groundPerCubic;
        heat.groundValue = river.groundTemperature * 1000.0;
        heat.inflowValue = river.waterTemperature * 1000.0;
        SetCarried(setup.carried[sim::TIME_STEP_HEAT], heat, cells, dx);
        CarriedValues dissolved;
        dissolved.capacity = WATER_MICROMOL_PPM;
        dissolved.conductivity = WATER_MICROMOL_PPM * river.dispersion;
        dissolved.speed = river.speed;
        dissolved.inflowValue = river.dissolvedPpm;
        SetCarried(setup.carried[sim::TIME_STEP_DISSOLVED], dissolved, cells, dx);

        // --- 発酵 ---
        setup.fuel.assign(cells, sim::TIME_STEP_FUEL_SLOW);
        setup.rateColdQ32 = ToQ(std::log(2.0) / river.fermentHalfLifeSeconds * TICK_SECONDS, Q32);
        setup.heatPerAmount = static_cast<int64_t>(std::llround(river.fermentJoulePerMol * 1e-3));
        setup.referenceAmount = ToQ(river.fermentMolPerCubic * dx * 1e6, 1.0);

        return setup;
    }

    inline void FillRiver(const RiverParameters& river, const sim::TimeStepLabSetup& setup,
                          sim::TimeStepLabState& state) {
        const double dx = river.cellMeters;
        const uint32_t cells = setup.cellCount;
        for (uint32_t cell = 0; cell < cells; ++cell) {
            const double x = (cell + 0.5) * dx;
            const double shape = 0.5 * (1.0 + std::tanh((x - river.lengthMeters * 0.25) / river.frontMeters));
            const double temperature = river.waterTemperature +
                                       (river.startTemperature - river.waterTemperature) * shape;
            const double ppm = river.dissolvedPpm + (river.startPpm - river.dissolvedPpm) * shape;
            state.values[cell] = std::llround(WATER_HEAT * dx * temperature * 1000.0);
            state.values[cells + cell] = std::llround(WATER_MICROMOL_PPM * dx * ppm);
            state.values[2 * cells + cell] = std::llround(river.fermentMolPerCubic * dx * 1e6);
        }
    }

    // --- 火(燃える物の床。燃えた熱は隣へ伝わり〔放射と炎をまとめた伝わりやすさ〕、空気へ逃げる)---
    struct FireParameters {
        double cellMeters = 0.5;
        double lengthMeters = 64.0;
        double heatPerCubic = 2.0e5;     // J/(m³·K)
        double conductivity = 2000.0;    // W/(m·K)
        double lossPerCubic = 200.0;     // 空気へ(W/(m³·K))
        double ambient = 288.0;          // K
        double ignition = 600.0;         // K
        double fuelMolPerCubic = 500.0;  // 燃える物
        double burnSeconds = 60.0;       // 燃える物が 1/e になる時間
        double heatJoulePerMol = 500.0e3;
        double burningMeters = 0.0;  // 燃える物のある長さ(列の頭から。0 = 全部)
        double blockMeters = 4.0;
    };

    inline sim::TimeStepLabSetup MakeFireSetup(const FireParameters& fire) {
        const double dx = fire.cellMeters;
        const uint32_t cells = CellCount(fire.lengthMeters, dx);
        sim::TimeStepLabSetup setup;
        setup.cellCount = cells;
        setup.blockCells = CellCount(fire.blockMeters, dx);

        CarriedValues heat;
        heat.capacity = fire.heatPerCubic;
        heat.conductivity = fire.conductivity;
        heat.ground = fire.lossPerCubic;
        heat.groundValue = fire.ambient * 1000.0;
        SetCarried(setup.carried[sim::TIME_STEP_HEAT], heat, cells, dx);
        CarriedValues nothing;
        nothing.capacity = 1.0;
        SetCarried(setup.carried[sim::TIME_STEP_DISSOLVED], nothing, cells, dx);

        const uint32_t burning = fire.burningMeters == 0.0 ? cells : CellCount(fire.burningMeters, dx);
        setup.fuel.assign(cells, sim::TIME_STEP_FUEL_NONE);
        for (uint32_t cell = 0; cell < burning && cell < cells; ++cell)
            setup.fuel[cell] = sim::TIME_STEP_FUEL_BURNS;

        setup.rateHotQ32 = ToQ(TICK_SECONDS / fire.burnSeconds, Q32);
        setup.ignitionTemperatureQ16 = ToQSigned(fire.ignition * 1000.0, Q16);
        setup.heatPerAmount = static_cast<int64_t>(std::llround(fire.heatJoulePerMol * 1e-3));
        setup.referenceAmount = ToQ(fire.fuelMolPerCubic * dx * 1e6, 1.0);

        return setup;
    }

    inline void FillFire(const FireParameters& fire, const sim::TimeStepLabSetup& setup, sim::TimeStepLabState& state) {
        const double dx = fire.cellMeters;
        const uint32_t cells = setup.cellCount;
        for (uint32_t cell = 0; cell < cells; ++cell) {
            state.values[cell] = std::llround(fire.heatPerCubic * dx * fire.ambient * 1000.0);
            if (setup.fuel[cell] != sim::TIME_STEP_FUEL_NONE)
                state.values[2 * cells + cell] = std::llround(fire.fuelMolPerCubic * dx * 1e6);
        }
    }

    // 火を付ける熱(1 m 分を着火の温度 + 400 K まで。セルの数で割る)
    inline int64_t IgnitionHeatPerCell(const FireParameters& fire) {
        return std::llround(fire.heatPerCubic * fire.cellMeters * (fire.ignition + 400.0 - fire.ambient) * 1000.0);
    }

}  // namespace bicameral::test
