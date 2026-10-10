// gpu_multires.cpp — 多重解像度の木の GPU 版(gpu_multires.h)。
// 要求の処理は Compute の段(multires_tree.hlsl)と Work Graph(細かくする鎖の再帰・粗くする要求)、影を作る・引き戻すは Work Graph の再帰
// (1 レベル = 1 グループ)、刻むのは Compute(待ちの丸め。1 グループ = 1 枠)か、活性だけなら Work Graph(1 ブロック = 1 グループ。T-0100)。
// 一様なブロック(T-0102)は刻む段が頁に広げる印を付け、TreeExpand が枠の順に頁を配り、StepExpandedWaitPass / ExpandStepNode が埋めて刻む。
// 静かで一様になった頁は、要求の処理の前に TreeFoldCheck が調べ TreeFold が枠の順に畳む(T-0103)。
// 熱の伝導(T-0107)は Compute の段(multires_conduct.hlsl)を刻みの中で順に投げる(RecordConduction)。細かいレベルの小刻み(T-0109)は
// 小刻みごとに段を積み、小刻みの終わりに変わったブロックの隣を活性のグラフで起こす。
// 結び付けは shaders/sim/multires_bindings.hlsli と同じ順(u0 見出し・u1 セル・u2 端数・u3 数える欄・u4 u5 外のバッファ・
// u6〜u10 木の管理・u11 伝導の作業場・u12 GPU の入力・u13 書き足す活性の一覧、b0、デバッグのリング、t0〜t3 表)。
// ルート署名は 64 / 64 語(UAV 14 × 2・定数 26・リング 2・表 4 × 2。T-0107 で uint32 の表を u6 にまとめて空けた。最後の 1 語は
// 成分の二段の溢れの始まり〔T-0176〕。溢れは新しい UAV を足さず u6 の後ろに置く)。
#include "sim/gpu_multires.h"

#include <limits>
#include <utility>

#include "common/multires_activity.hlsli"
#include "common/multires_wide.hlsli"

#include "core/log.h"
#include "gpu/resources.h"
#include "sim/gpu_multires_implicit.h"

using namespace bicameral::multires;
using namespace bicameral::reaction;

namespace bicameral::sim {

    namespace {

        constexpr uint32_t ROOT_CONSTANT_COUNT = 26;
        constexpr uint32_t EXTERNAL_VIEW_FIRST = 4;  // u4・u5
        constexpr uint32_t UAV_COUNT = 14;           // u0〜u13
        constexpr uint32_t ACTIVITY_VIEW = 13;       // u13
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
            .uavCount = UAV_COUNT, .rootConstantCount = ROOT_CONSTANT_COUNT, .debugRing = true, .srvCount = 4};
        constexpr uint32_t TREE_THREADS = 64;  // multires_tree.hlsl の TREE_THREADS

        enum Entry : uint8_t {
            EntryRefine,
            EntryCoarsenRequest,
            EntryPullBack,
            EntryRemoveShadow,
        };

        // multires_activity_graph.hlsl の入口
        enum ActivityEntry : uint8_t { ActivityEntrySeed, ActivityEntryObserver, ActivityEntryExpand };

        // multires_tree.hlsl の段(呼ぶ順)
        enum TreePass : uint8_t {
            PassResolve,
            PassSettle,
            PassAllocate,
            PassRelease,
            PassClearIndex,
            PassFillIndex,
            PassQuiet,       // 静かな葉を粗くする要求(要求の処理の前。T-0101)
            PassExpand,      // 一様で反応が進むブロックに頁を配る(刻んだ後。T-0102)
            PassFoldCheck,   // 静かで一様になった頁を調べる(要求の処理の前。T-0103)
            PassFold,        // 調べた頁を枠の順に畳む(T-0103)
            PassFractions,   // 伝導の端数の枠を枠の順に配る(頁を配った後。T-0107)
            PassQuietCheck,  // 静かな葉を粗くできるか調べる(PassQuiet の前。T-0113)
        };
        constexpr std::array<const char*, 12> TREE_SHADERS = {
            "sim/multires_tree_resolve.cso", "sim/multires_tree_settle.cso",      "sim/multires_tree_allocate.cso",
            "sim/multires_tree_release.cso", "sim/multires_tree_clear_index.cso", "sim/multires_tree_fill_index.cso",
            "sim/multires_tree_quiet.cso",   "sim/multires_tree_expand.cso",      "sim/multires_tree_fold_check.cso",
            "sim/multires_tree_fold.cso",    "sim/multires_tree_fractions.cso",   "sim/multires_tree_quiet_check.cso"};

        // multires_conduct.hlsl の段(呼ぶ順。T-0107)
        enum ConductPass : uint8_t {
            ConductPassBegin,
            ConductPassMark,
            ConductPassPrepare,
            ConductPassFlows,
            ConductPassApply,
            ConductPassEnd,  // 小刻みの終わり(T-0109)
        };
        constexpr std::array<const char*, 6> CONDUCT_SHADERS = {
            "sim/multires_conduct_begin.cso", "sim/multires_conduct_mark.cso",  "sim/multires_conduct_prepare.cso",
            "sim/multires_conduct_flows.cso", "sim/multires_conduct_apply.cso", "sim/multires_conduct_end.cso"};
        // 溢れを使う世界の変種(MR_WIDE_CELLS。T-0211)
        constexpr std::array<const char*, 6> CONDUCT_WIDE_SHADERS = {
            "sim/multires_wide_conduct_begin.cso",   "sim/multires_wide_conduct_mark.cso",
            "sim/multires_wide_conduct_prepare.cso", "sim/multires_wide_conduct_flows.cso",
            "sim/multires_wide_conduct_apply.cso",   "sim/multires_wide_conduct_end.cso"};

        // ルート定数の stepFlags(multires_bindings.hlsli の MR_STEP_*)
        constexpr uint32_t STEP_FLAG_CONDUCTION = 1;
        constexpr uint32_t STEP_FLAG_LISTED = 2;
        constexpr uint32_t STEP_FLAG_SUBSTEP_WAKE = 4;   // 小刻みの終わりに起こす(T-0109)
        constexpr uint32_t STEP_FLAG_WAKE_SEEDS = 16;    // 起こす段が種の一覧に足す(活性の刻みの待ちの丸め。T-0124)
        constexpr uint32_t STEP_FLAG_IMPLICIT = 8;       // 細かいレベルの熱の陰解法(T-0132)
        constexpr uint32_t STEP_IMPLICIT_GAP_SHIFT = 5;  // implicitMaxGap − 1(3bit)
        constexpr uint32_t MAX_GPU_IMPLICIT_GAP = 8;
        constexpr uint32_t STEP_FLAG_BITS = 0xFF;
        constexpr uint32_t STEP_SUBSTEP_SHIFT = 8;
        constexpr uint32_t STEP_GAP_SHIFT = 14;
        constexpr uint32_t STEP_BASE_SHIFT = 16;

        // 起こす段(multires_step.hlsl の WakeDue)の 1 グループのスレッドの数
        constexpr uint32_t WAKE_THREADS = 64;

        // 陰解法の印(T-0132。multires_bindings.hlsli の MR_STEP_IMPLICIT)。陰解法の刻みは小刻みに分けない(CPU の StepConduction と同じ約束)。
        // implicitMaxGap は 3bit に詰めるので 1〜8 だけ(0 なら陰解法のセルが無いので印を付けない。9 以上は CPU と合わない)
        uint32_t ImplicitFlags(const MultiresStepOptions& options) {
            if (!options.implicitConduction || options.implicitMaxGap == 0)
                return 0;

            FX_ASSERT(options.maxSubcycleGap == 0);
            FX_ASSERT(options.implicitMaxGap <= MAX_GPU_IMPLICIT_GAP);
            const uint32_t gap = std::min(options.implicitMaxGap, MAX_GPU_IMPLICIT_GAP);

            return STEP_FLAG_IMPLICIT | ((gap - 1) << STEP_IMPLICIT_GAP_SHIFT);
        }

        // stepFlags に小刻みの番号・maxSubcycleGap・subcycleBaseLevel(符号付き 16bit)を詰める(T-0109。multires_bindings.hlsli の
        // MR_STEP_*_SHIFT。ルート署名の語が残り少ないので定数を足さない)
        uint32_t SubcycleFlags(uint32_t flags, const MultiresStepOptions& options, uint32_t substep) {
            FX_ASSERT(options.maxSubcycleGap <= MULTIRES_MAX_SUBCYCLE_GAP);
            FX_ASSERT(options.subcycleBaseLevel >= std::numeric_limits<int16_t>::min() &&
                      options.subcycleBaseLevel <= std::numeric_limits<int16_t>::max());
            const auto base = static_cast<uint16_t>(static_cast<int16_t>(options.subcycleBaseLevel));

            return (flags & STEP_FLAG_BITS) | ImplicitFlags(options) | (substep << STEP_SUBSTEP_SHIFT) |
                   (options.maxSubcycleGap << STEP_GAP_SHIFT) | (uint32_t{base} << STEP_BASE_SHIFT);
        }

