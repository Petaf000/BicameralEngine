// multires_grading.cpp — 細かい所の半径と移り目の規則と、地表の 4 分木の見込みの数え(T-0954。形と理由は multires_grading.h)。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#include "sim/multires_grading.h"

#include <algorithm>
#include <cassert>

namespace bicameral::sim {

    namespace {

        // 遠くの葉の頭打ちの段
        uint32_t FarCoarseness(const FineRegion& region, uint64_t finestCellMm) {
            uint32_t coarseness = 0;
            while (coarseness < GRADED_MAX_COARSENESS && (finestCellMm << (coarseness + 1)) <= region.farLeafMm)
                ++coarseness;

            return coarseness;
        }

        // 1 次元の点と区間 [low, low + size) の距離
        uint64_t AxisDistance(uint64_t point, uint64_t low, uint64_t size) {
            if (point < low)
                return low - point;

            return point >= low + size ? point - (low + size) : 0;
        }

        struct LeafWalk {
            const FineRegion& region;
            uint64_t finestCellMm = 0;
            std::array<uint64_t, 2> seedMm = {};
            uint64_t countSideMm = 0;
            GradedLeafCount count;
        };

        void AddLeaf(LeafWalk& walk, uint64_t x, uint64_t y, uint32_t coarseness) {
            ++walk.count.leaves;
            ++walk.count.byCoarseness[coarseness];

            // FNV-1a(位置と段)
            for (const uint64_t value : {x, y, (uint64_t)coarseness}) {
                walk.count.hash ^= value;
                walk.count.hash *= 1099511628211ull;
            }
        }

        void Walk(LeafWalk& walk, uint64_t x, uint64_t y, uint32_t coarseness) {
            if (x >= walk.countSideMm || y >= walk.countSideMm)
                return;

            const uint64_t size = walk.finestCellMm << coarseness;
            const uint64_t distance = std::max(AxisDistance(walk.seedMm[0], x, size),
                                               AxisDistance(walk.seedMm[1], y, size));
            if (coarseness == 0 || coarseness <= GradedCoarseness(walk.region, walk.finestCellMm, distance)) {
                AddLeaf(walk, x, y, coarseness);
                return;
            }

            const uint64_t childSize = size >> 1;
            Walk(walk, x, y, coarseness - 1);
            Walk(walk, x + childSize, y, coarseness - 1);
            Walk(walk, x, y + childSize, coarseness - 1);
            Walk(walk, x + childSize, y + childSize, coarseness - 1);
        }

    }  // namespace

    uint32_t GradedCoarseness(const FineRegion& region, uint64_t finestCellMm, uint64_t distanceMm) {
        assert(finestCellMm != 0 && region.levelsPerDoubling != 0);
        if (distanceMm <= region.radiusMm)
            return 0;

        // --- 半径の外: 距離が半径 × 2^n 以下になる最小の n(半径 0 はいちばん細かいセルの大きさから数える)---
        const uint64_t reach = std::max(region.radiusMm, finestCellMm);
        uint32_t doublings = 1;
        while (doublings < 63 && distanceMm > (reach << doublings) && (reach << doublings) > reach)
            ++doublings;

        const uint64_t coarseness = (uint64_t)doublings * region.levelsPerDoubling;

        return (uint32_t)std::min<uint64_t>(coarseness, FarCoarseness(region, finestCellMm));
    }

    GradedLeafCount CountGradedSurfaceLeaves(const FineRegion& region, uint64_t finestCellMm, uint32_t rootCoarseness,
                                             std::array<uint64_t, 2> seedMm, uint64_t countSideMm) {
        assert(rootCoarseness <= GRADED_MAX_COARSENESS);
        LeafWalk walk{.region = region, .finestCellMm = finestCellMm, .seedMm = seedMm, .countSideMm = countSideMm};
        walk.count.hash = 14695981039346656037ull;
        Walk(walk, 0, 0, rootCoarseness);

        return walk.count;
    }

}  // namespace bicameral::sim
