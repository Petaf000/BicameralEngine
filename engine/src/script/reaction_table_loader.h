// reaction_table_loader.h — ランタイム(ゲーム本体・エディタ)とテストが、データのフォルダのパッケージから反応表を作る(T-0157・ADR-0033)。
//
// データの流れ: <exe の横>/data/packages(ビルドがリポジトリの data/packages を写す)→ ReadPackageRoot → LoadPackages(殻)
//   → ReadReactionTableDefinition → sim::BakeReactionTable(元素とエネルギーの検査)→ LoadedReactionTable → GPU へ上げる(呼び手)。
// 失敗(フォルダが無い・ゲーム本体のパッケージが読めない・形の誤り・検査で落ちる)は全部 error で返す。呼び手は起動を止める
// (半端な表で世界を動かさない。D-428)。ゲーム本体以外のパッケージ(Mod)が読めなかったときは、それを除いた表で続け、
// 理由を rejected に残す(ADR-0031 の 3)。呼び手がログに出す。
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

    // 今の仮の世界のゲーム本体のパッケージ(公開用の試験の表。ゲームの中身の表は T-0002 でユーザーが決める)
    constexpr std::string_view DEFAULT_BASE_PACKAGE = "combustion_test";

    struct ReactionTableSource {
        fs::path packageRoot;  // 直下のフォルダが 1 つずつパッケージ(空なら DefaultPackageRoot)
        std::string basePackage{DEFAULT_BASE_PACKAGE};  // ゲーム本体。読めなければ失敗
        MergeMode mergeMode = MergeMode::AddOnly;       // 表の合わせ方(仮で案 C の既定。QUESTIONS Q8)
    };

    struct LoadedReactionTable {
        sim::BakedReactionTable table;       // GPU に上げる表と CPU だけの名前など(warnings は文献の反応熱との食い違い)
        fs::path packageRoot;                // 実際に読んだフォルダ
        std::vector<std::string> loadOrder;  // 読めたパッケージ(読んだ順)
        std::vector<PackageRejection> rejected;  // 読まなかった Mod と理由(ゲーム本体はここに来ない。来たら失敗)
        std::vector<TableOverride> overrides;    // 上書き(OverrideMarked・Override のとき)
        bool modifiedWorld = false;              // 「改造された世界」の印
        uint64_t tableVersion = 0;               // 合わせた表の版(TableVersion。同じ中身なら同じ値)
    };

    // 既定のパッケージのフォルダ: <exe のフォルダ>/data/packages
    [[nodiscard]] fs::path DefaultPackageRoot();

    // パッケージのフォルダから反応表を読んでベイクする。誤りには「どのフォルダ・どのパッケージ・どの欄か」を付ける
    [[nodiscard]] std::expected<LoadedReactionTable, std::string> LoadReactionTable(const ReactionTableSource& source);

}  // namespace bicameral::script
