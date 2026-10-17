// probe_save_point.cpp — 仮の世界の保存点(刻みの境界の状態の写し)と、そこへ戻す(T-0143・ADR-0036)。
//
// 巻き戻し = 保存点 + 再生(docs/design/14 §2): 刻み t の境界の状態 S(t) を GPU → GPU のコピーで VRAM の保存点に写しておき、
// 戻すときは写しを世界へ書き戻してから、刻み t 以降のコマンドを足し直して進め直す。決定性(ADR-0008)があるので、
// 同じコマンドなら最初から流した時と同じハッシュ列になる(tests/gpu_probe_rewind_test.cpp)。CPU への読み戻しは無い。
//
// どちらもフレームのリストの先頭(単位とコマンドを足す前)で記録する。リストの間はどのバッファも COMMON(バッファは
// ExecuteCommandLists の後で COMMON に戻る)なので、COMMON → コピー → COMMON の明示の遷移で囲めば、後ろの単位の状態の扱いは変わらない。
// 写すもの: セルと熱のキャッシュ(S(t) の世代だけ)・予定の印・活性の一覧 2 組・ハッシュの表・刻みのイベントの一時置き場・
// 物理の u0〜u13・区画の持ち越す欄(気体: 欄 gasState と作業場 gasWork の全部〔帳簿・通しの数を含む〕/ 液体: 作業場 liquidWork の全部
// 〔持ち越す欄が作業の値の間にもある: 液体の質量 massOffset。T-0656〕。T-0620)。
// 区画の欄は世代を持たない(1 組。刻み t の境界では S(t) の値)。
// 写さないもの: コマンドキュー(戻すと空にし、呼ぶ側が足し直す)・抽出・イベントのリング・カウンタ・トレース(どれも出力。世界の結果に入らない)。
// 同じ道で、別の ProbeSim(本物の世界)の S(t) をこの世界へ写す(写しの早送り。T-0412・ADR-0198。sim/world_copy)。
#include <algorithm>
#include <array>

#include "core/aliases.h"
#include "core/log.h"
#include "gpu/com_ptr.h"
#include "gpu/resources.h"
#include "sim/probe_sim.h"

namespace bicameral::sim {
    namespace {

        constexpr uint64_t CELL_BYTES = uint64_t{PROBE_CELL_COUNT} * sizeof(reaction::RxCell);             // 1 世代
        constexpr uint64_t THERMAL_BYTES = uint64_t{PROBE_CELL_COUNT} * sizeof(reaction::HcThermalCache);  // 1 世代

        void AppendTransitions(std::vector<D3D12_RESOURCE_BARRIER>& barriers,
                               std::span<ID3D12Resource* const> resources, D3D12_RESOURCE_STATES before,
                               D3D12_RESOURCE_STATES after) {
            for (ID3D12Resource* resource : resources)
                barriers.push_back(gpu::Transition(resource, before, after));
        }

        // COMMON → state(コピーの前)と state → COMMON(コピーの後)
        void RecordAround(ID3D12GraphicsCommandList10* list, std::span<ID3D12Resource* const> sources,
                          std::span<ID3D12Resource* const> destinations, bool after) {
            constexpr D3D12_RESOURCE_STATES COMMON = D3D12_RESOURCE_STATE_COMMON;
            constexpr D3D12_RESOURCE_STATES SOURCE = D3D12_RESOURCE_STATE_COPY_SOURCE;
            constexpr D3D12_RESOURCE_STATES DESTINATION = D3D12_RESOURCE_STATE_COPY_DEST;

            std::vector<D3D12_RESOURCE_BARRIER> barriers;
            barriers.reserve(sources.size() + destinations.size());
            AppendTransitions(barriers, sources, after ? SOURCE : COMMON, after ? COMMON : SOURCE);
            AppendTransitions(barriers, destinations, after ? DESTINATION : COMMON, after ? COMMON : DESTINATION);
            list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }

    }  // namespace

    // --- 作る ---

    bool ProbeSim::CreateSavePoints(ID3D12Device5* device, uint32_t count) {
        m_savePoints.clear();
        const std::vector<StateRegion> regions = StateRegions(0);
        m_savePoints.resize(count);
        for (uint32_t index = 0; index < count; ++index) {
            SavePoint& savePoint = m_savePoints[index];
            for (const StateRegion& region : regions) {
                ComPtr<ID3D12Resource> buffer = gpu::CreateBuffer(device, region.bytes,
                                                                  gpu::BufferKind::UnorderedAccess);
                if (!buffer) {
                    Log(Channel::Sim, Level::Error, "保存点 {} を作れない({} バイト)", index, region.bytes);
                    m_savePoints.clear();
                    return false;
                }

                buffer->SetName(std::format(L"ProbeSim.savePoint{}.{}", index, savePoint.buffers.size()).c_str());
                savePoint.buffers.push_back(std::move(buffer));
            }
        }

        return true;
    }

