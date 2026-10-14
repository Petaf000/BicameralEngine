// edit_table.cpp — 編集の境界 E に当てる反応表を、控えの中の差し替えの印で決め直す(T-0548・ADR-0246)。流れは edit_table.h。
#include "editor/edit_table.h"

#include "common/probe_sim.hlsli"

namespace bicameral::editor {

    namespace {

        // 列の中の、条件に合う最後の印((刻み, 番号) の順。両方の列は並んでいる)
        template <typename Commands, typename Predicate>
        const sim::Command* LastMarker(const Commands& commands, Predicate predicate, const sim::Command* best) {
            for (const sim::Command& command : commands) {
                if (!IsTableMarker(command) || !predicate(command))
                    continue;

                if (best == nullptr || sim::CommandPrecedes(*best, command))
                    best = &command;
            }

            return best;
        }

    }  // namespace

    bool IsTableMarker(const sim::Command& command) {
        return command.type == sim::PROBE_COMMAND_TYPE_TABLE;
    }

    uint64_t TableMarkerVersion(const sim::Command& command) {
        return uint64_t{command.payload[0]} | (uint64_t{command.payload[1]} << 32);
    }

    BoundaryTablePlan PlanBoundaryTable(uint64_t tick, const std::vector<sim::Command>& history,
                                        const std::deque<sim::Command>& feed, uint64_t initialVersion) {
        const auto before = [&](const sim::Command& command) {
            return command.targetTick < tick;
        };
        const auto at = [&](const sim::Command& command) {
            return command.targetTick == tick;
        };

        const sim::Command* lastBefore = LastMarker(feed, before, LastMarker(history, before, nullptr));
        const sim::Command* lastAt = LastMarker(feed, at, LastMarker(history, at, nullptr));

        BoundaryTablePlan plan;
        plan.beforeVersion = lastBefore != nullptr ? TableMarkerVersion(*lastBefore) : initialVersion;
        plan.hasMarker = lastAt != nullptr;
        plan.currentVersion = plan.hasMarker ? TableMarkerVersion(*lastAt) : plan.beforeVersion;

        return plan;
    }

    size_t RemoveTableMarkersAt(uint64_t tick, std::vector<sim::Command>& history, std::deque<sim::Command>& feed) {
        const auto marker = [&](const sim::Command& command) {
            return IsTableMarker(command) && command.targetTick == tick;
        };

        return std::erase_if(history, marker) + std::erase_if(feed, marker);
    }

}  // namespace bicameral::editor
