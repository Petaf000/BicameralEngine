// debug_camera.cpp — デバッグ表示のカメラ。考え方は debug_camera.h。
#include "render/debug_camera.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace bicameral::render {
    namespace {

        constexpr float ORBIT_DEGREES_PER_PIXEL = 0.3f;
        constexpr float MAX_PITCH_DEGREES = 89.0f;
        constexpr float ZOOM_PER_STEP = 0.9f;             // ホイール 1 刻みで距離を 0.9 倍
        constexpr float ORTHOGRAPHIC_BACK_OFF = 1000.0f;  // 平行投影の始点を下げる距離(格子の全部が前に来るように)
        constexpr float PARALLEL_EPSILON = 1e-6f;

        float Radians(float degrees) {
            return degrees * std::numbers::pi_v<float> / 180.0f;
        }

        Vector3 Cross(Vector3 left, Vector3 right) {
            return {left.y * right.z - left.z * right.y, left.z * right.x - left.x * right.z,
                    left.x * right.y - left.y * right.x};
        }

        Vector3 Normalize(Vector3 vector) {
            const float length = std::sqrt(vector.x * vector.x + vector.y * vector.y + vector.z * vector.z);
            return length > 0.0f ? vector * (1.0f / length) : vector;
        }

        // 画面の縦の半分が注視点の距離で何セルか
        float HalfHeightAtTarget(float distance) {
            return distance * std::tan(Radians(OrbitCamera::VERTICAL_FIELD_OF_VIEW_DEGREES) * 0.5f);
        }

        struct Axes {
            Vector3 forward;
            Vector3 right;
            Vector3 up;
        };

        // 回る軸は −y。forward = cos(pitch)·(sin(yaw), 0, cos(yaw)) + sin(pitch)·(0, 1, 0)
        // (pitch が正 = 上(−y)から見下ろす = 下(+y)を向く)
        Axes CameraAxes(const OrbitCameraState& state) {
            const float yaw = Radians(state.yawDegrees);
            const float pitch = Radians(state.pitchDegrees);
            const Vector3 forward{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
            const Vector3 worldUp{0.0f, -1.0f, 0.0f};
            const Vector3 right = Normalize(Cross(forward, worldUp));

            return {.forward = forward, .right = right, .up = Cross(right, forward)};
        }

    }  // namespace

    Vector3 operator+(Vector3 left, Vector3 right) {
        return {left.x + right.x, left.y + right.y, left.z + right.z};
    }

    Vector3 operator-(Vector3 left, Vector3 right) {
        return {left.x - right.x, left.y - right.y, left.z - right.z};
    }

    Vector3 operator*(Vector3 vector, float scale) {
        return {vector.x * scale, vector.y * scale, vector.z * scale};
    }

    // --- 操作 ---

    // 右へドラッグすると格子が右へ回る(カメラは左へ)。下へドラッグすると上から見下ろす
    void OrbitCamera::Orbit(float deltaXPixels, float deltaYPixels) {
        m_state.yawDegrees -= deltaXPixels * ORBIT_DEGREES_PER_PIXEL;
        m_state.yawDegrees = std::remainder(m_state.yawDegrees, 360.0f);
        m_state.pitchDegrees = std::clamp(m_state.pitchDegrees + deltaYPixels * ORBIT_DEGREES_PER_PIXEL,
                                          -MAX_PITCH_DEGREES, MAX_PITCH_DEGREES);
    }

    // 注視点の距離で、ドラッグした画素のぶんだけ中身がポインタについてくるように動かす
    void OrbitCamera::Pan(float deltaXPixels, float deltaYPixels, uint32_t viewportHeight) {
        if (viewportHeight == 0)
            return;

        const Axes axes = CameraAxes(m_state);
        const float cellsPerPixel = 2.0f * HalfHeightAtTarget(m_state.distance) / static_cast<float>(viewportHeight);
        m_state.target = m_state.target - axes.right * (deltaXPixels * cellsPerPixel) +
                         axes.up * (deltaYPixels * cellsPerPixel);
    }

    void OrbitCamera::Zoom(int32_t wheelSteps) {
        m_state.distance *= std::pow(ZOOM_PER_STEP, static_cast<float>(wheelSteps));
        m_state.distance = std::clamp(m_state.distance, MIN_DISTANCE, MAX_DISTANCE);
    }

    void OrbitCamera::Focus(Vector3 target, float distance) {
        m_state.target = target;
        m_state.distance = std::clamp(distance, MIN_DISTANCE, MAX_DISTANCE);
    }

    // --- 描画と拾うための形 ---

    CameraBasis OrbitCamera::Basis(uint32_t width, uint32_t height) const {
        const Axes axes = CameraAxes(m_state);
        const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
        if (m_state.orthographic) {
            // 注視点の距離の透視と同じ大きさに見える幅
            const float halfHeight = HalfHeightAtTarget(m_state.distance);

            return {.position = m_state.target - axes.forward * ORTHOGRAPHIC_BACK_OFF,
                    .forward = axes.forward,
                    .right = axes.right * (halfHeight * aspect),
                    .up = axes.up * halfHeight,
                    .orthographic = true};
        }

        const float halfHeight = std::tan(Radians(VERTICAL_FIELD_OF_VIEW_DEGREES) * 0.5f);

        return {.position = m_state.target - axes.forward * m_state.distance,
                .forward = axes.forward,
                .right = axes.right * (halfHeight * aspect),
                .up = axes.up * halfHeight,
                .orthographic = false};
    }

    CameraRay RayThroughPixel(const CameraBasis& basis, float pixelX, float pixelY, uint32_t width, uint32_t height) {
        const float u = width > 0 ? 2.0f * pixelX / static_cast<float>(width) - 1.0f : 0.0f;
        const float v = height > 0 ? 1.0f - 2.0f * pixelY / static_cast<float>(height) : 0.0f;
        const Vector3 offset = basis.right * u + basis.up * v;
        if (basis.orthographic)
            return {.origin = basis.position + offset, .direction = basis.forward};

        return {.origin = basis.position, .direction = basis.forward + offset};
    }

    std::optional<CellCoordinate> PickSliceCell(const CameraRay& ray, uint32_t axis, uint32_t plane,
                                                uint32_t gridSize) {
        if (axis > 2 || plane >= gridSize)
            return std::nullopt;

        const float along = ray.direction[axis];
        if (std::abs(along) < PARALLEL_EPSILON)
            return std::nullopt;

        const float distance = (static_cast<float>(plane) + 0.5f - ray.origin[axis]) / along;
        if (distance < 0.0f)
            return std::nullopt;

        const Vector3 hit = ray.origin + ray.direction * distance;

        // 断面の軸は plane そのもの。他の 2 つの軸は格子の中なら切り捨て
        std::array<uint32_t, 3> cell{};
        for (uint32_t index = 0; index < 3; ++index) {
            if (index == axis) {
                cell[index] = plane;
                continue;
            }

            const float value = std::floor(hit[index]);
            // NaN も外
            if (!(value >= 0.0f && value < static_cast<float>(gridSize)))
                return std::nullopt;

            cell[index] = static_cast<uint32_t>(value);
        }

        return CellCoordinate{.x = cell[0], .y = cell[1], .z = cell[2]};
    }

    CellCoordinate CellOnSlice(uint32_t axis, uint32_t plane, uint32_t u, uint32_t v) {
        if (axis == 0)
            return {.x = plane, .y = u, .z = v};

        if (axis == 1)
            return {.x = u, .y = plane, .z = v};

        return {.x = u, .y = v, .z = plane};
    }

}  // namespace bicameral::render
