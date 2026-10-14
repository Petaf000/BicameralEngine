// luau_package.h — Luau のパッケージ(1 フォルダ = 1 パッケージ)を読み、表を合わせる(T-0138、13 §2・D-320・ADR-0031)。
//
// パッケージ: フォルダの中の *.luau。ゲーム本体もパッケージの 1 つ。
//   package.luau(マニフェスト)が表を返す: { format = 1, title = "見せる名前", depends = { "core" }, entry = "init", game = true }
//   (title・depends・entry・game は省ける)。
//   ゲーム本体 = マニフェストに game = true と書いたパッケージ(読む一覧の中に 1 つまで。名前には依らない。T-0557・ADR-0250)。
//   options.basePackage で名前を渡すと、それをゲーム本体にする(試験の道。ほかに game = true と書いたものがあれば誤り)。
//   entry のファイルが「分類 → 鍵 → 値」の表を返す(例 { reactions = { burn = {...} }, elements = {...} })。
//   require("sub/util") はそのパッケージの中の sub/util.luau だけを読む(ファイルの外には届かない)。
//   require("@dep") は、たどって依存するパッケージ dep の entry が返す表(require("@dep/sub/util") はその中のファイル)。
//   その Run の中でもう一度走らせた写しなので、書き換えても dep の表は変わらない(読むだけ。T-0498)。
//   依存していないパッケージには届かない。標準パッケージの上に世界を作り、値を写して一部だけ変える形に使う(13 §2.2)。
// 読む順番: 依存の後。依存の関係で決まらないものはパッケージの名前(バイト順)。フォルダを並べた順・入れた順には依らない(決定性)。
// 表の合わせ方(MergeMode。QUESTIONS Q8 が未回答なので切り替えられる形。仮で C: 既定は AddOnly、上書きを許す設定は OverrideMarked):
//   新しい鍵は足す。既にある鍵を書こうとしたら AddOnly ではそのパッケージ(と依存するもの)を読まない、
//   OverrideMarked では上書きして「改造された世界」の印を付ける、Override では印なしで上書きする。値は丸ごと置き換える。
//   ただしゲーム本体(basePackage)が、自分のたどる依存(標準パッケージなど)の書いた鍵を書くのは、どの設定でも通し印を付けない
//   (世界の作り手が自分の世界の値として決めたもの。Mod の上書きではない。上書きの記録 overrides には残す。T-0498・ADR-0183)。
//   ゲーム本体が依存するパッケージ(たどった全部)は、ゲーム本体に依存させない(循環にしない)。
// データの流れ: フォルダ → ReadPackageRoot → PackageSource → LoadPackages(殻で走らせる)→ PackageSetResult → ベイク(T-0021)。
// ここは CPU だけ(原則 1・2)。同じパッケージ群・同じ設定なら、同じ PackageSetResult(CanonicalBytes が同じ)になる。
#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/aliases.h"
#include "script/luau_type_check.h"
#include "script/script_value.h"

namespace bicameral::script {

    class LuauSandbox;

    constexpr int64_t PACKAGE_FORMAT_VERSION = 1;  // マニフェストの format(D-320: データ形式に版を付ける)

    // 1 つのパッケージの中身(ディスクから読んでも、テストで手で作ってもよい)
    // NOLINTNEXTLINE(bugprone-exception-escape) MSVC の std::map は作るときに番兵を確保する(投げたら落ちてよい)
    struct PackageSource {
        std::string name;                          // フォルダの名前 = パッケージの名前
        std::map<std::string, std::string> files;  // パッケージの中の相対パス("/" 区切り。例 "sub/util.luau")→ ソース
    };

    enum class MergeMode : uint8_t {
        AddOnly,         // 案 B・案 C の既定: 足すだけ。既にある鍵を書こうとしたパッケージは読まない
        OverrideMarked,  // 案 C の「上書きを許す」設定: 上書きを通し、「改造された世界」の印を付ける
        Override,        // 案 A: 上書きを印なしで通す
    };

    struct PackageLoadOptions {
        MergeMode mergeMode = MergeMode::AddOnly;
        // ゲーム本体の名前。空なら、マニフェストに game = true と書いたもの(無ければゲーム本体なし = 全部 Mod)。
        // ゲーム本体があれば、ほかの全部がこれに依存する(最初に読む)
        std::string basePackage;

        // あれば、走らせる前に型検査する(T-0140・ADR-0046): マニフェストの前に package.luau、entry の前に全部のファイル。
        // 誤りがあるパッケージ(と依存するもの)は読まない。理由に「ファイル:行:列: 何が違うか」が入る
        LuauTypeChecker* typeChecker = nullptr;
    };

    // 読まなかったパッケージと理由
    struct PackageRejection {
        std::string package;
        std::string reason;
    };

