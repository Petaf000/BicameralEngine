// slope_gauge.h — 開発ビルドの坂の計器: 刻みごとに、反応が坂を上った量を数える(T-0518・ADR-0230。D-487 ②・docs/design/19 §10)。
//
// ベイクの坂の検査(sim/reaction_slope)は標準の状態の ΔG° だけを見るので、「釣り合いの近くの一方向の規則が、生成物が溜まると
// 釣り合いを越えて進む」(T-0491 の boudouard の 1645〜1738 K)は捕まえられない。ここはそれを、進んだ反応ごとに
// その時のセルの成分(活量)込みの ΔG = ΔG°(T) + R T Σ ν ln a で数える。**見るだけで、シミュの結果は変えない**(D-465)。
//
// データの流れ:
//   ベイクした表(物質の h0・比熱・標準エントロピー・凝縮相の印)→ MakeSlopeGaugeTable(規則ごとの ΔG°(T) の表。1 K ごと)
//   → 刻みごとに、成分が変わったセルの反応を CPU リファレンスの刻みの核と同じ手順でなぞり(CountProbeSlopeTick。
//     なぞった結果が S(t + 1) とビット単位で同じことを数えて確かめる)、規則ごとの進行度 ξ と ΔG を CountCellSlope が数える
//   → SlopeGaugeTally → SlopeGaugeReport(ログ。frame/slope_gauge_cli の --slope-gauge)。
// 決めごと:
//   - 活量: 凝縮相(物質の任意の欄 condensed。BakedReactionTable::speciesCondensed)は 1、ほかは理想気体の分圧 / 1 bar
//     (a = n R T / (V p°)。0 の量は 1 µmol と置く)。エンジンの物質は相を持たない(M2)ので、condensed は相が入るまでの仮の欄。
//   - 温度は 1 K に切り捨て(ΔG° の表と同じ。反応の段の速さの表と同じ引き方)。ln は fixed.hlsli の FxLnU64(Q32)。
//   - 体積一定のセルだが坂は定圧の ΔG で見る(試験 slope_check と同じ近似。符号の判定には十分という仮定。未確認)。
// 整数だけで数える(engine/src/sim は浮動小数点を書けない。04 §4)ので、どの機械でも同じ数になる。CPU リファレンスは GPU と
// ビット一致なので、ここで数えた値は GPU の世界の値と同じ(GPU の版は要らない。生きた GPU の世界で数える版は T-0540)。
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "common/heat_conduction.hlsli"
#include "common/reaction.hlsli"
#include "sim/reaction_slope.h"

namespace bicameral::sim {

    struct BakedReactionTable;

    // 仮の世界(ProbeSim)のセルの V p°(0.5 m 角 = 0.125 m³ × 1 bar = 12,500 J)
    inline constexpr uint64_t PROBE_CELL_STANDARD_JOULES = 12500;

    // 計器の物差し(表から 1 回作る。表が変われば作り直す)
    struct SlopeGaugeTable {
        struct Rule {
            std::vector<RuleNetTerm> terms;      // 正味の係数(触媒は入らない)
            std::vector<int64_t> standardGibbs;  // ΔG°(T)(mJ/mol。添字 = K)。空なら調べられない規則
        };

        std::vector<Rule> rules;         // 添字 = 規則 ID
        std::vector<uint8_t> condensed;  // 1 = 凝縮相(活量 1)。添字 = 物質 ID
        int64_t gasLogOffset = 0;        // ln(V p° × 10^15)(Q32)。気体の ln a = ln n[µmol] + ln(R[nJ/(mol·K)] T) − これ
    };

    [[nodiscard]] SlopeGaugeTable MakeSlopeGaugeTable(const BakedReactionTable& baked,
                                                      uint64_t cellStandardJoules = PROBE_CELL_STANDARD_JOULES);

    // 規則 ruleId の、セル cell の成分・温度 kelvin(1〜4095)での ΔG(mJ/mol。正 = 坂を上る)。
    // 調べられない規則・0 K は false を返す(gibbs は書かない)
    [[nodiscard]] bool CellRuleGibbs(const SlopeGaugeTable& gauge, uint32_t ruleId, const reaction::RxCell& cell,
                                     uint32_t kelvin, int64_t& gibbs);

    // 規則ごとの数(刻みをまたいで足していく)
    struct SlopeGaugeRuleTally {
        uint64_t events = 0;            // 進んだ回(ξ > 0 のセル × 刻み)
        uint64_t extentMicromoles = 0;  // Σ ξ

        // --- 坂を上った分(成分込みの ΔG > 0)---
        uint64_t uphillEvents = 0;
        uint64_t uphillExtentMicromoles = 0;
        uint64_t uphillWorkNanojoules = 0;     // Σ ξ ΔG(坂を上らせたエネルギー。飽和する)
        uint64_t compositionUphillEvents = 0;  // そのうち ΔG° ≤ 0(成分しだい = ベイクの警告では捕まらない分)
        uint64_t compositionUphillExtentMicromoles = 0;
        uint32_t uphillLowKelvin = 0;  // 坂を上った温度の範囲(uphillEvents == 0 なら 0)
        uint32_t uphillHighKelvin = 0;
        uint64_t firstUphillTick = 0;
        uint64_t lastUphillTick = 0;
    };

    struct SlopeGaugeTally {
        std::vector<SlopeGaugeRuleTally> rules;  // 添字 = 規則 ID
        uint64_t ticks = 0;                      // 数えた刻み
        uint64_t uphillTicks = 0;                // 坂を上った反応が 1 つでもあった刻み
        uint64_t unevaluatedEvents = 0;          // 調べられない規則が進んだ回(数えていない)
        uint64_t replayedCells = 0;              // なぞったセル(延べ)
        uint64_t replayMismatches = 0;  // なぞった結果が S(t + 1) と違うセル(0 のはず。0 でなければ計器が壊れている)
    };

    // 1 セルの 1 刻みの反応(取り合いの後の進行度 extents。conducted = 反応の前のセル)を数える。
    // 坂を上った反応があれば true
    bool CountCellSlope(const SlopeGaugeTable& gauge, const reaction::RxCell& conducted, uint32_t kelvin,
                        const reaction::RxCandidates& extents, uint64_t tick, SlopeGaugeTally& tally);

    // 仮の世界(ProbeSim)の刻み tick を数える。current = S(tick)・caches = その熱のキャッシュ・next = S(tick + 1)・
    // changedMarks = Advance(tick) の前のブロックの印(ProbeReference::ChangedMarks の写し)。
    // 成分が変わったセルだけ、刻みの核と同じ手順で反応をなぞる
    void CountProbeSlopeTick(const BakedReactionTable& table, const SlopeGaugeTable& gauge,
                             std::span<const reaction::RxCell> current,
                             std::span<const reaction::HcThermalCache> caches, std::span<const reaction::RxCell> next,
                             std::span<const uint64_t> changedMarks, uint64_t tick, SlopeGaugeTally& tally);

    // ログの文: まとめ 1 行 + 坂を上った規則ごとに 1 行(+ 調べられない規則・なぞりの食い違いがあれば 1 行ずつ)
    [[nodiscard]] std::vector<std::string> SlopeGaugeReport(const BakedReactionTable& table,
                                                            const SlopeGaugeTally& tally);

}  // namespace bicameral::sim
