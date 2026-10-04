// multires_quiet_scene.h — 静かなブロックを粗くするテスト(tests/multires_quiet_test.cpp・gpu_multires_quiet_test.cpp)の場面(T-0101)。
// 場面: 木の上の活性の場面(multires_activity_scene.h)と同じ根 4×4×4 個。燃え始めの木箱のセルの 6 点(根の角の 2×2×2)と、
// 反対の角の根の静かな空気の 4 点を QUIET_DEPTH 段まで細かくする(空気は 1 刻みに 1 点: 同じ根を取り合わないように)。
// 空気の点 1 は点 0 と同じ QUIET_DEPTH − 1 段のブロックの別の八分の一なので、葉どうしが兄弟になる。外からの要求はこれと
//   - 刻み QUIET_STUFF_TICK から 2 刻み: 無効な要求で一覧を埋める(静かな葉の要求が入らず次へ回る。回った兄弟は同じ刻みに静かになる)
//   - 刻み QUIET_REFINE_AGAIN_TICK: 空気の点 0 をもう一度細かくする(空いた枠を使い直す)
// だけで、粗くするのは全部「静かな葉を粗くする要求」(SubmitQuietCoarsenRequests / RecordQuietRequests)。
// 空気の鎖は作ってすぐ静かなので N + 1 刻みごとに 1 段ずつ畳まれ、木箱の鎖は燃えている間は残り、燃え尽きると畳まれる。
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "multires_activity_scene.h"

namespace bicameral::test {

    constexpr int32_t QUIET_DEPTH = 6;
    constexpr uint32_t QUIET_CRATE_POINTS = 6;
    constexpr uint32_t QUIET_AIR_POINTS = 4;
    constexpr uint64_t QUIET_STUFF_TICK = multires::MR_QUIET_TICKS + 1;  // 空気の点 0 の葉が初めて静かになる刻み
    constexpr uint64_t QUIET_STUFF_TICKS = 2;
    constexpr uint64_t QUIET_REFINE_AGAIN_TICK = 150;
    constexpr int32_t QUIET_REFINE_AGAIN_DEPTH = 4;
    constexpr uint64_t QUIET_TICKS = 300;

    // 反対の角の根(原点 24, 24, 24)の空気のセルの中の点を、レベル level まで細かくする要求。点 0 と 1 は最も細かい
    // QUIET_DEPTH のレベルで隣どうし(同じ QUIET_DEPTH − 1 段のブロックの別の八分の一)、点 2・3 は根の別の八分の一
    inline multires::MrRequest MakeAirRequest(uint32_t point, int32_t level) {
        constexpr std::array<std::array<int64_t, 3>, QUIET_AIR_POINTS> FINEST = {
            {{(25 << QUIET_DEPTH) + 3, (25 << QUIET_DEPTH) + 3, (25 << QUIET_DEPTH) + 3},
             {(25 << QUIET_DEPTH) + 8 + 3, (25 << QUIET_DEPTH) + 3, (25 << QUIET_DEPTH) + 3},
             {(29 << QUIET_DEPTH) + 5, (25 << QUIET_DEPTH) + 5, (25 << QUIET_DEPTH) + 5},
             {(29 << QUIET_DEPTH) + 7, (29 << QUIET_DEPTH) + 7, (29 << QUIET_DEPTH) + 7}}};
        const auto shift = static_cast<uint32_t>(QUIET_DEPTH - level);

        return multires::MrMakeRequest(multires::MR_REQUEST_REFINE, level, FINEST[point][0] >> shift,
                                       FINEST[point][1] >> shift, FINEST[point][2] >> shift);
    }

    // この刻みの外からの要求
    inline std::vector<multires::MrRequest> QuietRequestsAt(uint64_t tick) {
        std::vector<multires::MrRequest> requests;
        if (tick == 0) {
            for (uint32_t spot = 0; spot < QUIET_CRATE_POINTS; ++spot)
                requests.push_back(MakeHotspotRequest(spot, QUIET_DEPTH));
        }

        if (tick < QUIET_AIR_POINTS)
            requests.push_back(MakeAirRequest(static_cast<uint32_t>(tick), QUIET_DEPTH));

        // 根より粗いレベルは無効(数えるだけ)
        if (tick >= QUIET_STUFF_TICK && tick < QUIET_STUFF_TICK + QUIET_STUFF_TICKS)
            requests.assign(multires::MR_MAX_REQUESTS,
                            multires::MrMakeRequest(multires::MR_REQUEST_REFINE, -1, 0, 0, 0));

        if (tick == QUIET_REFINE_AGAIN_TICK)
            requests.push_back(MakeAirRequest(0, QUIET_REFINE_AGAIN_DEPTH));

        return requests;
    }

    // 1 刻みの前半(CPU): 外からの要求 → 静かな葉を粗くする要求 → 要求の処理。GPU も同じ順
    inline void BeginQuietTick(sim::MultiresNest& nest, uint64_t tick, std::span<const multires::MrRequest> requests) {
        sim::SubmitRequests(nest, requests);
        sim::SubmitQuietCoarsenRequests(nest, tick);
        sim::ProcessRequests(nest);
    }

    // 本物のブロックの数
    inline uint32_t CountRealBlocks(const sim::MultiresNest& nest) {
        uint32_t count = 0;
        for (uint32_t slot = 0; slot < nest.capacity.worldBlocks; ++slot)
            count += nest.blocks[slot].kind == multires::MR_BLOCK_REAL ? 1 : 0;

        return count;
    }

}  // namespace bicameral::test
