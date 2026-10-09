// gpu_gas.cpp — 気体の流れの 1 刻みを GPU の Compute で走らせる(gpu_gas.h)。段を呼ぶ順は shaders/sim/gas_step.hlsl の先頭の表と同じで、
// CPU リファレンス(gas_reference.cpp の Substep)の (1)〜(5) を、面の段とセルの段に分けて並べたもの。段の間は全部の UAV の書き込みを待つ。
// 小刻みの数は設定で決まる(1 刻み = substeps × 9 段)。反復の回数が決まっているので Work Graph にしても結果は同じ(D-302。T-0208 で測る)。
#include "sim/gpu_gas.h"

#include "common/gas_gpu.hlsli"
#include "gpu/resources.h"

namespace bicameral::sim {

    namespace {

        constexpr uint32_t THREADS_PER_GROUP = 64;  // gas_step.hlsl の numthreads
        constexpr uint32_t ROOT_CONSTANT_COUNT = 4;
        constexpr uint32_t MAX_GROUPS = 65535;  // Dispatch の 1 次元の上限

        // u0〜u5 / b0 / デバッグのリング / t0〜t2
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
            .uavCount = 6, .rootConstantCount = ROOT_CONSTANT_COUNT, .debugRing = true, .srvCount = 3};

        // shaders/CMakeLists.txt で入口ごとに作る .cso(Pass の順)
        constexpr std::array<const char*, 9> SHADER_NAMES = {
            "sim/gas_derive.cso",    "sim/gas_face_force.cso", "sim/gas_apply_force.cso",
            "sim/gas_face_flow.cso", "sim/gas_outflow.cso",    "sim/gas_face_moved.cso",
            "sim/gas_moved_sum.cso", "sim/gas_face_final.cso", "sim/gas_transfer.cso"};

        // 面の段か(面の番号 = セル × 6 ごとに 1 スレッド)
        constexpr std::array<bool, 9> FACE_PASS = {false, true, false, true, false, true, false, true, false};

        static_assert(sizeof(GasGpuCell) == 64 && sizeof(GasCell) == 64);
        static_assert(sizeof(GasGpuDerived) == 48);
        static_assert(sizeof(GasGpuFace) == 104);
        static_assert(sizeof(GasGpuCellSums) == 40);
        static_assert(sizeof(GasGpuParameters) == 176);
        static_assert(sizeof(GasLedger) == GAS_GPU_LEDGER_BYTES);
        static_assert(GAS_GPU_MAX_SPECIES == GAS_MAX_SPECIES);
        static_assert((uint32_t)GasBoundary::Wall == GAS_GPU_BOUNDARY_WALL &&
                      (uint32_t)GasBoundary::Periodic == GAS_GPU_BOUNDARY_PERIODIC &&
                      (uint32_t)GasBoundary::Open == GAS_GPU_BOUNDARY_OPEN);
        static_assert((uint32_t)GasReconstruction::Upwind == GAS_GPU_RECONSTRUCTION_UPWIND &&
                      (uint32_t)GasReconstruction::Minmod == GAS_GPU_RECONSTRUCTION_MINMOD &&
                      (uint32_t)GasReconstruction::MonotonizedCentral == GAS_GPU_RECONSTRUCTION_MC);

        GasGpuCell ToGpuCell(const GasCell& cell) {
            GasGpuCell result{};
            for (uint32_t s = 0; s < GAS_MAX_SPECIES; ++s)
                result.amounts[s] = cell.amounts[s];

            result.energy = cell.energy;
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                result.momentum[axis] = cell.momentum[axis];

            return result;
        }

        GasCell FromGpuCell(const GasGpuCell& cell) {
            GasCell result;
            for (uint32_t s = 0; s < GAS_MAX_SPECIES; ++s)
                result.amounts[s] = cell.amounts[s];

            result.energy = cell.energy;
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                result.momentum[axis] = cell.momentum[axis];

            return result;
        }

        GasGpuDerived ToGpuDerived(const GasDerived& derived) {
            return {.mass = derived.mass,
                    .inverseMass = derived.inverseMass,
                    .inverseAmount = derived.inverseAmount,
                    .temperature = derived.temperature,
                    .unused = 0,
                    .pressure = derived.pressure,
                    .scaledPressure = derived.scaledPressure};
        }

