// gpu_time_tree_cost.h — 早送りの待ちの見積もり(D-650・D-661 の EstimateTimeTreeForward)が使う 1 出来事の GPU の費用の式
// (TimeTreeSolveCost)の値を、起動時にその PC で小さい系を解いて測る口(D-662・T-1152)。機種の表は持たない。
// 測る系は中身を持たない箱(セルの数と決まった係数だけ。法則の値によらない費用だけを見る)で、部品の GPU 版(GpuEquilibriumUpload)で解く。
// 測り方: 先に解きを続けて投げて GPU の時計を上げ(早送りは出来事を続けて積むので、その形の値。T-1140・T-1152: 休んだ GPU は
//   約 0.25 秒続けて回すと時計が上がる)、
//   段 1 の小さい箱・大きい箱・段の多い大きい箱 × V 1・V 2 の 6 つの時間(回の最小)から式の値(固定費・段の固定費・V の固定費・
//   段ごとの V の固定費・セルの傾き・セル × V の傾き)を解く。
// データの流れ: 箱の系(CPU)→ 段の形(CPU の SolveEquilibrium の道)→ GPU で解いた時刻印 → 6 つの時間 → TimeTreeSolveCost
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>

#include "sim/equilibrium.h"
#include "sim/gpu_equilibrium_upload.h"
#include "sim/time_step_tree.h"

namespace bicameral::gpu {
    class ImmediateQueue;
    class DebugRing;
}  // namespace bicameral::gpu

namespace bicameral::sim {

    struct TimeTreeCostProbeOptions {
        uint32_t smallSide = 8;      // 小さい箱の辺(セル。8 = 512)
        uint32_t largeSide = 16;     // 大きい箱の辺(16 = 4,096)
        int32_t levelExponent = 10;  // 段の多い箱の面の係数 × 2^e(長い歩の列の系の形。段の数ごとの費用を測る)
        // 測る前に GPU の時計を上げる: 大きい箱の解きを heatBatchSolves 回ずつ、GPU の時間が heatNs に届くまで(0 = 熱さない。
        // 既定 0.4 秒 = D-672。起動が延びるのは構わない)
        uint64_t heatNs = 400000000;
        uint32_t heatBatchSolves = 512;
        uint32_t maxHeatBatches = 64;
        uint32_t rounds = 8;  // 6 つの時間を測る回(最小を採る)
        uint32_t marginPercent = 200;
    };

    // 測った時間(1 回の解き = 段 0 + 段の重み + V)
    struct TimeTreeProbeTimes {
        std::array<uint64_t, 6> solveNs{};  // 小 V 1・小 V 2・大 V 1・大 V 2・段の多い大 V 1・同 V 2
        uint64_t smallCells = 0;
        uint64_t largeCells = 0;
        uint32_t levels = 1;  // 段の多い箱の段の数
    };

    struct TimeTreeCostProbe {
        TimeTreeSolveCost cost;
        TimeTreeProbeTimes times;
        uint64_t gpuNs = 0;   // 6 つの時間を測った GPU の時間(熱する分は除く)
        uint64_t heatNs = 0;  // 熱した GPU の時間
        uint32_t heatBatches = 0;
    };

    // 中身を持たない箱の系(辺 side の立方体・レベル 0・決まった係数。外の面は値 0 を固定した面)
    EquilibriumSystem TimeTreeProbeBox(uint32_t side, int32_t exponent = 0);

    // 6 つの時間から式の値を解く(負になる値は 0。浮動小数点は使わない)
    TimeTreeSolveCost FitTimeTreeSolveCost(const TimeTreeProbeTimes& times, uint32_t marginPercent);

    // 測る(熱する分 + 3 回コマンドリストを投げて待つ。熱する分の既定は GPU で 0.4 秒・測りは数 ms)。queue のタイムスタンプの周波数は timestampFrequency
    std::expected<TimeTreeCostProbe, std::string> ProbeTimeTreeSolveCost(ID3D12Device5* device,
                                                                         gpu::ImmediateQueue& queue,
                                                                         gpu::DebugRing& ring,
                                                                         uint64_t timestampFrequency,
                                                                         const TimeTreeCostProbeOptions& options = {});

}  // namespace bicameral::sim
