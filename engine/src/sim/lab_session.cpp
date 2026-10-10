// lab_session.cpp — 実験室の 1 回の実験(lab_session.h)。
#include "sim/lab_session.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <utility>

#include "core/log.h"

namespace bicameral::sim {

    std::expected<LabSession, std::string> LabSession::Create(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE queueType,
                                                              const BakedReactionTable& table, uint64_t tableVersion,
                                                              std::string tableBytes) {
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
        if (!tableBytes.empty())
            session.m_tableBytes.emplace(tableVersion, std::move(tableBytes));

        return session;
    }

    LabSession::LabSession(BakedReactionTable table, gpu::ImmediateQueue queue, gpu::DebugRing ring, GpuLabBox box)
        : m_table(std::move(table)),
          m_queue(std::move(queue)),
          m_ring(std::move(ring)),
          m_box(std::move(box)),
          m_ledgerColumns(LabBoxCapacity(m_table).ledgerColumns),
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

    bool LabSession::PlaceRegion(LabCellRange range, std::span<const SpeciesAmount> contents,
                                 uint32_t temperatureMilliKelvin) {
        return Queue(MakeLabFillRegionCommand(m_tick, m_sequence, range, contents, temperatureMilliKelvin));
    }

    bool LabSession::SetTemperatureRegion(LabCellRange range, uint32_t temperatureMilliKelvin) {
        return Queue(MakeLabTemperatureRegionCommand(m_tick, m_sequence, range, temperatureMilliKelvin));
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
    std::expected<void, std::string> LabSession::SwitchTable(uint64_t tableVersion, bool remapBox) {
        if (tableVersion == m_tableVersion)
            return {};

        const auto found = m_tables.find(tableVersion);
        if (found == m_tables.end())
            return std::unexpected(std::format("反応表の版 {:016x} をこの実験室は持っていない", tableVersion));

        // --- 物質の一覧が違えば、名前で付け替える(箱に当てるのは StepOne。ResetTo は新しい表で箱を作り直すので捨てる)---
        std::expected<SpeciesRemap, std::string> remap = SpeciesRemap{.identity = true};
        if (remapBox)
            remap = BuildSpeciesRemap(m_table, found->second);

        if (!remap)
            return std::unexpected(
                std::format("反応表の版 {:016x} へ物質を付け替えられない: {}", tableVersion, remap.error()));

        if (auto retired = m_box.ReplaceTable(found->second); !retired)
            return std::unexpected(retired.error());

        if (!remap->identity)
            m_boxRemap = std::move(*remap);

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
                if (auto switched = SwitchTable(*version, true); !switched)
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

        // --- 物質の一覧が変わった刻み: コマンドの前に箱の全部のセルを付け替える(GPU と CPU が同じ RxRemapCell。T-0242)---
        std::optional<SpeciesRemap> boxRemap = std::exchange(m_boxRemap, std::nullopt);
        if (boxRemap && !m_box.RecordSpeciesRemap(list, m_ring.GpuAddress(), PackSpeciesRemap(*boxRemap),
                                                  static_cast<uint32_t>(m_cpu.cells.size())))
            return std::unexpected("箱の物質の付け替えを記録できない");

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
        if (boxRemap) {
            m_lastRemapReport = RemapLabBox(m_cpu, *boxRemap, m_table);
            Log(Channel::Sim, Level::Info,
                "実験室: 刻み {} で物質を付け替えた(分けた物質 {} µmol・失った原子 {} + {} µmol・足したエネルギー {} "
                "mJ)",
                tick, m_lastRemapReport->decomposedMicromoles, m_lastRemapReport->remainderAtomMicromoles,
                m_lastRemapReport->overflowAtomMicromoles, m_lastRemapReport->energyDeltaMilliJoules);
        }

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
        if (auto switched = SwitchTable(tableVersion, false); !switched)
            return switched;

        m_boxRemap.reset();  // 新しい表で箱を作り直すので、付け替えは要らない
        m_lastRemapReport.reset();
        m_initialVersion = tableVersion;
        // 箱は 1 レベルで帳簿を使わない(いつも 0)。物質の数の違う表で作り直しても、GPU の箱の大きさ(作った時の列)に合わせる
        m_initial = MakeLabBoxNest(m_table);
        m_initial.capacity.ledgerColumns = m_ledgerColumns;
        m_initial.ledger.assign(size_t{multires::MR_LEDGER_LEVELS} * m_ledgerColumns, 0);
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
                return std::unexpected(
                    std::format("記録の反応表(版 {:016x})をこの実験室は持っていない(記録に表の中身が無い。版 3 "
                                "までの記録は記録した時の表で再生する)",
                                version));
            }
        }

        // --- 記録の表から表へ物質を付け替えられるか(T-0242)---
        if (auto chain = CheckTableChain(versions); !chain)
            return chain;

        if (auto reset = ResetTo(initialVersion); !reset)
            return reset;

        m_scheduled = recording.commands;
        m_replayHashes = recording.hashes;
        m_replayEnd = recording.tickCount;
        for (const Command& command : recording.commands)
            m_sequence = std::max(m_sequence, command.sequence + 1);  // 再生中に置いたコマンドは記録の後ろに並ぶ

        if (auto stepped = Step(static_cast<uint32_t>(recording.tickCount)); !stepped)
            return stepped;

