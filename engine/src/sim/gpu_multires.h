// gpu_multires.h — 多重解像度の木(sim/multires_nest の CPU リファレンスと同じ形)を GPU に置き、
// 世界の木の要求の処理(Compute の段 shaders/sim/multires_tree.hlsl + Work Graph shaders/sim/multires_graph.hlsl。T-0018)・
// 観察の影を作る・引き戻す(Work Graph の再帰)・反応の刻み(Compute。multires_step.hlsl)を記録する(T-0017)。
// 活性のブロックだけ刻む(Work Graph の ActivitySeedNode → WakeFaceNode → ActivityStepNode。T-0100)と、
// 静かな葉を粗くする要求を作る(Compute の TreeQuiet。T-0101)もできる。
// 活性の種の一覧は 2 本を刻みごとに入れ替える(この刻みの種 = GPU の入力、次の刻みの種 = u14 に書き足す)。
//
// 使い方(テスト。1 刻み = 要求の処理と影の出来事 → 刻む → 影の引き戻し。CPU の test::StepMultiresScene と同じ順。
// 刻むのは RecordStep〔全部〕か RecordStepActive〔活性だけ。CPU の StepActive〕):
//   auto gpu = GpuMultires::Create(device, table, capacity);
//   gpu->RecordUpload(list, nest);                 // 最初だけ
//   gpu->RecordRequests(list, requests); gpu->RecordProcessRequests(list, ring);
//   gpu->RecordStep(list, ring, seed, tick); gpu->RecordPullBack(list, ring, ...);
//   gpu->RecordReadback(list);  → 投げて待つ →  gpu->Read(nest);
// 各操作の後に UAV のバリアを入れる(次の操作は前の書き込みを読む)。
// 覗き窓(sim/probe_peek。T-0096)は、同じルート署名で自分の Compute(世界の写し・抽出)を起動する: SetExternalViews で u4・u5 に
// 外のバッファを結び、RecordExternalDispatch で外の定数 4 語つきで投げる。
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "gpu/com_ptr.h"
#include "gpu/work_graph.h"
#include "sim/multires_nest.h"
#include "sim/reaction_table.h"

namespace bicameral::sim {

    struct GpuMultiresOptions {
        // 活性の Work Graph(shaders/sim/multires_activity_graph.hlsl)を作る。RecordStepActive に要る。
        // 反応の核を含んで大きい(debug の GPU-based validation で作るのに数分)ので、使う時だけ
        bool activity = false;
    };

    class GpuMultires {
    public:
        [[nodiscard]] static std::expected<GpuMultires, std::string> Create(ID3D12Device5* device,
                                                                            const BakedReactionTable& table,
                                                                            const MultiresCapacity& capacity,
                                                                            const GpuMultiresOptions& options = {});

        // CPU の木(状態・索引・取り合いの印)を GPU へ写す。大きさは Create と同じでなければならない
        [[nodiscard]] bool RecordUpload(ID3D12GraphicsCommandList10* list, const MultiresNest& nest);

        // --- 世界の木(multires_nest.h の SubmitRequests・ProcessRequests と同じ結果)---
        // 要求の一覧を写す(一覧が空の時に。1 本のリストで REQUEST_UPLOAD_SLOTS 回まで)
        [[nodiscard]] bool RecordRequests(ID3D12GraphicsCommandList10* list,
                                          std::span<const multires::MrRequest> requests);
        // 静かな本物の葉を粗くする要求を一覧の後ろに足す(SubmitQuietCoarsenRequests と同じ。RecordRequests の後・
        // RecordProcessRequests の前。tick はこれから処理する刻み。T-0101)
        void RecordQuietRequests(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint64_t tick);
        void RecordProcessRequests(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing);

