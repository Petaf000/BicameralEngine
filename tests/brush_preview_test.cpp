// brush_preview_test.cpp — editor/brush_preview(カーソルの下に描く筆の形の線。T-0279)と、それが使う
// render/debug_camera の ProjectToPixel・render/debug_view_controller の CursorCell を CPU だけで確かめる。
// 確かめること: 断面の上で覆うセルの数と縁の数が、形の式(球・直方体・円柱)から数えた値と同じ・縁は断面の真ん中の面の上・
// 形を囲む箱は 12 本で格子の中に切る・断面が形に掛からなければ箱だけ・読めない形は何も描かない・
// ProjectToPixel は RayThroughPixel の逆(透視・平行)でカメラの後ろは写さない・CursorCell は最後のポインタの画素の光線が
// 断面に当たるセル(回している間は無し)。失敗すると条件と行を表示して 1 を返す(ctest が落ちる)。
#include "editor/brush_preview.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "common/probe_place.hlsli"
#include "platform/input.h"
#include "render/debug_view_controller.h"

namespace {

    using namespace bicameral;
    using editor::BrushPreview;
    using editor::BuildBrushPreview;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr uint32_t GRID = sim::PROBE_GRID_SIZE;
    constexpr uint32_t BOX_EDGES = 12;

    // 置くコマンドの語(common/probe_place.hlsli の約束。材料は 0 個〔形だけ見る〕)
    std::vector<uint32_t> Payload(uint32_t x, uint32_t y, uint32_t z, uint32_t kind, uint32_t radius,
                                  uint32_t size2 = 0, uint32_t size3 = 0) {
        std::vector<uint32_t> words(sim::PROBE_PLACE_PAYLOAD_WORDS, 0);
        words[sim::PROBE_PLACE_WORD_CENTER] = x | (y << 8) | (z << 16) | (size2 << 24) | (size3 << 28);
        words[sim::PROBE_PLACE_WORD_SHAPE] = radius | (kind << 24);
        return words;
    }

    uint32_t CountSliceSegments(const BrushPreview& preview) {
        uint32_t count = 0;
        for (const editor::BrushPreviewSegment& segment : preview.segments)
            count += segment.onSlice ? 1 : 0;

        return count;
    }

    // 断面の上の (u, v) の覆うかの式 covers から、覆うセルの数と縁(隣が覆われていない辺)の数を数える
    template <class Covers>
    void CountExpected(Covers covers, int32_t lo, int32_t hi, uint32_t& cells, uint32_t& edges) {
        cells = 0;
        edges = 0;
        for (int32_t v = lo; v <= hi; ++v) {
            for (int32_t u = lo; u <= hi; ++u) {
                if (!covers(u, v))
                    continue;

                ++cells;
                edges += covers(u - 1, v) ? 0 : 1;
                edges += covers(u + 1, v) ? 0 : 1;
                edges += covers(u, v - 1) ? 0 : 1;
                edges += covers(u, v + 1) ? 0 : 1;
            }
        }
    }

    void TestSphere() {
        const auto payload = Payload(10, 20, 32, sim::PROBE_PLACE_SHAPE_SPHERE, 2);
        const BrushPreview center = BuildBrushPreview(payload, 2, 32);
        uint32_t cells = 0;
        uint32_t edges = 0;
        CountExpected([](int32_t u, int32_t v) { return u * u + v * v <= 4; }, -4, 4, cells, edges);
        EXPECT(cells == 13);
        EXPECT(center.sliceCells == cells);
        EXPECT(CountSliceSegments(center) == edges);
        EXPECT(center.segments.size() == edges + BOX_EDGES);
        for (const editor::BrushPreviewSegment& segment : center.segments) {
            if (segment.onSlice)
                EXPECT(segment.from.z == 32.5f && segment.to.z == 32.5f);
            // 形を囲む箱は x ∈ [8, 13]・y ∈ [18, 23]・z ∈ [30, 35]
            EXPECT(segment.from.x >= 8.0f && segment.to.x <= 13.0f && segment.from.y >= 18.0f && segment.to.y <= 23.0f);
        }

        // 端の面は中心の 1 セルだけ・形の外の面は箱だけ
        const BrushPreview edge = BuildBrushPreview(payload, 2, 34);
        EXPECT(edge.sliceCells == 1 && CountSliceSegments(edge) == 4);
        const BrushPreview outside = BuildBrushPreview(payload, 2, 35);
        EXPECT(outside.sliceCells == 0 && outside.segments.size() == BOX_EDGES);
    }

