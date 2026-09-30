// gpu_debug_device_test.cpp — debug layer の報告がログに流れて数えられるか、DRED がデバイス喪失の記録を残すかを確かめる(T-0003)。
// debug layer と DRED は DeviceOptions で明示的に有効にする(Release のビルドでも同じことを確かめる)。GPU-based validation は付けない。
//   1. わざと誤った呼び出し(幅 0 のバッファを作る)をして、debug layer のエラーが 1 件以上数えられるか
//   2. コマンドを 1 本走らせてから ID3D12Device5::RemoveDevice でデバイスを失わせ、LogDeviceRemoved() が DRED の記録を読めるか
// どちらもわざとエラーを起こすので、ログの Error は期待どおり。引数は gpu_test_options.h。
#include "core/aliases.h"
#include "core/log.h"
#include "core/singleton.h"
#include "gpu/com_ptr.h"
#include "gpu/device.h"
#include "gpu/immediate_queue.h"
#include "gpu/resources.h"
#include "gpu_test_options.h"

using namespace bicameral;

namespace {

    constexpr uint64_t BUFFER_BYTES = 256;

    // --- 1. debug layer の報告 ---

    bool TestValidationMessages(const gpu::Device& device) {
        const uint32_t before = device.ValidationErrorCount();
        const D3D12_HEAP_PROPERTIES heap{.Type = D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC desc{
            .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
            .Width = 0,  // 誤り: 幅 0 のバッファ
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .Format = DXGI_FORMAT_UNKNOWN,
            .SampleDesc = {.Count = 1},
            .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        };

        ComPtr<ID3D12Resource> buffer;
        const HRESULT result = device.Get()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer));
        const uint32_t after = device.ValidationErrorCount();
        Log(Channel::Gpu, Level::Info, "幅 0 のバッファ: 作成 {}、debug layer のエラー {} → {}",
            FAILED(result) ? "失敗(期待どおり)" : "成功", before, after);

        return FAILED(result) && after > before;
    }

    // --- 2. DRED ---

    bool TestDeviceRemoved(const gpu::Device& device, D3D12_COMMAND_LIST_TYPE queueType) {
        auto queue = gpu::ImmediateQueue::Create(device.Get(), queueType);
        const ComPtr<ID3D12Resource> source =
            gpu::CreateBuffer(device.Get(), BUFFER_BYTES, gpu::BufferKind::UnorderedAccess);
        const ComPtr<ID3D12Resource> readback =
            gpu::CreateBuffer(device.Get(), BUFFER_BYTES, gpu::BufferKind::Readback);
        if (!queue || !source || !readback)
            return false;

        ID3D12GraphicsCommandList10* list = queue->Begin();
        if (list == nullptr)
            return false;

        list->CopyBufferRegion(readback.Get(), 0, source.Get(), 0, BUFFER_BYTES);
        if (!queue->ExecuteAndWait())
            return false;

        device.Get()->RemoveDevice();
        const gpu::DeviceRemovedReport report = gpu::LogDeviceRemoved(device.Get());
        Log(Channel::Gpu, Level::Info, "DRED: 読めた {}、記録のあるリスト {} 本", report.dredAvailable,
            report.breadcrumbListCount);

        return report.dredAvailable;
    }

    int Run(span<char*> arguments) {
        const auto options = test::ParseGpuTestOptions(arguments);
        if (!options) {
            Log(Channel::Gpu, Level::Error, "使い方: gpu_debug_device_test [--warp] [--queue direct|compute]");
            return 2;
        }

        Log(Channel::Gpu, Level::Info, "gpu_debug_device_test: adapter {}(この後の Error はわざと起こしたもの)",
            gpu::AdapterKindName(options->adapter));
        const auto device = gpu::Device::Create(options->adapter, {.debugLayer = true, .dred = true});

        if (!device) {
            Log(Channel::Gpu, Level::Error, "gpu_debug_device_test: FAILED ({})", device.error());
            return 1;
        }

        if (!TestValidationMessages(*device)) {
            Log(Channel::Gpu, Level::Error, "gpu_debug_device_test: FAILED(debug layer のエラーが数えられない)");
            return 1;
        }

        if (!TestDeviceRemoved(*device, options->queueType)) {
            Log(Channel::Gpu, Level::Error, "gpu_debug_device_test: FAILED(DRED の記録を読めない)");
            return 1;
        }

        Log(Channel::Gpu, Level::Info, "gpu_debug_device_test: OK");

        return 0;
    }

}  // namespace

int main(int argc, char** argv) {
    const int exitCode = Run(span(argv, static_cast<size_t>(argc)));
    SingletonFinalizer::Finalize();  // ログを閉じる

    return exitCode;
}
