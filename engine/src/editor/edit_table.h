// edit_table.h — 編集の境界 E に当てる反応表を、今の流れの控え(コマンドの列)の中の差し替えの印で決め直す(T-0548・ADR-0246)。
// GPU を知らない(CPU だけのテスト tests/edit_table_test.cpp)。
//
// 印(PROBE_COMMAND_TYPE_TABLE)は「刻み t の始めに版 v の表に替わる」(sim::MakeTableCommand)。控え(送ったコマンドの履歴と、
// 巻き戻して足し直す列)を (刻み, 番号) の順に見て、S(E) の表(E より前の最後の印)と、E に当てるコマンドが使う表(E の印の後)を出す。
// 編集の状態で表を差し替えた・プレイをやめてファイルの表に戻すとき、フレームのループ(frame/frame_loop の SetBoundaryTable)は:
//   PlanBoundaryTable で今の表を見る → E の印を外す(RemoveTableMarkersAt)→ E 以降のコマンドの物質の ID を付け替える
//   → S(E) の表と違えば新しい印を足す(InsertInOrder)→ S(E) へ戻して E の適用の単位で当て直す(置いたものと同じ道。ADR-0236)
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "sim/command.h"

namespace bicameral::editor {

    struct BoundaryTablePlan {
        uint64_t beforeVersion = 0;   // S(E) の表(E より前の最後の印。無ければ起動時の表)
        uint64_t currentVersion = 0;  // E に当てるコマンドが使う表(E の印の後。印が無ければ beforeVersion)
        bool hasMarker = false;       // E に印がある
    };

    // 印か(sim::PROBE_COMMAND_TYPE_TABLE)と、印の版(sim::TableCommandVersion と同じ読み方)
    [[nodiscard]] bool IsTableMarker(const sim::Command& command);
    [[nodiscard]] uint64_t TableMarkerVersion(const sim::Command& command);

    [[nodiscard]] BoundaryTablePlan PlanBoundaryTable(uint64_t tick, const std::vector<sim::Command>& history,
                                                      const std::deque<sim::Command>& feed, uint64_t initialVersion);

    // 刻み tick の印を両方の列から外す(外した数)
    size_t RemoveTableMarkersAt(uint64_t tick, std::vector<sim::Command>& history, std::deque<sim::Command>& feed);

    // (刻み, 番号) の順を保って入れる
    template <typename Commands>
    void InsertInOrder(Commands& commands, const sim::Command& command) {
        commands.insert(std::ranges::upper_bound(commands, command, sim::CommandPrecedes), command);
    }

}  // namespace bicameral::editor
