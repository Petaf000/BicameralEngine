// multires_grading.h — 細かい所の半径と移り目(どこを細かいセルにするか)を世界のデータの物理の長さ(mm)で選ぶ規則(T-0954・ADR-0431・20 §11.1)。
// 世界のデータ WorldPhysics::fineRegion(環境の fine_region)を読み、局所の状態で細かくした所(種)からの距離だけで粗さを決める(視点・予算では決めない。D-544):
//   距離 ≤ 半径 → いちばん細かいセル。その外は距離が 2 倍になるごとに levelsPerDoubling 段ずつ粗く、遠くの葉の大きさで頭打ち。
// 半径・遠くの葉は物理の長さなので、いちばん細かいセルの大きさを変えても、細かい所の広さと遠くの葉の大きさは変わらない(部品の閉じる長さ closeLengthMm と同じ考え)。
// 見込みの数え(CountGradedSurfaceLeaves)は地表の 4 分木の葉を数えるだけ(VRAM の線の見積もり・試験用)。木を実際に細かく/粗くする所へ
// この規則をつなぐのは T-1008。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <array>
#include <cstdint>

#include "sim/world_physics.h"

namespace bicameral::sim {

    // 種から distanceMm の所の粗さ(いちばん細かいセル finestCellMm から何段粗くするか。0 = いちばん細かい)。
    // 遠くの葉の頭打ち = finestCellMm × 2^段 ≤ farLeafMm の最大の段(遠くの葉がいちばん細かいセルより小さければ 0)
    [[nodiscard]] uint32_t GradedCoarseness(const FineRegion& region, uint64_t finestCellMm, uint64_t distanceMm);

    // 地表の 4 分木の見込み(T-0954 の試験・docs/perf.md)
    inline constexpr uint32_t GRADED_MAX_COARSENESS = 24;

    struct GradedLeafCount {
        uint64_t leaves = 0;
        std::array<uint64_t, GRADED_MAX_COARSENESS + 1> byCoarseness = {};  // 段ごとの葉の数
        uint64_t hash = 0;  // 葉の並び(位置と段)のハッシュ(同じ値なら同じ木)
    };

    // 一辺 finestCellMm × 2^rootCoarseness の正方形の根を、種の点 seedMm からの距離(Chebyshev)の規則で 4 分し、
    // 原点から一辺 countSideMm の正方形に掛かる葉を数える。呼ぶ側の約束: rootCoarseness ≤ GRADED_MAX_COARSENESS
    [[nodiscard]] GradedLeafCount CountGradedSurfaceLeaves(const FineRegion& region, uint64_t finestCellMm,
                                                           uint32_t rootCoarseness, std::array<uint64_t, 2> seedMm,
                                                           uint64_t countSideMm);

}  // namespace bicameral::sim
