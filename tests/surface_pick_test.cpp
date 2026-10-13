// surface_pick_test.cpp — editor/surface_pick(光線が最初に当たる物のセルの手前のセル。T-0332)を CPU だけで確かめる。
// 確かめること: 初めの世界と同じ木箱(中央 8³・壁 1 セル)に軸に沿った光線で当てると壁の外に接するセルと面の軸・格子の外から入る光線・
// 斜めの光線は細かく刻んで歩いた答えと同じ・印が空・物に当たらない・格子に当たらない・最初のセルが物なら無し・
// render の DebugViewController に拾い方(SetCellPicker)を渡すとカーソルとドラッグのセルがそれに従い、拾えなければ断面のセル。
// 失敗すると条件と行を表示して 1 を返す(ctest が落ちる)。
#include "editor/surface_pick.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

#include "platform/input.h"
#include "render/debug_view_controller.h"

namespace {

    using namespace bicameral;
    using editor::PickSurfaceCell;
    using render::CameraRay;
    using render::CellCoordinate;
    using render::Vector3;

    int failureCount = 0;

    void Expect(bool condition, const char* text, int line) {
        if (condition)
            return;

        std::printf("FAILED line %d: %s\n", line, text);
        ++failureCount;
    }

#define EXPECT(condition) Expect((condition), #condition, __LINE__)

    constexpr uint32_t GRID = 64;
    constexpr uint32_t CRATE_BEGIN = 28;  // 試験のパッケージの world_cells.luau の木箱(28〜35)
    constexpr uint32_t CRATE_END = 36;

    void SetSolid(std::vector<uint32_t>& words, uint32_t x, uint32_t y, uint32_t z) {
        const uint32_t index = x + (GRID * y) + (GRID * GRID * z);
        words[index / 32] |= 1u << (index % 32);
    }

    std::vector<uint32_t> CrateMask() {
        std::vector<uint32_t> words(GRID * GRID * GRID / 32, 0);
        const auto inside = [](uint32_t value) {
            return value >= CRATE_BEGIN && value < CRATE_END;
        };
        const auto wall = [](uint32_t value) {
            return value == CRATE_BEGIN || value == CRATE_END - 1;
        };
        for (uint32_t z = 0; z < GRID; ++z) {
            for (uint32_t y = 0; y < GRID; ++y) {
                for (uint32_t x = 0; x < GRID; ++x) {
                    if (inside(x) && inside(y) && inside(z) && (wall(x) || wall(y) || wall(z)))
                        SetSolid(words, x, y, z);
                }
            }
        }

        return words;
    }

    // 答え合わせ: 光線を細かく刻んで歩き、最初に物のセルに入った時の前のセル
    std::optional<CellCoordinate> WalkReference(const CameraRay& ray, const std::vector<uint32_t>& words) {
        std::optional<CellCoordinate> previous;
        for (int i = 0; i < 400000; ++i) {
            const float t = static_cast<float>(i) * 0.001f;
            const float x = ray.origin.x + (ray.direction.x * t);
            const float y = ray.origin.y + (ray.direction.y * t);
            const float z = ray.origin.z + (ray.direction.z * t);
            if (x < 0.0f || y < 0.0f || z < 0.0f || x >= GRID || y >= GRID || z >= GRID) {
                if (previous)
                    return std::nullopt;  // 入って出た

                continue;
            }

            const CellCoordinate cell{
                .x = static_cast<uint32_t>(x), .y = static_cast<uint32_t>(y), .z = static_cast<uint32_t>(z)};
            if (editor::IsSurfaceSolid(words, GRID, cell))
                return previous;

            previous = cell;
        }

        return std::nullopt;
    }

    void TestCrateAxis() {
        const std::vector<uint32_t> words = CrateMask();

        // +x の側から -x へ: 壁 x = 35 の外の x = 36
        const auto east = PickSurfaceCell({.origin = {60.5f, 32.5f, 30.5f}, .direction = {-1.0f, 0.0f, 0.0f}}, words,
                                          GRID);
        EXPECT(east && east->cell == (CellCoordinate{.x = 36, .y = 32, .z = 30}) && east->normalAxis == 0);

        // 格子の外(-x)から +x へ: x = 27
        const auto west = PickSurfaceCell({.origin = {-10.0f, 30.5f, 33.5f}, .direction = {2.0f, 0.0f, 0.0f}}, words,
                                          GRID);
        EXPECT(west && west->cell == (CellCoordinate{.x = 27, .y = 30, .z = 33}) && west->normalAxis == 0);

        // 上から下へ: 壁 y = 35 の上の y = 36、面の軸 1
        const auto top = PickSurfaceCell({.origin = {31.5f, 80.0f, 29.5f}, .direction = {0.0f, -1.0f, 0.0f}}, words,
                                         GRID);
        EXPECT(top && top->cell == (CellCoordinate{.x = 31, .y = 36, .z = 29}) && top->normalAxis == 1);
    }

