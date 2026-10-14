// slope_gauge_cli.h — コマンドラインの「坂の計器」(bicameral --slope-gauge <刻み>。開発ビルドの計器。T-0518・ADR-0230。D-487 ②)。
//
// 窓を開かずに、--packages・--mix・--world の表(フレームのループと同じ読み方)の遊びの指標(play_metrics)の場面を
// CPU リファレンス(GPU とビット一致)で流し、刻みごとに反応が坂を上った量を数えてログに出す(sim/slope_gauge)。
// 場面は指標ごとに、指標が止まる刻みか <刻み> の早い方まで。坂を上ったら警告(表はそのまま。D-487 ②)。
// 坂を上った刻みは --log-level debug で 1 行ずつ出る。CPU で全部のセルを刻むので遅い(release で 1 刻み約 0.1 秒。未確認)。
#pragma once

#include <cstdint>

#include "frame/frame_loop.h"

namespace bicameral::frame {

    // 終了コード: 0 = 数えられた(坂を上っていても 0。警告はログ)・1 = 表を読めない / なぞりが世界と食い違った(計器の不具合)
    [[nodiscard]] int RunSlopeGaugeCommand(const FrameLoopOptions& options, uint64_t maxTicks);

}  // namespace bicameral::frame
