// probe_trace.h — 伝導の連鎖のトレース(GPU の記録。shaders/common/graph_trace.hlsli)を刻みごとの木に組む・CPU リファレンスの予想と比べる・
// CPU リファレンスと食い違った最初の刻み・ブロック・セルを探す(T-0087、16 §1.3・§3)。
//
// データの流れ:
//   ProbeSim(範囲を ProbeSimOptions::trace に)→ ProbeFrameReadback::trace(atomic の順)→ 集めて gpu::SortGraphTrace
//   → FormatProbeTrace(刻みごとの木の文字列。ファイルへ)/ UniqueProbeTrace と ExpectedProbeTrace を比べる(CPU リファレンス)
//   ハッシュの列(ProbeTickHash)→ FirstDivergentTick → その刻みの GPU の状態(抽出)と CPU の状態 → FindCellDivergence
//
// 記録の種類は probe_sim.hlsli の PROBE_TRACE_*(つつき・起こす・計算した)。木の組み方:
//   刻み t の根 = 活性の一覧のブロック(起こすの主)。つつき(刻み t)か、刻み t − 1 で値が変わったブロック
//   根の子 = 起こされて計算したブロック。複数の根に起こされたブロックの親は「一番小さい番号の根」(GPU で誰が先に予定したかは
//   毎回変わるので、決まった規則で選ぶ)。値が変わった子は、刻み t + 1 の根になる。
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/aliases.h"
#include "gpu/graph_trace.h"
#include "sim/probe_sim.h"

namespace bicameral::sim {

    // --- 範囲 ---

    // セルの箱 [cellMin, cellMax) と刻み [tickBegin, tickEnd) を、トレースの範囲(場所はブロックの座標。セルを含むブロックを全部)にする
    [[nodiscard]] gpu::GraphTraceFilter ProbeTraceFilterForCells(uint64_t tickBegin, uint64_t tickEnd,
                                                                 std::array<uint32_t, 3> cellMin,
                                                                 std::array<uint32_t, 3> cellMax, uint32_t capacity);

    // --- 木にする ---

    // 並べた記録(gpu::SortGraphTrace の後)を刻みごとの木の文字列にする(1 刻み = 見出しの行 + 根ごとの行 + 子ごとの行)
    [[nodiscard]] std::string FormatProbeTrace(std::span<const gpu::GraphTraceRecord> sorted);

    // 集めた記録(順不同)を並べて木にし、範囲と落とした数の見出しを付けてファイル(UTF-8)に書く
    [[nodiscard]] std::expected<void, std::string> WriteProbeTraceFile(const fs::path& path,
                                                                       std::vector<gpu::GraphTraceRecord> records,
                                                                       const gpu::GraphTraceFilter& filter,
                                                                       uint64_t droppedCount);

    // --- CPU リファレンスと比べる ---

    // 並べて重なりを除く(活性の一覧には同じブロックが 2 度入りうる。CPU リファレンスは重なりを数えないので、比べるときはこの形)
    [[nodiscard]] std::vector<gpu::GraphTraceRecord> UniqueProbeTrace(std::vector<gpu::GraphTraceRecord> records);

    // CPU リファレンスが予想する刻み tick の記録(重なりなし)を expected に足す。
    // changedBefore = 刻み tick − 1 で値が変わったブロック、changedAfter = 刻み tick で変わったブロック(ProbeReference::ChangedBlocks)
    void AppendExpectedProbeTrace(const gpu::GraphTraceFilter& filter, uint64_t tick,
                                  std::span<const ProbeCommand> commands, std::span<const uint8_t> changedBefore,
                                  std::span<const uint8_t> changedAfter, std::vector<gpu::GraphTraceRecord>& expected);

    // 並べた 2 つの列の最初の食い違い(無ければ nullopt)。「刻み t: GPU … / CPU …」の 1 行
    [[nodiscard]] std::optional<std::string> FirstProbeTraceMismatch(std::span<const gpu::GraphTraceRecord> gpu,
                                                                     std::span<const gpu::GraphTraceRecord> cpu);

    // --- 食い違いの場所(16 §3 の道具の最初の形)---

    // 刻みの順の 2 つの列で、同じ刻みのハッシュ(と熱の合計)が違う最初の状態の刻み(S(t) の t)。無ければ nullopt
    [[nodiscard]] std::optional<uint64_t> FirstDivergentTick(std::span<const ProbeTickHash> gpu,
                                                             std::span<const ProbeTickHash> cpu);

    struct ProbeDivergence {
        uint64_t tick = 0;  // 状態 S(tick)

        // --- 最初に食い違ったブロック(番号の小さい順)とその中の最初のセル(ブロックの中の z, y, x の順)---
        uint32_t block = 0;
        std::array<uint32_t, 3> cell = {0, 0, 0};
        uint32_t gpuValue = 0;
        uint32_t cpuValue = 0;

        // --- 広がり ---
        uint32_t differingCells = 0;
        uint32_t differingBlocks = 0;
    };

    // 同じ刻みの全部のセル(PROBE_CELL_COUNT 個)を比べる。全部同じなら nullopt
    [[nodiscard]] std::optional<ProbeDivergence> FindCellDivergence(uint64_t tick, std::span<const uint32_t> gpuCells,
                                                                    std::span<const uint32_t> cpuCells);

    // 「S(t) で CPU リファレンスと食い違った: ブロック … セル (x, y, z) GPU … CPU …(食い違ったセル n・ブロック m)」
    [[nodiscard]] std::string FormatProbeDivergence(const ProbeDivergence& divergence);

}  // namespace bicameral::sim
