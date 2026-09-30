// device.h — D3D12 のデバイスを作る(T-0013)。検証とデバイス喪失の調べ方(debug layer・GPU-based validation・DRED)も付ける(T-0003)。
// 本物の GPU(一番速いハードウェアのアダプタ)か WARP(ソフトウェアの D3D12)を選べる。
// WARP は GPU の無い機械(CI)でも GPU のテストを回すため(docs/design/16-debug-test.md §4)。
//
// 検証(docs/design/16-debug-test.md §1):
//   debug layer の報告は ID3D12InfoQueue1 のコールバックでログ(Channel::Gpu)へ流し、エラーの数を数える(テストはこれが 0 であることも見る)。
//   DRED(自動のブレッドクラムとページフォールト)は、デバイスが失われたとき LogDeviceRemoved() がログに出す。
//   どれも debug プリセット(BICAMERAL_GPU_VALIDATION)で既定で有効。プロセス全体の設定なので、最初のデバイスを作る前に決まる。
// d3d12.h などは pch.h から来る(このヘッダを include する .cpp は pch.h を使う)。
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include "core/aliases.h"
#include "gpu/com_ptr.h"

namespace bicameral::gpu {

    enum class AdapterKind : uint8_t {
        Hardware,  // 描画する画面を持つアダプタ → 無ければ高性能の順に並べて最初の、条件を満たすアダプタ
        Warp,      // ソフトウェアの D3D12(OS の d3d10warp.dll)
    };

    [[nodiscard]] const char* AdapterKindName(AdapterKind kind);

    struct DeviceOptions {
        bool debugLayer = false;          // D3D12 の debug layer。報告をログへ流す
        bool gpuBasedValidation = false;  // GPU で走る検証(遅い)。debugLayer が要る
        bool dred = false;                // デバイス喪失の記録(自動のブレッドクラム・ページフォールト)
        // 描画する画面(窓がある画面。MonitorFromWindow)。この画面をつないでいるアダプタを優先する。
        // 同じ GPU が別の LUID で 2 回列挙されることがあり(片方は画面を持たない)、画面を持たない方で描くと
        // Present のたびにアダプタをまたぐ写しが入るため(T-0004)。nullptr なら高性能の順だけで選ぶ(テスト)
        HMONITOR presentMonitor = nullptr;
    };

    // ビルドの既定: BICAMERAL_GPU_VALIDATION(debug プリセット)なら全部有効、それ以外は全部無効
    [[nodiscard]] DeviceOptions DefaultDeviceOptions();

    class Device {
    public:
        // 失敗の理由は文字列で返す(Error として出すか、テストの失敗にするかは呼ぶ側が決める)
        [[nodiscard]] static expected<Device, std::string> Create(
            AdapterKind kind, const DeviceOptions& options = DefaultDeviceOptions());

        [[nodiscard]] ID3D12Device5* Get() const { return m_device.Get(); }
        [[nodiscard]] IDXGIFactory6* Factory() const { return m_factory.Get(); }  // スワップチェインを作るとき
        [[nodiscard]] const DeviceOptions& Options() const { return m_options; }

        // debug layer が出したエラー(ERROR と CORRUPTION)と警告の数。debug layer が無効なら常に 0
        [[nodiscard]] uint32_t ValidationErrorCount() const;
        [[nodiscard]] uint32_t ValidationWarningCount() const;

    private:
        // debug layer のコールバックに渡す数え先。コールバックが登録されている間は動かさない(Device を動かしても同じ場所)
        struct MessageSink {
            std::atomic<uint32_t> errorCount = 0;
            std::atomic<uint32_t> warningCount = 0;
            ComPtr<ID3D12InfoQueue1> infoQueue;
            DWORD callbackCookie = 0;
            ~MessageSink();
        };

        Device() = default;

        // debug layer の 1 件の報告をログへ流して数える(ID3D12InfoQueue1::RegisterMessageCallback に渡す)
        static void __stdcall OnDebugMessage(D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity,
                                             D3D12_MESSAGE_ID id, LPCSTR description, void* context);
        [[nodiscard]] static std::unique_ptr<MessageSink> AttachMessageSink(ID3D12Device5* device);

        ComPtr<IDXGIFactory6> m_factory;
        ComPtr<ID3D12Device5> m_device;
        DeviceOptions m_options;
        std::unique_ptr<MessageSink> m_messageSink;  // debug layer が無効なら空
    };

    // DRED が記録したもの(LogDeviceRemoved の結果。テストが DRED が効いているかを見る)
    struct DeviceRemovedReport {
        bool dredAvailable = false;        // DRED の記録を読めた(DRED が無効・デバイスが失われていないなら false)
        uint32_t breadcrumbListCount = 0;  // 記録のあるコマンドリストの数
        uint64_t pageFaultAddress = 0;     // ページフォールトした GPU の仮想アドレス(無ければ 0)
    };

    // デバイスが失われた理由と、DRED が有効なら最後に走っていたコマンドとページフォールトの場所をログ(Error)に出す
    DeviceRemovedReport LogDeviceRemoved(ID3D12Device* device);

}  // namespace bicameral::gpu
