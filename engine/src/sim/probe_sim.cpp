// probe_sim.cpp — T-0004 の仮の刻みを GPU で走らせる(記録済みのバッチのリスト)と、その CPU リファレンス。
// 使い方とデータの流れは probe_sim.h、規則は shaders/common/probe_sim.hlsli。
#include "sim/probe_sim.h"

#include "core/log.h"
#include "gpu/resources.h"

using Microsoft::WRL::ComPtr;

namespace bicameral::sim {
    namespace {

        // ルート署名: u0 世界・u1 イベント・u2/u3/u5 抽出・u4 重さの捨て場 → b0 刻みの枠 → デバッグのリング → t0 バッチの入力
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
            .uavCount = 6, .rootConstantCount = 1, .debugRing = true, .srvCount = 1};
        constexpr uint32_t UAV_WORLD = 0;
        constexpr uint32_t UAV_EVENTS = 1;
        constexpr uint32_t UAV_EXTRACTION0 = 2;
        constexpr uint32_t UAV_EXTRACTION1 = 3;
        constexpr uint32_t UAV_BUSY_SINK = 4;
        constexpr uint32_t UAV_EXTRACTION2 = 5;
        constexpr uint32_t SRV_BATCH = 0;

        constexpr uint32_t TIMESTAMPS_PER_SLOT = 2;
        constexpr uint32_t CELL_BYTES = PROBE_CELL_COUNT * 4;
        constexpr uint32_t DIFFUSE_GROUPS = PROBE_GRID_SIZE / PROBE_GROUP_SIZE;
        constexpr uint32_t EXTRACT_GROUPS = PROBE_CELL_COUNT / PROBE_COMMAND_GROUP_SIZE;
        constexpr uint32_t APPLY_GROUPS = PROBE_MAX_COMMANDS / PROBE_COMMAND_GROUP_SIZE;
        constexpr uint32_t MAX_LOGGED_DEBUG_MESSAGES = 8;

        template <typename T>
        void WriteAt(std::byte* base, uint32_t offset, const T& value) {
            std::memcpy(base + offset, &value, sizeof(T));
        }

        ComPtr<ID3D12PipelineState> LoadComputePipeline(ID3D12Device* device, ID3D12RootSignature* rootSignature,
                                                        std::string_view shaderPath) {
            const auto bytecode = gpu::LoadShader(shaderPath);
            if (!bytecode) {
                Log(Channel::Sim, Level::Error, "{}", bytecode.error());
                return nullptr;
            }
            return gpu::CreateComputePipeline(device, rootSignature, *bytecode);
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

    std::expected<ProbeSim, std::string> ProbeSim::Create(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE listType) {
        auto events = gpu::ReadbackRing::Create(device, PROBE_EVENT_BYTES, PROBE_EVENT_HEADER_BYTES, BATCH_SLOT_COUNT,
                                                L"ProbeSim.events");
        if (!events) return std::unexpected(events.error());
        auto debugRing = gpu::DebugRing::Create(device, BATCH_SLOT_COUNT);
        if (!debugRing) return std::unexpected(debugRing.error());

        ProbeSim sim(std::move(*events), std::move(*debugRing));
        if (!sim.CreatePipelines(device)) return std::unexpected("仮の刻みのパイプラインを作れない");
        if (!sim.CreateBuffers(device)) return std::unexpected("仮の刻みのバッファを作れない");
        if (!sim.RecordBatchLists(device, listType)) return std::unexpected("仮の刻みのリストを記録できない");
        return sim;
    }

    // バッチの枠ごと・刻みの数ごとにリストを記録する
    bool ProbeSim::RecordBatchLists(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE listType) {
        for (uint32_t slot = 0; slot < BATCH_SLOT_COUNT; ++slot) {
            if (FAILED(device->CreateCommandAllocator(listType, IID_PPV_ARGS(&m_slots[slot].allocator)))) return false;
            for (uint32_t tickCount = 1; tickCount <= PROBE_MAX_TICKS_PER_BATCH; ++tickCount) {
                if (!RecordBatchList(device, listType, slot, tickCount)) return false;
            }
        }
        return true;
    }

    bool ProbeSim::CreatePipelines(ID3D12Device5* device) {
        m_rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!m_rootSignature) return false;
        m_applyPipeline = LoadComputePipeline(device, m_rootSignature.Get(), "sim/probe_tick_apply.cso");
        m_diffusePipeline = LoadComputePipeline(device, m_rootSignature.Get(), "sim/probe_tick_diffuse.cso");
        m_extractPipeline = LoadComputePipeline(device, m_rootSignature.Get(), "sim/probe_tick_extract.cso");
        return m_applyPipeline && m_diffusePipeline && m_extractPipeline;
    }