    void TestBox() {
        // 半幅 x 2・y 1・z 3。x の断面(x = 31)は y × z = 3 × 7
        const auto payload = Payload(30, 30, 30, sim::PROBE_PLACE_SHAPE_BOX, 2, 1, 3);
        const BrushPreview preview = BuildBrushPreview(payload, 0, 31);
        EXPECT(preview.sliceCells == 21);
        EXPECT(CountSliceSegments(preview) == 2 * (3 + 7));
        for (const editor::BrushPreviewSegment& segment : preview.segments) {
            if (segment.onSlice)
                EXPECT(segment.from.x == 31.5f && segment.to.x == 31.5f);
        }

        EXPECT(BuildBrushPreview(payload, 0, 33).sliceCells == 0);
        EXPECT(BuildBrushPreview(payload, 1, 31).sliceCells == 5 * 7);  // y の断面は x × z
    }

    void TestCylinder() {
        // 軸 y・半径 2・軸の向きの半幅 3
        const auto payload = Payload(30, 30, 30, sim::PROBE_PLACE_SHAPE_CYLINDER, 2, 3, 1);
        uint32_t cells = 0;
        uint32_t edges = 0;
        CountExpected([](int32_t u, int32_t v) { return u * u + v * v <= 4; }, -4, 4, cells, edges);
        const BrushPreview across = BuildBrushPreview(payload, 1, 30);  // 軸に垂直な断面は円
        EXPECT(across.sliceCells == cells && CountSliceSegments(across) == edges);
        EXPECT(BuildBrushPreview(payload, 0, 30).sliceCells == 5 * 7);  // 軸を含む断面は長方形
        EXPECT(BuildBrushPreview(payload, 0, 32).sliceCells == 1 * 7);  // 縁をかすめる断面は 1 列
        EXPECT(BuildBrushPreview(payload, 1, 33).sliceCells == cells);
        EXPECT(BuildBrushPreview(payload, 1, 34).sliceCells == 0);
    }

    void TestClipped() {
        // 格子の隅の球: 格子の中の 1/4 の円だけ。箱は [0, 3)
        const BrushPreview preview = BuildBrushPreview(Payload(0, 0, 0, sim::PROBE_PLACE_SHAPE_SPHERE, 2), 2, 0);
        EXPECT(preview.sliceCells == 6);
        for (const editor::BrushPreviewSegment& segment : preview.segments) {
            EXPECT(segment.from.x >= 0.0f && segment.from.y >= 0.0f && segment.to.x <= 3.0f && segment.to.y <= 3.0f);
        }

        const BrushPreview far = BuildBrushPreview(Payload(GRID - 1, GRID - 1, 5, sim::PROBE_PLACE_SHAPE_BOX, 3, 3, 0),
                                                   2, 5);
        EXPECT(far.sliceCells == 4 * 4);
    }

    void TestUnreadable() {
        EXPECT(BuildBrushPreview(Payload(0xFF, 0, 0, sim::PROBE_PLACE_SHAPE_SPHERE, 2), 2, 0).segments.empty());
        EXPECT(BuildBrushPreview(Payload(5, 5, 5, sim::PROBE_PLACE_SHAPE_SPHERE, sim::PROBE_PLACE_MAX_RADIUS + 1), 2, 5)
                   .segments.empty());
        EXPECT(BuildBrushPreview(Payload(5, 5, 5, 3, 2), 2, 5).segments.empty());  // 知らない形
        EXPECT(BuildBrushPreview(Payload(5, 5, 5, sim::PROBE_PLACE_SHAPE_SPHERE, 2), 3, 5).segments.empty());
    }

