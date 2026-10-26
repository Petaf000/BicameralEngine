// gpu_time_tree_choose.cpp — 刻みを選ぶ段の GPU 版(gpu_time_tree_choose.h。T-1112)。段は shaders/sim/time_tree_choose.hlsl。
// CPU リファレンスの順(time_step_tree.cpp): WindowCaps(望みを窓の格子に置く → 軸ごとに半径の内の最小 × 3 → 単位の格子のセルの最小)
//   → ChooseExponents(窓の上限・隣の差 1 段・長すぎる歩の途中は打ち切る。不動点まで)→ 列を解いて書いた後の単位の記録。
#include "sim/gpu_time_tree_choose.h"

#include <algorithm>
#include <array>

namespace bicameral::sim {

    namespace {

        constexpr uint32_t THREADS_PER_GROUP = 64;  // time_tree_choose.hlsl の numthreads
        constexpr uint32_t ROOT_CONSTANT_COUNT = 9;
        constexpr uint32_t STATE_EXPONENT_MASK = 0xFFu;  // time_tree_choose.hlsl の TTC_STATE_*
        constexpr uint32_t STATE_MIDSTEP = 1u << 8;

        // u0 窓の上限・u1 今の e・u2 後の記録・u3 掃く回の印・u4/u5 窓の格子 / b0 / デバッグのリング /
        // t0 選ぶ前の記録・t1 格子の単位・t2 隣の始まり・t3 隣
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
            .uavCount = 6, .rootConstantCount = ROOT_CONSTANT_COUNT, .debugRing = true, .srvCount = 4};

        // b0(time_tree_choose.hlsl の TimeTreeChooseConstants と同じ順)
        struct Constants {
            uint32_t units = 0;
            uint32_t gridX = 0;
            uint32_t gridY = 0;
            uint32_t gridZ = 0;
            uint32_t radius = 0;
            uint32_t maxExponent = 0;
            uint32_t cap = 0;
            uint32_t pass = 0;
            uint32_t beforeWord = 0;
        };

        enum Stage : uint32_t {
            STAGE_INIT,
            STAGE_SCATTER,
            STAGE_MIN_AXIS,
            STAGE_GATHER,
            STAGE_SWEEP,
            STAGE_PACK,
            STAGES
        };

        constexpr std::array<const char*, STAGES> STAGE_SHADERS = {
            "sim/time_tree_choose_init.cso",   "sim/time_tree_choose_scatter.cso", "sim/time_tree_choose_min_axis.cso",
            "sim/time_tree_choose_gather.cso", "sim/time_tree_choose_sweep.cso",   "sim/time_tree_choose_pack.cso"};

        // 空の表は 1 語にして作る(ルートの SRV は 0 バイトのバッファを持てない)
        gpu::ResidentBuffer MakeTable(ID3D12Device5* device, std::span<const uint32_t> values) {
            static constexpr std::array<uint32_t, 1> EMPTY = {0};
            const std::span<const uint32_t> data = values.empty() ? std::span<const uint32_t>(EMPTY) : values;

            return gpu::CreateResidentBuffer(device, std::as_bytes(data));
        }

