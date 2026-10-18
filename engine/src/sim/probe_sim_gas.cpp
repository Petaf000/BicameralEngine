// probe_sim_gas.cpp — 仮の世界(ProbeSim)の GPU の刻みに気体の区画を載せる(T-0592・ADR-0256)。
//
// CPU リファレンスは sim/probe_gas.cpp の ProbeGasField::Step(ProbeReference::Advance の (1b))。GPU は適用の単位の中
// (コマンド → 物理の熱 → 表の作り直し → **気体** → 起こす刻みの来たブロックを一覧へ)で、shaders/sim/probe_gas_tick.hlsl の段と
// 気体の箱の 1 刻み(sim/gpu_gas の gas_step.hlsl)を投げる。段の中身と並びは shaders/common/probe_gas_gpu.hlsli。
// 作る時に CPU が決めるのは、区画の形・基準の大気だけの静かな箱(層の基準の導く値・開いた境界の外・設定の型)・物質ごとの値だけ。
// 刻みごとの欄の物質(区画にある気体の物質。ID の順)と、それに合う設定は GPU が決める(D-107)。
// 区画の気体の物質の数に上限は無い(気体の箱の件の欄の数 = 表の気体の物質の数。T-0598)。
// 保存点・写しの早送りは気体の欄(gasState)と作業場(gasWork。帳簿を含む)を写し、状態のハッシュは GasHash が気体の欄と帳簿を足す(T-0620)。
// 今の制限(分けたチケット): 陽解法だけ(投影は T-0621)・表の差し替えは区画の欄の作りが変わらない表だけ(付け替えと作り直しは T-0670)。
#include <algorithm>
#include <array>
#include <cstring>
#include <format>

#include "core/aliases.h"
#include "core/log.h"
#include "gpu/resources.h"
#include "sim/gas_cells.h"
#include "sim/gpu_gas_internal.h"
#include "sim/probe_sim.h"

namespace bicameral::sim {

    namespace {

        // shaders/CMakeLists.txt で入口ごとに作る .cso(呼ぶ順。m_gasPipelines の並び)
        constexpr std::array<const char*, 8> GAS_SHADER_NAMES = {
            "sim/probe_gas_begin.cso",  "sim/probe_gas_collect.cso", "sim/probe_gas_prepare.cso",
            "sim/probe_gas_gather.cso", "sim/probe_gas_check.cso",   "sim/probe_gas_scatter.cso",
            "sim/probe_gas_finish.cso", "sim/probe_gas_hash.cso"};

        enum GasPass : uint8_t { Begin, Collect, Prepare, Gather, Check, Scatter, Finish, Hash };

        constexpr uint32_t MICRO = 1000000;

        static_assert(sizeof(ProbeGasCellState) == PROBE_GAS_CELL_STATE_BYTES);
        static_assert(sizeof(ProbeGasSpecies) == PROBE_GAS_SPECIES_BYTES);
        static_assert(sizeof(ProbeGasHeader) == 96);
        static_assert(PROBE_GAS_HEADER_OFFSET - PROBE_GAS_TEMPLATE_OFFSET >= sizeof(GasGpuParameters));
        static_assert(PROBE_GAS_DYNAMIC_OFFSET - PROBE_GAS_HEADER_OFFSET >= sizeof(ProbeGasHeader));

        uint32_t AlignUp(uint32_t value) {
            return (value + PROBE_GAS_ALIGNMENT - 1) / PROBE_GAS_ALIGNMENT * PROBE_GAS_ALIGNMENT;
        }

        uint32_t GroupsFor(uint32_t threads) {
            return std::max(1u, (threads + PROBE_LINEAR_GROUP_SIZE - 1) / PROBE_LINEAR_GROUP_SIZE);
        }

        template <typename T>
        void WriteAt(std::vector<std::byte>& bytes, uint32_t offset, const T& value) {
            std::memcpy(bytes.data() + offset, &value, sizeof(T));
        }

        bool IsCondensed(const BakedReactionTable& table, uint32_t id) {
            return id < table.speciesCondensed.size() && table.speciesCondensed[id] != 0;
        }

        // 気体の箱の件の欄の数 = 区画に現れうる気体の物質の数(表の凝縮相でない物質と基準の大気。T-0598)
        uint32_t SlotCapacity(const BakedReactionTable& table, std::span<const uint32_t> atmosphere) {
            uint32_t capacity = 0;
            for (uint32_t id = 0; id < table.species.size(); ++id) {
                if (!IsCondensed(table, id) || rng::contains(atmosphere, id))
                    ++capacity;
            }

            return std::max(1u, capacity);
        }

