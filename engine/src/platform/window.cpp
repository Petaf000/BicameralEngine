// window.cpp — Win32 の窓(T-0004)。使い方は window.h。
// DPI はモニターごとに扱う(Per-Monitor v2)。クライアント領域の大きさは物理ピクセルで、スワップチェインもその大きさにする。
#include "platform/window.h"

#include <utility>

#include "core/hresult.h"
#include "core/log.h"

namespace bicameral {
    namespace {

        constexpr wchar_t WINDOW_CLASS_NAME[] = L"BicameralEngineWindow";
        constexpr DWORD WINDOW_STYLE = WS_OVERLAPPEDWINDOW;

        bool RegisterWindowClass(WNDPROC procedure) {
            const WNDCLASSEXW windowClass{
                .cbSize = sizeof(WNDCLASSEXW),
                .style = CS_HREDRAW | CS_VREDRAW,
                .lpfnWndProc = procedure,
                .hInstance = GetModuleHandleW(nullptr),
                .hCursor = LoadCursorW(nullptr, IDC_ARROW),
                .lpszClassName = WINDOW_CLASS_NAME,
            };

            // 2 つ目の窓を作るときは登録済み(ERROR_CLASS_ALREADY_EXISTS)なのでそれでよい
            return RegisterClassExW(&windowClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }

    }  // namespace

    std::expected<std::unique_ptr<Window>, std::string> Window::Create(std::wstring_view title, uint32_t clientWidth,
                                                                       uint32_t clientHeight) {
        // 高 DPI の画面でぼやけないように、モニターごとの DPI を自分で扱うと宣言する(最初の窓より前)
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (!RegisterWindowClass(&Window::WindowProcedure))
            return std::unexpected("窓のクラスを登録できない: " + DescribeHresult(HRESULT_FROM_WIN32(GetLastError())));

        // 指定はクライアント領域の大きさ。枠の分を足して窓の大きさにする
        RECT rect{
            .left = 0, .top = 0, .right = static_cast<LONG>(clientWidth), .bottom = static_cast<LONG>(clientHeight)};
        AdjustWindowRectEx(&rect, WINDOW_STYLE, FALSE, 0);

        std::unique_ptr<Window> window(new Window());
        const std::wstring titleText(title);

        const HWND handle = CreateWindowExW(0, WINDOW_CLASS_NAME, titleText.c_str(), WINDOW_STYLE, CW_USEDEFAULT,
                                            CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr,
                                            nullptr, GetModuleHandleW(nullptr), window.get());

        if (handle == nullptr)
            return std::unexpected("窓を作れない: " + DescribeHresult(HRESULT_FROM_WIN32(GetLastError())));

        ShowWindow(handle, SW_SHOWNORMAL);

        return window;
    }

    Window::~Window() {
        if (m_handle != nullptr)
            DestroyWindow(m_handle);
    }

    bool Window::PumpMessages() {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT)
                m_closed = true;

            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        return !m_closed;
    }

    bool Window::TakeResized() {
        return std::exchange(m_resized, false);
    }

    std::vector<InputEvent> Window::TakeInputEvents() {
        return std::exchange(m_inputEvents, {});
    }

    // --- 入力 ---

    namespace {

        // 位置は符号つきの 16bit(捕まえている間は窓の外 = 負にもなる。windowsx.h の GET_X_LPARAM と同じ)
        InputEvent PointerAt(InputKind kind, LPARAM lParam) {
            return {.kind = kind, .x = static_cast<int16_t>(LOWORD(lParam)), .y = static_cast<int16_t>(HIWORD(lParam))};
        }

    }  // namespace

    // ボタンを押している間はポインタを捕まえる(窓の外へドラッグしても離したことが届くように)
    void Window::AddButton(InputKind kind, PointerButton button, LPARAM lParam) {
        InputEvent event = PointerAt(kind, lParam);
        event.button = button;
        m_inputEvents.push_back(event);
        const uint32_t bit = 1u << static_cast<uint32_t>(button);
        const bool wasHeld = m_heldButtons != 0;
        m_heldButtons = kind == InputKind::ButtonDown ? m_heldButtons | bit : m_heldButtons & ~bit;
        if (!wasHeld && m_heldButtons != 0)
            SetCapture(m_handle);

        if (wasHeld && m_heldButtons == 0)
            ReleaseCapture();
    }