        // --- 観察の枠(multires_nest.h の同じ名前の関数と同じ結果)---
        void RecordRefineShadow(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                uint32_t parentSlot, uint32_t firstChildSlot, uint32_t levelCount,
                                const MultiresPoint& point);
        void RecordRemoveShadow(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                uint32_t firstSlot, uint32_t levelCount);
        void RecordStep(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint64_t worldSeed,
                        uint64_t tick);
        // 活性のブロックだけ刻む(multires_nest.h の StepActive と同じ結果。観察の枠は全部刻む。T-0100)。
        // GpuMultiresOptions::activity で作っていなければ false
        [[nodiscard]] bool RecordStepActive(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                            uint64_t worldSeed, uint64_t tick);
        void RecordPullBack(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                            uint32_t firstShadowSlot, uint32_t levelCount);

        // --- 外のバッファとパイプライン(T-0096)---
        // u4・u5 に結ぶ外のバッファ。0 なら自分のセルのバッファを代わりに結ぶ(使わないシェーダーは読まない。ルートの引数は全部結ぶ約束)
        void SetExternalViews(D3D12_GPU_VIRTUAL_ADDRESS first, D3D12_GPU_VIRTUAL_ADDRESS second);
        // このルート署名で作った Compute のパイプラインを groupCount グループ起動する(外の定数は multires_bindings.hlsli の g_external*)
        void RecordExternalDispatch(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                    ID3D12PipelineState* pipeline, uint32_t groupCount,
                                    const std::array<uint32_t, 4>& external);
        [[nodiscard]] ID3D12RootSignature* RootSignature() const { return m_rootSignature.Get(); }
        [[nodiscard]] uint32_t BlockCapacity() const { return m_blockCapacity; }

        static constexpr uint32_t REQUEST_UPLOAD_SLOTS = 16;

        // --- 読み戻し ---
        void RecordReadback(ID3D12GraphicsCommandList10* list);
        [[nodiscard]] bool Read(MultiresNest& nest) const;

        // 次の刻みの活性の種(RecordReadback の時の一覧。枠の順・重なりなし)と、一覧が一杯で落とした数
        struct ActivitySeeds {
            std::vector<uint32_t> slots;
            uint32_t dropped = 0;
        };
        [[nodiscard]] std::optional<ActivitySeeds> ReadSeeds() const;

        // --- 計測: index 番目のタイムスタンプを打つ。RecordReadback が打った分を写し、ReadTimestamps で読む ---
        void RecordTimestamp(ID3D12GraphicsCommandList10* list, uint32_t index);
        [[nodiscard]] std::vector<uint64_t> ReadTimestamps(uint32_t count) const;

        [[nodiscard]] uint64_t GraphBackingMemoryBytes() const { return m_graph->BackingMemoryBytes(); }

    private:
        // multires_bindings.hlsli の RootConstants と同じ並び
        struct RootConstants {
            uint32_t seedLow = 0;
            uint32_t seedHigh = 0;
            uint32_t tickLow = 0;
            uint32_t tickHigh = 0;
            std::array<uint32_t, 6> point{};  // x・y・z の下位と上位
            int32_t pointLevel = 0;
            uint32_t blockCount = 0;
            std::array<uint32_t, 4> external{};  // 外のパイプラインの定数(T-0096)

            // --- 木の管理(T-0018)---
            int32_t rootLevel = 0;
            uint32_t worldBlocks = 0;
            uint32_t indexEntries = 0;
            uint32_t ledgerColumns = 0;
            uint32_t graphInputLow = 0;
            uint32_t graphInputHigh = 0;
            uint32_t graphEntries = 0;  // 下位 16bit = RefineNode、上位 16bit = CoarsenRequestNode
        };

        // バッファの並び(u0〜u3、u6〜u13。multires_bindings.hlsli)
        enum Buffer : uint8_t {
            BufferBlocks,
            BufferCells,
            BufferFractions,
            BufferCounters,
            BufferFreeBlocks,
            BufferFreeFractions,
            BufferLedger,
            BufferIndex,
            BufferRequests,
            BufferStates,
            BufferClaims,
            BufferGraphInput,
            BufferCount
        };

