// reaction_table_loader.h — ランタイム(ゲーム本体・エディタ)とテストが、データのフォルダのパッケージから反応表を作る(T-0157・ADR-0033)。
//
// データの流れ: <exe の横>/data/packages(ビルドがリポジトリの data/packages を写す)→ ReadPackageRoot
//   (+ 環境ミキサーで足した標準パッケージのフォルダ。ReactionTableSource::addedPackages。T-0373)
//   (+ それらが依存しているのに無い標準パッケージを棚から。AddStandardDependencies。T-0498)
//   → LoadPackages(走らせる前に data/types の型の定義で型検査〔T-0140〕→ 殻)
//   → ReadReactionTableDefinition → sim::BakeReactionTable(元素とエネルギーの検査)→ LoadedReactionTable → GPU へ上げる(呼び手)。
// 失敗(フォルダが無い・ゲーム本体のパッケージが読めない・形の誤り・検査で落ちる)は全部 error で返す。呼び手は起動を止める
// (半端な表で世界を動かさない。D-428)。ゲーム本体以外のパッケージ(Mod)が読めなかったときは、それを除いた表で続け、
// 理由を rejected に残す(ADR-0031 の 3)。呼び手がログに出す。
// 再生ファイルの表は RebuildReactionTable が中身(TableBytes)からフォルダ無しで作り直す(T-0193)。
// ここは CPU だけ(原則 2: 表は CPU でベイクして GPU へ渡す。起動時に 1 回)。
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "core/aliases.h"
#include "script/luau_package.h"
#include "sim/reaction_table.h"

namespace bicameral::script {

    struct ReactionTableSource {
        fs::path packageRoot;  // 直下のフォルダが 1 つずつパッケージ(空なら DefaultPackageRoot)
        // ゲーム本体は、読むパッケージのうちマニフェストに game = true と書いたもの(名前に依らない。1 つまで。T-0557・ADR-0250)。
        // 見つかれば、読めなければ失敗。requireGame: 無ければ失敗(サンプルの世界・場面)。false なら無い時は全部 Mod(ブランクの世界)
        bool requireGame = true;
        MergeMode mergeMode = MergeMode::AddOnly;  // 表の合わせ方(仮で案 C の既定。QUESTIONS Q8)
        fs::path typeDefinitions;  // 型検査に使う型の定義(T-0140。空なら DefaultTypeDefinitionsPath。読めなければ失敗)

        // --- 環境ミキサーで足した標準パッケージ(T-0373・ADR-0183・script/standard_packages.h)---
        bool readPackageRoot = true;          // false: packageRoot を読まない(ブランクの世界に標準パッケージだけを足す)
        std::vector<fs::path> addedPackages;  // 1 つのフォルダ = 1 つのパッケージ。packageRoot のものと一緒に読む

        // 読むパッケージが依存しているのに無い標準パッケージを探す棚(空なら DefaultStandardPackageRoot。T-0498)
        fs::path standardRoot;
    };

    struct LoadedReactionTable {
        sim::BakedReactionTable table;  // GPU に上げる表と CPU だけの名前など(warnings は文献の反応熱との食い違い)
        fs::path packageRoot;           // 実際に読んだフォルダ
        fs::path standardRoot;  // 依存の標準パッケージを探した棚(T-0498。エディタの表のパネルが書き戻す先を探す)
        std::vector<std::string> loadOrder;      // 読めたパッケージ(読んだ順)
        std::string gamePackage;                 // ゲーム本体(game = true のパッケージ。無ければ空。T-0557)
        std::vector<PackageRejection> rejected;  // 読まなかった Mod と理由(ゲーム本体はここに来ない。来たら失敗)
        std::vector<TableOverride> overrides;    // 上書き(OverrideMarked・Override のとき)
        bool modifiedWorld = false;              // 「改造された世界」の印
        uint64_t tableVersion = 0;               // 合わせた表の版(TableVersion。同じ中身なら同じ値)
        std::string tableBytes;  // 合わせた表の中身(TableBytes。再生ファイルに残し、フォルダ無しで作り直す。T-0193)
    };

    // 既定のパッケージのフォルダ: <exe のフォルダ>/data/packages
    [[nodiscard]] fs::path DefaultPackageRoot();

    // パッケージのフォルダから反応表を読んでベイクする。誤りには「どのフォルダ・どのパッケージ・どの欄か」を付ける
    [[nodiscard]] std::expected<LoadedReactionTable, std::string> LoadReactionTable(const ReactionTableSource& source);

    // ブランクの世界の表(D-455・ADR-0151 の 3・T-0371): パッケージを 1 つも読まない空の表(物質は使わない 0 番だけ・規則 0)。
    // 版と中身は空の表のもの(再生ファイルに残し、RebuildReactionTable で同じ表に作り直せる)。packageRoot は空
    [[nodiscard]] std::expected<LoadedReactionTable, std::string> LoadBlankReactionTable();

    // 再生ファイルに残した表の中身(TableBytes)から、パッケージのフォルダ無しで同じ表を作り直す(T-0193・ADR-0050)。
    // 中身のハッシュと、作り直した表の版が expectedVersion と同じでなければ失敗(壊れた・書き換えた中身で世界を動かさない)。
    // Luau は走らせない(中身は値だけ)。loadOrder・modifiedWorld は呼び手が再生ファイルから埋める
    [[nodiscard]] std::expected<LoadedReactionTable, std::string> RebuildReactionTable(std::string_view tableBytes,
                                                                                       uint64_t expectedVersion);

}  // namespace bicameral::script
