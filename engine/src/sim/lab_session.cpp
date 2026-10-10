// lab_session.cpp — 実験室の 1 回の実験(lab_session.h)。
#include "sim/lab_session.h"

#include <algorithm>
#include <cstddef>
#include <format>

namespace bicameral::sim {

    std::expected<LabSession, std::string> LabSession::Create(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE queueType,
                                                              const BakedReactionTable& table, uint64_t tableVersion) {
        auto queue = gpu::ImmediateQueue::Create(device, queueType);
        if (!queue)
            return std::unexpected(queue.error());

        auto ring = gpu::DebugRing::Create(device);
        if (!ring)
            return std::unexpected(ring.error());

        auto box = GpuLabBox::Create(device, table);
        if (!box)
            return std::unexpected(box.error());

        LabSession session(table, std::move(*queue), std::move(*ring), std::move(*box));
        session.m_tableVersion = tableVersion;
        session.m_latestVersion = tableVersion;
        session.m_initialVersion = tableVersion;
        session.m_tables.emplace(tableVersion, table);

        return session;
    }

    LabSession::LabSession(BakedReactionTable table, gpu::ImmediateQueue queue, gpu::DebugRing ring, GpuLabBox box)
        : m_table(std::move(table)),
          m_queue(std::move(queue)),
          m_ring(std::move(ring)),
          m_box(std::move(box)),
          m_initial(MakeLabBoxNest(m_table)),
          m_cpu(m_initial),
          m_read(m_initial) {}

    // --- 置く ---

    // 置く操作は 1 刻みに LAB_MAX_COMMANDS_PER_TICK − 1 まで(1 つは表を替えた印のために空けておく)
    bool LabSession::Queue(const Command& command) {
        const auto placed = std::ranges::count_if(m_pending, [](const Command& c) { return !LabTableVersionOf(c); });
        if (static_cast<size_t>(placed) + 1 >= LAB_MAX_COMMANDS_PER_TICK)
            return false;

        m_pending.push_back(command);
        m_sequence += 1;

        return true;
    }

    void LabSession::DropPendingTableChange() {
        std::erase_if(m_pending, [](const Command& command) { return LabTableVersionOf(command).has_value(); });
    }

    bool LabSession::TableChangePending() const {
        return std::ranges::any_of(m_pending,
                                   [](const Command& command) { return LabTableVersionOf(command).has_value(); });
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

        // --- 多すぎたら後ろの置く操作から捨てる(表を替えた印は捨てない。GPU の写しの大きさ)---
        for (size_t i = commands.size(); commands.size() > LAB_MAX_COMMANDS_PER_TICK && i > 0; --i) {
            if (!LabTableVersionOf(commands[i - 1]))
                commands.erase(commands.begin() + static_cast<std::ptrdiff_t>(i - 1));
        }

        return commands;
    }

    // 箱の表を版 tableVersion の表に替える(GPU と CPU の両方。箱のキューは刻みごとに待っていて空なので、前の表のバッファはすぐ捨ててよい)
    std::expected<void, std::string> LabSession::SwitchTable(uint64_t tableVersion) {
        if (tableVersion == m_tableVersion)
            return {};

        const auto found = m_tables.find(tableVersion);
        if (found == m_tables.end())
            return std::unexpected(std::format("反応表の版 {:016x} をこの実験室は持っていない", tableVersion));

        if (auto retired = m_box.ReplaceTable(found->second); !retired)
            return std::unexpected(retired.error());

        m_table = found->second;
        m_tableVersion = tableVersion;

        return {};
    }

    std::expected<void, std::string> LabSession::StepOne() {
        const uint64_t tick = m_tick;
        const std::vector<Command> commands = TakeCommands(tick);

        // --- 表を替えた印: この刻みのコマンドを当てる前に替える(印そのものは箱をつつく。ADR-0055)---
        for (const Command& command : commands) {
            if (const auto version = LabTableVersionOf(command); version) {
                if (auto switched = SwitchTable(*version); !switched)
                    return switched;
            }
        }

        // --- GPU: 1 本のリスト(最初だけ初めの箱を写す)---
        ID3D12GraphicsCommandList10* list = m_queue.Begin();
        if (list == nullptr)
            return std::unexpected("コマンドリストを始められない");

        m_ring.RecordBegin(list);
        if (!m_uploaded && !m_box.RecordUpload(list, m_initial))
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
        return ResetTo(m_latestVersion);
    }

