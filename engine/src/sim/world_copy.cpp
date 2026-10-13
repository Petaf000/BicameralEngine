// world_copy.cpp — 世界の写しの早送り(T-0412・ADR-0198)。使い方と約束は world_copy.h。
//
// 写しの世界は ProbeSim をもう 1 つ作ったもの(同じ表・同じ物理の場面)。写すのは保存点から戻すのと同じ道
// (ProbeFrameInput::adoptFrom。sim/probe_save_point.cpp)で、写す元のバッファは読むだけ。
// GPU にある写しのリストは 1 組だけなので、ProbeSim のフレームの枠は 0 だけを使う。
#include "sim/world_copy.h"

#include <algorithm>
#include <array>

#include "core/aliases.h"
#include "core/log.h"
#include "gpu/resources.h"

namespace bicameral::sim {
    namespace {

        constexpr uint64_t CELL_BYTES = uint64_t{PROBE_CELL_COUNT} * sizeof(reaction::RxCell);             // 1 世代
        constexpr uint64_t THERMAL_BYTES = uint64_t{PROBE_CELL_COUNT} * sizeof(reaction::HcThermalCache);  // 1 世代
        constexpr uint32_t SLOT = 0;

        void TransitionPair(ID3D12GraphicsCommandList10* list, ID3D12Resource* first, ID3D12Resource* second,
                            D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
            const std::array<D3D12_RESOURCE_BARRIER, 2> barriers = {gpu::Transition(first, before, after),
                                                                    gpu::Transition(second, before, after)};
            list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }

    }  // namespace

    // --- 作る ---

    std::expected<WorldCopy, std::string> WorldCopy::Create(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE listType,
                                                            const BakedReactionTable& table,
                                                            const ProbeSimOptions& options) {
        // 初めの世界は写す元の状態で上書きするので空でよい(表の世界の組み立てを作り直さない)
        const std::vector<reaction::RxCell> blank = MakeBlankProbeWorld();
        auto simulation = ProbeSim::Create(device, listType, table,
                                           {.physicsScene = options.physicsScene,
                                            .physicsOptions = options.physicsOptions,
                                            .physicsParameters = options.physicsParameters,
                                            .initialWorld = blank});
        if (!simulation)
            return std::unexpected(std::format("写しの世界を作れない: {}", simulation.error()));

        WorldCopy copy;
        copy.m_simulation = std::make_unique<ProbeSim>(std::move(*simulation));
        if (!copy.CreateReadback(device, listType))
            return std::unexpected("写しの読み戻しを作れない");

        copy.m_vramBytes = copy.m_simulation->DefaultHeapBytes();
        copy.m_costs = TickCostAccumulator(copy.m_simulation->UnitsPerTick());
        Log(Channel::Sim, Level::Info, "写しの世界を作った(既定のヒープ {} MiB)", copy.m_vramBytes >> 20);

        return copy;
    }

    bool WorldCopy::CreateReadback(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE listType) {
        if (FAILED(device->CreateCommandAllocator(listType, IID_PPV_ARGS(&m_allocator))) ||
            FAILED(device->CreateCommandList1(0, listType, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&m_list)))) {
            return false;
        }

        m_cellReadback = gpu::CreateBuffer(device, CELL_BYTES, gpu::BufferKind::Readback);
        m_thermalReadback = gpu::CreateBuffer(device, THERMAL_BYTES, gpu::BufferKind::Readback);
        if (!m_cellReadback || !m_thermalReadback)
            return false;

        m_cellReadback->SetName(L"WorldCopy.cellReadback");
        m_thermalReadback->SetName(L"WorldCopy.thermalReadback");
        m_cells.resize(PROBE_CELL_COUNT);
        m_thermal.resize(PROBE_CELL_COUNT);

