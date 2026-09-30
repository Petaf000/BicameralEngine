// input.h — 窓に届いた入力のイベント(OS に依存しない形。T-0015)。
//
// データの流れ: platform/window.cpp の WndProc が溜める → フレームのループが TakeInputEvents() で受け取り、
// render/debug_view_controller がカメラ・表示の切り替え・つつき(コマンド)に振り分ける。
// 位置はクライアント領域の物理ピクセル。キーは Windows の仮想キーコード(VK_* と 'A' などの大文字)。
#pragma once

#include <cstdint>

namespace bicameral {

    enum class PointerButton : uint8_t { Left, Right, Middle };

    enum class InputKind : uint8_t {
        ButtonDown,   // button を押した(x, y)
        ButtonUp,     // button を離した(x, y)
        PointerMove,  // ポインタが動いた(x, y)
        Wheel,        // ホイール(wheelSteps: 奥へ 1 刻み = +1。高分解能のホイールは端数を溜めて 1 刻みごとに出す)
        KeyDown,      // キーを押した(key。押しっぱなしの繰り返しも来る)
    };

    struct InputEvent {
        InputKind kind = InputKind::PointerMove;

        // --- ポインタ ---
        PointerButton button = PointerButton::Left;
        int32_t x = 0;  // 窓の中の位置(画素)
        int32_t y = 0;
        int32_t wheelSteps = 0;

        // --- キー ---
        uint32_t key = 0;
        bool shift = false;  // Shift を押しながら
    };

}  // namespace bicameral
