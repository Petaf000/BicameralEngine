// replay_file.h — 再生ファイル(初期状態 + コマンドの列 + 刻みごとのハッシュ)の形と読み書き(T-0086、15 §2・06 §3)。
//
// 世界は「初期状態 + その刻みに当たるコマンド」だけで決まる(06 §1・D-205)ので、コマンドの列を持てば世界が再現できる。
// 刻みごとの状態のハッシュ S(t) は検査用(再生して同じハッシュ列になれば同じ世界。間引いてよい)。
//   書く: フレームのループ(--record)が、GPU のキューへ足したコマンドと、読み戻したハッシュを集めて終わりに書く。
//   読む: フレームのループ(--replay)が、コマンドを刻みに間に合うように GPU のキューへ足し、読み戻したハッシュを突き合わせる。
// 使い道はバグの再現・機種間の比較(04 §5)・発見の共有(D-110)・デモの撮影(15 §2)。
//
// ファイルの形(リトルエンディアン。形式の版を変えたら、古い版を読む変換とテストを足す。15 §1):
//   [0]  "BCRP"(4 バイト)   [4] 形式の版 u32   [8] 世界の種類 u32   [12] 予約 u32(0)
//   [16] 表の版 u64          [24] シード u64      [32] 初期状態の刻み u64
//   [40] 初期状態の差分のバイト数 u64   [48] コマンドの数 u64   [56] ハッシュの数 u64
//   [64] 初期状態の差分(8 バイトの境界まで 0 で埋める)→ コマンド × 64 バイト(sim/command.h)→ ハッシュ × 16 バイト(刻み u64・値 u64)
// 読むときは大きさ・並び((targetTick, sequence) の昇順・ハッシュの刻みの昇順)を確かめ、合わなければ理由を返す。
// ここは CPU だけ(GPU を知らない)。
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "core/aliases.h"
#include "sim/command.h"

namespace bicameral::save {

    inline constexpr uint32_t REPLAY_FORMAT_VERSION = 1;

    // 世界の種類(どのシミュのコマンドと状態か)。ファイルには u32 で書く
    enum class ReplayWorld : uint8_t {
        Probe = 1,  // 仮の世界(sim/probe_sim。T-0004〜)。初期状態は全部 0、表は無い
    };

    struct ReplayTickHash {
        uint64_t tick = 0;  // 状態 S(tick)(刻み tick の始め)
        uint64_t hash = 0;

        friend bool operator==(const ReplayTickHash&, const ReplayTickHash&) = default;
    };

    struct ReplayFile {
        ReplayWorld world = ReplayWorld::Probe;
        uint64_t tableVersion = 0;               // 反応表などの版(パッケージの一覧のハッシュ。13 §2)。仮の世界は 0
        uint64_t seed = 0;                       // 世界の生成のシード(D-312)
        uint64_t startTick = 0;                  // 初期状態の刻み
        std::vector<std::byte> initialDelta;     // シードからの差分(05 §6)。仮の世界は空
        std::vector<sim::Command> commands;      // (targetTick, sequence) の昇順
        std::vector<ReplayTickHash> tickHashes;  // 刻みの昇順

        friend bool operator==(const ReplayFile&, const ReplayFile&) = default;
    };

    // バイト列との変換(ファイルを介さないテスト用にも)
    [[nodiscard]] expected<std::vector<std::byte>, std::string> SerializeReplay(const ReplayFile& replay);
    [[nodiscard]] expected<ReplayFile, std::string> ParseReplay(span<const std::byte> bytes);

    [[nodiscard]] expected<void, std::string> WriteReplayFile(const fs::path& path, const ReplayFile& replay);
    [[nodiscard]] expected<ReplayFile, std::string> ReadReplayFile(const fs::path& path);

}  // namespace bicameral::save
