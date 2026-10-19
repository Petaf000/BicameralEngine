// probe_sim_liquid.cpp — 仮の世界(ProbeSim)の GPU の刻みに液体の区画を載せる(T-0613・ADR-0266・ADR-0276・ADR-0268)。
//
// CPU リファレンスは sim/probe_liquid.cpp の ProbeLiquidField::Step(ProbeReference::Advance の (1c))。GPU は適用の単位の中
// (コマンド → 物理の熱 → 表の作り直し → 気体 → **液体** → 起こす刻みの来たブロックを一覧へ)で、shaders/sim/probe_liquid_tick.hlsl の
// 10 段を投げる。置き場所の形は shaders/common/probe_liquid_gpu.hlsli(バッファは 1 つ。ルート署名の u19)。
// 作る時に CPU が決めるのは、区画の形・模型の値・物質ごとの値(液体か・凝縮相か・モル質量・モル体積)と欄の場所だけ。状態は 0(静止)から。
// 保存点・写しの早送りは作業場の持ち越す部分を写し、状態のハッシュは LiquidHash が持ち越す欄と帳簿を足す(T-0620)。
// 表の差し替え(T-0670): 見出し(重力・物質の数・欄の場所)と物質ごとの値を新しい表で書き直す。欄の場所が変わる時(物質の数で
// 物質ごとの値の欄が伸び縮みする時)は作業場を作り直し、持ち越す欄(制御・帳簿・面の運動量・面の運動のエネルギー・位置のエネルギー・
// 液体の質量)を写す。持ち越す欄は物質に依らない。断るのは新しい表で区画を作れない時(重力が 0 など)だけ。
#include <algorithm>
#include <array>
#include <cstring>
#include <format>

#include "core/log.h"
#include "gpu/resources.h"
#include "sim/probe_sim.h"
#include "sim/species_remap.h"

namespace bicameral::sim {

    namespace {

        // shaders/CMakeLists.txt で入口ごとに作る .cso(呼ぶ順。m_liquidPipelines の並び)
        constexpr std::array<const char*, 11> LIQUID_SHADER_NAMES = {
            "sim/probe_liquid_read.cso",         "sim/probe_liquid_pressure.cso",   "sim/probe_liquid_forces.cso",
            "sim/probe_liquid_outgoing.cso",     "sim/probe_liquid_move_faces.cso", "sim/probe_liquid_apply.cso",
            "sim/probe_liquid_leaving.cso",      "sim/probe_liquid_advect.cso",     "sim/probe_liquid_settle_faces.cso",
            "sim/probe_liquid_settle_cells.cso", "sim/probe_liquid_hash.cso"};

        enum LiquidPass : uint8_t {
            Read,
            Pressure,
            Forces,
            Outgoing,
            MoveFaces,
            Apply,
            Leaving,
            Advect,
            SettleFaces,
            SettleCells,
            Hash
        };

        constexpr uint32_t RX_CELL_BYTES = 112;  // probe_liquid_tick.hlsl の RX_CELL_BYTES

        static_assert(sizeof(reaction::RxCell) == RX_CELL_BYTES);
        static_assert(sizeof(ProbeLiquidSpecies) == PROBE_LIQUID_SPECIES_BYTES);
        static_assert(sizeof(ProbeLiquidCellState) == PROBE_LIQUID_CELL_STATE_BYTES);
        static_assert(sizeof(ProbeLiquidHeader) == 128);
        static_assert(PROBE_LIQUID_DYNAMIC_OFFSET - PROBE_LIQUID_HEADER_OFFSET >= sizeof(ProbeLiquidHeader));

        uint32_t AlignUp(uint32_t value) {
            return (value + PROBE_LIQUID_ALIGNMENT - 1) / PROBE_LIQUID_ALIGNMENT * PROBE_LIQUID_ALIGNMENT;
        }

        uint32_t GroupsFor(uint32_t threads) {
            return std::max(1u, (threads + PROBE_LINEAR_GROUP_SIZE - 1) / PROBE_LINEAR_GROUP_SIZE);
        }

        template <typename T>
        void WriteAt(std::vector<std::byte>& bytes, uint32_t offset, const T& value) {
            std::memcpy(bytes.data() + offset, &value, sizeof(T));
        }

