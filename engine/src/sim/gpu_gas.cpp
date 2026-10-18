// gpu_gas.cpp — 気体の流れの 1 刻みを GPU の Compute で走らせる(gpu_gas.h)。段を呼ぶ順は shaders/sim/gas_step.hlsl の先頭の表と同じで、
// CPU リファレンス(gas_reference.cpp の Substep)の (1)〜(5) を、面の段とセルの段に分けて並べたもの。段の間は全部の UAV の書き込みを待つ。
// 小刻みの数は設定で決まる(1 刻み = substeps × 9 段)。反復の回数が決まっているので Work Graph にしても結果は同じ(D-302。T-0208 で測る)。
#include "sim/gpu_gas.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "common/gas_gpu.hlsli"
#include "gpu/resources.h"
#include "sim/gpu_gas_internal.h"

namespace bicameral::sim {

    namespace {

        constexpr uint32_t THREADS_PER_GROUP = 64;  // gas_step.hlsl の numthreads
        constexpr uint32_t ROOT_CONSTANT_COUNT = 4;
        constexpr uint32_t MAX_GROUPS = 65535;  // Dispatch の 1 次元の上限

        // u0〜u5 / b0 / デバッグのリング / t0〜t3
        constexpr gpu::RootSignatureLayout ROOT_LAYOUT{
            .uavCount = 6, .rootConstantCount = ROOT_CONSTANT_COUNT, .debugRing = true, .srvCount = 4};

        // shaders/CMakeLists.txt で入口ごとに作る .cso(Pass の順)
        constexpr std::array<const char*, 11> SHADER_NAMES = {
            "sim/gas_derive.cso",   "sim/gas_face_force.cso", "sim/gas_apply_force.cso",    "sim/gas_face_flow.cso",
            "sim/gas_outflow.cso",  "sim/gas_face_moved.cso", "sim/gas_moved_sum.cso",      "sim/gas_face_final.cso",
            "sim/gas_transfer.cso", "sim/gas_viscosity.cso",  "sim/gas_viscosity_apply.cso"};

        // 面の段か(面の番号 = セル × 6 ごとに 1 スレッド)
        constexpr std::array<bool, 11> FACE_PASS = {false, true, false, true,  false, true,
                                                    false, true, false, false, false};

        static_assert(sizeof(GasGpuCell) == GAS_GPU_CELL_BYTES);
        static_assert(sizeof(GasGpuDerived) == 48);
        static_assert(sizeof(GasGpuFace) == GAS_GPU_FACE_BYTES);
        static_assert(sizeof(GasGpuCellSums) == GAS_GPU_SUMS_BYTES);
        static_assert(sizeof(GasGpuParameters) == GAS_GPU_PARAMETERS_BYTES);
        static_assert(offsetof(GasGpuParameters, openness) == GAS_GPU_PARAMETERS_OPENNESS);
        static_assert(GAS_GPU_OPEN_FULL == GAS_OPEN_FULL);
        static_assert(sizeof(GasGpuSpecies) == GAS_GPU_SPECIES_BYTES);
        static_assert((uint32_t)GasBoundary::Wall == GAS_GPU_BOUNDARY_WALL &&
                      (uint32_t)GasBoundary::Periodic == GAS_GPU_BOUNDARY_PERIODIC &&
                      (uint32_t)GasBoundary::Open == GAS_GPU_BOUNDARY_OPEN);
        static_assert((uint32_t)GasReconstruction::Upwind == GAS_GPU_RECONSTRUCTION_UPWIND &&
                      (uint32_t)GasReconstruction::Minmod == GAS_GPU_RECONSTRUCTION_MINMOD &&
                      (uint32_t)GasReconstruction::MonotonizedCentral == GAS_GPU_RECONSTRUCTION_MC);
        static_assert((uint32_t)GasThermodynamics::Isothermal == GAS_GPU_THERMODYNAMICS_ISOTHERMAL &&
                      (uint32_t)GasThermodynamics::Adiabatic == GAS_GPU_THERMODYNAMICS_ADIABATIC);

        ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, std::span<const std::byte> bytes) {
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

        template <typename T>
        void WriteRecord(std::span<std::byte> bytes, size_t offset, const T& value) {
            std::memcpy(bytes.data() + offset, &value, sizeof(T));
        }

        template <typename T>
        T ReadRecord(std::span<const std::byte> bytes, size_t offset) {
            T value;
            std::memcpy(&value, bytes.data() + offset, sizeof(T));
            return value;
        }

        // 帳簿(頭 + 物質量 × 欄の数。gas_gpu.hlsli の GAS_GPU_LEDGER_*)
        std::vector<std::byte> PackLedger(const GasLedger& ledger, uint32_t speciesStride) {
            std::vector<std::byte> bytes(GasGpuLedgerBytes(speciesStride));
            WriteRecord(std::span(bytes), GAS_GPU_LEDGER_ENERGY, ledger.energy);
            WriteRecord(std::span(bytes), GAS_GPU_LEDGER_MOMENTUM, ledger.momentum);
            for (uint32_t s = 0; s < speciesStride; ++s)
                WriteRecord(std::span(bytes), GAS_GPU_LEDGER_AMOUNTS + (s * 8), ledger.amounts[s]);

            return bytes;
        }

    }  // namespace

