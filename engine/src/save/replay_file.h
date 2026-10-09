// replay_file.h — 再生ファイル(初期状態 + コマンドの列 + 刻みごとのハッシュ)の形と読み書き(T-0086、15 §2・06 §3)。
//
// 世界は「初期状態 + その刻みに当たるコマンド」だけで決まる(06 §1・D-205)ので、コマンドの列を持てば世界が再現できる。
// 刻みごとの状態のハッシュ S(t) は検査用(再生して同じハッシュ列になれば同じ世界。間引いてよい)。
//   書く: フレームのループ(--record)が、GPU のキューへ足したコマンドと、読み戻したハッシュを集めて終わりに書く。
//   読む: フレームのループ(--replay)が、コマンドを刻みに間に合うように GPU のキューへ足し、読み戻したハッシュを突き合わせる。
// 使い道はバグの再現・機種間の比較(04 §5)・発見の共有(D-110)・デモの撮影(15 §2)。
//
// ファイルの形(リトルエンディアン。形式の版を変えたら、古い版を読む変換とテストを足す。15 §1):
//   [0]  "BCRP"(4 バイト)   [4] 形式の版 u32   [8] 世界の種類 u32   [12] 表の数 u32(版 1 は予約で 0)
//   [16] 表の版 u64(初期状態の表)  [24] シード u64      [32] 初期状態の刻み u64
//   [40] 初期状態の差分のバイト数 u64   [48] コマンドの数 u64   [56] ハッシュの数 u64
//   [64] 初期状態の差分(8 バイトの境界まで 0 で埋める)→ コマンド × 64 バイト(sim/command.h)→ ハッシュ × 16 バイト(刻み u64・値 u64)
//   → (版 2〜)表 × 表の数(T-0170・T-0193・ADR-0050): 見出し 32 バイト(表の版 u64・印 u32〔1 = 改造された世界〕・パッケージの数 u32・
//     中身のバイト数 u64・名前のバイト数 u64)→ 名前(パッケージごとに 長さ u32 + バイト)→ 中身(script::TableBytes)。名前と中身はそれぞれ 8 バイトの境界まで 0 で埋める。
// 表は、初期状態の表と、記録の中で差し替えた表(PROBE_COMMAND_TYPE_TABLE の版)の全部。版の昇順に 1 つずつ。
// 中身は「合わせた表」の正準なバイト列なので、元のパッケージのフォルダが無くても同じ表をベイクし直せる(版 = 中身のハッシュで確かめる。script 側)。
// 版 1(T-0086〜T-0139)のファイルは表が無い形として読む(表の数 0。再生は今のパッケージの表を使う)。
// 読むときは大きさ・並び((targetTick, sequence) の昇順・ハッシュの刻みの昇順・表の版の昇順)を確かめ、合わなければ理由を返す。
// ここは CPU だけ(GPU も Luau も知らない。表の中身はバイト列のまま持つ)。
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

    inline constexpr uint32_t REPLAY_FORMAT_VERSION = 2;         // 書く版(2: 表の中身を持つ。T-0170・T-0193)
    inline constexpr uint32_t REPLAY_OLDEST_FORMAT_VERSION = 1;  // 読める一番古い版(1: 表が無い。T-0086)

    // 世界の種類(どのシミュのコマンドと状態か)。ファイルには u32 で書く
    enum class ReplayWorld : uint8_t {
        Probe = 1,  // 仮の世界(sim/probe_sim。T-0004〜)。初期状態は全部 0、表は無い
    };

    struct ReplayTickHash {
        uint64_t tick = 0;  // 状態 S(tick)(刻み tick の始め)
        uint64_t hash = 0;

        friend bool operator==(const ReplayTickHash&, const ReplayTickHash&) = default;
    };

    // 記録の中で使った反応表(T-0170・T-0193・ADR-0050)。中身があれば、パッケージのフォルダが無くても同じ表で再生できる
    struct ReplayTable {
        uint64_t version = 0;                // 表の版(script::TableVersion = 中身のハッシュ)
        std::vector<std::string> loadOrder;  // 読んだパッケージ(読んだ順。人が読む・知らせる用)
        bool modifiedWorld = false;          // 「改造された世界」の印(ADR-0031 の 4)
        std::string content;                 // 合わせた表の中身(script::TableBytes。バイト列のまま)

        friend bool operator==(const ReplayTable&, const ReplayTable&) = default;
    };

    struct ReplayFile {
        // --- どの世界の、どの状態から始まるか ---
        ReplayWorld world = ReplayWorld::Probe;
        uint64_t tableVersion = 0;            // 初期状態の反応表の版(script::TableVersion。版 1 のファイルは 0)
        uint64_t seed = 0;                    // 世界の生成のシード(D-312)
        uint64_t startTick = 0;               // 初期状態の刻み
        std::vector<std::byte> initialDelta;  // シードからの差分(05 §6)。仮の世界は空

        // --- 進め方と検査 ---
        std::vector<sim::Command> commands;      // (targetTick, sequence) の昇順
        std::vector<ReplayTickHash> tickHashes;  // 刻みの昇順

        // --- 表(版 2〜)---
        std::vector<ReplayTable> tables;  // 版の昇順。空でなければ tableVersion の表を含む(版 1 のファイルは空)

        friend bool operator==(const ReplayFile&, const ReplayFile&) = default;

        // 版 version の表(無ければ nullptr)
        [[nodiscard]] const ReplayTable* FindTable(uint64_t version) const;
    };

    // バイト列との変換(ファイルを介さないテスト用にも)
    [[nodiscard]] std::expected<std::vector<std::byte>, std::string> SerializeReplay(const ReplayFile& replay);
    [[nodiscard]] std::expected<ReplayFile, std::string> ParseReplay(std::span<const std::byte> bytes);

    [[nodiscard]] std::expected<void, std::string> WriteReplayFile(const fs::path& path, const ReplayFile& replay);
    [[nodiscard]] std::expected<ReplayFile, std::string> ReadReplayFile(const fs::path& path);

}  // namespace bicameral::save