        // 見出し(区画の形・模型の値・欄の場所)。欄の場所は区画のセルの数と表の物質の数から
        std::expected<ProbeLiquidHeader, std::string> MakeHeader(const ProbeLiquidOptions& options,
                                                                 const BakedReactionTable& table,
                                                                 uint32_t& totalBytes) {
            ProbeLiquidHeader header{};
            for (uint32_t axis = 0; axis < 3; ++axis) {
                header.regionMin[axis] = options.minCell[axis];
                header.regionSize[axis] = options.sizeCells[axis];
            }

            header.regionCellCount = options.sizeCells[0] * options.sizeCells[1] * options.sizeCells[2];
            header.speciesCount = (uint32_t)table.species.size();
            header.gravityMicrometersPerSecond2 = (uint32_t)table.environment.gravityMicrometersPerSecond2;
            header.frictionShift = options.frictionShift;
            header.overfillStiffness = options.overfillStiffness;
            header.advection = options.advection ? 1u : 0u;
            header.maxSpeedMillimetersPerSecond = options.maxSpeedMillimetersPerSecond;
            if (table.environment.gravityMicrometersPerSecond2 > UINT32_MAX)
                return std::unexpected("重力が GPU の液体の段の範囲(32bit の µm/s²)を超える");

            // 作業場は区画のセルあたり約 0.7 KB。バイトの場所が 32bit に収まる区画まで(64³ の全部でも約 180 MB)
            uint64_t offset = PROBE_LIQUID_DYNAMIC_OFFSET;
            bool fits = true;
            const auto place = [&](uint64_t bytes) {
                const uint64_t at = offset;
                offset = AlignUp((uint32_t)std::min<uint64_t>(offset + bytes, UINT32_MAX - PROBE_LIQUID_ALIGNMENT));
                fits = fits && at + bytes < UINT32_MAX - PROBE_LIQUID_ALIGNMENT;
                return (uint32_t)at;
            };
            const uint64_t cells = header.regionCellCount;
            header.speciesOffset = place((uint64_t)header.speciesCount * PROBE_LIQUID_SPECIES_BYTES);
            header.facesOffset = place(cells * 3 * 8);
            header.kineticOffset = place(cells * 3 * 8);
            header.potentialOffset = place(cells * 8);
            header.snapshotOffset = place(cells * RX_CELL_BYTES);
            header.stateOffset = place(cells * PROBE_LIQUID_CELL_STATE_BYTES);
            header.pressureOffset = place(cells * 8);
            header.aboveOffset = place(cells * 8);
            header.forcedOffset = place(cells * 3 * 8);
            header.desiredOffset = place(cells * 3 * 8);
            header.outgoingOffset = place(cells * 8);
            header.movedOffset = place(cells * 3 * 8);
            header.leavingOffset = place(cells * 3 * 8);
            header.carriedOffset = place(cells * 3 * 8);
            header.releasedOffset = place(cells * 3 * 8);
            header.massOffset = place(cells * 8);
            header.potentialCarriedOffset = place(cells * 8);
            header.changedOffset = place(cells * 4);
            header.listOffset = place(cells * PROBE_LIQUID_LIST_ENTRIES * PROBE_LIQUID_LIST_ENTRY_BYTES);
            if (!fits)
                return std::unexpected("液体の区画が GPU の作業場(4 GB)に収まらない");

            totalBytes = (uint32_t)offset;

            return header;
        }

        ProbeLiquidSpecies MakeLiquidSpecies(const BakedReactionTable& table, uint32_t id) {
            const bool liquid = id < table.speciesLiquid.size() && table.speciesLiquid[id] != 0;
            const bool condensed = id < table.speciesCondensed.size() && table.speciesCondensed[id] != 0;

            return {.molarVolume = id < table.speciesMolarVolume.size() ? table.speciesMolarVolume[id] : 0,
                    .molarMass = table.molarMasses[id],
                    .flags = (liquid ? PROBE_LIQUID_SPECIES_LIQUID : 0u) |
                             (condensed ? PROBE_LIQUID_SPECIES_CONDENSED : 0u)};
        }

    }  // namespace

