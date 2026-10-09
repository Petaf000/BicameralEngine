// reaction_table_loader.cpp — データのフォルダのパッケージ → 反応表のベイク(reaction_table_loader.h。T-0157・ADR-0033)。
// 失敗の出し方: どの段で落ちたか(フォルダ・パッケージの読み込み・形・ベイクの検査)を頭に付けて 1 行で返す。
#include "script/reaction_table_loader.h"

#include <algorithm>
#include <format>
#include <utility>

#include "core/paths.h"
#include "script/luau_sandbox.h"
#include "script/luau_type_check.h"
#include "script/reaction_package.h"

namespace bicameral::script {

    namespace {

        std::string PathText(const fs::path& path) {
            const std::u8string text = path.generic_u8string();

            return {text.begin(), text.end()};
        }

        // ゲーム本体が読めたか。読めなかったら理由(rejected にあればそれ、無ければ「見つからない」)
        std::expected<void, std::string> CheckBasePackage(const PackageSetResult& loaded,
                                                          std::string_view basePackage) {
            if (std::ranges::contains(loaded.loadOrder, basePackage))
                return {};

            const auto rejection = std::ranges::find(loaded.rejected, basePackage, &PackageRejection::package);
            if (rejection != loaded.rejected.end())
                return std::unexpected(
                    std::format("ゲーム本体のパッケージ {} を読めない: {}", basePackage, rejection->reason));

            return std::unexpected(std::format("ゲーム本体のパッケージ {} が見つからない", basePackage));
        }

        // パッケージ群を型検査してから殻で読んで合わせる(ホストの関数は無い。反応表は値だけ)
        std::expected<PackageSetResult, std::string> LoadPackageSet(std::span<const PackageSource> packages,
                                                                    const ReactionTableSource& source) {
            const fs::path definitions = source.typeDefinitions.empty() ? DefaultTypeDefinitionsPath()
                                                                        : source.typeDefinitions;
            auto checker = CreateTypeCheckerFromFile(definitions);
            if (!checker)
                return std::unexpected(checker.error());

            auto sandbox = LuauSandbox::Create(SandboxLimits{});
            if (!sandbox)
                return std::unexpected(sandbox.error());

            (*sandbox)->Seal();

            return LoadPackages(
                **sandbox, packages,
                PackageLoadOptions{
                    .mergeMode = source.mergeMode, .basePackage = source.basePackage, .typeChecker = checker->get()});
        }

    }  // namespace

    fs::path DefaultPackageRoot() {
        return ExecutableDirectory() / "data" / "packages";
    }

    std::expected<LoadedReactionTable, std::string> LoadReactionTable(const ReactionTableSource& source) {
        const fs::path root = source.packageRoot.empty() ? DefaultPackageRoot() : source.packageRoot;
        const std::string where = std::format("反応表のパッケージ({})", PathText(root));

        // --- フォルダ → パッケージ → 合わせた表 ---
        const auto packages = ReadPackageRoot(root);
        if (!packages)
            return std::unexpected(std::format("{}: {}", where, packages.error()));

        const auto loaded = LoadPackageSet(*packages, source);
        if (!loaded)
            return std::unexpected(std::format("{}: {}", where, loaded.error()));

        if (const auto base = CheckBasePackage(*loaded, source.basePackage); !base)
            return std::unexpected(std::format("{}: {}", where, base.error()));

        // --- 定義 → ベイク(元素の釣り合い・知らない物質・係数・速度の範囲の検査)---
        const auto definition = ReadReactionTableDefinition(*loaded);
        if (!definition)
            return std::unexpected(std::format("{}: {}", where, definition.error()));

        auto baked = sim::BakeReactionTable(*definition);
        if (!baked)
            return std::unexpected(std::format("{}: ベイクの検査で落ちた: {}", where, baked.error()));

        return LoadedReactionTable{.table = std::move(*baked),
                                   .packageRoot = root,
                                   .loadOrder = loaded->loadOrder,
                                   .rejected = loaded->rejected,
                                   .overrides = loaded->overrides,
                                   .modifiedWorld = loaded->modifiedWorld,
                                   .tableVersion = TableVersion(*loaded),
                                   .tableBytes = TableBytes(*loaded)};
    }

    std::expected<LoadedReactionTable, std::string> RebuildReactionTable(std::string_view tableBytes,
                                                                         uint64_t expectedVersion) {
        const std::string where = std::format("再生ファイルの反応表(版 {:016x})", expectedVersion);
        if (HashBytes(tableBytes) != expectedVersion)
            return std::unexpected(std::format("{}: 中身のハッシュが版と合わない(壊れている)", where));

        // --- 中身 → 合わせた表(正準な並びか、版で確かめ直す)→ 定義 → ベイク ---
        auto parsed = ParseTableBytes(tableBytes);
        if (!parsed)
            return std::unexpected(std::format("{}: {}", where, parsed.error()));

        if (TableVersion(*parsed) != expectedVersion)
            return std::unexpected(std::format("{}: 読んだ表の版が合わない(正準な並びでない)", where));

        const auto definition = ReadReactionTableDefinition(*parsed);
        if (!definition)
            return std::unexpected(std::format("{}: {}", where, definition.error()));

        auto baked = sim::BakeReactionTable(*definition);
        if (!baked)
            return std::unexpected(std::format("{}: ベイクの検査で落ちた: {}", where, baked.error()));

        return LoadedReactionTable{
            .table = std::move(*baked), .tableVersion = expectedVersion, .tableBytes = std::string(tableBytes)};
    }

}  // namespace bicameral::script
