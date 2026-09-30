// swap_chain.cpp — フリップモデルの待てるスワップチェイン(T-0004)。使い方は swap_chain.h。
#include "gpu/swap_chain.h"

#include "core/hresult.h"
#include "core/log.h"

using Microsoft::WRL::ComPtr;

namespace bicameral::gpu {
    namespace {

        constexpr UINT SWAP_CHAIN_FLAGS_BASE = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

        bool IsTearingSupported(IDXGIFactory6* factory) {
            BOOL allowTearing = FALSE;
            return SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing,
                                                          sizeof(allowTearing))) &&
                   allowTearing == TRUE;
        }

    }  // namespace

    std::expected<SwapChain, std::string> SwapChain::Create(ID3D12Device* device, IDXGIFactory6* factory,
                                                            ID3D12CommandQueue* directQueue, HWND window,
                                                            uint32_t maxFrameLatency) {
        SwapChain swapChain;
        swapChain.m_tearingSupported = IsTearingSupported(factory);
        const DXGI_SWAP_CHAIN_DESC1 desc{
            .Width = 0,  // 0 = 窓のクライアント領域の大きさ
            .Height = 0,
            .Format = FORMAT,
            .SampleDesc = {.Count = 1},
            .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
            .BufferCount = BUFFER_COUNT,
            .Scaling = DXGI_SCALING_NONE,
            .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
            .AlphaMode = DXGI_ALPHA_MODE_IGNORE,
            .Flags = SWAP_CHAIN_FLAGS_BASE | (swapChain.m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u),
        };
        ComPtr<IDXGISwapChain1> swapChain1;
        HRESULT result = factory->CreateSwapChainForHwnd(directQueue, window, &desc, nullptr, nullptr, &swapChain1);
        if (FAILED(result)) return std::unexpected("スワップチェインを作れない: " + DescribeHresult(result));
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);  // 全画面の切り替えは自分でやる(今は無し)
        result = swapChain1.As(&swapChain.m_swapChain);
        if (FAILED(result)) return std::unexpected("IDXGISwapChain3 が無い: " + DescribeHresult(result));

        result = swapChain.m_swapChain->SetMaximumFrameLatency(maxFrameLatency);
        if (FAILED(result)) return std::unexpected("先行するフレームの数を設定できない: " + DescribeHresult(result));
        swapChain.m_frameLatencyWaitable.reset(swapChain.m_swapChain->GetFrameLatencyWaitableObject());

        const D3D12_DESCRIPTOR_HEAP_DESC heapDesc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                                                  .NumDescriptors = BUFFER_COUNT};
        result = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&swapChain.m_rtvHeap));
        if (FAILED(result)) return std::unexpected("RTV のヒープを作れない: " + DescribeHresult(result));
        swapChain.m_rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        if (!swapChain.AcquireBuffers(device)) return std::unexpected("バックバッファを取れない");

        Log(Channel::Gpu, Level::Info, "スワップチェイン: {}×{}  {} 枚  先行 {} フレーム  tearing {}",
            swapChain.m_width, swapChain.m_height, BUFFER_COUNT, maxFrameLatency,
            swapChain.m_tearingSupported ? "可" : "不可");
        return swapChain;
    }

    bool SwapChain::AcquireBuffers(ID3D12Device* device) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        if (!BICAMERAL_CHECK_HR(Channel::Gpu, m_swapChain->GetDesc1(&desc))) return false;
        m_width = desc.Width;
        m_height = desc.Height;
        for (uint32_t index = 0; index < BUFFER_COUNT; ++index) {
            if (!BICAMERAL_CHECK_HR(Channel::Gpu, m_swapChain->GetBuffer(index, IID_PPV_ARGS(&m_buffers[index])))) {
                return false;
            }
            m_buffers[index]->SetName(std::format(L"BackBuffer{}", index).c_str());
            device->CreateRenderTargetView(m_buffers[index].Get(), nullptr, RenderTargetView(index));
        }
        return true;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE SwapChain::RenderTargetView(uint32_t index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += size_t{index} * m_rtvStride;
        return handle;
    }

    bool SwapChain::WaitForFrame(uint32_t timeoutMs) const {
        return WaitForSingleObjectEx(m_frameLatencyWaitable.get(), timeoutMs, TRUE) == WAIT_OBJECT_0;
    }

    bool SwapChain::Present(bool vsync) const {
        const UINT flags = !vsync && m_tearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0;
        return BICAMERAL_CHECK_HR(Channel::Gpu, m_swapChain->Present(vsync ? 1 : 0, flags));
    }

    bool SwapChain::Resize(ID3D12Device* device, uint32_t width, uint32_t height) {
        for (auto& buffer : m_buffers) {
            buffer.Reset();
        }
        const UINT flags = SWAP_CHAIN_FLAGS_BASE | (m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
        if (!BICAMERAL_CHECK_HR(Channel::Gpu, m_swapChain->ResizeBuffers(BUFFER_COUNT, width, height, FORMAT, flags))) {
            return false;
        }
        return AcquireBuffers(device);
    }

}  // namespace bicameral::gpu