    // 区画を作る(区画が無ければ何もしない)。断る: ProbeLiquidField が断るもの・作業場が 4 GB を超える
    std::expected<void, std::string> ProbeSim::CreateLiquid(ID3D12Device5* device, const BakedReactionTable& table) {
        const ProbeLiquidOptions& options = m_options.liquid;
        if (!options.Enabled())
            return {};

        if (auto field = ProbeLiquidField::Create(table, options); !field)
            return std::unexpected("液体の区画を作れない: " + field.error());

        uint32_t totalBytes = 0;
        auto header = MakeHeader(options, table, totalBytes);
        if (!header)
            return std::unexpected(header.error());

        m_liquidHeader = *header;
        if (auto uploaded = UploadLiquidHeader(device, table); !uploaded)
            return uploaded;

        m_liquidWork = gpu::CreateBuffer(device, totalBytes, gpu::BufferKind::UnorderedAccess);
        if (!m_liquidWork)
            return std::unexpected("液体の作業場を作れない");

        m_liquidWork->SetName(L"ProbeSim.liquidWork");

        // --- 段 ---
        for (size_t pass = 0; pass < LIQUID_SHADER_NAMES.size(); ++pass) {
            const auto bytecode = gpu::LoadShader(LIQUID_SHADER_NAMES[pass]);
            if (!bytecode)
                return std::unexpected(bytecode.error());

            m_liquidPipelines[pass] = gpu::CreateComputePipeline(device, m_rootSignature.Get(), *bytecode);
            if (!m_liquidPipelines[pass])
                return std::unexpected(std::format("パイプラインを作れない: {}", LIQUID_SHADER_NAMES[pass]));
        }

        Log(Channel::Sim, Level::Info, "液体の区画: ({}, {}, {}) から {} × {} × {} セル・作業場 {} KB",
            options.minCell[0], options.minCell[1], options.minCell[2], options.sizeCells[0], options.sizeCells[1],
            options.sizeCells[2], totalBytes / 1024);

        return {};
    }

    // 作業場の初めの中身(見出し・物質ごとの値。状態と帳簿は 0 = 静止)のアップロードを m_liquidHeader と表から作る
    std::expected<void, std::string> ProbeSim::UploadLiquidHeader(ID3D12Device5* device,
                                                                  const BakedReactionTable& table) {
        std::vector<std::byte> bytes(m_liquidHeader.speciesOffset +
                                     (m_liquidHeader.speciesCount * PROBE_LIQUID_SPECIES_BYTES));
        WriteAt(bytes, PROBE_LIQUID_HEADER_OFFSET, m_liquidHeader);
        for (uint32_t id = 0; id < m_liquidHeader.speciesCount; ++id)
            WriteAt(bytes, m_liquidHeader.speciesOffset + (id * PROBE_LIQUID_SPECIES_BYTES),
                    MakeLiquidSpecies(table, id));

        m_liquidUpload = gpu::CreateBuffer(device, bytes.size(), gpu::BufferKind::Upload);
        void* mapped = nullptr;
        const D3D12_RANGE noRead{.Begin = 0, .End = 0};
        if (!m_liquidUpload || FAILED(m_liquidUpload->Map(0, &noRead, &mapped)))
            return std::unexpected("液体の作業場のアップロードを作れない");

        std::memcpy(mapped, bytes.data(), bytes.size());
        m_liquidUpload->Unmap(0, nullptr);

        return {};
    }

    // 最初のフレーム: 作業場の見出しと物質ごとの値を写す(残りは作った時の 0)
    void ProbeSim::RecordLiquidInitialization(ID3D12GraphicsCommandList10* list) const {
        if (!m_liquidWork)
            return;

        const D3D12_RESOURCE_BARRIER toCopy = gpu::Transition(m_liquidWork.Get(), D3D12_RESOURCE_STATE_COMMON,
                                                              D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &toCopy);
        list->CopyBufferRegion(m_liquidWork.Get(), 0, m_liquidUpload.Get(), 0, m_liquidUpload->GetDesc().Width);
        const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(m_liquidWork.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);
    }

    // 表の差し替えの検査(T-0670): 作業場は新しい表で書き直すので、断るのは新しい表で区画を作れない時だけ
    // (重力が 0・速さや摩擦の設定・作業場が 4 GB を超える。ProbeLiquidField::Create・MakeHeader と同じ理由)
    std::expected<void, std::string> ProbeSim::CheckLiquidTableSwap(const BakedReactionTable& table) const {
        if (!m_liquidWork)
            return {};

        if (auto field = ProbeLiquidField::Create(table, m_options.liquid); !field)
            return std::unexpected("液体の流れる世界に当てられない表: " + field.error());

        uint32_t totalBytes = 0;
        if (auto header = MakeHeader(m_options.liquid, table, totalBytes); !header)
            return std::unexpected("液体の流れる世界に当てられない表: " + header.error());

        return {};
    }

