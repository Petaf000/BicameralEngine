// probe_sim.cpp — 仮の刻みを単位の列として GPU で走らせる(フレームの枠ごとのリストに毎フレーム記録)と、その CPU リファレンス。
// 使い方とデータの流れは probe_sim.h、規則は shaders/common/probe_sim.hlsli。
#include "sim/probe_sim.h"

#include <algorithm>
#include <cstddef>

#include "core/aliases.h"
#include "core/log.h"
#include "gpu/com_ptr.h"
#include "gpu/resources.h"

namespace bicameral::sim {
    namespace {

        // ルート署名(shaders/sim/probe_bindings.hlsli と同じ順。伝導の Work Graph もこれをグローバルのルート署名に使う):
        //   u0 世界・u1 イベントのリング・u2/u3/u5 抽出・u4 重さの捨て場・u6 ハッシュの表・u7 コマンドキュー・
        //   u8 刻みのイベントの一時置き場・u9/u10 活性の一覧・u11 予定の印 → b0 単位の定数 → デバッグのリング
        //   → Work Graphs のカウンタ(u1 space1。T-0008)→ t0 フレームの入力
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{.uavCount = 12,
                                                       .rootConstantCount = PROBE_ROOT_CONSTANT_COUNT,
                                                       .debugRing = true,
                                                       .graphStats = true,
                                                       .srvCount = 1};

        constexpr uint32_t UAV_WORLD = 0;
        constexpr uint32_t UAV_EVENTS = 1;
        constexpr uint32_t UAV_EXTRACTION0 = 2;
        constexpr uint32_t UAV_EXTRACTION1 = 3;
        constexpr uint32_t UAV_BUSY_SINK = 4;
        constexpr uint32_t UAV_EXTRACTION2 = 5;
        constexpr uint32_t UAV_HASHES = 6;
        constexpr uint32_t UAV_COMMAND_QUEUE = 7;
        constexpr uint32_t UAV_TICK_EVENTS = 8;
        constexpr uint32_t UAV_ACTIVE_LIST0 = 9;
        constexpr uint32_t UAV_ACTIVE_LIST1 = 10;
        constexpr uint32_t UAV_BLOCK_SCHEDULE = 11;
        constexpr uint32_t SRV_INPUT = 0;

        constexpr uint32_t TIMESTAMPS_PER_SLOT = ProbeSim::MAX_UNITS_PER_FRAME + 2;  // 始め・単位ごと・終わり
        constexpr uint32_t CELL_BYTES = PROBE_CELL_COUNT * 4;
        constexpr uint32_t SLICE_BYTES = PROBE_SLICE_CELL_COUNT * 4;
        constexpr uint32_t BUSY_GROUPS = PROBE_GRID_SIZE / PROBE_GROUP_SIZE;
        constexpr uint32_t LINEAR_CELL_GROUPS = PROBE_CELL_COUNT / PROBE_LINEAR_GROUP_SIZE;
        constexpr uint32_t EXTRACTION_BYTES = PROBE_EXTRACTION_WORDS * 4;

        // 活性の一覧の見出しは D3D12_NODE_GPU_INPUT そのもの(DispatchGraph が GPU のメモリから読む。probe_sim.hlsli)
        static_assert(offsetof(D3D12_NODE_GPU_INPUT, EntrypointIndex) == size_t{PROBE_ACTIVE_LIST_ENTRYPOINT} * 4);
        static_assert(offsetof(D3D12_NODE_GPU_INPUT, NumRecords) == size_t{PROBE_ACTIVE_LIST_COUNT} * 4);
        static_assert(offsetof(D3D12_NODE_GPU_INPUT, Records) == size_t{PROBE_ACTIVE_LIST_ADDRESS} * 4);

        static_assert(offsetof(D3D12_NODE_GPU_INPUT, Records) +
                          offsetof(D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE, StrideInBytes) ==
                      size_t{PROBE_ACTIVE_LIST_STRIDE} * 4);

        static_assert(sizeof(D3D12_NODE_GPU_INPUT) <= PROBE_ACTIVE_LIST_HEADER_BYTES);
        constexpr uint32_t MAX_LOGGED_DEBUG_MESSAGES = 8;

        // 伝導のグラフのカウンタの番号と上限(probe_sim.hlsli の PROBE_STATS_* と同じ順。T-0008)。
        // WakeBlocks の出力は構造で 7 まで・ConductBlock は出力しない(一覧へは UAV で足す)ので、出力の「近い」は見ない。
        // 活性の一覧は全部のブロックが活性でも溢れない大きさなので、満杯のときだけ知らせる
        gpu::GraphStatsLayout MakeConductStatsLayout() {
            gpu::GraphStatsLayout layout{.name = "伝導(ProbeConduct)"};
            layout.nodes.resize(2);
            layout.nodes[PROBE_STATS_NODE_WAKE] = {.name = "WakeBlocks", .maxOutputRecords = PROBE_WAKE_MAX_RECORDS};
            layout.nodes[PROBE_STATS_NODE_CONDUCT] = {.name = "ConductBlock"};
            layout.gauges.resize(2);
            layout.gauges[PROBE_STATS_GAUGE_ACTIVE_LIST] = {
                .name = "活性の一覧", .capacity = PROBE_ACTIVE_LIST_CAPACITY, .warnPercent = 100};
            layout.gauges[PROBE_STATS_GAUGE_COMMAND_QUEUE] = {.name = "コマンドキュー",
                                                              .capacity = PROBE_COMMAND_QUEUE_CAPACITY};

            return layout;
        }

