// luau_type_check.h — パッケージの Luau を、読み込む(走らせる)前に型検査する(T-0140、13 §2・ADR-0046)。
//
// データの流れ: 型の定義(data/types/bicameral.d.luau)→ LuauTypeChecker::Create
//   → LoadPackages が各パッケージを読む前に CheckManifest(package.luau)・CheckPackage(全部のファイル + entry の戻り値)
//   → TypeDiagnostic(どのファイルの何行目・何列目・何が違うか)。誤りがあるパッケージは読まない(理由に診断が入る)。
// 検査は Luau.Analysis(vcpkg の luau port に入っている)の新しい型ソルバーで、全部のファイルを strict で見る
// (ファイルの先頭の --!nonstrict などはそのファイルだけに効く)。殻で見えないグローバル(os・debug など)は検査でも見えない。
// 決定性: 検査のたびにモジュールの状態を捨て、診断はファイル → 行 → 列 → 文の順に並べて重複を除く。同じ入力なら同じ診断の列になる。
// ここは CPU だけ(原則 2)。
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/aliases.h"

namespace bicameral::script {

    struct
        PackageSource;  // luau_package.h(luau_package.h がこのヘッダの TypeDiagnostic を使うので、ここでは include しない)

    // 型の定義に必ずある型の名前(package.luau の戻り値・entry の戻り値)
    constexpr std::string_view MANIFEST_TYPE_NAME = "PackageManifest";
    constexpr std::string_view ENTRY_TYPE_NAME = "PackageEntry";

    // 1 つの誤り。並べる順番 = ファイル → 行 → 列 → 種類 → 文
    struct TypeDiagnostic {
        std::string file;     // "パッケージ/パッケージの中の相対パス"(例 "combustion_test/species.luau")
        uint32_t line = 0;    // 1 始まり
        uint32_t column = 0;  // 1 始まり
        std::string kind;     // 何が違うか(日本語。「型が合わない」「知らない名前」など)
        std::string message;  // Luau の説明(英語のまま。型の名前と食い違いの場所が入る)

        auto operator<=>(const TypeDiagnostic&) const = default;
    };

    // "combustion_test/species.luau:12:9: 型が合わない: Type 'string' could not be converted into 'number'"
    [[nodiscard]] std::string FormatDiagnostic(const TypeDiagnostic& diagnostic);

    // 診断の列を 1 行に(最初の maxShown 個と、残りの数)。パッケージを読まない理由に使う
    [[nodiscard]] std::string SummarizeDiagnostics(const std::vector<TypeDiagnostic>& diagnostics, size_t maxShown = 3);

    class LuauTypeChecker {
    public:
        // definitions は Luau の定義ファイルのソース(MANIFEST_TYPE_NAME・ENTRY_TYPE_NAME を export type で持つ)。
        // 定義ファイル自体の誤り・型が無いときは error
        [[nodiscard]] static std::expected<std::unique_ptr<LuauTypeChecker>, std::string> Create(
            std::string_view definitions);

        ~LuauTypeChecker();
        LuauTypeChecker(const LuauTypeChecker&) = delete;
        LuauTypeChecker& operator=(const LuauTypeChecker&) = delete;

        // package.luau だけ(戻り値が PackageManifest か)。マニフェストを走らせる前に使う
        [[nodiscard]] std::vector<TypeDiagnostic> CheckManifest(const PackageSource& package);

        // パッケージの全部の *.luau と、entry("init" の形)の戻り値が PackageEntry か。entry を走らせる前に使う
        [[nodiscard]] std::vector<TypeDiagnostic> CheckPackage(const PackageSource& package, std::string_view entry);

    private:
        struct Impl;

        explicit LuauTypeChecker(std::unique_ptr<Impl> impl);

        std::unique_ptr<Impl> m_impl;
    };

    // 既定の型の定義のファイル: <exe のフォルダ>/data/types/bicameral.d.luau(ビルドがリポジトリの data/ を写す)
    [[nodiscard]] fs::path DefaultTypeDefinitionsPath();

    // 型の定義のファイルを読んで LuauTypeChecker を作る
    [[nodiscard]] std::expected<std::unique_ptr<LuauTypeChecker>, std::string> CreateTypeCheckerFromFile(
        const fs::path& definitionsFile);

}  // namespace bicameral::script
