// debug_view_controller.h — デバッグ表示の操作: 窓の入力を、カメラ・表示の切り替え・つつくセルに振り分ける(T-0015)。
//
// データの流れ: platform/window の入力のイベント → HandleInput → カメラと表示の設定(View の状態。世界には入らない)
//   + つつくセル(フレームのループがコマンドにする。06 §3)→ Constants() が描画の定数(render/probe_view_constants.h)を作る。
// 操作: 左クリック = つつく / 右ドラッグ = 回る / 中ドラッグ = 平行移動 / ホイール = 寄る / 1・2・3 = 表示 / X・Y・Z = 断面の軸 /
//   Q・E = 断面を動かす(Shift で 8) / B = 活性なブロック / L = 対数・線形 / O = 透視・平行 / R = カメラを戻す /
//   T = 連鎖のトレースを頼む(どこを何刻みかはフレームのループが決める。T-0088)。
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

    struct DebugViewSettings {
        DebugViewMode mode = DebugViewMode::Volume;
        uint32_t sliceAxis = 2;       // 0 = x・1 = y・2 = z
        uint32_t slicePosition = 32;  // 断面のセルの番号(クリックがつつく面)
        bool showActiveBlocks = true;
        bool logarithmic = true;
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

    private:
        void HandlePointer(const InputEvent& event, uint32_t width, uint32_t height,
                           std::vector<CellCoordinate>& pokes);
        void HandleKey(const InputEvent& event);

        uint32_t m_gridSize;  // 格子の 1 辺のセルの数
        DebugViewSettings m_settings;
        OrbitCamera m_camera;

        // --- ドラッグ中の状態 ---
        bool m_rotating = false;
        bool m_panning = false;
        int32_t m_lastX = 0;  // 前のポインタの位置(画素)
        int32_t m_lastY = 0;

        bool m_traceRequested = false;  // T が押された(TakeTraceRequest で取る)
    };

    // 表示の名前(--view の値。volume・mip・slice)→ 表示。知らない名前なら false
    [[nodiscard]] bool ParseDebugViewMode(std::string_view name, DebugViewMode& mode);

}  // namespace bicameral::render
