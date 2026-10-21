// surface_detail_scene.h — 表面の細かい形の試験の場面(R-T1・T-0879)。CPU の試験(surface_detail_test)と GPU の試験(gpu_surface_detail_test)が同じ場面を作る。
//
// 場面「溝の多い所」(23 §10 R-T1 の合格の 2 つ目): 16 m 四方(32 × 32 の柱)・地面 1.0 m・肌理(振れ幅 3 cm・波 52 cm〜3.3 cm の 5 段・割れ目 4 cm)、
//   掘った溝 24 本(10 cm の深さ・幅 20 cm・長さ 4 m)と足跡 200 個(3 cm の深さ・幅 12 cm・長さ 15 cm)。位置は決まった乱数(04 R6)。
// 場面「物 100 個」(合格の 1 つ目): 物 100 個がそれぞれ底の 4 × 4 点で地面に触れている(点の高さ = 地面の 1.0 m)。
#pragma once

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "sim/surface_detail.h"

namespace bicameral::test {

    inline constexpr uint32_t BUSY_COLUMNS = 32;
    inline constexpr uint32_t BUSY_GROUND_UM = 1000000;
    inline constexpr uint32_t BUSY_GROOVES = 24;
    inline constexpr uint32_t BUSY_FOOTPRINTS = 200;
    inline constexpr uint32_t BODY_COUNT = 100;
    inline constexpr uint32_t BODY_POINT_SIDE = 4;
    inline constexpr int64_t OPENNESS_BOTTOM_UM = 500000;  // 開き具合を測る柱の下のセルの床(地面の 1 つ下のセル)

    inline surface::SdConfig BusyConfig() {
        surface::SdConfig config{};
        config.relief = {.amplitudeUm = 30000,
                         .waveShift = 19,
                         .octaveCount = 5,
                         .crackDepthUm = 40000,
                         .crackWidthQ16 = 2000,
                         .crackWaveShift = 20};
        config.seed = 0x5EED0879u;
        config.originXUm = (uint64_t)37 << 20;  // 区画の角は最も大きい波(2^20 µm)にそろえる(描画の小数の版の約束)
        config.originZUm = (uint64_t)11 << 20;
        config.columnsX = BUSY_COLUMNS;
        config.columnsZ = BUSY_COLUMNS;
        config.baseSmooth = 0;

        return config;
    }

    // [0, range) の決まった乱数
    inline uint32_t SceneRandom(uint64_t id, uint32_t purpose, uint32_t range) {
        return fx::FxRandomBelow(fx::FxHash64(0x0879, 0, id, purpose), range);
    }

    // 場面の形は必ず彫れる大きさで書く(断られたら場面の作り違い)
    inline void EnsureCarved(sim::SurfaceDetailScene& scene, const surface::SdCarve& carve) {
        if (!sim::CarveSurface(scene, carve))
            std::abort();
    }

    inline sim::SurfaceDetailScene MakeBusyScene() {
        sim::SurfaceDetailScene scene = sim::MakeSurfaceDetailScene(BusyConfig(), BUSY_GROUND_UM);
        const uint32_t size = BUSY_COLUMNS * surface::SD_CELL_UM;
        const uint32_t margin = 600000;

        // --- 溝(x・z に 4 m か、斜めに約 4 m)---
        for (uint32_t i = 0; i < BUSY_GROOVES; ++i) {
            const uint32_t ax = margin + SceneRandom(i, 1, size - (2 * margin) - 4000000);
            const uint32_t az = margin + SceneRandom(i, 2, size - (2 * margin) - 4000000);
            const uint32_t direction = SceneRandom(i, 3, 3);
            const uint32_t bx = ax + (direction == 0 ? 4000000u : (direction == 1 ? 0u : 2800000u));
            const uint32_t bz = az + (direction == 0 ? 0u : (direction == 1 ? 4000000u : 2800000u));
            EnsureCarved(scene, {.ax = ax, .az = az, .bx = bx, .bz = bz, .radiusUm = 100000, .depthUm = 100000});
        }

        // --- 足跡 ---
        for (uint32_t i = 0; i < BUSY_FOOTPRINTS; ++i) {
            const uint32_t ax = margin + SceneRandom(i, 4, size - (2 * margin));
            const uint32_t az = margin + SceneRandom(i, 5, size - (2 * margin));
            EnsureCarved(scene, {.ax = ax, .az = az, .bx = ax + 150000, .bz = az, .radiusUm = 60000, .depthUm = 30000});
        }

        return scene;
    }

    // 物 100 個 × 底の 4 × 4 点(0.4 m 四方)
    inline std::vector<surface::SdProbePoint> MakeBodyPoints() {
        std::vector<surface::SdProbePoint> points;
        const uint32_t size = BUSY_COLUMNS * surface::SD_CELL_UM;
        for (uint32_t body = 0; body < BODY_COUNT; ++body) {
            const uint32_t x0 = 100000 + SceneRandom(body, 6, size - 600000);
            const uint32_t z0 = 100000 + SceneRandom(body, 7, size - 600000);
            for (uint32_t j = 0; j < BODY_POINT_SIDE; ++j) {
                for (uint32_t i = 0; i < BODY_POINT_SIDE; ++i)
                    points.push_back({.xUm = x0 + (i * 133333), .zUm = z0 + (j * 133333), .yUm = BUSY_GROUND_UM});
            }
        }

        return points;
    }

}  // namespace bicameral::test