        // 伝導の作業場(multires_bindings.hlsli の CONDUCT_MARK_WORDS・CONDUCT_DELTA_BYTES)
        constexpr uint64_t CONDUCT_MARK_BYTES = 16 * sizeof(uint32_t);
        constexpr uint64_t CONDUCT_DELTA_BYTES = 16;

        // Work Graph の GPU の入力(multires_bindings.hlsli の MR_GRAPH_INPUT_*)。見出しは D3D12_NODE_GPU_INPUT そのもの
        constexpr uint32_t GRAPH_INPUT_REFINE_HEADER = 0;
        constexpr uint32_t GRAPH_INPUT_COARSEN_HEADER = 32;
        constexpr uint32_t REFINE_RECORD_BYTES = 24;
        constexpr uint32_t GRAPH_INPUT_BYTES = 64 + ((REFINE_RECORD_BYTES + 4) * (MR_MAX_REQUESTS + 1));
        constexpr uint32_t GRAPH_INPUT_EXPAND_HEADER = (GRAPH_INPUT_BYTES + 15) & ~15u;  // 頁に広げて刻む一覧(T-0102)
        // 伝導の一覧の見出し 4 つ(埋める・流れ・足す・小刻みの終わり。同じレコード。T-0107・T-0109。ConductPassPrepare からの順)
        constexpr std::array<uint32_t, 4> GRAPH_INPUT_CONDUCT_HEADERS = {
            GRAPH_INPUT_EXPAND_HEADER + 32, GRAPH_INPUT_EXPAND_HEADER + 64, GRAPH_INPUT_EXPAND_HEADER + 96,
            GRAPH_INPUT_EXPAND_HEADER + 128};
        constexpr uint32_t GRAPH_INPUT_EXPAND_RECORDS = GRAPH_INPUT_EXPAND_HEADER + 160;
        static_assert(offsetof(D3D12_NODE_GPU_INPUT, EntrypointIndex) == 0);
        static_assert(offsetof(D3D12_NODE_GPU_INPUT, NumRecords) == 4);
        static_assert(offsetof(D3D12_NODE_GPU_INPUT, Records) == 8);
        static_assert(sizeof(D3D12_NODE_GPU_INPUT) <= GRAPH_INPUT_COARSEN_HEADER);

        // multires_graph.hlsl・multires_bindings.hlsli のレコード
        struct RefineRecord {
            uint32_t request;
            uint32_t parentSlot;
            uint32_t childSlot;
            uint32_t levelsLeft;
            uint32_t kind;
            uint32_t depth;
        };
        static_assert(sizeof(RefineRecord) == REFINE_RECORD_BYTES);
        static_assert(sizeof(MrRequest) == 40);

        // 活性の一覧の先頭(multires_activity.hlsli の MR_ACTIVITY_*)と、観察の枠を刻むノードのレコード
        struct ActivityHeader {
            D3D12_NODE_GPU_INPUT input;
            uint32_t reserved;
            uint32_t dropped;
        };
        static_assert(offsetof(ActivityHeader, input) + offsetof(D3D12_NODE_GPU_INPUT, NumRecords) ==
                      MR_ACTIVITY_NUM_RECORDS);
        static_assert(offsetof(ActivityHeader, reserved) == MR_ACTIVITY_RESERVED);
        static_assert(offsetof(ActivityHeader, dropped) == MR_ACTIVITY_DROPPED);
        static_assert(sizeof(ActivityHeader) == MR_ACTIVITY_RECORDS);

        // 活性の写しの並び: [空の一覧 0][空の一覧 1][最初の一覧](空の一覧 = 先頭 + 空のレコード)
        constexpr uint64_t ACTIVITY_EMPTY_BYTES = sizeof(ActivityHeader) + sizeof(uint32_t);
        constexpr uint64_t ACTIVITY_EMPTY_STRIDE = 64;
        constexpr uint64_t ACTIVITY_FIRST_OFFSET = 2 * ACTIVITY_EMPTY_STRIDE;
        static_assert(ACTIVITY_EMPTY_BYTES <= ACTIVITY_EMPTY_STRIDE);

        struct ObserverRecord {
            std::array<uint32_t, 3> grid;
            uint32_t firstSlot;
        };

        // 1 回の要求の写しの大きさ(要求の一覧 + 数)
        constexpr uint64_t REQUEST_UPLOAD_BYTES = (uint64_t{MR_MAX_REQUESTS} * sizeof(MrRequest)) + 256;

        uint32_t GroupsFor(uint32_t threads, uint32_t perGroup) {
            return std::max(1u, (threads + perGroup - 1) / perGroup);
        }

        struct ChainRecord {
            uint32_t slot;
            uint32_t levelsLeft;
        };

        template <typename T>
        ComPtr<ID3D12Resource> CreateFilledUpload(ID3D12Device* device, std::span<const T> data) {
            const auto bytes = std::as_bytes(data);
            ComPtr<ID3D12Resource> buffer = gpu::CreateBuffer(device, bytes.size(), gpu::BufferKind::Upload);
            void* mapped = nullptr;
            const D3D12_RANGE noRead{.Begin = 0, .End = 0};
            if (!buffer || FAILED(buffer->Map(0, &noRead, &mapped)))
                return nullptr;

            std::memcpy(mapped, bytes.data(), bytes.size());
            buffer->Unmap(0, nullptr);

            return buffer;
        }

        bool WriteUpload(ID3D12Resource* upload, std::span<const std::byte> bytes) {
            void* mapped = nullptr;
            const D3D12_RANGE noRead{.Begin = 0, .End = 0};
            if (FAILED(upload->Map(0, &noRead, &mapped)))
                return false;

            std::memcpy(mapped, bytes.data(), bytes.size());
            upload->Unmap(0, nullptr);

            return true;
        }

        // .cso を読んで Compute のパイプラインを作る
        std::expected<ComPtr<ID3D12PipelineState>, std::string> LoadComputePipeline(ID3D12Device5* device,
                                                                                    ID3D12RootSignature* rootSignature,
                                                                                    const char* path) {
            const auto bytecode = gpu::LoadShader(path);
            if (!bytecode)
                return std::unexpected(bytecode.error());

            ComPtr<ID3D12PipelineState> pipeline = gpu::CreateComputePipeline(device, rootSignature, *bytecode);
            if (!pipeline)
                return std::unexpected(std::format("多重解像度のパイプラインを作れない({})", path));

            return pipeline;
        }