        // --- 流し終えたら、次の刻みから最新の表(世界の表)に戻す印。物質を付け替えられない(単体の無い元素の物質を消す)なら
        //     記録の表のまま(Reset で最新の表の箱から始め直す。T-0242)---
        const std::array<uint64_t, 2> back = {m_tableVersion, m_latestVersion};
        if (m_tableVersion != m_latestVersion && CheckTableChain(back))
            m_pending.push_back(MakeLabTableCommand(m_tick, m_sequence++, m_latestVersion));
        else if (m_tableVersion != m_latestVersion)
            Log(Channel::Sim, Level::Warning,
                "実験室: 再生の後、最新の表(版 {:016x})へ物質を付け替えられないので記録の表のまま", m_latestVersion);

        return {};
    }

    LabRecording LabSession::Recording() const {
        LabRecording recording = {
            .tableVersion = m_initialVersion, .tickCount = m_tick, .commands = m_history, .hashes = m_hashes};

        // --- 使った表(刻み 0 と印)の中身。分かるものだけ、版の昇順(T-0217)---
        std::vector<uint64_t> versions = {m_initialVersion};
        for (const Command& command : m_history) {
            if (const auto version = LabTableVersionOf(command); version)
                versions.push_back(*version);
        }

        std::ranges::sort(versions);
        const auto [last, end] = std::ranges::unique(versions);
        versions.erase(last, end);
        for (const uint64_t version : versions) {
            if (const auto found = m_tableBytes.find(version); found != m_tableBytes.end())
                recording.tables.push_back({.version = version, .bytes = found->second});
        }

        return recording;
    }

    // --- 反応表の差し替え(T-0218・ADR-0055)---

    std::expected<void, std::string> LabSession::ChangeTable(const BakedReactionTable& table, uint64_t tableVersion,
                                                             std::string tableBytes) {
        if (tableVersion == 0)
            return std::unexpected("版の分からない表(版 0)には替えられない(記録の印が表を指せない)");

        // --- 物質の一覧が違う表: 箱の表から付け替えられるか確かめ、まだ刻んでいない置く操作の材料を新しい ID に(T-0242)---
        if (const auto box = BuildSpeciesRemap(m_table, table); !box)
            return std::unexpected(
                std::format("反応表の版 {:016x} へ物質を付け替えられない: {}", tableVersion, box.error()));

        const auto latest = m_tables.find(m_latestVersion);
        if (latest != m_tables.end()) {
            const auto pending = BuildSpeciesRemap(latest->second, table);
            if (!pending)
                return std::unexpected(pending.error());

            for (Command& command : m_pending)
                command = RemapLabCommand(command, *pending);
        }

        m_tables.insert_or_assign(tableVersion, table);
        if (!tableBytes.empty())
            m_tableBytes.insert_or_assign(tableVersion, std::move(tableBytes));
        m_latestVersion = tableVersion;

        // --- 次の刻みから替える印(刻む前に替え直したら、印は最後の 1 つ。今の表に戻したなら印は要らない)---
        DropPendingTableChange();
        if (tableVersion != m_tableVersion)
            m_pending.push_back(MakeLabTableCommand(m_tick, m_sequence++, tableVersion));

        return {};
    }

    std::expected<void, std::string> LabSession::AddTable(const BakedReactionTable& table, uint64_t tableVersion,
                                                          std::string tableBytes) {
        if (tableVersion == 0)
            return std::unexpected("版の分からない表(版 0)は足せない");

        m_tables.insert_or_assign(tableVersion, table);
        if (!tableBytes.empty())
            m_tableBytes.insert_or_assign(tableVersion, std::move(tableBytes));

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

        // --- 置いた時の表(刻み 0 の表と印)から最新の表へ、材料の物質 ID を名前で付け替える(T-0242)---
        const auto latest = m_tables.find(m_latestVersion);
        uint64_t era = m_initialVersion;
        for (Command& command : history) {
            if (const auto version = LabTableVersionOf(command); version) {
                era = *version;
                continue;
            }

            const auto from = m_tables.find(era);
            if (era == m_latestVersion || from == m_tables.end() || latest == m_tables.end())
                continue;

            const auto remap = BuildSpeciesRemap(from->second, latest->second);
            if (!remap)
                return std::unexpected(std::format("最新の表で流し直せない: {}", remap.error()));

            command = RemapLabCommand(command, *remap);
        }

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

    // 表の列(刻み 0 の表 → 印の表 → …)の隣どうしが、物質を名前で付け替えられるか(T-0242。どれも持っていること)
    std::expected<void, std::string> LabSession::CheckTableChain(std::span<const uint64_t> versions) const {
        for (size_t i = 1; i < versions.size(); ++i) {
            const auto from = m_tables.find(versions[i - 1]);
            const auto to = m_tables.find(versions[i]);
            if (from == m_tables.end() || to == m_tables.end() || versions[i - 1] == versions[i])
                continue;

            if (const auto remap = BuildSpeciesRemap(from->second, to->second); !remap) {
                return std::unexpected(std::format("記録の反応表(版 {:016x} → {:016x})の物質を付け替えられない: {}",
                                                   versions[i - 1], versions[i], remap.error()));
            }
        }

        return {};
    }

}  // namespace bicameral::sim