        return true;
    }

    // --- 記録する ---

    std::vector<ID3D12CommandList*> WorldCopy::RecordInitialization() {
        if (m_pending || m_initialized)
            return {};

        ID3D12CommandList* list = m_simulation->RecordFrame(SLOT, {.firstTick = 0, .unitCount = 0});
        if (list == nullptr)
            return {};

        m_pending = true;

        return {list};
    }

    std::vector<ID3D12CommandList*> WorldCopy::RecordStart(const ProbeSim& source, uint64_t tick,
                                                           WorldCopyQuery query) {
        const bool ordered = rng::is_sorted(query.commands, CommandPrecedes) &&
                             (query.commands.empty() || query.commands.front().targetTick >= tick);
        if (m_pending || !m_initialized || query.maxTicks == 0 || !ordered) {
            Log(Channel::Sim, Level::Error,
                "写しの早送りを始められない: 待ち {} 初期化 {} 上限 {} 刻み、コマンドの並び {}(刻み {} 以上・昇順)",
                m_pending, m_initialized, query.maxTicks, ordered, tick);
            return {};
        }

        m_query = std::move(query);
        m_result = {.startTick = tick, .endTick = tick};
        m_tick = tick;
        m_nextCommand = 0;
        m_costs.Reset();
        m_running = true;
        m_finished = false;

        ID3D12CommandList* frame = m_simulation->RecordFrame(
            SLOT, {.firstTick = tick, .firstUnit = 0, .unitCount = 0, .adoptFrom = &source});
        if (frame == nullptr) {
            Fail("写しを取るリストを記録できない");
            return {};
        }

        std::vector<ID3D12CommandList*> lists = {frame};
        m_pendingSample = m_query.sampleIntervalTicks > 0 ? tick : UINT64_MAX;
        if (m_pendingSample != UINT64_MAX)
            lists.push_back(RecordReadback(tick));

        if (rng::contains(lists, nullptr)) {
            Fail("写しの読み戻しを記録できない");
            return {};
        }

        m_pendingStart = true;
        m_pending = true;

        return lists;
    }

    std::vector<ID3D12CommandList*> WorldCopy::RecordNext(uint64_t maxTicks) {
        if (m_pending || !m_running || maxTicks == 0)
            return {};

        // 上限・1 リストの単位の上限・次に読み戻す刻みで切る
        const uint64_t end = m_result.startTick + m_query.maxTicks;
        uint64_t ticks = std::min({maxTicks, end - m_tick, uint64_t{ProbeSim::MAX_UNITS_PER_FRAME / UnitsPerTick()}});
        if (m_query.sampleIntervalTicks > 0) {
            const uint64_t interval = m_query.sampleIntervalTicks;
            const uint64_t nextSample = m_result.startTick + ((m_tick - m_result.startTick) / interval + 1) * interval;
            ticks = std::min(ticks, nextSample - m_tick);
        }

        const std::vector<ProbeCommand> commands = TakeCommands(ticks);
        if (ticks == 0) {
            Fail("1 刻みのコマンドが GPU のキューに入りきらない");
            return {};
        }

        ID3D12CommandList* frame = m_simulation->RecordFrame(
            SLOT,
            {.firstTick = m_tick, .unitCount = static_cast<uint32_t>(ticks * UnitsPerTick()), .commands = commands});
        if (frame == nullptr) {
            Fail("早送りのリストを記録できない");
            return {};
        }

        m_nextCommand += commands.size();
        m_tick += ticks;
        m_result.ticksRun += ticks;

        std::vector<ID3D12CommandList*> lists = {frame};
        m_pendingSample = IsSampleTick(m_tick) ? m_tick : UINT64_MAX;
        if (m_pendingSample != UINT64_MAX)
            lists.push_back(RecordReadback(m_tick));

        if (rng::contains(lists, nullptr)) {
            Fail("写しの読み戻しを記録できない");
            return {};
        }

        m_pending = true;

        return lists;
    }

    // このリストで当てるコマンド(刻み m_tick + ticks より前のもの)。GPU のキューに入りきらなければ、入らない最初のコマンドの刻みの前で
    // ticks を切る(その刻みは次のリストで。同じ刻みのコマンドを全部足すまで、その刻みは走らせない)
    std::vector<ProbeCommand> WorldCopy::TakeCommands(uint64_t& ticks) {
        const size_t limit = std::min<size_t>(m_simulation->FreeCommandSlots(), PROBE_MAX_COMMANDS);
        std::vector<ProbeCommand> taken;
        for (size_t index = m_nextCommand; index < m_query.commands.size(); ++index) {
            const ProbeCommand& command = m_query.commands[index];
            if (command.targetTick >= m_tick + ticks)
                break;

            if (taken.size() == limit) {
                ticks = command.targetTick - m_tick;
                std::erase_if(taken, [&](const ProbeCommand& kept) { return kept.targetTick >= command.targetTick; });
                break;
            }

            taken.push_back(command);
        }

        return taken;
    }

    // 写しの世界の S(tick) のセルと熱を読み戻しへ(フレームのリストの後ろの別のリスト。バッファは COMMON)
    ID3D12CommandList* WorldCopy::RecordReadback(uint64_t tick) {
        if (FAILED(m_allocator->Reset()) || FAILED(m_list->Reset(m_allocator.Get(), nullptr)))
            return nullptr;

        ID3D12Resource* cells = m_simulation->Cells();
        ID3D12Resource* thermal = m_simulation->Thermal();
        const uint64_t generation = tick & 1;
        TransitionPair(m_list.Get(), cells, thermal, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        m_list->CopyBufferRegion(m_cellReadback.Get(), 0, cells, generation * CELL_BYTES, CELL_BYTES);
        m_list->CopyBufferRegion(m_thermalReadback.Get(), 0, thermal, generation * THERMAL_BYTES, THERMAL_BYTES);
        TransitionPair(m_list.Get(), cells, thermal, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

        return SUCCEEDED(m_list->Close()) ? m_list.Get() : nullptr;
    }

    bool WorldCopy::IsSampleTick(uint64_t tick) const {
        const uint64_t interval = m_query.sampleIntervalTicks;
        return interval > 0 && tick >= m_result.startTick && (tick - m_result.startTick) % interval == 0;
    }

    // --- 終えたリストを読む ---

    void WorldCopy::Complete() {
        if (!m_pending)
            return;

        m_pending = false;
        const ProbeFrameReadback readback = m_simulation->ReadFrame(SLOT);
        m_result.debugAssertCount += readback.debugAssertCount;
        if (!m_running) {
            m_initialized = true;  // 初期化のリスト(と、失敗して止めた後のリスト)
            return;
        }

        ++m_result.frames;
        for (const uint64_t ticks : readback.unitGpuTicks)
            m_result.gpuTimestampTicks += ticks;

        const uint64_t sample = std::exchange(m_pendingSample, UINT64_MAX);
        if (sample != UINT64_MAX &&
            (!gpu::ReadBuffer(m_cellReadback.Get(), std::as_writable_bytes(std::span(m_cells))) ||
             !gpu::ReadBuffer(m_thermalReadback.Get(), std::as_writable_bytes(std::span(m_thermal))))) {
            Fail("写しのセルを読み戻せない");
            return;
        }

        // 始めの刻み S(t) は要約が無い(GPU の表は前の刻みが書く)
        if (std::exchange(m_pendingStart, false) && Judge(m_result.startTick, nullptr, sample == m_result.startTick))
            return;

        // 刻みごとの GPU の時間(写しのリストはいつも刻みの境界から刻みの境界まで。数えられない刻みは 0)
        std::vector<TickCost> costs;
        (void)m_costs.Add(readback, costs);
        auto cost = costs.begin();
        for (const ProbeTickHash& hash : readback.hashes) {
            while (cost != costs.end() && cost->tick < hash.tick)
                ++cost;

            m_result.hashes.push_back(hash);
            m_result.tickGpuTimestampTicks.push_back(
                cost != costs.end() && cost->tick == hash.tick ? cost->gpuTimestampTicks : 0);
            if (Judge(hash.tick, &hash, sample == hash.tick))
                return;
        }

        if (m_tick >= m_result.startTick + m_query.maxTicks)
            Finish(m_tick, false);
    }

    bool WorldCopy::Judge(uint64_t tick, const ProbeTickHash* summary, bool withCells) {
        if (!m_query.until)
            return false;

        WorldCopySample sample{.tick = tick, .summary = summary};
        if (withCells) {
            sample.cells = m_cells;
            sample.thermal = m_thermal;
        }

        if (!m_query.until(sample))
            return false;

        Finish(tick, true);

        return true;
    }

    void WorldCopy::Finish(uint64_t endTick, bool reached) {
        m_result.endTick = endTick;
        m_result.reached = reached;
        m_running = false;
        m_finished = true;
        Log(Channel::Sim, Level::Info, "写しの早送り: 刻み {} → {}({}。{} 刻み走らせた・{} リスト)", m_result.startTick,
            endTick, reached ? "条件に着いた" : "上限", m_result.ticksRun, m_result.frames);
    }

    void WorldCopy::Fail(std::string_view reason) {
        Log(Channel::Sim, Level::Error, "写しの早送り: {}(刻み {})", reason, m_tick);
        m_result.failed = true;
        m_result.endTick = m_tick;
        m_running = false;
        m_finished = true;
    }

    // --- 条件を書く小さな道具 ---

    uint64_t ProbeSpeciesAmount(std::span<const reaction::RxCell> cells, uint32_t species) {
        uint64_t total = 0;
        for (const reaction::RxCell& cell : cells) {
            const uint32_t count = std::min<uint32_t>(cell.speciesCount, reaction::RX_MAX_CELL_SPECIES);
            for (uint32_t index = 0; index < count; ++index) {
                if (cell.species[index] == species)
                    total += cell.amounts[index];
            }
        }

        return total;
    }

    uint32_t ProbeMaxTemperature(std::span<const reaction::HcThermalCache> thermal) {
        uint32_t highest = 0;
        for (const reaction::HcThermalCache& cache : thermal)
            highest = std::max(highest, cache.temperature);

        return highest;
    }

    uint32_t ProbeCellsAtOrAbove(std::span<const reaction::HcThermalCache> thermal, uint32_t temperatureMillikelvin) {
        return static_cast<uint32_t>(rng::count_if(thermal, [&](const reaction::HcThermalCache& cache) {
            return cache.temperature >= temperatureMillikelvin;
        }));
    }

}  // namespace bicameral::sim
