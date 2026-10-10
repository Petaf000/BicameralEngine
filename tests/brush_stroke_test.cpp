// brush_stroke_test.cpp — editor/brush_stroke(筆をドラッグして塗る時の置く中心の列。T-0245・ADR-0089)を CPU だけで確かめる。
// 確かめること: 押したセルに最初の 1 つ・置き換えは半幅ごと(隙間なし)・足すは 2 × 半幅 + 1 ごと(形を囲む箱が重ならない)・
// 1 フレームの上限と待っているコマンドの上限・置ききれない分は次のフレームに続きから・離したら目標まで置いて終わる・
// 同じセルに止まっていても重ねない・斜めの運び。失敗すると条件と行を表示して 1 を返す(ctest が落ちる)。
#include "editor/brush_stroke.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

    using namespace bicameral;
    using editor::BrushCell;
    using editor::BrushExtents;
    using editor::BrushStroke;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr uint32_t FULL = BrushStroke::MAX_DABS_PER_FRAME;

    void TestReplaceSpacing() {
        BrushStroke stroke;
        const BrushExtents radius2 = {2, 2, 2};
        const std::vector<BrushCell> pressed = {{10, 20, 32}};
        const std::vector<BrushCell> dragged = {{17, 20, 32}};
        const std::vector<BrushCell> dabs = stroke.Paint(pressed, dragged, false, radius2, false, FULL);
        const std::vector<BrushCell> expected = {{10, 20, 32}, {12, 20, 32}, {14, 20, 32}, {16, 20, 32}};
        EXPECT(dabs == expected);
        EXPECT(stroke.Active());

        // 同じセルに止まっている間は置かない(17 は 16 から間隔 2 より近い)
        EXPECT(stroke.Paint({}, dragged, false, radius2, false, FULL).empty());

        // 離すと終わる
        EXPECT(stroke.Paint({}, {}, true, radius2, false, FULL).empty());
        EXPECT(!stroke.Active());

        // 半径 0 は 1 セルずつ(隙間なし)
        BrushStroke single;
        const std::vector<BrushCell> line = single.Paint(pressed, dragged, true, {0, 0, 0}, false, FULL);
        EXPECT(line.size() == 8 && line.back() == (BrushCell{17, 20, 32}));
        EXPECT(!single.Active());
    }

    void TestAddDoesNotOverlap() {
        BrushStroke stroke;
        const BrushExtents box = {1, 3, 0};  // 直方体の半幅 x 1・y 3・z 0
        const std::vector<BrushCell> pressed = {{40, 10, 32}};
        const std::vector<BrushCell> alongX = {{52, 10, 32}};
        const std::vector<BrushCell> dabs = stroke.Paint(pressed, alongX, false, box, true, FULL);
        const std::vector<BrushCell> expected = {{40, 10, 32}, {43, 10, 32}, {46, 10, 32}, {49, 10, 32}, {52, 10, 32}};
        EXPECT(dabs == expected);  // x の向きは 2 × 1 + 1 = 3 ごと

        // y の向きは 2 × 3 + 1 = 7 ごと
        const std::vector<BrushCell> alongY = {{52, 30, 32}};
        const std::vector<BrushCell> down = stroke.Paint({}, alongY, true, box, true, FULL);
        const std::vector<BrushCell> expectedDown = {{52, 17, 32}, {52, 24, 32}};
        EXPECT(down == expectedDown);
        EXPECT(!stroke.Active());  // 30 は 24 から 7 より近いので置き終えた
    }

    void TestBudget() {
        EXPECT(BrushStroke::Budget(0) == FULL);
        EXPECT(BrushStroke::Budget(BrushStroke::MAX_WAITING_COMMANDS - 3) == 3);
        EXPECT(BrushStroke::Budget(BrushStroke::MAX_WAITING_COMMANDS) == 0);
        EXPECT(BrushStroke::Budget(BrushStroke::MAX_WAITING_COMMANDS + 100) == 0);

        // 置ききれない分は次のフレームに続きから(離した後も)
        BrushStroke stroke;
        const std::vector<BrushCell> pressed = {{0, 5, 5}};
        const std::vector<BrushCell> dragged = {{30, 5, 5}};
        const std::vector<BrushCell> first = stroke.Paint(pressed, dragged, true, {0, 0, 0}, false, 4);
        EXPECT(first.size() == 4 && first.back() == (BrushCell{3, 5, 5}));
        EXPECT(stroke.Active());

        const std::vector<BrushCell> second = stroke.Paint({}, {}, false, {0, 0, 0}, false, FULL);
        EXPECT(second.size() == FULL && second.front() == (BrushCell{4, 5, 5}) &&
               second.back() == (BrushCell{11, 5, 5}));

        size_t rest = 0;
        for (int frame = 0; frame < 10 && stroke.Active(); ++frame)
            rest += stroke.Paint({}, {}, false, {0, 0, 0}, false, FULL).size();

        EXPECT(rest == 30 - 11);
        EXPECT(!stroke.Active());

        // 置けない(待ちが溢れている)時に押しても始めない
        BrushStroke blocked;
        EXPECT(blocked.Paint(pressed, dragged, false, {0, 0, 0}, false, 0).empty());
        EXPECT(!blocked.Active());
    }

    void TestDiagonal() {
        BrushStroke stroke;
        const std::vector<BrushCell> pressed = {{10, 10, 10}};
        const std::vector<BrushCell> dragged = {{14, 12, 10}};
        const std::vector<BrushCell> dabs = stroke.Paint(pressed, dragged, true, {1, 1, 1}, false, FULL);

        // x がいちばん動く軸。y は残りの差から毎回比で割り、0 の方向へ切る: (11, 10)・(12, 10)・(13, 11)・(14, 12)
        const std::vector<BrushCell> expected = {{10, 10, 10}, {11, 10, 10}, {12, 10, 10}, {13, 11, 10}, {14, 12, 10}};
        EXPECT(dabs == expected);
        EXPECT(!dabs.empty() && dabs.back() == (BrushCell{14, 12, 10}));
        for (size_t i = 1; i < dabs.size(); ++i)
            EXPECT(dabs[i][0] == dabs[i - 1][0] + 1 && dabs[i][1] >= dabs[i - 1][1]);
    }

}  // namespace

int main() {
    TestReplaceSpacing();
    TestAddDoesNotOverlap();
    TestBudget();
    TestDiagonal();

    if (failureCount == 0)
        std::printf("brush_stroke: all passed\n");

    return failureCount == 0 ? 0 : 1;
}