    uint64_t ProbeSim::DefaultHeapBytes() const {
        uint64_t bytes = m_conductGraph->BackingMemoryBytes();
        const auto add = [&](const ComPtr<ID3D12Resource>& buffer) {
            if (buffer)
                bytes += buffer->GetDesc().Width;
        };

        for (const ComPtr<ID3D12Resource>& buffer :
             {m_cells, m_thermal, m_busySink, m_hashes, m_commandQueue, m_tickEvents, m_blockSchedule, m_speciesRemap,
              m_gasState, m_gasWork, m_liquidWork})
            add(buffer);

        rng::for_each(m_activeLists, add);
        rng::for_each(m_extractions, add);
        rng::for_each(m_reactionTable, add);
        if (m_physics)
            rng::for_each(m_physics->StateBuffers(), add);

        return bytes;
    }

    uint64_t ProbeSim::SavePointBytes() const {
        uint64_t bytes = 0;
        for (const StateRegion& region : StateRegions(0))
            bytes += region.bytes;

        return bytes;
    }

    // 刻み tick の境界で写す部分(並びは保存点のバッファの並び。tick で変わるのはセルと熱の世代の位置だけ)
    std::vector<ProbeSim::StateRegion> ProbeSim::StateRegions(uint64_t tick) const {
        const uint64_t generation = tick & 1;
        std::vector<StateRegion> regions = {
            {.resource = m_cells.Get(),
             .offset = generation * CELL_BYTES,
             .bytes = CELL_BYTES,
             .generationBytes = CELL_BYTES},
            {.resource = m_thermal.Get(),
             .offset = generation * THERMAL_BYTES,
             .bytes = THERMAL_BYTES,
             .generationBytes = THERMAL_BYTES},
            {.resource = m_blockSchedule.Get(), .bytes = PROBE_SCHEDULE_BYTES},
            {.resource = m_activeLists[0].Get(), .bytes = PROBE_ACTIVE_LIST_BYTES},
            {.resource = m_activeLists[1].Get(), .bytes = PROBE_ACTIVE_LIST_BYTES},
            {.resource = m_hashes.Get(), .bytes = PROBE_HASH_BYTES},
            {.resource = m_tickEvents.Get(), .bytes = PROBE_TICK_EVENT_BYTES},
        };

        if (m_physics) {
            for (const ComPtr<ID3D12Resource>& buffer : m_physics->StateBuffers())
                regions.push_back({.resource = buffer.Get(), .bytes = buffer->GetDesc().Width});
        }

        // 区画(T-0620): 気体の箱のセルと 1 刻みの帳簿(GpuGas)は毎刻み区画から作り直すので写さない
        if (m_gas) {
            regions.push_back({.resource = m_gasState.Get(), .bytes = m_gasState->GetDesc().Width});
            regions.push_back({.resource = m_gasWork.Get(), .bytes = m_gasWork->GetDesc().Width});
        }

        if (m_liquidWork)
            regions.push_back({.resource = m_liquidWork.Get(), .bytes = m_liquidWork->GetDesc().Width});

        return regions;
    }

    // --- 入力の約束 ---

    bool ProbeSim::ValidateSavePoints(const ProbeFrameInput& input) const {
        const bool saving = input.saveTo != NO_SAVE_POINT;
        const bool restoring = input.restoreFrom != NO_SAVE_POINT;
        if (input.adoptFrom != nullptr)
            return !saving && !restoring && ValidateAdopt(input);

        if (!saving && !restoring)
            return true;

        const uint32_t index = restoring ? input.restoreFrom : input.saveTo;
        const bool valid = !(saving && restoring) && m_initialized && input.firstUnit == 0 &&
                           index < m_savePoints.size() && (!restoring || m_savePoints[index].tick == input.firstTick);
        if (!valid) {
            Log(Channel::Sim, Level::Error,
                "保存点の使い方が不正: 写す {} 戻す {} 刻み {} 単位 {}(保存点の数 {}、初期化 {})",
                saving ? static_cast<int64_t>(input.saveTo) : -1,
                restoring ? static_cast<int64_t>(input.restoreFrom) : -1, input.firstTick, input.firstUnit,
                m_savePoints.size(), m_initialized);
        }

        return valid;
    }

    // 写しの早送りの約束(T-0412): 刻みの境界・両方とも初期化の後・同じ形(写す部分の並びと大きさ)・物理の刻みが firstTick
    bool ProbeSim::ValidateAdopt(const ProbeFrameInput& input) const {
        const ProbeSim& source = *input.adoptFrom;
        bool valid = &source != this && input.firstUnit == 0 && m_initialized && source.m_initialized &&
                     (m_physics == nullptr) == (source.m_physics == nullptr);
        if (valid && m_physics)
            valid = m_physicsInitialized && source.m_physics->Tick() == input.firstTick;

        if (valid) {
            const std::vector<StateRegion> own = StateRegions(0);
            const std::vector<StateRegion> other = source.StateRegions(0);
            valid = own.size() == other.size() &&
                    rng::equal(own, other,
                               [](const StateRegion& a, const StateRegion& b) { return a.bytes == b.bytes; });
        }

        if (!valid) {
            Log(Channel::Sim, Level::Error,
                "写しの早送りの写し方が不正: 刻み {} 単位 {}(初期化 {} / 写す元 {}、物理 {} / {})", input.firstTick,
                input.firstUnit, m_initialized, source.m_initialized, m_physics != nullptr,
                source.m_physics != nullptr);
        }

        return valid;
    }