    // --- CPU の形と GPU の形の写し(gpu_gas_internal.h。木の GPU〔gpu_gas_tree.cpp〕も使う)---

    uint32_t GasGpuSpeciesStride(uint32_t speciesCount, uint32_t reserved) {
        return std::max({1u, speciesCount, reserved});
    }

    void PackGasGpuCell(const GasCell& cell, uint32_t speciesStride, std::span<std::byte> record) {
        const GasGpuCell head{.energy = cell.energy,
                              .momentum = {cell.momentum[0], cell.momentum[1], cell.momentum[2]}};
        WriteRecord(record, 0, head);
        for (uint32_t s = 0; s < speciesStride; ++s)
            WriteRecord(record, GAS_GPU_CELL_BYTES + (s * 8), cell.amounts[s]);
    }

    std::vector<std::byte> PackGasGpuCellRecords(std::span<const GasCell> cells, uint32_t speciesStride) {
        const size_t stride = GasGpuCellStride(speciesStride);
        std::vector<std::byte> bytes(cells.size() * stride);
        for (size_t i = 0; i < cells.size(); ++i)
            PackGasGpuCell(cells[i], speciesStride, std::span(bytes).subspan(i * stride, stride));

        return bytes;
    }

    std::vector<std::byte> PackGasGpuCellPlanes(std::span<const GasCell> cells, uint32_t speciesStride) {
        const auto count = (uint32_t)cells.size();
        std::vector<std::byte> bytes((size_t)count * GasGpuCellStride(speciesStride));
        for (uint32_t i = 0; i < count; ++i) {
            const GasCell& cell = cells[i];
            const GasGpuCell head{.energy = cell.energy,
                                  .momentum = {cell.momentum[0], cell.momentum[1], cell.momentum[2]}};
            WriteRecord(std::span(bytes), (size_t)i * GAS_GPU_CELL_BYTES, head);
            for (uint32_t s = 0; s < speciesStride; ++s)
                WriteRecord(std::span(bytes), GasGpuPlaneAddress(GAS_GPU_CELL_BYTES, count, i, s), cell.amounts[s]);
        }

        return bytes;
    }

    std::vector<GasCell> UnpackGasGpuCellPlanes(std::span<const std::byte> bytes, uint32_t cellCount,
                                                uint32_t speciesCount) {
        std::vector<GasCell> cells(cellCount);
        for (uint32_t i = 0; i < cellCount; ++i) {
            const auto head = ReadRecord<GasGpuCell>(bytes, (size_t)i * GAS_GPU_CELL_BYTES);
            GasCell& cell = cells[i];
            for (uint32_t s = 0; s < speciesCount; ++s)
                cell.amounts[s] = ReadRecord<uint64_t>(bytes, GasGpuPlaneAddress(GAS_GPU_CELL_BYTES, cellCount, i, s));

            cell.energy = head.energy;
            for (uint32_t axis = 0; axis < GAS_AXES; ++axis)
                cell.momentum[axis] = head.momentum[axis];
        }

        return cells;
    }