        void CopyOut(ID3D12GraphicsCommandList* list, ID3D12Resource* source, ID3D12Resource* readback,
                     uint64_t bytes) {
            D3D12_RESOURCE_BARRIER barrier = gpu::Transition(source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                             D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &barrier);
            list->CopyBufferRegion(readback, 0, source, 0, bytes);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            list->ResourceBarrier(1, &barrier);
        }

    }  // namespace

    std::expected<GpuTimeTreeChoose, std::string> GpuTimeTreeChoose::Create(ID3D12Device5* device,
                                                                            const TimeTreeChooseTables& tables,
                                                                            std::span<const uint16_t> befores) {
        if (!tables.supported || tables.units == 0 || befores.size() % tables.units != 0)
            return std::unexpected(
                "刻みを選ぶ段の GPU 版の範囲の外(窓・隣の差 1 段だけ)か、記録の数が単位の倍数でない");

        GpuTimeTreeChoose result;
        result.m_units = tables.units;
        result.m_gridCells = tables.gridSize[0] * tables.gridSize[1] * tables.gridSize[2];
        result.m_wordsPerEvent = (tables.units + 1) / 2;
        result.m_events = static_cast<uint32_t>(befores.size() / tables.units);
        result.m_shape.units = tables.units;
        result.m_shape.gridSize = tables.gridSize;
        result.m_shape.radius = tables.radius;
        result.m_shape.maxExponent = tables.maxExponent;

        // --- 選ぶ前の記録: 出来事ごとに 2 単位で 1 語(単位の数が奇数なら最後の上半分は 0)---
        std::vector<uint32_t> words(size_t{result.m_wordsPerEvent} * result.m_events, 0);
        for (uint32_t event = 0; event < result.m_events; ++event) {
            for (uint32_t unit = 0; unit < tables.units; ++unit) {
                const uint32_t record = befores[(size_t{event} * tables.units) + unit];
                words[(size_t{event} * result.m_wordsPerEvent) + (unit / 2)] |= (unit & 1) == 0 ? record : record << 16;
            }
        }

        // --- パイプライン ---
        result.m_rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!result.m_rootSignature)
            return std::unexpected("ルート署名を作れない");

        for (const char* shader : STAGE_SHADERS) {
            const auto bytecode = gpu::LoadShader(shader);
            if (!bytecode)
                return std::unexpected(bytecode.error());

            result.m_pipelines.push_back(gpu::CreateComputePipeline(device, result.m_rootSignature.Get(), *bytecode));
            if (!result.m_pipelines.back())
                return std::unexpected("パイプラインを作れない");
        }

        // --- 表・作業場・読み戻し・時刻印 ---
        result.m_before = MakeTable(device, words);
        result.m_gridUnits = MakeTable(device, tables.gridUnits);
        result.m_neighborStarts = MakeTable(device, tables.neighborStarts);
        result.m_neighbors = MakeTable(device, tables.neighbors);
        const uint64_t unitBytes = uint64_t{tables.units} * sizeof(uint32_t);
        const uint64_t afterBytes = uint64_t{result.m_wordsPerEvent} * sizeof(uint32_t);
        const uint64_t counterBytes = uint64_t{MAX_SWEEPS} * sizeof(uint32_t);
        const uint64_t gridBytes = uint64_t{std::max(result.m_gridCells, 1u)} * sizeof(uint32_t);
        constexpr auto UAV = gpu::BufferKind::UnorderedAccess;
        constexpr auto READBACK = gpu::BufferKind::Readback;
        result.m_caps = gpu::CreateBuffer(device, unitBytes, UAV);
        result.m_state = gpu::CreateBuffer(device, unitBytes, UAV);
        result.m_after = gpu::CreateBuffer(device, afterBytes, UAV);
        result.m_counters = gpu::CreateBuffer(device, counterBytes, UAV);
        result.m_windowA = gpu::CreateBuffer(device, gridBytes, UAV);
        result.m_windowB = gpu::CreateBuffer(device, gridBytes, UAV);
        result.m_capsReadback = gpu::CreateBuffer(device, unitBytes, READBACK);
        result.m_stateReadback = gpu::CreateBuffer(device, unitBytes, READBACK);
        result.m_afterReadback = gpu::CreateBuffer(device, afterBytes, READBACK);
        result.m_countersReadback = gpu::CreateBuffer(device, counterBytes, READBACK);
        result.m_timestampReadback = gpu::CreateBuffer(device, uint64_t{MAX_TIMESTAMPS} * sizeof(uint64_t), READBACK);
        const D3D12_QUERY_HEAP_DESC queryDesc{
            .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = MAX_TIMESTAMPS, .NodeMask = 0};
        const bool tablesMade = result.m_before.Valid() && result.m_gridUnits.Valid() &&
                                result.m_neighborStarts.Valid() && result.m_neighbors.Valid();
        const bool buffersMade = result.m_caps && result.m_state && result.m_after && result.m_counters &&
                                 result.m_windowA && result.m_windowB && result.m_capsReadback &&
                                 result.m_stateReadback && result.m_afterReadback && result.m_countersReadback &&
                                 result.m_timestampReadback;
        if (!tablesMade || !buffersMade ||
            FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&result.m_timestamps))))
            return std::unexpected("刻みを選ぶ段のバッファを作れない");

        return result;
    }

    void GpuTimeTreeChoose::RecordUpload(ID3D12GraphicsCommandList* list) const {
        const std::array<gpu::ResidentBuffer, 4> tables = {m_before, m_gridUnits, m_neighborStarts, m_neighbors};
        gpu::RecordResidentUploads(list, tables);
    }

    void GpuTimeTreeChoose::Bind(ID3D12GraphicsCommandList* list, uint64_t debugRing) const {
        list->SetComputeRootSignature(m_rootSignature.Get());
        const std::array<ID3D12Resource*, 6> buffers = {m_caps.Get(),     m_state.Get(),   m_after.Get(),
                                                        m_counters.Get(), m_windowA.Get(), m_windowB.Get()};
        for (uint32_t i = 0; i < buffers.size(); ++i)
            list->SetComputeRootUnorderedAccessView(i, buffers[i]->GetGPUVirtualAddress());

        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing);
        const std::array<ID3D12Resource*, 4> tables = {m_before.buffer.Get(), m_gridUnits.buffer.Get(),
                                                       m_neighborStarts.buffer.Get(), m_neighbors.buffer.Get()};
        for (uint32_t i = 0; i < tables.size(); ++i)
            list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(i), tables[i]->GetGPUVirtualAddress());
    }

    void GpuTimeTreeChoose::Dispatch(ID3D12GraphicsCommandList* list, uint32_t pipeline, uint32_t threads,
                                     uint32_t pass, uint32_t event, int32_t cap) const {
        static_assert(sizeof(Constants) == ROOT_CONSTANT_COUNT * sizeof(uint32_t));
        const Constants constants{.units = m_units,
                                  .gridX = m_shape.gridSize[0],
                                  .gridY = m_shape.gridSize[1],
                                  .gridZ = m_shape.gridSize[2],
                                  .radius = m_shape.radius,
                                  .maxExponent = static_cast<uint32_t>(m_shape.maxExponent),
                                  .cap = static_cast<uint32_t>(cap),
                                  .pass = pass,
                                  .beforeWord = event * m_wordsPerEvent};
        list->SetPipelineState(m_pipelines[pipeline].Get());
        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT, &constants, 0);
        list->Dispatch(std::max((threads + THREADS_PER_GROUP - 1) / THREADS_PER_GROUP, 1u), 1, 1);

        const D3D12_RESOURCE_BARRIER barrier = gpu::UavBarrier(nullptr);
        list->ResourceBarrier(1, &barrier);
    }

    void GpuTimeTreeChoose::RecordChoose(ID3D12GraphicsCommandList* list, uint64_t debugRing, uint32_t event,
                                         int32_t cap) const {
        Bind(list, debugRing);
        Dispatch(list, STAGE_INIT, std::max(m_units, MAX_SWEEPS), 0, event, cap);
        Dispatch(list, STAGE_SCATTER, m_gridCells, 0, event, cap);
        for (uint32_t axis = 0; axis < 3; ++axis)
            Dispatch(list, STAGE_MIN_AXIS, m_gridCells, axis, event, cap);

        Dispatch(list, STAGE_GATHER, m_gridCells, 0, event, cap);
        for (uint32_t sweep = 0; sweep < MAX_SWEEPS; ++sweep)
            Dispatch(list, STAGE_SWEEP, m_units, sweep, event, cap);

        Dispatch(list, STAGE_PACK, m_wordsPerEvent, 0, event, cap);
    }

    void GpuTimeTreeChoose::RecordReadback(ID3D12GraphicsCommandList* list) const {
        const uint64_t unitBytes = uint64_t{m_units} * sizeof(uint32_t);
        CopyOut(list, m_caps.Get(), m_capsReadback.Get(), unitBytes);
        CopyOut(list, m_state.Get(), m_stateReadback.Get(), unitBytes);
        CopyOut(list, m_after.Get(), m_afterReadback.Get(), uint64_t{m_wordsPerEvent} * sizeof(uint32_t));
        CopyOut(list, m_counters.Get(), m_countersReadback.Get(), uint64_t{MAX_SWEEPS} * sizeof(uint32_t));
    }

    bool GpuTimeTreeChoose::Read(GpuTimeTreeChoice& choice) const {
        std::vector<uint32_t> state(m_units);
        std::vector<uint32_t> after(m_wordsPerEvent);
        std::vector<uint32_t> counters(MAX_SWEEPS);
        choice.windowCaps.resize(m_units);
        if (!gpu::ReadBuffer(m_capsReadback.Get(), std::as_writable_bytes(std::span(choice.windowCaps))) ||
            !gpu::ReadBuffer(m_stateReadback.Get(), std::as_writable_bytes(std::span(state))) ||
            !gpu::ReadBuffer(m_afterReadback.Get(), std::as_writable_bytes(std::span(after))) ||
            !gpu::ReadBuffer(m_countersReadback.Get(), std::as_writable_bytes(std::span(counters))))
            return false;

        choice.next.resize(m_units);
        choice.after.resize(m_units);
        for (uint32_t unit = 0; unit < m_units; ++unit) {
            const bool midStep = (state[unit] & STATE_MIDSTEP) != 0;
            choice.next[unit] = midStep ? -1 : static_cast<int32_t>(state[unit] & STATE_EXPONENT_MASK);
            const uint32_t word = after[unit / 2];
            choice.after[unit] = static_cast<uint16_t>((unit & 1) == 0 ? word & 0xFFFFu : word >> 16);
        }

        choice.sweeps = static_cast<uint32_t>(std::ranges::count_if(counters, [](uint32_t c) { return c != 0; }));
        choice.settled = counters.back() == 0;

        return true;
    }

    void GpuTimeTreeChoose::RecordTimestamp(ID3D12GraphicsCommandList* list, uint32_t index) const {
        if (index < MAX_TIMESTAMPS)
            list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index);
    }

    void GpuTimeTreeChoose::RecordTimestampReadback(ID3D12GraphicsCommandList* list, uint32_t count) const {
        list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, std::min(count, MAX_TIMESTAMPS),
                               m_timestampReadback.Get(), 0);
    }

    std::vector<uint64_t> GpuTimeTreeChoose::ReadTimestamps(uint32_t count) const {
        std::vector<uint64_t> ticks(std::min(count, MAX_TIMESTAMPS));
        if (!gpu::ReadBuffer(m_timestampReadback.Get(), std::as_writable_bytes(std::span(ticks))))
            return {};

        return ticks;
    }

    uint64_t GpuTimeTreeChoose::DeviceBytes() const {
        uint64_t bytes = 0;
        for (ID3D12Resource* resource :
             {m_before.buffer.Get(), m_gridUnits.buffer.Get(), m_neighborStarts.buffer.Get(), m_neighbors.buffer.Get(),
              m_caps.Get(), m_state.Get(), m_after.Get(), m_counters.Get(), m_windowA.Get(), m_windowB.Get()})
            bytes += resource->GetDesc().Width;

        return bytes;
    }

}  // namespace bicameral::sim
