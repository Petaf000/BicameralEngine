// probe_sim.h — 仮の刻み(shaders/common/probe_sim.hlsli)を「単位」の列として GPU で走らせる道具と、その CPU リファレンス(T-0004・T-0012)。
//
// 刻みのループの形(06 §4・ADR-0011)を確かめる。中身(拡散)は T-0005 以降で本物の段に置き換える:
//   - 1 刻み = 決まった数の単位(適用 → 拡散 → 重さ × k → ハッシュ)。フレームの切れ目はどの単位の間にも来てよい(刻みはフレームをまたぐ)。
//   - CPU は毎フレーム、そのフレームに投げる単位(何番目の刻みの何番目から何個)を、フレームの枠ごとのリストに記録して投げる。
//     同じリストは前の実行が終わるまで投げ直せない(debug layer [553])ので、使い回す記録済みのリストではなく毎フレーム記録する。
//     CPU が書くのはコマンドの並び・ルート定数・Dispatch だけ(世界の状態には触れない。D-107)。
//   - 単位ごとのタイムスタンプを取り、CPU は単位の GPU 時間から次のフレームに投げる数を決める(frame/sim_scheduler)。
//   - 刻みの最後の単位が状態のハッシュを GPU で取る(06 §2 段 9)。フレームの終わりに CPU へ読み戻す(待たない)。
//   - 抽出(描画が読む)は、投げた単位の後ろで、刻みの境界の状態を写す(1 フレームに 1 回まで)。
//
// 使い方:
//   auto sim = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE, {.busyIterations = n, .busyPieces = k});
//   ID3D12CommandList* list = sim->RecordFrame(slot, {.firstTick = t, .firstUnit = u, .unitCount = c, ...});
//   computeQueue.Submit(list) → フェンスが進んだら sim->ReadFrame(slot)
// 浮動小数点は使わない(engine/src/sim は検査の対象。04 §4)。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "common/probe_sim.hlsli"
#include "gpu/debug_ring.h"
#include "gpu/readback_ring.h"

namespace bicameral::sim {

    // --- コマンド(06 §3 の 64 バイト)---

    struct ProbeCommand {
        uint64_t targetTick = 0;  // 適用する刻み
        uint32_t sequence = 0;    // 同じ刻みの中の順番(つつきは max なので順番に依存しないが、形は本物と同じにする)
        uint16_t type = 0;
        uint16_t size = 0;                   // payload の使っているバイト数
        std::array<uint32_t, 12> payload{};  // 48 バイト
    };
    static_assert(sizeof(ProbeCommand) == PROBE_COMMAND_BYTES);

    [[nodiscard]] ProbeCommand MakePokeCommand(uint64_t targetTick, uint32_t sequence, uint32_t x, uint32_t y);

    // --- GPU から戻ってくるもの ---

    struct ProbeEvent {
        uint64_t tick = 0;
        uint32_t type = 0;  // PROBE_EVENT_POKE_APPLIED
        uint32_t x = 0;
        uint32_t y = 0;
    };

    // 刻み tick の始めの状態 S(tick) のハッシュ(ProbeStateHash)
    struct ProbeTickHash {
        uint64_t tick = 0;
        uint64_t hash = 0;
    };

    struct ProbeFrameReadback {
        std::vector<ProbeEvent> events;      // 並びは GPU が空きを取った順(毎回同じとは限らない。表示にだけ使う)
        uint32_t droppedEventCount = 0;      // 容量を超えて書けなかった数
        std::vector<ProbeTickHash> hashes;   // このフレームで終えた刻みの状態(刻みの順)
        std::vector<uint64_t> unitGpuTicks;  // 投げた i 番目の単位の GPU 時間(タイムスタンプの刻み)
        uint32_t firstUnit = 0;              // unitGpuTicks[0] の単位の、刻みの中の番号
        uint64_t gpuBeginTimestamp = 0;      // リスト全体(抽出と読み戻しを含む)の始めと終わり
        uint64_t gpuEndTimestamp = 0;
        uint32_t debugAssertCount = 0;  // シェーダーの assert の数(中身はログに出る)
    };

