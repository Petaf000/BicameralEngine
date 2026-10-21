// support_scenes.h — 支えの法則(sim/support_law)の試験の場面(石の張り出し・梁・アーチ・洞窟)。support_law_test(CPU。T-0871・T-0919)と
// gpu_equilibrium_block_test(成分を持つ節の GPU 版と CPU のビット一致。T-0970)が同じ場面を使う。縦の面・奥行 1 セル。
#pragma once

#include <cstdint>

#include "sim/support_law.h"

namespace bicameral::test {

    // 石: 硬さ 50 GPa・密度 2600 kg/m³
    inline constexpr uint64_t STONE_KPA = 50000000;
    inline constexpr uint64_t STONE_DENSITY = 2600;

    // 梁(width × height のセル。左の端を壁に固定。bothEnds なら右の端も)
    inline sim::SupportScene Beam(uint32_t width, uint32_t height, uint64_t cellMm, bool bothEnds) {
        sim::SupportScene scene;
        scene.width = width;
        scene.height = height;
        scene.cellMm = cellMm;
        scene.stiffnessKpa = STONE_KPA;
        scene.densityKgPerM3 = STONE_DENSITY;
        scene.solid.assign((size_t)width * height, 1);
        scene.clamps.assign((size_t)width * height, 0);
        for (uint32_t up = 0; up < height; ++up) {
            scene.clamps[(size_t)width * up] |= sim::SUPPORT_CLAMP_LEFT;
            if (bothEnds)
                scene.clamps[width - 1 + (size_t)width * up] |= sim::SUPPORT_CLAMP_RIGHT;
        }

        return scene;
    }

    // アーチ: 内径 4 m・厚さ 1 m の半円。セルの中心が内径〜外径にあるセル。下の段を地面に固定
    inline sim::SupportScene Arch(uint64_t cellMm) {
        const auto inner = (int64_t)(4000 / cellMm);
        const auto outer = (int64_t)(5000 / cellMm);
        sim::SupportScene scene = Beam((uint32_t)(2 * outer), (uint32_t)outer, cellMm, false);
        for (int64_t up = 0; up < outer; ++up) {
            for (int64_t x = 0; x < 2 * outer; ++x) {
                // 中心の 2 倍の座標(セルの大きさ ÷ 2 の単位)
                const int64_t cx = 2 * x - 2 * outer + 1;
                const int64_t cy = 2 * up + 1;
                const int64_t radius = cx * cx + cy * cy;
                const bool inside = radius >= 4 * inner * inner && radius <= 4 * outer * outer;
                const size_t i = (size_t)x + (size_t)(2 * outer) * (size_t)up;
                scene.solid[i] = inside ? 1 : 0;
                scene.clamps[i] = inside && up == 0 ? sim::SUPPORT_CLAMP_BOTTOM : 0;
            }
        }

        return scene;
    }

    // 洞窟: 幅 24 × 高さ 10 m の岩・x 8〜16 m・高さ 4〜8 m の空洞(天井の厚さ 2 m)。下を地面に固定
    inline sim::SupportScene Cave(uint64_t cellMm) {
        const auto width = (uint32_t)(24000 / cellMm);
        const auto height = (uint32_t)(10000 / cellMm);
        sim::SupportScene scene = Beam(width, height, cellMm, false);
        for (uint32_t up = 0; up < height; ++up) {
            for (uint32_t x = 0; x < width; ++x) {
                const uint64_t cx = (2 * x + 1) * cellMm;
                const uint64_t cy = (2 * up + 1) * cellMm;
                const bool hollow = cx > 16000 && cx < 32000 && cy > 8000 && cy < 16000;
                scene.solid[x + (size_t)width * up] = hollow ? 0 : 1;
                scene.clamps[x + (size_t)width * up] = !hollow && up == 0 ? sim::SUPPORT_CLAMP_BOTTOM : 0;
            }
        }

        return scene;
    }

}  // namespace bicameral::test
