// multires_implicit_scene.h — 細かいレベルの熱の陰解法の試作(T-0110・T-0117)のテストの場面。CPU のテスト(multires_implicit_test.cpp)と
// GPU のテスト(gpu_multires_implicit_test.cpp)が同じ場面を使う。
//   - 物差しの山: x の向きの余弦の温度の山(長さ length セル、y・z は STICK_CROSS セル)
//   - 違うレベルの面を含む場面: レベル 基準 の 4 セルの列(x = 0〜3)の x = 1 をレベル 基準 + 3 の 8³ セルに細かくし、そのうち (8, 3, 3) を
//     さらにレベル 基準 + 6 の 8³ セルに細かくする。最も細かい所の −x の面はレベル 基準 のセル (0, 0, 0) に面する(差 6)。最も細かい所だけ 1500 K、ほかは 300 K
#pragma once

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

#include "common/multires_conduction.hlsli"
#include "multires_conduction_scene.h"
#include "sim/implicit_conduction.h"
#include "sim/reaction_table.h"

namespace bicameral::test {

    struct Material {
        uint64_t heatCapacity = 0;
        uint32_t conductance = 0;
    };

    inline Material AirAt(const sim::BakedReactionTable& table, int32_t millikelvin) {
        const multires::MrThermal thermal = multires::MrCellThermal(table.View(),
                                                                    test::MakeConductionAir(table, millikelvin));

        return {.heatCapacity = 8 * thermal.capacityLimit, .conductance = thermal.conductance};
    }

    inline sim::ImplicitCell MakeCell(const Material& material, int32_t level, int64_t x, int64_t y, int64_t z,
                                      int64_t millikelvin) {
        return {.level = level,
                .x = x,
                .y = y,
                .z = z,
                .heatCapacity = material.heatCapacity,
                .conductance = material.conductance,
                .energy = sim::ImplicitEnergyFor(material.heatCapacity, millikelvin)};
    }

    // --- 物差しの山 ---

    constexpr int64_t STICK_MEAN = 400000;      // mK
    constexpr int64_t STICK_AMPLITUDE = 40000;  // mK
    constexpr uint32_t STICK_CROSS = 2;         // y・z のセルの数(3 次元の面と縮約を通す)

    inline double WaveWeight(int64_t x, uint32_t length) {
        return std::cos(std::numbers::pi * (static_cast<double>(x) + 0.5) / static_cast<double>(length));
    }

    inline std::vector<sim::ImplicitCell> MakeWaveCells(const Material& air, int32_t level, uint32_t length) {
        std::vector<sim::ImplicitCell> cells;
        for (uint32_t row = 0; row < STICK_CROSS * STICK_CROSS; ++row) {
            for (uint32_t x = 0; x < length; ++x) {
                const double wave = static_cast<double>(STICK_AMPLITUDE) * WaveWeight(x, length);
                const int64_t temperature = STICK_MEAN + std::llround(wave);
                cells.push_back(MakeCell(air, level, x, row % STICK_CROSS, row / STICK_CROSS, temperature));
            }
        }

        return cells;
    }

    // --- 違うレベルの面を含む場面 ---

    constexpr int64_t COMPOSITE_COLD = 300000;
    constexpr int64_t COMPOSITE_HOT = 1500000;
    constexpr int32_t COMPOSITE_STEP = 3;  // 細かくする 1 回のレベルの差
    constexpr int64_t COMPOSITE_EDGE = 8;

    inline std::vector<sim::ImplicitCell> MakeCompositeCells(const Material& air, int32_t base) {
        std::vector<sim::ImplicitCell> cells;
        for (int64_t x : {0, 2, 3})
            cells.push_back(MakeCell(air, base, x, 0, 0, COMPOSITE_COLD));

        const int32_t middle = base + COMPOSITE_STEP;
        const int32_t finest = middle + COMPOSITE_STEP;
        // 8³ の 2 組: レベル middle の (8〜15, 0〜7, 0〜7)(細かくした (8, 3, 3) を除く)と、そこを埋めるレベル finest の 8³
        constexpr int64_t COUNT = COMPOSITE_EDGE * COMPOSITE_EDGE * COMPOSITE_EDGE;
        for (int64_t n = 0; n < COUNT; ++n) {
            const int64_t x = n % COMPOSITE_EDGE;
            const int64_t y = (n / COMPOSITE_EDGE) % COMPOSITE_EDGE;
            const int64_t z = n / (COMPOSITE_EDGE * COMPOSITE_EDGE);
            if (x != 0 || y != 3 || z != 3)
                cells.push_back(MakeCell(air, middle, COMPOSITE_EDGE + x, y, z, COMPOSITE_COLD));

            const int64_t fineOrigin = 3 * COMPOSITE_EDGE;
            cells.push_back(MakeCell(air, finest, (COMPOSITE_EDGE * COMPOSITE_EDGE) + x, fineOrigin + y, fineOrigin + z,
                                     COMPOSITE_HOT));
        }

        return cells;
    }

}  // namespace bicameral::test
