// device.cpp — D3D12 のデバイスを作る(T-0013)。アダプタの選び方は caps.cpp の列挙と同じ(ソフトウェアのアダプタを除く)。
#include "gpu/device.h"

#include "core/hresult.h"
#include "core/log.h"
#include "core/unicode.h"

using Microsoft::WRL::ComPtr;

namespace bicameral::gpu {
    namespace {

        using DeviceResult = std::expected<ComPtr<ID3D12Device5>, std::string>;

        // WARP は版によって FL 12_2 に届かない(Windows 11 build 26200 の WARP は 12_1。2026-09-30 の --caps)。
        // Work Graphs と SM 6.8 は FL と別に問い合わせる機能なので、WARP は作れる一番低い FL で作る
        constexpr D3D_FEATURE_LEVEL HARDWARE_FEATURE_LEVEL = D3D_FEATURE_LEVEL_12_2;
        constexpr D3D_FEATURE_LEVEL WARP_FEATURE_LEVEL = D3D_FEATURE_LEVEL_11_0;

        DeviceResult CreateHardwareDevice(IDXGIFactory6* factory) {
            for (UINT index = 0;; ++index) {
                ComPtr<IDXGIAdapter1> adapter;
                if (factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                        IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                DXGI_ADAPTER_DESC1 desc{};
                if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;

                ComPtr<ID3D12Device5> device;
                if (FAILED(D3D12CreateDevice(adapter.Get(), HARDWARE_FEATURE_LEVEL, IID_PPV_ARGS(&device)))) continue;
                Log(Channel::Gpu, Level::Info, "アダプタ: {}", ToUtf8(desc.Description));
                return device;
            }
            return std::unexpected("FL 12_2 のデバイスを作れるハードウェアのアダプタが無い");
        }

        DeviceResult CreateWarpDevice(IDXGIFactory6* factory) {
            ComPtr<IDXGIAdapter1> adapter;
            HRESULT result = factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
            if (FAILED(result)) return std::unexpected("WARP のアダプタを取れない: " + DescribeHresult(result));

            ComPtr<ID3D12Device5> device;
            result = D3D12CreateDevice(adapter.Get(), WARP_FEATURE_LEVEL, IID_PPV_ARGS(&device));
            if (FAILED(result)) return std::unexpected("WARP のデバイスを作れない: " + DescribeHresult(result));
            Log(Channel::Gpu, Level::Info, "アダプタ: WARP");
            return device;
        }

    }  // namespace

    const char* AdapterKindName(AdapterKind kind) {
        return kind == AdapterKind::Warp ? "WARP" : "hardware";
    }

    std::expected<ComPtr<ID3D12Device5>, std::string> CreateDevice(AdapterKind kind) {
        ComPtr<IDXGIFactory6> factory;
        const HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
        if (FAILED(result)) return std::unexpected("DXGI のファクトリを作れない: " + DescribeHresult(result));
        return kind == AdapterKind::Warp ? CreateWarpDevice(factory.Get()) : CreateHardwareDevice(factory.Get());
    }

}  // namespace bicameral::gpu