    // --- 写す・戻す ---

    void ProbeSim::RecordSave(ID3D12GraphicsCommandList10* list, uint32_t index, uint64_t tick) {
        SavePoint& savePoint = m_savePoints[index];
        const std::vector<StateRegion> regions = StateRegions(tick);
        std::vector<ID3D12Resource*> sources;
        std::vector<ID3D12Resource*> destinations;
        for (size_t region = 0; region < regions.size(); ++region) {
            sources.push_back(regions[region].resource);
            destinations.push_back(savePoint.buffers[region].Get());
        }

        RecordAround(list, sources, destinations, false);
        for (size_t region = 0; region < regions.size(); ++region) {
            list->CopyBufferRegion(destinations[region], 0, regions[region].resource, regions[region].offset,
                                   regions[region].bytes);
        }

        RecordAround(list, sources, destinations, true);

        savePoint.tick = tick;
        savePoint.physics = m_physics ? m_physics->Cursor() : GpuPhysicsCursor{};
        // その境界の表(差し替えの後の最初の保存のときだけ写す。T-0548)
        if (savePoint.tableSerial != m_tableSerial) {
            savePoint.table = m_tableCopy;
            savePoint.tableSerial = m_tableSerial;
        }
    }

    // 世界を保存点の状態へ。その境界の表が今の表と違えば、表も結び直す(セルと熱のキャッシュは保存点がその表で作ったもの。
    // 差し替えをまたいで戻れる。T-0548・ADR-0246)
    bool ProbeSim::RecordRestore(ID3D12GraphicsCommandList10* list, FrameSlot& frame, uint32_t index) {
        const SavePoint& savePoint = m_savePoints[index];
        std::vector<StateRegion> from = StateRegions(savePoint.tick);
        for (size_t region = 0; region < from.size(); ++region)
            from[region] = {.resource = savePoint.buffers[region].Get(), .bytes = from[region].bytes};

        RecordOverwriteState(list, from);
        if (m_physics)
            m_physics->SetCursor(savePoint.physics);

        if (savePoint.tableSerial == m_tableSerial)
            return true;

        Log(Channel::Sim, Level::Info, "巻き戻し: 刻み {} の保存点の表へ戻す", savePoint.tick);
        if (!RecordTableBuffers(list, frame, savePoint.table, savePoint.tick))
            return false;

        m_tableSerial = savePoint.tableSerial;

        return true;
    }

    // 写しの早送り(T-0412): 写す元の S(tick)(その世代の位置)を、この世界へ。写す元のバッファは読むだけ
    void ProbeSim::RecordAdopt(ID3D12GraphicsCommandList10* list, const ProbeSim& source, uint64_t tick) {
        RecordOverwriteState(list, source.StateRegions(tick));
        if (m_physics)
            m_physics->SetCursor(source.m_physics->Cursor());
    }

    // from(StateRegions の順。resource の offset から bytes)を世界へ写し、コマンドキューを空にする(戻す・写しを取る)
    void ProbeSim::RecordOverwriteState(ID3D12GraphicsCommandList10* list, std::span<const StateRegion> from) {
        const std::vector<StateRegion> regions = StateRegions(0);
        std::vector<ID3D12Resource*> sources = {m_zeroQueueHeader.Get()};
        std::vector<ID3D12Resource*> destinations = {m_commandQueue.Get()};
        for (size_t region = 0; region < regions.size(); ++region) {
            sources.push_back(from[region].resource);
            destinations.push_back(regions[region].resource);
        }

        RecordAround(list, sources, destinations, false);
        for (size_t region = 0; region < regions.size(); ++region) {
            const StateRegion& state = regions[region];
            const StateRegion& source = from[region];
            if (state.generationBytes == 0) {
                list->CopyBufferRegion(state.resource, 0, source.resource, source.offset, state.bytes);
                continue;
            }

            // 2 世代とも S(t) にする(予定していないブロックは 2 世代が同じ値、という約束を保つ)
            for (uint64_t generation = 0; generation < 2; ++generation) {
                list->CopyBufferRegion(state.resource, generation * state.generationBytes, source.resource,
                                       source.offset, state.bytes);
            }
        }

        // コマンドキューを空に(末尾 = 先頭 = 0。CPU の控えは RecordFrame が入力を書く前に空にしている)
        list->CopyBufferRegion(m_commandQueue.Get(), 0, m_zeroQueueHeader.Get(), 0, PROBE_COMMAND_QUEUE_HEADER_BYTES);
        RecordAround(list, sources, destinations, true);
    }

    void ProbeSim::ResetCommandMirror() {
        m_commandTail = 0;
        m_queuedCommandCount = 0;
        m_queuedTicks.clear();
        m_hasEnqueued = false;
    }

}  // namespace bicameral::sim