    // 作業場に表から書いた部分(重力・物質の数・物質ごとの値)が変わるか
    bool ProbeSim::LiquidWorkChanges(const BakedReactionTable& table, const SpeciesRemap& remap) const {
        const BakedReactionTable& current = m_tableCopy;
        if (!remap.identity ||
            table.environment.gravityMicrometersPerSecond2 != current.environment.gravityMicrometersPerSecond2)
            return true;

        for (uint32_t id = 0; id < m_liquidHeader.speciesCount; ++id) {
            const ProbeLiquidSpecies before = MakeLiquidSpecies(current, id);
            const ProbeLiquidSpecies after = MakeLiquidSpecies(table, id);
            if (std::memcmp(&before, &after, sizeof(ProbeLiquidSpecies)) != 0)
                return true;
        }

        return false;
    }

    // 見出しと物質ごとの値を新しい表で作り直す。欄の場所が変わる(作業場を作り直す)なら古い作業場を返す(変わらなければ nullptr)
    std::expected<ComPtr<ID3D12Resource>, std::string> ProbeSim::RebuildLiquidWork(ID3D12Device5* device,
                                                                                   FrameSlot& frame,
                                                                                   const BakedReactionTable& table) {
        uint32_t totalBytes = 0;
        auto header = MakeHeader(m_options.liquid, table, totalBytes);
        if (!header)
            return std::unexpected(header.error());

        // 欄の場所が同じか(物質の数と重力のほかが同じ見出し)
        ProbeLiquidHeader layout = m_liquidHeader;
        layout.speciesCount = header->speciesCount;
        layout.gravityMicrometersPerSecond2 = header->gravityMicrometersPerSecond2;
        const bool sameLayout = std::memcmp(&layout, &*header, sizeof(ProbeLiquidHeader)) == 0 &&
                                totalBytes == m_liquidWork->GetDesc().Width;

        frame.keepAlive.push_back(std::move(m_liquidUpload));
        m_liquidHeader = *header;
        if (auto uploaded = UploadLiquidHeader(device, table); !uploaded)
            return std::unexpected(uploaded.error());

        if (sameLayout)
            return ComPtr<ID3D12Resource>();

        ComPtr<ID3D12Resource> oldWork = std::move(m_liquidWork);
        m_liquidWork = gpu::CreateBuffer(device, totalBytes, gpu::BufferKind::UnorderedAccess);
        if (!m_liquidWork)
            return std::unexpected("液体の作業場を作れない");

        m_liquidWork->SetName(L"ProbeSim.liquidWork");
        frame.keepAlive.push_back(oldWork);

        return oldWork;
    }

