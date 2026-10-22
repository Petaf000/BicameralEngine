// multires_graded.h — 多重解像度の木の細かくする・粗くする要求が、種からの距離で細かい所の半径と移り目の規則(sim/multires_grading の
// GradedCoarseness)を読む CPU(T-1008・ADR-0436・20 §11.1)。
// 種 = 局所の状態で細かくした所(活性の種の枠。SeedSlots。視点・カメラからではない。D-544・D-623)。距離は種のブロックの箱からの Chebyshev(mm)。
//   - 細かくする: 本物のブロックの子の無い八分の一で、その所の規則のレベルより粗いものを 1 段細かくする要求を足す。1 つのブロックの要求は
//     1 刻みに 1 つしか通らない(取り合い)ので、ブロックごとに八分の一の順の最初の 1 つ。数十刻みで規則の形に着く。
//   - 粗くする: 一覧の粗くする要求(静かな葉。SubmitQuietCoarsenRequests)のうち、粗くすると規則のレベルより粗くなるものを外す
//     (種の近くで静かな葉を粗くしては細かくし直す行き来を止める)。
// 呼ぶ側が選んで呼ぶ(呼ばない木は今と同じ)。順: 種が決まった後・ProcessRequests の前に、SubmitQuietCoarsenRequests → FilterGradedCoarsenRequests
// → SubmitGradedRefineRequests。種が無ければどちらも何もしない(種の無い静かな世界の木は呼ばない時と同じ)。
// GPU は T-1028。浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <cstdint>
#include <span>

#include "sim/multires_nest.h"
#include "sim/world_physics.h"

namespace bicameral::sim {

    struct GradedRequestReport {
        uint32_t refines = 0;   // 足した細かくする要求
        uint32_t deferred = 0;  // 一覧が一杯で次の刻みに回した八分の一
    };

    // いちばん細かいレベル finestLevel(規則の 0 段。0 = 0.5 m。2 まで)からの規則で、種 seedSlots(本物のブロックの枠)からの距離ごとに
    // 規則より粗い八分の一を 1 段細かくする要求を、一覧の後ろに世界の枠の順・八分の一の順で足す
    GradedRequestReport SubmitGradedRefineRequests(MultiresNest& nest, const FineRegion& region, int32_t finestLevel,
                                                   std::span<const uint32_t> seedSlots);

    // 一覧の粗くする要求のうち、粗くすると規則より粗くなる(ブロックのレベル − 1 < 規則のレベル)ものを外す(残りの順は変えない)。外した数
    uint32_t FilterGradedCoarsenRequests(MultiresNest& nest, const FineRegion& region, int32_t finestLevel,
                                         std::span<const uint32_t> seedSlots);

}  // namespace bicameral::sim