        static constexpr uint32_t TABLE_COUNT = 4;  // 物質・規則・索引・速度
        static constexpr uint32_t ACTIVITY_LISTS = 2;
        static constexpr uint32_t TREE_PASS_COUNT = 7;
        static constexpr uint32_t MAX_TIMESTAMPS = 16;

        GpuMultires() = default;

        [[nodiscard]] std::expected<void, std::string> CreatePipelines(ID3D12Device5* device,
                                                                       const GpuMultiresOptions& options);
        [[nodiscard]] std::expected<void, std::string> CreateBuffers(ID3D12Device5* device,
                                                                     const BakedReactionTable& table);
        void BindRoot(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing) const;
        void SetGraphProgram(ID3D12GraphicsCommandList10* list);
        void SetActivityProgram(ID3D12GraphicsCommandList10* list);
        void DispatchGraph(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint32_t entry,
                           const void* record, uint32_t recordBytes);
        void RecordTreePass(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint32_t pass,
                            uint32_t groupCount);
        [[nodiscard]] std::array<uint64_t, BufferCount> BufferSizes() const;
        [[nodiscard]] uint64_t ActivityBytes() const;
        [[nodiscard]] std::vector<std::byte> MakeActivityList(uint32_t list, std::span<const uint32_t> slots) const;
        void SetTick(uint64_t worldSeed, uint64_t tick);

        // --- 大きさ ---
        MultiresCapacity m_capacity;
        uint32_t m_blockCapacity = 0;  // 世界の枠 + 観察の枠

        // --- パイプライン ---
        ComPtr<ID3D12RootSignature> m_rootSignature;
        ComPtr<ID3D12PipelineState> m_stepPipeline;
        std::array<ComPtr<ID3D12PipelineState>, TREE_PASS_COUNT> m_treePipelines;
        std::unique_ptr<gpu::WorkGraph> m_graph;
        bool m_graphInitialized = false;
        std::array<uint32_t, 4> m_entries{};              // 細かくする・粗くする要求・引き戻す・影を捨てる
        std::unique_ptr<gpu::WorkGraph> m_activityGraph;  // 無ければ活性を使わない
        bool m_activityGraphInitialized = false;
        std::array<uint32_t, 2> m_activityEntries = {UINT32_MAX, UINT32_MAX};  // 活性の種・観察の枠を刻む
        RootConstants m_constants;

        // --- バッファ(既定のヒープ・アップロード・読み戻し。並びは Buffer の順)---
        std::array<ComPtr<ID3D12Resource>, BufferCount> m_buffers;
        std::array<ComPtr<ID3D12Resource>, BufferCount> m_uploads;
        std::array<ComPtr<ID3D12Resource>, BufferCount> m_readbacks;
        ComPtr<ID3D12Resource> m_requestUpload;  // REQUEST_UPLOAD_SLOTS × (要求の数 + 一覧)
        uint32_t m_requestUploadCursor = 0;
        std::array<ComPtr<ID3D12Resource>, TABLE_COUNT> m_tables;
        std::array<D3D12_GPU_VIRTUAL_ADDRESS, 2> m_externalViews{};  // u4・u5(0 なら代わりにセル)

        // --- 活性の種の一覧(T-0100。MR_ACTIVITY_*)---
        std::array<ComPtr<ID3D12Resource>, ACTIVITY_LISTS> m_activity;
        ComPtr<ID3D12Resource> m_activityUpload;    // 最初の一覧 + 空の一覧 2 本
        ComPtr<ID3D12Resource> m_activityReadback;  // 次の刻みの種の一覧
        uint32_t m_activityCurrent = 0;             // この刻みの種の一覧
        uint32_t m_activityWrite = 0;               // u14 に結ぶ一覧

        // --- 計測 ---
        ComPtr<ID3D12QueryHeap> m_timestamps;
        ComPtr<ID3D12Resource> m_timestampReadback;
        uint32_t m_timestampCount = 0;
    };

}  // namespace bicameral::sim
