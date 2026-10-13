// editor_overlay.h — エディタの殻: 窓に重ねる Dear ImGui のパネル(T-0023、docs/design/14 §1〜§3・§2「実装(T-0023)」)。
//
// データの流れ:
//   窓のメッセージ → Window の MessageHook → ImGui(Win32 の backend)。ImGui が使っている入力は、フレームのループが
//   WantsPointer / WantsKeyboard を見てカメラとつつきに渡さない。
//   フレームのループ → EditorStatus(読み戻した刻み・ハッシュ・重さ。読むだけ)→ Build がパネルを作る
//   → 押されたボタンは TimeRequest(editor/time_control)で返す。世界には触れない(CPU は View と Controller。D-107)。
//   → Submit がシーンの描画の後・Present の前に、バックバッファへパネルを重ねるリストを direct キューへ投げる。
// エディタは CPU を好きなだけ使ってよい(ADR-0001)。世界を変える操作は、ここからもコマンド(sim/command.h)として渡す
// (実験室〔editor/lab_panel〕の箱に物を置く操作もコマンド。T-0142)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "editor/brush_panel.h"
#include "editor/brush_preview.h"
#include "editor/copy_panel.h"
#include "editor/gauge_panel.h"
#include "editor/graph_panel.h"
#include "editor/lab_panel.h"
#include "editor/mixer_panel.h"
#include "editor/patch_panel.h"
#include "editor/reaction_table_panel.h"
#include "editor/time_control.h"
#include "gpu/com_ptr.h"
#include "gpu/queue.h"

struct ImGui_ImplDX12_InitInfo;

namespace bicameral {
    class Window;
}

namespace bicameral::editor {

    // パネルに出す 1 フレームの状態(フレームのループが詰める)
    // 反応表とホットリロード(T-0139。frame/table_hot_reload)
    struct ReactionTableStatus {
        uint64_t version = 0;       // 世界が使っている表の版
        uint64_t worldVersion = 0;  // 世界を作った(刻み 0 の)表の版(違えば作り直すと初めのセル・物が替わる。T-0453)
        uint32_t swaps = 0;         // 差し替えた回数
        uint32_t failures = 0;      // 読み直せなかった回数
        bool watching = false;      // ファイルを見ている(再生中は見ない)
        bool waiting = false;       // 読めた表が刻みの境界を待っている(止めている間など)
        bool lastFailed = false;
        bool modifiedWorld = false;  // 「改造された世界」(プレイ中に法則が変わった・印付きの Mod。D-437・D-438)
        std::string message;         // 最後の読み直しの結果(誤りならファイル:行:列: 何が違うか)
    };

    struct EditorStatus {
        // --- 世界の時間 ---
        uint64_t tick = 0;  // 次に投げる単位の刻み(SimScheduler のカーソル)
        uint32_t unit = 0;  // 刻みの中の単位の番号(0 = 刻みの境界)
        uint32_t unitsPerTick = 1;
        double pendingTicks = 0.0;  // 現実の時間からみて、まだ始めていない刻み
        uint64_t droppedTicks = 0;  // 追いつけずに捨てた刻み(世界が遅れた分。D-202)

        // --- 最後に読み戻した刻みの状態(待たない読み戻し。数フレーム遅れる)---
        uint64_t hashedTick = 0;
        uint64_t worldHash = 0;  // セル + 物(再生ファイルで突き合わせる値)
        uint64_t energyMillijoules = 0;
        uint32_t scheduledBlocks = 0;

        // --- 重さ(直近の 1 秒)---
        double framesPerSecond = 0.0;
        double ticksPerSecond = 0.0;
        double cpuMilliseconds = 0.0;
        double simGpuMilliseconds = 0.0;  // シミュのリスト 1 回あたり
        double renderGpuMilliseconds = 0.0;
        double budgetMilliseconds = 0.0;

        // --- 重さの見張り(安全網の警告。T-0413・D-468)---
        double tickBudgetMilliseconds = 0.0;  // 等速の 1 刻みの予算
        uint64_t overBudgetTicks = 0;         // 起動からの、等速の予算を超えた刻み
        frame::WeightSpan lastOverBudget;     // 最後に警告した区間

        // --- 入力と表示 ---
        std::string view;  // デバッグ表示の設定(DebugViewController::Describe)
        bool replaying = false;
        bool recording = false;
        size_t waitingCommands = 0;  // まだ GPU に渡していないコマンド(止めている間のつつきはここで待つ)

        // --- 巻き戻し(T-0143)---
        std::vector<uint64_t> savePointTicks;  // 戻れる保存点の刻み(昇順)
        uint64_t saveIntervalTicks = 0;        // 何刻みごとに写すか
        std::string rewindUnavailable;         // 空でなければ巻き戻せない理由

        // --- Work Graphs と性能(直近 1 秒。T-0143)---
        GraphPanelStatus graph;

        // --- 反応表(T-0139)---
        ReactionTableStatus reactionTable;

        // --- 筆の形(カーソルの下。画面の線分。筆を持っていない・カーソルが断面に当たらない・パネルの上なら空。T-0279)---
        std::vector<BrushPreviewLine> brushPreview;

