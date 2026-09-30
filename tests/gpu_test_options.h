// gpu_test_options.h — GPU を使うテスト(gpu_*_test.cpp)に共通の引数(T-0013)と、検証の確かめ方(T-0003)。
//   --warp                  ハードウェアの GPU の代わりに WARP(ソフトウェアの D3D12)で走らせる
//   --queue direct|compute  コマンドを投げるキュー(既定は compute。シミュは compute キューで走らせる予定: 06 §4)
// 同じテストを引数だけ変えて ctest に複数登録する(tests/CMakeLists.txt)。
#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "core/log.h"
#include "gpu/device.h"

namespace bicameral::test {

    struct GpuTestOptions {
        gpu::AdapterKind adapter = gpu::AdapterKind::Hardware;
        D3D12_COMMAND_LIST_TYPE queueType = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    };

    inline const char* QueueTypeName(D3D12_COMMAND_LIST_TYPE type) {
        return type == D3D12_COMMAND_LIST_TYPE_DIRECT ? "direct" : "compute";
    }

    // 知らない引数があれば std::nullopt(呼ぶ側が使い方を出して終える)
    inline std::optional<GpuTestOptions> ParseGpuTestOptions(std::span<char*> arguments) {
        GpuTestOptions options;
        for (size_t index = 1; index < arguments.size(); ++index) {
            const std::string_view argument = arguments[index];
            const bool hasValue = index + 1 < arguments.size();
            if (argument == "--warp")
                options.adapter = gpu::AdapterKind::Warp;
            else if (argument == "--queue" && hasValue) {
                const std::string_view queue = arguments[++index];
                if (queue != "direct" && queue != "compute")
                    return std::nullopt;

                options.queueType = queue == "direct" ? D3D12_COMMAND_LIST_TYPE_DIRECT
                                                      : D3D12_COMMAND_LIST_TYPE_COMPUTE;
            } else
                return std::nullopt;
        }

        return options;
    }

    // debug layer(debug プリセットで有効。T-0003)がエラーを報告していたら false。テストの最後に呼び、false なら失敗にする
    inline bool PassesValidation(const gpu::Device& device, std::string_view testName) {
        const uint32_t errorCount = device.ValidationErrorCount();
        if (errorCount == 0)
            return true;

        Log(Channel::Gpu, Level::Error, "{}: FAILED(debug layer のエラーが {} 件。上のログの D3D12 [...] の行)",
            testName, errorCount);

        return false;
    }

}  // namespace bicameral::test