    GasGpuDerived ToGasGpuDerived(const GasDerived& derived) {
        return {.mass = derived.mass,
                .inverseMass = derived.inverseMass,
                .inverseAmount = derived.inverseAmount,
                .temperature = derived.temperature,
                .unused = 0,
                .pressure = derived.pressure,
                .scaledPressure = derived.scaledPressure};
    }

    GasGpuParameters MakeGasGpuParameters(const GasConfig& config, const GasCoefficients& coefficients) {
        GasGpuParameters parameters{};
        parameters.centralFlow = coefficients.centralFlow;
        parameters.pressureFlow = coefficients.pressureFlow;
        parameters.pressureImpulse = coefficients.pressureImpulse;
        parameters.gravityImpulse = coefficients.gravityImpulse;
        parameters.hydrostatic = coefficients.hydrostatic;
        parameters.gravityWork = coefficients.gravityWork;
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
        parameters.thermodynamics = (uint32_t)config.thermodynamics;
        parameters.eddyViscosity = config.eddyViscosityMm2PerS;

        return parameters;
    }

    GasGpuSpecies MakeGasGpuSpecies(const GasConfig& config, const GasCoefficients& coefficients, uint32_t slot) {
        const uint32_t key = config.species[slot].roundingKey;

        return {.massPerAmount = coefficients.massPerAmount[slot],
                .formationEnergy = config.species[slot].formationEnergy,
                .heatCapacity = config.species[slot].heatCapacity,
                .roundingKey = key == GAS_ROUNDING_KEY_SLOT ? slot : key,  // 使わない欄も欄の番号(読まれない)
                .viscosity = config.species[slot].viscosityMicroPascalSeconds,
                .unused = 0};
    }

    std::vector<std::byte> PackGasGpuParameters(const GasConfig& config, const GasCoefficients& coefficients,
                                                uint32_t speciesStride) {
        std::vector<std::byte> bytes(GasGpuParametersBytes(speciesStride));
        WriteRecord(std::span(bytes), 0, MakeGasGpuParameters(config, coefficients));
        for (uint32_t s = 0; s < speciesStride; ++s) {
            WriteRecord(std::span(bytes), GAS_GPU_PARAMETERS_BYTES + (s * GAS_GPU_SPECIES_BYTES),
                        MakeGasGpuSpecies(config, coefficients, s));
        }

        return bytes;
    }

