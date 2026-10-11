// surface_pick.cpp — 光線が最初に当たる物のセルの手前のセル(T-0332。説明は surface_pick.h)。
#include "editor/surface_pick.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace bicameral::editor {

    namespace {

        using Vector = std::array<float, 3>;

        constexpr float INFINITE_DISTANCE = std::numeric_limits<float>::infinity();

        Vector ToArray(const render::Vector3& value) {
            return {value.x, value.y, value.z};
        }

        // 光線が箱 [0, N]³ の中にある t の範囲(始点より前は切る)。通らなければ無し
        std::optional<std::array<float, 2>> ClipToGrid(const Vector& origin, const Vector& direction, float size) {
            float enter = 0.0f;
            float leave = INFINITE_DISTANCE;
            for (size_t axis = 0; axis < 3; ++axis) {
                if (direction[axis] == 0.0f) {
                    if (origin[axis] < 0.0f || origin[axis] >= size)
                        return std::nullopt;

                    continue;
                }

                const float entryDistance = (0.0f - origin[axis]) / direction[axis];
                const float exitDistance = (size - origin[axis]) / direction[axis];
                enter = std::max(enter, std::min(entryDistance, exitDistance));
                leave = std::min(leave, std::max(entryDistance, exitDistance));
            }

            if (enter >= leave)
                return std::nullopt;

            return std::array<float, 2>{enter, leave};
        }

    }  // namespace

    bool IsSurfaceSolid(std::span<const uint32_t> words, uint32_t gridSize, const render::CellCoordinate& cell) {
        const uint64_t index = cell.x + (uint64_t{gridSize} * cell.y) + (uint64_t{gridSize} * gridSize * cell.z);
        if (index / 32 >= words.size())
            return false;

        return ((words[index / 32] >> (index % 32)) & 1u) != 0;
    }

    std::optional<SurfaceHit> PickSurfaceCell(const render::CameraRay& ray, std::span<const uint32_t> words,
                                              uint32_t gridSize) {
        if (words.empty() || gridSize == 0)
            return std::nullopt;

        const Vector origin = ToArray(ray.origin);
        const Vector direction = ToArray(ray.direction);
        const auto size = static_cast<float>(gridSize);
        const auto range = ClipToGrid(origin, direction, size);
        if (!range)
            return std::nullopt;

        // --- 格子に入った点のセルと、軸ごとの次の面までの t・1 セルの t(Amanatides–Woo)---
        const float start = (*range)[0];
        std::array<int32_t, 3> cell{};
        std::array<int32_t, 3> step{};
        Vector next{};
        Vector delta{};
        for (size_t axis = 0; axis < 3; ++axis) {
            const float position = origin[axis] + (direction[axis] * start);
            cell[axis] = std::clamp(static_cast<int32_t>(std::floor(position)), 0, static_cast<int32_t>(gridSize) - 1);
            step[axis] = direction[axis] > 0.0f ? 1 : (direction[axis] < 0.0f ? -1 : 0);
            if (step[axis] == 0) {
                next[axis] = INFINITE_DISTANCE;
                delta[axis] = INFINITE_DISTANCE;
                continue;
            }

            const float boundary = static_cast<float>(cell[axis] + (step[axis] > 0 ? 1 : 0));
            next[axis] = (boundary - origin[axis]) / direction[axis];
            delta[axis] = std::abs(1.0f / direction[axis]);
        }

        // --- 1 セルずつ進む: 物のセルに入ったら、その前のセルと越えた面の軸 ---
        const auto toCell = [](const std::array<int32_t, 3>& value) {
            return render::CellCoordinate{.x = static_cast<uint32_t>(value[0]),
                                          .y = static_cast<uint32_t>(value[1]),
                                          .z = static_cast<uint32_t>(value[2])};
        };
        if (IsSurfaceSolid(words, gridSize, toCell(cell)))
            return std::nullopt;  // 格子に入った最初のセルが物(手前に置けるセルが無い)

        const uint32_t maxSteps = 3 * gridSize;
        for (uint32_t i = 0; i < maxSteps; ++i) {
            const auto axis = static_cast<size_t>(std::ranges::min_element(next) - next.begin());
            const std::array<int32_t, 3> previous = cell;
            cell[axis] += step[axis];
            if (cell[axis] < 0 || cell[axis] >= static_cast<int32_t>(gridSize))
                return std::nullopt;  // 物に当たらずに格子を出た

            if (IsSurfaceSolid(words, gridSize, toCell(cell)))
                return SurfaceHit{.cell = toCell(previous), .normalAxis = static_cast<uint32_t>(axis)};

            next[axis] += delta[axis];
        }

        return std::nullopt;
    }

}  // namespace bicameral::editor
