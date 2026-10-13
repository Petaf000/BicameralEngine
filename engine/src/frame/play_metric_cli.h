// play_metric_cli.h — コマンドラインの「遊びの指標を測る」(bicameral --measure-metrics。T-0432・ADR-0206。D-482 ④)。
//
// 窓を開かずに、--packages・--mix・--world の表(フレームのループと同じ読み方)の遊びの指標(パッケージの分類 play_metrics)を
// 写しの早送りで全部測り、前の版との差をログに出して終える。前の版の値はファイル(--metrics-baseline)から読み、
// --metrics-save なら測った値で書き直す(表を変える前に 1 回 --metrics-save、変えた後に差を見る)。
// 指標の形と値のファイルは sim/play_metric.h、測り方は sim/play_metric_measure.h。
#pragma once

#include "core/aliases.h"
#include "frame/frame_loop.h"

namespace bicameral::frame {

    struct PlayMetricCommandOptions {
        fs::path baselinePath;      // 前の版の値のファイル(空 = 比べない。無いファイルは「前の版なし」)
        bool saveBaseline = false;  // 測った値で baselinePath を書き直す
    };

    // 終了コード: 0 = 全部測れた・1 = 測れない指標があった / 表・GPU・ファイルの誤り
    [[nodiscard]] int RunPlayMetricCommand(const FrameLoopOptions& options, const PlayMetricCommandOptions& command);

}  // namespace bicameral::frame
