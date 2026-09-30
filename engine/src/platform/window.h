// window.h — ゲームの窓と、窓に届いた入力(T-0004)。
//
// データの流れ: OS のメッセージ → WndProc → 入力のイベントを溜める → フレームのループが TakePointerEvents() で受け取り、
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

namespace bicameral {

    // クリック(押した瞬間)。位置はクライアント領域の物理ピクセル
    struct PointerEvent {
        int32_t x = 0;
        int32_t y = 0;
    };

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

        // 前に呼んでからのクリック(古い順)
        [[nodiscard]] std::vector<PointerEvent> TakePointerEvents();

    private:
        Window() = default;

        static LRESULT CALLBACK WindowProcedure(HWND handle, UINT message, WPARAM wParam, LPARAM lParam);
        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

        HWND m_handle = nullptr;
        uint32_t m_clientWidth = 0;
        uint32_t m_clientHeight = 0;
        bool m_resized = false;
        bool m_closed = false;
        std::vector<PointerEvent> m_pointerEvents;
    };

}  // namespace bicameral