    std::expected<GpuGas, std::string> GpuGas::Create(ID3D12Device5* device, const GasBox& box,
                                                      uint32_t reservedSpecies) {
        GpuGas result;
        result.m_cellCount = (uint32_t)box.cells.size();
        result.m_speciesCount = box.config.speciesCount;
        result.m_speciesStride = GasGpuSpeciesStride(box.config.speciesCount, reservedSpecies);
        result.m_substeps = box.config.substeps;
        result.m_tick = box.tick;
        if (GroupsFor((uint64_t)result.m_cellCount * GAS_GPU_FACES_PER_CELL) > MAX_GROUPS)
            return std::unexpected("気体の箱が大きすぎる(面の段の Dispatch の上限)");

        // 気体の粘り(静まる性質。T-0758・D-516)がある箱だけ、刻みの終わりに粘りの 2 段を投げる(T-0775)
        result.m_viscous = GasHasViscosity(box.config);
        if (result.m_viscous && box.config.cellLevel != 0)
            return std::unexpected(
                "GPU の気体の粘りの段はレベル 0 の箱だけ(CPU の LevelViscosity は GPU に無い。T-0765・T-0775)");

        // 成分の数に上限は無い(1 件 = 頭 + 欄 × m_speciesStride。T-0598)。丸めの鍵は設定のまま(欄の番号か物質 ID。T-0592)。
        // 件の大きさが 32bit の番地に収まること(ByteAddressBuffer の番地)
        const uint64_t faceBytes = (uint64_t)result.m_cellCount * GAS_GPU_FACES_PER_CELL *
                                   GasGpuFaceStride(result.m_speciesStride);
        if (faceBytes > UINT32_MAX)
            return std::unexpected("気体の箱が大きすぎる(面の件の番地が 32bit を超える)");

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

    // t0〜t3(設定・層ごとの基準の導く値・開いた境界の外・セルの空き)の既定のヒープの写し。写すのは RecordResidentUploads
    std::array<gpu::ResidentBuffer, 4> GpuGas::CreateLayerBuffers(ID3D12Device* device, const GasBox& box,
                                                                  uint32_t reservedSpecies) {
        const uint32_t speciesStride = GasGpuSpeciesStride(box.config.speciesCount, reservedSpecies);
        std::vector<std::byte> parameters = PackGasGpuParameters(box.config, box.coefficients, speciesStride);

        // セルの空き(T-0617。空 = 読まない。読まない時も t3 に結ぶ小さい表を置く)
        const uint32_t openness = box.openQ16.empty() ? 0u : 1u;
        WriteRecord(std::span(parameters), GAS_GPU_PARAMETERS_OPENNESS, openness);
        const std::vector<uint32_t> open = box.openQ16.empty() ? std::vector<uint32_t>(1, GAS_OPEN_FULL) : box.openQ16;
        std::vector<GasGpuDerived> referenceDerived;
        referenceDerived.reserve(box.referenceDerived.size());
        for (const GasDerived& layer : box.referenceDerived)
            referenceDerived.push_back(ToGasGpuDerived(layer));

        const std::vector<std::byte> ghost = PackGasGpuCellRecords(box.ghost, speciesStride);

        return {gpu::CreateResidentBuffer(device, parameters),
                gpu::CreateResidentBuffer(device, std::as_bytes(std::span<const GasGpuDerived>(referenceDerived))),
                gpu::CreateResidentBuffer(device, ghost),
                gpu::CreateResidentBuffer(device, std::as_bytes(std::span<const uint32_t>(open)))};
    }

    // 環境の値のホットリロード(T-0444): 新しい t0〜t3 を作って写し、前のものは呼ぶ側に持たせる(記録済みの刻みがまだ読む)。
    // 新しいバッファは作った直後で COMMON なので、同じリストで前の刻みが読んだ後でも写せる(RecordResidentUploads の約束)
    bool GpuGas::RecordEnvironment(ID3D12GraphicsCommandList10* list, const GasBox& box,
                                   std::vector<ComPtr<ID3D12Resource>>& keepAlive) {
        if ((uint32_t)box.cells.size() != m_cellCount || box.config.substeps != m_substeps ||
            GasGpuSpeciesStride(box.config.speciesCount, m_speciesStride) != m_speciesStride ||
            (GasHasViscosity(box.config) && box.config.cellLevel != 0))  // 粘りの段はレベル 0 だけ(Create と同じ)
            return false;

        ComPtr<ID3D12Device> device;
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
            return false;

        std::array<gpu::ResidentBuffer, 4> buffers = CreateLayerBuffers(device.Get(), box, m_speciesStride);
        if (!std::ranges::all_of(buffers, &gpu::ResidentBuffer::Valid))
            return false;

        gpu::RecordResidentUploads(list, buffers);
        for (gpu::ResidentBuffer& old : m_srvs) {
            keepAlive.push_back(std::move(old.buffer));
            keepAlive.push_back(std::move(old.upload));
        }

        for (const gpu::ResidentBuffer& buffer : buffers)
            keepAlive.push_back(buffer.upload);

        m_srvs = std::move(buffers);
        m_viscous = GasHasViscosity(box.config);

        return true;
    }

    // バッファ(gas_step.hlsl の結び付けの順)・初めの箱の写し・読み戻し
    std::expected<void, std::string> GpuGas::CreateBuffers(ID3D12Device5* device, const GasBox& box) {
        const uint64_t n = m_cellCount;
        const uint32_t stride = m_speciesStride;
        const std::array<uint64_t, 6> uavBytes = {n * GasGpuCellStride(stride),  // u0 セル
                                                  n * GasGpuCellStride(stride),  // u1 力を当てた後のセル
                                                  n * sizeof(GasGpuDerived),     // u2 導く値
                                                  n * GAS_GPU_FACES_PER_CELL * GasGpuFaceStride(stride),  // u3 面
                                                  n * GasGpuSumsStride(stride),  // u4 セルごとの合計
                                                  GasGpuLedgerBytes(stride)};    // u5 帳簿
        for (size_t i = 0; i < uavBytes.size(); ++i) {
            m_uavs[i] = gpu::CreateBuffer(device, uavBytes[i], gpu::BufferKind::UnorderedAccess);
            if (!m_uavs[i])
                return std::unexpected("気体のバッファを作れない");
        }

        // --- t0〜t3(設定・層ごとの基準と外・セルの空き)---
        m_srvs = CreateLayerBuffers(device, box, stride);

        // --- 初めの箱 ---
        m_cellUpload = CreateUploadBuffer(device, PackGasGpuCellPlanes(box.cells, stride));
        m_ledgerUpload = CreateUploadBuffer(device, PackLedger(box.ledger, stride));
        m_cellReadback = gpu::CreateBuffer(device, uavBytes[0], gpu::BufferKind::Readback);
        m_ledgerReadback = gpu::CreateBuffer(device, uavBytes[5], gpu::BufferKind::Readback);
        m_faceReadback = gpu::CreateBuffer(device, uavBytes[3], gpu::BufferKind::Readback);
        m_sumReadback = gpu::CreateBuffer(device, uavBytes[4], gpu::BufferKind::Readback);
        if (!std::ranges::all_of(m_srvs, &gpu::ResidentBuffer::Valid) || !m_cellUpload || !m_ledgerUpload ||
            !m_cellReadback || !m_ledgerReadback || !m_faceReadback || !m_sumReadback)
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
        gpu::RecordResidentUploads(list, m_srvs);
    }

    // 1 つの段を投げ、全部の UAV の書き込みを次の段より前に終わらせる
    void GpuGas::RecordPass(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, Pass pass,
                            const Constants& constants, const std::array<D3D12_GPU_VIRTUAL_ADDRESS, 4>& srvs) const {
        static_assert(sizeof(Constants) == ROOT_CONSTANT_COUNT * sizeof(uint32_t));
        list->SetComputeRootSignature(m_rootSignature.Get());
        list->SetPipelineState(m_pipelines[(size_t)pass].Get());
        for (uint32_t i = 0; i < m_uavs.size(); ++i)
            list->SetComputeRootUnorderedAccessView(i, m_uavs[i]->GetGPUVirtualAddress());

        list->SetComputeRoot32BitConstants(ROOT_LAYOUT.RootConstantIndex(), ROOT_CONSTANT_COUNT, &constants, 0);
        list->SetComputeRootUnorderedAccessView(ROOT_LAYOUT.DebugRingIndex(), debugRing);
        for (uint32_t i = 0; i < srvs.size(); ++i)
            list->SetComputeRootShaderResourceView(ROOT_LAYOUT.SrvIndex(i), srvs[i]);

        const uint64_t threads = FACE_PASS[(size_t)pass] ? (uint64_t)m_cellCount * GAS_GPU_FACES_PER_CELL : m_cellCount;
        list->Dispatch(GroupsFor(threads), 1, 1);
        const D3D12_RESOURCE_BARRIER barrier = gpu::UavBarrier(nullptr);
        list->ResourceBarrier(1, &barrier);
    }

    std::array<D3D12_GPU_VIRTUAL_ADDRESS, 4> GpuGas::OwnSrvs() const {
        return {m_srvs[0].buffer->GetGPUVirtualAddress(), m_srvs[1].buffer->GetGPUVirtualAddress(),
                m_srvs[2].buffer->GetGPUVirtualAddress(), m_srvs[3].buffer->GetGPUVirtualAddress()};
    }

    // 刻み tick の小刻み × 9 段(乱数の丸めの小刻みの通し番号は gas_reference.cpp の StepGas と同じ)。
    // viscous なら小刻みの後に粘りの 2 段(gas_reference.cpp の ApplyViscosity。T-0775)
    void GpuGas::RecordSubsteps(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint64_t tick,
                                const std::array<D3D12_GPU_VIRTUAL_ADDRESS, 4>& srvs, bool viscous) const {
        Constants constants{.cellCount = m_cellCount, .speciesStride = m_speciesStride};
        for (uint32_t substep = 0; substep < m_substeps; ++substep) {
            const uint64_t roundingTick = (tick * m_substeps) + substep;
            constants.roundingTickLow = (uint32_t)roundingTick;
            constants.roundingTickHigh = (uint32_t)(roundingTick >> 32);
            for (size_t pass = 0; pass < std::min((size_t)m_debugPassLimit, SUBSTEP_PASS_COUNT); ++pass)
                RecordPass(list, debugRing, (Pass)pass, constants, srvs);
        }

        if (!viscous || m_debugPassLimit < SUBSTEP_PASS_COUNT)
            return;

        RecordPass(list, debugRing, Pass::Viscosity, constants, srvs);
        RecordPass(list, debugRing, Pass::ViscosityApply, constants, srvs);
    }

    void GpuGas::RecordStep(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing) {
        RecordSubsteps(list, debugRing, m_tick, OwnSrvs(), m_viscous);
        ++m_tick;
    }

    void GpuGas::RecordStepAt(ID3D12GraphicsCommandList10* list, D3D12_GPU_VIRTUAL_ADDRESS debugRing, uint64_t tick,
                              D3D12_GPU_VIRTUAL_ADDRESS parameters, D3D12_GPU_VIRTUAL_ADDRESS ghost,
                              D3D12_GPU_VIRTUAL_ADDRESS open, bool viscous) const {
        RecordSubsteps(list, debugRing, tick, {parameters, m_srvs[1].buffer->GetGPUVirtualAddress(), ghost, open},
                       viscous);
    }

    void GpuGas::RecordReadback(ID3D12GraphicsCommandList10* list) const {
        gpu::RecordCopyToReadback(list, m_uavs[0].Get(), m_cellReadback.Get());
        gpu::RecordCopyToReadback(list, m_uavs[5].Get(), m_ledgerReadback.Get());
    }

    bool GpuGas::Read(std::vector<GasCell>& cells, GasLedger& ledger) const {
        const size_t stride = GasGpuCellStride(m_speciesStride);
        std::vector<std::byte> cellBytes((size_t)m_cellCount * stride);
        std::vector<std::byte> ledgerBytes(GasGpuLedgerBytes(m_speciesStride));
        if (!gpu::ReadBuffer(m_cellReadback.Get(), cellBytes) || !gpu::ReadBuffer(m_ledgerReadback.Get(), ledgerBytes))
            return false;

        ledger = {};
        for (uint32_t s = 0; s < m_speciesCount; ++s)
            ledger.amounts[s] = ReadRecord<int64_t>(ledgerBytes, GAS_GPU_LEDGER_AMOUNTS + (s * 8));

        ledger.energy = ReadRecord<int64_t>(ledgerBytes, GAS_GPU_LEDGER_ENERGY);
        ledger.momentum = ReadRecord<std::array<int64_t, GAS_AXES>>(ledgerBytes, GAS_GPU_LEDGER_MOMENTUM);
        cells = UnpackGasGpuCellPlanes(cellBytes, m_cellCount, m_speciesCount);

        return true;
    }

    // --- 調べる用 ---

    void GpuGas::RecordDebugReadback(ID3D12GraphicsCommandList10* list) const {
        gpu::RecordCopyToReadback(list, m_uavs[3].Get(), m_faceReadback.Get());
        gpu::RecordCopyToReadback(list, m_uavs[4].Get(), m_sumReadback.Get());
    }

    bool GpuGas::ReadDebug(std::vector<uint64_t>& faceWords, std::vector<uint64_t>& sumWords) const {
        faceWords.assign(
            (size_t)m_cellCount * GAS_GPU_FACES_PER_CELL * GasGpuFaceStride(m_speciesStride) / sizeof(uint64_t), 0);
        sumWords.assign((size_t)m_cellCount * GasGpuSumsStride(m_speciesStride) / sizeof(uint64_t), 0);

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