        // 見出しの場所を決める(区画の形と表の物質の数から)
        ProbeGasHeader MakeHeader(const ProbeGasOptions& options, const BakedReactionTable& table,
                                  std::span<const uint32_t> atmosphere, uint32_t& totalBytes) {
            ProbeGasHeader header{};
            for (uint32_t axis = 0; axis < 3; ++axis) {
                header.regionMin[axis] = options.minCell[axis];
                header.regionSize[axis] = options.sizeCells[axis];
            }

            header.regionCellCount = options.sizeCells[0] * options.sizeCells[1] * options.sizeCells[2];
            header.layerCount = options.sizeCells[1];
            header.speciesCount = (uint32_t)table.species.size();
            header.maskWords = std::max(1u, (header.speciesCount + 31) / 32);
            header.atmosphereCount = (uint32_t)atmosphere.size();
            header.slotCapacity = SlotCapacity(table, atmosphere);

            uint32_t offset = PROBE_GAS_DYNAMIC_OFFSET;
            const auto place = [&offset](uint32_t bytes) {
                const uint32_t at = offset;
                offset = AlignUp(offset + bytes);
                return at;
            };
            const uint32_t cellStride = GasGpuCellStride(header.slotCapacity);
            header.parametersOffset = place(GasGpuParametersBytes(header.slotCapacity));
            header.ghostOffset = place(header.layerCount * cellStride);
            header.ghostAtmosphereOffset = place(header.layerCount * cellStride);
            header.speciesOffset = place(header.speciesCount * PROBE_GAS_SPECIES_BYTES);
            header.maskOffset = place(header.maskWords * 4);
            header.slotsOffset = place(header.slotCapacity * 4);
            header.slotOfOffset = place(header.speciesCount * 4);
            header.atmosphereOffset = place(std::max(1u, header.atmosphereCount) * 4);
            header.ledgerOffset = place((header.speciesCount + 4) * 8);
            header.condensedOffset = place(header.regionCellCount * 8);
            header.openOffset = place(header.regionCellCount * 4);
            totalBytes = offset;

            return header;
        }

        // 物質ごとの気体の値(probe_gas.cpp の MakeConfig・gas_reference.cpp の MakeGasCoefficients と同じ式)
        ProbeGasSpecies MakeGasSpecies(const BakedReactionTable& table, uint32_t id) {
            const bool condensed = IsCondensed(table, id);
            const bool liquid = id < table.speciesLiquid.size() && table.speciesLiquid[id] != 0;
            const uint64_t molarVolume = condensed && id < table.speciesMolarVolume.size()
                                             ? table.speciesMolarVolume[id]
                                             : 0;

            return {.massPerAmount = ((uint64_t)table.molarMasses[id] << 32) / MICRO,
                    .formationEnergy = table.species[id].h0,
                    .molarVolume = molarVolume,
                    .heatCapacity = table.species[id].heatCapacity,
                    .isGas = condensed ? 0u : 1u,
                    .isLiquid = liquid ? 1u : 0u,
                    .unused = 0};
        }

    }  // namespace

