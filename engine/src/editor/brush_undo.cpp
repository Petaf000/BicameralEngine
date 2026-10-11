// brush_undo.cpp — 筆の元に戻す: 戻す保存点を選ぶ(T-0278・ADR-0101)。流れは brush_undo.h。
#include "editor/brush_undo.h"

#include <format>

namespace bicameral::editor {

    std::expected<uint64_t, std::string> PlanBrushUndo(uint64_t firstTick, std::span<const uint64_t> savePointTicks,
                                                       std::optional<uint64_t> lastTableSwapTick) {
        std::optional<uint64_t> best;
        for (const uint64_t tick : savePointTicks) {
            if (tick <= firstTick && (!best || tick > *best))
                best = tick;
        }

        if (!best)
            return std::unexpected(std::format("刻み {} より前の保存点が無い(古すぎる運び)", firstTick));

        if (lastTableSwapTick && *lastTableSwapTick >= *best)
            return std::unexpected(std::format("保存点の刻み {} の後に反応表を差し替えた(刻み {})ので戻せない", *best,
                                               *lastTableSwapTick));

        return *best;
    }

}  // namespace bicameral::editor