    // 表の差し替え(T-0670。適用の単位の前・RecordTableBuffers の前 = m_tableCopy は今の表): 見出しと物質ごとの値を書き直す。
    // 作業場を作り直した時は持ち越す欄を古い作業場から写す(物質に依らないので付け替えは要らない。重力が変わった分の位置の
    // エネルギーは次の刻みの入口で帳簿へ入る = CPU の ProbeLiquidField と同じ)。古い作業場は UAV の状態(RecordFrame が揃える)
    bool ProbeSim::RecordLiquidTableSwap(ID3D12GraphicsCommandList10* list, FrameSlot& frame,
                                         const BakedReactionTable& table, const SpeciesRemap& remap) {
        if (!m_liquidWork || !LiquidWorkChanges(table, remap))
            return true;

        ComPtr<ID3D12Device5> device;
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
            return false;

        const ProbeLiquidHeader oldHeader = m_liquidHeader;
        auto oldWork = RebuildLiquidWork(device.Get(), frame, table);
        if (!oldWork) {
            Log(Channel::Sim, Level::Error, "表の差し替えで液体の区画を作り直せない: {}", oldWork.error());
            return false;
        }

        const uint64_t uploadBytes = m_liquidUpload->GetDesc().Width;
        if (!*oldWork) {
            const D3D12_RESOURCE_BARRIER toCopy = gpu::Transition(
                m_liquidWork.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            list->ResourceBarrier(1, &toCopy);
        }

        list->CopyBufferRegion(m_liquidWork.Get(), PROBE_LIQUID_HEADER_OFFSET, m_liquidUpload.Get(),
                               PROBE_LIQUID_HEADER_OFFSET, uploadBytes - PROBE_LIQUID_HEADER_OFFSET);
        if (*oldWork) {
            ID3D12Resource* from = oldWork->Get();
            const D3D12_RESOURCE_BARRIER toSource = gpu::Transition(from, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                                    D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &toSource);
            list->CopyBufferRegion(m_liquidWork.Get(), 0, from, 0, PROBE_LIQUID_HEADER_OFFSET);

            const uint64_t cells = oldHeader.regionCellCount;
            const std::array<std::array<uint64_t, 3>, 4> carried = {{
                {m_liquidHeader.facesOffset, oldHeader.facesOffset, cells * 3 * 8},
                {m_liquidHeader.kineticOffset, oldHeader.kineticOffset, cells * 3 * 8},
                {m_liquidHeader.potentialOffset, oldHeader.potentialOffset, cells * 8},
                {m_liquidHeader.massOffset, oldHeader.massOffset, cells * 8},
            }};
            for (const auto& [to, at, bytes] : carried)
                list->CopyBufferRegion(m_liquidWork.Get(), to, from, at, bytes);
        }

        const D3D12_RESOURCE_BARRIER toUav = gpu::Transition(m_liquidWork.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &toUav);
        Log(Channel::Sim, Level::Info, "液体の区画を新しい表で書き直した(物質 {} → {}・作業場を{})",
            oldHeader.speciesCount, m_liquidHeader.speciesCount, *oldWork ? "作り直した" : "そのまま使う");

        return true;
    }

    // 表の違う保存点へ戻す時(T-0670。フレームの先頭・状態を写す前): その表の欄の場所に合わせる。中身は保存点が全部を写す
    // (見出しと物質ごとの値も保存点の表のもの)ので、ここでは写さない
    bool ProbeSim::RecordLiquidForRestore(ID3D12GraphicsCommandList10* list, FrameSlot& frame,
                                          const BakedReactionTable& table, const SpeciesRemap& remap) {
        if (!m_liquidWork || !LiquidWorkChanges(table, remap))
            return true;

        ComPtr<ID3D12Device5> device;
        if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))))
            return false;

        if (auto oldWork = RebuildLiquidWork(device.Get(), frame, table); !oldWork) {
            Log(Channel::Sim, Level::Error, "保存点の表で液体の区画を作り直せない: {}", oldWork.error());
            return false;
        }

        return true;
    }

    // 状態のハッシュの単位: HashCells と同じ欄へ液体の持ち越す欄と帳簿の寄与を足す
    void ProbeSim::RecordLiquidHash(ID3D12GraphicsCommandList10* list) const {
        list->SetPipelineState(m_liquidPipelines[LiquidPass::Hash].Get());
        list->Dispatch(GroupsFor(m_liquidHeader.regionCellCount), 1, 1);
    }

    // 液体の 10 段(ProbeSim のルート署名と単位の定数が結んである所で。気体の段の後)
    void ProbeSim::RecordLiquid(ID3D12GraphicsCommandList10* list) const {
        const D3D12_RESOURCE_BARRIER allUavs = gpu::UavBarrier(nullptr);
        const uint32_t cellGroups = GroupsFor(m_liquidHeader.regionCellCount);
        const uint32_t columnGroups = GroupsFor(m_liquidHeader.regionSize[0] * m_liquidHeader.regionSize[2]);
        for (uint32_t pass = LiquidPass::Read; pass <= LiquidPass::SettleCells; ++pass) {
            if (pass == LiquidPass::Leaving && m_liquidHeader.advection == 0)
                continue;  // 移流なし: 出る流れの合計は読まない(LiquidAdvect は写すだけ)

            list->ResourceBarrier(1, &allUavs);
            list->SetPipelineState(m_liquidPipelines[pass].Get());
            list->Dispatch(pass == LiquidPass::Pressure ? columnGroups : cellGroups, 1, 1);
        }
    }

}  // namespace bicameral::sim
