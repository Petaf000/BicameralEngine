// swap_chain.h — 窓に描くためのスワップチェイン(T-0004、docs/design/06-simulation-loop.md §4)。
//
// フリップモデル(FLIP_DISCARD)で BUFFER_COUNT 枚。FRAME_LATENCY_WAITABLE_OBJECT を使い、CPU は WaitForFrame() で
// 「次のフレームを描き始めてよい」まで待つ(これがフレームの歩調。GPU のフェンスを CPU が待つのではない)。
// 先に進めるフレームの数は SetMaximumFrameLatency で決める(maxFrameLatency。2〜3。T-0004 の完了条件)。
// vsync なしで tearing に対応していれば、ALLOW_TEARING で Present する(可変リフレッシュの画面向け)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace bicameral::gpu {

    class SwapChain {
    public:
        static constexpr uint32_t BUFFER_COUNT = 3;
        static constexpr DXGI_FORMAT FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;

        [[nodiscard]] static std::expected<SwapChain, std::string> Create(ID3D12Device* device, IDXGIFactory6* factory,
                                                                          ID3D12CommandQueue* directQueue, HWND window,
                                                                          uint32_t maxFrameLatency);

        // 次のフレームを始めてよいまで待つ(最大 timeoutMs)。待ちきれなければ false(そのフレームも進めてよい)
        bool WaitForFrame(uint32_t timeoutMs) const;

        [[nodiscard]] uint32_t CurrentIndex() const { return m_swapChain->GetCurrentBackBufferIndex(); }
        [[nodiscard]] ID3D12Resource* BackBuffer(uint32_t index) const { return m_buffers[index].Get(); }
        [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView(uint32_t index) const;
        [[nodiscard]] uint32_t Width() const { return m_width; }
        [[nodiscard]] uint32_t Height() const { return m_height; }

        // 失敗(デバイスの喪失など)なら false
        bool Present(bool vsync) const;

        // 窓の大きさに合わせる。呼ぶ側の約束: バックバッファを使うコマンドを GPU が全部終えている(記録済みのリストも作り直す)
        bool Resize(ID3D12Device* device, uint32_t width, uint32_t height);

    private:
        SwapChain() = default;

        bool AcquireBuffers(ID3D12Device* device);

        Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, BUFFER_COUNT> m_buffers;
        struct HandleCloser {
            void operator()(HANDLE handle) const { CloseHandle(handle); }
        };
        std::unique_ptr<void, HandleCloser>
            m_frameLatencyWaitable;  // 使い終わったら閉じる(GetFrameLatencyWaitableObject)
        uint32_t m_rtvStride = 0;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        bool m_tearingSupported = false;
    };

}  // namespace bicameral::gpu
