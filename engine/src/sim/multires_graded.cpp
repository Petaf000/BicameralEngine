// multires_graded.cpp — 木の細かくする・粗くする要求が細かい所の半径と移り目の規則を読む CPU(T-1008。形と理由は multires_graded.h)。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#include "sim/multires_graded.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>
#include <vector>

#include "common/multires_tree.hlsli"
#include "sim/multires_grading.h"

namespace bicameral::sim {

    using namespace multires;

    namespace {

        // いちばん細かいレベルのセルの単位の箱(下の角と一辺)
        struct FineBox {
            std::array<int64_t, 3> low = {};
            int64_t size = 1;
        };

        uint64_t FinestCellMm(int32_t finestLevel) {
            assert(finestLevel <= 2 && finestLevel >= -20);

            return finestLevel >= 0 ? 500u >> (uint32_t)finestLevel : 500u << (uint32_t)(-finestLevel);
        }

        // レベル level・下の角 origin(そのレベルのセルの単位)・一辺 cells の箱を、いちばん細かいレベルの単位へ
        FineBox ToFine(std::array<int64_t, 3> origin, int32_t level, int64_t cells, int32_t finestLevel) {
            FineBox box;
            if (level <= finestLevel) {
                const auto shift = (uint32_t)(finestLevel - level);
                for (uint32_t axis = 0; axis < 3; ++axis)
                    box.low[axis] = origin[axis] << shift;

                box.size = cells << shift;

                return box;
            }

            const auto shift = (uint32_t)(level - finestLevel);
            for (uint32_t axis = 0; axis < 3; ++axis)
                box.low[axis] = origin[axis] >> shift;  // 負も下へ(算術シフト)

            box.size = std::max<int64_t>(1, cells >> shift);

            return box;
        }

        // 2 つの箱の Chebyshev の隙間(いちばん細かいセルの数。重なれば 0)
        int64_t Gap(const FineBox& a, const FineBox& b) {
            int64_t gap = 0;
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const int64_t below = b.low[axis] - (a.low[axis] + a.size);
                const int64_t above = a.low[axis] - (b.low[axis] + b.size);
                gap = std::max({gap, below, above});
            }

            return gap;
        }

        std::vector<FineBox> SeedBoxes(const MultiresNest& nest, std::span<const uint32_t> seedSlots,
                                       int32_t finestLevel) {
            std::vector<FineBox> boxes;
            for (const uint32_t slot : seedSlots) {
                const MrBlock& block = nest.blocks[slot];
                if (block.kind != MR_BLOCK_REAL)
                    continue;

                boxes.push_back(
                    ToFine({block.originX, block.originY, block.originZ}, block.level, MR_BLOCK_EDGE, finestLevel));
            }

            return boxes;
        }

        // 箱の所の規則のレベル(いちばん近い種からの距離で)
        int32_t GradedLevel(const FineRegion& region, int32_t finestLevel, std::span<const FineBox> seeds,
                            const FineBox& box) {
            int64_t gap = std::numeric_limits<int64_t>::max();
            for (const FineBox& seed : seeds)
                gap = std::min(gap, Gap(box, seed));

            const uint64_t cellMm = FinestCellMm(finestLevel);
            const uint64_t limit = std::numeric_limits<uint64_t>::max() / cellMm;
            const uint64_t distanceMm = (uint64_t)gap >= limit ? std::numeric_limits<uint64_t>::max()
                                                               : (uint64_t)gap * cellMm;

            return finestLevel - (int32_t)GradedCoarseness(region, cellMm, distanceMm);
        }

        // 八分の一 octant の、レベル level + 1 の子の原点(子のレベルのセルの単位)
        std::array<int64_t, 3> ChildOrigin(const MrBlock& block, uint32_t octant) {
            return {MrChildOrigin(block.originX, octant & 1u), MrChildOrigin(block.originY, (octant >> 1) & 1u),
                    MrChildOrigin(block.originZ, (octant >> 2) & 1u)};
        }

    }  // namespace

    GradedRequestReport SubmitGradedRefineRequests(MultiresNest& nest, const FineRegion& region, int32_t finestLevel,
                                                   std::span<const uint32_t> seedSlots) {
        GradedRequestReport report;
        const std::vector<FineBox> seeds = SeedBoxes(nest, seedSlots, finestLevel);
        if (seeds.empty())
            return report;

        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot) {
            const MrBlock& block = nest.blocks[slot];
            if (block.kind != MR_BLOCK_REAL || block.level >= finestLevel)
                continue;

            for (uint32_t octant = 0; octant < 8; ++octant) {
                if (block.children[octant] != MR_NO_BLOCK)
                    continue;

                // --- 子の箱(= 八分の一)の所の規則より粗ければ 1 段細かく ---
                const std::array<int64_t, 3> origin = ChildOrigin(block, octant);
                const FineBox box = ToFine(origin, block.level + 1, MR_BLOCK_EDGE, finestLevel);
                if (block.level >= GradedLevel(region, finestLevel, seeds, box))
                    continue;

                uint32_t& count = nest.counters[MR_COUNTER_REQUESTS];
                if (count >= MR_MAX_REQUESTS) {
                    ++report.deferred;
                    continue;
                }

                nest.requests[count++] = MrMakeRequest(MR_REQUEST_REFINE, block.level + 1, origin[0], origin[1],
                                                       origin[2]);
                ++report.refines;
                break;  // 1 つのブロックの要求は 1 刻みに 1 つしか通らない(取り合い。残りの八分の一は次の刻み)
            }
        }

        return report;
    }

    uint32_t FilterGradedCoarsenRequests(MultiresNest& nest, const FineRegion& region, int32_t finestLevel,
                                         std::span<const uint32_t> seedSlots) {
        const std::vector<FineBox> seeds = SeedBoxes(nest, seedSlots, finestLevel);
        if (seeds.empty())
            return 0;

        uint32_t& count = nest.counters[MR_COUNTER_REQUESTS];
        uint32_t kept = 0;
        uint32_t removed = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const MrRequest request = nest.requests[i];
            if (request.op == MR_REQUEST_COARSEN) {
                // 点を含むレベル L のブロック(原点 = 点を 8 の倍数へ)を粗くすると、その所は L − 1 になる
                const int64_t mask = ~(int64_t)(MR_BLOCK_EDGE - 1);
                const FineBox box = ToFine({request.x & mask, request.y & mask, request.z & mask}, request.level,
                                           MR_BLOCK_EDGE, finestLevel);
                if (request.level - 1 < GradedLevel(region, finestLevel, seeds, box)) {
                    ++removed;
                    continue;
                }
            }

            nest.requests[kept++] = request;
        }

        count = kept;

        return removed;
    }

}  // namespace bicameral::sim
