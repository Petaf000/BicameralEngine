// probe_sim.cpp — 仮の刻みを単位の列として GPU で走らせる(フレームの枠ごとのリストに毎フレーム記録)と、その CPU リファレンス。
// 使い方とデータの流れは probe_sim.h、規則は shaders/common/probe_sim.hlsli。
#include "sim/probe_sim.h"

#include "core/log.h"
#include "gpu/resources.h"

using Microsoft::WRL::ComPtr;

namespace bicameral::sim {
    namespace {

        // ルート署名: u0 世界・u1 イベントのリング・u2/u3/u5 抽出・u4 重さの捨て場・u6 ハッシュの表・u7 コマンドキュー・
        //            u8 刻みのイベントの一時置き場 → b0 単位の定数 → デバッグのリング → t0 フレームの入力
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
            .uavCount = 9, .rootConstantCount = PROBE_ROOT_CONSTANT_COUNT, .debugRing = true, .srvCount = 1};
        constexpr uint32_t UAV_WORLD = 0;
        constexpr uint32_t UAV_EVENTS = 1;
        constexpr uint32_t UAV_EXTRACTION0 = 2;
        constexpr uint32_t UAV_EXTRACTION1 = 3;
        constexpr uint32_t UAV_BUSY_SINK = 4;
        constexpr uint32_t UAV_EXTRACTION2 = 5;
        constexpr uint32_t UAV_HASHES = 6;
        constexpr uint32_t UAV_COMMAND_QUEUE = 7;
        constexpr uint32_t UAV_TICK_EVENTS = 8;
        constexpr uint32_t SRV_INPUT = 0;

        constexpr uint32_t TIMESTAMPS_PER_SLOT = ProbeSim::MAX_UNITS_PER_FRAME + 2;  // 始め・単位ごと・終わり
        constexpr uint32_t CELL_BYTES = PROBE_CELL_COUNT * 4;
        constexpr uint32_t DIFFUSE_GROUPS = PROBE_GRID_SIZE / PROBE_GROUP_SIZE;
        constexpr uint32_t LINEAR_CELL_GROUPS = PROBE_CELL_COUNT / PROBE_LINEAR_GROUP_SIZE;
        constexpr uint32_t MAX_LOGGED_DEBUG_MESSAGES = 8;

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

    ProbeCommand MakePokeCommand(uint64_t targetTick, uint32_t sequence, uint32_t x, uint32_t y) {
        ProbeCommand command{.targetTick = targetTick,
                             .sequence = sequence,
                             .type = static_cast<uint16_t>(PROBE_COMMAND_TYPE_POKE),
                             .size = 8};
        command.payload[0] = x;
        command.payload[1] = y;
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
        if (!events) return std::unexpected(events.error());
        auto debugRing = gpu::DebugRing::Create(device, FRAME_SLOT_COUNT);
        if (!debugRing) return std::unexpected(debugRing.error());

        ProbeSim sim(options, std::move(*events), std::move(*debugRing));
        if (!sim.CreatePipelines(device)) return std::unexpected("仮の刻みのパイプラインを作れない");
        if (!sim.CreateBuffers(device)) return std::unexpected("仮の刻みのバッファを作れない");
        if (!sim.CreateFrameSlots(device, listType)) return std::unexpected("仮の刻みのフレームの枠を作れない");
        return sim;
    }