        // --- 魔法のパッチの範囲(仮の世界の画面の線分。T-0325)---
        std::vector<BrushPreviewLine> patchRanges;

        // --- 写しの早送り(T-0412)---
        CopyProgress copy;
    };

    class EditorOverlay {
    public:
        // ImGui の文脈と Win32・DX12 の backend を作る。frameCount はバックバッファの数(リストを使い回す枠の数)
        [[nodiscard]] static std::expected<std::unique_ptr<EditorOverlay>, std::string> Create(
            Window& window, ID3D12Device* device, gpu::Queue& direct, DXGI_FORMAT format, uint32_t frameCount);

        ~EditorOverlay();
        EditorOverlay(const EditorOverlay&) = delete;
        EditorOverlay& operator=(const EditorOverlay&) = delete;
        EditorOverlay(EditorOverlay&&) = delete;
        EditorOverlay& operator=(EditorOverlay&&) = delete;

        // ImGui がポインタ・キーを使っている(前のフレームのパネルの上にある・文字を打っている)
        [[nodiscard]] bool WantsPointer() const;
        [[nodiscard]] bool WantsKeyboard() const;

        // パネルを作る(ImGui の NewFrame〜Render)。押されたボタンとキー(Space・N)を時間の操作の依頼にして返す
        [[nodiscard]] TimeRequest Build(const EditorStatus& status, const TimeControl& time);

        // Build で作ったパネルをバックバッファに重ねるリストを記録して direct キューへ投げる。
        // backBuffer は PRESENT の状態で受け取り、PRESENT に戻す。記録に失敗したら false
        [[nodiscard]] bool Submit(gpu::Queue& direct, ID3D12Resource* backBuffer,
                                  D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView);

        // 実験室のパネル(T-0142)
        [[nodiscard]] LabPanel& Lab() { return *m_lab; }

        // 計器と比べる画面(T-0221。実験室の箱の値)
        [[nodiscard]] GaugePanel& Gauge() { return *m_gauge; }

        // 反応表のパネル(T-0219)
        [[nodiscard]] ReactionTablePanel& ReactionTable() { return *m_reactionTable; }

        // 世界に物を置く筆(T-0222)
        [[nodiscard]] BrushPanel& Brush() { return *m_brush; }

        // 魔法のパッチを置くパネル(T-0325)
        [[nodiscard]] PatchPanel& Patch() { return *m_patch; }

        // 環境ミキサー(T-0452。標準パッケージを世界に足す・外す)
        [[nodiscard]] MixerPanel& Mixer() { return *m_mixer; }

        // 写しの早送り(T-0412。今の世界の写しを別に早送りして測る)
        [[nodiscard]] CopyPanel& Copy() { return *m_copy; }

    private:
        EditorOverlay(Window& window, uint32_t frameCount);

        // --- 作る ---
        [[nodiscard]] bool CreateGpuObjects(ID3D12Device* device, uint32_t frameCount);
        [[nodiscard]] bool InitializeBackends(ID3D12Device* device, gpu::Queue& direct, DXGI_FORMAT format);
        void LoadJapaneseFont();

        // --- ImGui の DX12 backend に渡す SRV の記述子(フォントの画像など。1.92 から複数使う)---
        static void AllocateDescriptor(::ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
                                       D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
        static void FreeDescriptor(::ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
                                   D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle);

        // --- パネル ---
        [[nodiscard]] TimeRequest BuildTimePanel(const EditorStatus& status, const TimeControl& time) const;
        [[nodiscard]] uint64_t BuildRewind(const EditorStatus& status) const;
        void BuildStatusPanel(const EditorStatus& status) const;
        static void DrawBrushPreview(std::span<const BrushPreviewLine> lines);
        void BuildReactionTable(const ReactionTableStatus& table) const;
        [[nodiscard]] TimeRequest TakeShortcuts(const TimeControl& time) const;

        // --- 描画の枠(バックバッファごと)---
        struct FrameSlot {
            ComPtr<ID3D12CommandAllocator> allocator;
            uint64_t fence = 0;  // このリストを最後に投げた direct のフェンスの値(0 = まだ)
        };

        Window& m_window;
        std::unique_ptr<LabPanel> m_lab;
        std::unique_ptr<GaugePanel> m_gauge;
        std::unique_ptr<ReactionTablePanel> m_reactionTable;
        std::unique_ptr<BrushPanel> m_brush;
        std::unique_ptr<PatchPanel> m_patch;
        std::unique_ptr<MixerPanel> m_mixer;
        std::unique_ptr<CopyPanel> m_copy;
        bool m_contextCreated = false;
        bool m_win32Initialized = false;
        bool m_dx12Initialized = false;

        std::vector<FrameSlot> m_frames;  // ImGui の DX12 backend の頂点のバッファと同じ順で回す(投げるたびに 1 つ進む)
        uint64_t m_submitCount = 0;
        ComPtr<ID3D12GraphicsCommandList> m_list;

        // --- SRV の記述子の山(shader visible)と空きの番号 ---
        ComPtr<ID3D12DescriptorHeap> m_srvHeap;
        uint32_t m_srvIncrement = 0;
        std::vector<uint32_t> m_freeSrvSlots;
    };

}  // namespace bicameral::editor