    // 区画を作る(区画が無ければ何もしない)。断る: 投影・ProbeGasField が断るもの
    std::expected<void, std::string> ProbeSim::CreateGas(ID3D12Device5* device, const BakedReactionTable& table) {
        const ProbeGasOptions& options = m_options.gas;
        if (!options.Enabled())
            return {};

        if (options.pressure != ProbeGasPressure::Explicit)
            return std::unexpected("GPU の気体の区画は陽解法だけ(投影は T-0621)");

        // 気体の粘り(T-0758・D-516)は GPU の段に無い(区画の物質は刻みごとに変わるので、表の気体の物質の全部を見る)
        bool viscous = table.environment.gasEddyViscosityMm2PerS != 0;
        for (uint32_t id = 0; id < table.speciesTransport.size() && !viscous; ++id)
            viscous = IsGasSpecies(table, id) &&
                      table.speciesTransport[id].viscosityMicroPascalSeconds.value_or(0) != 0;

        if (viscous)
            return std::unexpected(
                "GPU の気体はまだ粘り(gas_eddy_viscosity_mm2_per_s・気体の viscosity_upa_s)を持たない(CPU "
                "だけ。T-0758)");

        auto field = ProbeGasField::Create(table, options);
        if (!field)
            return std::unexpected("気体の区画を作れない: " + field.error());

        const std::span<const uint32_t> atmosphere = field->AtmosphereSpecies();
        uint32_t totalBytes = 0;
        m_gasHeader = MakeHeader(options, table, atmosphere, totalBytes);

        // --- 気体の箱(基準の大気だけの設定。件の欄は表の気体の物質の数だけ取る。層の基準の導く値は欄の並びによらない)---
        const GasBox box = field->MakeAtmosphereBox(table);
        auto gas = GpuGas::Create(device, box, m_gasHeader.slotCapacity);
        if (!gas)
            return std::unexpected("気体の箱を GPU に作れない: " + gas.error());

        m_gas = std::make_unique<GpuGas>(std::move(*gas));

        // --- 作業場の初めの中身(設定の型・見出し・開いた境界の外の元・物質ごとの値・基準の大気の物質。ほかは 0)---
        std::vector<std::byte> bytes(totalBytes);
        WriteAt(bytes, PROBE_GAS_TEMPLATE_OFFSET, MakeGasGpuParameters(box.config, box.coefficients));
        WriteAt(bytes, PROBE_GAS_HEADER_OFFSET, m_gasHeader);
        const uint32_t cellStride = GasGpuCellStride(m_gasHeader.slotCapacity);
        for (uint32_t layer = 0; layer < m_gasHeader.layerCount; ++layer) {
            PackGasGpuCell(
                box.ghost[layer], m_gasHeader.slotCapacity,
                std::span(bytes).subspan(m_gasHeader.ghostAtmosphereOffset + (layer * cellStride), cellStride));
        }

        for (uint32_t a = 0; a < atmosphere.size(); ++a)
            WriteAt(bytes, m_gasHeader.atmosphereOffset + (a * 4), atmosphere[a]);

        for (uint32_t id = 0; id < m_gasHeader.speciesCount; ++id)
            WriteAt(bytes, m_gasHeader.speciesOffset + (id * PROBE_GAS_SPECIES_BYTES), MakeGasSpecies(table, id));

        m_gasUpload = gpu::CreateBuffer(device, totalBytes, gpu::BufferKind::Upload);
        void* mapped = nullptr;
        const D3D12_RANGE noRead{.Begin = 0, .End = 0};
        if (!m_gasUpload || FAILED(m_gasUpload->Map(0, &noRead, &mapped)))
            return std::unexpected("気体の作業場のアップロードを作れない");

        std::memcpy(mapped, bytes.data(), bytes.size());
        m_gasUpload->Unmap(0, nullptr);

        // 気体の欄は既定のヒープの 0(静止)で始める
        m_gasWork = gpu::CreateBuffer(device, totalBytes, gpu::BufferKind::UnorderedAccess);
        m_gasState = gpu::CreateBuffer(device, (uint64_t)m_gasHeader.regionCellCount * PROBE_GAS_CELL_STATE_BYTES,
                                       gpu::BufferKind::UnorderedAccess);
        if (!m_gasWork || !m_gasState)
            return std::unexpected("気体のバッファを作れない");

        m_gasWork->SetName(L"ProbeSim.gasWork");
        m_gasState->SetName(L"ProbeSim.gasState");

        // --- 段 ---
        for (size_t pass = 0; pass < GAS_SHADER_NAMES.size(); ++pass) {
            const auto bytecode = gpu::LoadShader(GAS_SHADER_NAMES[pass]);
            if (!bytecode)
                return std::unexpected(bytecode.error());

            m_gasPipelines[pass] = gpu::CreateComputePipeline(device, m_rootSignature.Get(), *bytecode);
            if (!m_gasPipelines[pass])
                return std::unexpected(std::format("パイプラインを作れない: {}", GAS_SHADER_NAMES[pass]));
        }

        Log(Channel::Sim, Level::Info,
            "気体の区画: ({}, {}, {}) から {} × {} × {} セル・基準の大気の物質 {} つ・欄 {} 個", options.minCell[0],
            options.minCell[1], options.minCell[2], options.sizeCells[0], options.sizeCells[1], options.sizeCells[2],
            atmosphere.size(), m_gasHeader.slotCapacity);

        return {};
    }

    // 最初のフレーム: 作業場の初めの中身と、気体の箱の層の基準(t1)を写す
    void ProbeSim::RecordGasInitialization(ID3D12GraphicsCommandList10* list) const {
        if (!m_gas)
            return;

        list->CopyResource(m_gasWork.Get(), m_gasUpload.Get());
        const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(m_gasWork.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);
        m_gas->RecordUpload(list);
    }