    void TestDiagonal() {
        const std::vector<uint32_t> words = CrateMask();
        const CameraRay rays[] = {
            {.origin = {5.3f, 50.2f, 7.7f}, .direction = {0.61f, -0.43f, 0.57f}},
            {.origin = {70.0f, 20.0f, 64.5f}, .direction = {-0.7f, 0.25f, -0.66f}},
            {.origin = {33.1f, 1.2f, 2.9f}, .direction = {-0.05f, 0.62f, 0.78f}},
        };
        for (const CameraRay& ray : rays) {
            const auto hit = PickSurfaceCell(ray, words, GRID);
            const auto expected = WalkReference(ray, words);
            EXPECT(hit.has_value() == expected.has_value());
            if (hit && expected) {
                EXPECT(hit->cell == *expected);
                EXPECT(!editor::IsSurfaceSolid(words, GRID, hit->cell));
            }
        }
    }

    void TestMisses() {
        const std::vector<uint32_t> words = CrateMask();
        const std::vector<uint32_t> empty(GRID * GRID * GRID / 32, 0);

        // 印が空・物に当たらない・格子に当たらない
        const CameraRay axis{.origin = {60.5f, 32.5f, 30.5f}, .direction = {-1.0f, 0.0f, 0.0f}};
        EXPECT(!PickSurfaceCell(axis, empty, GRID));
        EXPECT(!PickSurfaceCell(axis, {}, GRID));
        EXPECT(!PickSurfaceCell({.origin = {60.5f, 10.5f, 30.5f}, .direction = {-1.0f, 0.0f, 0.0f}}, words, GRID));
        EXPECT(!PickSurfaceCell({.origin = {80.0f, 80.0f, 80.0f}, .direction = {1.0f, 0.0f, 0.0f}}, words, GRID));

        // 最初のセルが物(壁の中から)
        EXPECT(!PickSurfaceCell({.origin = {35.5f, 32.5f, 30.5f}, .direction = {1.0f, 0.0f, 0.0f}}, words, GRID));
    }

    // DebugViewController に拾い方を渡すと、カーソルとドラッグのセルがそれに従う(拾えなければ断面)
    void TestControllerPicker() {
        render::DebugViewSettings settings;
        settings.sliceAxis = 2;
        settings.slicePosition = 32;
        render::DebugViewController controller(GRID, settings, {});
        const std::vector<InputEvent> move = {{.kind = InputKind::PointerMove, .x = 640, .y = 360}};
        (void)controller.HandleInput(move, 1280, 720);
        const auto slice = controller.CursorCell(1280, 720);
        EXPECT(slice.has_value());

        const CellCoordinate picked{.x = 1, .y = 2, .z = 3};
        controller.SetCellPicker([&](const CameraRay&) { return std::optional<CellCoordinate>(picked); });
        EXPECT(controller.CursorCell(1280, 720) == picked);
        EXPECT(controller.CursorRay(1280, 720).has_value());

        const std::vector<InputEvent> press = {
            {.kind = InputKind::ButtonDown, .button = PointerButton::Left, .x = 640, .y = 360}};
        const auto pokes = controller.HandleInput(press, 1280, 720);
        EXPECT(pokes.size() == 1 && pokes.front() == picked);

        controller.SetCellPicker([](const CameraRay&) { return std::optional<CellCoordinate>(); });
        EXPECT(controller.CursorCell(1280, 720) == slice);

        controller.SetCellPicker({});
        EXPECT(controller.CursorCell(1280, 720) == slice);
    }

}  // namespace

int main() {
    TestCrateAxis();
    TestDiagonal();
    TestMisses();
    TestControllerPicker();

    if (failureCount != 0) {
        std::printf("surface_pick_test: %d failed\n", failureCount);
        return 1;
    }

    std::printf("surface_pick_test: OK\n");
    return 0;
}