    void TestProjection() {
        constexpr uint32_t WIDTH = 800;
        constexpr uint32_t HEIGHT = 600;
        for (const bool orthographic : {false, true}) {
            render::OrbitCamera camera({.orthographic = orthographic});
            const render::CameraBasis basis = camera.Basis(WIDTH, HEIGHT);
            for (const auto& pixel : {std::array<float, 2>{400.0f, 300.0f}, std::array<float, 2>{13.5f, 580.25f},
                                      std::array<float, 2>{790.0f, 7.0f}}) {
                const render::CameraRay ray = render::RayThroughPixel(basis, pixel[0], pixel[1], WIDTH, HEIGHT);
                const render::Vector3 point = ray.origin + ray.direction * 120.0f;
                const auto projected = render::ProjectToPixel(basis, point, WIDTH, HEIGHT);
                EXPECT(projected.has_value());
                if (projected)
                    EXPECT(std::abs((*projected)[0] - pixel[0]) < 0.05f &&
                           std::abs((*projected)[1] - pixel[1]) < 0.05f);
            }
        }

        // 透視のカメラの後ろは写さない
        render::OrbitCamera camera;
        const render::CameraBasis basis = camera.Basis(WIDTH, HEIGHT);
        EXPECT(!render::ProjectToPixel(basis, basis.position - basis.forward * 5.0f, WIDTH, HEIGHT).has_value());

        // 既定のカメラは格子の全部が見えるので、真ん中の筆の線は全部画面の中
        const BrushPreview preview = BuildBrushPreview(
            Payload(GRID / 2, GRID / 2, GRID / 2, sim::PROBE_PLACE_SHAPE_SPHERE, 3), 2, GRID / 2);
        const auto lines = editor::ProjectBrushPreview(preview, basis, WIDTH, HEIGHT);
        EXPECT(lines.size() == preview.segments.size());
        for (const editor::BrushPreviewLine& line : lines) {
            EXPECT(line.from[0] >= 0.0f && line.from[0] <= static_cast<float>(WIDTH) && line.from[1] >= 0.0f &&
                   line.from[1] <= static_cast<float>(HEIGHT));
        }
    }

    void TestCursorCell() {
        constexpr uint32_t WIDTH = 800;
        constexpr uint32_t HEIGHT = 600;
        render::DebugViewController controller(GRID, {}, {});
        EXPECT(!controller.CursorCell(WIDTH, HEIGHT).has_value());  // ポインタがまだ来ていない

        const std::vector<InputEvent> move = {{.kind = InputKind::PointerMove, .x = 410, .y = 290}};
        (void)controller.HandleInput(move, WIDTH, HEIGHT);
        const render::CameraRay ray = render::RayThroughPixel(controller.Camera().Basis(WIDTH, HEIGHT), 410.5f, 290.5f,
                                                              WIDTH, HEIGHT);
        const auto expected = render::PickSliceCell(ray, controller.Settings().sliceAxis,
                                                    controller.Settings().slicePosition, GRID);
        EXPECT(expected.has_value());
        EXPECT(controller.CursorCell(WIDTH, HEIGHT) == expected);

        // 断面を動かすと付いてくる(E = 断面を +1)
        const std::vector<InputEvent> key = {{.kind = InputKind::KeyDown, .key = 'E'}};
        (void)controller.HandleInput(key, WIDTH, HEIGHT);
        const auto moved = controller.CursorCell(WIDTH, HEIGHT);
        EXPECT(moved.has_value() && moved->z == controller.Settings().slicePosition);

        // 右ドラッグで回している間は描かない
        const std::vector<InputEvent> rotate = {
            {.kind = InputKind::ButtonDown, .button = PointerButton::Right, .x = 410, .y = 290}};
        (void)controller.HandleInput(rotate, WIDTH, HEIGHT);
        EXPECT(!controller.CursorCell(WIDTH, HEIGHT).has_value());
    }

}  // namespace

int main() {
    TestSphere();
    TestBox();
    TestCylinder();
    TestClipped();
    TestUnreadable();
    TestProjection();
    TestCursorCell();

    if (failureCount != 0) {
        std::printf("brush_preview_test: %d failed\n", failureCount);
        return 1;
    }

    std::printf("brush_preview_test: OK\n");
    return 0;
}