        void UavBarrier(ID3D12GraphicsCommandList10* list) {
            const D3D12_RESOURCE_BARRIER barrier = gpu::UavBarrier(nullptr);
            list->ResourceBarrier(1, &barrier);
        }

    }  // namespace

    std::expected<GpuMultires, std::string> GpuMultires::Create(ID3D12Device5* device, const BakedReactionTable& table,
                                                                const MultiresCapacity& capacity,
                                                                const GpuMultiresOptions& options) {
        GpuMultires result;
        result.m_capacity = capacity;
        result.m_blockCapacity = capacity.worldBlocks + capacity.observerBlocks;
        result.m_constants.blockCount = result.m_blockCapacity;
        result.m_constants.rootLevel = capacity.rootLevel;
        result.m_constants.worldBlocks = capacity.worldBlocks;
        result.m_constants.indexEntries = capacity.indexEntries;
        result.m_constants.ledgerColumns = capacity.ledgerColumns;
        result.m_wideCells = options.wideCells;
        result.m_constants.overflowBase = result.OverflowBaseWord();

        if (auto pipelines = result.CreatePipelines(device, options); !pipelines)
            return std::unexpected(pipelines.error());

        if (auto buffers = result.CreateBuffers(device, table); !buffers)
            return std::unexpected(buffers.error());

        return result;
    }

    GpuMultires::GpuMultires(GpuMultires&& other) noexcept = default;
    GpuMultires& GpuMultires::operator=(GpuMultires&& other) noexcept = default;
    GpuMultires::~GpuMultires() = default;

    std::expected<void, std::string> GpuMultires::EnableImplicitConduction(ID3D12Device5* device,
                                                                           const GpuMultiresImplicitLimits& limits) {
        auto implicit = GpuMultiresImplicit::Create(device, *this, m_capacity, limits);
        if (!implicit)
            return std::unexpected(implicit.error());

        m_implicit = std::make_unique<GpuMultiresImplicit>(std::move(*implicit));

        return {};
    }

    void GpuMultires::StampImplicitPhases(bool stamp) {
        if (m_implicit != nullptr)
            m_implicit->StampPhases(stamp);
    }

    void GpuMultires::ForceImplicitCheapest(bool force) {
        if (m_implicit != nullptr)
            m_implicit->ForceCheapest(force);
    }

    std::expected<void, std::string> GpuMultires::CreatePipelines(ID3D12Device5* device,
                                                                  const GpuMultiresOptions& options) {
        m_rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!m_rootSignature)
            return std::unexpected("多重解像度のルート署名を作れない");

        // --- Compute(起こす段・刻む・頁に広げて刻む〔待ちの丸め。T-0121〕・木の管理の段・伝導の段)---
        auto wake = LoadComputePipeline(device, m_rootSignature.Get(), "sim/multires_step_wake.cso");
        auto stepWait = LoadComputePipeline(device, m_rootSignature.Get(), "sim/multires_step_wait.cso");
        auto expandedWait = LoadComputePipeline(device, m_rootSignature.Get(), "sim/multires_step_expanded_wait.cso");
        for (const auto* loaded : {&wake, &stepWait, &expandedWait}) {
            if (!*loaded)
                return std::unexpected(loaded->error());
        }

        m_wakePipeline = std::move(*wake);
        m_stepWaitPipeline = std::move(*stepWait);
        m_stepExpandedWaitPipeline = std::move(*expandedWait);
        if (options.wideCells) {
            auto wideWait = LoadComputePipeline(device, m_rootSignature.Get(), "sim/multires_step_wide_wait.cso");
            auto wideExpanded = LoadComputePipeline(device, m_rootSignature.Get(),
                                                    "sim/multires_step_wide_expanded_wait.cso");
            if (!wideWait || !wideExpanded)
                return std::unexpected(!wideWait ? wideWait.error() : wideExpanded.error());

            m_stepWideWaitPipeline = std::move(*wideWait);
            m_stepWideExpandedWaitPipeline = std::move(*wideExpanded);
        }

        static_assert(TREE_SHADERS.size() == TREE_PASS_COUNT && CONDUCT_SHADERS.size() == CONDUCT_PASS_COUNT);
        for (uint32_t pass = 0; pass < TREE_PASS_COUNT + CONDUCT_PASS_COUNT; ++pass) {
            const bool tree = pass < TREE_PASS_COUNT;
            const char* path = tree ? TREE_SHADERS[pass] : CONDUCT_SHADERS[pass - TREE_PASS_COUNT];
            auto pipeline = LoadComputePipeline(device, m_rootSignature.Get(), path);
            if (!pipeline)
                return std::unexpected(pipeline.error());

            (tree ? m_treePipelines[pass] : m_conductPipelines[pass - TREE_PASS_COUNT]) = std::move(*pipeline);
        }

        static_assert(CONDUCT_WIDE_SHADERS.size() == CONDUCT_PASS_COUNT);
        for (uint32_t pass = 0; options.wideCells && pass < CONDUCT_PASS_COUNT; ++pass) {
            auto pipeline = LoadComputePipeline(device, m_rootSignature.Get(), CONDUCT_WIDE_SHADERS[pass]);
            if (!pipeline)
                return std::unexpected(pipeline.error());

            m_conductWidePipelines[pass] = std::move(*pipeline);
        }

        const auto library = gpu::LoadShader("sim/multires_graph.cso");
        if (!library)
            return std::unexpected(library.error());

        auto graph = gpu::WorkGraph::Create(device, m_rootSignature.Get(), *library, L"Multires");
        if (!graph)
            return std::unexpected(graph.error());

        m_graph = std::make_unique<gpu::WorkGraph>(std::move(*graph));
        m_entries = {m_graph->EntrypointIndex(L"RefineNode"), m_graph->EntrypointIndex(L"CoarsenRequestNode"),
                     m_graph->EntrypointIndex(L"PullBackNode"), m_graph->EntrypointIndex(L"RemoveShadowNode")};
        if (std::ranges::contains(m_entries, UINT32_MAX))
            return std::unexpected("multires_graph.cso に入口のノードが足りない");

        m_constants.graphEntries = m_entries[EntryRefine] | (m_entries[EntryCoarsenRequest] << 16);
        if (options.conductionGraph) {
            if (auto created = CreateConductionGraph(device); !created)
                return created;
        }

        if (!options.activity)
            return {};

        // --- 活性のグラフ(T-0100)---
        const auto activityLibrary = gpu::LoadShader("sim/multires_activity_graph.cso");
        if (!activityLibrary)
            return std::unexpected(activityLibrary.error());

        auto activityGraph = gpu::WorkGraph::Create(device, m_rootSignature.Get(), *activityLibrary,
                                                    L"MultiresActivity");
        if (!activityGraph)
            return std::unexpected(activityGraph.error());

        m_activityGraph = std::make_unique<gpu::WorkGraph>(std::move(*activityGraph));
        m_activityEntries = {m_activityGraph->EntrypointIndex(L"ActivitySeedNode"),
                             m_activityGraph->EntrypointIndex(L"ObserverStepNode"),
                             m_activityGraph->EntrypointIndex(L"ExpandStepNode")};
        if (std::ranges::contains(m_activityEntries, UINT32_MAX))
            return std::unexpected("multires_activity_graph.cso に入口のノードが足りない");

        return {};
    }

    std::expected<void, std::string> GpuMultires::CreateConductionGraph(ID3D12Device5* device) {
        const auto library = gpu::LoadShader("sim/multires_conduct_graph.cso");
        if (!library)
            return std::unexpected(library.error());

        auto graph = gpu::WorkGraph::Create(device, m_rootSignature.Get(), *library, L"MultiresConduct");
        if (!graph)
            return std::unexpected(graph.error());

        m_conductGraph = std::make_unique<gpu::WorkGraph>(std::move(*graph));
        m_conductEntries = {m_conductGraph->EntrypointIndex(L"ConductPrepareNode"),
                            m_conductGraph->EntrypointIndex(L"ConductFlowsNode"),
                            m_conductGraph->EntrypointIndex(L"ConductApplyNode"),
                            m_conductGraph->EntrypointIndex(L"ConductEndNode")};
        if (std::ranges::contains(m_conductEntries, UINT32_MAX))
            return std::unexpected("multires_conduct_graph.cso に入口のノードが足りない");

        m_useConductGraph = true;

        return {};
    }

    std::expected<void, std::string> GpuMultires::CreateBuffers(ID3D12Device5* device,
                                                                const BakedReactionTable& table) {
        // 0 の大きさのバッファは作れないので、最低 16 バイト
        const std::array<uint64_t, BufferCount> sizes = BufferSizes();
        for (uint32_t i = 0; i < BufferCount; ++i) {
            const uint64_t bytes = std::max<uint64_t>(sizes[i], 16);
            m_buffers[i] = gpu::CreateBuffer(device, bytes, gpu::BufferKind::UnorderedAccess);
            m_uploads[i] = gpu::CreateBuffer(device, bytes, gpu::BufferKind::Upload);
            m_readbacks[i] = gpu::CreateBuffer(device, bytes, gpu::BufferKind::Readback);
            if (!m_buffers[i] || !m_uploads[i] || !m_readbacks[i])
                return std::unexpected("多重解像度のバッファを作れない");
        }

        m_requestUpload = gpu::CreateBuffer(device, REQUEST_UPLOAD_BYTES * REQUEST_UPLOAD_SLOTS,
                                            gpu::BufferKind::Upload);
        if (!m_requestUpload)
            return std::unexpected("要求の写しのバッファを作れない");

        // --- 活性の一覧 2 本 ---
        for (ComPtr<ID3D12Resource>& buffer : m_activity) {
            buffer = gpu::CreateBuffer(device, ActivityBytes(), gpu::BufferKind::UnorderedAccess);
            if (!buffer)
                return std::unexpected("活性の一覧のバッファを作れない");
        }

        m_activityUpload = gpu::CreateBuffer(device, ACTIVITY_FIRST_OFFSET + ActivityBytes(), gpu::BufferKind::Upload);
        m_activityReadback = gpu::CreateBuffer(device, ActivityBytes(), gpu::BufferKind::Readback);
        if (!m_activityUpload || !m_activityReadback)
            return std::unexpected("活性の一覧の写しを作れない");

        const D3D12_GPU_VIRTUAL_ADDRESS graphInput = m_buffers[BufferGraphInput]->GetGPUVirtualAddress();
        m_constants.graphInputLow = static_cast<uint32_t>(graphInput);
        m_constants.graphInputHigh = static_cast<uint32_t>(graphInput >> 32);

        m_tables = {
            CreateFilledUpload(device, std::span(table.species)), CreateFilledUpload(device, std::span(table.rules)),
            CreateFilledUpload(device, std::span(table.ruleIndex)), CreateFilledUpload(device, std::span(table.rates))};
        if (!std::ranges::all_of(m_tables, [](const auto& buffer) { return buffer != nullptr; }))
            return std::unexpected("反応の表のバッファを作れない");

        const D3D12_QUERY_HEAP_DESC queryDesc{
            .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = MAX_TIMESTAMPS, .NodeMask = 0};
        if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_timestamps))))
            return std::unexpected("タイムスタンプのヒープを作れない");

        m_timestampReadback = gpu::CreateBuffer(device, uint64_t{MAX_TIMESTAMPS} * sizeof(uint64_t),
                                                gpu::BufferKind::Readback);
        if (!m_timestampReadback)
            return std::unexpected("タイムスタンプの読み戻しを作れない");

        return {};
    }

    std::array<uint64_t, GpuMultires::BufferCount> GpuMultires::BufferSizes() const {
        std::array<uint64_t, BufferCount> sizes{};
        sizes[BufferBlocks] = uint64_t{m_blockCapacity} * sizeof(MrBlock);
        sizes[BufferCells] = (m_blockCapacity + (PageCount() * MR_BLOCK_CELLS)) * sizeof(RxCell);
        sizes[BufferFractions] = uint64_t{m_capacity.fractions} * MR_BLOCK_CELLS * sizeof(MrFraction);
        sizes[BufferCounters] = uint64_t{MR_COUNTER_COUNT} * sizeof(uint32_t);
        const uint64_t overflowWords = m_wideCells ? PageCount() * MR_WIDE_PAGE_WORDS : 0;
        sizes[BufferTreeWords] = (uint64_t{OverflowBaseWord()} + overflowWords) * sizeof(uint32_t);
        sizes[BufferFreeFractions] = uint64_t{m_capacity.fractions} * sizeof(uint32_t);
        sizes[BufferLedger] = uint64_t{MR_LEDGER_LEVELS} * m_capacity.ledgerColumns * sizeof(uint64_t);
        sizes[BufferRequests] = uint64_t{MR_MAX_REQUESTS} * sizeof(MrRequest);
        sizes[BufferStates] = uint64_t{MR_MAX_REQUESTS} * sizeof(MrRequestState);
        sizes[BufferConduction] = (uint64_t{m_blockCapacity} * CONDUCT_MARK_BYTES) +
                                  (PageCount() * MR_BLOCK_CELLS * CONDUCT_DELTA_BYTES);
        sizes[BufferGraphInput] = ConductRecordsOffset() + ((uint64_t{m_blockCapacity} + 1) * sizeof(uint32_t));

        return sizes;
    }

    uint64_t GpuMultires::PageCount() const {
        return uint64_t{m_capacity.observerBlocks} + m_capacity.pages;
    }

    // 伝導の一覧のレコードの位置(multires_bindings.hlsli の ConductRecordsOffset)
    uint64_t GpuMultires::ConductRecordsOffset() const {
        return GRAPH_INPUT_EXPAND_RECORDS + ((uint64_t{m_capacity.worldBlocks} + 1) * sizeof(uint32_t));
    }

    // GPU の入力の最初の中身: 頁に広げて刻む一覧と伝導の一覧の見出し(入口の番号と番地は変わらない。数は GPU が書く)とレコード 0 だけ。
    // ほかは 0(要求の処理が毎回書いてから読む)
    std::vector<std::byte> GpuMultires::MakeGraphInputImage() const {
        std::vector<std::byte> image(BufferSizes()[BufferGraphInput]);
        const D3D12_GPU_VIRTUAL_ADDRESS base = m_buffers[BufferGraphInput]->GetGPUVirtualAddress();
        const auto writeList = [&](uint32_t headerOffset, uint64_t recordsOffset, uint32_t entry) {
            const D3D12_NODE_GPU_INPUT header{
                .EntrypointIndex = entry,
                .NumRecords = 1,
                .Records = {.StartAddress = base + recordsOffset, .StrideInBytes = sizeof(uint32_t)}};
            std::memcpy(image.data() + headerOffset, &header, sizeof(header));
            const uint32_t emptyRecord = MR_NO_BLOCK;
            std::memcpy(image.data() + recordsOffset, &emptyRecord, sizeof(emptyRecord));
        };

        writeList(GRAPH_INPUT_EXPAND_HEADER, GRAPH_INPUT_EXPAND_RECORDS,
                  m_activityGraph ? m_activityEntries[ActivityEntryExpand] : 0);
        for (uint32_t i = 0; i < GRAPH_INPUT_CONDUCT_HEADERS.size(); ++i)
            writeList(GRAPH_INPUT_CONDUCT_HEADERS[i], ConductRecordsOffset(), m_conductGraph ? m_conductEntries[i] : 0);

        return image;
    }

    // u6 の中身: [世界の枠の空き][取り合いの印][索引][世界の頁の空き](CPU の木では別々の配列)
    std::vector<std::byte> GpuMultires::MakeTreeWordsImage(const MultiresNest& nest) const {
        const std::span<const uint32_t> freeBlocks = std::span(nest.freeBlocks).first(m_capacity.worldBlocks);
        const std::span<const uint32_t> freePages = std::span(nest.freeBlocks).subspan(m_capacity.worldBlocks);
        std::vector<uint32_t> words;
        words.reserve(BufferSizes()[BufferTreeWords] / sizeof(uint32_t));
        words.insert(words.end(), freeBlocks.begin(), freeBlocks.end());
        words.insert(words.end(), nest.claims.begin(), nest.claims.end());
        words.insert(words.end(), nest.index.begin(), nest.index.end());
        words.insert(words.end(), freePages.begin(), freePages.end());
        AppendOverflowImage(nest, words);

        std::vector<std::byte> image(words.size() * sizeof(uint32_t));
        std::memcpy(image.data(), words.data(), image.size());

        return image;
    }

    // --- 成分の二段(T-0176。shaders/common/multires_wide.hlsli の並び)---

    // u6 の溢れの領域の始まり: [世界の枠の空き][取り合いの印][索引][世界の頁の空き] の後ろ
    uint32_t GpuMultires::OverflowBaseWord() const {
        return (m_capacity.worldBlocks * 2) + m_capacity.indexEntries + m_capacity.pages;
    }

    // CPU の溢れを GPU に写せるか(頁の溢れの枠・1 セルの成分の数に入る。端数の溢れはまだ持てない)
    bool GpuMultires::OverflowFits(const MultiresNest& nest) const {
        if (!nest.wideCells)
            return true;

        if (!m_wideCells || nest.cellOverflow.size() != PageCount())
            return false;

        for (uint32_t fractionSlot = 0; fractionSlot < m_capacity.fractions; ++fractionSlot) {
            if (FractionHasOverflow(nest, fractionSlot))
                return false;
        }

        constexpr uint32_t MOST_TAIL = RX_GPU_WIDE_SPECIES - RX_MAX_CELL_SPECIES;
        for (const MultiresOverflowArea& area : nest.cellOverflow) {
            if (area.offsets[MR_BLOCK_CELLS] > MR_WIDE_PAGE_ENTRIES)
                return false;

            for (uint32_t index = 0; index < MR_BLOCK_CELLS; ++index) {
                if (area.offsets[index + 1] - area.offsets[index] > MOST_TAIL)
                    return false;
            }
        }

        return true;
    }

    // 頁ごとの溢れの像(今の面 0 = CPU の溢れ、面 1 = 0)
    void GpuMultires::AppendOverflowImage(const MultiresNest& nest, std::vector<uint32_t>& words) const {
        if (!m_wideCells)
            return;

        const size_t first = words.size();
        words.resize(first + (PageCount() * MR_WIDE_PAGE_WORDS), 0);
        if (!nest.wideCells)
            return;

        for (uint32_t page = 0; page < PageCount(); ++page) {
            const MultiresOverflowArea& area = nest.cellOverflow[page];
            const size_t side = first + MrWideSideWord(0, page, 0);
            std::ranges::copy(area.offsets, words.begin() + static_cast<ptrdiff_t>(side));
            for (uint32_t entry = 0; entry < area.offsets[MR_BLOCK_CELLS]; ++entry) {
                const size_t word = side + MrWideEntryWord(entry);
                words[word] = area.species[entry];
                words[word + 1] = static_cast<uint32_t>(area.amounts[entry]);
                words[word + 2] = static_cast<uint32_t>(area.amounts[entry] >> 32);
            }
        }
    }

    // 読み戻した u6 の溢れ(今の面)を CPU の溢れにする(並びは同じ: セルの番号の順に詰める)
    void GpuMultires::ReadOverflow(std::span<const uint32_t> words, MultiresNest& nest) const {
        nest.wideCells = false;
        if (!m_wideCells)
            return;

        EnableWideCells(nest);
        const uint32_t base = OverflowBaseWord();
        for (uint32_t page = 0; page < PageCount(); ++page) {
            const uint32_t current = words[MrWidePageWord(base, page)];
            const auto side = words.subspan(MrWideSideWord(base, page, current), MR_WIDE_SIDE_WORDS);
            MultiresOverflowArea& area = nest.cellOverflow[page];
            std::ranges::copy(side.first(MR_BLOCK_CELLS + 1), area.offsets.begin());
            const uint32_t count = area.offsets[MR_BLOCK_CELLS];
            area.species.resize(count);
            area.amounts.resize(count);
            for (uint32_t entry = 0; entry < count; ++entry) {
                const uint32_t word = MrWideEntryWord(entry);
                area.species[entry] = side[word];
                area.amounts[entry] = (uint64_t{side[word + 2]} << 32) | side[word + 1];
            }
        }
    }

    uint64_t GpuMultires::ActivityBytes() const {
        return MR_ACTIVITY_RECORDS + (uint64_t{MrActivityCapacity(m_capacity.worldBlocks)} * sizeof(uint32_t));
    }

    // 一覧 list の中身: 先頭 + 空のレコード + slots(写す大きさは先頭とレコードの分だけ)
    std::vector<std::byte> GpuMultires::MakeActivityList(uint32_t list, std::span<const uint32_t> slots) const {
        const auto count = static_cast<uint32_t>(slots.size());
        const ActivityHeader header{
            .input = {.EntrypointIndex = m_activityEntries[ActivityEntrySeed],
                      .NumRecords = 1 + count,
                      .Records = {.StartAddress = m_activity[list]->GetGPUVirtualAddress() + MR_ACTIVITY_RECORDS,
                                  .StrideInBytes = sizeof(uint32_t)}},
            .reserved = 1 + count,
            .dropped = 0};
        std::vector<uint32_t> records = {MR_NO_BLOCK};
        records.insert(records.end(), slots.begin(), slots.end());

        std::vector<std::byte> bytes(sizeof(header) + (records.size() * sizeof(uint32_t)));
        std::memcpy(bytes.data(), &header, sizeof(header));
        std::memcpy(bytes.data() + sizeof(header), records.data(), records.size() * sizeof(uint32_t));

        return bytes;
    }

    bool GpuMultires::RecordUpload(ID3D12GraphicsCommandList10* list, const MultiresNest& nest) {
        if (nest.blocks.size() != m_blockCapacity || nest.seeds.size() != m_capacity.worldBlocks ||
            nest.cells.size() * sizeof(RxCell) != BufferSizes()[BufferCells] ||
            nest.freeBlocks.size() != size_t{m_capacity.worldBlocks} + m_capacity.pages ||
            nest.claims.size() != m_capacity.worldBlocks ||
            nest.fractions.size() != size_t{m_capacity.fractions} * MR_BLOCK_CELLS ||
            nest.index.size() != m_capacity.indexEntries || nest.ledger.size() != BufferSizes()[BufferLedger] / 8)
            return false;

        if (!OverflowFits(nest)) {
            Log(Channel::Gpu, Level::Error,
                "成分の溢れを GPU に写せない(溢れを使う世界か、頁の溢れの枠・1 セルの成分の数を超える。T-0176)");
            return false;
        }

        // 要求の途中の値・伝導の作業場(印と変化は 0 から)・GPU の入力は写さない(毎回の処理が書いてから読む)
        const std::vector<std::byte> zeros(std::max(BufferSizes()[BufferStates], BufferSizes()[BufferConduction]));
        const std::vector<std::byte> graphInput = MakeGraphInputImage();
        const std::vector<std::byte> treeWords = MakeTreeWordsImage(nest);
        const std::array<std::span<const std::byte>, BufferCount> sources = {
            std::as_bytes(std::span(nest.blocks)),
            std::as_bytes(std::span(nest.cells)),
            std::as_bytes(std::span(nest.fractions)),
            std::as_bytes(std::span(nest.counters)),
            std::span(treeWords),
            std::as_bytes(std::span(nest.freeFractions)),
            std::as_bytes(std::span(nest.ledger)),
            std::as_bytes(std::span(nest.requests)),
            std::span(zeros).first(BufferSizes()[BufferStates]),
            std::span(zeros).first(BufferSizes()[BufferConduction]),
            std::span(graphInput)};
        std::array<D3D12_RESOURCE_BARRIER, BufferCount> barriers{};
        for (uint32_t i = 0; i < BufferCount; ++i) {
            if (!WriteUpload(m_uploads[i].Get(), sources[i]))
                return false;

            // COMMON から暗黙に COPY_DEST へ昇格する。写した後は UAV へ
            list->CopyResource(m_buffers[i].Get(), m_uploads[i].Get());
            barriers[i] = gpu::Transition(m_buffers[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        list->ResourceBarrier(BufferCount, barriers.data());

        // --- 活性の一覧: この刻みの種 = CPU の種、もう 1 本は空 ---
        const std::vector<uint32_t> seeds = SeedSlots(nest);
        if (seeds.size() + 1 > MrActivityCapacity(m_capacity.worldBlocks))
            return false;

        m_activityCurrent = 0;
        m_activityWrite = 0;
        const std::vector<std::byte> first = MakeActivityList(0, seeds);
        std::vector<std::byte> staging(ACTIVITY_FIRST_OFFSET);
        for (uint32_t i = 0; i < ACTIVITY_LISTS; ++i) {
            const std::vector<std::byte> empty = MakeActivityList(i, {});
            std::ranges::copy(empty, staging.begin() + static_cast<ptrdiff_t>(i * ACTIVITY_EMPTY_STRIDE));
        }

        staging.insert(staging.end(), first.begin(), first.end());
        if (!WriteUpload(m_activityUpload.Get(), staging))
            return false;

        list->CopyBufferRegion(m_activity[0].Get(), 0, m_activityUpload.Get(), ACTIVITY_FIRST_OFFSET, first.size());
        list->CopyBufferRegion(m_activity[1].Get(), 0, m_activityUpload.Get(), ACTIVITY_EMPTY_STRIDE,
                               ACTIVITY_EMPTY_BYTES);
        const std::array<D3D12_RESOURCE_BARRIER, ACTIVITY_LISTS> toUav = {
            gpu::Transition(m_activity[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            gpu::Transition(m_activity[1].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        list->ResourceBarrier(ACTIVITY_LISTS, toUav.data());

        return true;
    }

    // --- 結び付けと起動 ---

    void GpuMultires::BindRoot(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing) const {
        static_assert(sizeof(RootConstants) == ROOT_CONSTANT_COUNT * sizeof(uint32_t));
        static_assert(BufferCount + 3 == UAV_COUNT);  // u4・u5 は外のバッファ、u13 は活性の一覧
        static_assert((UAV_COUNT * 2) + ROOT_CONSTANT_COUNT + 2 + (TABLE_COUNT * 2) <= 64);  // ルート署名の語
        for (uint32_t i = 0; i < BufferCount; ++i) {
            const uint32_t parameter = i < EXTERNAL_VIEW_FIRST ? i : i + 2;
            list->SetComputeRootUnorderedAccessView(parameter, m_buffers[i]->GetGPUVirtualAddress());
        }

        const D3D12_GPU_VIRTUAL_ADDRESS standIn = m_buffers[BufferCells]->GetGPUVirtualAddress();
        for (uint32_t i = 0; i < m_externalViews.size(); ++i) {
            const D3D12_GPU_VIRTUAL_ADDRESS view = m_externalViews[i] != 0 ? m_externalViews[i] : standIn;
            list->SetComputeRootUnorderedAccessView(EXTERNAL_VIEW_FIRST + i, view);
        }

        list->SetComputeRootUnorderedAccessView(ACTIVITY_VIEW, m_activity[m_activityWrite]->GetGPUVirtualAddress());
        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT, &m_constants, 0);
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing);
        for (uint32_t i = 0; i < TABLE_COUNT; ++i)
            list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(i), m_tables[i]->GetGPUVirtualAddress());
    }

    void GpuMultires::SetGraphProgram(ID3D12GraphicsCommandList10* list) {
        list->SetComputeRootSignature(m_rootSignature.Get());
        m_graph->SetProgram(list, !m_graphInitialized);
        m_graphInitialized = true;
    }

    void GpuMultires::SetActivityProgram(ID3D12GraphicsCommandList10* list) {
        list->SetComputeRootSignature(m_rootSignature.Get());
        m_activityGraph->SetProgram(list, !m_activityGraphInitialized);
        m_activityGraphInitialized = true;
    }

    void GpuMultires::DispatchGraph(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                    uint32_t entry, const void* record, uint32_t recordBytes) {
        SetGraphProgram(list);
        BindRoot(list, debugRing);
        gpu::WorkGraph::DispatchFromCpu(list, m_entries[entry], record, 1, recordBytes);
        UavBarrier(list);
    }

    void GpuMultires::RecordTreePass(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                     uint32_t pass, uint32_t groupCount) {
        list->SetComputeRootSignature(m_rootSignature.Get());
        list->SetPipelineState(m_treePipelines[pass].Get());
        BindRoot(list, debugRing);
        list->Dispatch(groupCount, 1, 1);
        UavBarrier(list);
    }

    // --- 世界の木 ---

    bool GpuMultires::RecordRequests(ID3D12GraphicsCommandList10* list, std::span<const MrRequest> requests) {
        if (requests.size() > MR_MAX_REQUESTS)
            return false;

        // --- 写しの枠に [数][要求の一覧] を書く ---
        const uint64_t offset = REQUEST_UPLOAD_BYTES * (m_requestUploadCursor++ % REQUEST_UPLOAD_SLOTS);
        const auto count = static_cast<uint32_t>(requests.size());
        void* mapped = nullptr;
        const D3D12_RANGE noRead{.Begin = 0, .End = 0};
        if (FAILED(m_requestUpload->Map(0, &noRead, &mapped)))
            return false;

        auto* bytes = static_cast<std::byte*>(mapped) + offset;
        std::memcpy(bytes, &count, sizeof(count));
        std::memcpy(bytes + 256, requests.data(), requests.size_bytes());
        m_requestUpload->Unmap(0, nullptr);

        // --- 一覧と数える欄の「要求の数」へ写す ---
        ID3D12Resource* requestBuffer = m_buffers[BufferRequests].Get();
        ID3D12Resource* counters = m_buffers[BufferCounters].Get();
        const std::array<D3D12_RESOURCE_BARRIER, 2> toCopy = {
            gpu::Transition(requestBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
            gpu::Transition(counters, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST)};
        list->ResourceBarrier(static_cast<UINT>(toCopy.size()), toCopy.data());
        if (count > 0)
            list->CopyBufferRegion(requestBuffer, 0, m_requestUpload.Get(), offset + 256, requests.size_bytes());

        list->CopyBufferRegion(counters, uint64_t{MR_COUNTER_REQUESTS} * sizeof(uint32_t), m_requestUpload.Get(),
                               offset, sizeof(uint32_t));
        const std::array<D3D12_RESOURCE_BARRIER, 2> toUav = {
            gpu::Transition(requestBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            gpu::Transition(counters, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        list->ResourceBarrier(static_cast<UINT>(toUav.size()), toUav.data());

        return true;
    }

    void GpuMultires::RecordFoldPages(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                      uint64_t tick) {
        RecordFoldPages(list, debugRing, tick, MrExactFoldTolerance());
    }

    void GpuMultires::RecordFoldPages(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                      uint64_t tick, const MrFoldTolerance& tolerance) {
        m_constants.foldTolerance = MrPackFoldTolerance(tolerance);
        m_constants.tickLow = static_cast<uint32_t>(tick);
        m_constants.tickHigh = static_cast<uint32_t>(tick >> 32);
        RecordTreePass(list, debugRing, PassFoldCheck, std::max(1u, m_capacity.worldBlocks));
        RecordTreePass(list, debugRing, PassFold, 1);
    }

    void GpuMultires::RecordQuietRequests(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                          uint64_t tick) {
        RecordQuietRequests(list, debugRing, tick, MrExactFoldTolerance());
    }

    void GpuMultires::RecordQuietRequests(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                          uint64_t tick, const MrFoldTolerance& tolerance) {
        m_constants.foldTolerance = MrPackFoldTolerance(tolerance);
        m_constants.tickLow = static_cast<uint32_t>(tick);
        m_constants.tickHigh = static_cast<uint32_t>(tick >> 32);
        RecordTreePass(list, debugRing, PassQuietCheck, std::max(1u, m_capacity.worldBlocks));
        RecordTreePass(list, debugRing, PassQuiet, 1);
    }

    void GpuMultires::RecordProcessRequests(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing) {
        m_activityWrite = m_activityCurrent;  // 木を変えたブロックはこの刻みの種へ
        const uint32_t requestGroups = GroupsFor(MR_MAX_REQUESTS, TREE_THREADS);
        RecordTreePass(list, debugRing, PassResolve, requestGroups);
        RecordTreePass(list, debugRing, PassSettle, requestGroups);
        RecordTreePass(list, debugRing, PassAllocate, 1);

        // --- 適用(Work Graph。GPU の入力は SRV の状態で読む)---
        ID3D12Resource* graphInput = m_buffers[BufferGraphInput].Get();
        const D3D12_RESOURCE_BARRIER toInput = gpu::Transition(graphInput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &toInput);
        SetGraphProgram(list);
        BindRoot(list, debugRing);
        gpu::WorkGraph::DispatchFromGpu(list, graphInput->GetGPUVirtualAddress() + GRAPH_INPUT_REFINE_HEADER);
        UavBarrier(list);
        gpu::WorkGraph::DispatchFromGpu(list, graphInput->GetGPUVirtualAddress() + GRAPH_INPUT_COARSEN_HEADER);
        UavBarrier(list);
        const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(graphInput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);

        // --- 解放と索引の作り直し(印がある時だけ働く)---
        RecordTreePass(list, debugRing, PassRelease, 1);
        RecordTreePass(list, debugRing, PassClearIndex, GroupsFor(m_capacity.indexEntries, TREE_THREADS));
        RecordTreePass(list, debugRing, PassFillIndex, GroupsFor(m_capacity.worldBlocks, TREE_THREADS));
    }

    // --- 観察の枠 ---

    void GpuMultires::RecordRefineShadow(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                         uint32_t parentSlot, uint32_t firstChildSlot, uint32_t levelCount,
                                         const MultiresPoint& point) {
        const std::array<int64_t, 3> coordinates = {point.x, point.y, point.z};
        for (size_t axis = 0; axis < coordinates.size(); ++axis) {
            const auto value = static_cast<uint64_t>(coordinates[axis]);
            m_constants.point[axis * 2] = static_cast<uint32_t>(value);
            m_constants.point[(axis * 2) + 1] = static_cast<uint32_t>(value >> 32);
        }

        m_constants.pointLevel = point.level;
        const RefineRecord record{.request = MR_NO_BLOCK,
                                  .parentSlot = parentSlot,
                                  .childSlot = firstChildSlot,
                                  .levelsLeft = levelCount,
                                  .kind = MR_BLOCK_SHADOW,
                                  .depth = 0};
        DispatchGraph(list, debugRing, EntryRefine, &record, sizeof(record));
    }

    void GpuMultires::RecordRemoveShadow(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                         uint32_t firstSlot, uint32_t levelCount) {
        const ChainRecord record{.slot = firstSlot, .levelsLeft = levelCount};
        DispatchGraph(list, debugRing, EntryRemoveShadow, &record, sizeof(record));
    }

    void GpuMultires::RecordPullBack(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                     uint32_t firstShadowSlot, uint32_t levelCount) {
        const ChainRecord record{.slot = firstShadowSlot, .levelsLeft = levelCount};
        DispatchGraph(list, debugRing, EntryPullBack, &record, sizeof(record));
    }

    void GpuMultires::SetTick(uint64_t worldSeed, uint64_t tick) {
        m_constants.seedLow = static_cast<uint32_t>(worldSeed);
        m_constants.seedHigh = static_cast<uint32_t>(worldSeed >> 32);
        m_constants.tickLow = static_cast<uint32_t>(tick);
        m_constants.tickHigh = static_cast<uint32_t>(tick >> 32);
    }

    bool GpuMultires::RecordStepActive(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                       uint64_t worldSeed, uint64_t tick, const MultiresStepOptions& options) {
        if (!m_activityGraph || m_wideCells)
            return false;  // 活性のグラフはまだ溢れを読まない(T-0212)

        SetTick(worldSeed, tick);
        m_constants.stepFlags = options.conduction ? STEP_FLAG_CONDUCTION | STEP_FLAG_LISTED : 0;

        // --- 起こす段がつつかれたブロックの印を直し、起こす刻みが来たブロックをこの刻みの種の一覧へ足す(CPU の StepActive の種。
        //     次の刻みの種は書かない: 刻んだブロックは見出しの wakeTick で起こす。T-0124)---
        m_activityWrite = m_activityCurrent;
        RecordWake(list, debugRing, STEP_FLAG_WAKE_SEEDS);

        const uint32_t next = m_activityCurrent ^ 1u;
        ID3D12Resource* input = m_activity[m_activityCurrent].Get();
        ID3D12Resource* output = m_activity[next].Get();

        // --- 次の刻みの一覧を空にし、この刻みの一覧を GPU の入力の状態へ ---
        const std::array<D3D12_RESOURCE_BARRIER, 2> before = {
            gpu::Transition(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST),
            gpu::Transition(input, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
        list->ResourceBarrier(static_cast<UINT>(before.size()), before.data());
        list->CopyBufferRegion(output, 0, m_activityUpload.Get(), uint64_t{next} * ACTIVITY_EMPTY_STRIDE,
                               ACTIVITY_EMPTY_BYTES);
        const D3D12_RESOURCE_BARRIER outputToUav = gpu::Transition(output, D3D12_RESOURCE_STATE_COPY_DEST,
                                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &outputToUav);

        // --- 種 → 面の隣 → 刻む(Work Graph)。観察の枠は全部刻む。伝導を入れるなら、グラフは伝導の一覧に足すだけで、刻むのは伝導の段
        //     (使い終わったこの刻みの種の一覧は、小刻みの終わりに起こす一覧に使い回す。T-0109)---
        m_activityWrite = next;
        if (options.conduction)
            RecordConductPass(list, debugRing, ConductPassBegin, 1);

        SetActivityProgram(list);
        BindRoot(list, debugRing);
        gpu::WorkGraph::DispatchFromGpu(list, input->GetGPUVirtualAddress());
        UavBarrier(list);
        const D3D12_RESOURCE_BARRIER inputToUav = gpu::Transition(input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &inputToUav);
        if (options.conduction) {
            RecordConduction(list, debugRing, options, m_activityCurrent);
        } else {
            if (m_capacity.observerBlocks > 0) {
                const ObserverRecord record{.grid = {m_capacity.observerBlocks, 1, 1},
                                            .firstSlot = m_capacity.worldBlocks};
                gpu::WorkGraph::DispatchFromCpu(list, m_activityEntries[ActivityEntryObserver], &record, 1,
                                                sizeof(record));
                UavBarrier(list);
            }

            RecordExpandPages(list, debugRing);  // 一様で反応が進むブロック(T-0102)
        }

        m_activityCurrent = next;

        return true;
    }

    void GpuMultires::RecordStep(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                 uint64_t worldSeed, uint64_t tick, const MultiresStepOptions& options) {
        SetTick(worldSeed, tick);
        m_constants.stepFlags = options.conduction ? STEP_FLAG_CONDUCTION : 0;
        FX_ASSERT(!m_wideCells || !options.implicitConduction);  // 陰解法の段はまだ溢れを読まない(T-0211 の残り)
        if (options.conduction) {
            RecordWake(list, debugRing, 0);                              // つつかれたブロックの印を直す
            RecordConduction(list, debugRing, options, ACTIVITY_LISTS);  // 全部を刻むので起こさない
            return;
        }

        RecordStepWait(list, debugRing);
    }

    void GpuMultires::RecordWake(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                 uint32_t extraFlags) {
        const uint32_t flags = std::exchange(m_constants.stepFlags, m_constants.stepFlags | extraFlags);
        list->SetComputeRootSignature(m_rootSignature.Get());
        list->SetPipelineState(m_wakePipeline.Get());
        BindRoot(list, debugRing);
        list->Dispatch(GroupsFor(m_blockCapacity, WAKE_THREADS), 1, 1);
        UavBarrier(list);
        m_constants.stepFlags = flags;
    }

    void GpuMultires::RecordStepWait(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing) {
        // --- 起こす段(つつかれたブロックの印を直す)→ 全部の枠を刻む ---
        RecordWake(list, debugRing, 0);
        list->SetPipelineState(m_wideCells ? m_stepWideWaitPipeline.Get() : m_stepWaitPipeline.Get());
        BindRoot(list, debugRing);
        list->Dispatch(std::max(1u, m_blockCapacity), 1, 1);
        UavBarrier(list);

        // --- 一様で反応が進むブロックを頁に広げて刻む(数は GPU が決めるので、世界の枠の数だけグループを投げる)---
        RecordTreePass(list, debugRing, PassExpand, 1);
        list->SetPipelineState(m_wideCells ? m_stepWideExpandedWaitPipeline.Get() : m_stepExpandedWaitPipeline.Get());
        list->Dispatch(std::max(1u, m_capacity.worldBlocks), 1, 1);
        UavBarrier(list);
    }

    void GpuMultires::RecordConductPass(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                        uint32_t pass, uint32_t groupCount) {
        list->SetComputeRootSignature(m_rootSignature.Get());
        list->SetPipelineState(m_wideCells ? m_conductWidePipelines[pass].Get() : m_conductPipelines[pass].Get());
        BindRoot(list, debugRing);
        list->Dispatch(groupCount, 1, 1);
        UavBarrier(list);
    }

    // 熱の伝導を入れた刻みの段(CPU の StepBlocks・StepConduction と同じ順。multires_conduct.hlsli の先頭)。活性(stepFlags の LISTED)なら
    // 伝導の一覧のブロックだけ(数は GPU が決めるので、全部の枠の数だけグループを投げる。一覧は活性のグラフが作り終えている)。
    // 細かいレベルの刻み(T-0109): 小刻みを最大回数 4^maxSubcycleGap ぶん積む(Work Graphs に全体の同期が無いので小刻みごとに段を分ける。
    // その小刻みに始まるレベルが無ければ段は空で抜ける)。wakeList = 小刻みの終わりに起こす一覧に使う活性の一覧(全部を刻むなら ACTIVITY_LISTS)
    void GpuMultires::RecordConduction(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                       const MultiresStepOptions& options, uint32_t wakeList) {
        FX_ASSERT(options.maxSubcycleGap <= MULTIRES_MAX_SUBCYCLE_GAP);
        const uint32_t flags = m_constants.stepFlags;
        const uint32_t substeps = 1u << (2 * options.maxSubcycleGap);
        for (uint32_t substep = 0; substep < substeps; ++substep) {
            m_constants.stepFlags = SubcycleFlags(flags, options, substep);
            RecordConductSubstep(list, debugRing, options);
            if (substep + 1 < substeps)
                RecordSubstepEnd(list, debugRing, wakeList);
        }

        // --- 細かいレベルの熱の陰解法(T-0132。最後の小刻みの流れの後。CPU の StepConduction の AddImplicitConduction)---
        if (options.implicitConduction)
            RecordImplicitConduction(list, debugRing, options);

        // --- 最後の小刻みの変化を足して反応 ---
        RecordConductStage(list, debugRing, ConductPassApply);
        m_constants.stepFlags = flags;
    }

    // 陰解法の段(sim/gpu_multires_implicit)。刻みの印と stepFlags(最後の小刻み)はこの刻みのまま
    void GpuMultires::RecordImplicitConduction(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                               const MultiresStepOptions& options) {
        FX_ASSERT(m_implicit != nullptr);
        if (m_implicit == nullptr) {
            Log(Channel::Gpu, Level::Error, "陰解法の段が無い(EnableImplicitConduction を呼んでいない)");
            return;
        }

        m_implicit->Record(list, debugRing, *this, options);
    }

    // 小刻み 1 回: 印 → 頁 → 端数の枠 → 埋める →(陰解法の刻みなら系に入れるブロックを選ぶ。T-0178)→ 流れ(その小刻みに始まるレベルのブロックだけ)
    void GpuMultires::RecordConductSubstep(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                           const MultiresStepOptions& options) {
        RecordConductPass(list, debugRing, ConductPassMark, std::max(1u, m_blockCapacity));
        RecordTreePass(list, debugRing, PassExpand, 1);
        RecordTreePass(list, debugRing, PassFractions, 1);
        RecordConductStage(list, debugRing, ConductPassPrepare);
        if ((m_constants.stepFlags & STEP_FLAG_IMPLICIT) != 0 && m_implicit != nullptr)
            m_implicit->RecordAdmit(list, debugRing, *this, options);

        RecordConductStage(list, debugRing, ConductPassFlows);
    }

    // 小刻みの終わり(T-0109。CPU の ApplyEndingDeltas・WakeChangedBlocks): 終わるレベルに変化を足す(ConductEnd)。活性なら、変わった
    // ブロックの面の隣を活性のグラフ(ActivitySeedNode → WakeFaceNode → ActivityStepNode)で起こし、初めて印を付けたブロックを伝導の一覧に足す。
    // 起こす一覧はこの刻みの種の一覧(使い終わっている)を空にして u13 に結び、ConductEnd に書かせる
    void GpuMultires::RecordSubstepEnd(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                       uint32_t wakeList) {
        if (wakeList >= ACTIVITY_LISTS) {
            RecordConductStage(list, debugRing, ConductPassEnd);
            return;
        }

        // --- 起こす一覧を空にして u13 に結び、変化を足す ---
        ID3D12Resource* wake = m_activity[wakeList].Get();
        const D3D12_RESOURCE_BARRIER toCopy = gpu::Transition(wake, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                              D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &toCopy);
        list->CopyBufferRegion(wake, 0, m_activityUpload.Get(), uint64_t{wakeList} * ACTIVITY_EMPTY_STRIDE,
                               ACTIVITY_EMPTY_BYTES);
        const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(wake, D3D12_RESOURCE_STATE_COPY_DEST,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);
        const uint32_t write = std::exchange(m_activityWrite, wakeList);
        RecordConductStage(list, debugRing, ConductPassEnd);
        m_activityWrite = write;

        // --- 面の隣を起こす(GPU の入力 = 起こす一覧)---
        const D3D12_RESOURCE_BARRIER toInput = gpu::Transition(wake, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &toInput);
        const uint32_t flags = std::exchange(m_constants.stepFlags, m_constants.stepFlags | STEP_FLAG_SUBSTEP_WAKE);
        SetActivityProgram(list);
        BindRoot(list, debugRing);
        gpu::WorkGraph::DispatchFromGpu(list, wake->GetGPUVirtualAddress());
        UavBarrier(list);
        m_constants.stepFlags = flags;
        const D3D12_RESOURCE_BARRIER back = gpu::Transition(wake, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &back);
    }

    // 伝導の段 1 つ(埋める・流れ・足す・小刻みの終わり)。活性で Work Graph 版を使うなら GPU の入力 = 伝導の一覧(段ごとに見出しが別)、
    // ほかは Compute
    void GpuMultires::RecordConductStage(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                         uint32_t pass) {
        // 陰解法の段(T-0132)は流れの後に伝導の一覧を足すが、Work Graph の見出しの数は TreeFractions が写した後なので、足す段は Compute で
        const bool listed = (m_constants.stepFlags & STEP_FLAG_LISTED) != 0;
        const bool implicitApply = pass == ConductPassApply && (m_constants.stepFlags & STEP_FLAG_IMPLICIT) != 0;
        if (!m_useConductGraph || !listed || implicitApply) {
            RecordConductPass(list, debugRing, pass, std::max(1u, m_blockCapacity));
            return;
        }

        ID3D12Resource* graphInput = m_buffers[BufferGraphInput].Get();
        const D3D12_RESOURCE_BARRIER toInput = gpu::Transition(graphInput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &toInput);
        list->SetComputeRootSignature(m_rootSignature.Get());
        m_conductGraph->SetProgram(list, !m_conductGraphInitialized);
        m_conductGraphInitialized = true;
        BindRoot(list, debugRing);
        const uint32_t header = GRAPH_INPUT_CONDUCT_HEADERS[pass - ConductPassPrepare];
        gpu::WorkGraph::DispatchFromGpu(list, graphInput->GetGPUVirtualAddress() + header);
        UavBarrier(list);
        const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(graphInput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);
    }

    bool GpuMultires::UseConductionGraph(bool use) {
        if (use && !m_conductGraph)
            return false;

        m_useConductGraph = use;

        return true;
    }

    void GpuMultires::RecordExpandPass(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing) {
        RecordTreePass(list, debugRing, PassExpand, 1);
    }

    void GpuMultires::RecordExpandPages(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing) {
        // --- 頁を配る(枠の順)→ 埋めて刻む(Work Graph の GPU の入力)---
        RecordTreePass(list, debugRing, PassExpand, 1);
        ID3D12Resource* graphInput = m_buffers[BufferGraphInput].Get();
        const D3D12_RESOURCE_BARRIER toInput = gpu::Transition(graphInput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &toInput);
        SetActivityProgram(list);
        BindRoot(list, debugRing);
        gpu::WorkGraph::DispatchFromGpu(list, graphInput->GetGPUVirtualAddress() + GRAPH_INPUT_EXPAND_HEADER);
        UavBarrier(list);
        const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(graphInput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);
    }

    // --- 外のバッファとパイプライン ---

    // 反応表の差し替え(T-0194): 表はアップロードのヒープにあり、BindRoot が記録のたびに結ぶので、持ち替えるだけで次のリストから効く
    std::expected<std::vector<ComPtr<ID3D12Resource>>, std::string> GpuMultires::ReplaceTable(
        const BakedReactionTable& table) {
        ComPtr<ID3D12Device> device;
        if (!m_tables[0] || FAILED(m_tables[0]->GetDevice(IID_PPV_ARGS(&device))))
            return std::unexpected("反応の表のバッファが無い");

        std::array<ComPtr<ID3D12Resource>, TABLE_COUNT> tables = {
            CreateFilledUpload(device.Get(), std::span(table.species)),
            CreateFilledUpload(device.Get(), std::span(table.rules)),
            CreateFilledUpload(device.Get(), std::span(table.ruleIndex)),
            CreateFilledUpload(device.Get(), std::span(table.rates))};
        if (!std::ranges::all_of(tables, [](const auto& buffer) { return buffer != nullptr; }))
            return std::unexpected("差し替える反応の表のバッファを作れない");

        std::vector<ComPtr<ID3D12Resource>> retired(m_tables.begin(), m_tables.end());
        m_tables = std::move(tables);

        return retired;
    }

    void GpuMultires::SetExternalViews(D3D12_GPU_VIRTUAL_ADDRESS first, D3D12_GPU_VIRTUAL_ADDRESS second) {
        m_externalViews = {first, second};
    }

    void GpuMultires::RecordExternalDispatch(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                             ID3D12PipelineState* pipeline, uint32_t groupCount,
                                             const std::array<uint32_t, 4>& external) {
        m_constants.external = external;

        list->SetComputeRootSignature(m_rootSignature.Get());
        list->SetPipelineState(pipeline);
        BindRoot(list, debugRing);
        list->Dispatch(groupCount, 1, 1);
        UavBarrier(list);
    }

    // --- 読み戻しと計測 ---

    void GpuMultires::RecordReadback(ID3D12GraphicsCommandList10* list) {
        // 状態と索引(要求・途中の値・伝導の作業場・GPU の入力は読まない)
        for (uint32_t i = 0; i <= BufferLedger; ++i)
            gpu::RecordCopyToReadback(list, m_buffers[i].Get(), m_readbacks[i].Get());

        gpu::RecordCopyToReadback(list, m_activity[m_activityCurrent].Get(), m_activityReadback.Get());

        if (m_timestampCount > 0) {
            list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, m_timestampCount,
                                   m_timestampReadback.Get(), 0);
        }
    }

    bool GpuMultires::Read(MultiresNest& nest) const {
        if (nest.blocks.size() != m_blockCapacity || nest.index.size() != m_capacity.indexEntries)
            nest = MakeMultiresNest(m_capacity);

        std::vector<uint32_t> treeWords(BufferSizes()[BufferTreeWords] / sizeof(uint32_t));
        const std::array<std::span<std::byte>, BufferLedger + 1> destinations = {
            std::as_writable_bytes(std::span(nest.blocks)),    std::as_writable_bytes(std::span(nest.cells)),
            std::as_writable_bytes(std::span(nest.fractions)), std::as_writable_bytes(std::span(nest.counters)),
            std::as_writable_bytes(std::span(treeWords)),      std::as_writable_bytes(std::span(nest.freeFractions)),
            std::as_writable_bytes(std::span(nest.ledger))};
        for (uint32_t i = 0; i < destinations.size(); ++i) {
            if (!gpu::ReadBuffer(m_readbacks[i].Get(), destinations[i]))
                return false;
        }

        // --- u6 を CPU の配列に分ける(取り合いの印は読まない: 要求の処理の外では全部 MR_NO_CLAIM)---
        const uint32_t worldBlocks = m_capacity.worldBlocks;
        const auto words = std::span(treeWords);
        const auto index = words.subspan(size_t{worldBlocks} * 2, m_capacity.indexEntries);
        const auto freePages = words.subspan((size_t{worldBlocks} * 2) + m_capacity.indexEntries, m_capacity.pages);
        std::ranges::copy(words.first(worldBlocks), nest.freeBlocks.begin());
        std::ranges::copy(freePages, nest.freeBlocks.begin() + worldBlocks);
        std::ranges::copy(index, nest.index.begin());
        ReadOverflow(words, nest);

        return true;
    }

    std::optional<GpuMultires::ActivitySeeds> GpuMultires::ReadSeeds() const {
        std::vector<std::byte> bytes(ActivityBytes());
        if (!gpu::ReadBuffer(m_activityReadback.Get(), bytes))
            return std::nullopt;

        ActivityHeader header{};
        std::memcpy(&header, bytes.data(), sizeof(header));
        const uint32_t capacity = MrActivityCapacity(m_capacity.worldBlocks);
        if (header.input.NumRecords == 0 || header.input.NumRecords > capacity)
            return std::nullopt;

        ActivitySeeds seeds{.slots = std::vector<uint32_t>(header.input.NumRecords - 1), .dropped = header.dropped};
        std::memcpy(seeds.slots.data(), bytes.data() + sizeof(header) + sizeof(uint32_t),
                    seeds.slots.size() * sizeof(uint32_t));
        std::ranges::sort(seeds.slots);
        const auto duplicates = std::ranges::unique(seeds.slots);
        seeds.slots.erase(duplicates.begin(), duplicates.end());

        return seeds;
    }

    void GpuMultires::RecordTimestamp(ID3D12GraphicsCommandList10* list, uint32_t index) {
        if (index >= MAX_TIMESTAMPS)
            return;

        list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
        m_timestampCount = std::max(m_timestampCount, index + 1);
    }

    std::vector<uint64_t> GpuMultires::ReadTimestamps(uint32_t count) const {
        std::vector<uint64_t> ticks(std::min(count, m_timestampCount));
        if (!gpu::ReadBuffer(m_timestampReadback.Get(), std::as_writable_bytes(std::span(ticks))))
            ticks.clear();

        return ticks;
    }

}  // namespace bicameral::sim
