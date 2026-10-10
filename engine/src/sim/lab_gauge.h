// lab_gauge.h — 実験室(14 §2・T-0142)の計器と、2 つの実験を比べる段取り(T-0221・D-449)の CPU 側。GPU を知らない。
// 計器は箱の状態から、見るセルの温度と物質量・箱全体の物質量と最高温度を 1 刻みに 1 つ取る(LabGaugeSample)。
// 反応の速さは物質量の刻みごとの差(µmol/刻み)。グラフ(editor/gauge_panel)は GPU から読み戻した箱の値を描き、
// CPU リファレンスだけで同じ記録を流した値(RunLabGaugeOnCpu)と一致することを自動の確認(--auto-lab-compare)とテストが見る。
//
// データの流れ:
//   sim::LabSession が刻むたびに、読み戻した GPU の箱から SampleLabGauge(世界に何も返さない。ADR-0035 の「見るだけ」)
//   → editor/gauge_panel が浮動小数点に直して描く。
//   比べる: 実験の記録(LabRecording)を刻み t で切ったもの = 保存点(CutLabRecording。初めの箱から決定的に同じ状態へ戻れる)
//   → MakeLabComparisonPlan(A = 元の実験の続き / B = それに条件を 1 つ足す)→ sim/lab_comparison が同じ箱で A・B を流す。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "sim/lab_box.h"

namespace bicameral::sim {

    inline constexpr size_t LAB_GAUGE_MAX_SAMPLES = 20000;  // 計器が持つ刻みの数の上限(超えたら古い方から捨てる)

    // 計器の 1 刻み(刻み tick を終えた箱)
    struct LabGaugeSample {
        uint64_t tick = 0;
        uint32_t cell = 0;  // 見るセル(MrCellIndex)

        // --- 温度(mK)---
        int32_t cellTemperatureMilliKelvin = 0;
        int32_t maxTemperatureMilliKelvin = 0;  // 箱の中の最高

        // --- 物質量(µmol。添字 = 物質 ID。表の物質の数)---
        std::vector<uint64_t> cellAmounts;
        std::vector<uint64_t> boxAmounts;

        bool operator==(const LabGaugeSample&) const = default;
    };

    // 箱の状態から計器の値を取る(nest は GPU から読み戻した箱でも CPU リファレンスでも同じ形)
    [[nodiscard]] LabGaugeSample SampleLabGauge(const MultiresNest& nest, const BakedReactionTable& table,
                                                uint64_t tick, uint32_t cellIndex);

    // 反応の速さ: 物質 species の、前の刻みからの増え方(µmol/刻み。減れば負)。最初の刻み・物質が範囲の外なら 0。
    // wholeBox = 箱全体の量、でなければ見るセルの量
    [[nodiscard]] int64_t LabGaugeRate(std::span<const LabGaugeSample> samples, size_t index, uint32_t species,
                                       bool wholeBox);

    // CPU リファレンスだけで記録を初めの箱から流し、刻みごとの計器の値を返す(GPU の値と比べる基準)。
    // 表を替えた印のある記録は扱わない(表が 1 つの実験だけ。エラー)
    [[nodiscard]] std::expected<std::vector<LabGaugeSample>, std::string> RunLabGaugeOnCpu(
        const LabRecording& recording, const BakedReactionTable& table, uint32_t cellIndex);

    // --- 2 つの実験を比べる(T-0221)---

    // 保存点: 記録を刻み tick の始め(刻み 0 〜 tick − 1 を終えた所)で切る。コマンドは tick より前の刻みのものだけ、ハッシュは tick 個。
    // 表の中身は切らない(印の表が残っても再生に害は無い)。tick が記録の刻みの数より大きければ記録の刻みの数で切る
    [[nodiscard]] LabRecording CutLabRecording(const LabRecording& recording, uint64_t tick);

    // 同じ保存点から流す 2 つの実験。A は元の実験の続き、B は A に条件を 1 つ(change)足したもの
    struct LabComparisonPlan {
        LabRecording savePoint;  // 刻み savePoint.tickCount の始めまで(両方が同じ所から始まる)
        std::vector<Command>
            continued;           // 保存点から後の、両方に入るコマンド(元の実験の続き。(targetTick, sequence) の昇順)
        Command change{};        // B だけに入る条件(targetTick = 保存点の刻み。sequence は元の実験のどれよりも後)
        uint32_t tickCount = 0;  // 保存点から流す刻みの数
    };

    // 実験の記録 experiment の刻み savePointTick を保存点にして、そこから tickCount 刻みを A・B で比べる段取り。
    // change の刻みと番号はここで決める(保存点の刻み・元の実験のどの番号よりも後 = 同じ刻みの元の操作の後に当たる)
    [[nodiscard]] LabComparisonPlan MakeLabComparisonPlan(const LabRecording& experiment, uint64_t savePointTick,
                                                          uint32_t tickCount, Command change);

    // 段取りの片方を、初めの箱から流せる記録にする(ハッシュは保存点まで。その先は流した時に決まる)
    [[nodiscard]] LabRecording MakeLabBranchRecording(const LabComparisonPlan& plan, bool withChange);

    // 2 つの計器の列が最初に違った刻み(短い方の終わりまで。同じなら無し)
    [[nodiscard]] std::optional<uint64_t> FirstGaugeDifference(std::span<const LabGaugeSample> a,
                                                               std::span<const LabGaugeSample> b);

}  // namespace bicameral::sim
