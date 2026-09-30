// probe_sim.h — 仮の刻み(shaders/common/probe_sim.hlsli)を「単位」の列として GPU で走らせる道具と、その CPU リファレンス
// (T-0004・T-0012・T-0086・T-0005)。
//
// 刻みのループの形(06 §4・ADR-0011)と、Work Graphs の伝播(T-0005)を確かめる。中身(64³ の格子の熱の伝導)は段ごとに本物に置き換える:
//   - 伝導の単位は Work Graph(shaders/sim/probe_conduct.hlsl)。入力は GPU が作る活性の一覧(DispatchGraph の GPU の入力)なので、
//     どこが活性か・何ブロック計算するかを CPU は知らない(D-107)。熱が広がっている所だけが計算される。
//   - 1 刻み = 決まった数の単位(適用 → 伝導 → 重さ × k → ハッシュ)。フレームの切れ目はどの単位の間にも来てよい(刻みはフレームをまたぐ)。
//   - CPU は毎フレーム、そのフレームに投げる単位(何番目の刻みの何番目から何個)を、フレームの枠ごとのリストに記録して投げる。
//     同じリストは前の実行が終わるまで投げ直せない(debug layer [553])ので、使い回す記録済みのリストではなく毎フレーム記録する。
//     CPU が書くのはコマンドの並び・ルート定数・Dispatch だけ(世界の状態には触れない。D-107)。
//   - 単位ごとのタイムスタンプを取り、CPU は単位の GPU 時間から次のフレームに投げる数を決める(frame/sim_scheduler)。
//   - 刻みの最後の単位が状態のハッシュを GPU で取り、刻みの中のイベントを並べてリングへ写す(06 §2 段 9)。フレームの終わりに CPU へ読み戻す(待たない)。
//   - コマンドはフレームのリストの先頭で GPU のコマンドキューへ足す。コマンドは自分の刻みの適用の単位まで GPU で待つ(06 §3)。
//     CPU の約束: 足すコマンドは (targetTick, sequence) の昇順で、targetTick はまだ記録していない最初の適用の刻み(NextApplyTick)以上、
//     数は FreeCommandSlots() 以下(RecordFrame が確かめる)。
//   - 抽出(描画が読む)は、投げた単位の後ろで、刻みの境界の状態を写す(1 フレームに 1 回まで)。
//
// 使い方:
//   auto sim = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, {.busyIterations = n, .busyPieces = k});
//   ID3D12CommandList* list = sim->RecordFrame(slot, {.firstTick = t, .firstUnit = u, .unitCount = c, .commands = 新しいコマンド, ...});
//   computeQueue.Submit(list) → フェンスが進んだら sim->ReadFrame(slot)
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "common/probe_sim.hlsli"
#include "gpu/debug_ring.h"
#include "gpu/readback_ring.h"
#include "gpu/work_graph.h"
#include "sim/command.h"

namespace bicameral::sim {

    // --- コマンド(06 §3 の 64 バイト。sim/command.h)---

    using ProbeCommand = Command;
    static_assert(sizeof(ProbeCommand) == PROBE_COMMAND_BYTES);

    // セル (x, y, z) に PROBE_POKE_AMOUNT の熱を足す
    [[nodiscard]] ProbeCommand MakePokeCommand(uint64_t targetTick, uint32_t sequence, uint32_t x, uint32_t y,
                                               uint32_t z);

    // --- GPU から戻ってくるもの ---

    struct ProbeEvent {
        uint64_t tick = 0;
        uint32_t type = 0;   // PROBE_EVENT_*
        uint32_t place = 0;  // つつき: x | y << 8 | z << 16(ProbePokePlace)。遅れたコマンド: コマンドの種類

        [[nodiscard]] uint32_t PokeX() const { return place & 0xFFu; }
        [[nodiscard]] uint32_t PokeY() const { return (place >> 8) & 0xFFu; }
        [[nodiscard]] uint32_t PokeZ() const { return (place >> 16) & 0xFFu; }
        friend bool operator==(const ProbeEvent&, const ProbeEvent&) = default;
    };

