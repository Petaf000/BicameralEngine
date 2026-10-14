// brush_undo.cpp — 筆の元に戻す: 戻す保存点を選ぶ(T-0278・ADR-0101)。流れは brush_undo.h。
#include "editor/brush_undo.h"

#include <format>

namespace bicameral::editor {

    std::expected<uint64_t, std::string> PlanBrushUndo(uint64_t firstTick, std::span<const uint64_t> savePointTicks,
                                                       uint64_t floorTick) {
        if (firstTick < floorTick) {
            return std::unexpected(
                std::format("刻み {} の運びは戻せない(プレイ中は刻み {} より後だけ。プレイ前に置いたものは"
                            "プレイをやめてから・プレイ中は最近約 12 秒)",
                            firstTick, floorTick));
        }

        std::optional<uint64_t> best;
        for (const uint64_t tick : savePointTicks) {
            if (tick >= floorTick && tick <= firstTick && (!best || tick > *best))
                best = tick;
        }

        if (!best)
            return std::unexpected(std::format("刻み {} より前の保存点が無い(古すぎる運び)", firstTick));

        return *best;
    }

}  // namespace bicameral::editor
