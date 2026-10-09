// lab_session.cpp — 実験室の 1 回の実験(lab_session.h)。
#include "sim/lab_session.h"

#include <algorithm>
#include <format>

namespace bicameral::sim {

    std::expected<LabSession, std::string> LabSession::Create(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE queueType,
                                                              const BakedReactionTable& table) {
        auto queue = gpu::ImmediateQueue::Create(device, queueType);
        if (!queue)
            return std::unexpected(queue.error());

        auto ring = gpu::DebugRing::Create(device);
        if (!ring)
            return std::unexpected(ring.error());

        auto box = GpuLabBox::Create(device, table);
        if (!box)
            return std::unexpected(box.error());

        return LabSession(table, std::move(*queue), std::move(*ring), std::move(*box));
    }

    LabSession::LabSession(BakedReactionTable table, gpu::ImmediateQueue queue, gpu::DebugRing ring, GpuLabBox box)
        : m_table(std::move(table)),
          m_queue(std::move(queue)),
          m_ring(std::move(ring)),
          m_box(std::move(box)),
          m_cpu(MakeLabBoxNest(m_table)),
          m_read(MakeLabBoxNest(m_table)) {}

    // --- 置く ---

    bool LabSession::Queue(const Command& command) {
        if (m_pending.size() >= LAB_MAX_COMMANDS_PER_TICK)
            return false;

        m_pending.push_back(command);
        m_sequence += 1;

        return true;
    }

    bool LabSession::Place(LabCellPosition cell, std::span<const SpeciesAmount> contents,
                           uint32_t temperatureMilliKelvin) {
        return Queue(MakeLabFillCommand(m_tick, m_sequence, cell, contents, temperatureMilliKelvin));
    }

    bool LabSession::SetTemperature(LabCellPosition cell, uint32_t temperatureMilliKelvin) {
        return Queue(MakeLabTemperatureCommand(m_tick, m_sequence, cell, temperatureMilliKelvin));
    }

    // --- 刻む ---

    // 刻み tick のコマンド(置いたもの + 再生中の記録)を (targetTick, sequence) の順に
    std::vector<Command> LabSession::TakeCommands(uint64_t tick) {
        std::vector<Command> commands;
        for (Command command : m_pending) {
            command.targetTick = tick;  // 置いた後に刻みが進んでいなければ同じ値
            commands.push_back(command);
        }

        m_pending.clear();
        while (!m_scheduled.empty() && m_scheduled.front().targetTick == tick) {
            commands.push_back(m_scheduled.front());
            m_scheduled.erase(m_scheduled.begin());
        }

        std::ranges::sort(commands, CommandPrecedes);
        if (commands.size() > LAB_MAX_COMMANDS_PER_TICK)
            commands.resize(LAB_MAX_COMMANDS_PER_TICK);

        return commands;
    }

    std::expected<void, std::string> LabSession::StepOne() {
        const uint64_t tick = m_tick;
        const std::vector<Command> commands = TakeCommands(tick);

        // --- GPU: 1 本のリスト(最初だけ初めの箱を写す)---
        ID3D12GraphicsCommandList10* list = m_queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        m_ring.RecordBegin(list);
        if (!m_uploaded && !m_box.RecordUpload(list, MakeLabBoxNest(m_table)))
            return std::unexpected("初めの箱を写せない");

        if (!m_box.RecordTick(list, m_ring.GpuAddress(), tick, commands))
            return std::unexpected("刻みを記録できない");

        m_box.RecordReadback(list);
        m_ring.RecordReadbackAndReset(list);
        if (!m_queue.ExecuteAndWait())
            return std::unexpected("GPU での実行に失敗");

        m_uploaded = true;
        if (const gpu::DebugRingContents debug = m_ring.Drain(); debug.assertCount > 0)
            return std::unexpected(std::format("GPU の FX_ASSERT が {} 件(刻み {})", debug.assertCount, tick));

        if (!m_box.Read(m_read))
            return std::unexpected("箱を読み戻せない");

        // --- CPU リファレンス → 比べる ---
        StepLabBox(m_cpu, m_table, tick, commands);
        m_mismatch = FindLabMismatch(m_cpu, m_read, tick);

        m_history.insert(m_history.end(), commands.begin(), commands.end());
        m_hashes.push_back(HashWholeNest(m_cpu));
        if (tick < m_replayHashes.size() && m_replayHashes[tick] != m_hashes.back() && !m_replayDivergence)
            m_replayDivergence = tick;

        m_tick += 1;

        return {};
    }

    std::expected<void, std::string> LabSession::Step(uint32_t tickCount) {
        for (uint32_t i = 0; i < tickCount && !m_mismatch; ++i) {
            if (auto stepped = StepOne(); !stepped)
                return stepped;
        }

        return {};
    }

    // --- 戻す・再生 ---

    std::expected<void, std::string> LabSession::Reset() {
        m_cpu = MakeLabBoxNest(m_table);
        m_read = MakeLabBoxNest(m_table);
        m_uploaded = false;  // 次の刻みのリストの先頭で、初めの箱を写し直す
        m_tick = 0;
        m_sequence = 0;
        m_pending.clear();
        m_scheduled.clear();
        m_history.clear();
        m_hashes.clear();
        m_mismatch.reset();
        m_replayHashes.clear();
        m_replayEnd = 0;
        m_replayDivergence.reset();

        return {};
    }

    std::expected<void, std::string> LabSession::Replay(const LabRecording& recording) {
        if (auto reset = Reset(); !reset)
            return reset;

        m_scheduled = recording.commands;
        m_replayHashes = recording.hashes;
        m_replayEnd = recording.tickCount;
        for (const Command& command : recording.commands)
            m_sequence = std::max(m_sequence, command.sequence + 1);  // 再生中に置いたコマンドは記録の後ろに並ぶ

        return Step(static_cast<uint32_t>(recording.tickCount));
    }

    LabRecording LabSession::Recording() const {
        return {.tickCount = m_tick, .commands = m_history, .hashes = m_hashes};
    }

}  // namespace bicameral::sim