    // 刻み tick の始めの状態 S(tick) の要約(ハッシュの表の欄。probe_sim.hlsli)
    struct ProbeTickHash {
        uint64_t tick = 0;
        uint64_t hash = 0;             // ProbeStateHash
        uint64_t heat = 0;             // 熱の合計(ProbeHeatSum。つつき以外で変わらない)
        uint32_t scheduledBlocks = 0;  // S(tick) を作った刻み(tick − 1)で伝導を計算したブロックの数
    };

    struct ProbeFrameReadback {
        std::vector<ProbeEvent> events;      // (刻み, 種類, 場所) の順(刻みの最後に GPU が並べる。分け方に依存しない)
        uint32_t droppedEventCount = 0;      // 容量を超えて書けなかった数(刻みの一時置き場 + リング)
        std::vector<ProbeTickHash> hashes;   // このフレームで終えた刻みの状態(刻みの順)
        std::vector<uint64_t> unitGpuTicks;  // 投げた i 番目の単位の GPU 時間(タイムスタンプの刻み)
        uint32_t firstUnit = 0;              // unitGpuTicks[0] の単位の、刻みの中の番号
        uint64_t gpuBeginTimestamp = 0;      // リスト全体(抽出と読み戻しを含む)の始めと終わり
        uint64_t gpuEndTimestamp = 0;
        uint32_t debugAssertCount = 0;  // シェーダーの assert の数(中身はログに出る)
    };

    struct ProbeFrameInput {
        uint64_t firstTick = 0;         // 最初の単位の刻み
        uint32_t firstUnit = 0;         // 最初の単位の、刻みの中の番号(0〜UnitsPerTick()-1)
        uint32_t unitCount = 0;         // 投げる単位の数(0〜MAX_UNITS_PER_FRAME。0 なら抽出だけ)
        bool extract = false;           // 単位の後ろで、刻みの境界の状態を抽出へ写すか
        uint32_t extractionTarget = 0;  // 抽出の書き先(0〜PROBE_EXTRACTION_COUNT-1)
        std::span<const ProbeCommand>
            commands;  // GPU のキューへ足す新しいコマンド(最大 PROBE_MAX_COMMANDS。約束はファイルの先頭)
    };

    // 重さの試験(R-LOOP-2)。世界の結果には入らない
    struct ProbeSimOptions {
        uint32_t busyIterations =
            0;  // 1 刻みに足す繰り返しの合計(0 なら重さの単位は無し。上限 PROBE_BUSY_ITERATIONS_LIMIT)
        uint32_t busyPieces = 1;  // それを何個の単位に分けるか(1〜PROBE_MAX_BUSY_PIECES)
    };

    // --- GPU で走らせる ---

    class ProbeSim {
    public:
        static constexpr uint32_t FRAME_SLOT_COUNT = 4;  // 同時に GPU にあってよいフレームのリストの数
        static constexpr uint32_t MAX_UNITS_PER_FRAME = 256;

        // listType: フレームのリストを投げるキューの種類(シミュは compute。06 §4)
        [[nodiscard]] static std::expected<ProbeSim, std::string> Create(ID3D12Device5* device,
                                                                         D3D12_COMMAND_LIST_TYPE listType,
                                                                         const ProbeSimOptions& options = {});

        // 1 刻みの単位の数(適用・伝導・ハッシュ + 重さの単位)
        [[nodiscard]] uint32_t UnitsPerTick() const { return PROBE_FIXED_UNITS_PER_TICK + BusyUnitCount(); }
        [[nodiscard]] uint32_t HashUnit() const { return UnitsPerTick() - 1; }

        // 次に記録する単位が (tick, unit) のとき、まだ記録していない最初の適用の単位の刻み。
        // そのフレームに足すコマンドの targetTick はこれ以上でなければならない(でなければ適用に間に合わない)
        [[nodiscard]] static uint64_t NextApplyTick(uint64_t tick, uint32_t unit) {
            return unit == 0 ? tick : tick + 1;
        }

        // GPU のコマンドキューの空き(記録した適用の単位で取り出される分を引いた、次のフレームに足せる数)
        [[nodiscard]] uint32_t FreeCommandSlots() const { return PROBE_COMMAND_QUEUE_CAPACITY - m_queuedCommandCount; }