    bool ProbeSim::CreateBuffers(ID3D12Device5* device) {
        m_world = gpu::CreateBuffer(device, uint64_t{CELL_BYTES} * 2, gpu::BufferKind::UnorderedAccess);
        m_busySink = gpu::CreateBuffer(device, CELL_BYTES, gpu::BufferKind::UnorderedAccess);
        for (auto& extraction : m_extractions) {
            extraction = gpu::CreateBuffer(device, CELL_BYTES, gpu::BufferKind::UnorderedAccess);
            if (!extraction) return false;
        }
        if (!m_world || !m_busySink) return false;
        m_world->SetName(L"ProbeSim.world");
        m_busySink->SetName(L"ProbeSim.busySink");
        for (uint32_t index = 0; index < PROBE_EXTRACTION_COUNT; ++index) {
            m_extractions[index]->SetName(std::format(L"ProbeSim.extraction{}", index).c_str());
        }

        const D3D12_QUERY_HEAP_DESC queryDesc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP,
                                              .Count = BATCH_SLOT_COUNT * TIMESTAMPS_PER_SLOT};
        if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_timestamps)))) return false;
        m_timestampReadback =
            gpu::CreateBuffer(device, uint64_t{BATCH_SLOT_COUNT} * TIMESTAMPS_PER_SLOT * 8, gpu::BufferKind::Readback);
        if (!m_timestampReadback) return false;

        for (BatchSlot& slot : m_slots) {
            slot.input = gpu::CreateBuffer(device, PROBE_BATCH_BYTES, gpu::BufferKind::Upload);
            if (!slot.input) return false;
            void* mapped = nullptr;
            const D3D12_RANGE noRead{};
            if (FAILED(slot.input->Map(0, &noRead, &mapped))) return false;
            slot.mappedInput = static_cast<std::byte*>(mapped);
        }
        return true;
    }

    // --- バッチのリストを 1 度だけ記録する ---

    bool ProbeSim::RecordBatchList(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE listType, uint32_t slot,
                                   uint32_t tickCount) {
        BatchSlot& batch = m_slots[slot];
        ComPtr<ID3D12GraphicsCommandList10>& recorded = batch.lists[tickCount - 1];
        if (FAILED(device->CreateCommandList(0, listType, batch.allocator.Get(), nullptr, IID_PPV_ARGS(&recorded)))) {
            return false;
        }
        ID3D12GraphicsCommandList10* list = recorded.Get();
        ID3D12Resource* input = batch.input.Get();
        const uint32_t firstQuery = slot * TIMESTAMPS_PER_SLOT;
        list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery);

        // --- ルートの引数 ---
        list->SetComputeRootSignature(m_rootSignature.Get());
        list->SetComputeRootUnorderedAccessView(UAV_WORLD, m_world->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_EVENTS, m_events.GpuAddress());
        list->SetComputeRootUnorderedAccessView(UAV_EXTRACTION0, m_extractions[0]->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_EXTRACTION1, m_extractions[1]->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_EXTRACTION2, m_extractions[2]->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(UAV_BUSY_SINK, m_busySink->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), m_debugRing.GpuAddress());
        list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(SRV_BATCH), input->GetGPUVirtualAddress());
        m_events.RecordBegin(list);
        m_debugRing.RecordBegin(list);

        // --- 刻み k = 0..tickCount-1(k はルート定数。コマンドの適用は最大の数だけ起動し、余ったスレッドは何もしない)---
        const D3D12_RESOURCE_BARRIER worldBarrier = gpu::UavBarrier(m_world.Get());
        for (uint32_t tickSlot = 0; tickSlot < tickCount; ++tickSlot) {
            list->SetComputeRoot32BitConstant(ROOT_LAYOUT.RootConstantIndex(), tickSlot, 0);
            list->SetPipelineState(m_applyPipeline.Get());
            list->Dispatch(APPLY_GROUPS, 1, 1);
            list->ResourceBarrier(1, &worldBarrier);
            list->SetPipelineState(m_diffusePipeline.Get());
            list->Dispatch(DIFFUSE_GROUPS, DIFFUSE_GROUPS, 1);
            list->ResourceBarrier(1, &worldBarrier);
        }

        // --- 描画用の抽出と、CPU への読み戻し ---
        list->SetPipelineState(m_extractPipeline.Get());
        list->Dispatch(EXTRACT_GROUPS, 1, 1);
        m_events.RecordReadbackAndReset(list, slot);
        m_debugRing.RecordReadbackAndReset(list, slot);
        list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery + 1);
        list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery, TIMESTAMPS_PER_SLOT,
                               m_timestampReadback.Get(), uint64_t{firstQuery} * 8);
        return SUCCEEDED(list->Close());
    }

    // --- バッチごと ---

    ID3D12CommandList* ProbeSim::PrepareBatch(uint32_t slot, const ProbeBatchInput& input) {
        if (slot >= BATCH_SLOT_COUNT || input.tickCount == 0 || input.tickCount > PROBE_MAX_TICKS_PER_BATCH ||
            input.commands.size() > PROBE_MAX_COMMANDS || input.extractionTarget >= PROBE_EXTRACTION_COUNT ||
            input.busyIterations > PROBE_BUSY_ITERATIONS_LIMIT) {
            Log(Channel::Sim, Level::Error, "バッチの入力が範囲外: slot {} 刻み {} コマンド {} 抽出 {} 重さ {}", slot,
                input.tickCount, input.commands.size(), input.extractionTarget, input.busyIterations);
            return nullptr;
        }
        std::byte* base = m_slots[slot].mappedInput;
        const auto commandCount = static_cast<uint32_t>(input.commands.size());

        // 見出し(probe_sim.hlsli の PROBE_HEADER_*)
        const std::array<uint32_t, 6> header = {static_cast<uint32_t>(input.firstTick),
                                                static_cast<uint32_t>(input.firstTick >> 32),
                                                input.tickCount,
                                                commandCount,
                                                input.extractionTarget,
                                                input.busyIterations};
        WriteAt(base, PROBE_BATCH_HEADER_OFFSET, header);

        if (commandCount > 0) {
            std::memcpy(base + PROBE_BATCH_COMMANDS_OFFSET, input.commands.data(), input.commands.size_bytes());
        }
        return m_slots[slot].lists[input.tickCount - 1].Get();
    }

    ProbeBatchReadback ProbeSim::ReadBatch(uint32_t slot) const {
        ProbeBatchReadback result;

        // 見出しを先に読み、書かれた分だけを読む
        uint32_t requested = 0;
        const bool headerRead = m_events.Read(slot, std::as_writable_bytes(std::span(&requested, 1)));
        const uint32_t stored = std::min(requested, PROBE_EVENT_CAPACITY);
        std::vector<uint32_t> words(PROBE_EVENT_HEADER_BYTES / 4 + size_t{stored} * PROBE_EVENT_WORDS);
        if (headerRead && (stored == 0 || m_events.Read(slot, std::as_writable_bytes(std::span(words))))) {
            result.droppedEventCount = requested - stored;
            result.events.reserve(stored);
            for (uint32_t index = 0; index < stored; ++index) {
                const uint32_t* record =
                    words.data() + PROBE_EVENT_HEADER_BYTES / 4 + size_t{index} * PROBE_EVENT_WORDS;
                result.events.push_back({.tick = record[0] | (uint64_t{record[1]} << 32),
                                         .type = record[2],
                                         .x = record[3] & 0xFFFFu,
                                         .y = record[3] >> 16});
            }
        }

        std::array<uint64_t, size_t{TIMESTAMPS_PER_SLOT} * BATCH_SLOT_COUNT> timestamps{};
        if (gpu::ReadBuffer(m_timestampReadback.Get(), std::as_writable_bytes(std::span(timestamps)))) {
            result.gpuBeginTimestamp = timestamps[size_t{slot} * TIMESTAMPS_PER_SLOT];
            result.gpuEndTimestamp = timestamps[size_t{slot} * TIMESTAMPS_PER_SLOT + 1];
        }
        result.debugAssertCount = m_debugRing.Drain(MAX_LOGGED_DEBUG_MESSAGES, slot).assertCount;
        return result;
    }

    // --- CPU リファレンス ---

    ProbeReference::ProbeReference() : m_cells(size_t{PROBE_CELL_COUNT} * 2, 0) {}

    void ProbeReference::Advance(uint64_t tick, std::span<const ProbeCommand> commands) {
        const size_t current = static_cast<size_t>(tick & 1) * PROBE_CELL_COUNT;
        const size_t next = static_cast<size_t>((tick + 1) & 1) * PROBE_CELL_COUNT;

        // (1) コマンドの適用(max)
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

    uint64_t HashCells(std::span<const uint32_t> cells) {
        uint64_t hash = 0xcbf29ce484222325ull;
        for (const uint32_t cell : cells) {
            for (uint32_t shift = 0; shift < 32; shift += 8) {
                hash ^= (cell >> shift) & 0xFFu;
                hash *= 0x100000001b3ull;
            }
        }
        return hash;
    }

}  // namespace bicameral::sim