    bool ProbeSim::CreatePipelines(ID3D12Device5* device) {
        m_rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!m_rootSignature) return false;
        ID3D12RootSignature* root = m_rootSignature.Get();
        m_enqueuePipeline = LoadComputePipeline(device, root, "sim/probe_tick_enqueue.cso");
        m_applyPipeline = LoadComputePipeline(device, root, "sim/probe_tick_apply.cso");
        m_diffusePipeline = LoadComputePipeline(device, root, "sim/probe_tick_diffuse.cso");
        m_busyPipeline = LoadComputePipeline(device, root, "sim/probe_tick_busy.cso");
        m_hashBeginPipeline = LoadComputePipeline(device, root, "sim/probe_tick_hash_begin.cso");
        m_hashCellsPipeline = LoadComputePipeline(device, root, "sim/probe_tick_hash_cells.cso");
        m_flushEventsPipeline = LoadComputePipeline(device, root, "sim/probe_tick_flush_events.cso");
        m_extractPipeline = LoadComputePipeline(device, root, "sim/probe_tick_extract.cso");
        return m_enqueuePipeline && m_applyPipeline && m_diffusePipeline && m_busyPipeline && m_hashBeginPipeline &&
               m_hashCellsPipeline && m_flushEventsPipeline && m_extractPipeline;
    }

    bool ProbeSim::CreateBuffers(ID3D12Device5* device) {
        m_world = gpu::CreateBuffer(device, uint64_t{CELL_BYTES} * 2, gpu::BufferKind::UnorderedAccess);
        m_busySink = gpu::CreateBuffer(device, CELL_BYTES, gpu::BufferKind::UnorderedAccess);
        m_hashes = gpu::CreateBuffer(device, PROBE_HASH_BYTES, gpu::BufferKind::UnorderedAccess);
        m_commandQueue = gpu::CreateBuffer(device, PROBE_COMMAND_QUEUE_BYTES, gpu::BufferKind::UnorderedAccess);
        m_tickEvents = gpu::CreateBuffer(device, PROBE_TICK_EVENT_BYTES, gpu::BufferKind::UnorderedAccess);
        if (!m_world || !m_busySink || !m_hashes || !m_commandQueue || !m_tickEvents) return false;
        m_world->SetName(L"ProbeSim.world");
        m_busySink->SetName(L"ProbeSim.busySink");
        m_hashes->SetName(L"ProbeSim.hashes");
        m_commandQueue->SetName(L"ProbeSim.commandQueue");  // 作った時は 0(末尾 = 先頭 = 0 の空のキュー)
        m_tickEvents->SetName(L"ProbeSim.tickEvents");
        for (uint32_t index = 0; index < PROBE_EXTRACTION_COUNT; ++index) {
            m_extractions[index] = gpu::CreateBuffer(device, CELL_BYTES, gpu::BufferKind::UnorderedAccess);
            if (!m_extractions[index]) return false;
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
            frame.timestampReadback =
                gpu::CreateBuffer(device, uint64_t{TIMESTAMPS_PER_SLOT} * 8, gpu::BufferKind::Readback);
            frame.hashReadback = gpu::CreateBuffer(device, PROBE_HASH_BYTES, gpu::BufferKind::Readback);
            if (!frame.input || !frame.timestampReadback || !frame.hashReadback) return false;
            void* mapped = nullptr;
            const D3D12_RANGE noRead{};
            if (FAILED(frame.input->Map(0, &noRead, &mapped))) return false;
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
        const uint32_t busyPerPiece =
            m_options.busyIterations == 0 ? 0 : std::max(1u, m_options.busyIterations / m_options.busyPieces);
        const std::array<uint32_t, 3> header = {commandCount, busyPerPiece, m_commandTail};
        WriteAt(frame.mappedInput, PROBE_INPUT_HEADER_OFFSET, header);
        if (commandCount > 0) {
            std::memcpy(frame.mappedInput + PROBE_INPUT_COMMANDS_OFFSET, input.commands.data(),
                        input.commands.size_bytes());
        }
    }

    ID3D12CommandList* ProbeSim::RecordFrame(uint32_t slot, const ProbeFrameInput& input) {
        if (!ValidateInput(slot, input)) return nullptr;
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
        const D3D12_RESOURCE_BARRIER hashesToUav =
            gpu::Transition(m_hashes.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &hashesToUav);
        RecordEnqueue(list, static_cast<uint32_t>(input.commands.size()));

        // --- 単位を順に。刻みの終わりをまたいだら次の刻みへ(単位ごとに終わりのタイムスタンプ)---
        uint64_t tick = input.firstTick;
        uint32_t unit = input.firstUnit;
        bool hasHash = false;
        for (uint32_t index = 0; index < input.unitCount; ++index) {
            RecordUnit(list, tick, unit);
            list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery + 1 + index);
            hasHash = hasHash || unit == HashUnit();
            if (++unit == UnitsPerTick()) {
                unit = 0;
                ++tick;
            }
        }

        // --- 刻みの境界の状態 S(tick) を抽出へ(刻みの途中で終わっても、途中の刻みは別の世代に書いているので S(tick) は揃っている)---
        if (input.extract) RecordExtract(list, tick, input.extractionTarget);
        RecordReadbacks(list, slot, hasHash);
        const uint32_t lastQuery = firstQuery + 1 + input.unitCount;
        list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, lastQuery);
        list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery, input.unitCount + 2,
                               frame.timestampReadback.Get(), 0);
        if (FAILED(list->Close())) return nullptr;

        frame.firstTick = input.firstTick;
        frame.firstUnit = input.firstUnit;
        frame.unitCount = input.unitCount;
        TrackCommands(input.commands, NextApplyTick(tick, unit));
        return list;
    }

    // 新しいコマンドを GPU のキューの末尾へ(フレームのリストの先頭。単位より前)
    void ProbeSim::RecordEnqueue(ID3D12GraphicsCommandList10* list, uint32_t commandCount) const {
        if (commandCount == 0) return;
        const D3D12_RESOURCE_BARRIER allUavs = gpu::UavBarrier(nullptr);
        SetUnitConstants(list, 0, 0);
        list->SetPipelineState(m_enqueuePipeline.Get());
        list->Dispatch((commandCount + PROBE_LINEAR_GROUP_SIZE - 1) / PROBE_LINEAR_GROUP_SIZE, 1, 1);
        list->ResourceBarrier(1, &allUavs);
    }

    // CPU 側の控え: 足したコマンドを数え、このフレームで適用の単位を記録した刻み(nextApplyTick より前)の分を取り出し済みにする
    void ProbeSim::TrackCommands(std::span<const ProbeCommand> commands, uint64_t nextApplyTick) {
        for (const ProbeCommand& command : commands) {
            if (m_queuedTicks.empty() || m_queuedTicks.back().targetTick != command.targetTick) {
                m_queuedTicks.push_back({.targetTick = command.targetTick});
            }
            ++m_queuedTicks.back().count;
        }
        m_queuedCommandCount += static_cast<uint32_t>(commands.size());
        m_commandTail += static_cast<uint32_t>(commands.size());
        if (!commands.empty()) {
            m_hasEnqueued = true;
            m_lastEnqueued = commands.back();
        }

        const auto applied = std::ranges::find_if(
            m_queuedTicks, [&](const QueuedTick& queued) { return queued.targetTick >= nextApplyTick; });
        for (auto queued = m_queuedTicks.begin(); queued != applied; ++queued) {
            m_queuedCommandCount -= queued->count;
        }
        m_queuedTicks.erase(m_queuedTicks.begin(), applied);
    }

    // ルートの引数: ROOT_LAYOUT の順(u0〜u8・デバッグのリング・t0 フレームの入力)
    void ProbeSim::BindRootArguments(ID3D12GraphicsCommandList10* list, ID3D12Resource* input) const {
        list->SetComputeRootSignature(m_rootSignature.Get());
        list->SetComputeRootUnorderedAccessView(UAV_WORLD, m_world->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_EVENTS, m_events.GpuAddress());
        list->SetComputeRootUnorderedAccessView(UAV_EXTRACTION0, m_extractions[0]->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_EXTRACTION1, m_extractions[1]->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_EXTRACTION2, m_extractions[2]->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_BUSY_SINK, m_busySink->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_HASHES, m_hashes->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_COMMAND_QUEUE, m_commandQueue->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_TICK_EVENTS, m_tickEvents->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), m_debugRing.GpuAddress());
        list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(SRV_INPUT), input->GetGPUVirtualAddress());
    }

    // 1 つの単位(probe_sim.hlsli の単位の表)。最後に UAV バリアで、次の単位が結果を読めるようにする
    void ProbeSim::RecordUnit(ID3D12GraphicsCommandList10* list, uint64_t tick, uint32_t unit) const {
        const D3D12_RESOURCE_BARRIER allUavs = gpu::UavBarrier(nullptr);
        SetUnitConstants(list, tick, 0);
        if (unit == PROBE_UNIT_APPLY) {
            // コマンドの適用は 1 スレッドがキューの先頭から番号順に(probe_tick.hlsl)
            list->SetPipelineState(m_applyPipeline.Get());
            list->Dispatch(1, 1, 1);
        } else if (unit == PROBE_UNIT_DIFFUSE) {
            list->SetPipelineState(m_diffusePipeline.Get());
            list->Dispatch(DIFFUSE_GROUPS, DIFFUSE_GROUPS, 1);
        } else if (unit == HashUnit()) {
            list->SetPipelineState(m_hashBeginPipeline.Get());
            list->Dispatch(1, 1, 1);
            list->ResourceBarrier(1, &allUavs);
            list->SetPipelineState(m_hashCellsPipeline.Get());
            list->Dispatch(LINEAR_CELL_GROUPS, 1, 1);
            // 刻みのイベントを並べてリングへ(ハッシュとは別のバッファなので間のバリアは要らない)
            list->SetPipelineState(m_flushEventsPipeline.Get());
            list->Dispatch(1, 1, 1);
        } else {
            list->SetPipelineState(m_busyPipeline.Get());
            list->Dispatch(DIFFUSE_GROUPS, DIFFUSE_GROUPS, 1);
        }
        list->ResourceBarrier(1, &allUavs);
    }

    void ProbeSim::RecordExtract(ID3D12GraphicsCommandList10* list, uint64_t tick, uint32_t target) const {
        SetUnitConstants(list, tick, target);
        list->SetPipelineState(m_extractPipeline.Get());
        list->Dispatch(LINEAR_CELL_GROUPS, 1, 1);
    }

    // イベント・デバッグの出力・(ハッシュの単位があれば)ハッシュの表を、slot の読み戻しのバッファへ
    void ProbeSim::RecordReadbacks(ID3D12GraphicsCommandList10* list, uint32_t slot, bool hasHash) const {
        m_events.RecordReadbackAndReset(list, slot);
        m_debugRing.RecordReadbackAndReset(list, slot);
        if (hasHash) {
            gpu::RecordCopyToReadback(list, m_hashes.Get(), m_slots[slot].hashReadback.Get());
            const D3D12_RESOURCE_BARRIER toCommon =
                gpu::Transition(m_hashes.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            list->ResourceBarrier(1, &toCommon);
            return;
        }
        const D3D12_RESOURCE_BARRIER toCommon =
            gpu::Transition(m_hashes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        list->ResourceBarrier(1, &toCommon);
    }

    // --- 読み戻し ---

    ProbeFrameReadback ProbeSim::ReadFrame(uint32_t slot) const {
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
                const uint32_t* record =
                    words.data() + PROBE_EVENT_HEADER_BYTES / 4 + size_t{index} * PROBE_EVENT_WORDS;
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
        return result;
    }

    // このフレームで終えた刻み t ごとに、表の (t + 1) 番目の S(t + 1) のハッシュ
    std::vector<ProbeTickHash> ProbeSim::ReadHashes(const FrameSlot& frame) const {
        std::vector<ProbeTickHash> hashes;
        const uint64_t unitsBefore = frame.firstUnit;
        const uint64_t unitsTotal = unitsBefore + frame.unitCount;
        const uint64_t completedTicks =
            unitsTotal / UnitsPerTick();  // firstTick から数えて終えた刻み(途中から始めた刻みを含む)
        if (completedTicks == 0) return hashes;

        std::vector<uint32_t> table(PROBE_HASH_BYTES / 4);
        if (!gpu::ReadBuffer(frame.hashReadback.Get(), std::as_writable_bytes(std::span(table)))) return hashes;
        hashes.reserve(completedTicks);
        for (uint64_t offset = 1; offset <= completedTicks; ++offset) {
            const uint64_t stateTick = frame.firstTick + offset;
            const uint32_t* entry = table.data() + (stateTick % PROBE_HASH_CAPACITY) * (PROBE_HASH_ENTRY_BYTES / 4);
            const uint64_t storedTick = entry[0] | (uint64_t{entry[1]} << 32);
            if (storedTick != stateTick) {
                Log(Channel::Sim, Level::Error, "ハッシュの表の刻みが合わない: 期待 {} 実際 {}", stateTick, storedTick);
                continue;
            }
            hashes.push_back({.tick = stateTick, .hash = entry[2] | (uint64_t{entry[3]} << 32)});
        }
        return hashes;
    }

    // --- CPU リファレンス ---

    ProbeReference::ProbeReference() : m_cells(size_t{PROBE_CELL_COUNT} * 2, 0) {}

    void ProbeReference::Advance(uint64_t tick, std::span<const ProbeCommand> commands) {
        const size_t current = static_cast<size_t>(tick & 1) * PROBE_CELL_COUNT;
        const size_t next = static_cast<size_t>((tick + 1) & 1) * PROBE_CELL_COUNT;

        // (1) コマンドの適用(max。GPU と同じく並びの順に。つつきは max なので順番に依存しないが、形は本物と同じにする)
        for (const ProbeCommand& command : commands) {
            if (command.targetTick != tick || command.type != PROBE_COMMAND_TYPE_POKE) continue;
            const uint32_t x = command.payload[0];
            const uint32_t y = command.payload[1];
            if (x >= PROBE_GRID_SIZE || y >= PROBE_GRID_SIZE) continue;
            uint32_t& cell = m_cells[current + ProbeCellIndex(x, y)];
            cell = std::max(cell, PROBE_POKE_AMOUNT);
        }

        // (2) 拡散(gather)
        for (uint32_t y = 0; y < PROBE_GRID_SIZE; ++y) {
            for (uint32_t x = 0; x < PROBE_GRID_SIZE; ++x) {
                const size_t index = current + ProbeCellIndex(x, y);
                const uint32_t left = x > 0 ? m_cells[index - 1] : 0;
                const uint32_t right = x + 1 < PROBE_GRID_SIZE ? m_cells[index + 1] : 0;
                const uint32_t up = y > 0 ? m_cells[index - PROBE_GRID_SIZE] : 0;
                const uint32_t down = y + 1 < PROBE_GRID_SIZE ? m_cells[index + PROBE_GRID_SIZE] : 0;
                m_cells[next + ProbeCellIndex(x, y)] = ProbeDiffuseValue(m_cells[index], left, right, up, down);
            }
        }
    }

    std::span<const uint32_t> ProbeReference::State(uint64_t tick) const {
        return std::span(m_cells).subspan(static_cast<size_t>(tick & 1) * PROBE_CELL_COUNT, PROBE_CELL_COUNT);
    }

    uint64_t ProbeStateHash(std::span<const uint32_t> cells) {
        uint64_t hash = 0;
        for (uint32_t index = 0; index < cells.size(); ++index) {
            hash += ProbeCellHash(index, cells[index]);
        }
        return hash;
    }

}  // namespace bicameral::sim
