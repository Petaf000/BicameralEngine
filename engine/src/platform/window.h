// window.h — ゲームの窓と、窓に届いた入力(T-0004)。
//
// データの流れ: OS のメッセージ → WndProc → 入力のイベント(platform/input.h)を溜める → フレームのループが TakeInputEvents() で受け取り、
// 世界に届けるものはコマンドにする(06 §3)。窓は世界の状態を知らない(CPU は View と Controller。D-107)。
// PumpMessages() は待たない(PeekMessage)。フレームの歩調はスワップチェインの待ち(gpu/swap_chain.h)が決める。
// WndProc が this を使うので、Window は動かさない(Create は unique_ptr で返す)。
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "platform/input.h"

namespace bicameral {

    class Window {
    public:
        [[nodiscard]] static std::expected<std::unique_ptr<Window>, std::string> Create(std::wstring_view title,
                                                                                        uint32_t clientWidth,
                                                                                        uint32_t clientHeight);
        ~Window();
        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;
        Window(Window&&) = delete;
        Window& operator=(Window&&) = delete;

        // 溜まっているメッセージを全部処理する(待たない)。窓が閉じられたら false
        bool PumpMessages();

        [[nodiscard]] HWND Handle() const { return m_handle; }
        [[nodiscard]] uint32_t ClientWidth() const { return m_clientWidth; }
        [[nodiscard]] uint32_t ClientHeight() const { return m_clientHeight; }
        [[nodiscard]] bool IsMinimized() const { return m_clientWidth == 0 || m_clientHeight == 0; }

        // 前に呼んでから大きさが変わったか(呼ぶと false に戻る)
        [[nodiscard]] bool TakeResized();

        // 前に呼んでからの入力(古い順)
        [[nodiscard]] std::vector<InputEvent> TakeInputEvents();

    private:
        Window() = default;

        static LRESULT CALLBACK WindowProcedure(HWND handle, UINT message, WPARAM wParam, LPARAM lParam);
        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
        void AddButton(InputKind kind, PointerButton button, LPARAM lParam);
        void AddWheel(WPARAM wParam);

        HWND m_handle = nullptr;
        uint32_t m_clientWidth = 0;
        uint32_t m_clientHeight = 0;
        bool m_resized = false;
        bool m_closed = false;
        std::vector<InputEvent> m_inputEvents;
        int32_t m_wheelRemainder = 0;  // WHEEL_DELTA に満たないホイールの端数
        uint32_t m_heldButtons = 0;    // 押している PointerButton のビット(離すまで SetCapture する)
    };

}  // namespace bicameral
