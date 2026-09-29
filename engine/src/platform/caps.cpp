// caps.cpp — GPU の対応状況を調べて表示する(T-0001 の到達点)。
// Work Graphs / Mesh Shader / DXR / SM6.8 がこの PC で使えるかを、エンジン開発の最初に確定させる。
//
// 注意: OS 標準の D3D12 ランタイムで問い合わせている。Work Graphs は Agility SDK(1.613 以降)を
// 読み込まないと Tier が NOT_SUPPORTED になることがある。Agility SDK の導入は T-0001 の後半で行う。
#include "platform/caps.h"

#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

namespace bicameral {
namespace {

const char* WorkGraphsTierName(D3D12_WORK_GRAPHS_TIER t) {
  switch (t) {
    case D3D12_WORK_GRAPHS_TIER_NOT_SUPPORTED: return "NOT_SUPPORTED";
    case D3D12_WORK_GRAPHS_TIER_1_0: return "1.0";
    default: return "(newer)";
  }
}

std::string ShaderModelName(D3D_SHADER_MODEL sm) {
  const int v = static_cast<int>(sm);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d.%d", (v >> 4) & 0xF, v & 0xF);
  return buf;
}

std::string Narrow(const wchar_t* w) {
  std::string s;
  for (; *w; ++w) s.push_back(*w < 128 ? static_cast<char>(*w) : '?');
  return s;
}

void ReportDevice(ID3D12Device* dev) {
  D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_8};
  if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
    sm.HighestShaderModel = D3D_SHADER_MODEL_6_0;
  }
  std::printf("  HighestShaderModel : %s\n", ShaderModelName(sm.HighestShaderModel).c_str());

  D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
  if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5)))) {
    std::printf("  RaytracingTier     : %d.%d\n", static_cast<int>(o5.RaytracingTier) / 10,
                static_cast<int>(o5.RaytracingTier) % 10);
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
  if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7)))) {
    std::printf("  MeshShaderTier     : %d\n", static_cast<int>(o7.MeshShaderTier));
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS21 o21{};
  if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21, &o21, sizeof(o21)))) {
    std::printf("  WorkGraphsTier     : %s\n", WorkGraphsTierName(o21.WorkGraphsTier));
  } else {
    std::printf("  WorkGraphsTier     : (OPTIONS21 の問い合わせ自体が失敗。ランタイムが古い → Agility SDK が必要)\n");
  }
}

}  // namespace

int RunCapsProbe() {
  ComPtr<IDXGIFactory6> factory;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
    std::printf("ERROR: CreateDXGIFactory2 failed\n");
    return 1;
  }
  int found = 0;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                            IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

    std::printf("Adapter %u: %s  VRAM %llu MB\n", i, Narrow(desc.Description).c_str(),
                static_cast<unsigned long long>(desc.DedicatedVideoMemory >> 20));
    ComPtr<ID3D12Device> dev;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&dev)))) {
      std::printf("  FL 12_2 device を作れない(DX12 Ultimate 非対応)\n");
      continue;
    }
    ReportDevice(dev.Get());
    ++found;
  }
  if (found == 0) {
    std::printf("ERROR: DX12 Ultimate 対応の GPU が見つからない\n");
    return 1;
  }
  return 0;
}

}  // namespace bicameral
