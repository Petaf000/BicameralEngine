// editor_overlay.cpp — エディタの殻の Dear ImGui のパネル(T-0023)。考え方は editor_overlay.h。
//
// ImGui のヘッダは pch.h に入れない(pch.h は ImGui をリンクしないライブラリとテストでも使うので)。ImGui を使うのはこのファイルだけ。
// ImGui の DX12 backend は <d3d12.h> を include するが、pch.h の <directx/d3d12.h>(Agility SDK と同じ版。ADR-0004)が先に入っていて、
// 同じ include guard で飛ばされる。
#include "editor/editor_overlay.h"

#include <format>

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include "core/aliases.h"
#include "core/log.h"
#include "core/unicode.h"
#include "gpu/resources.h"
#include "platform/window.h"

// imgui_impl_win32.h は windows.h に依存しないよう、この宣言を #if 0 で囲んでいる(使う側が書く約束)
// NOLINTNEXTLINE(readability-identifier-naming) ImGui の名前
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace bicameral::editor {
    namespace {

        // フォントの画像(1.92 からは大きさごとに増える)と、将来の画像の表示に足りる数
        constexpr uint32_t SRV_DESCRIPTOR_COUNT = 64;
        constexpr float FONT_SIZE_PIXELS = 18.0f;
        constexpr float PANEL_MARGIN_PIXELS = 12.0f;

        // 日本語の字がある Windows の標準のフォント(上から順に探す)
        constexpr std::array<std::wstring_view, 3> JAPANESE_FONT_FILES = {L"meiryo.ttc", L"YuGothM.ttc",
                                                                          L"msgothic.ttc"};

        void Line(std::string_view text) {
            ImGui::TextUnformatted(text.data(), text.data() + text.size());
        }

        std::string FormatSpeed(double speed) {
            return speed >= 1.0 ? std::format("×{:g}", speed) : std::format("×1/{:g}", 1.0 / speed);
        }

        // 2 つの依頼を 1 つに(キーとボタンが同じフレームに来たとき)
        TimeRequest Merge(const TimeRequest& a, const TimeRequest& b) {
            return {.togglePause = a.togglePause != b.togglePause,
                    .stepTicks = a.stepTicks + b.stepTicks,
                    .speedSteps = a.speedSteps + b.speedSteps,
                    .resetSpeed = a.resetSpeed || b.resetSpeed};
        }

    }  // namespace

    // --- 作る ---

    EditorOverlay::EditorOverlay(Window& window, uint32_t frameCount) : m_window(window), m_frames(frameCount) {}

    std::expected<std::unique_ptr<EditorOverlay>, std::string> EditorOverlay::Create(
        Window& window, ID3D12Device* device, gpu::Queue& direct, DXGI_FORMAT format, uint32_t frameCount) {
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) コンストラクタが private なので make_unique を使えない
        std::unique_ptr<EditorOverlay> overlay(new EditorOverlay(window, frameCount));
        if (!overlay->CreateGpuObjects(device, frameCount))
            return std::unexpected("エディタの描画のリソースを作れない");

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        overlay->m_contextCreated = true;

        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;  // 窓の配置を exe の横に書かない(テストの作業フォルダを汚さない)
        ImGui::StyleColorsDark();
        overlay->LoadJapaneseFont();

        if (!overlay->InitializeBackends(device, direct, format))
            return std::unexpected("ImGui の Win32 / DX12 の backend を作れない");

        window.SetMessageHook([](HWND handle, UINT message, WPARAM wParam, LPARAM lParam) {
            return ImGui_ImplWin32_WndProcHandler(handle, message, wParam, lParam) != 0;
        });

        Log(Channel::Render, Level::Info, "エディタの殻(Dear ImGui {})を作った", IMGUI_VERSION);

        return overlay;
    }

    // 呼ぶ側が direct キューを待ってから壊す(frame/frame_loop の Finish)
    EditorOverlay::~EditorOverlay() {
        m_window.SetMessageHook({});
        if (m_dx12Initialized)
            ImGui_ImplDX12_Shutdown();

        if (m_win32Initialized)
            ImGui_ImplWin32_Shutdown();

        if (m_contextCreated)
            ImGui::DestroyContext();
    }

    bool EditorOverlay::CreateGpuObjects(ID3D12Device* device, uint32_t frameCount) {
        const D3D12_DESCRIPTOR_HEAP_DESC heapDesc{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                  .NumDescriptors = SRV_DESCRIPTOR_COUNT,
                                                  .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap))))
            return false;

        m_srvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (uint32_t slot = SRV_DESCRIPTOR_COUNT; slot > 0; --slot)
            m_freeSrvSlots.push_back(slot - 1);

        for (FrameSlot& frame : m_frames) {
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator))))
                return false;
        }

        if (frameCount == 0 ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_frames.front().allocator.Get(),
                                             nullptr, IID_PPV_ARGS(&m_list)))) {
            return false;
        }

        return SUCCEEDED(m_list->Close());
    }

    bool EditorOverlay::InitializeBackends(ID3D12Device* device, gpu::Queue& direct, DXGI_FORMAT format) {
        m_win32Initialized = ImGui_ImplWin32_Init(m_window.Handle());
        if (!m_win32Initialized)
            return false;

        ImGui_ImplDX12_InitInfo info;
        info.Device = device;
        info.CommandQueue = direct.Native();  // フォントの画像の転送に使う
        info.NumFramesInFlight = static_cast<int>(m_frames.size());
        info.RTVFormat = format;
        info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        info.UserData = this;
        info.SrvDescriptorHeap = m_srvHeap.Get();
        info.SrvDescriptorAllocFn = &EditorOverlay::AllocateDescriptor;
        info.SrvDescriptorFreeFn = &EditorOverlay::FreeDescriptor;
        m_dx12Initialized = ImGui_ImplDX12_Init(&info);

        return m_dx12Initialized;
    }

    // 標準のフォントには日本語の字が無い。1.92 のフォントは使う字だけを後から作るので、字の範囲は要らない
    void EditorOverlay::LoadJapaneseFont() {
        std::array<wchar_t, MAX_PATH> windows{};
        const UINT length = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
        const fs::path fonts = fs::path(std::wstring_view(windows.data(), length)) / L"Fonts";

        for (const std::wstring_view file : JAPANESE_FONT_FILES) {
            const fs::path path = fonts / file;
            std::error_code error;
            if (!fs::exists(path, error))
                continue;

            const std::string utf8 = ToUtf8(path.wstring());
            if (ImGui::GetIO().Fonts->AddFontFromFileTTF(utf8.c_str(), FONT_SIZE_PIXELS) != nullptr)
                return;
        }

        Log(Channel::Render, Level::Warning, "日本語のフォントが見つからない(パネルの日本語は ? になる)");
    }

    // --- SRV の記述子 ---

    void EditorOverlay::AllocateDescriptor(::ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
                                           D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle) {
        auto* overlay = static_cast<EditorOverlay*>(info->UserData);
        IM_ASSERT(!overlay->m_freeSrvSlots.empty());  // 足りなければ SRV_DESCRIPTOR_COUNT を増やす

        const uint32_t slot = overlay->m_freeSrvSlots.back();
        overlay->m_freeSrvSlots.pop_back();

        *cpuHandle = overlay->m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle->ptr += size_t{slot} * overlay->m_srvIncrement;
        *gpuHandle = overlay->m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        gpuHandle->ptr += uint64_t{slot} * overlay->m_srvIncrement;
    }

    void EditorOverlay::FreeDescriptor(::ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
                                       D3D12_GPU_DESCRIPTOR_HANDLE /*gpuHandle*/) {
        auto* overlay = static_cast<EditorOverlay*>(info->UserData);
        const size_t start = overlay->m_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
        overlay->m_freeSrvSlots.push_back(static_cast<uint32_t>((cpuHandle.ptr - start) / overlay->m_srvIncrement));
    }

    // --- 入力 ---

    bool EditorOverlay::WantsPointer() const {
        return ImGui::GetIO().WantCaptureMouse;
    }

    bool EditorOverlay::WantsKeyboard() const {
        return ImGui::GetIO().WantCaptureKeyboard;
    }

    // Space = 止める / 動かす、N = 1 刻み(押しっぱなしで続けて進む)。文字を打っている間は効かない
    TimeRequest EditorOverlay::TakeShortcuts(const TimeControl& time) const {
        if (WantsKeyboard())
            return {};

        TimeRequest request;
        request.togglePause = ImGui::IsKeyPressed(ImGuiKey_Space, false);
        if (time.Paused() && ImGui::IsKeyPressed(ImGuiKey_N, true))
            request.stepTicks = 1;

        return request;
    }

    // --- パネル ---

    TimeRequest EditorOverlay::Build(const EditorStatus& status, const TimeControl& time) {
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const TimeRequest keys = TakeShortcuts(time);
        const TimeRequest buttons = BuildTimePanel(status, time);
        BuildStatusPanel(status);

        ImGui::Render();

        return Merge(keys, buttons);
    }

    TimeRequest EditorOverlay::BuildTimePanel(const EditorStatus& status, const TimeControl& time) const {
        TimeRequest request;
        ImGui::SetNextWindowPos({PANEL_MARGIN_PIXELS, PANEL_MARGIN_PIXELS}, ImGuiCond_FirstUseEver);
        ImGui::Begin("時間", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        // --- 止める・1 刻み ---
        if (ImGui::Button(time.Paused() ? "動かす (Space)" : "止める (Space)"))
            request.togglePause = true;

        ImGui::SameLine();
        ImGui::BeginDisabled(!time.Paused());
        if (ImGui::Button("1 刻み (N)"))
            request.stepTicks += 1;

        ImGui::SameLine();
        if (ImGui::Button("60 刻み"))
            request.stepTicks += 60;

        ImGui::EndDisabled();

        // --- 速さ ---
        if (ImGui::ArrowButton("##slower", ImGuiDir_Left))
            request.speedSteps -= 1;

        ImGui::SameLine();
        Line(std::format("速さ {}", FormatSpeed(time.Speed())));
        ImGui::SameLine();
        if (ImGui::ArrowButton("##faster", ImGuiDir_Right))
            request.speedSteps += 1;

        ImGui::SameLine();
        if (ImGui::Button("等速"))
            request.resetSpeed = true;

        // --- 今の刻み ---
        ImGui::Separator();
        if (!time.Paused())
            Line(std::format("動いている: 刻み {}", status.tick));
        else if (status.unit == 0)
            Line(std::format("止まっている: 刻み {} の前", status.tick));
        else
            Line(std::format("止めている: 刻み {} の残りを進めている", status.tick));

        Line(std::format("1 刻みずつ進めた: {} 刻み", time.SteppedTicks()));
        ImGui::End();

        return request;
    }

    void EditorOverlay::BuildStatusPanel(const EditorStatus& status) const {
        ImGui::SetNextWindowPos({PANEL_MARGIN_PIXELS, 170.0f}, ImGuiCond_FirstUseEver);
        ImGui::Begin("状態", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        // --- 世界(読み戻しは数フレーム遅れる)---
        Line(std::format("刻み {}(単位 {} / {})  未処理 {:.1f} 刻み  捨てた刻み {}", status.tick, status.unit,
                         status.unitsPerTick, status.pendingTicks, status.droppedTicks));
        Line(std::format("S({}) = {:016x}", status.hashedTick, status.worldHash));
        Line(std::format("エネルギー {} mJ  計算したブロック {}", static_cast<int64_t>(status.energyMillijoules),
                         status.scheduledBlocks));

        // --- 重さ(直近 1 秒)---
        ImGui::Separator();
        Line(std::format("{:.1f} fps  世界 {:.1f} 刻み/秒", status.framesPerSecond, status.ticksPerSecond));
        Line(std::format("CPU {:.2f} ms/フレーム", status.cpuMilliseconds));
        Line(std::format("シミュ GPU {:.2f} ms/投入(予算 {:.2f})", status.simGpuMilliseconds,
                         status.budgetMilliseconds));
        Line(std::format("描画 GPU {:.2f} ms/フレーム", status.renderGpuMilliseconds));

        // --- 入力と表示 ---
        ImGui::Separator();
        Line(std::format("表示: {}", status.view));
        if (status.replaying)
            Line("再生中(窓の操作は世界に入らない)");

        if (status.recording)
            Line("記録中");

        Line(std::format("GPU に渡す前のコマンド: {}", status.waitingCommands));

        if (ImGui::CollapsingHeader("操作")) {
            Line("左クリック: つつく(断面)  Shift + 左: 押す");
            Line("右ドラッグ: 回る  中ドラッグ: 平行移動  ホイール: 寄る");
            Line("1/2/3: 表示  X/Y/Z・Q/E: 断面  L: 対数  B: 活性  O: 透視  R: 戻す");
            Line("T: トレース  P: 覗き窓  Space: 止める  N: 1 刻み");
        }

        ImGui::End();
    }

    // --- 描く ---

    bool EditorOverlay::Submit(gpu::Queue& direct, ID3D12Resource* backBuffer,
                               D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView) {
        FrameSlot& frame = m_frames[m_submitCount % m_frames.size()];
        if (frame.fence != 0 && !direct.IsComplete(frame.fence))
            direct.WaitCpu(frame.fence);  // スワップチェインの待ちがあるので、ふつうは起きない

        if (FAILED(frame.allocator->Reset()) || FAILED(m_list->Reset(frame.allocator.Get(), nullptr)))
            return false;

        const D3D12_RESOURCE_BARRIER toRenderTarget = gpu::Transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT,
                                                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_list->ResourceBarrier(1, &toRenderTarget);
        m_list->OMSetRenderTargets(1, &renderTargetView, FALSE, nullptr);

        ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Get()};
        m_list->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_list.Get());

        const D3D12_RESOURCE_BARRIER toPresent = gpu::Transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                                 D3D12_RESOURCE_STATE_PRESENT);
        m_list->ResourceBarrier(1, &toPresent);
        if (FAILED(m_list->Close()))
            return false;

        frame.fence = direct.Submit(m_list.Get());
        ++m_submitCount;

        return true;
    }

}  // namespace bicameral::editor
