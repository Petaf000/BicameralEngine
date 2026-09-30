// probe_sim.h — T-0004 の仮の刻み(shaders/common/probe_sim.hlsli)を GPU で走らせる道具と、その CPU リファレンス。
//
// 本物の刻みのループ(06 §2)は T-0012 から。ここではフレームループ(frame/frame_loop)の形を確かめる:
//   - コマンドリストは作るときに 1 度だけ記録し、毎回使い回す(CPU はアップロードのバッファに値を書くだけ)。
//     刻みの数 n(1〜PROBE_MAX_TICKS_PER_BATCH)ごとにリストを記録しておき、n で選ぶ。
//     (最初は 1 本のリストの刻みの枠を ExecuteIndirect の数 0/1 で切り替えたが、空の ExecuteIndirect が 1 回数十 µs かかった。
//      docs/perf.md 2026-09-30 T-0004)
//   - バッチの入力・読み戻しはバッチの枠(slot)ごとに持つ。slot のリストが GPU で終わるまで、その slot には書かない。
//   - GPU → CPU は待たない読み戻し(gpu/readback_ring): つつきを適用したイベント・デバッグの出力・タイムスタンプ。
//   - 重さの試験(R-LOOP-2、T-0085): 1 刻みに結果に入らない重さを足し、それを何個の Dispatch に分けるか・
//     バッチのリストの中に置くか別々の投入にするかを選べる(ProbeSimOptions)。
//
// 使い方:
//   auto sim = ProbeSim::Create(device, D3D12_COMMAND_LIST_TYPE_COMPUTE);
//   ID3D12CommandList* list = sim->PrepareBatch(slot, {.firstTick = t, .tickCount = n, .extractionTarget = b % 3, ...});
//   computeQueue.Submit(list) → フェンスが進んだら sim->ReadBatch(slot)
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

    struct ProbeBatchReadback {
        std::vector<ProbeEvent> events;  // 並びは GPU が空きを取った順(毎回同じとは限らない。表示にだけ使う)
        uint32_t droppedEventCount = 0;  // 容量を超えて書けなかった数
        uint64_t gpuBeginTimestamp = 0;  // バッチのリストの始めと終わり(キューのタイムスタンプの刻み)
        uint64_t gpuEndTimestamp = 0;
        uint32_t debugAssertCount = 0;  // シェーダーの assert の数(中身はログに出る)
    };

    struct ProbeBatchInput {
        uint64_t firstTick = 0;
        uint32_t tickCount = 0;         // 1〜PROBE_MAX_TICKS_PER_BATCH
        uint32_t extractionTarget = 0;  // 最後の状態を写す抽出(0〜PROBE_EXTRACTION_COUNT-1)
        uint32_t busyIterations = 0;    // 重さの試験の 1 刻みの合計(0 なら無し。上限 PROBE_BUSY_ITERATIONS_LIMIT)
        std::span<const ProbeCommand> commands;  // 最大 PROBE_MAX_COMMANDS
    };

    // 重さの試験の分け方(R-LOOP-2、T-0085)。世界の結果には入らない
    struct ProbeSimOptions {
        uint32_t busyPieces = 1;             // 1 刻みの重さを何個の Dispatch に分けるか(1〜PROBE_MAX_BUSY_PIECES)
        bool busyInSeparateSubmits = false;  // true: 1 個ずつ別の投入(ExecuteCommandLists)。false: バッチのリストの中
    };

    // --- GPU で走らせる ---

    class ProbeSim {
    public:
        static constexpr uint32_t BATCH_SLOT_COUNT = 3;

        // listType: バッチのリストを投げるキューの種類(シミュは compute。06 §4)
        [[nodiscard]] static std::expected<ProbeSim, std::string> Create(ID3D12Device5* device,
                                                                         D3D12_COMMAND_LIST_TYPE listType,
                                                                         const ProbeSimOptions& options = {});

        // slot のアップロードのバッファに入力を書き、記録済みのリストを返す。
        // 呼ぶ側の約束: slot の前のバッチを GPU が終えている。入力が範囲外なら nullptr(理由はログ)
        [[nodiscard]] ID3D12CommandList* PrepareBatch(uint32_t slot, const ProbeBatchInput& input);

        // 重さを別々の投入にするとき(options.busyInSeparateSubmits): PrepareBatch の後、バッチのリストより前に
        // BusyPieceList(slot, i) を i = 0..(刻みの数 × BusyPieceCount() − 1) の順に、フェンスを進めずに投げる。
        // 1 本ずつ別のリスト(同じリストは、キューのフェンスが前の実行を越えるまで投げ直せない。debug layer の [553])。
        // i = 0 のリストがバッチの始めのタイムスタンプを書く(バッチの GPU 時間に重さを含めるため)
        [[nodiscard]] bool BusyInSeparateSubmits() const { return m_options.busyInSeparateSubmits; }
        [[nodiscard]] uint32_t BusyPieceCount() const { return m_options.busyPieces; }
        [[nodiscard]] ID3D12CommandList* BusyPieceList(uint32_t slot, uint32_t index) const {
            return m_slots[slot].busyLists[index].Get();
        }

        // slot のバッチを GPU が終えた後に呼ぶ(待たない。終わったかどうかは呼ぶ側がフェンスで見る)
        [[nodiscard]] ProbeBatchReadback ReadBatch(uint32_t slot) const;

        // 描画用の抽出(0〜PROBE_EXTRACTION_COUNT-1)。描画は読むだけ
        [[nodiscard]] ID3D12Resource* Extraction(uint32_t target) const { return m_extractions[target].Get(); }

    private:
        ProbeSim(const ProbeSimOptions& options, gpu::ReadbackRing&& events, gpu::DebugRing&& debugRing)
            : m_options(options), m_events(std::move(events)), m_debugRing(std::move(debugRing)) {}

        [[nodiscard]] bool CreatePipelines(ID3D12Device5* device);
        [[nodiscard]] bool CreateBuffers(ID3D12Device5* device);
        [[nodiscard]] bool RecordBatchLists(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE listType);
        [[nodiscard]] bool RecordBatchList(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE listType, uint32_t slot,
                                           uint32_t tickCount);
        [[nodiscard]] bool RecordBusyPieceLists(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE listType, uint32_t slot);
        [[nodiscard]] bool RecordBusyPieceList(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE listType, uint32_t slot,
                                               uint32_t index);
        void BindRootArguments(ID3D12GraphicsCommandList10* list, ID3D12Resource* input) const;
        void RecordBusyPieces(ID3D12GraphicsCommandList10* list, uint32_t pieceCount) const;

        struct BatchSlot {
            Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
            // 刻みの数 n のリストが lists[n - 1]。作るときに 1 度だけ記録する
            std::array<Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList10>, PROBE_MAX_TICKS_PER_BATCH> lists;
            // 重さを別々の投入にするときの 1 個分 × (刻みの数の上限 × 分けた数)。[0] はバッチの始めのタイムスタンプも書く
            std::vector<Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList10>> busyLists;
            Microsoft::WRL::ComPtr<ID3D12Resource> input;  // アップロード(PROBE_BATCH_BYTES)
            std::byte* mappedInput = nullptr;              // Map したまま
        };

        ProbeSimOptions m_options;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_applyPipeline;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_diffusePipeline;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_extractPipeline;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> m_busyPipeline;

        Microsoft::WRL::ComPtr<ID3D12Resource> m_world;  // 2 世代 × PROBE_CELL_COUNT × uint32
        std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, PROBE_EXTRACTION_COUNT> m_extractions;
        Microsoft::WRL::ComPtr<ID3D12Resource> m_busySink;
        gpu::ReadbackRing m_events;
        gpu::DebugRing m_debugRing;
        Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_timestamps;        // slot ごとに 2 つ(始め・終わり)
        Microsoft::WRL::ComPtr<ID3D12Resource> m_timestampReadback;  // slot ごとに 16 バイト
        std::array<BatchSlot, BATCH_SLOT_COUNT> m_slots;
    };

    // --- CPU リファレンス(GPU とビット一致するはずのもの。D-307・CLAUDE.md 原則 4)---

    class ProbeReference {
    public:
        ProbeReference();

        // 刻み tick を 1 つ進める(targetTick == tick のコマンドを適用 → 拡散)
        void Advance(uint64_t tick, std::span<const ProbeCommand> commands);

        // 刻み tick の始めの状態(= tick 回進めた後)
        [[nodiscard]] std::span<const uint32_t> State(uint64_t tick) const;

    private:
        std::vector<uint32_t> m_cells;  // 2 世代 × PROBE_CELL_COUNT(GPU と同じ並び)
    };

    // 状態の要約(FNV-1a 64bit)。GPU と CPU の比較と、分け方を変えても同じ結果かの比較に使う
    [[nodiscard]] uint64_t HashCells(std::span<const uint32_t> cells);

}  // namespace bicameral::sim
