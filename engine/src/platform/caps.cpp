// caps.cpp — GPU の対応状況を調べてログに出す(T-0001 の到達点。T-0007 で printf からログへ)。
// Work Graphs / Mesh Shader / DXR / SM6.8 がこの PC で使えるかを、エンジン開発の最初に確定させる。
//
// 注意: OS 標準の D3D12 ランタイムで問い合わせている。古い OS では Agility SDK(1.613 以降)を
// 読み込まないと Work Graphs が NOT_SUPPORTED になる。Windows 11 build 26200 + RTX 3070 Ti では
// OS 標準のランタイムで WorkGraphsTier 1.0 を確認済み(2026-09-29)。
#include "platform/caps.h"

#include "core/hresult.h"
#include "core/log.h"
#include "core/unicode.h"

using Microsoft::WRL::ComPtr;

namespace bicameral {
    namespace {

        const char* WorkGraphsTierName(D3D12_WORK_GRAPHS_TIER tier) {
            switch (tier) {
                case D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED: return "NOT_SUPPORTED";
                case D3D12_WORK_GRAPHS_TIER_1_0: return "1.0";
                default: return "(newer)";
            }
        }

        // D3D_SHADER_MODEL_6_8 = 0x68 → "6.8"
        std::string ShaderModelName(D3D_SHADER_MODEL shaderModel) {
            const int value = static_cast<int>(shaderModel);
            return std::format("{}.{}", (value >> 4) & 0xF, value & 0xF);
        }

        // RaytracingTier・MeshShaderTier は 10 倍表記(TIER_1_1 = 11)
        std::string TierName(int tier) {
            return std::format("{}.{}", tier / 10, tier % 10);
        }

        // どの D3D12Core.dll が読み込まれたかを表示する。
        // exe 横の D3D12\ から読まれていれば Agility SDK、System32 からなら OS 標準のランタイム。
        // D3D12Core.dll はデバイスを作った後でないと読み込まれていないので、その後に呼ぶ。
        void ReportRuntime() {
            HMODULE core = GetModuleHandleW(L"D3D12Core.dll");
            if (core == nullptr) {
                Log(Channel::Platform, Level::Warning, "D3D12Core : 読み込まれていない");
                return;
            }
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(core, path, MAX_PATH);
            Log(Channel::Platform, Level::Info, "D3D12Core : {}  (D3D12SDKVersion {})", ToUtf8(path),
                D3D12_SDK_VERSION);
        }

        void ReportDevice(ID3D12Device* device) {
            D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{D3D_SHADER_MODEL_6_8};
            if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)))) {
                shaderModel.HighestShaderModel = D3D_SHADER_MODEL_6_0;
            }
            Log(Channel::Platform, Level::Info, "  HighestShaderModel : {}",
                ShaderModelName(shaderModel.HighestShaderModel));

            D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)))) {
                Log(Channel::Platform, Level::Info, "  RaytracingTier     : {}",
                    TierName(static_cast<int>(options5.RaytracingTier)));
            }
            D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7)))) {
                Log(Channel::Platform, Level::Info, "  MeshShaderTier     : {}",
                    TierName(static_cast<int>(options7.MeshShaderTier)));
            }

            // 古いランタイムでは OPTIONS21 の問い合わせ自体が失敗する。バグではなく環境の問題なので Warning
            // 0 で埋めて渡し、CheckFeatureSupport が書き込む(Tier の enum に 0 の値が無いのは SDK の定義による)
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization)
            D3D12_FEATURE_DATA_D3D12_OPTIONS21 options21{};
            const HRESULT result =
                device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21, &options21, sizeof(options21));
            if (FAILED(result)) {
                Log(Channel::Platform, Level::Warning,
                    "  WorkGraphsTier     : 問い合わせが失敗(ランタイムが古い → Agility SDK が必要): {}",
                    DescribeHresult(result));
                return;
            }
            Log(Channel::Platform, Level::Info, "  WorkGraphsTier     : {}",
                WorkGraphsTierName(options21.WorkGraphsTier));
        }

        bool SameLuid(const LUID& a, const LUID& b) {
            return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
        }

    }  // namespace

    int RunCapsProbe() {
        ComPtr<IDXGIFactory6> factory;
        if (!BICAMERAL_CHECK_HR(Channel::Platform, CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return 1;

        int found = 0;
        std::vector<LUID> seen;  // 同じ GPU が複数回列挙されることがある(仮想ディスプレイ等)ので LUID で除く
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                    IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            if (!BICAMERAL_CHECK_HR(Channel::Platform, adapter->GetDesc1(&desc))) continue;
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (std::ranges::any_of(seen, [&](const LUID& luid) { return SameLuid(luid, desc.AdapterLuid); })) {
                continue;
            }
            seen.push_back(desc.AdapterLuid);

            Log(Channel::Platform, Level::Info, "Adapter {}: {}  VRAM {} MB  LUID {:08x}:{:08x}  flags 0x{:x}", index,
                ToUtf8(desc.Description), desc.DedicatedVideoMemory >> 20,
                static_cast<unsigned long>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart, desc.Flags);

            // FL 12_2 に届かない GPU は対象外なだけ(エラーではない)
            ComPtr<ID3D12Device> device;
            const HRESULT result = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&device));
            if (FAILED(result)) {
                Log(Channel::Platform, Level::Warning, "  FL 12_2 のデバイスを作れない(DX12 Ultimate 非対応): {}",
                    DescribeHresult(result));
                continue;
            }
            if (found == 0) ReportRuntime();
            ReportDevice(device.Get());
            ++found;
        }
        if (found == 0) {
            Log(Channel::Platform, Level::Error, "DX12 Ultimate 対応の GPU が見つからない");
            return 1;
        }
        return 0;
    }

}  // namespace bicameral
