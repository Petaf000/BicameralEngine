// debug_camera.h — デバッグ表示のカメラ(格子の周りを回るカメラ)と、画面の点からの光線・断面のセルの拾い方(T-0015)。
//
// データの流れ: 入力(render/debug_view_controller)→ OrbitCamera の状態を変える → Basis() が描画の定数へ
//   (render/probe_view.hlsl が同じ式で画素ごとの光線を作る)。クリックは RayThroughPixel → PickSliceCell でセルにして、つつきのコマンドへ。
// カメラは View の状態(D-107)。世界には入らない。浮動小数点を使ってよい(描画側。04 §4 の検査の外)。
// 座標: 世界 = セルの番号の空間(格子は [0, 1 辺)³)。回る軸は −y(既定の向きで画面の右 = +x・上 = −y・奥 = +z。
//   今までの z = 32 の面の見え方と同じ)。
#pragma once

#include <cstdint>
#include <optional>

namespace bicameral::render {

    struct Vector3 {
        constexpr Vector3() = default;
        constexpr Vector3(float xValue, float yValue, float zValue) : x(xValue), y(yValue), z(zValue) {}

        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        [[nodiscard]] float operator[](uint32_t axis) const { return axis == 0 ? x : axis == 1 ? y : z; }
    };

    [[nodiscard]] Vector3 operator+(Vector3 left, Vector3 right);
    [[nodiscard]] Vector3 operator-(Vector3 left, Vector3 right);
    [[nodiscard]] Vector3 operator*(Vector3 vector, float scale);

    struct CameraRay {
        Vector3 origin;
        Vector3 direction;  // 長さは 1 とは限らない
    };

    // 描画に渡すカメラ(probe_view.hlsl と同じ意味)。画面の点 (u, v) ∈ [−1, 1]²(v は上が +)の光線は
    //   透視: 始点 = position、向き = forward + u × right + v × up
    //   平行: 始点 = position + u × right + v × up、向き = forward
    struct CameraBasis {
        Vector3 position;
        Vector3 forward;
        Vector3 right;  // 画面の半分の幅の分(透視は向きの傾き、平行は世界の長さ)
        Vector3 up;     // 画面の半分の高さの分
        bool orthographic = false;
    };

    struct OrbitCameraState {
        float yawDegrees = 35.0f;    // −y の軸の周りの向き(0 で +z を向く)
        float pitchDegrees = 25.0f;  // 上下(正で上から見下ろす)
        float distance = 150.0f;     // 注視点からの距離(セル)
        Vector3 target{32.0f, 32.0f, 32.0f};
        bool orthographic = false;
    };

    class OrbitCamera {
    public:
        static constexpr float VERTICAL_FIELD_OF_VIEW_DEGREES = 45.0f;

        explicit OrbitCamera(const OrbitCameraState& initial = {}) : m_initial(initial), m_state(initial) {}

        void Orbit(float deltaXPixels, float deltaYPixels);                         // 回る(右ドラッグ)
        void Pan(float deltaXPixels, float deltaYPixels, uint32_t viewportHeight);  // 平行移動(中ドラッグ)
        void Zoom(int32_t wheelSteps);                                              // 寄る(奥へ回すと近づく)
        void ToggleProjection() { m_state.orthographic = !m_state.orthographic; }
        void Reset() { m_state = m_initial; }

        [[nodiscard]] const OrbitCameraState& State() const { return m_state; }
        [[nodiscard]] CameraBasis Basis(uint32_t width, uint32_t height) const;

    private:
        OrbitCameraState m_initial;
        OrbitCameraState m_state;
    };

    // 画素 (pixelX, pixelY)(左上が 0。画素の中心は +0.5)を通る光線
    [[nodiscard]] CameraRay RayThroughPixel(const CameraBasis& basis, float pixelX, float pixelY, uint32_t width,
                                            uint32_t height);

    struct CellCoordinate {
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t z = 0;

        bool operator==(const CellCoordinate&) const = default;
    };

    // 断面(軸 axis = 0/1/2 の座標が plane のセルの面。面の真ん中 plane + 0.5 で交わりを取る)のうち、光線が当たるセル。
    // 面と平行・カメラの後ろ・格子の外なら無し
    [[nodiscard]] std::optional<CellCoordinate> PickSliceCell(const CameraRay& ray, uint32_t axis, uint32_t plane,
                                                              uint32_t gridSize);

    // 断面の上の (u, v) のセル(u, v は軸 axis 以外の 2 つの軸を x → y → z の順に)
    [[nodiscard]] CellCoordinate CellOnSlice(uint32_t axis, uint32_t plane, uint32_t u, uint32_t v);

}  // namespace bicameral::render
