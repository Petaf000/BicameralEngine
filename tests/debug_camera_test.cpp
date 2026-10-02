// debug_camera_test.cpp — render/debug_camera(カメラの光線・断面のセルを拾う)と render/debug_view_controller(入力の振り分け)を
// GPU なしで確かめる(T-0015)。
// 拾う試験: 断面のセルの中心を、カメラの基底から画面の画素へ写し(描画と同じ式の逆)、その画素から拾うと同じセルに戻るか。
// 失敗すると失敗した条件と行を表示して 1 を返す(ctest が落ちる)。
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

#include "core/singleton.h"
#include "render/debug_camera.h"
#include "render/debug_view_controller.h"

namespace {

    using namespace bicameral;
    using namespace bicameral::render;

    constexpr uint32_t GRID_SIZE = 64;
    constexpr uint32_t WIDTH = 1280;
    constexpr uint32_t HEIGHT = 720;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    float Dot(Vector3 left, Vector3 right) {
        return left.x * right.x + left.y * right.y + left.z * right.z;
    }

    struct Pixel {
        float x = 0.0f;
        float y = 0.0f;
    };

    // 世界の点 → 画面の画素(RayThroughPixel の逆。right・up・forward は直交している)。カメラの後ろ・画面の外なら無し
    std::optional<Pixel> Project(const CameraBasis& basis, Vector3 point) {
        const Vector3 offset = point - basis.position;
        const float depth = Dot(offset, basis.forward);
        if (depth <= 0.0f)
            return std::nullopt;

        const float scale = basis.orthographic ? 1.0f : depth;
        const float u = Dot(offset, basis.right) / Dot(basis.right, basis.right) / scale;
        const float v = Dot(offset, basis.up) / Dot(basis.up, basis.up) / scale;
        if (std::abs(u) >= 1.0f || std::abs(v) >= 1.0f)
            return std::nullopt;

        return Pixel{.x = (u + 1.0f) * 0.5f * WIDTH, .y = (1.0f - v) * 0.5f * HEIGHT};
    }

    Vector3 CellCenter(const CellCoordinate& cell) {
        return {static_cast<float>(cell.x) + 0.5f, static_cast<float>(cell.y) + 0.5f,
                static_cast<float>(cell.z) + 0.5f};
    }

    // --- カメラ ---

    // 既定の向き(0°・0°)で、画面の右 = +x・下 = +y・真ん中 = 注視点
    void TestFrontView() {
        const OrbitCamera camera({.yawDegrees = 0.0f, .pitchDegrees = 0.0f, .distance = 80.0f});
        const CameraBasis basis = camera.Basis(WIDTH, HEIGHT);
        const auto pick = [&](float x, float y) {
            return PickSliceCell(RayThroughPixel(basis, x, y, WIDTH, HEIGHT), 2, 32, GRID_SIZE);
        };

        const auto center = pick(WIDTH * 0.5f + 0.5f, HEIGHT * 0.5f + 0.5f);
        EXPECT(center && *center == (CellCoordinate{.x = 32, .y = 32, .z = 32}));
        const auto right = pick(WIDTH * 0.5f + 100.0f, HEIGHT * 0.5f);
        const auto below = pick(WIDTH * 0.5f, HEIGHT * 0.5f + 100.0f);
        EXPECT(right && right->x > 32 && right->y == 32);
        EXPECT(below && below->y > 32 && below->x == 32);
        EXPECT(!pick(2.0f, 2.0f));  // 画面の隅は格子の外(横長の画面の左端)
    }

    struct RoundTrip {
        uint32_t checked = 0;  // 画面に入ったセルの数
        uint32_t matched = 0;  // 拾い直して同じセルに戻った数
    };

    // 軸 axis の断面(位置 20)の全部のセルの中心を画素へ写し、その画素から拾い直す
    RoundTrip PickEverySliceCell(uint32_t axis, const OrbitCameraState& camera) {
        const CameraBasis basis = OrbitCamera(camera).Basis(WIDTH, HEIGHT);
        RoundTrip result;
        for (uint32_t u = 0; u < GRID_SIZE; ++u) {
            for (uint32_t v = 0; v < GRID_SIZE; ++v) {
                const CellCoordinate cell = CellOnSlice(axis, 20, u, v);
                const auto pixel = Project(basis, CellCenter(cell));
                if (!pixel)
                    continue;

                ++result.checked;
                const auto picked = PickSliceCell(RayThroughPixel(basis, pixel->x, pixel->y, WIDTH, HEIGHT), axis, 20,
                                                  GRID_SIZE);
                if (picked && *picked == cell)
                    ++result.matched;
            }
        }

        return result;
    }

