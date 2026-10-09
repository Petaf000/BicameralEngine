// gpu_multires.h — 多重解像度の木(sim/multires_nest の CPU リファレンスと同じ形)を GPU に置き、
// 世界の木の要求の処理(Compute の段 shaders/sim/multires_tree.hlsl + Work Graph shaders/sim/multires_graph.hlsl。T-0018)・
// 観察の影を作る・引き戻す(Work Graph の再帰)・反応の刻み(Compute。multires_step.hlsl)を記録する(T-0017)。
// 活性のブロックだけ刻む(Work Graph の ActivitySeedNode → WakeFaceNode → ActivityStepNode。T-0100)と、
// 静かな葉を粗くする要求を作る(Compute の TreeQuiet。T-0101)・静かで一様になった頁を畳む(TreeFoldCheck → TreeFold。T-0103)もできる。
// 活性の種の一覧は 2 本を刻みごとに入れ替える(この刻みの種 = GPU の入力、次の刻みの種 = u13 に書き足す)。
// 熱の伝導(MultiresStepOptions::conduction。T-0107)は Compute の段 shaders/sim/multires_conduct.hlsl(印 → 頁 → 端数の枠 → 埋める →
// 面の流れ → 変化を足して反応)。活性の刻みでは Work Graph が刻むブロックを伝導の一覧に足し、段はその一覧のブロックだけを受け持つ。
// 細かいレベルの小刻み(subcycleBaseLevel・maxSubcycleGap。T-0109)は、印 〜 流れを小刻みごとに積み、小刻みの終わりに変化を足して
// (ConductEnd)変わったブロックの隣を活性のグラフで起こす。
// 細かいレベルの熱の陰解法(MultiresStepOptions::implicitConduction。T-0132)は、流れの段が基準より細かいブロックを飛ばし、流れの後に
// 陰解法の段(sim/gpu_multires_implicit。EnableImplicitConduction で作る)が系を作って解き、変化を伝導の表へ足す(ConductApply が足す)。
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

        // 活性の刻みの熱の伝導の段(埋める・流れ・足す)を Work Graph(shaders/sim/multires_conduct_graph.hlsl)で投げる。
        // 無ければ Compute(全部の枠の数だけグループを投げ、一覧の数を超えたグループは何もしない)。T-0107 で両方を測り、差は揺れの中で
        // Compute がわずかに安かったので既定は Compute(D-302。docs/perf.md)
        bool conductionGraph = false;
    };

    class GpuMultiresImplicit;
    using GpuMultiresImplicitLimits = MultiresImplicitLimits;  // 陰解法の系の大きさの上限(VRAM に先に取る。T-0178)

    class GpuMultires {
    public:
        GpuMultires(GpuMultires&& other) noexcept;
        GpuMultires& operator=(GpuMultires&& other) noexcept;
        GpuMultires(const GpuMultires&) = delete;
        GpuMultires& operator=(const GpuMultires&) = delete;
        ~GpuMultires();

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
        // 静かで一様になった頁を枠の順に畳む(FoldQuietPages と同じ。RecordQuietRequests の前。tick はこれから処理する刻み。T-0103)
        void RecordFoldPages(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint64_t tick);
        // 許容差つき(FoldQuietPages の許容差つきと同じ。ちょうど静かになった端数の枠を帳簿へ移して返し、ほぼ同じ頁も平均で畳む。T-0112)
        void RecordFoldPages(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint64_t tick,
                             const multires::MrFoldTolerance& tolerance);
        // 静かな本物の葉を粗くする要求を一覧の後ろに足す(SubmitQuietCoarsenRequests と同じ。RecordRequests の後・
        // RecordProcessRequests の前。tick はこれから処理する刻み。T-0101)。粗くするのは子のセル 2×2×2 の組ごとの差が
        // 許容差の中の葉だけ(TreeQuietCheck。D-430・T-0113)。許容差を渡さない版は完全に同じ時だけ
        void RecordQuietRequests(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint64_t tick);
        void RecordQuietRequests(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint64_t tick,
                                 const multires::MrFoldTolerance& tolerance);
        void RecordProcessRequests(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing);
        // 頁を配る段(TreeExpand)だけ(計測用。刻む段が印を付けていなければ何も配らない。T-0102)
        void RecordExpandPass(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing);

        // --- 観察の枠(multires_nest.h の同じ名前の関数と同じ結果)---
        void RecordRefineShadow(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                uint32_t parentSlot, uint32_t firstChildSlot, uint32_t levelCount,
                                const MultiresPoint& point);
        void RecordRemoveShadow(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                uint32_t firstSlot, uint32_t levelCount);
        // 全部を刻む(multires_nest.h の StepNest と同じ結果。options.conduction で熱の伝導も。T-0107。細かいレベルの小刻みも。T-0109)。
        // 反応は待ちの丸め(ADR-0018。T-0121・T-0125)
        void RecordStep(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint64_t worldSeed,
                        uint64_t tick, const MultiresStepOptions& options = {});
        // 活性のブロックだけ刻む(multires_nest.h の StepActive と同じ結果。観察の枠は全部刻む。T-0100)。
        // GpuMultiresOptions::activity で作っていなければ false。反応は待ちの丸め(T-0124。熱の伝導を入れる刻みも。T-0125)
        [[nodiscard]] bool RecordStepActive(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                            uint64_t worldSeed, uint64_t tick, const MultiresStepOptions& options = {});
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

        // 伝導の段を Work Graph で投げるか(計測で切り替える。conductionGraph で作っていなければ true にできず false を返す)
        [[nodiscard]] bool UseConductionGraph(bool use);

        // --- 細かいレベルの熱の陰解法(MultiresStepOptions::implicitConduction。T-0132)---
        // 陰解法の段(系を作る・多重格子・解く・変化を足す)を上限の大きさで作る。implicitConduction の刻みの前に 1 回
        [[nodiscard]] std::expected<void, std::string> EnableImplicitConduction(
            ID3D12Device5* device, const GpuMultiresImplicitLimits& limits);
        // 計測用: 陰解法の段の境にタイムスタンプを打つ(GpuMultiresImplicit::StampPhases)
        void StampImplicitPhases(bool stamp);
        // 陰解法の段(無ければ nullptr。最後に読み戻せた刻みの数を読む用)
        [[nodiscard]] const GpuMultiresImplicit* ImplicitConduction() const { return m_implicit.get(); }

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

            // --- 刻み(T-0107)---
            uint32_t stepFlags = 0;  // STEP_FLAG_*(multires_bindings.hlsli の MR_STEP_*)

            // --- 頁を畳む(T-0112)---
            uint32_t foldTolerance = 0;  // MrPackFoldTolerance(0 = 完全に同じ)
        };

        // バッファの並び(u0〜u3、u6〜u12。multires_bindings.hlsli)
        enum Buffer : uint8_t {
            BufferBlocks,
            BufferCells,
            BufferFractions,
            BufferCounters,
            BufferTreeWords,  // [世界の枠の空き][取り合いの印][索引][世界の頁の空き](T-0107 でまとめた)
            BufferFreeFractions,
            BufferLedger,
            BufferRequests,
            BufferStates,
            BufferConduction,  // 伝導の作業場(T-0107)
            BufferGraphInput,
            BufferCount
        };

        static constexpr uint32_t TABLE_COUNT = 4;  // 物質・規則・索引・速度
        static constexpr uint32_t ACTIVITY_LISTS = 2;
        static constexpr uint32_t TREE_PASS_COUNT = 12;
        static constexpr uint32_t CONDUCT_PASS_COUNT = 6;
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
        [[nodiscard]] uint64_t PageCount() const;
        [[nodiscard]] uint64_t ConductRecordsOffset() const;
        [[nodiscard]] std::vector<std::byte> MakeGraphInputImage() const;
        void RecordExpandPages(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing);
        // 待ちの丸め(T-0121): 起こす段(extraFlags に STEP_FLAG_WAKE_SEEDS なら種の一覧〔m_activityWrite〕に足す)・全部を刻む
        void RecordWake(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint32_t extraFlags);
        void RecordStepWait(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing);
        void RecordConductPass(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint32_t pass,
                               uint32_t groupCount);
        void RecordConduction(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                              const MultiresStepOptions& options, uint32_t wakeList);
        void RecordConductSubstep(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                  const MultiresStepOptions& options);
        void RecordSubstepEnd(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                              uint32_t wakeList);
        void RecordConductStage(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint32_t pass);
        void RecordImplicitConduction(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                      const MultiresStepOptions& options);
        [[nodiscard]] std::expected<void, std::string> CreateConductionGraph(ID3D12Device5* device);
        [[nodiscard]] std::vector<std::byte> MakeTreeWordsImage(const MultiresNest& nest) const;
        [[nodiscard]] uint64_t ActivityBytes() const;
        [[nodiscard]] std::vector<std::byte> MakeActivityList(uint32_t list, std::span<const uint32_t> slots) const;
        void SetTick(uint64_t worldSeed, uint64_t tick);

        // --- 大きさ ---
        MultiresCapacity m_capacity;
        uint32_t m_blockCapacity = 0;  // 世界の枠 + 観察の枠

        // --- パイプライン ---
        ComPtr<ID3D12RootSignature> m_rootSignature;
        // --- 待ちの丸め(T-0121)---
        ComPtr<ID3D12PipelineState> m_wakePipeline;              // 起こす段: 見出しを全部なめる
        ComPtr<ID3D12PipelineState> m_stepWaitPipeline;          // 全部の枠を刻む(1 グループ = 1 枠)
        ComPtr<ID3D12PipelineState> m_stepExpandedWaitPipeline;  // 頁に広げたブロックを埋めて刻む
        std::array<ComPtr<ID3D12PipelineState>, TREE_PASS_COUNT> m_treePipelines;
        std::array<ComPtr<ID3D12PipelineState>, CONDUCT_PASS_COUNT> m_conductPipelines;  // 熱の伝導の段(T-0107)
        std::unique_ptr<gpu::WorkGraph> m_conductGraph;  // 伝導の段の Work Graph 版(無ければ Compute だけ)
        bool m_conductGraphInitialized = false;
        bool m_useConductGraph = false;
        std::array<uint32_t, 4> m_conductEntries = {UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                                    UINT32_MAX};  // 埋める・流れ・足す・小刻みの終わり
        std::unique_ptr<gpu::WorkGraph> m_graph;
        bool m_graphInitialized = false;
        std::array<uint32_t, 4> m_entries{};              // 細かくする・粗くする要求・引き戻す・影を捨てる
        std::unique_ptr<gpu::WorkGraph> m_activityGraph;  // 無ければ活性を使わない
        bool m_activityGraphInitialized = false;
        std::array<uint32_t, 3> m_activityEntries = {UINT32_MAX, UINT32_MAX,
                                                     UINT32_MAX};  // 活性の種・観察の枠を刻む・頁に広げて刻む
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
        uint32_t m_activityWrite = 0;               // u13 に結ぶ一覧

        // --- 細かいレベルの熱の陰解法(T-0132。EnableImplicitConduction で作る)---
        std::unique_ptr<GpuMultiresImplicit> m_implicit;

        // --- 計測 ---
        ComPtr<ID3D12QueryHeap> m_timestamps;
        ComPtr<ID3D12Resource> m_timestampReadback;
        uint32_t m_timestampCount = 0;
    };

}  // namespace bicameral::sim