        GasGpuParameters MakeParameters(const GasBox& box) {
            const GasConfig& config = box.config;
            const GasCoefficients& coefficients = box.coefficients;
            GasGpuParameters parameters{};
            for (uint32_t s = 0; s < GAS_MAX_SPECIES; ++s) {
                parameters.massPerAmount[s] = coefficients.massPerAmount[s];
                parameters.formationEnergy[s] = config.species[s].formationEnergy;
                parameters.heatCapacity[s] = config.species[s].heatCapacity;
            }

            parameters.centralFlow = coefficients.centralFlow;
            parameters.pressureFlow = coefficients.pressureFlow;
            parameters.pressureImpulse = coefficients.pressureImpulse;
            parameters.gravityImpulse = coefficients.gravityImpulse;
            parameters.soundScale = coefficients.soundScale;
            parameters.randomSeed = config.randomSeed;

            parameters.size[0] = config.sizeX;
            parameters.size[1] = config.sizeY;
            parameters.size[2] = config.sizeZ;
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                parameters.boundary[axis] = (uint32_t)config.boundary[axis];

            parameters.speciesCount = config.speciesCount;
            parameters.soundTemperature = coefficients.soundTemperature;
            parameters.gravity = config.gravity ? 1 : 0;
            parameters.reconstruction = (uint32_t)config.reconstruction;
            parameters.stochasticRounding = config.stochasticRounding ? 1 : 0;

            return parameters;
        }