    // 断面の全部のセルが、写して拾い直すと同じセルに戻る(透視・平行、3 つの軸、斜めのカメラ)
    void TestPickRoundTrip() {
        struct Case {
            uint32_t axis;
            OrbitCameraState camera;
        };

        const std::vector<Case> cases = {
            {.axis = 2, .camera = {.yawDegrees = 0.0f, .pitchDegrees = 0.0f, .distance = 80.0f}},
            {.axis = 2, .camera = {.yawDegrees = 25.0f, .pitchDegrees = 20.0f, .distance = 120.0f}},
            {.axis = 2,
             .camera = {.yawDegrees = -30.0f, .pitchDegrees = -15.0f, .distance = 120.0f, .orthographic = true}},
            {.axis = 0, .camera = {.yawDegrees = 70.0f, .pitchDegrees = 10.0f, .distance = 120.0f}},
            {.axis = 1, .camera = {.yawDegrees = 35.0f, .pitchDegrees = 60.0f, .distance = 150.0f}},
            {.axis = 1,
             .camera = {.yawDegrees = 35.0f, .pitchDegrees = -60.0f, .distance = 150.0f, .orthographic = true}},
        };

        for (const Case& test : cases) {
            const RoundTrip result = PickEverySliceCell(test.axis, test.camera);
            EXPECT(result.checked > GRID_SIZE * GRID_SIZE / 2);  // 断面の半分以上が画面に入る置き方
            EXPECT(result.matched == result.checked);
        }
    }

    // 面と平行・カメラの後ろ・範囲外の断面は拾わない
    void TestPickMisses() {
        const CameraRay parallel{.origin = {10.0f, 10.0f, -5.0f}, .direction = {1.0f, 0.0f, 0.0f}};
        EXPECT(!PickSliceCell(parallel, 2, 5, GRID_SIZE));
        const CameraRay away{.origin = {10.0f, 10.0f, -5.0f}, .direction = {0.0f, 0.0f, -1.0f}};
        EXPECT(!PickSliceCell(away, 2, 5, GRID_SIZE));
        const CameraRay toward{.origin = {10.0f, 10.0f, -5.0f}, .direction = {0.0f, 0.0f, 1.0f}};
        EXPECT(PickSliceCell(toward, 2, 5, GRID_SIZE) == (CellCoordinate{.x = 10, .y = 10, .z = 5}));
        EXPECT(!PickSliceCell(toward, 2, GRID_SIZE, GRID_SIZE));
        EXPECT(!PickSliceCell(toward, 3, 5, GRID_SIZE));
    }

    // 上下は ±89° で止まり、距離は範囲に収まり、Reset で最初に戻る
    void TestCameraLimits() {
        OrbitCamera camera;
        camera.Orbit(0.0f, 10000.0f);
        EXPECT(camera.State().pitchDegrees <= 89.0f);
        camera.Orbit(0.0f, -20000.0f);
        EXPECT(camera.State().pitchDegrees >= -89.0f);
        camera.Zoom(1000);
        EXPECT(camera.State().distance >= OrbitCamera::MIN_DISTANCE);
        camera.Zoom(-1000);
        EXPECT(camera.State().distance <= 2000.0f);
        camera.Pan(50.0f, 50.0f, HEIGHT);
        camera.Reset();
        EXPECT(camera.State().distance == OrbitCameraState{}.distance);
        EXPECT(camera.State().target.x == 32.0f && camera.State().target.y == 32.0f);
    }

    // --- 入力の振り分け ---

    InputEvent Key(uint32_t key, bool shift = false) {
        return {.kind = InputKind::KeyDown, .key = key, .shift = shift};
    }