    std::expected<void, std::string> LabSession::ResetTo(uint64_t tableVersion) {
        if (auto switched = SwitchTable(tableVersion); !switched)
            return switched;

        m_initialVersion = tableVersion;
        m_initial = MakeLabBoxNest(m_table);
        m_cpu = m_initial;
        m_read = m_initial;
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
        // --- 記録の表(刻み 0 と印)を全部持っているか、流す前に確かめる(途中で止まらないように)---
        const uint64_t initialVersion = recording.tableVersion != 0 ? recording.tableVersion : m_latestVersion;
        std::vector<uint64_t> versions = {initialVersion};
        for (const Command& command : recording.commands) {
            if (const auto version = LabTableVersionOf(command); version)
                versions.push_back(*version);
        }

        for (const uint64_t version : versions) {
            if (!m_tables.contains(version)) {
                return std::unexpected(std::format(
                    "記録の反応表(版 "
                    "{:016x})をこの実験室は持っていない。記録した時の表で再生する(表の中身を記録に残すのは T-0217)",
                    version));
            }
        }

        if (auto reset = ResetTo(initialVersion); !reset)
            return reset;

        m_scheduled = recording.commands;
        m_replayHashes = recording.hashes;
        m_replayEnd = recording.tickCount;
        for (const Command& command : recording.commands)
            m_sequence = std::max(m_sequence, command.sequence + 1);  // 再生中に置いたコマンドは記録の後ろに並ぶ

        if (auto stepped = Step(static_cast<uint32_t>(recording.tickCount)); !stepped)
            return stepped;

        // --- 流し終えたら、次の刻みから最新の表(世界の表)に戻す印 ---
        if (m_tableVersion != m_latestVersion)
            m_pending.push_back(MakeLabTableCommand(m_tick, m_sequence++, m_latestVersion));

        return {};
    }

    LabRecording LabSession::Recording() const {
        return {.tableVersion = m_initialVersion, .tickCount = m_tick, .commands = m_history, .hashes = m_hashes};
    }

    // --- 反応表の差し替え(T-0218・ADR-0055)---

    std::expected<void, std::string> LabSession::ChangeTable(const BakedReactionTable& table, uint64_t tableVersion) {
        if (tableVersion == 0)
            return std::unexpected("版の分からない表(版 0)には替えられない(記録の印が表を指せない)");

        m_tables.insert_or_assign(tableVersion, table);
        m_latestVersion = tableVersion;

        // --- 次の刻みから替える印(刻む前に替え直したら、印は最後の 1 つ。今の表に戻したなら印は要らない)---
        DropPendingTableChange();
        if (tableVersion != m_tableVersion)
            m_pending.push_back(MakeLabTableCommand(m_tick, m_sequence++, tableVersion));

        return {};
    }

    std::expected<void, std::string> LabSession::RerunWithLatestTable() {
        // --- 今までの操作(表の印を除く)を、初めの箱から最新の表で同じ刻みまで(置いてまだ刻んでいない操作は後で戻す。
        //     再生の途中なら、残りの記録のコマンドも続けて流れる。記録のハッシュとはもう比べない)---
        const auto isTableMark = [](const Command& command) {
            return LabTableVersionOf(command).has_value();
        };
        const uint64_t tickCount = m_tick;
        std::vector<Command> history = std::move(m_history);
        history.insert(history.end(), m_scheduled.begin(), m_scheduled.end());
        std::erase_if(history, isTableMark);
        std::vector<Command> pending = std::move(m_pending);
        std::erase_if(pending, isTableMark);
        if (auto reset = Reset(); !reset)
            return reset;

        m_scheduled = std::move(history);
        for (const Command& command : m_scheduled)
            m_sequence = std::max(m_sequence, command.sequence + 1);

        auto stepped = Step(static_cast<uint32_t>(tickCount));
        for (const Command& command : pending)
            m_sequence = std::max(m_sequence, command.sequence + 1);

        m_pending = std::move(pending);

        return stepped;
    }

}  // namespace bicameral::sim
