// gpu_gas_projection.cpp — 気体の圧力の投影の 1 刻みを GPU で(gpu_gas_projection.h)。段の順は shaders/sim/gas_projection.hlsl の先頭の表と同じ。
// CPU リファレンスの順(gas_projection.cpp の ProjectOnEquilibrium。部品の上の道):
//   BeginGasPressure(導く値・C_i・F*・K_f)→ MakeGasEquilibriumSystem → SolveEquilibrium(対角・右辺 ÷ D・段の重み・V を固定の回数)
//   → FinishGasPressure(x の力積・力を当てる・面の流れ・出ていく割合・移す)。段の間は全部の UAV の書き込みを待つ。
#include "sim/gpu_gas_projection.h"

#include <algorithm>
#include <cstring>
#include <format>

#include "common/gas_gpu.hlsli"
#include "sim/gas_pressure_system.h"
#include "sim/gpu_gas_internal.h"

namespace bicameral::sim {

    namespace {

        constexpr uint32_t THREADS_PER_GROUP = 64;  // gas_projection.hlsl の numthreads
        constexpr uint32_t ROOT_CONSTANT_COUNT = 12;
        constexpr uint32_t GAS_UAV_COUNT = 6;  // u0〜u5 は GpuGas のバッファ

        // u0〜u5 気体・u6〜u10 部品(節・隣・子の一覧・作業場・作業用の 128bit)/ b0 / デバッグのリング / t0〜t3
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
            .uavCount = 11, .rootConstantCount = ROOT_CONSTANT_COUNT, .debugRing = true, .srvCount = 4};

        constexpr std::array<const char*, 4> SHADER_NAMES = {"sim/gas_project_face.cso", "sim/gas_project_cell.cso",
                                                             "sim/gas_project_pressure.cso",
                                                             "sim/gas_project_flow.cso"};

        uint32_t GroupsFor(uint64_t threadCount) {
            return (uint32_t)((threadCount + THREADS_PER_GROUP - 1) / THREADS_PER_GROUP);
        }