        // slot のアップロードのバッファに入力を書き、slot のリストに単位を記録して返す。
        // 呼ぶ側の約束: slot の前のリストを GPU が終えている。入力が範囲外・記録の失敗なら nullptr(理由はログ)
        [[nodiscard]] ID3D12CommandList* RecordFrame(uint32_t slot, const ProbeFrameInput& input);

        // slot のリストを GPU が終えた後に呼ぶ(待たない。終わったかどうかは呼ぶ側がフェンスで見る)
        [[nodiscard]] ProbeFrameReadback ReadFrame(uint32_t slot) const;

        // 描画用の抽出(0〜PROBE_EXTRACTION_COUNT-1。z = PROBE_VIEW_Z の面、PROBE_SLICE_CELL_COUNT 個)。描画は読むだけ
        [[nodiscard]] ID3D12Resource* Extraction(uint32_t target) const { return m_extractions[target].Get(); }

        // 伝導の Work Graph の裏のメモリ(ドライバが決める。docs/perf.md に残す)
        [[nodiscard]] uint64_t ConductBackingMemoryBytes() const { return m_conductGraph->BackingMemoryBytes(); }

    private:
        struct FrameSlot {
            Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
            Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList10> list;  // 毎フレーム記録し直す
            Microsoft::WRL::ComPtr<ID3D12Resource> input;              // アップロード(PROBE_INPUT_BYTES)
            std::byte* mappedInput = nullptr;                          // Map したまま
            Microsoft::WRL::ComPtr<ID3D12Resource> timestampReadback;  // (MAX_UNITS_PER_FRAME + 2) × 8 バイト
            Microsoft::WRL::ComPtr<ID3D12Resource> hashReadback;       // PROBE_HASH_BYTES
            // 最後に記録した範囲(ReadFrame が、どの刻みのハッシュとどの単位の時間かを知るため)
            uint64_t firstTick = 0;
            uint32_t firstUnit = 0;
            uint32_t unitCount = 0;
        };

        ProbeSim(const ProbeSimOptions& options, gpu::ReadbackRing&& events, gpu::DebugRing&& debugRing)
            : m_options(options), m_events(std::move(events)), m_debugRing(std::move(debugRing)) {}

        [[nodiscard]] uint32_t BusyUnitCount() const { return m_options.busyIterations > 0 ? m_options.busyPieces : 0; }
        [[nodiscard]] bool CreatePipelines(ID3D12Device5* device);
        [[nodiscard]] bool CreateConductGraph(ID3D12Device5* device);
        [[nodiscard]] bool CreateBuffers(ID3D12Device5* device);
        [[nodiscard]] bool CreateFrameSlots(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE listType);
        [[nodiscard]] bool ValidateInput(uint32_t slot, const ProbeFrameInput& input) const;
        [[nodiscard]] bool ValidateCommands(const ProbeFrameInput& input) const;
        void WriteInput(FrameSlot& frame, const ProbeFrameInput& input) const;
        void RecordEnqueue(ID3D12GraphicsCommandList10* list, uint32_t commandCount) const;
        void TrackCommands(std::span<const ProbeCommand> commands, uint64_t nextApplyTick);
        void BindRootArguments(ID3D12GraphicsCommandList10* list, ID3D12Resource* input) const;
        void BindRootViews(ID3D12GraphicsCommandList10* list, ID3D12Resource* input) const;
        void RecordUnit(ID3D12GraphicsCommandList10* list, ID3D12Resource* input, uint64_t tick, uint32_t unit);
        void RecordConduct(ID3D12GraphicsCommandList10* list, ID3D12Resource* input, uint64_t tick);
        void RecordActiveListStates(ID3D12GraphicsCommandList10* list, D3D12_RESOURCE_STATES before,
                                    D3D12_RESOURCE_STATES after) const;
        void RecordExtract(ID3D12GraphicsCommandList10* list, uint64_t tick, uint32_t target) const;
        void RecordReadbacks(ID3D12GraphicsCommandList10* list, uint32_t slot, bool hasHash) const;
        [[nodiscard]] std::vector<ProbeTickHash> ReadHashes(const FrameSlot& frame) const;