        template <typename T>
        ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, std::span<const T> data) {
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

        uint32_t GroupsFor(uint64_t threadCount) {
            return (uint32_t)((threadCount + THREADS_PER_GROUP - 1) / THREADS_PER_GROUP);
        }

    }  // namespace

    std::expected<GpuGas, std::string> GpuGas::Create(ID3D12Device5* device, const GasBox& box) {
        GpuGas result;
        result.m_cellCount = (uint32_t)box.cells.size();
        result.m_substeps = box.config.substeps;
        result.m_tick = box.tick;
        if (GroupsFor((uint64_t)result.m_cellCount * GAS_GPU_FACES_PER_CELL) > MAX_GROUPS)
            return std::unexpected("気体の箱が大きすぎる(面の段の Dispatch の上限)");

        // GPU は成分の欄を GAS_MAX_SPECIES 個とも回す(回数を定数にして展開する)。使わない欄は 0 の前提
        for (const GasCell& cell : box.cells) {
            for (uint32_t s = box.config.speciesCount; s < GAS_MAX_SPECIES; ++s) {
                if (cell.amounts[s] != 0)
                    return std::unexpected("使わない成分の欄に物質量がある");
            }
        }

        if (auto created = result.CreatePipelines(device); !created)
            return std::unexpected(created.error());

        if (auto created = result.CreateBuffers(device, box); !created)
            return std::unexpected(created.error());

        return result;
    }

    std::expected<void, std::string> GpuGas::CreatePipelines(ID3D12Device5* device) {
        m_rootSignature = gpu::CreateRootSignature(device, ROOT_LAYOUT);
        if (!m_rootSignature)
            return std::unexpected("気体のルート署名を作れない");

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

    // バッファ(gas_step.hlsl の結び付けの順)・初めの箱の写し・読み戻し
    std::expected<void, std::string> GpuGas::CreateBuffers(ID3D12Device5* device, const GasBox& box) {
        const uint64_t n = m_cellCount;
        const std::array<uint64_t, 6> uavBytes = {n * sizeof(GasGpuCell),     // u0 セル
                                                  n * sizeof(GasGpuCell),     // u1 力を当てた後のセル
                                                  n * sizeof(GasGpuDerived),  // u2 導く値
                                                  n * GAS_GPU_FACES_PER_CELL * sizeof(GasGpuFace),  // u3 面
                                                  n * sizeof(GasGpuCellSums),                       // u4 セルごとの合計
                                                  GAS_GPU_LEDGER_BYTES};                            // u5 帳簿
        for (size_t i = 0; i < uavBytes.size(); ++i) {
            m_uavs[i] = gpu::CreateBuffer(device, uavBytes[i], gpu::BufferKind::UnorderedAccess);
            if (!m_uavs[i])
                return std::unexpected("気体のバッファを作れない");
        }

        // --- t0〜t2(設定・層ごとの基準と外)---
        const std::array<GasGpuParameters, 1> parameters = {MakeParameters(box)};
        std::vector<GasGpuDerived> referenceDerived;
        std::vector<GasGpuCell> ghost;
        referenceDerived.reserve(box.referenceDerived.size());
        ghost.reserve(box.ghost.size());
        for (const GasDerived& layer : box.referenceDerived)
            referenceDerived.push_back(ToGpuDerived(layer));

        for (const GasCell& layer : box.ghost)
            ghost.push_back(ToGpuCell(layer));

        m_srvs = {CreateUploadBuffer(device, std::span<const GasGpuParameters>(parameters)),
                  CreateUploadBuffer(device, std::span<const GasGpuDerived>(referenceDerived)),
                  CreateUploadBuffer(device, std::span<const GasGpuCell>(ghost))};

        // --- 初めの箱 ---
        std::vector<GasGpuCell> cells;
        cells.reserve(box.cells.size());
        for (const GasCell& cell : box.cells)
            cells.push_back(ToGpuCell(cell));

        const std::array<GasLedger, 1> ledger = {box.ledger};
        m_cellUpload = CreateUploadBuffer(device, std::span<const GasGpuCell>(cells));
        m_ledgerUpload = CreateUploadBuffer(device, std::span<const GasLedger>(ledger));
        m_cellReadback = gpu::CreateBuffer(device, uavBytes[0], gpu::BufferKind::Readback);
        m_ledgerReadback = gpu::CreateBuffer(device, GAS_GPU_LEDGER_BYTES, gpu::BufferKind::Readback);
        m_faceReadback = gpu::CreateBuffer(device, uavBytes[3], gpu::BufferKind::Readback);
        m_sumReadback = gpu::CreateBuffer(device, uavBytes[4], gpu::BufferKind::Readback);
        if (!m_srvs[0] || !m_srvs[1] || !m_srvs[2] || !m_cellUpload || !m_ledgerUpload || !m_cellReadback ||
            !m_ledgerReadback || !m_faceReadback || !m_sumReadback)
            return std::unexpected("気体のアップロード・読み戻しのバッファを作れない");

        return {};
    }

    void GpuGas::RecordUpload(ID3D12GraphicsCommandList10* list) {
        // バッファは COMMON から COPY_DEST へ暗黙に上がる(リストの終わりに COMMON へ戻る)
        list->CopyResource(m_uavs[0].Get(), m_cellUpload.Get());
        list->CopyResource(m_uavs[5].Get(), m_ledgerUpload.Get());
        const std::array<D3D12_RESOURCE_BARRIER, 2> toUav = {
            gpu::Transition(m_uavs[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            gpu::Transition(m_uavs[5].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        list->ResourceBarrier((uint32_t)toUav.size(), toUav.data());
    }

    // 1 つの段を投げ、全部の UAV の書き込みを次の段より前に終わらせる
    void GpuGas::RecordPass(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, Pass pass,
                            const Constants& constants) const {
        static_assert(sizeof(Constants) == ROOT_CONSTANT_COUNT * sizeof(uint32_t));
        list->SetComputeRootSignature(m_rootSignature.Get());
        list->SetPipelineState(m_pipelines[(size_t)pass].Get());
        for (uint32_t i = 0; i < m_uavs.size(); ++i)
            list->SetComputeRootUnorderedAccessView(i, m_uavs[i]->GetGPUVirtualAddress());

        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT, &constants, 0);
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing);
        for (uint32_t i = 0; i < m_srvs.size(); ++i)
            list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(i), m_srvs[i]->GetGPUVirtualAddress());

        const uint64_t threads = FACE_PASS[(size_t)pass] ? (uint64_t)m_cellCount * GAS_GPU_FACES_PER_CELL : m_cellCount;
        list->Dispatch(GroupsFor(threads), 1, 1);
        const D3D12_RESOURCE_BARRIER barrier = gpu::UavBarrier(nullptr);
        list->ResourceBarrier(1, &barrier);
    }

    void GpuGas::RecordStep(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing) {
        for (uint32_t substep = 0; substep < m_substeps; ++substep) {
            // 乱数の丸めの小刻みの通し番号(gas_reference.cpp の StepGas と同じ)
            const uint64_t roundingTick = (m_tick * m_substeps) + substep;
            const Constants constants{.roundingTickLow = (uint32_t)roundingTick,
                                      .roundingTickHigh = (uint32_t)(roundingTick >> 32),
                                      .cellCount = m_cellCount,
                                      .unused = 0};
            for (size_t pass = 0; pass < std::min((size_t)m_debugPassLimit, (size_t)Pass::Count); ++pass)
                RecordPass(list, debugRing, (Pass)pass, constants);
        }

        ++m_tick;
    }

    void GpuGas::RecordReadback(ID3D12GraphicsCommandList10* list) const {
        gpu::RecordCopyToReadback(list, m_uavs[0].Get(), m_cellReadback.Get());
        gpu::RecordCopyToReadback(list, m_uavs[5].Get(), m_ledgerReadback.Get());
    }

    bool GpuGas::Read(std::vector<GasCell>& cells, GasLedger& ledger) const {
        std::vector<GasGpuCell> gpuCells(m_cellCount);
        if (!gpu::ReadBuffer(m_cellReadback.Get(), std::as_writable_bytes(std::span(gpuCells))))
            return false;

        if (!gpu::ReadBuffer(m_ledgerReadback.Get(), std::as_writable_bytes(std::span(&ledger, 1))))
            return false;

        cells.clear();
        cells.reserve(gpuCells.size());
        for (const GasGpuCell& cell : gpuCells)
            cells.push_back(FromGpuCell(cell));

        return true;
    }

    // --- 調べる用 ---

    void GpuGas::RecordDebugReadback(ID3D12GraphicsCommandList10* list) const {
        gpu::RecordCopyToReadback(list, m_uavs[3].Get(), m_faceReadback.Get());
        gpu::RecordCopyToReadback(list, m_uavs[4].Get(), m_sumReadback.Get());
    }

    bool GpuGas::ReadDebug(std::vector<uint64_t>& faceWords, std::vector<uint64_t>& sumWords) const {
        faceWords.assign((size_t)m_cellCount * GAS_GPU_FACES_PER_CELL * sizeof(GasGpuFace) / sizeof(uint64_t), 0);
        sumWords.assign((size_t)m_cellCount * sizeof(GasGpuCellSums) / sizeof(uint64_t), 0);

        return gpu::ReadBuffer(m_faceReadback.Get(), std::as_writable_bytes(std::span(faceWords))) &&
               gpu::ReadBuffer(m_sumReadback.Get(), std::as_writable_bytes(std::span(sumWords)));
    }

    // --- 計測 ---

    bool GpuGas::EnableTiming(ID3D12Device* device) {
        const D3D12_QUERY_HEAP_DESC queryDesc{.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP, .Count = 2, .NodeMask = 0};
        if (FAILED(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_timestamps))))
            return false;

        m_timestampReadback = gpu::CreateBuffer(device, 2 * sizeof(uint64_t), gpu::BufferKind::Readback);

        return m_timestampReadback != nullptr;
    }

    void GpuGas::RecordTimestamp(ID3D12GraphicsCommandList10* list, uint32_t slot) const {
        if (m_timestamps)
            list->EndQuery(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot);
    }

    void GpuGas::RecordTimingReadback(ID3D12GraphicsCommandList10* list) const {
        if (m_timestamps)
            list->ResolveQueryData(m_timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, m_timestampReadback.Get(), 0);
    }

    std::optional<uint64_t> GpuGas::TimingTicks() const {
        std::array<uint64_t, 2> ticks = {};
        if (!m_timestamps || !gpu::ReadBuffer(m_timestampReadback.Get(), std::as_writable_bytes(std::span(ticks))))
            return std::nullopt;

        return ticks[1] - ticks[0];
    }

}  // namespace bicameral::sim
