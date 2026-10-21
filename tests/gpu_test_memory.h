// gpu_test_memory.h — GPU の試験・ベンチで、物を作った前後のこのプロセスの VRAM(ローカルのメモリ)の使用量を読む(T-0838)。
// DXGI の QueryVideoMemoryInfo(DXGI_MEMORY_SEGMENT_GROUP_LOCAL)の CurrentUsage。プロセス全体の値なので、作る前と後の差を
// 「その物の VRAM」とする(ドライバの粒度で切り上がる・ほかの物が同時に作られると混ざる。桁を知るための値)。
// WARP やアダプタを引けない時は 0。
#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>

#include "gpu/com_ptr.h"

namespace bicameral::test {

    inline uint64_t LocalVideoMemoryUsage(ID3D12Device* device) {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
            return 0;

        ComPtr<IDXGIAdapter3> adapter;
        if (FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
            return 0;

        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            return 0;

        return info.CurrentUsage;
    }

    // 前後の差(減っていれば 0)
    inline uint64_t VideoMemoryGrowth(uint64_t before, uint64_t after) {
        return after > before ? after - before : 0;
    }

}  // namespace bicameral::test
