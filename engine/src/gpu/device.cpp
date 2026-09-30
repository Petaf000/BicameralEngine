// device.cpp — D3D12 のデバイスを作る(T-0013)と、検証・デバイス喪失の記録(T-0003、docs/design/16-debug-test.md §1)。
// アダプタの選び方: ソフトウェアのアダプタを除き、描画する画面を持つアダプタ → 高性能の順(T-0004。device.h の presentMonitor)。
// 最低機の条件(D-210・D-211・ADR-0009)を満たさないデバイスは使わない: SM 6.8・Work Graphs 1.0・Int64ShaderOps。
// raw / structured バッファへの 64bit atomic は SM 6.6 以上で必須なので、SM 6.8 の確認に含まれる。
// debug layer の d3d12SDKLayers.dll は Agility SDK のもの(exe の横の D3D12\。engine/CMakeLists.txt がコピーする)。
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

        // DRED の記録をログに出すとき、止まったコマンドの前後いくつを見せるか
        constexpr uint32_t BREADCRUMB_CONTEXT_BEFORE = 3;
        constexpr uint32_t BREADCRUMB_CONTEXT_AFTER = 3;
        constexpr int MAX_LOGGED_ALLOCATIONS = 8;

        // --- アダプタとデバイス ---

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

        // ソフトウェアでなく、FL 12_2 のデバイスを作れて最低機の機能がそろうならデバイスを返す(だめなら nullptr)
        ComPtr<ID3D12Device5> TryCreateHardwareDevice(IDXGIAdapter1* adapter) {
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) return nullptr;
            ComPtr<ID3D12Device5> device;
            if (FAILED(D3D12CreateDevice(adapter, HARDWARE_FEATURE_LEVEL, IID_PPV_ARGS(&device)))) return nullptr;
            const std::string missing = FindMissingFeatures(device.Get());
            if (!missing.empty()) {
                Log(Channel::Gpu, Level::Warning, "アダプタ {} は使わない(足りない機能:{})", ToUtf8(desc.Description),
                    missing);
                return nullptr;
            }
            Log(Channel::Gpu, Level::Info, "アダプタ: {}  LUID {:08x}:{:08x}", ToUtf8(desc.Description),
                static_cast<unsigned long>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart);
            return device;
        }

        // アダプタが monitor をつないでいるか(画面の一覧に含むか)
        bool AdapterOwnsMonitor(IDXGIAdapter1* adapter, HMONITOR monitor) {
            for (UINT outputIndex = 0;; ++outputIndex) {
                ComPtr<IDXGIOutput> output;
                if (adapter->EnumOutputs(outputIndex, &output) == DXGI_ERROR_NOT_FOUND) return false;
                DXGI_OUTPUT_DESC outputDesc{};
                if (SUCCEEDED(output->GetDesc(&outputDesc)) && outputDesc.Monitor == monitor) return true;
            }
        }

        // 高性能の順に見て、最初に条件を満たすアダプタでデバイスを作る。monitor が nullptr でなければ、その画面を持つものだけ
        ComPtr<ID3D12Device5> CreateFirstHardwareDevice(IDXGIFactory6* factory, HMONITOR monitor) {
            for (UINT index = 0;; ++index) {
                ComPtr<IDXGIAdapter1> adapter;
                if (factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                        IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) {
                    return nullptr;
                }
                if (monitor != nullptr && !AdapterOwnsMonitor(adapter.Get(), monitor)) continue;
                if (ComPtr<ID3D12Device5> device = TryCreateHardwareDevice(adapter.Get())) return device;
            }
        }

        // 窓の画面を持つアダプタ → 無ければ(使えなければ)全部を高性能の順に
        DeviceResult CreateHardwareDevice(IDXGIFactory6* factory, HMONITOR presentMonitor) {
            if (presentMonitor != nullptr) {
                if (ComPtr<ID3D12Device5> device = CreateFirstHardwareDevice(factory, presentMonitor)) return device;
                Log(Channel::Gpu, Level::Warning, "窓の画面を持つアダプタが使えないので、高性能の順で選ぶ");
            }
            if (ComPtr<ID3D12Device5> device = CreateFirstHardwareDevice(factory, nullptr)) return device;
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

        // --- 検証と DRED(プロセス全体の設定)---

        // デバイスを作る前に呼ぶ。debug layer はデバイスを作った後に有効にすると、そのデバイスが失われる(D3D12 の仕様)
        void EnableDebugFeatures(const DeviceOptions& options) {
            if (options.debugLayer || options.gpuBasedValidation) {
                ComPtr<ID3D12Debug1> debug;
                if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
                    debug->EnableDebugLayer();
                    debug->SetEnableGPUBasedValidation(options.gpuBasedValidation ? TRUE : FALSE);
                } else {
                    Log(Channel::Gpu, Level::Warning, "debug layer を使えない(D3D12\\d3d12SDKLayers.dll が無い?)");
                }
            }
            if (options.dred) {
                ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred;
                if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
                    dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                    dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                    dred->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                } else {
                    Log(Channel::Gpu, Level::Warning, "DRED の設定を取れない");
                }
            }
            Log(Channel::Gpu, Level::Info, "検証: debug layer {}・GPU-based validation {}・DRED {}",
                options.debugLayer || options.gpuBasedValidation ? "on" : "off",
                options.gpuBasedValidation ? "on" : "off", options.dred ? "on" : "off");
        }

        Level LevelOfSeverity(D3D12_MESSAGE_SEVERITY severity) {
            switch (severity) {
                case D3D12_MESSAGE_SEVERITY_CORRUPTION:
                case D3D12_MESSAGE_SEVERITY_ERROR: return Level::Error;
                case D3D12_MESSAGE_SEVERITY_WARNING: return Level::Warning;
                case D3D12_MESSAGE_SEVERITY_INFO: return Level::Debug;
                default: return Level::Trace;
            }
        }

        // --- DRED の記録を読む ---

        std::string BreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op) {
            switch (op) {
                case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
                case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
                case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
                case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
                case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
                case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
                case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
                case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
                case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
                case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
                case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUnorderedAccessView";
                case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
                case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
                case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return "ResolveQueryData";
                case D3D12_AUTO_BREADCRUMB_OP_WRITEBUFFERIMMEDIATE: return "WriteBufferImmediate";
                case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return "DispatchRays";
                case D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH: return "DispatchMesh";
                case D3D12_AUTO_BREADCRUMB_OP_BARRIER: return "Barrier";
                case D3D12_AUTO_BREADCRUMB_OP_BEGIN_COMMAND_LIST: return "BeginCommandList";
                case D3D12_AUTO_BREADCRUMB_OP_DISPATCHGRAPH: return "DispatchGraph";
                case D3D12_AUTO_BREADCRUMB_OP_SETPROGRAM: return "SetProgram";
                default: return std::format("op {}", static_cast<int>(op));
            }
        }

        std::string DebugNameOf(const wchar_t* name) {
            return name != nullptr ? ToUtf8(name) : std::string("(名前なし)");
        }

        // 1 本のコマンドリストの記録: どこまで終わったか、止まった所の前後に何があったか
        void LogBreadcrumbNode(const D3D12_AUTO_BREADCRUMB_NODE1& node) {
            const uint32_t completed = node.pLastBreadcrumbValue != nullptr ? *node.pLastBreadcrumbValue : 0;
            const std::string where = std::format("リスト {}(キュー {})", DebugNameOf(node.pCommandListDebugNameW),
                                                  DebugNameOf(node.pCommandQueueDebugNameW));
            if (completed >= node.BreadcrumbCount) {
                Log(Channel::Gpu, Level::Info, "DRED: {}: {} 個のコマンドを全部終えている", where,
                    node.BreadcrumbCount);
                return;
            }
            Log(Channel::Gpu, Level::Error, "DRED: {}: {} 個中 {} 個まで終えた。次のコマンドで止まった", where,
                node.BreadcrumbCount, completed);
            const uint32_t first = completed > BREADCRUMB_CONTEXT_BEFORE ? completed - BREADCRUMB_CONTEXT_BEFORE : 0;
            const uint32_t last = std::min(node.BreadcrumbCount, completed + BREADCRUMB_CONTEXT_AFTER + 1);
            for (uint32_t index = first; index < last; ++index) {
                Log(Channel::Gpu, Level::Error, "DRED:   {} [{}] {}", index == completed ? "→" : " ", index,
                    BreadcrumbOpName(node.pCommandHistory[index]));
            }
        }

        void LogAllocations(std::string_view title, const D3D12_DRED_ALLOCATION_NODE1* node) {
            for (int count = 0; node != nullptr && count < MAX_LOGGED_ALLOCATIONS; node = node->pNext, ++count) {
                Log(Channel::Gpu, Level::Error, "DRED:   {}: {}(種類 {})", title, DebugNameOf(node->ObjectNameW),
                    static_cast<int>(node->AllocationType));
            }
        }

    }  // namespace

    const char* AdapterKindName(AdapterKind kind) {
        return kind == AdapterKind::Warp ? "WARP" : "hardware";
    }

    DeviceOptions DefaultDeviceOptions() {
#ifdef BICAMERAL_GPU_VALIDATION
        return {.debugLayer = true, .gpuBasedValidation = true, .dred = true};
#else
        return {};
#endif
    }

    // --- Device ---

    std::expected<Device, std::string> Device::Create(AdapterKind kind, const DeviceOptions& options) {
        EnableDebugFeatures(options);
        ComPtr<IDXGIFactory6> factory;
        const HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
        if (FAILED(result)) return std::unexpected("DXGI のファクトリを作れない: " + DescribeHresult(result));
        DeviceResult created = kind == AdapterKind::Warp ? CreateWarpDevice(factory.Get())
                                                         : CreateHardwareDevice(factory.Get(), options.presentMonitor);
        if (!created) return std::unexpected(created.error());

        Device device;
        device.m_factory = std::move(factory);
        device.m_device = std::move(*created);
        device.m_options = options;
        if (options.debugLayer || options.gpuBasedValidation) device.m_messageSink = AttachMessageSink(device.Get());
        return device;
    }

    std::unique_ptr<Device::MessageSink> Device::AttachMessageSink(ID3D12Device5* device) {
        auto sink = std::make_unique<MessageSink>();
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&sink->infoQueue)))) {
            Log(Channel::Gpu, Level::Warning, "ID3D12InfoQueue1 が無いので、debug layer の報告はログに出ない");
            return sink;  // 数は 0 のまま
        }
        if (!BICAMERAL_CHECK_HR(Channel::Gpu, sink->infoQueue->RegisterMessageCallback(
                                                  &Device::OnDebugMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, sink.get(),
                                                  &sink->callbackCookie))) {
            sink->infoQueue.Reset();
        }
        return sink;
    }

    Device::MessageSink::~MessageSink() {
        if (infoQueue) infoQueue->UnregisterMessageCallback(callbackCookie);
    }

    void __stdcall Device::OnDebugMessage(D3D12_MESSAGE_CATEGORY /*category*/, D3D12_MESSAGE_SEVERITY severity,
                                          D3D12_MESSAGE_ID id, LPCSTR description, void* context) {
        auto* sink = static_cast<MessageSink*>(context);
        const Level level = LevelOfSeverity(severity);
        if (level == Level::Error) sink->errorCount.fetch_add(1, std::memory_order_relaxed);
        if (level == Level::Warning) sink->warningCount.fetch_add(1, std::memory_order_relaxed);
        Log(Channel::Gpu, level, "D3D12 [{}] {}", static_cast<int>(id), description != nullptr ? description : "");
    }

    uint32_t Device::ValidationErrorCount() const {
        return m_messageSink ? m_messageSink->errorCount.load(std::memory_order_relaxed) : 0;
    }

    uint32_t Device::ValidationWarningCount() const {
        return m_messageSink ? m_messageSink->warningCount.load(std::memory_order_relaxed) : 0;
    }

    // --- デバイスの喪失 ---

    DeviceRemovedReport LogDeviceRemoved(ID3D12Device* device) {
        DeviceRemovedReport report;
        Log(Channel::Gpu, Level::Error, "デバイスが失われた: {}", DescribeHresult(device->GetDeviceRemovedReason()));

        ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) return report;
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs{};
        const HRESULT breadcrumbResult = dred->GetAutoBreadcrumbsOutput1(&breadcrumbs);
        if (FAILED(breadcrumbResult)) {
            Log(Channel::Gpu, Level::Warning, "DRED の記録が無い(debug プリセットで有効になる): {}",
                DescribeHresult(breadcrumbResult));
            return report;
        }
        report.dredAvailable = true;
        for (const D3D12_AUTO_BREADCRUMB_NODE1* node = breadcrumbs.pHeadAutoBreadcrumbNode; node != nullptr;
             node = node->pNext) {
            ++report.breadcrumbListCount;
            LogBreadcrumbNode(*node);
        }

        D3D12_DRED_PAGE_FAULT_OUTPUT1 pageFault{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&pageFault)) && pageFault.PageFaultVA != 0) {
            report.pageFaultAddress = pageFault.PageFaultVA;
            Log(Channel::Gpu, Level::Error, "DRED: ページフォールト: GPU の仮想アドレス {:#x}", pageFault.PageFaultVA);
            LogAllocations("そこにあるもの", pageFault.pHeadExistingAllocationNode);
            LogAllocations("最近解放したもの", pageFault.pHeadRecentFreedAllocationNode);
        }
        return report;
    }

}  // namespace bicameral::gpu
