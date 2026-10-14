// edit_table_test.cpp — editor/edit_table(編集の境界 E に当てる反応表を控えの印で決め直す。T-0548・ADR-0246)を CPU だけで確かめる。
// 確かめること: S(E) の表(E より前の最後の印・無ければ起動時の表)・E の印の後の表・印は送った履歴と足し直す列の両方から
// (刻み, 番号) の順で読む・E の印だけを外す・順を保って入れる。
// 失敗すると条件と行を表示して 1 を返す(ctest が落ちる)。
#include "editor/edit_table.h"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <vector>

#include "common/probe_sim.hlsli"

namespace {

    using namespace bicameral;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr uint64_t INITIAL = 0x1000;

    sim::Command Place(uint64_t tick, uint32_t sequence) {
        return {.targetTick = tick, .sequence = sequence, .type = 1};
    }

    sim::Command Marker(uint64_t tick, uint32_t sequence, uint64_t version) {
        sim::Command command{
            .targetTick = tick, .sequence = sequence, .type = static_cast<uint16_t>(sim::PROBE_COMMAND_TYPE_TABLE)};
        command.payload[0] = static_cast<uint32_t>(version);
        command.payload[1] = static_cast<uint32_t>(version >> 32);

        return command;
    }

    void TestPlan() {
        // 印が無い: どちらも起動時の表
        std::vector<sim::Command> history = {Place(0, 1), Place(5, 2)};
        std::deque<sim::Command> feed;
        auto plan = editor::PlanBoundaryTable(5, history, feed, INITIAL);
        EXPECT(plan.beforeVersion == INITIAL && plan.currentVersion == INITIAL && !plan.hasMarker);

        // E より前の印(履歴)と E の印(足し直す列)。E の印は同じ刻みの番号の大きい方
        history = {Marker(2, 3, 0x2000), Marker(3, 4, 0x3000), Place(5, 5)};
        feed = {Marker(5, 6, 0x4000), Marker(5, 7, 0x5000), Marker(9, 8, 0x9000)};
        plan = editor::PlanBoundaryTable(5, history, feed, INITIAL);
        EXPECT(plan.beforeVersion == 0x3000);
        EXPECT(plan.hasMarker && plan.currentVersion == 0x5000);

        // E より前の最後の印が足し直す列にある(戻した刻みより先の印)
        feed = {Marker(4, 9, 0x6000)};
        plan = editor::PlanBoundaryTable(5, history, feed, INITIAL);
        EXPECT(plan.beforeVersion == 0x6000 && plan.currentVersion == 0x6000 && !plan.hasMarker);
        EXPECT(editor::TableMarkerVersion(Marker(0, 0, 0x123456789abcdef0)) == 0x123456789abcdef0);
        EXPECT(!editor::IsTableMarker(Place(0, 0)));
    }

    void TestRemoveAndInsert() {
        std::vector<sim::Command> history = {Marker(0, 1, 0x2000), Place(0, 2), Marker(1, 3, 0x3000)};
        std::deque<sim::Command> feed = {Marker(0, 4, 0x4000), Place(0, 5)};
        EXPECT(editor::RemoveTableMarkersAt(0, history, feed) == 2);
        EXPECT(history.size() == 2 && feed.size() == 1 && !editor::IsTableMarker(history[0]));

        editor::InsertInOrder(history, Marker(0, 9, 0x7000));
        EXPECT(history.size() == 3 && history[1].sequence == 9 && history[2].targetTick == 1);
        editor::InsertInOrder(feed, Marker(0, 1, 0x7000));
        EXPECT(feed.front().sequence == 1);
    }

}  // namespace

int main() {
    TestPlan();
    TestRemoveAndInsert();
    if (failureCount != 0) {
        std::printf("edit_table_test: %d 件失敗\n", failureCount);
        return 1;
    }

    std::printf("edit_table_test: OK\n");
    return 0;
}
