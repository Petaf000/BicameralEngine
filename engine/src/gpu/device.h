// device.h — D3D12 のデバイスを作る(T-0013)。
// 本物の GPU(一番速いハードウェアのアダプタ)か WARP(ソフトウェアの D3D12)を選べる。
// WARP は GPU の無い機械(CI)でも GPU のテストを回すため(docs/design/16-debug-test.md §4)。
// d3d12.h などは pch.h から来る(このヘッダを include する .cpp は pch.h を使う)。
#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace bicameral::gpu {

    enum class AdapterKind : uint8_t {
        Hardware,  // 高性能の順に並べて最初の、FL 12_2 のデバイスを作れるアダプタ
        Warp,      // ソフトウェアの D3D12(OS の d3d10warp.dll)
    };

    [[nodiscard]] const char* AdapterKindName(AdapterKind kind);

    // 失敗の理由は文字列で返す(Error として出すか、テストの失敗にするかは呼ぶ側が決める)
    [[nodiscard]] std::expected<Microsoft::WRL::ComPtr<ID3D12Device5>, std::string> CreateDevice(AdapterKind kind);

}  // namespace bicameral::gpu
