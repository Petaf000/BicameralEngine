// brush_undo.h — 筆の元に戻す(T-0278・ADR-0101。docs/design/14 §2「実装(T-0278)」)。GPU を知らない CPU の計算だけ(brush_undo_test)。
//
// 元に戻す = 「その運びが最初に当たった刻み以前の保存点へ巻き戻し、その運びのコマンドを除いて今の刻みまで流し直す」(ADR-0089 §4)。
// 逆のコマンドで戻さないのは、置く前のセルの中身を CPU が知らず(D-107)、置いた後に進んだ反応(燃え広がった先)も戻せないから。
// データの流れ:
//   筆のパネル(BrushPanel)が運び(押してから、離して置き終えるまで)ごとにコマンドの番号(sequence)を控える
//   → 元に戻す: フレームのループが EarliestStrokeTick と PlanBrushUndo で戻す保存点を決め、RemoveStrokeCommands で
//     待っているコマンド・今の流れの控え・足し直しを待つ列からその運びを除く
//   → 保存点へ戻して今の刻みまで流し直す(frame_loop の PrepareRewind。SimScheduler::CatchUpTo で現実の時間を待たずに追いつく)
//   → 記録中なら記録も保存点の刻みまで切り詰め(save::ReplayRecorder::Rewind)、流し直した分を書き直す(運びは記録に残らない)
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "sim/command.h"

namespace bicameral::editor {

    // 1 回の筆の運び(1 回のクリック・1 回のドラッグ)が作ったコマンドの番号
    using BrushStrokeSequences = std::vector<uint32_t>;

    [[nodiscard]] inline bool StrokeHas(std::span<const uint32_t> stroke, const sim::Command& command) {
        return std::ranges::contains(stroke, command.sequence);
    }

    // commands の中で、その運びのコマンドがいちばん早く当たる刻み(無ければ UINT64_MAX)
    template <class Commands>
    [[nodiscard]] uint64_t EarliestStrokeTick(std::span<const uint32_t> stroke, const Commands& commands) {
        uint64_t earliest = UINT64_MAX;
        for (const sim::Command& command : commands) {
            if (StrokeHas(stroke, command))
                earliest = std::min(earliest, command.targetTick);
        }

        return earliest;
    }

    // commands からその運びのコマンドを除く。除いた数を返す
    template <class Commands>
    size_t RemoveStrokeCommands(std::span<const uint32_t> stroke, Commands& commands) {
        // std::erase_if は入れ物ごとの多重定義なので、どの入れ物でも使える remove_if + erase で
        const auto removed = std::ranges::remove_if(
            commands, [&](const sim::Command& command) { return StrokeHas(stroke, command); });
        const auto count = static_cast<size_t>(std::ranges::distance(removed));
        commands.erase(removed.begin(), removed.end());

        return count;
    }

    // 刻み firstTick に当たった運びを除くために戻る保存点の刻み(firstTick 以前でいちばん新しいもの)。
    // 保存点 S(s) には刻み s のコマンドはまだ入っていない(s 以降を足し直す。ADR-0036)ので s ≤ firstTick でよい。
    // lastTableSwapTick = 最後に反応表を差し替えた刻み。巻き戻しは差し替えを足し直さないので、それ以前の保存点へは戻さない
    [[nodiscard]] std::expected<uint64_t, std::string> PlanBrushUndo(uint64_t firstTick,
                                                                     std::span<const uint64_t> savePointTicks,
                                                                     std::optional<uint64_t> lastTableSwapTick);

}  // namespace bicameral::editor