    struct ProbeFrameInput {
        uint64_t firstTick = 0;                  // 最初の単位の刻み
        uint32_t firstUnit = 0;                  // 最初の単位の、刻みの中の番号(0〜UnitsPerTick()-1)
        uint32_t unitCount = 0;                  // 投げる単位の数(0〜MAX_UNITS_PER_FRAME。0 なら抽出だけ)
        bool extract = false;                    // 単位の後ろで、刻みの境界の状態を抽出へ写すか
        uint32_t extractionTarget = 0;           // 抽出の書き先(0〜PROBE_EXTRACTION_COUNT-1)
        std::span<const ProbeCommand> commands;  // 最大 PROBE_MAX_COMMANDS。適用の単位が targetTick で選ぶ
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

        // 1 刻みの単位の数(適用・拡散・ハッシュ + 重さの単位)
        [[nodiscard]] uint32_t UnitsPerTick() const { return PROBE_FIXED_UNITS_PER_TICK + BusyUnitCount(); }
        [[nodiscard]] uint32_t HashUnit() const { return UnitsPerTick() - 1; }

        // slot のアップロードのバッファに入力を書き、slot のリストに単位を記録して返す。
        // 呼ぶ側の約束: slot の前のリストを GPU が終えている。入力が範囲外・記録の失敗なら nullptr(理由はログ)
        [[nodiscard]] ID3D12CommandList* RecordFrame(uint32_t slot, const ProbeFrameInput& input);

        // slot のリストを GPU が終えた後に呼ぶ(待たない。終わったかどうかは呼ぶ側がフェンスで見る)
        [[nodiscard]] ProbeFrameReadback ReadFrame(uint32_t slot) const;

        // 描画用の抽出(0〜PROBE_EXTRACTION_COUNT-1)。描画は読むだけ
        [[nodiscard]] ID3D12Resource* Extraction(uint32_t target) const { return m_extractions[target].Get(); }

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
        [[nodiscard]] bool CreateBuffers(ID3D12Device5* device);
        [[nodiscard]] bool CreateFrameSlots(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE listType);
        [[nodiscard]] bool ValidateInput(uint32_t slot, const ProbeFrameInput& input) const;
        void WriteInput(FrameSlot& frame, const ProbeFrameInput& input) const;
        void BindRootArguments(ID3D12GraphicsCommandList10* list, ID3D12Resource* input) const;
        void RecordUnit(ID3D12GraphicsCommandList10* list, uint64_t tick, uint32_t unit) const;
        void RecordExtract(ID3D12GraphicsCommandList10* list, uint64_t tick, uint32_t target) const;
        void RecordReadbacks(ID3D12GraphicsCommandList10* list, uint32_t slot, bool hasHash) const;
        [[nodiscard]] std::vector<ProbeTickHash> ReadHashes(const FrameSlot& frame) const;

        ProbeSimOptions m_options;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_applyPipeline;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_diffusePipeline;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_busyPipeline;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_hashBeginPipeline;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_hashCellsPipeline;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_extractPipeline;

        Microsoft::WRL::ComPtr<ID3D12Resource> m_world;  // 2 世代 × PROBE_CELL_COUNT × uint32
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, PROBE_EXTRACTION_COUNT> m_extractions;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_busySink;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_hashes;  // ハッシュの表(PROBE_HASH_BYTES)
        gpu::ReadbackRing m_events;
        gpu::DebugRing m_debugRing;
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_timestamps;  // slot ごとに MAX_UNITS_PER_FRAME + 2
        std::array<FrameSlot, FRAME_SLOT_COUNT> m_slots;
    };

    // --- CPU リファレンス(GPU とビット一致するはずのもの。D-307・CLAUDE.md 原則 4)---

    class ProbeReference {
    public:
        ProbeReference();

        // 刻み tick を 1 つ進める(targetTick == tick のコマンドを適用 → 拡散)
        void Advance(uint64_t tick, std::span<const ProbeCommand> commands);

        // 刻み tick の始めの状態 S(tick)(= tick 回進めた後)
        [[nodiscard]] std::span<const uint32_t> State(uint64_t tick) const;

    private:
        std::vector<uint32_t> m_cells;  // 2 世代 × PROBE_CELL_COUNT(GPU と同じ並び)
    };

    // 状態のハッシュ = Σ ProbeCellHash(セルの番号, 値)(mod 2^64)。GPU のハッシュの単位と同じ値になる
    [[nodiscard]] uint64_t ProbeStateHash(std::span<const uint32_t> cells);

}  // namespace bicameral::sim