    void TestController() {
        DebugViewController controller(GRID_SIZE, {}, {.yawDegrees = 0.0f, .pitchDegrees = 0.0f, .distance = 80.0f});
        const std::vector<InputEvent> keys = {Key('3'), Key('X'), Key('Q', true), Key('B'), Key('L')};
        EXPECT(controller.HandleInput(keys, WIDTH, HEIGHT).empty());
        const DebugViewSettings& settings = controller.Settings();
        EXPECT(settings.mode == DebugViewMode::Slice && settings.sliceAxis == 0 && settings.slicePosition == 24);
        EXPECT(!settings.showActiveBlocks && !settings.logarithmic);
        for (int index = 0; index < 10; ++index)
            (void)controller.HandleInput(std::vector{Key('Q', true)}, WIDTH, HEIGHT);

        EXPECT(controller.Settings().slicePosition == 0);
        for (int index = 0; index < 10; ++index)
            (void)controller.HandleInput(std::vector{Key('E', true)}, WIDTH, HEIGHT);

        EXPECT(controller.Settings().slicePosition == GRID_SIZE - 1);

        // T はトレースを頼むだけ(表示は変えない)。取ったら忘れる
        EXPECT(!controller.TakeTraceRequest());
        EXPECT(controller.HandleInput(std::vector{Key('T')}, WIDTH, HEIGHT).empty());
        EXPECT(controller.TakeTraceRequest() && !controller.TakeTraceRequest());
        EXPECT(controller.Settings().slicePosition == GRID_SIZE - 1);

        // z = 32 の断面で真ん中を左クリック → (32, 32, 32) をつつく。右ドラッグは回るだけ(つつかない)
        (void)controller.HandleInput(std::vector{Key('Z'), Key('Q', true), Key('Q', true), Key('Q', true)}, WIDTH,
                                     HEIGHT);
        EXPECT(controller.Settings().slicePosition == 39);
        DebugViewController front(GRID_SIZE, {.sliceAxis = 2, .slicePosition = 32},
                                  {.yawDegrees = 0.0f, .pitchDegrees = 0.0f, .distance = 80.0f});
        const std::vector<InputEvent> click = {
            {.kind = InputKind::ButtonDown, .button = PointerButton::Left, .x = WIDTH / 2, .y = HEIGHT / 2}};
        const auto pokes = front.HandleInput(click, WIDTH, HEIGHT);
        EXPECT(pokes.size() == 1 && pokes[0] == (CellCoordinate{.x = 32, .y = 32, .z = 32}));
        const std::vector<InputEvent> drag = {
            {.kind = InputKind::ButtonDown, .button = PointerButton::Right, .x = 100, .y = 100},
            {.kind = InputKind::PointerMove, .x = 200, .y = 100},
            {.kind = InputKind::ButtonUp, .button = PointerButton::Right, .x = 200, .y = 100},
            {.kind = InputKind::PointerMove, .x = 400, .y = 100},
        };

        EXPECT(front.HandleInput(drag, WIDTH, HEIGHT).empty());
        // 100 px × 0.3°(離した後の動きは効かない)
        EXPECT(std::abs(front.Camera().State().yawDegrees - -30.0f) < 1e-3f);

        const ProbeViewConstants constants = front.Constants(2, WIDTH, HEIGHT);
        EXPECT(constants.extractionIndex == 2 && constants.width == WIDTH && constants.sliceAxis == 2);
        EXPECT(constants.flags == (VIEW_FLAG_ACTIVE_BLOCKS | VIEW_FLAG_LOGARITHMIC));

        // 覗き窓(T-0096): P はポインタの下の断面のセルを覗く。PageDown で潜るとカメラが寄り、段が flags に入る。Shift + P でやめる
        const std::vector<InputEvent> peek = {
            {.kind = InputKind::PointerMove, .x = WIDTH / 2, .y = HEIGHT / 2}, Key('P'), Key(0x22), Key(0x22)};
        DebugViewController peeker(GRID_SIZE, {.sliceAxis = 2, .slicePosition = 32},
                                   {.yawDegrees = 0.0f, .pitchDegrees = 0.0f, .distance = 80.0f});
        EXPECT(peeker.HandleInput(peek, WIDTH, HEIGHT).empty());
        EXPECT(peeker.TakePeekChange() && !peeker.TakePeekChange());
        EXPECT(peeker.Peeking().peeking && peeker.Peeking().depth == 2 &&
               peeker.Peeking().cell == (CellCoordinate{.x = 32, .y = 32, .z = 32}));
        // 段 2 のブロック: 点 32 × 4 + 2 = 130 → 揃えて 128、真ん中 132 / 4 = 33
        EXPECT(std::abs(peeker.Camera().State().target.x - 33.0f) < 1e-4f &&
               std::abs(peeker.Camera().State().distance - 6.0f) < 1e-4f);
        EXPECT(((peeker.Constants(0, WIDTH, HEIGHT).flags >> VIEW_PEEK_DEPTH_SHIFT) & 15u) == 2);
        (void)peeker.HandleInput(std::vector{Key('P', true)}, WIDTH, HEIGHT);
        EXPECT(peeker.TakePeekChange() && !peeker.Peeking().peeking);

        DebugViewMode mode = DebugViewMode::Volume;
        EXPECT(ParseDebugViewMode("mip", mode) && mode == DebugViewMode::MaximumProjection);
        EXPECT(!ParseDebugViewMode("box", mode));
    }

}  // namespace

int main() {
    TestFrontView();
    TestPickRoundTrip();
    TestPickMisses();
    TestCameraLimits();
    TestController();
    if (failureCount == 0)
        std::printf("debug_camera_test: OK\n");

    SingletonFinalizer::Finalize();

    return failureCount == 0 ? 0 : 1;
}
