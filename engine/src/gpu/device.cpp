// device.cpp — D3D12 のデバイスを作る(T-0013)。アダプタの選び方は caps.cpp の列挙と同じ(ソフトウェアのアダプタを除く)。
// 最低機の条件(D-210・D-211・ADR-0009)を満たさないデバイスは使わない: SM 6.8・Work Graphs 1.0・Int64ShaderOps。
// raw / structured バッファへの 64bit atomic は SM 6.6 以上で必須なので、SM 6.8 の確認に含まれる。
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

        // 足りない機能の名前を返す(全部あれば空)
        std::string FindMissingFeatures(ID3D12Device* device) {
            std::string missing;
            D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_8};
            if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))) ||
                shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_8) {
                missing += " SM6.8";
            }
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) 0 で埋めて渡す(caps.cpp と同じ)
            D3D12_FEATURE_DATA_D3D12_OPTIONS21 options21{};
            if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21, &options21, sizeof(options21))) ||
                options21.WorkGraphsTier == D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED) {
                missing += " WorkGraphs";
            }
            D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
            if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1))) ||
                !options1.Int64ShaderOps) {
                missing += " Int64ShaderOps";
            }
            return missing;
        }

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
                const std::string missing = FindMissingFeatures(device.Get());
                if (!missing.empty()) {
                    Log(Channel::Gpu, Level::Warning, "アダプタ {} は使わない(足りない機能:{})",
                        ToUtf8(desc.Description), missing);
                    continue;
                }
                Log(Channel::Gpu, Level::Info, "アダプタ: {}", ToUtf8(desc.Description));
                return device;
            }
            return std::unexpected("最低機の条件(FL 12_2・SM 6.8・Work Graphs・Int64ShaderOps)を満たすアダプタが無い");
        }

        DeviceResult CreateWarpDevice(IDXGIFactory6* factory) {
            ComPtr<IDXGIAdapter1> adapter;
            HRESULT result = factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
            if (FAILED(result)) return std::unexpected("WARP のアダプタを取れない: " + DescribeHresult(result));

            ComPtr<ID3D12Device5> device;
            result = D3D12CreateDevice(adapter.Get(), WARP_FEATURE_LEVEL, IID_PPV_ARGS(&device));
            if (FAILED(result)) return std::unexpected("WARP のデバイスを作れない: " + DescribeHresult(result));
            const std::string missing = FindMissingFeatures(device.Get());
            if (!missing.empty()) return std::unexpected("WARP に足りない機能:" + missing);
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
