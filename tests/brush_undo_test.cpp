// brush_undo_test.cpp — editor/brush_undo(筆の元に戻す: 戻す保存点を選ぶ・運びのコマンドを除く。T-0278・ADR-0101)を CPU だけで確かめる。
// 確かめること: 運びがいちばん早く当たる刻み・その刻み以前でいちばん新しい保存点(同じ刻みでもよい)・保存点が無い / 古すぎる・
// 反応表の差し替えより前へは戻さない・待っている列と足し直しの列(deque)からその運びだけを除く。
// 失敗すると条件と行を表示して 1 を返す(ctest が落ちる)。
#include "editor/brush_undo.h"

#include <cstdint>
#include <cstdio>
#include <deque>
#include <vector>

namespace {

    using namespace bicameral;
    using editor::BrushStrokeSequences;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    sim::Command MakeCommand(uint64_t tick, uint32_t sequence) {
        return {.targetTick = tick, .sequence = sequence};
    }

    void TestEarliestTick() {
        const BrushStrokeSequences stroke = {4, 5, 6};
        const std::vector<sim::Command> history = {MakeCommand(3, 1), MakeCommand(9, 4), MakeCommand(10, 5)};
        const std::deque<sim::Command> feed = {MakeCommand(7, 6), MakeCommand(8, 7)};
        EXPECT(editor::EarliestStrokeTick(stroke, history) == 9);
        EXPECT(editor::EarliestStrokeTick(stroke, feed) == 7);
        EXPECT(editor::EarliestStrokeTick(BrushStrokeSequences{2}, history) == UINT64_MAX);
    }

    void TestPlan() {
        const std::vector<uint64_t> savePoints = {16, 4, 8, 12};  // 並びは問わない
        EXPECT(editor::PlanBrushUndo(10, savePoints, std::nullopt) == uint64_t{8});
        EXPECT(editor::PlanBrushUndo(12, savePoints, std::nullopt) ==
               uint64_t{12});  // 保存点 S(12) に刻み 12 のコマンドはまだ入っていない
        EXPECT(editor::PlanBrushUndo(40, savePoints, std::nullopt) == uint64_t{16});
        EXPECT(!editor::PlanBrushUndo(3, savePoints, std::nullopt).has_value());  // 古すぎる
        EXPECT(!editor::PlanBrushUndo(10, std::vector<uint64_t>{}, std::nullopt).has_value());

        // 反応表の差し替え: 戻す保存点より前(刻み 7)なら戻せる・同じか後(8・9)なら戻せない
        EXPECT(editor::PlanBrushUndo(10, savePoints, 7) == uint64_t{8});
        EXPECT(!editor::PlanBrushUndo(10, savePoints, 8).has_value());
        EXPECT(!editor::PlanBrushUndo(10, savePoints, 9).has_value());
    }

    void TestRemove() {
        const BrushStrokeSequences stroke = {4, 6};
        std::vector<sim::Command> pending = {MakeCommand(0, 3), MakeCommand(0, 4), MakeCommand(0, 5),
                                             MakeCommand(0, 6)};
        std::deque<sim::Command> feed = {MakeCommand(7, 6), MakeCommand(8, 7)};
        EXPECT(editor::RemoveStrokeCommands(stroke, pending) == 2);
        EXPECT(pending.size() == 2 && pending[0].sequence == 3 && pending[1].sequence == 5);
        EXPECT(editor::RemoveStrokeCommands(stroke, feed) == 1);
        EXPECT(feed.size() == 1 && feed.front().sequence == 7);
        EXPECT(editor::RemoveStrokeCommands(stroke, feed) == 0);
    }

}  // namespace

int main() {
    TestEarliestTick();
    TestPlan();
    TestRemove();
    std::printf("brush_undo_test: %s\n", failureCount == 0 ? "OK" : "FAILED");

    return failureCount == 0 ? 0 : 1;
}
