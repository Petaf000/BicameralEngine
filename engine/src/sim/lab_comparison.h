// lab_comparison.h — 実験室で 2 つの実験を並べて比べる(T-0221・D-449)。同じ保存点(実験の記録を刻み t で切ったもの)から、
// A は元の実験の続き、B は条件を 1 つ足したものを、同じ箱(sim::LabSession)で順に初めの箱から流し、刻みごとの計器の値を並べる。
//
// データの流れ:
//   sim/lab_gauge の MakeLabComparisonPlan(段取り)→ RunLabComparison:
//     A の記録(保存点 + 続き)を LabSession::Replay → 計器の列 A・記録 A → B の記録(+ 条件)を Replay → 計器の列 B・記録 B
//   → editor/gauge_panel が同じ刻みの値を重ねて描く。
// 保存点は記録から流し直して作る(箱は決定的。ADR-0008・ADR-0037)。保存点までのハッシュは元の実験と突き合わせるので、
// 流し直した箱が保存点で同じでなければ失敗にする。箱は 1 つだけ(debug では箱の作成に時間がかかるので、2 つ目の箱を作らない)。
// GPU と CPU リファレンスは刻みごとに比べたまま流す(食い違えば失敗)。流し終えた箱は B の状態。
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "sim/lab_gauge.h"
#include "sim/lab_session.h"

namespace bicameral::sim {

    struct LabComparison {
        uint64_t savePointTick = 0;
        uint32_t gaugeCell = 0;

        // --- 刻み 0 から保存点 + 流した刻みまでの計器の値(GPU から読み戻した箱。保存点までは A と B で同じ)---
        std::vector<LabGaugeSample> a;
        std::vector<LabGaugeSample> b;

        // --- 流した記録(ハッシュは流した全部の刻み。保存して別に再生できる)---
        LabRecording recordingA;
        LabRecording recordingB;

        std::optional<uint64_t> firstDifference;  // A と B の計器の値が最初に違った刻み(違わなければ無し)
    };

    // 段取りを session の箱で A → B の順に流す。session の今の実験は捨てる(初めの箱から流し直す)。見るセルは session の今のもの
    [[nodiscard]] std::expected<LabComparison, std::string> RunLabComparison(LabSession& session,
                                                                             const LabComparisonPlan& plan);

}  // namespace bicameral::sim
