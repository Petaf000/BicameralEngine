// standard_packages.cpp — 標準パッケージの棚と、環境ミキサーで足す一覧(standard_packages.h。T-0373・ADR-0183)。
#include "script/standard_packages.h"

#include <algorithm>
#include <format>
#include <set>

#include "core/paths.h"
#include "script/luau_package.h"
#include "script/luau_sandbox.h"

namespace bicameral::script {

    namespace {

        std::string PathText(const fs::path& path) {
            const std::u8string text = path.generic_u8string();

            return {text.begin(), text.end()};
        }

        // 棚の直下のフォルダ(名前の順)
        std::expected<std::vector<fs::path>, std::string> PackageFolders(const fs::path& root) {
            std::error_code error;
            if (!fs::is_directory(root, error))
                return std::unexpected(std::format("標準パッケージの棚 {} が無い", PathText(root)));

            std::vector<fs::path> folders;
            for (const fs::directory_entry& entry : fs::directory_iterator(root, error)) {
                if (entry.is_directory(error))
                    folders.push_back(entry.path());
            }

            if (error)
                return std::unexpected(
                    std::format("標準パッケージの棚 {} を読めない: {}", PathText(root), error.message()));

            std::ranges::sort(folders);

            return folders;
        }

        // name と依存を再帰的に集める(棚に無ければ誤り。循環は集めた名前で止まる)
        std::expected<void, std::string> CollectWithDependencies(const StandardCatalog& catalog, std::string_view name,
                                                                 std::set<std::string>& collected) {
            const StandardPackage* package = catalog.Find(name);
            if (package == nullptr)
                return std::unexpected(std::format("標準パッケージ {} が棚に無い", name));

            if (!collected.insert(package->name).second)
                return {};

            for (const std::string& depend : package->depends) {
                if (auto result = CollectWithDependencies(catalog, depend, collected); !result)
                    return std::unexpected(std::format("{}({} の依存)", result.error(), name));
            }

            return {};
        }

        bool DependsOnAny(const StandardCatalog& catalog, std::string_view name, const std::set<std::string>& targets) {
            const StandardPackage* package = catalog.Find(name);
            if (package == nullptr)
                return false;

            return std::ranges::any_of(package->depends,
                                       [&](const std::string& depend) { return targets.contains(depend); });
        }

    }  // namespace

    const StandardPackage* StandardCatalog::Find(std::string_view name) const {
        const auto found = std::ranges::find(packages, name, &StandardPackage::name);

        return found == packages.end() ? nullptr : &*found;
    }

    fs::path DefaultStandardPackageRoot() {
        return ExecutableDirectory() / "data" / "standard";
    }

    std::expected<StandardCatalog, std::string> ReadStandardCatalog(const fs::path& root) {
        const auto folders = PackageFolders(root);
        if (!folders)
            return std::unexpected(folders.error());

        auto sandbox = LuauSandbox::Create(SandboxLimits{});
        if (!sandbox)
            return std::unexpected(sandbox.error());

        (*sandbox)->Seal();

        // --- フォルダ → マニフェスト(表は読まない)---
        StandardCatalog catalog{.root = root};
        for (const fs::path& folder : *folders) {
            const auto source = ReadPackageFolder(folder);
            if (!source)
                return std::unexpected(std::format("標準パッケージ {}: {}", PathText(folder), source.error()));

            auto manifest = ReadPackageManifest(**sandbox, *source);
            if (!manifest)
                return std::unexpected(std::format("標準パッケージの棚 {}: {}", PathText(root), manifest.error()));

            catalog.packages.push_back(
                StandardPackage{.name = manifest->name,
                                .title = manifest->title.empty() ? manifest->name : std::move(manifest->title),
                                .depends = std::move(manifest->depends),
                                .folder = folder});
        }

        return catalog;
    }

    std::expected<std::vector<std::string>, std::string> MixAdd(const StandardCatalog& catalog,
                                                                std::span<const std::string> mix,
                                                                std::string_view name) {
        std::set<std::string> collected(mix.begin(), mix.end());
        if (auto result = CollectWithDependencies(catalog, name, collected); !result)
            return std::unexpected(result.error());

        return std::vector<std::string>(collected.begin(), collected.end());
    }

    std::vector<std::string> MixDependents(const StandardCatalog& catalog, std::span<const std::string> mix,
                                           std::string_view name) {
        // name に依存するものを、増えなくなるまで足していく(mix は小さいので素直に回す)
        std::set<std::string> removed{std::string(name)};
        bool grew = true;
        while (grew) {
            grew = false;
            for (const std::string& candidate : mix) {
                if (!removed.contains(candidate) && DependsOnAny(catalog, candidate, removed)) {
                    removed.insert(candidate);
                    grew = true;
                }
            }
        }

        removed.erase(std::string(name));

        return std::vector<std::string>(removed.begin(), removed.end());
    }

    std::vector<std::string> MixRemove(const StandardCatalog& catalog, std::span<const std::string> mix,
                                       std::string_view name) {
        const std::vector<std::string> dependents = MixDependents(catalog, mix, name);
        std::set<std::string> kept;
        for (const std::string& entry : mix) {
            if (entry != name && !std::ranges::contains(dependents, entry))
                kept.insert(entry);
        }

        return std::vector<std::string>(kept.begin(), kept.end());
    }

    std::expected<std::vector<fs::path>, std::string> MixFolders(const StandardCatalog& catalog,
                                                                 std::span<const std::string> mix) {
        std::vector<fs::path> folders;
        std::string missing;
        for (const std::string& name : std::set<std::string>(mix.begin(), mix.end())) {
            const StandardPackage* package = catalog.Find(name);
            if (package == nullptr)
                return std::unexpected(std::format("標準パッケージ {} が棚 {} に無い", name, PathText(catalog.root)));

            for (const std::string& depend : package->depends) {
                if (!std::ranges::contains(mix, depend))
                    missing += std::format("{}{} は {} が要る", missing.empty() ? "" : "・", name, depend);
            }

            folders.push_back(package->folder);
        }

        if (!missing.empty())
            return std::unexpected(std::format("足す標準パッケージの依存が足りない: {}", missing));

        return folders;
    }

    std::expected<std::vector<std::string>, std::string> ParseMixList(std::string_view text) {
        std::vector<std::string> names;
        size_t begin = 0;
        while (begin <= text.size()) {
            const size_t comma = text.find(',', begin);
            const std::string_view name = text.substr(begin, comma == std::string_view::npos ? comma : comma - begin);
            if (!IsValidPackageName(name))
                return std::unexpected(std::format("--mix はパッケージの名前をカンマで区切ったもの: '{}'", text));

            names.emplace_back(name);
            if (comma == std::string_view::npos)
                break;

            begin = comma + 1;
        }

        return names;
    }

}  // namespace bicameral::script
