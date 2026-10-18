// probe_sim_gas.cpp — 仮の世界(ProbeSim)の GPU の刻みに気体の区画を載せる(T-0592・ADR-0256)。
//
// CPU リファレンスは sim/probe_gas.cpp の ProbeGasField::Step(ProbeReference::Advance の (1b))。GPU は適用の単位の中
// (コマンド → 物理の熱 → 表の作り直し → **気体** → 起こす刻みの来たブロックを一覧へ)で、shaders/sim/probe_gas_tick.hlsl の段と
// 気体の箱の 1 刻み(sim/gpu_gas の gas_step.hlsl)を投げる。段の中身と並びは shaders/common/probe_gas_gpu.hlsli。
// 作る時に CPU が決めるのは、区画の形・基準の大気だけの静かな箱(層の基準の導く値・開いた境界の外・設定の型)・物質ごとの値だけ。
// 刻みごとの欄の物質(区画にある気体の物質。ID の順)と、それに合う設定は GPU が決める(D-107)。
// 区画の気体の物質の数に上限は無い(気体の箱の件の欄の数 = 表の気体の物質の数。T-0598)。
// 保存点・写しの早送りは気体の欄(gasState)と作業場(gasWork。帳簿を含む)を写し、状態のハッシュは GasHash が気体の欄と帳簿を足す(T-0620)。
// 表の差し替え(T-0670): 作業場(設定の型・層の基準・物質ごとの値・欄の数と並び)は表から作るので、新しい表で作り直し、持ち越す部分
// (制御の通しの数・帳簿)を物質の名前で付け替えて写す。気体の欄は物質に依らないのでそのまま。断るのは新しい表で区画を作れない時だけ。
// 今の制限(分けたチケット): 陽解法だけ(投影は T-0621)。
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
#include "sim/species_remap.h"

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
        static_assert(sizeof(ProbeGasHeader) == 104);
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
            header.ledgerOffset = place((header.speciesCount + PROBE_GAS_LEDGER_WORDS_AFTER_SPECIES) * 8);
            header.condensedOffset = place(header.regionCellCount * 8);
            header.openOffset = place(header.regionCellCount * 4);
            header.restBeforeOffset = place(header.regionCellCount * 8);
            header.restSpeedUnits = options.restSpeedUnits;
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
            // 気体の粘り(T-0758。gas_cells.cpp の GasAtmosphereLayers::MakeConfig と同じ値)
            const uint64_t viscosity = id < table.speciesTransport.size()
                                           ? table.speciesTransport[id].viscosityMicroPascalSeconds.value_or(0)
                                           : 0;

            return {.massPerAmount = ((uint64_t)table.molarMasses[id] << 32) / MICRO,
                    .formationEnergy = table.species[id].h0,
                    .molarVolume = molarVolume,
                    .heatCapacity = table.species[id].heatCapacity,
                    .isGas = condensed ? 0u : 1u,
                    .isLiquid = liquid ? 1u : 0u,
                    .viscosity = (uint32_t)std::min<uint64_t>(viscosity, gas::GF_SPECIES_VISCOSITY_LIMIT)};
        }

        // 帳簿の物質ごとの量の写し方(今の ID → 新しい ID が続く並びをまとめる。消えた物質〔新しい ID が 0〕は写さない)
        struct LedgerRun {
            uint32_t oldId = 0;
            uint32_t newId = 0;
            uint32_t count = 0;
        };

        std::vector<LedgerRun> LedgerRuns(const SpeciesRemap& remap, uint32_t oldSpeciesCount) {
            if (remap.identity)
                return {{.oldId = 0, .newId = 0, .count = oldSpeciesCount}};

            std::vector<LedgerRun> runs;
            for (uint32_t oldId = 1; oldId < oldSpeciesCount && oldId < remap.newIds.size(); ++oldId) {
                const uint32_t newId = remap.newIds[oldId];
                if (newId == 0)
                    continue;

                const bool extends = !runs.empty() && runs.back().oldId + runs.back().count == oldId &&
                                     runs.back().newId + runs.back().count == newId;
                if (extends)
                    ++runs.back().count;
                else
                    runs.push_back({.oldId = oldId, .newId = newId, .count = 1});
            }

            return runs;
        }

    }  // namespace

    // 区画を作る(区画が無ければ何もしない)。断る: 投影・ProbeGasField が断るもの
    std::expected<void, std::string> ProbeSim::CreateGas(ID3D12Device5* device, const BakedReactionTable& table) {
        const ProbeGasOptions& options = m_options.gas;
        if (!options.Enabled())
            return {};

        if (options.pressure != ProbeGasPressure::Explicit)
            return std::unexpected("GPU の気体の区画は陽解法だけ(投影は T-0621)");

        if (auto built = BuildGasWork(device, table); !built)
            return built;

        // 気体の欄は既定のヒープの 0(静止)で始める。物質に依らないので表の差し替えでも作り直さない
        m_gasState = gpu::CreateBuffer(device, (uint64_t)m_gasHeader.regionCellCount * PROBE_GAS_CELL_STATE_BYTES,
                                       gpu::BufferKind::UnorderedAccess);
        if (!m_gasState)
            return std::unexpected("気体のバッファを作れない");

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
            m_gasHeader.atmosphereCount, m_gasHeader.slotCapacity);

        return {};
    }

    // 表から作る部分(気体の箱・作業場・その初めの中身のアップロード・粘りの段を投げるか)を作る。作る時と、表の差し替え・
    // 表の違う保存点へ戻す時(T-0670)。作業場はまだ写していない(呼ぶ側が写す。作ったバッファは COMMON)
    std::expected<void, std::string> ProbeSim::BuildGasWork(ID3D12Device5* device, const BakedReactionTable& table) {
        auto field = ProbeGasField::Create(table, m_options.gas);
        if (!field)
            return std::unexpected("気体の区画を作れない: " + field.error());

        // 気体の粘り(T-0758・D-516)があれば刻みの終わりに粘りの段を投げる(T-0775)。区画の物質は刻みごとに変わるので、
        // 表の気体の物質の全部を見る(粘りの無い物質しか区画に無い刻みも投げる。値は粘りの段を飛ばした時と同じ)
        m_gasViscous = table.environment.gasEddyViscosityMm2PerS != 0;
        for (uint32_t id = 0; id < table.speciesTransport.size() && !m_gasViscous; ++id)
            m_gasViscous = IsGasSpecies(table, id) &&
                           table.speciesTransport[id].viscosityMicroPascalSeconds.value_or(0) != 0;

        const std::span<const uint32_t> atmosphere = field->AtmosphereSpecies();
        uint32_t totalBytes = 0;
        m_gasHeader = MakeHeader(m_options.gas, table, atmosphere, totalBytes);

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

        m_gasWork = gpu::CreateBuffer(device, totalBytes, gpu::BufferKind::UnorderedAccess);
        if (!m_gasWork)
            return std::unexpected("気体の作業場を作れない");

        m_gasWork->SetName(L"ProbeSim.gasWork");

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

    // 表の差し替えの検査(T-0670): 作業場は新しい表で作り直すので、断るのは新しい表で区画を作れない時だけ
    // (基準の大気が無い・音速の基準が 0 か小刻みの安定の上限を超える。ProbeGasField::Create と同じ理由)
    std::expected<void, std::string> ProbeSim::CheckGasTableSwap(const BakedReactionTable& table) const {
        if (!m_gas)
            return {};

        if (auto field = ProbeGasField::Create(table, m_options.gas); !field)
            return std::unexpected("気体の流れる世界に当てられない表: " + field.error());

        return {};
    }

    // 表から作った部分(設定の型・層の基準・開いた境界の外・物質ごとの値・欄の数と並び)が変わるか
    bool ProbeSim::GasWorkChanges(const BakedReactionTable& table, const SpeciesRemap& remap) const {
        const BakedReactionTable& current = m_tableCopy;
        if (!remap.identity || table.environment != current.environment || table.atmosphere != current.atmosphere)
            return true;

        for (uint32_t id = 0; id < m_gasHeader.speciesCount; ++id) {
            const ProbeGasSpecies before = MakeGasSpecies(current, id);
            const ProbeGasSpecies after = MakeGasSpecies(table, id);
            if (std::memcmp(&before, &after, sizeof(ProbeGasSpecies)) != 0)
                return true;
        }

        return false;
    }

    // 表の差し替え(T-0670。適用の単位の前・RecordTableBuffers の前 = m_tableCopy は今の表): 作業場を新しい表で作り直し、
    // 持ち越す部分を写す。制御(通しの数)はそのまま、帳簿の物質ごとの量は名前で付け替える(消えた物質の量は捨てる。CPU の
    // ProbeGasField::ApplyTable と同じ)、帳簿の後ろの語(エネルギー・運動量・丸めの内訳)はそのまま。気体の箱は作り直す
    // (箱のセルと 1 刻みの帳簿は毎刻み区画から作るので写さない)。古い作業場は UAV の状態(RecordFrame が揃える)
    bool ProbeSim::RecordGasTableSwap(ID3D12GraphicsCommandList10* list, FrameSlot& frame,
                                      const BakedReactionTable& table, const SpeciesRemap& remap) {
        if (!m_gas || !GasWorkChanges(table, remap))
            return true;

        ComPtr<ID3D12Device5> device;
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
            return false;

        const ComPtr<ID3D12Resource> oldWork = m_gasWork;
        const ProbeGasHeader oldHeader = m_gasHeader;
        RetireGas(frame);
        if (auto built = BuildGasWork(device.Get(), table); !built) {
            Log(Channel::Sim, Level::Error, "表の差し替えで気体の区画を作り直せない: {}", built.error());
            return false;
        }

        // --- 新しい作業場: 表から作った部分(帳簿は 0 のまま = 作ったバッファの 0。付け替えた物質の分だけ下で写す)---
        const uint32_t ledgerBytes = (m_gasHeader.speciesCount + PROBE_GAS_LEDGER_WORDS_AFTER_SPECIES) * 8;
        const uint32_t ledgerEnd = m_gasHeader.ledgerOffset + ledgerBytes;
        const uint64_t totalBytes = m_gasUpload->GetDesc().Width;
        list->CopyBufferRegion(m_gasWork.Get(), PROBE_GAS_TEMPLATE_OFFSET, m_gasUpload.Get(), PROBE_GAS_TEMPLATE_OFFSET,
                               m_gasHeader.ledgerOffset - PROBE_GAS_TEMPLATE_OFFSET);
        list->CopyBufferRegion(m_gasWork.Get(), ledgerEnd, m_gasUpload.Get(), ledgerEnd, totalBytes - ledgerEnd);

        // --- 持ち越す部分: 制御・帳簿(物質 ID の続く並びはまとめて写す)---
        const D3D12_RESOURCE_BARRIER toSource = gpu::Transition(oldWork.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &toSource);
        list->CopyBufferRegion(m_gasWork.Get(), 0, oldWork.Get(), 0, PROBE_GAS_TEMPLATE_OFFSET);
        for (const LedgerRun& run : LedgerRuns(remap, oldHeader.speciesCount)) {
            list->CopyBufferRegion(m_gasWork.Get(), m_gasHeader.ledgerOffset + (run.newId * 8), oldWork.Get(),
                                   oldHeader.ledgerOffset + (run.oldId * 8), run.count * 8);
        }

        list->CopyBufferRegion(m_gasWork.Get(), m_gasHeader.ledgerOffset + (m_gasHeader.speciesCount * 8),
                               oldWork.Get(), oldHeader.ledgerOffset + (oldHeader.speciesCount * 8),
                               PROBE_GAS_LEDGER_WORDS_AFTER_SPECIES * 8);

        const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(m_gasWork.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);
        frame.keepAlive.push_back(oldWork);
        m_gas->RecordUpload(list);
        Log(Channel::Sim, Level::Info, "気体の区画を新しい表で作り直した(物質 {} → {}・欄 {} → {})",
            oldHeader.speciesCount, m_gasHeader.speciesCount, oldHeader.slotCapacity, m_gasHeader.slotCapacity);

        return true;
    }

    // 表の違う保存点へ戻す時(T-0670。フレームの先頭・状態を写す前): その表で作業場を作り直す。中身は保存点が全部を写すので
    // ここでは写さない(作ったバッファは COMMON のまま。RecordOverwriteState が写す)。気体の箱は作り直して初めの中身を写す
    bool ProbeSim::RecordGasForRestore(ID3D12GraphicsCommandList10* list, FrameSlot& frame,
                                       const BakedReactionTable& table, const SpeciesRemap& remap) {
        if (!m_gas || !GasWorkChanges(table, remap))
            return true;

        ComPtr<ID3D12Device5> device;
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
            return false;

        frame.keepAlive.push_back(m_gasWork);
        RetireGas(frame);
        if (auto built = BuildGasWork(device.Get(), table); !built) {
            Log(Channel::Sim, Level::Error, "保存点の表で気体の区画を作り直せない: {}", built.error());
            return false;
        }

        m_gas->RecordUpload(list);

        return true;
    }

    // 作り直す前の気体の箱と作業場のアップロードを、このリストが GPU で終わるまで持つ
    void ProbeSim::RetireGas(FrameSlot& frame) {
        frame.retiredGas.push_back(std::move(m_gas));
        frame.keepAlive.push_back(std::move(m_gasUpload));
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
                            work + m_gasHeader.ghostOffset, work + m_gasHeader.openOffset, m_gasViscous);

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