        ProbeSimOptions m_options;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_enqueuePipeline;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_applyPipeline;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_busyPipeline;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_hashCellsPipeline;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_flushEventsPipeline;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_extractPipeline;
        std::unique_ptr<gpu::WorkGraph> m_conductGraph;  // 伝導(shaders/sim/probe_conduct.hlsl)
        uint32_t m_conductEntrypoint = 0;                // WakeBlocks の入口の番号
        bool m_conductInitialized = false;               // 裏のメモリを初期化するリストを記録したか(最初の 1 回だけ)

        Microsoft::WRL::ComPtr<ID3D12Resource> m_world;  // 2 世代 × PROBE_CELL_COUNT × uint32
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, PROBE_EXTRACTION_COUNT> m_extractions;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_busySink;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_hashes;        // ハッシュの表(PROBE_HASH_BYTES)
        Microsoft::WRL::ComPtr<ID3D12Resource> m_commandQueue;  // GPU のコマンドキュー(PROBE_COMMAND_QUEUE_BYTES)
        Microsoft::WRL::ComPtr<ID3D12Resource> m_tickEvents;  // 刻みの中のイベントの一時置き場(PROBE_TICK_EVENT_BYTES)
        // 活性の一覧(刻みの偶奇で 2 組。PROBE_ACTIVE_LIST_BYTES)。フレームの中では UAV、伝導の間だけ入力の組を GPU の入力の状態にする
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> m_activeLists;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_blockSchedule;  // 予定の印(PROBE_SCHEDULE_BYTES)
        gpu::ReadbackRing m_events;
        gpu::DebugRing m_debugRing;
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_timestamps;  // slot ごとに MAX_UNITS_PER_FRAME + 2
        std::array<FrameSlot, FRAME_SLOT_COUNT> m_slots;

        // --- GPU のコマンドキューの CPU 側の控え(足すのは CPU だけなので、末尾と待っている数を CPU が知っている)---
        struct QueuedTick {
            uint64_t targetTick = 0;
            uint32_t count = 0;
        };
        uint32_t m_commandTail = 0;             // 足した総数(GPU の末尾と同じ。2^32 で一周)
        uint32_t m_queuedCommandCount = 0;      // まだ適用の単位を記録していないコマンドの数
        std::vector<QueuedTick> m_queuedTicks;  // その内訳(targetTick の昇順)
        bool m_hasEnqueued = false;
        ProbeCommand m_lastEnqueued;  // 最後に足したコマンド(並びの確認)
    };

    // --- CPU リファレンス(GPU とビット一致するはずのもの。D-307・CLAUDE.md 原則 4)---

    // 全部のセルを毎刻み計算する(活性を使わない)。GPU は活性のブロックだけを計算するので、一致すれば活性の取り方も正しい。
    // 予定のブロックの数は、変わったブロックの記録から GPU と同じ規則(probe_sim.hlsli の「活性」)で予想する
    class ProbeReference {
    public:
        ProbeReference();

        // 刻み tick を 1 つ進める(targetTick == tick のコマンドを並びの順に適用 → 伝導)
        void Advance(uint64_t tick, std::span<const ProbeCommand> commands);

        // 刻み tick の始めの状態 S(tick)(= tick 回進めた後)
        [[nodiscard]] std::span<const uint32_t> State(uint64_t tick) const;

        // 最後の Advance で GPU が伝導を計算するはずのブロックの数
        [[nodiscard]] uint32_t ScheduledBlocks() const { return m_scheduledBlocks; }

    private:
        std::vector<uint32_t> m_cells;         // 2 世代 × PROBE_CELL_COUNT(GPU と同じ並び)
        std::vector<uint8_t> m_changedBlocks;  // 前の刻みで値が変わったブロック(PROBE_BLOCK_COUNT)
        uint32_t m_scheduledBlocks = 0;
    };

    // 状態のハッシュ = Σ ProbeCellHash(セルの番号, 値)(mod 2^64)。GPU のハッシュの単位と同じ値になる
    [[nodiscard]] uint64_t ProbeStateHash(std::span<const uint32_t> cells);

    // 熱の合計(Σ 値。GPU の表の熱の合計と同じ値になる)
    [[nodiscard]] uint64_t ProbeHeatSum(std::span<const uint32_t> cells);

    // 状態の z = PROBE_VIEW_Z の面(描画用の抽出と同じ並び)
    [[nodiscard]] std::span<const uint32_t> ProbeViewSlice(std::span<const uint32_t> cells);

}  // namespace bicameral::sim