    // 上書きの記録(「改造された世界」の中身。誰の何を誰が書き換えたか)
    struct TableOverride {
        std::string category;
        std::string key;
        std::string previousPackage;
        std::string package;
    };

    struct TableEntry {
        ScriptValue value;
        std::string package;  // 最後に書いたパッケージ
    };

    // NOLINTNEXTLINE(bugprone-exception-escape) MSVC の std::map は作るときに番兵を確保する(投げたら落ちてよい)
    struct PackageSetResult {
        std::vector<std::string> loadOrder;                               // 読めたパッケージ(読んだ順)
        std::vector<PackageRejection> rejected;                           // 読まなかったパッケージ(見つけた順)
        std::map<std::string, std::map<std::string, TableEntry>> tables;  // 分類 → 鍵 → 値(バイト順)
        std::vector<TableOverride> overrides;                             // 上書き(起きた順)
        bool modifiedWorld = false;  // 「改造された世界」の印(OverrideMarked で上書きがあった)
        std::string gamePackage;     // ゲーム本体の名前(無ければ空。読めずに rejected に入っても名前は入る。T-0557)
        std::vector<TypeDiagnostic>
            typeDiagnostics;  // 型検査の誤り(見つけた順。1 つのパッケージの中はファイル → 行の順)
    };

    // --- ディスクから読む(エディタ・ツール用)---

    // folder の中の *.luau を全部(下のフォルダも)読む。名前はフォルダの名前
    [[nodiscard]] std::expected<PackageSource, std::string> ReadPackageFolder(const fs::path& folder);

    // root の直下のフォルダを 1 つずつパッケージとして読む(名前の順)
    [[nodiscard]] std::expected<std::vector<PackageSource>, std::string> ReadPackageRoot(const fs::path& root);

    // --- 読み込みと合わせ ---

    // sandbox は Seal 済みのもの(ホストの関数は呼び手が登録しておく)。名前の重複・Seal 前は error、パッケージごとの誤りは rejected へ
    [[nodiscard]] std::expected<PackageSetResult, std::string> LoadPackages(LuauSandbox& sandbox,
                                                                            std::span<const PackageSource> packages,
                                                                            const PackageLoadOptions& options);

    // マニフェスト(package.luau)だけを読んだもの(環境ミキサーの一覧と依存の確かめ。T-0373)。表は読まない
    struct PackageManifestInfo {
        std::string name;
        std::string title;                 // 人に見せる名前(書いていなければ空)
        std::vector<std::string> depends;  // 依存するパッケージ(書いた順)
        bool game = false;                 // マニフェストに game = true(ゲーム本体。T-0557)
    };

    // sandbox は Seal 済みのもの。マニフェストが無い・形が違う・名前の規則に合わなければ理由
    [[nodiscard]] std::expected<PackageManifestInfo, std::string> ReadPackageManifest(LuauSandbox& sandbox,
                                                                                      const PackageSource& package);

    // 合わせた表の中身(分類・鍵・値)のバイト列。どのパッケージが書いたかは入れない(同じ中身なら同じ版)
    [[nodiscard]] std::string TableBytes(const PackageSetResult& result);

    // 表の版(TableBytes のハッシュ)。再生ファイルや発見の共有(D-110・15 §3)で、同じ法則の世界かを見分けるのに使う
    [[nodiscard]] uint64_t TableVersion(const PackageSetResult& result);

    // TableBytes の逆: 合わせた表の中身のバイト列から、表(tables だけ)を作り直す(再生ファイルの表。T-0193・ADR-0050)。
    // 書いたパッケージは分からないので、どの値も package = REPLAY_TABLE_PACKAGE。壊れていれば理由
    inline constexpr std::string_view REPLAY_TABLE_PACKAGE = "(再生ファイルの表)";
    [[nodiscard]] std::expected<PackageSetResult, std::string> ParseTableBytes(std::string_view bytes);

    // require("@dep") / require("@dep/sub/util") の名前 → 依存するパッケージの名前と、その中のファイル("sub/util.luau"。
    // "@dep" だけなら空 = entry)。@ で始まらない・名前の規則に合わなければ理由
    struct DependencyRequire {
        std::string package;
        std::string path;
    };
    [[nodiscard]] std::expected<DependencyRequire, std::string> SplitDependencyRequire(std::string_view name);

    // 名前の規則(パッケージ・分類・require のパスの区切りごと): 英小文字・数字・'_'・'-' だけで 1〜64 文字
    [[nodiscard]] bool IsValidPackageName(std::string_view name);

    // require の名前 → パッケージの中のファイル("sub/util" → "sub/util.luau")。名前の規則に合わなければ理由。型検査(T-0140)も使う
    [[nodiscard]] std::expected<std::string, std::string> PackageModulePath(std::string_view name);

}  // namespace bicameral::script