    // WHEEL_DELTA(120)ごとに 1 刻み。高分解能のホイールの端数は溜めておく
    void Window::AddWheel(WPARAM wParam) {
        m_wheelRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
        const int32_t steps = m_wheelRemainder / WHEEL_DELTA;
        if (steps == 0)
            return;

        m_wheelRemainder -= steps * WHEEL_DELTA;
        m_inputEvents.push_back({.kind = InputKind::Wheel, .wheelSteps = steps});
    }

    // --- メッセージ ---

    LRESULT CALLBACK Window::WindowProcedure(HWND handle, UINT message, WPARAM wParam, LPARAM lParam) {
        // CreateWindowExW に渡した this を、最初のメッセージ(WM_NCCREATE)で窓に結び付ける
        if (message == WM_NCCREATE) {
            // NOLINTNEXTLINE(performance-no-int-to-ptr) Win32 のメッセージは LPARAM にポインタを入れて渡す
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            auto* window = static_cast<Window*>(create->lpCreateParams);
            window->m_handle = handle;
            SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        }

        // NOLINTNEXTLINE(performance-no-int-to-ptr) GWLP_USERDATA に入れた this を取り出す
        auto* window = reinterpret_cast<Window*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
        if (window == nullptr)
            return DefWindowProcW(handle, message, wParam, lParam);

        return window->HandleMessage(message, wParam, lParam);
    }

    LRESULT Window::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        const HWND handle = m_handle;  // WM_NCDESTROY で m_handle を空にした後も DefWindowProcW に渡す
        switch (message) {
            case WM_SIZE:
                m_clientWidth = LOWORD(lParam);
                m_clientHeight = HIWORD(lParam);
                m_resized = true;
                return 0;
            case WM_LBUTTONDOWN: AddButton(InputKind::ButtonDown, PointerButton::Left, lParam); return 0;
            case WM_LBUTTONUP: AddButton(InputKind::ButtonUp, PointerButton::Left, lParam); return 0;
            case WM_RBUTTONDOWN: AddButton(InputKind::ButtonDown, PointerButton::Right, lParam); return 0;
            case WM_RBUTTONUP: AddButton(InputKind::ButtonUp, PointerButton::Right, lParam); return 0;
            case WM_MBUTTONDOWN: AddButton(InputKind::ButtonDown, PointerButton::Middle, lParam); return 0;
            case WM_MBUTTONUP: AddButton(InputKind::ButtonUp, PointerButton::Middle, lParam); return 0;
            case WM_MOUSEMOVE: m_inputEvents.push_back(PointerAt(InputKind::PointerMove, lParam)); return 0;
            case WM_MOUSEWHEEL: AddWheel(wParam); return 0;
            case WM_KEYDOWN:
                m_inputEvents.push_back({.kind = InputKind::KeyDown,
                                         .key = static_cast<uint32_t>(wParam),
                                         .shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0});

                return 0;
            case WM_CAPTURECHANGED: m_heldButtons = 0; return 0;  // 別の窓に取られた(離したことは届かない)
            case WM_DPICHANGED: {
                // 新しい DPI で OS が勧める位置と大きさにする(クライアント領域の物理ピクセルが変わり、WM_SIZE が来る)
                // NOLINTNEXTLINE(performance-no-int-to-ptr) WM_DPICHANGED は LPARAM に RECT のポインタを入れる
                const auto* suggested = reinterpret_cast<const RECT*>(lParam);
                SetWindowPos(m_handle, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                             suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);

                return 0;
            }
            case WM_DESTROY:
                m_closed = true;
                PostQuitMessage(0);
                return 0;
            case WM_NCDESTROY:
                SetWindowLongPtrW(handle, GWLP_USERDATA, 0);
                m_handle = nullptr;
                break;
            default: break;
        }

        return DefWindowProcW(handle, message, wParam, lParam);
    }

}  // namespace bicameral