        template <typename T>
        void WriteAt(std::byte* base, uint32_t offset, const T& value) {
            std::memcpy(base + offset, &value, sizeof(T));
        }

        // イベントのリングの見出し(probe_sim.hlsli の PROBE_EVENT_HEADER_*)
        struct EventRingHeader {
            uint32_t requested = 0;    // リングに書こうとした数
            uint32_t tickDropped = 0;  // 刻みの一時置き場で落とした数
        };

        ComPtr<ID3D12PipelineState> LoadComputePipeline(ID3D12Device* device, ID3D12RootSignature* rootSignature,
                                                        std::string_view shaderPath) {
            const auto bytecode = gpu::LoadShader(shaderPath);
            if (!bytecode) {
                Log(Channel::Sim, Level::Error, "{}", bytecode.error());
                return nullptr;
            }

            return gpu::CreateComputePipeline(device, rootSignature, *bytecode);
        }

        // 単位の定数(b0): 刻みの下位・上位・引数
        void SetUnitConstants(ID3D12GraphicsCommandList10* list, uint64_t tick, uint32_t argument) {
            const std::array<uint32_t, PROBE_ROOT_CONSTANT_COUNT> constants = {
                static_cast<uint32_t>(tick), static_cast<uint32_t>(tick >> 32), argument};
            list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), PROBE_ROOT_CONSTANT_COUNT,
                                               constants.data(), 0);
        }

    }  // namespace

    ProbeCommand MakePokeCommand(uint64_t targetTick, uint32_t sequence, uint32_t x, uint32_t y, uint32_t z) {
        ProbeCommand command{.targetTick = targetTick,
                             .sequence = sequence,
                             .type = static_cast<uint16_t>(PROBE_COMMAND_TYPE_POKE),
                             .size = 12};

        command.payload[0] = x;
        command.payload[1] = y;
        command.payload[2] = z;

        return command;
    }

    // --- 作る ---

    std::expected<ProbeSim, std::string> ProbeSim::Create(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE listType,
                                                          const ProbeSimOptions& options) {
        if (options.busyPieces == 0 || options.busyPieces > PROBE_MAX_BUSY_PIECES ||
            options.busyIterations > PROBE_BUSY_ITERATIONS_LIMIT) {
            return std::unexpected(std::format("重さの試験の値が範囲外: 繰り返し {} 分ける数 {}",
                                               options.busyIterations, options.busyPieces));
        }

        auto events = gpu::ReadbackRing::Create(device, PROBE_EVENT_BYTES, PROBE_EVENT_HEADER_BYTES, FRAME_SLOT_COUNT,
                                                L"ProbeSim.events");
        if (!events)
            return std::unexpected(events.error());

        auto debugRing = gpu::DebugRing::Create(device, FRAME_SLOT_COUNT);
        if (!debugRing)
            return std::unexpected(debugRing.error());

        auto graphStats = gpu::WorkGraphStats::Create(device, MakeConductStatsLayout(), FRAME_SLOT_COUNT);
        if (!graphStats)
            return std::unexpected(graphStats.error());

        ProbeSim sim(options, std::move(*events), std::move(*debugRing), std::move(*graphStats));
        if (!sim.CreatePipelines(device))
            return std::unexpected("仮の刻みのパイプラインを作れない");

        if (!sim.CreateBuffers(device))
            return std::unexpected("仮の刻みのバッファを作れない");

        if (!sim.CreateFrameSlots(device, listType))
            return std::unexpected("仮の刻みのフレームの枠を作れない");

        return sim;
    }

    bool ProbeSim::CreatePipelines(ID3D12Device5* device) {
        m_rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!m_rootSignature)
            return false;

        ID3D12RootSignature* root = m_rootSignature.Get();
        m_enqueuePipeline = LoadComputePipeline(device, root, "sim/probe_tick_enqueue.cso");
        m_applyPipeline = LoadComputePipeline(device, root, "sim/probe_tick_apply.cso");
        m_busyPipeline = LoadComputePipeline(device, root, "sim/probe_tick_busy.cso");
        m_hashCellsPipeline = LoadComputePipeline(device, root, "sim/probe_tick_hash_cells.cso");
        m_flushEventsPipeline = LoadComputePipeline(device, root, "sim/probe_tick_flush_events.cso");
        m_extractPipeline = LoadComputePipeline(device, root, "sim/probe_tick_extract.cso");

        return m_enqueuePipeline && m_applyPipeline && m_busyPipeline && m_hashCellsPipeline && m_flushEventsPipeline &&
               m_extractPipeline && CreateConductGraph(device);
    }

    // 伝導の Work Graph(WakeBlocks → ConductBlock)。compute と同じルート署名をグローバルのルート署名にする
    bool ProbeSim::CreateConductGraph(ID3D12Device5* device) {
        const auto library = gpu::LoadShader("sim/probe_conduct.cso");
        if (!library) {
            Log(Channel::Sim, Level::Error, "{}", library.error());
            return false;
        }

        auto graph = gpu::WorkGraph::Create(device, m_rootSignature.Get(), *library, L"ProbeConduct");
        if (!graph) {
            Log(Channel::Sim, Level::Error, "{}", graph.error());
            return false;
        }

        m_conductGraph = std::make_unique<gpu::WorkGraph>(std::move(*graph));
        m_conductEntrypoint = m_conductGraph->EntrypointIndex(L"WakeBlocks");

        return m_conductEntrypoint != UINT32_MAX;
    }

    bool ProbeSim::CreateBuffers(ID3D12Device5* device) {
        m_world = gpu::CreateBuffer(device, uint64_t{CELL_BYTES} * 2, gpu::BufferKind::UnorderedAccess);
        m_busySink = gpu::CreateBuffer(device, SLICE_BYTES, gpu::BufferKind::UnorderedAccess);
        m_hashes = gpu::CreateBuffer(device, PROBE_HASH_BYTES, gpu::BufferKind::UnorderedAccess);
        m_commandQueue = gpu::CreateBuffer(device, PROBE_COMMAND_QUEUE_BYTES, gpu::BufferKind::UnorderedAccess);
        m_tickEvents = gpu::CreateBuffer(device, PROBE_TICK_EVENT_BYTES, gpu::BufferKind::UnorderedAccess);
        m_blockSchedule = gpu::CreateBuffer(device, PROBE_SCHEDULE_BYTES, gpu::BufferKind::UnorderedAccess);
        if (!m_world || !m_busySink || !m_hashes || !m_commandQueue || !m_tickEvents || !m_blockSchedule)
            return false;

        m_blockSchedule->SetName(L"ProbeSim.blockSchedule");  // 作った時は 0(まだ予定していない)
        // 活性の一覧: 作った時は 0(空。見出しは刻みの適用の単位が毎刻み書く)
        for (uint32_t parity = 0; parity < 2; ++parity) {
            m_activeLists[parity] = gpu::CreateBuffer(device, PROBE_ACTIVE_LIST_BYTES,
                                                      gpu::BufferKind::UnorderedAccess);
            if (!m_activeLists[parity])
                return false;

            m_activeLists[parity]->SetName(std::format(L"ProbeSim.activeList{}", parity).c_str());
        }

        m_world->SetName(L"ProbeSim.world");
        m_busySink->SetName(L"ProbeSim.busySink");
        m_hashes->SetName(L"ProbeSim.hashes");
        m_commandQueue->SetName(L"ProbeSim.commandQueue");  // 作った時は 0(末尾 = 先頭 = 0 の空のキュー)
        m_tickEvents->SetName(L"ProbeSim.tickEvents");

        for (uint32_t index = 0; index < PROBE_EXTRACTION_COUNT; ++index) {
            m_extractions[index] = gpu::CreateBuffer(device, EXTRACTION_BYTES, gpu::BufferKind::UnorderedAccess);
            if (!m_extractions[index])
                return false;

            m_extractions[index]->SetName(std::format(L"ProbeSim.extraction{}", index).c_str());
        }

        const D3D12_QUERY_HEAP_DESC queryDesc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
                                              .Count = FRAME_SLOT_COUNT * TIMESTAMPS_PER_SLOT};

        return SUCCEEDED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_timestamps)));
    }

    // フレームの枠ごとに: リスト(毎フレーム記録し直す)・入力のアップロード・読み戻し
    bool ProbeSim::CreateFrameSlots(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE listType) {
        for (FrameSlot& frame : m_slots) {
            if (FAILED(device->CreateCommandAllocator(listType, IID_PPV_ARGS(&frame.allocator))) ||
                FAILED(
                    device->CreateCommandList1(0, listType, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&frame.list)))) {
                return false;
            }

            frame.input = gpu::CreateBuffer(device, PROBE_INPUT_BYTES, gpu::BufferKind::Upload);
            frame.timestampReadback = gpu::CreateBuffer(device, uint64_t{TIMESTAMPS_PER_SLOT} * 8,
                                                        gpu::BufferKind::Readback);
            frame.hashReadback = gpu::CreateBuffer(device, PROBE_HASH_BYTES, gpu::BufferKind::Readback);
            if (!frame.input || !frame.timestampReadback || !frame.hashReadback)
                return false;

            void* mapped = nullptr;
            const D3D12_RANGE noRead{};
            if (FAILED(frame.input->Map(0, &noRead, &mapped)))
                return false;

            frame.mappedInput = static_cast<std::byte*>(mapped);
        }

        return true;
    }

    // --- フレームごとに記録する ---

    bool ProbeSim::ValidateInput(uint32_t slot, const ProbeFrameInput& input) const {
        const bool valid = slot < FRAME_SLOT_COUNT && input.firstUnit < UnitsPerTick() &&
                           input.unitCount <= MAX_UNITS_PER_FRAME && input.commands.size() <= PROBE_MAX_COMMANDS &&
                           input.extractionTarget < PROBE_EXTRACTION_COUNT;

        if (!valid) {
            Log(Channel::Sim, Level::Error, "フレームの入力が範囲外: slot {} 単位 {}+{} コマンド {} 抽出 {}", slot,
                input.firstUnit, input.unitCount, input.commands.size(), input.extractionTarget);
            return false;
        }

        return ValidateCommands(input);
    }

    // コマンドの約束(ファイルの先頭): キューの空き・並び・適用に間に合う刻み。破ると GPU で捨てられるか、キューが壊れる
    bool ProbeSim::ValidateCommands(const ProbeFrameInput& input) const {
        if (input.commands.size() > FreeCommandSlots()) {
            Log(Channel::Sim, Level::Error, "コマンドキューの空きが足りない: 足す {} 空き {}", input.commands.size(),
                FreeCommandSlots());
            return false;
        }

        const uint64_t nextApplyTick = NextApplyTick(input.firstTick, input.firstUnit);
        const ProbeCommand* previous = m_hasEnqueued ? &m_lastEnqueued : nullptr;
        for (const ProbeCommand& command : input.commands) {
            if (command.targetTick < nextApplyTick || (previous != nullptr && !CommandPrecedes(*previous, command))) {
                Log(Channel::Sim, Level::Error,
                    "コマンドの刻みか並びが不正: 刻み {} 番号 {}(適用に間に合う最初の刻み {}、前のコマンド {} / {})",
                    command.targetTick, command.sequence, nextApplyTick, previous != nullptr ? previous->targetTick : 0,
                    previous != nullptr ? previous->sequence : 0);
                return false;
            }

            previous = &command;
        }

        return true;
    }

    // 見出し(probe_sim.hlsli の PROBE_HEADER_*)とコマンド
    void ProbeSim::WriteInput(FrameSlot& frame, const ProbeFrameInput& input) const {
        const auto commandCount = static_cast<uint32_t>(input.commands.size());

        // 重さは 1 個あたりの回数にする(分けても 1 刻みの合計がほぼ同じになるように)
        const uint32_t busyPerPiece = m_options.busyIterations == 0
                                          ? 0
                                          : std::max(1u, m_options.busyIterations / m_options.busyPieces);

        const D3D12_GPU_VIRTUAL_ADDRESS list0 = m_activeLists[0]->GetGPUVirtualAddress();
        const D3D12_GPU_VIRTUAL_ADDRESS list1 = m_activeLists[1]->GetGPUVirtualAddress();

        const std::array<uint32_t, 8> header = {commandCount,
                                                busyPerPiece,
                                                m_commandTail,
                                                m_conductEntrypoint,
                                                static_cast<uint32_t>(list0),
                                                static_cast<uint32_t>(list0 >> 32),
                                                static_cast<uint32_t>(list1),
                                                static_cast<uint32_t>(list1 >> 32)};

        WriteAt(frame.mappedInput, PROBE_INPUT_HEADER_OFFSET, header);

        if (commandCount > 0) {
            std::memcpy(frame.mappedInput + PROBE_INPUT_COMMANDS_OFFSET, input.commands.data(),
                        input.commands.size_bytes());
        }
    }

    ID3D12CommandList* ProbeSim::RecordFrame(uint32_t slot, const ProbeFrameInput& input) {
        if (!ValidateInput(slot, input))
            return nullptr;

        FrameSlot& frame = m_slots[slot];
        WriteInput(frame, input);
        if (FAILED(frame.allocator->Reset()) || FAILED(frame.list->Reset(frame.allocator.Get(), nullptr))) {
            Log(Channel::Sim, Level::Error, "フレームのリストを記録し直せない(slot {})", slot);
            return nullptr;
        }

        ID3D12GraphicsCommandList10* list = frame.list.Get();
        const uint32_t firstQuery = slot * TIMESTAMPS_PER_SLOT;
        list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery);

        BindRootArguments(list, frame.input.Get());
        m_events.RecordBegin(list);
        m_debugRing.RecordBegin(list);
        m_graphStats.RecordBegin(list);
        const D3D12_RESOURCE_BARRIER hashesToUav = gpu::Transition(m_hashes.Get(), D3D12_RESOURCE_STATE_COMMON,
                                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &hashesToUav);
        // 活性の一覧はフレームの中では UAV(伝導の間だけ入力の組を GPU の入力の状態に)。フレームの終わりに COMMON へ戻す
        RecordActiveListStates(list, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        RecordEnqueue(list, static_cast<uint32_t>(input.commands.size()));

        // --- 単位を順に。刻みの終わりをまたいだら次の刻みへ(単位ごとに終わりのタイムスタンプ)---
        uint64_t tick = input.firstTick;
        uint32_t unit = input.firstUnit;
        bool hasHash = false;

        for (uint32_t index = 0; index < input.unitCount; ++index) {
            RecordUnit(list, frame.input.Get(), tick, unit);
            list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery + 1 + index);
            hasHash = hasHash || unit == HashUnit();

            if (++unit == UnitsPerTick()) {
                unit = 0;
                ++tick;
            }
        }

        // --- 刻みの境界の状態 S(tick) を抽出へ(刻みの途中で終わっても、途中の刻みは別の世代に書いているので S(tick) は揃っている)---
        if (input.extract)
            RecordExtract(list, tick, input.extractionTarget);

        RecordActiveListStates(list, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        RecordReadbacks(list, slot, hasHash);
        const uint32_t lastQuery = firstQuery + 1 + input.unitCount;
        list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, lastQuery);
        list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery, input.unitCount + 2,
                               frame.timestampReadback.Get(), 0);
        if (FAILED(list->Close()))
            return nullptr;

        frame.firstTick = input.firstTick;
        frame.firstUnit = input.firstUnit;
        frame.unitCount = input.unitCount;
        TrackCommands(input.commands, NextApplyTick(tick, unit));

        return list;
    }

    // 新しいコマンドを GPU のキューの末尾へ(フレームのリストの先頭。単位より前)
    void ProbeSim::RecordEnqueue(ID3D12GraphicsCommandList10* list, uint32_t commandCount) const {
        if (commandCount == 0)
            return;

        const D3D12_RESOURCE_BARRIER allUavs = gpu::UavBarrier(nullptr);
        SetUnitConstants(list, 0, 0);
        list->SetPipelineState(m_enqueuePipeline.Get());
        list->Dispatch((commandCount + PROBE_LINEAR_GROUP_SIZE - 1) / PROBE_LINEAR_GROUP_SIZE, 1, 1);
        list->ResourceBarrier(1, &allUavs);
    }

    // CPU 側の控え: 足したコマンドを数え、このフレームで適用の単位を記録した刻み(nextApplyTick より前)の分を取り出し済みにする
    void ProbeSim::TrackCommands(std::span<const ProbeCommand> commands, uint64_t nextApplyTick) {
        for (const ProbeCommand& command : commands) {
            if (m_queuedTicks.empty() || m_queuedTicks.back().targetTick != command.targetTick)
                m_queuedTicks.push_back({.targetTick = command.targetTick});

            ++m_queuedTicks.back().count;
        }

        m_queuedCommandCount += static_cast<uint32_t>(commands.size());
        m_commandTail += static_cast<uint32_t>(commands.size());
        if (!commands.empty()) {
            m_hasEnqueued = true;
            m_lastEnqueued = commands.back();
        }

        const auto applied = rng::find_if(m_queuedTicks,
                                          [&](const QueuedTick& queued) { return queued.targetTick >= nextApplyTick; });
        for (auto queued = m_queuedTicks.begin(); queued != applied; ++queued)
            m_queuedCommandCount -= queued->count;

        m_queuedTicks.erase(m_queuedTicks.begin(), applied);
    }

    void ProbeSim::BindRootArguments(ID3D12GraphicsCommandList10* list, ID3D12Resource* input) const {
        list->SetComputeRootSignature(m_rootSignature.Get());
        BindRootViews(list, input);
    }

    // ルートの引数: ROOT_LAYOUT の順(u0〜u11・デバッグのリング・Work Graphs のカウンタ・t0 フレームの入力)
    void ProbeSim::BindRootViews(ID3D12GraphicsCommandList10* list, ID3D12Resource* input) const {
        list->SetComputeRootUnorderedAccessView(UAV_WORLD, m_world->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_EVENTS, m_events.GpuAddress());
        list->SetComputeRootUnorderedAccessView(UAV_EXTRACTION0, m_extractions[0]->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_EXTRACTION1, m_extractions[1]->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_EXTRACTION2, m_extractions[2]->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_BUSY_SINK, m_busySink->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_HASHES, m_hashes->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_COMMAND_QUEUE, m_commandQueue->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_TICK_EVENTS, m_tickEvents->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_ACTIVE_LIST0, m_activeLists[0]->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_ACTIVE_LIST1, m_activeLists[1]->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_BLOCK_SCHEDULE, m_blockSchedule->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), m_debugRing.GpuAddress());
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.GraphStatsIndex(), m_graphStats.GpuAddress());
        list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(SRV_INPUT), input->GetGPUVirtualAddress());
    }

    // 1 つの単位(probe_sim.hlsli の単位の表)。最後に UAV バリアで、次の単位が結果を読めるようにする
    void ProbeSim::RecordUnit(ID3D12GraphicsCommandList10* list, ID3D12Resource* input, uint64_t tick, uint32_t unit) {
        const D3D12_RESOURCE_BARRIER allUavs = gpu::UavBarrier(nullptr);
        SetUnitConstants(list, tick, 0);
        if (unit == PROBE_UNIT_APPLY) {
            // コマンドの適用は 1 スレッドがキューの先頭から番号順に(probe_tick.hlsl)。刻みの一覧と表の欄の用意も
            list->SetPipelineState(m_applyPipeline.Get());
            list->Dispatch(1, 1, 1);
        } else if (unit == PROBE_UNIT_CONDUCT)
            RecordConduct(list, input, tick);
        else if (unit == HashUnit()) {
            // 表の欄は適用の単位が用意してある(刻み・0)
            list->SetPipelineState(m_hashCellsPipeline.Get());
            list->Dispatch(LINEAR_CELL_GROUPS, 1, 1);
            // 刻みのイベントを並べてリングへ(ハッシュとは別のバッファなので間のバリアは要らない)
            list->SetPipelineState(m_flushEventsPipeline.Get());
            list->Dispatch(1, 1, 1);
        } else {
            list->SetPipelineState(m_busyPipeline.Get());
            list->Dispatch(BUSY_GROUPS, BUSY_GROUPS, 1);
        }

        list->ResourceBarrier(1, &allUavs);
    }

    // 伝導: 刻み t の活性の一覧を GPU の入力にして DispatchGraph(何ブロック計算するかは GPU だけが知っている)。
    // 一覧は GPU の入力の間だけ読む状態(仕様: NON_PIXEL_SHADER_RESOURCE か COMMON)。グラフは次の刻みの一覧(もう一方の組)に UAV で書く
    void ProbeSim::RecordConduct(ID3D12GraphicsCommandList10* list, ID3D12Resource* input, uint64_t tick) {
        ID3D12Resource* activeList = m_activeLists[tick & 1].Get();
        const D3D12_RESOURCE_BARRIER toInput = gpu::Transition(activeList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &toInput);

        // 裏のメモリの初期化は最初の 1 回だけ(リストは記録した順に実行される)。グラフを設定してからルートの引数を結び直す
        m_conductGraph->SetProgram(list, !m_conductInitialized);
        m_conductInitialized = true;
        BindRootViews(list, input);
        SetUnitConstants(list, tick, 0);
        gpu::WorkGraph::DispatchFromGpu(list, activeList->GetGPUVirtualAddress());

        const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(activeList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);
    }

    void ProbeSim::RecordActiveListStates(ID3D12GraphicsCommandList10* list, D3D12_RESOURCE_STATES before,
                                          D3D12_RESOURCE_STATES after) const {
        const std::array<D3D12_RESOURCE_BARRIER, 2> barriers = {gpu::Transition(m_activeLists[0].Get(), before, after),
                                                                gpu::Transition(m_activeLists[1].Get(), before, after)};
        list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    }

    void ProbeSim::RecordExtract(ID3D12GraphicsCommandList10* list, uint64_t tick, uint32_t target) const {
        SetUnitConstants(list, tick, target);
        list->SetPipelineState(m_extractPipeline.Get());
        list->Dispatch(LINEAR_CELL_GROUPS, 1, 1);
    }

    // イベント・デバッグの出力・ノードのカウンタ・(ハッシュの単位があれば)ハッシュの表を、slot の読み戻しのバッファへ
    void ProbeSim::RecordReadbacks(ID3D12GraphicsCommandList10* list, uint32_t slot, bool hasHash) const {
        m_events.RecordReadbackAndReset(list, slot);
        m_debugRing.RecordReadbackAndReset(list, slot);
        m_graphStats.RecordReadbackAndReset(list, slot);

        if (hasHash) {
            gpu::RecordCopyToReadback(list, m_hashes.Get(), m_slots[slot].hashReadback.Get());
            const D3D12_RESOURCE_BARRIER toCommon = gpu::Transition(m_hashes.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                                    D3D12_RESOURCE_STATE_COMMON);
            list->ResourceBarrier(1, &toCommon);

            return;
        }

        const D3D12_RESOURCE_BARRIER toCommon = gpu::Transition(m_hashes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                D3D12_RESOURCE_STATE_COMMON);
        list->ResourceBarrier(1, &toCommon);
    }

    // --- 読み戻し ---

    ProbeFrameReadback ProbeSim::ReadFrame(uint32_t slot) {
        ProbeFrameReadback result;
        const FrameSlot& frame = m_slots[slot];

        // イベント: 見出しを先に読み、書かれた分だけを読む(並びは GPU が刻みの最後に並べた (刻み, 種類, 場所) の順)
        EventRingHeader header;
        const bool headerRead = m_events.Read(slot, std::as_writable_bytes(std::span(&header, 1)));
        const uint32_t stored = std::min(header.requested, PROBE_EVENT_CAPACITY);
        std::vector<uint32_t> words(PROBE_EVENT_HEADER_BYTES / 4 + size_t{stored} * PROBE_EVENT_WORDS);

        if (headerRead && (stored == 0 || m_events.Read(slot, std::as_writable_bytes(std::span(words))))) {
            result.droppedEventCount = header.requested - stored + header.tickDropped;
            result.events.reserve(stored);
            for (uint32_t index = 0; index < stored; ++index) {
                const uint32_t* record = words.data() + PROBE_EVENT_HEADER_BYTES / 4 +
                                         size_t{index} * PROBE_EVENT_WORDS;
                result.events.push_back(
                    {.tick = record[0] | (uint64_t{record[1]} << 32), .type = record[2], .place = record[3]});
            }
        }

        // タイムスタンプ: [0] 始め、[1 + i] 単位 i の終わり、[1 + 数] 全体の終わり
        std::vector<uint64_t> timestamps(size_t{frame.unitCount} + 2);
        if (gpu::ReadBuffer(frame.timestampReadback.Get(), std::as_writable_bytes(std::span(timestamps)))) {
            result.gpuBeginTimestamp = timestamps.front();
            result.gpuEndTimestamp = timestamps.back();
            result.unitGpuTicks.reserve(frame.unitCount);

            for (uint32_t index = 0; index < frame.unitCount; ++index) {
                const uint64_t begin = timestamps[index];
                const uint64_t end = timestamps[size_t{index} + 1];
                result.unitGpuTicks.push_back(end >= begin ? end - begin : 0);
            }
        }

        result.firstUnit = frame.firstUnit;
        result.hashes = ReadHashes(frame);
        result.debugAssertCount = m_debugRing.Drain(MAX_LOGGED_DEBUG_MESSAGES, slot).assertCount;

        // ノードのカウンタ: 要約は Trace、上限に当たった・近づいたものは Warning(gpu/work_graph_stats.h)
        auto graphStats = m_graphStats.Read(slot);
        if (graphStats) {
            result.graphFindingCount = static_cast<uint32_t>(m_graphStats.Report(*graphStats).size());
            result.graphStats = std::move(*graphStats);
        } else
            Log(Channel::WorkGraph, Level::Warning, "{}", graphStats.error());

        return result;
    }

    // このフレームで終えた刻み t ごとに、表の (t + 1) 番目の S(t + 1) のハッシュ
    std::vector<ProbeTickHash> ProbeSim::ReadHashes(const FrameSlot& frame) const {
        std::vector<ProbeTickHash> hashes;
        const uint64_t unitsBefore = frame.firstUnit;
        const uint64_t unitsTotal = unitsBefore + frame.unitCount;
        // firstTick から数えて終えた刻み(途中から始めた刻みを含む)
        const uint64_t completedTicks = unitsTotal / UnitsPerTick();
        if (completedTicks == 0)
            return hashes;

        std::vector<uint32_t> table(PROBE_HASH_BYTES / 4);
        if (!gpu::ReadBuffer(frame.hashReadback.Get(), std::as_writable_bytes(std::span(table))))
            return hashes;

        hashes.reserve(completedTicks);
        for (uint64_t offset = 1; offset <= completedTicks; ++offset) {
            const uint64_t stateTick = frame.firstTick + offset;
            const uint32_t* entry = table.data() + (stateTick % PROBE_HASH_CAPACITY) * (PROBE_HASH_ENTRY_BYTES / 4);
            const uint64_t storedTick = entry[0] | (uint64_t{entry[1]} << 32);

            if (storedTick != stateTick) {
                Log(Channel::Sim, Level::Error, "ハッシュの表の刻みが合わない: 期待 {} 実際 {}", stateTick, storedTick);
                continue;
            }

            hashes.push_back({.tick = stateTick,
                              .hash = entry[2] | (uint64_t{entry[3]} << 32),
                              .heat = entry[4] | (uint64_t{entry[5]} << 32),
                              .scheduledBlocks = entry[6]});
        }

        return hashes;
    }

    // --- CPU リファレンス ---

    namespace {

        // 1 セルの伝導(GPU の ConductBlock と同じ。格子の外の面は自分を渡す = 断熱)
        uint32_t ReferenceConductCell(const uint32_t* generation, uint32_t x, uint32_t y, uint32_t z) {
            const uint32_t self = generation[ProbeCellIndex(x, y, z)];
            const auto at = [&](bool inside, uint32_t nx, uint32_t ny, uint32_t nz) {
                return inside ? generation[ProbeCellIndex(nx, ny, nz)] : self;
            };

            constexpr uint32_t LAST = PROBE_GRID_SIZE - 1;

            return ProbeConductValue(self, at(x > 0, x - 1, y, z), at(x < LAST, x + 1, y, z), at(y > 0, x, y - 1, z),
                                     at(y < LAST, x, y + 1, z), at(z > 0, x, y, z - 1), at(z < LAST, x, y, z + 1));
        }

        // ブロック block と 6 面の隣(格子の中)に印を付ける
        void MarkWithNeighbors(std::vector<uint8_t>& scheduled, uint32_t block) {
            constexpr auto AXIS = static_cast<int32_t>(PROBE_BLOCKS_PER_AXIS);
            constexpr std::array<std::array<int32_t, 3>, 7> OFFSETS = {
                {{0, 0, 0}, {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}}};
            const auto x = static_cast<int32_t>(block % PROBE_BLOCKS_PER_AXIS);
            const auto y = static_cast<int32_t>((block / PROBE_BLOCKS_PER_AXIS) % PROBE_BLOCKS_PER_AXIS);
            const auto z = static_cast<int32_t>(block / (PROBE_BLOCKS_PER_AXIS * PROBE_BLOCKS_PER_AXIS));

            for (const auto& offset : OFFSETS) {
                const int32_t nx = x + offset[0];
                const int32_t ny = y + offset[1];
                const int32_t nz = z + offset[2];
                if (nx < 0 || ny < 0 || nz < 0 || nx >= AXIS || ny >= AXIS || nz >= AXIS)
                    continue;

                scheduled[ProbeBlockIndex(static_cast<uint32_t>(nx), static_cast<uint32_t>(ny),
                                          static_cast<uint32_t>(nz))] = 1;
            }
        }

        // seeds(ブロックごとの 0/1)のブロックと 6 面の隣の数(GPU の WakeBlocks が予定する数)
        uint32_t CountScheduledBlocks(std::span<const uint8_t> seeds) {
            std::vector<uint8_t> scheduled(PROBE_BLOCK_COUNT, 0);
            for (uint32_t block = 0; block < PROBE_BLOCK_COUNT; ++block) {
                if (seeds[block] != 0)
                    MarkWithNeighbors(scheduled, block);
            }

            return static_cast<uint32_t>(rng::count(scheduled, uint8_t{1}));
        }

        // 刻み tick のつつきを current に適用し、つついたブロックに印を付ける(GPU と同じく並びの順に)
        void ApplyPokes(uint32_t* current, uint64_t tick, std::span<const ProbeCommand> commands,
                        std::vector<uint8_t>& seeds) {
            for (const ProbeCommand& command : commands) {
                if (command.targetTick != tick || command.type != PROBE_COMMAND_TYPE_POKE)
                    continue;

                const uint32_t x = command.payload[0];
                const uint32_t y = command.payload[1];
                const uint32_t z = command.payload[2];
                if (x >= PROBE_GRID_SIZE || y >= PROBE_GRID_SIZE || z >= PROBE_GRID_SIZE)
                    continue;

                uint32_t& cell = current[ProbeCellIndex(x, y, z)];
                cell = ProbeAddHeat(cell, PROBE_POKE_AMOUNT);
                seeds[ProbeBlockOfCell(x, y, z)] = 1;
            }
        }

        // 全部のセルの伝導(gather)。値が変わったブロックに印を付ける
        void ConductAllCells(const uint32_t* current, uint32_t* next, std::vector<uint8_t>& changedBlocks) {
            rng::fill(changedBlocks, uint8_t{0});
            for (uint32_t index = 0; index < PROBE_CELL_COUNT; ++index) {
                const uint32_t x = index % PROBE_GRID_SIZE;
                const uint32_t y = (index / PROBE_GRID_SIZE) % PROBE_GRID_SIZE;
                const uint32_t z = index / PROBE_SLICE_CELL_COUNT;
                next[index] = ReferenceConductCell(current, x, y, z);
                if (next[index] != current[index])
                    changedBlocks[ProbeBlockOfCell(x, y, z)] = 1;
            }
        }

    }  // namespace

    ProbeReference::ProbeReference()
        : m_cells(size_t{PROBE_CELL_COUNT} * 2, 0), m_changedBlocks(PROBE_BLOCK_COUNT, 0) {}

    void ProbeReference::Advance(uint64_t tick, std::span<const ProbeCommand> commands) {
        uint32_t* current = m_cells.data() + static_cast<size_t>(tick & 1) * PROBE_CELL_COUNT;
        uint32_t* next = m_cells.data() + static_cast<size_t>((tick + 1) & 1) * PROBE_CELL_COUNT;

        // (1) コマンドの適用。つついたブロックは、前の刻みで変わったブロックと同じく予定の種になる
        std::vector<uint8_t> seeds = m_changedBlocks;
        ApplyPokes(current, tick, commands, seeds);
        m_scheduledBlocks = CountScheduledBlocks(seeds);

        // (2) 伝導(全部のセル)と、値が変わったブロックの記録(次の刻みの予定の種)
        ConductAllCells(current, next, m_changedBlocks);
    }

    std::span<const uint32_t> ProbeReference::State(uint64_t tick) const {
        return std::span(m_cells).subspan(static_cast<size_t>(tick & 1) * PROBE_CELL_COUNT, PROBE_CELL_COUNT);
    }

    uint64_t ProbeStateHash(std::span<const uint32_t> cells) {
        uint64_t hash = 0;
        for (uint32_t index = 0; index < cells.size(); ++index)
            hash += ProbeCellHash(index, cells[index]);

        return hash;
    }

    uint64_t ProbeHeatSum(std::span<const uint32_t> cells) {
        uint64_t heat = 0;
        for (const uint32_t value : cells)
            heat += value;

        return heat;
    }

}  // namespace bicameral::sim
