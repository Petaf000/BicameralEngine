// replay_session.cpp — 記録と再生の帳簿。使い方は replay_session.h。
#include "save/replay_session.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <utility>

#include "core/aliases.h"
#include "core/log.h"

namespace bicameral::save {
    namespace {

        constexpr uint64_t MAX_LOGGED_MISMATCHES = 8;

    }  // namespace

    // --- 記録 ---

    void ReplayRecorder::AddCommands(std::span<const sim::Command> commands) {
        m_commands.insert(m_commands.end(), commands.begin(), commands.end());
    }

    void ReplayRecorder::AddHash(uint64_t tick, uint64_t hash) {
        m_hashes.push_back({.tick = tick, .hash = hash});
    }

    // S(lastTick) まで確かめられるので、その状態に入るコマンド(targetTick < lastTick)だけを残す
    ReplayFile ReplayRecorder::Build() const {
        ReplayFile replay{.world = ReplayWorld::Probe, .startTick = 0, .tickHashes = m_hashes};
        const uint64_t lastTick = m_hashes.empty() ? 0 : m_hashes.back().tick;
        rng::copy_if(m_commands, std::back_inserter(replay.commands),
                     [&](const sim::Command& command) { return command.targetTick < lastTick; });

        return replay;
    }

    std::expected<void, std::string> ReplayRecorder::Write(const fs::path& path) const {
        return WriteReplayFile(path, Build());
    }

    // --- 再生 ---

    ReplayPlayer::ReplayPlayer(ReplayFile replay) : m_replay(std::move(replay)) {}

    std::expected<ReplayPlayer, std::string> ReplayPlayer::Load(const fs::path& path) {
        auto replay = ReadReplayFile(path);
        if (!replay)
            return std::unexpected(replay.error());

        if (replay->world != ReplayWorld::Probe || replay->startTick != 0 || !replay->initialDelta.empty())
            return std::unexpected("仮の世界は刻み 0 の全部 0 の状態からしか再生できない");

        return ReplayPlayer(std::move(*replay));
    }

    std::vector<sim::Command> ReplayPlayer::TakeCommands(uint64_t applyTick, uint32_t limit) {
        std::vector<sim::Command> commands;
        while (m_nextCommand < m_replay.commands.size() && commands.size() < limit) {
            const sim::Command& command = m_replay.commands[m_nextCommand];
            if (command.targetTick >= applyTick + LOOKAHEAD_TICKS)
                break;

            ++m_nextCommand;
            if (command.targetTick < applyTick) {
                ++m_lateCommands;

                Log(Channel::Sim, Level::Error,
                    "再生: 刻み {} のコマンド(番号 {})が適用に間に合わなかった(次の適用は刻み {})", command.targetTick,
                    command.sequence, applyTick);

                continue;
            }

            commands.push_back(command);
        }

        return commands;
    }

    void ReplayPlayer::Rewind(uint64_t tick) {
        const auto& commands = m_replay.commands;
        const auto& hashes = m_replay.tickHashes;
        m_nextCommand = static_cast<size_t>(
            rng::find_if(commands, [&](const sim::Command& command) { return command.targetTick >= tick; }) -
            commands.begin());
        m_nextHash = static_cast<size_t>(
            rng::find_if(hashes, [&](const ReplayTickHash& entry) { return entry.tick > tick; }) - hashes.begin());
    }

    void ReplayPlayer::CheckHash(uint64_t tick, uint64_t hash) {
        const auto& expected = m_replay.tickHashes;
        while (m_nextHash < expected.size() && expected[m_nextHash].tick < tick) {
            // 読み戻しに無かった刻み(あり得ないはず。ハッシュは全部の刻みで戻る)
            Log(Channel::Sim, Level::Error, "再生: S({}) のハッシュが戻ってこなかった", expected[m_nextHash].tick);
            ++m_mismatches;
            ++m_nextHash;
        }

        // ファイルが間引いた刻み
        if (m_nextHash == expected.size() || expected[m_nextHash].tick != tick)
            return;

        if (expected[m_nextHash].hash == hash)
            ++m_matches;
        else {
            if (m_mismatches < MAX_LOGGED_MISMATCHES) {
                Log(Channel::Sim, Level::Error, "再生: S({}) = {:016x}(記録は {:016x})", tick, hash,
                    expected[m_nextHash].hash);
            }

            ++m_mismatches;
        }

        ++m_nextHash;
    }

}  // namespace bicameral::save