    // 表の差し替え(T-0620・ADR-0300): 作業場の設定の型・層の基準・開いた境界の外・物質ごとの値は作る時に表から作ったので、
    // それが変わらない表だけ通す(変わる表の作り直しは T-0670)。物質の一覧が同じことは CheckRegionTableSwap が見てある
    std::expected<void, std::string> ProbeSim::CheckGasTableSwap(const BakedReactionTable& table) const {
        if (!m_gas)
            return {};

        const BakedReactionTable& current = m_tableCopy;
        if (table.environment != current.environment || table.atmosphere != current.atmosphere)
            return std::unexpected("気体の区画のある世界で、環境の値(重力・音速の基準)か基準の大気が変わる表(T-0670)");

        for (uint32_t id = 0; id < m_gasHeader.speciesCount; ++id) {
            const ProbeGasSpecies before = MakeGasSpecies(current, id);
            const ProbeGasSpecies after = MakeGasSpecies(table, id);
            if (std::memcmp(&before, &after, sizeof(ProbeGasSpecies)) != 0) {
                return std::unexpected(std::format(
                    "気体の区画のある世界で、物質 {} の気体の値(モル質量・h0・比熱・凝縮相)が変わる表(T-0670)",
                    table.speciesNames[id]));
            }
        }

        return {};
    }

    // 区画 → 気体の箱(ProbeSim のルート署名と単位の定数が結んである所で)
    void ProbeSim::RecordGasGather(ID3D12GraphicsCommandList10* list) const {
        const D3D12_RESOURCE_BARRIER allUavs = gpu::UavBarrier(nullptr);
        const uint32_t regionGroups = GroupsFor(m_gasHeader.regionCellCount);
        const std::array<uint32_t, 4> groups = {GroupsFor(m_gasHeader.maskWords), regionGroups, 1, regionGroups};
        for (uint32_t pass = GasPass::Begin; pass <= GasPass::Gather; ++pass) {
            list->ResourceBarrier(1, &allUavs);
            list->SetPipelineState(m_gasPipelines[pass].Get());
            list->Dispatch(groups[pass], 1, 1);
        }

        list->ResourceBarrier(1, &allUavs);
    }

    // 気体の箱の 1 刻み(GpuGas のルート署名。設定・開いた境界の外・セルの空きは作業場から読むので、その間だけ読む状態にする)
    void ProbeSim::RecordGasStep(ID3D12GraphicsCommandList10* list, uint64_t tick) const {
        const D3D12_RESOURCE_BARRIER toRead = gpu::Transition(m_gasWork.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &toRead);

        const D3D12_GPU_VIRTUAL_ADDRESS work = m_gasWork->GetGPUVirtualAddress();
        m_gas->RecordStepAt(list, m_debugRing.GpuAddress(), tick, work + m_gasHeader.parametersOffset,
                            work + m_gasHeader.ghostOffset, work + m_gasHeader.openOffset);

        const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(
            m_gasWork.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);
    }

    // 状態のハッシュの単位: HashCells と同じ欄へ気体の欄と帳簿の寄与を足す(atomic の和なので HashCells との間のバリアは要らない)
    void ProbeSim::RecordGasHash(ID3D12GraphicsCommandList10* list) const {
        list->SetPipelineState(m_gasPipelines[GasPass::Hash].Get());
        list->Dispatch(GroupsFor(m_gasHeader.regionCellCount), 1, 1);
    }

    // 気体の箱 → 区画(RecordUnit が ProbeSim のルート署名と単位の定数を結び直した後で)
    void ProbeSim::RecordGasScatter(ID3D12GraphicsCommandList10* list) const {
        const D3D12_RESOURCE_BARRIER allUavs = gpu::UavBarrier(nullptr);
        const uint32_t regionGroups = GroupsFor(m_gasHeader.regionCellCount);
        const std::array<uint32_t, 3> groups = {regionGroups, regionGroups, 1};
        for (uint32_t pass = GasPass::Check; pass <= GasPass::Finish; ++pass) {
            list->ResourceBarrier(1, &allUavs);
            list->SetPipelineState(m_gasPipelines[pass].Get());
            list->Dispatch(groups[pass - GasPass::Check], 1, 1);
        }
    }

}  // namespace bicameral::sim
