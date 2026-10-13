// play_metrics_package.h — パッケージの分類 play_metrics(遊びの指標)を名前のまま読む(T-0432・ADR-0206。D-482 ④)。
//
// データの流れ: パッケージのフォルダ → LoadPackages(合わせた「分類 → 鍵 → 値」)→ ReadPlayMetrics → sim::PlayMetricDefinition
//   (ReactionTableDefinition::playMetrics)→ sim::BakePlayMetrics(物質の名前を ID に)→ sim/play_metric_measure が測る。
// 書き方(1 つの鍵 = 1 つの指標。Mod は鍵を足して増やす。数は整数だけ):
//   play_metrics = {
//       sealed_fire_out = {
//           label = "密室の火が消えるまで",       -- 省けば鍵
//           max_ticks = 36000,                   -- 早送りの上限(刻み。sample_interval_ticks の倍数)
//           sample_interval_ticks = 60,          -- 量を測る間隔(刻み。答えの細かさ)
//           pokes = { { tick = 0, cell = { 28, 32, 32 } } },  -- 場面に当てるつつき(セルを温める。省ける)
//           quantity = "cells_at_or_above",      -- 測る量: max_temperature(mK)・cells_at_or_above(セル)・species_amount(µmol)
//           temperature_mk = 1000000,            -- cells_at_or_above のしきい値の温度(この量だけ・必須)
//           -- species = "oxygen",               -- species_amount の物質(この量だけ・必須)
//           at_most = 0,                         -- 量がこれ以下になったら止める(at_least = 以上。どちらも省けば上限の刻みの量を測る)
//       },
//   }
// 場面は表の世界の組み立て(world_cells)の初めの世界。判定はつつきを全部当てた後の測る刻みだけ(sim/play_metric.h)。
// 合わせた表の分類の 1 つなので、表の版(TableVersion)に入る(T-0398 の共有の決め事。指標だけを変えても版が変わる。世界は変わらない)。
// 欄を変えたら、型の定義 data/types/bicameral.d.luau の PlayMetric も変える。ここは CPU だけ(原則 2)。
#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "script/luau_package.h"
#include "sim/play_metric.h"

namespace bicameral::script {

    constexpr std::string_view PLAY_METRICS_CATEGORY = "play_metrics";

    // 分類が無ければ空(ブランクの世界。D-455)。誤りは「パッケージ X: play_metrics.鍵.欄: 何が違うか」。
    // ReadReactionTableDefinition がこれを呼ぶ(T-0398 の共有の決め事)
    [[nodiscard]] std::expected<std::vector<sim::PlayMetricDefinition>, std::string> ReadPlayMetrics(
        const PackageSetResult& packages);

}  // namespace bicameral::script