        // 面の段は 1 行を超えたら 2 次元で(gas_step.hlsl の GasFaceThread)
        void DispatchRows(ID3D12GraphicsCommandList* list, uint32_t groups) {
            if (groups <= GAS_GPU_FACE_ROW_GROUPS) {
                list->Dispatch(groups, 1, 1);
                return;
            }

            list->Dispatch(GAS_GPU_FACE_ROW_GROUPS, (groups + GAS_GPU_FACE_ROW_GROUPS - 1) / GAS_GPU_FACE_ROW_GROUPS,
                           1);
        }

    }  // namespace

    std::expected<GpuGasProjection, std::string> GpuGasProjection::Create(ID3D12Device5* device, const GasBox& box,
                                                                          const ImplicitGrid& grid,
                                                                          const GpuGasProjectionOptions& options) {
        if (grid.levels.empty() || grid.levels.front().levels.size() != box.cells.size() ||
            !GpuEquilibrium::Supports(grid))
            return std::unexpected("投影の段の形が気体の箱と合わない");

        auto gas = GpuGas::Create(device, box);
        if (!gas)
            return std::unexpected(gas.error());

        // 部品(段の形は刻みで変わりうるので、上限に余裕を持たせる。作業場の番地は上限で決まる)
        auto equilibrium = GpuEquilibrium::Create(
            device, grid, {.solver = options.solver, .shape = options.shape, .meterResiduals = options.meterResiduals});
        if (!equilibrium)
            return std::unexpected(equilibrium.error());

        GpuGasProjection result(std::move(*gas), std::move(*equilibrium));
        result.m_tick = box.tick;
        if (auto created = result.CreatePipelines(device); !created)
            return std::unexpected(created.error());

        // --- t0: 投影の係数の設定(空きを読むかは箱のまま)---
        const uint32_t stride = result.m_gas.SpeciesStride();
        std::vector<std::byte> parameters = PackGasGpuParameters(box.config, GasPressureCoefficients(box, 1), stride);
        const uint32_t openness = box.openQ16.empty() ? 0u : 1u;
        std::memcpy(parameters.data() + GAS_GPU_PARAMETERS_OPENNESS, &openness, sizeof(openness));
        result.m_parameters = gpu::CreateResidentBuffer(device, parameters);
        if (!result.m_parameters.Valid())
            return std::unexpected("投影のバッファを作れない");

        // --- 刻みで変わらないルート定数 ---
        const uint32_t ground = box.referenceDerived.front().temperature;
        const uint64_t perSound = GasPressurePerSound(options.soundSpeedMmPerS, ground);
        const uint64_t statePerSound = GasPressurePerSound(box.config.soundSpeedMmPerS, ground);
        const uint64_t link = GasPressureLinkCoefficient(1);
        result.m_constants = {.cellCount = (uint32_t)box.cells.size(),
                              .speciesStride = stride,
                              .maxNodes = result.m_equilibrium.MaxNodes(),
                              .maxLinks = result.m_equilibrium.MaxLinks(),
                              .perSoundLow = (uint32_t)perSound,
                              .perSoundHigh = (uint32_t)(perSound >> 32),
                              .statePerSoundLow = (uint32_t)statePerSound,
                              .statePerSoundHigh = (uint32_t)(statePerSound >> 32),
                              .linkLow = (uint32_t)link,
                              .linkHigh = (uint32_t)(link >> 32)};

        return result;
    }

    std::expected<void, std::string> GpuGasProjection::CreatePipelines(ID3D12Device5* device) {
        m_rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!m_rootSignature)
            return std::unexpected("投影のルート署名を作れない");

        for (size_t pass = 0; pass < SHADER_NAMES.size(); ++pass) {
            const auto bytecode = gpu::LoadShader(SHADER_NAMES[pass]);
            if (!bytecode)
                return std::unexpected(bytecode.error());

            m_pipelines[pass] = gpu::CreateComputePipeline(device, m_rootSignature.Get(), *bytecode);
            if (!m_pipelines[pass])
                return std::unexpected(std::format("パイプラインを作れない: {}", SHADER_NAMES[pass]));
        }

        return {};
    }

    bool GpuGasProjection::RecordUpload(ID3D12GraphicsCommandList10* list, const ImplicitGrid& grid) {
        m_gas.RecordUpload(list);
        gpu::RecordResidentUploads(list, std::span(&m_parameters, 1));

        // 段の形と、前の解(値 A・B)を 0 に(CPU の模型の GasProjection::pressures の初め)
        return m_equilibrium.RecordUpload(list, grid, true);
    }

    bool GpuGasProjection::RecordBegin(ID3D12GraphicsCommandList10* list, const ImplicitGrid* shape) {
        if (shape != nullptr)
            return m_equilibrium.RecordUpload(list, *shape, false);

        m_equilibrium.RecordBegin(list);

        return true;
    }

    // 1 つの段を投げ、全部の UAV の書き込みを次の段より前に終わらせる
    void GpuGasProjection::RecordPass(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, Pass pass,
                                      const Constants& constants, uint32_t threads) const {
        static_assert(sizeof(Constants) == ROOT_CONSTANT_COUNT * sizeof(uint32_t));
        list->SetComputeRootSignature(m_rootSignature.Get());
        list->SetPipelineState(m_pipelines[(size_t)pass].Get());
        for (uint32_t i = 0; i < GAS_UAV_COUNT; ++i)
            list->SetComputeRootUnorderedAccessView(i, m_gas.Uav(i)->GetGPUVirtualAddress());

        const std::array<ID3D12Resource*, 5> implicitBuffers = {
            m_equilibrium.NodesBuffer(), m_equilibrium.LinksBuffer(), m_equilibrium.ListsBuffer(),
            m_equilibrium.WorkBuffer(), m_equilibrium.ScratchBuffer()};
        for (uint32_t i = 0; i < implicitBuffers.size(); ++i)
            list->SetComputeRootUnorderedAccessView(GAS_UAV_COUNT + i, implicitBuffers[i]->GetGPUVirtualAddress());

        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT, &constants, 0);
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing);
        list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(0), m_parameters.buffer->GetGPUVirtualAddress());
        for (uint32_t i = 1; i < 4; ++i)
            list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(i), m_gas.SrvAddress(i));

        const bool facePass = pass == Pass::Face || pass == Pass::Flow;
        if (facePass)
            DispatchRows(list, GroupsFor((uint64_t)threads * GAS_GPU_FACES_PER_CELL));
        else
            list->Dispatch(GroupsFor(threads), 1, 1);

        const D3D12_RESOURCE_BARRIER barrier = gpu::UavBarrier(nullptr);
        list->ResourceBarrier(1, &barrier);
    }

    void GpuGasProjection::RecordStamp(ID3D12GraphicsCommandList10* list, bool timed, uint32_t part) const {
        if (timed && m_timestamps && m_timedSteps < MAX_TIMED_STEPS)
            list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (m_timedSteps * STAMPS_PER_STEP) + part);
    }

    bool GpuGasProjection::RecordStep(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing,
                                      bool timed) {
        // 1 刻み = 投影 1 回(substeps 1)。乱数の丸めの通し番号 = 刻み(gas_projection.cpp の StepGasProjection)
        const uint64_t roundingTick = m_tick;
        Constants constants = m_constants;
        constants.roundingTickLow = (uint32_t)roundingTick;
        constants.roundingTickHigh = (uint32_t)(roundingTick >> 32);
        const D3D12_GPU_VIRTUAL_ADDRESS parameters = m_parameters.buffer->GetGPUVirtualAddress();
        const uint32_t cells = m_constants.cellCount;

        // --- 式を作る: 導く値 → 面の K_f・F* → セルの C_i・右辺・段 0 の重み ---
        RecordStamp(list, timed, 0);
        m_gas.RecordProjectionPhase(list, debugRing, GpuGas::ProjectionPhase::Derive, roundingTick, parameters);
        RecordPass(list, debugRing, Pass::Face, constants, cells);
        RecordPass(list, debugRing, Pass::Cell, constants, cells);
        RecordStamp(list, timed, 1);

        // --- 部品: 粗い段の重み(段 1 から順に)→ V サイクル(ちょうど solver.cycles 回)→ 残差の計器(積む時)---
        m_equilibrium.RecordLevels(list, debugRing);
        RecordStamp(list, timed, 2);
        if (!m_equilibrium.RecordSolve(list, debugRing))
            return false;

        RecordStamp(list, timed, 3);

        // --- 当てる: x を p̃ へ → 面の力・力を当てる → 面の流れ ---
        RecordPass(list, debugRing, Pass::Pressure, constants, cells);
        m_gas.RecordProjectionPhase(list, debugRing, GpuGas::ProjectionPhase::Force, roundingTick, parameters);
        RecordPass(list, debugRing, Pass::Flow, constants, cells);
        RecordStamp(list, timed, 4);

        // --- 運ぶ ---
        m_gas.RecordProjectionPhase(list, debugRing, GpuGas::ProjectionPhase::Carry, roundingTick, parameters);
        RecordStamp(list, timed, 5);

        if (timed && m_timestamps && m_timedSteps < MAX_TIMED_STEPS)
            ++m_timedSteps;

        ++m_tick;

        return true;
    }

    uint64_t GpuGasProjection::DeviceBytes() const {
        uint64_t total = m_equilibrium.DeviceBytes() + m_parameters.buffer->GetDesc().Width;
        for (uint32_t i = 0; i < GAS_UAV_COUNT; ++i)
            total += m_gas.Uav(i)->GetDesc().Width;

        return total;
    }

    // --- 計測 ---

    bool GpuGasProjection::EnableTiming(ID3D12Device* device) {
        const D3D12_QUERY_HEAP_DESC queryDesc{
            .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = MAX_TIMED_STEPS * STAMPS_PER_STEP, .NodeMask = 0};
        if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_timestamps))))
            return false;

        m_timestampReadback = gpu::CreateBuffer(device, (uint64_t)MAX_TIMED_STEPS * STAMPS_PER_STEP * sizeof(uint64_t),
                                                gpu::BufferKind::Readback);

        return m_timestampReadback != nullptr;
    }

    void GpuGasProjection::RecordTimingReadback(ID3D12GraphicsCommandList10* list) const {
        if (m_timestamps && m_timedSteps > 0) {
            list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, m_timedSteps * STAMPS_PER_STEP,
                                   m_timestampReadback.Get(), 0);
        }
    }

    std::vector<std::array<uint64_t, (size_t)GpuGasProjectionPart::Count>> GpuGasProjection::ReadTiming() const {
        std::vector<std::array<uint64_t, (size_t)GpuGasProjectionPart::Count>> steps;
        std::vector<uint64_t> stamps((size_t)m_timedSteps * STAMPS_PER_STEP);
        if (!m_timestamps || stamps.empty() ||
            !gpu::ReadBuffer(m_timestampReadback.Get(), std::as_writable_bytes(std::span(stamps))))
            return steps;

        for (uint32_t step = 0; step < m_timedSteps; ++step) {
            std::array<uint64_t, (size_t)GpuGasProjectionPart::Count> parts{};
            for (uint32_t part = 0; part < parts.size(); ++part) {
                const size_t base = ((size_t)step * STAMPS_PER_STEP) + part;
                parts[part] = stamps[base + 1] - stamps[base];
            }

            steps.push_back(parts);
        }

        return steps;
    }

}  // namespace bicameral::sim
