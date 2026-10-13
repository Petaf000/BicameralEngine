// play_metric.h — 遊びの指標(T-0432・ADR-0206。D-482 ④・docs/design/14 §2「調整の負担を減らす道具」)。
//
// 作り手が登録した「測りたい場面」(例: 密室の火が何分で消えるか)を、表を変えるたびに写しの早送り(sim::WorldCopy・ADR-0198)で
// 測り、前の版との差を出すための形。決定性(ADR-0008)があるので、同じ表なら毎回同じ値になる。
// データの流れ: パッケージの分類 play_metrics(script/play_metrics_package が名前のまま読む)→ PlayMetricDefinition
//   (ReactionTableDefinition::playMetrics)→ BakePlayMetrics(BakeReactionTable の最後。物質の名前を ID に直す)
//   → PlayMetric(BakedReactionTable::playMetrics)→ sim/play_metric_measure(GPU の写し・CPU リファレンスで測る)
//   → PlayMetricValue → ComparePlayMetrics(前の版との差)→ エディタのパネル・コマンドライン(--measure-metrics)。
// 指標の形(中身は持たない。D-454):
//   - 場面 = 表の世界の組み立て(world_cells)の初めの世界 S(0) に、指標のつつき(刻みと場所)を当てたもの
//   - 測る量 = 最高温度・しきい値以上のセルの数・物質の世界全体の量(world_copy.h の小さな道具)のどれか 1 つ
//   - 止め方 = 量がしきい値以下(at_most)/以上(at_least)になった最初の読み戻しの刻み。書かなければ上限の刻みの量を測る
//   - 判定は、つつきを全部当てた後(最後のつつきの刻みより後)の読み戻しの刻みだけ(火をつける前に「火が無い」で止まらないように)
// 値は「何刻みで止まったか」と「止まった刻みの量」。前の版の値はファイル(SerializePlayMetricValues)かエディタの前の測りから。
// 浮動小数点は使わない(engine/src/sim は検査の対象)。秒の表示も整数で作る。ここは CPU だけ。
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bicameral::sim {

    struct BakedReactionTable;

    // 測る量
    enum class PlayMetricQuantity : uint8_t {
        MaxTemperature,  // 世界の最高温度(mK)
        CellsAtOrAbove,  // temperatureMillikelvin 以上のセルの数
        SpeciesAmount,   // 物質 species の世界全体の量(µmol)
    };

    // 止め方
    enum class PlayMetricStop : uint8_t {
        AtLimit,  // 止めない(上限の刻みの量を測る)
        AtMost,   // 量 <= threshold になったら止める
        AtLeast,  // 量 >= threshold になったら止める
    };

    // 指標の場面に当てるつつき(sim::MakePokeCommand。セル (x, y, z) を温める明示的な湧き出し)
    struct PlayMetricPoke {
        uint64_t tick = 0;
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t z = 0;
        friend bool operator==(const PlayMetricPoke&, const PlayMetricPoke&) = default;
    };

    // --- 定義(人が書く値。名前のまま)---

    struct PlayMetricDefinition {
        std::string key;    // 分類 play_metrics の鍵(前の版と突き合わせる名前)
        std::string label;  // 表示の名前

        uint64_t maxTicks = 0;             // 早送りの上限(1 以上)
        uint32_t sampleIntervalTicks = 0;  // 量を測る間隔(1 以上。maxTicks はこの倍数)
        std::vector<PlayMetricPoke> pokes;

        PlayMetricQuantity quantity = PlayMetricQuantity::MaxTemperature;
        std::string species;                  // SpeciesAmount の物質の名前
        uint32_t temperatureMillikelvin = 0;  // CellsAtOrAbove のしきい値の温度
        PlayMetricStop stop = PlayMetricStop::AtLimit;
        uint64_t threshold = 0;  // AtMost・AtLeast のしきい値(量の単位)
    };

    // --- ベイクしたもの(物質は ID)---

    struct PlayMetric {
        std::string key;
        std::string label;
        uint64_t maxTicks = 0;
        uint32_t sampleIntervalTicks = 0;
        std::vector<PlayMetricPoke> pokes;  // 刻みの順(同じ刻みは書いた順)

        PlayMetricQuantity quantity = PlayMetricQuantity::MaxTemperature;
        uint32_t species = 0;
        uint32_t temperatureMillikelvin = 0;
        PlayMetricStop stop = PlayMetricStop::AtLimit;
        uint64_t threshold = 0;

        // 判定を始める刻み(これより後の読み戻しの刻みだけ判定する = 最後のつつきの刻み。つつきが無ければ 0)
        [[nodiscard]] uint64_t JudgeAfterTick() const;
        // 量 value で止めるか
        [[nodiscard]] bool Stops(uint64_t value) const;
    };

    // 鍵の順に並べ、物質の名前を ID に直して確かめる(知らない物質・上限が間隔の倍数でない・つつきが上限より後は誤り)
    [[nodiscard]] std::expected<std::vector<PlayMetric>, std::string> BakePlayMetrics(
        const std::vector<PlayMetricDefinition>& definitions, const BakedReactionTable& table);

    // --- 測った値 ---

    struct PlayMetricValue {
        std::string key;
        std::string label;
        bool measured = false;                               // 測れた(false = 失敗。理由は message)
        bool reached = false;                                // 止め方の条件に着いた(AtLimit は常に false)
        uint64_t ticks = 0;                                  // 始めの刻みから止めた刻みまで(着かなければ上限)
        uint64_t quantity = 0;                               // 止めた刻みの量
        std::vector<std::pair<uint64_t, uint64_t>> samples;  // (始めからの刻み, 量)。読み戻した刻みごと
        std::string message;
        friend bool operator==(const PlayMetricValue&, const PlayMetricValue&) = default;
    };

    // 前の版との差(1 つの指標)
    struct PlayMetricDiff {
        std::string key;
        std::string label;
        std::optional<PlayMetricValue> current;   // 今の表で測った値(今の表に無い指標は空)
        std::optional<PlayMetricValue> previous;  // 前の版の値(無ければ空 = 初めて測った)
        bool changed = false;                     // 着いたか・刻み・量のどれかが違う(どちらかが無い場合も)
        int64_t deltaTicks = 0;                   // 今 − 前(両方あるとき)
        int64_t deltaQuantity = 0;
    };

    // 鍵ごとに突き合わせる(今の順、続いて前にだけある指標を鍵の順)
    [[nodiscard]] std::vector<PlayMetricDiff> ComparePlayMetrics(const std::vector<PlayMetricValue>& current,
                                                                 const std::vector<PlayMetricValue>& previous);

    // 量の単位(表示用。"mK"・"セル"・"µmol")
    [[nodiscard]] std::string_view PlayMetricQuantityUnit(PlayMetricQuantity quantity);

    // 刻みを秒で(整数の計算。例 "12.50 秒")
    [[nodiscard]] std::string FormatTicksAsSeconds(uint64_t ticks, uint64_t ticksPerSecond);

    // 差の一覧を人が読む表に(コマンドラインとログ。1 行 = 1 指標)
    [[nodiscard]] std::string FormatPlayMetricReport(const std::vector<PlayMetricDiff>& diffs, uint64_t ticksPerSecond);

    // --- 前の版の値を残すファイル(テキスト。1 行 = 1 指標。道のりは残さない)---
    //   bicameral-play-metrics 1
    //   <鍵> <measured 0/1> <reached 0/1> <刻み> <量>
    [[nodiscard]] std::string SerializePlayMetricValues(const std::vector<PlayMetricValue>& values);
    [[nodiscard]] std::expected<std::vector<PlayMetricValue>, std::string> ParsePlayMetricValues(std::string_view text);

}  // namespace bicameral::sim
