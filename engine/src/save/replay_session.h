// replay_session.h — 走っている世界を再生ファイルに記録する側(ReplayRecorder)と、再生ファイルを流して確かめる側(ReplayPlayer)(T-0086、15 §2)。
//
// どちらも CPU の帳簿だけ(GPU を知らない)。フレームのループ(frame/frame_loop.cpp)が次のように使う:
//   記録: GPU のキューへ足したコマンドを AddCommands、読み戻した刻みごとのハッシュを AddHash → 終わるときに Write。
//         書くのは「最後にハッシュを読み戻した刻み」までに適用されるコマンドだけ(その先の刻みの結果は確かめられないので)。
//   再生: 毎フレーム TakeCommands(まだ記録していない最初の適用の刻み, 足せる数)で、刻みに間に合う分だけ(先読みは数刻み)GPU のキューへ足す。
//         読み戻したハッシュを CheckHash に渡す。ファイルの最後のハッシュまで確かめたら Finished()。
//         間に合わなかったコマンド(キューの空きが足りずに刻みを過ぎた)とハッシュの不一致は失敗として数える。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "core/aliases.h"
#include "save/replay_file.h"

namespace bicameral::save {

    class ReplayRecorder {
    public:
        void AddCommands(std::span<const sim::Command> commands);
        void AddHash(uint64_t tick, uint64_t hash);

        // 記録を再生ファイルの形にする(仮の世界。刻み 0 の全部 0 の状態から)
        [[nodiscard]] ReplayFile Build() const;
        [[nodiscard]] std::expected<void, std::string> Write(const fs::path& path) const;

    private:
        std::vector<sim::Command> m_commands;
        std::vector<ReplayTickHash> m_hashes;
    };

    class ReplayPlayer {
    public:
        static constexpr uint64_t LOOKAHEAD_TICKS = 8;  // 何刻み先のコマンドまで先に GPU のキューへ足すか

        explicit ReplayPlayer(ReplayFile replay);

        // 仮の世界の再生ファイルを読む(刻み 0 の全部 0 の状態から始まるものだけ)
        [[nodiscard]] static std::expected<ReplayPlayer, std::string> Load(const fs::path& path);

        // このフレームに GPU のキューへ足すコマンド(targetTick < applyTick + LOOKAHEAD_TICKS のものを、最大 limit 個)。
        // applyTick = まだ記録していない最初の適用の刻み。それより前のコマンドは間に合わないので捨てて失敗に数える
        [[nodiscard]] std::vector<sim::Command> TakeCommands(uint64_t applyTick, uint32_t limit);

        // 読み戻した S(tick) のハッシュを、ファイルにあれば突き合わせる(ファイルは間引いてあってよい)
        void CheckHash(uint64_t tick, uint64_t hash);

        // 巻き戻し(保存点 + 再生。T-0143): 世界が刻み tick の境界へ戻った。tick 以降のコマンドをもう一度足し、
        // S(tick + 1) からのハッシュをもう一度突き合わせる(同じコマンドなら同じハッシュになるはず。ADR-0008)
        void Rewind(uint64_t tick);

        // --- 結果 ---
        [[nodiscard]] bool Finished() const { return m_nextHash == m_replay.tickHashes.size(); }
        [[nodiscard]] bool Passed() const { return Finished() && m_mismatches == 0 && m_lateCommands == 0; }

        [[nodiscard]] uint64_t Matches() const { return m_matches; }
        [[nodiscard]] uint64_t Mismatches() const { return m_mismatches; }
        [[nodiscard]] uint64_t LateCommands() const { return m_lateCommands; }

        // --- ファイルの中身 ---
        [[nodiscard]] size_t HashCount() const { return m_replay.tickHashes.size(); }
        [[nodiscard]] size_t CommandCount() const { return m_replay.commands.size(); }
        [[nodiscard]] const ReplayFile& File() const { return m_replay; }
        [[nodiscard]] uint64_t LastTick() const {
            return m_replay.tickHashes.empty() ? 0 : m_replay.tickHashes.back().tick;
        }

    private:
        ReplayFile m_replay;

        // --- どこまで進んだか ---
        size_t m_nextCommand = 0;  // 次に足すコマンド
        size_t m_nextHash = 0;     // 次に突き合わせるハッシュ

        // --- 数えたもの ---
        uint64_t m_matches = 0;
        uint64_t m_mismatches = 0;
        uint64_t m_lateCommands = 0;
    };

}  // namespace bicameral::save
