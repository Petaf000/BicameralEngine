// debug_view_controller.h — デバッグ表示の操作: 窓の入力を、カメラ・表示の切り替え・つつくセルに振り分ける(T-0015)。
//
// データの流れ: platform/window の入力のイベント → HandleInput → カメラと表示の設定(View の状態。世界には入らない)
//   + つつくセル(フレームのループがコマンドにする。06 §3)→ Constants() が描画の定数(render/probe_view_constants.h)を作る。
// 操作: 左クリック = つつく / Shift + 左クリック = 光線の先の物を押す(T-0098)/ 右ドラッグ = 回る / 中ドラッグ = 平行移動 / ホイール = 寄る / 1・2・3 = 表示 / X・Y・Z = 断面の軸 /
//   Q・E = 断面を動かす(Shift で 8) / B = 活性なブロック / L = 対数・線形 / C = 色分けする量(温度・O2 の減り・CO2・炭)/
//   O = 透視・平行 / R = カメラを戻す /
//   T = 連鎖のトレースを頼む(どこを何刻みかはフレームのループが決める。T-0088)/
//   P = カーソルの下の断面のセルを覗く(覗き窓。T-0096)/ Shift + P = やめる / PageDown・PageUp = 潜る・浮かぶ(カメラが点に寄る)。
// 設定が変わったらログに出す(画面に文字はまだ無い。12 §5)。
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "platform/input.h"
#include "render/debug_camera.h"
#include "render/probe_view_constants.h"

namespace bicameral::render {

    // 覗き窓の段の数(sim/probe_peek.h の PEEK_LEVEL_COUNT。frame_loop.cpp が static_assert で揃える)
    inline constexpr uint32_t PEEK_MAX_DEPTH = 9;

    // 覗き窓(View の状態。世界には入らない。T-0096)
    struct PeekView {
        bool peeking = false;
        CellCoordinate cell;  // 覗いている世界のセル
        uint32_t depth = 0;   // 潜っている段(0〜PEEK_MAX_DEPTH。カメラの寄り方と枠の色だけ)

        bool operator==(const PeekView&) const = default;
    };

    struct DebugViewSettings {
        DebugViewMode mode = DebugViewMode::Volume;
        uint32_t sliceAxis = 2;       // 0 = x・1 = y・2 = z
        uint32_t slicePosition = 32;  // 断面のセルの番号(クリックがつつく面)
        bool showActiveBlocks = true;
        bool logarithmic = true;
        DebugViewQuantity quantity = DebugViewQuantity::Temperature;
    };

    class DebugViewController {
    public:
        DebugViewController(uint32_t gridSize, const DebugViewSettings& settings, const OrbitCameraState& camera);

        // 入力を振り分け、つつくセル(左クリックの光線が断面に当たった所。古い順)を返す。width・height は描く大きさ(px)
        [[nodiscard]] std::vector<CellCoordinate> HandleInput(std::span<const InputEvent> events, uint32_t width,
                                                              uint32_t height);

        [[nodiscard]] ProbeViewConstants Constants(uint32_t extractionIndex, uint32_t width, uint32_t height) const;
        [[nodiscard]] const DebugViewSettings& Settings() const { return m_settings; }
        [[nodiscard]] const OrbitCamera& Camera() const { return m_camera; }
        [[nodiscard]] std::string Describe() const;  // 今の設定を 1 行で(ログ用)

        // 前に呼んでから T が押されたか(押されていたら true を返して忘れる)
        [[nodiscard]] bool TakeTraceRequest();

        // 前に呼んでから Shift + 左クリックした画素の光線(格子の座標。古い順。取ったら忘れる。T-0098)
        [[nodiscard]] std::vector<CameraRay> TakePushRays();

        // --- 覗き窓(T-0096)---
        // cell を覗いて depth まで潜る(--peek。窓では P と PageDown・PageUp)
        void Peek(CellCoordinate cell, uint32_t depth);
        [[nodiscard]] const PeekView& Peeking() const { return m_peek; }
        // 前に呼んでから覗く場所(覗いているか・セル)が変わったか(変わっていたら true を返して忘れる。潜る段だけの変化は入らない)
        [[nodiscard]] bool TakePeekChange();

    private:
        void HandlePointer(const InputEvent& event, uint32_t width, uint32_t height,
                           std::vector<CellCoordinate>& pokes);
        void HandleKey(const InputEvent& event, uint32_t width, uint32_t height);
        void HandlePeekKey(const InputEvent& event, uint32_t width, uint32_t height);
        void FocusOnPeek();

        uint32_t m_gridSize;  // 格子の 1 辺のセルの数
        DebugViewSettings m_settings;
        OrbitCamera m_camera;

        // --- ドラッグ中の状態 ---
        bool m_rotating = false;
        bool m_panning = false;
        int32_t m_lastX = 0;  // 前のポインタの位置(画素)
        int32_t m_lastY = 0;

        bool m_traceRequested = false;      // T が押された(TakeTraceRequest で取る)
        std::vector<CameraRay> m_pushRays;  // Shift + 左クリックの光線(TakePushRays で取る)

        PeekView m_peek;
        bool m_peekChanged = false;  // 覗く場所が変わった(TakePeekChange で取る)
    };

    // 表示の名前(--view の値。volume・mip・slice)→ 表示。知らない名前なら false
    [[nodiscard]] bool ParseDebugViewMode(std::string_view name, DebugViewMode& mode);

}  // namespace bicameral::render
