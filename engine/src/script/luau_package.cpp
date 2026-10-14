// luau_package.cpp — パッケージの読み込み・順番・require の解決・表の合わせ方(T-0138、13 §2・ADR-0031)。
//
// 流れ: マニフェストを全部読む(名前の順)→ ゲーム本体への依存を足す(ゲーム本体のたどる依存は除く)
//   → 依存で順番を決める(決まらない所は名前の順)→ 順番に entry を走らせる(require は自分のファイルと、たどる依存の写し)
//   → 戻り値の「分類 → 鍵 → 値」を確かめる → MergeMode で合わせる。
// パッケージの誤り(マニフェスト・依存・実行・上書きの禁止)はそのパッケージと、それに依存するものだけを読まない(ほかは続ける)。
// Luau は全部 1 つの殻で走らせる(Run ごとにグローバルは別。13 §2.1)。
#include "script/luau_package.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <iterator>
#include <set>
#include <utility>

#include "script/luau_sandbox.h"

namespace bicameral::script {

    namespace {

        constexpr std::string_view MANIFEST_FILE = "package.luau";
        constexpr std::string_view MODULE_EXTENSION = ".luau";
        constexpr size_t MAX_NAME_LENGTH = 64;
        constexpr size_t MAX_LISTED_CONFLICTS = 3;

        constexpr size_t MAX_TITLE_BYTES = 256;

        struct Manifest {
            std::vector<std::string> depends;  // 書いた依存(LoadPackages が basePackage も足す)
            std::string entry = "init";
            std::string title;  // 人に見せる名前(環境ミキサーの一覧。省けば空。T-0373)
        };

        // 分類 → 鍵 → 値(1 つのパッケージが返したもの)
        using Contribution = std::map<std::string, std::map<std::string, ScriptValue>>;

        std::string ToUtf8String(const fs::path& path) {
            const std::u8string text = path.generic_u8string();

            return {text.begin(), text.end()};
        }

        // --- require の名前 → パッケージの中のファイル ---

        // "sub/util" → "sub/util.luau"。区切りごとに名前の規則(".." や絶対パス・拡張子つきは通らない)
        std::expected<std::string, std::string> ModulePath(std::string_view name) {
            size_t begin = 0;
            while (true) {
                const size_t slash = name.find('/', begin);
                const std::string_view segment = name.substr(begin,
                                                             slash == std::string_view::npos ? slash : slash - begin);
                if (!IsValidPackageName(segment))
                    return std::unexpected(
                        "名前は 'sub/util' の形(英小文字・数字・_・- を / で区切る。.. や拡張子は書かない)");
                if (slash == std::string_view::npos)
                    break;

                begin = slash + 1;
            }

            return std::string(name) + std::string(MODULE_EXTENSION);
        }

        // require で見えるパッケージ: 走らせるパッケージ自身と、そのたどる依存(読み込み済み。T-0498)
        struct VisiblePackage {
            const PackageSource* source = nullptr;
            std::string entry;
        };
        using VisiblePackages = std::map<std::string, VisiblePackage>;

        // require("@dep") / require("@dep/sub/util") → 依存のパッケージの名前とファイル
        std::expected<std::pair<std::string, std::string>, std::string> ResolveDependencyRequire(
            const VisiblePackages& visible, std::string_view ownPackage, std::string_view name) {
            const auto split = SplitDependencyRequire(name);
            if (!split)
                return std::unexpected(split.error());

            const auto found = visible.find(split->package);
            if (found == visible.end() || split->package == ownPackage)
                return std::unexpected(std::format("'{}' はパッケージ '{}' がたどって依存するパッケージではない",
                                                   split->package, ownPackage));
            if (!split->path.empty())
                return std::pair{split->package, split->path};

            auto entryPath = ModulePath(found->second.entry);
            if (!entryPath)
                return std::unexpected(entryPath.error());

            return std::pair{split->package, std::move(*entryPath)};
        }

        // require の名前 → ファイル。名前の無い require("sub/util") は、require を書いたファイルのパッケージの中で探す
        // (依存のパッケージのモジュールの中の require は、そのパッケージの中)
        ModuleResolver MakeResolver(const VisiblePackages& visible) {
            return [&visible](std::string_view name,
                              std::string_view fromChunk) -> std::expected<ModuleFile, std::string> {
                const std::string fromPackage(fromChunk.substr(0, fromChunk.find('/')));
                std::pair<std::string, std::string> target;
                if (name.starts_with('@')) {
                    auto dependency = ResolveDependencyRequire(visible, fromPackage, name);
                    if (!dependency)
                        return std::unexpected(dependency.error());

                    target = std::move(*dependency);
                } else {
                    auto path = ModulePath(name);
                    if (!path)
                        return std::unexpected(path.error());

                    target = {fromPackage, std::move(*path)};
                }

                const std::string missing = std::format("パッケージ '{}' に {} が無い", target.first, target.second);
                const auto package = visible.find(target.first);
                if (package == visible.end())
                    return std::unexpected(missing);

                const auto& files = package->second.source->files;
                const auto found = files.find(target.second);
                if (found == files.end())
                    return std::unexpected(missing);

                return ModuleFile{.chunkName = target.first + "/" + target.second, .source = found->second};
            };
        }

        // --- マニフェスト ---

        std::expected<std::vector<std::string>, std::string> ReadNameList(const ScriptValue& list) {
            if (!list.IsTable())
                return std::unexpected("depends は名前の配列");

            std::vector<std::string> names;
            for (const ScriptField& field : list.fields) {
                const bool inSequence = field.key.IsNumber() &&
                                        field.key.number == static_cast<double>(names.size() + 1);
                if (!inSequence || !field.value.IsString() || !IsValidPackageName(field.value.text))
                    return std::unexpected("depends は名前の配列({ \"core\", \"other\" } の形)");

                names.push_back(field.value.text);
            }

            return names;
        }

        std::expected<Manifest, std::string> ParseManifest(const ScriptValue& table) {
            if (!table.IsTable())
                return std::unexpected("package.luau は表を 1 つ返す");

            Manifest manifest;
            bool hasFormat = false;
            for (const ScriptField& field : table.fields) {
                const std::string_view key = field.key.IsString() ? std::string_view(field.key.text) : "";
                if (key == "format") {
                    if (!field.value.IsNumber() || field.value.number != static_cast<double>(PACKAGE_FORMAT_VERSION))
                        return std::unexpected(std::format("format は {}(この版が読める形)", PACKAGE_FORMAT_VERSION));
                    hasFormat = true;
                } else if (key == "depends") {
                    auto names = ReadNameList(field.value);
                    if (!names)
                        return std::unexpected(names.error());
                    manifest.depends = std::move(*names);
                } else if (key == "entry") {
                    if (!field.value.IsString() || !ModulePath(field.value.text))
                        return std::unexpected("entry はパッケージの中のファイルの名前('init' の形)");
                    manifest.entry = field.value.text;
                } else if (key == "title") {
                    if (!field.value.IsString() || field.value.text.size() > MAX_TITLE_BYTES)
                        return std::unexpected(std::format("title は {} バイトまでの文字列", MAX_TITLE_BYTES));
                    manifest.title = field.value.text;
                } else {
                    return std::unexpected(
                        std::format("知らない欄 {}(書けるのは format・title・depends・entry)", ToDebugText(field.key)));
                }
            }

            if (!hasFormat)
                return std::unexpected(std::format("format = {} が無い", PACKAGE_FORMAT_VERSION));

            return manifest;
        }

        std::expected<Manifest, std::string> ReadManifest(LuauSandbox& sandbox, const PackageSource& package) {
            const auto file = package.files.find(std::string(MANIFEST_FILE));
            if (file == package.files.end())
                return std::unexpected(std::format("{} が無い", MANIFEST_FILE));

            const auto run = sandbox.Run(package.name + "/" + std::string(MANIFEST_FILE), file->second,
                                         RunOptions{.modules = nullptr, .captureValues = true});
            if (!run)
                return std::unexpected(run.error().message);
            if (run->values.size() != 1)
                return std::unexpected("package.luau は表を 1 つ返す");

            return ParseManifest(run->values[0]);
        }

        // --- 順番(依存の後。決まらない所は名前の順)---

        enum class OrderState : uint8_t {
            Pending,
            Ordered,
            Rejected,
        };

        struct OrderNode {
            Manifest manifest;
            OrderState state = OrderState::Pending;
        };

        // name のたどる依存(depends をたどった全部。name は含めない。無い依存は飛ばす。循環しても止まる)
        std::set<std::string> DependencyClosure(const std::map<std::string, OrderNode>& nodes,
                                                const std::string& name) {
            std::set<std::string> closure;
            std::vector<std::string> pending{name};
            while (!pending.empty()) {
                const auto node = nodes.find(pending.back());
                pending.pop_back();
                if (node == nodes.end())
                    continue;

                for (const std::string& depend : node->second.manifest.depends) {
                    if (depend != name && closure.insert(depend).second)
                        pending.push_back(depend);
                }
            }

            return closure;
        }

        // ほかの全部をゲーム本体に依存させる(Mod はゲーム本体の後に読む)。ゲーム本体のたどる依存は除く(循環にしない。T-0498)
        void AddBaseDependency(std::map<std::string, OrderNode>& nodes, const std::string& basePackage,
                               const std::set<std::string>& baseClosure) {
            for (auto& [name, node] : nodes) {
                std::vector<std::string>& depends = node.manifest.depends;
                if (name == basePackage || baseClosure.contains(name) || std::ranges::contains(depends, basePackage))
                    continue;

                depends.push_back(basePackage);
            }
        }

        // 依存のうち、まだ並べられていないもの(無い・読まないものなら理由も)
        bool DependsReady(const std::map<std::string, OrderNode>& nodes, const OrderNode& node,
                          std::string* missingReason) {
            for (const std::string& depend : node.manifest.depends) {
                const auto found = nodes.find(depend);
                if (found == nodes.end()) {
                    if (missingReason)
                        *missingReason = std::format("依存するパッケージ '{}' が無い", depend);
                    return false;
                }
                if (found->second.state == OrderState::Rejected) {
                    if (missingReason)
                        *missingReason = std::format("依存するパッケージ '{}' を読まなかった", depend);
                    return false;
                }
                if (found->second.state != OrderState::Ordered)
                    return false;
            }

            return true;
        }

        // 1 つ進める: 並べられる最小の名前を並べる → 依存が欠けたものを読まない → 残りは循環。進めなければ false
        bool OrderStep(std::map<std::string, OrderNode>& nodes, std::vector<std::string>& order,
                       PackageSetResult& result) {
            for (auto& [name, node] : nodes) {
                if (node.state == OrderState::Pending && DependsReady(nodes, node, nullptr)) {
                    node.state = OrderState::Ordered;
                    order.push_back(name);

                    return true;
                }
            }

            for (auto& [name, node] : nodes) {
                std::string reason;
                if (node.state == OrderState::Pending && !DependsReady(nodes, node, &reason) && !reason.empty()) {
                    node.state = OrderState::Rejected;
                    result.rejected.push_back(PackageRejection{.package = name, .reason = std::move(reason)});

                    return true;
                }
            }

            bool anyPending = false;
            for (auto& [name, node] : nodes) {
                if (node.state != OrderState::Pending)
                    continue;

                node.state = OrderState::Rejected;
                result.rejected.push_back(PackageRejection{.package = name, .reason = "依存が循環している"});
                anyPending = true;
            }

            return anyPending;
        }

        // --- entry の戻り値 → 分類 → 鍵 → 値 ---

        std::expected<Contribution, std::string> ReadContribution(const std::vector<ScriptValue>& values) {
            Contribution contribution;
            if (values.empty() || values[0].kind == ScriptValue::Kind::Nil)
                return contribution;  // 何も足さないパッケージ(ほかのパッケージの道具だけなど)
            if (values.size() != 1 || !values[0].IsTable())
                return std::unexpected("entry は「分類 → 鍵 → 値」の表を 1 つ返す");

            for (const ScriptField& category : values[0].fields) {
                if (!category.key.IsString() || !IsValidPackageName(category.key.text) || !category.value.IsTable())
                    return std::unexpected(
                        std::format("分類 {} は名前(英小文字・数字・_・-)で、値は表", ToDebugText(category.key)));

                auto& entries = contribution[category.key.text];
                for (const ScriptField& entry : category.value.fields) {
                    if (!entry.key.IsString() || entry.key.text.empty())
                        return std::unexpected(
                            std::format("{} の鍵 {} は空でない文字列", category.key.text, ToDebugText(entry.key)));

                    entries.emplace(entry.key.text, entry.value);
                }
            }

            return contribution;
        }

        std::expected<Contribution, std::string> RunEntry(LuauSandbox& sandbox, const PackageSource& package,
                                                          const Manifest& manifest, const VisiblePackages& visible) {
            const std::string path = manifest.entry + std::string(MODULE_EXTENSION);
            const auto file = package.files.find(path);
            if (file == package.files.end())
                return std::unexpected(std::format("entry の {} が無い", path));

            const ModuleResolver resolver = MakeResolver(visible);
            const auto run = sandbox.Run(package.name + "/" + path, file->second,
                                         RunOptions{.modules = &resolver, .captureValues = true});
            if (!run)
                return std::unexpected(run.error().message);

            return ReadContribution(run->values);
        }

        // --- 合わせる ---

        // 1 つのパッケージを読むときの周り(T-0498)
        struct LoadContext {
            VisiblePackages visible;                                 // require で見えるもの(自分 + たどる依存)
            std::vector<PackageDependency> dependencies;             // 型検査に見せるたどる依存
            const std::set<std::string>* ownDependencies = nullptr;  // ゲーム本体のとき: 印なしで上書きしてよい書き手
        };

        bool OverridesOwnDependency(const LoadContext& context, const std::string& previousPackage) {
            return context.ownDependencies != nullptr && context.ownDependencies->contains(previousPackage);
        }

        // AddOnly で上書きがあれば理由を返す(合わせない)。ゲーム本体が自分のたどる依存の鍵を書くのは上書きの禁止に当たらない
        std::string FindForbiddenOverrides(const PackageSetResult& result, const Contribution& contribution,
                                           const LoadContext& context) {
            std::vector<std::string> conflicts;
            for (const auto& [category, entries] : contribution) {
                const auto existing = result.tables.find(category);
                if (existing == result.tables.end())
                    continue;

                for (const auto& [key, value] : entries) {
                    const auto previous = existing->second.find(key);
                    if (previous != existing->second.end() &&
                        !OverridesOwnDependency(context, previous->second.package))
                        conflicts.push_back(
                            std::format("{}.{}('{}' が書いた)", category, key, previous->second.package));
                }
            }

            if (conflicts.empty())
                return {};

            std::string reason = "既にある鍵を上書きしようとした(追加だけの設定): ";
            for (size_t index = 0; index < conflicts.size() && index < MAX_LISTED_CONFLICTS; ++index)
                reason += (index > 0 ? "、" : "") + conflicts[index];
            if (conflicts.size() > MAX_LISTED_CONFLICTS)
                reason += std::format(" ほか {} 件", conflicts.size() - MAX_LISTED_CONFLICTS);

            return reason;
        }

        // 1 つの鍵を合わせる(既にあれば上書きを記録する。AddOnly の禁止は FindForbiddenOverrides が先に見ている)
        void MergeEntry(PackageSetResult& result, const std::string& category, const std::string& key,
                        ScriptValue&& value, const std::string& package, MergeMode mode, const LoadContext& context) {
            auto [slot, added] = result.tables[category].try_emplace(key);
            if (!added) {
                result.overrides.push_back(TableOverride{
                    .category = category, .key = key, .previousPackage = slot->second.package, .package = package});
                if (mode == MergeMode::OverrideMarked && !OverridesOwnDependency(context, slot->second.package))
                    result.modifiedWorld = true;
            }

            slot->second = TableEntry{.value = std::move(value), .package = package};
        }

        void Merge(PackageSetResult& result, const std::string& package, Contribution&& contribution, MergeMode mode,
                   const LoadContext& context) {
            for (auto& [category, entries] : contribution) {
                for (auto& [key, value] : entries)
                    MergeEntry(result, category, key, std::move(value), package, mode, context);
            }
        }

        // 1 つのパッケージを読んで合わせる。読まなかったら理由
        // 型検査の誤りを結果に足し、パッケージを読まない理由を返す(誤りが無ければ空)
        std::string TypeCheckReason(std::vector<TypeDiagnostic>&& diagnostics, PackageSetResult& result) {
            if (diagnostics.empty())
                return {};

            std::string reason = "型検査: " + SummarizeDiagnostics(diagnostics);
            result.typeDiagnostics.insert(result.typeDiagnostics.end(), std::make_move_iterator(diagnostics.begin()),
                                          std::make_move_iterator(diagnostics.end()));

            return reason;
        }

        std::string LoadOne(LuauSandbox& sandbox, const PackageSource& package, const Manifest& manifest,
                            const std::set<std::string>& loaded, const PackageLoadOptions& options,
                            const LoadContext& context, PackageSetResult& result) {
            for (const std::string& depend : manifest.depends) {
                if (!loaded.contains(depend))
                    return std::format("依存するパッケージ '{}' を読まなかった", depend);
            }

            // --- 走らせる前に型検査(T-0140)---
            if (options.typeChecker != nullptr) {
                std::string reason = TypeCheckReason(
                    options.typeChecker->CheckPackage(package, manifest.entry, context.dependencies), result);
                if (!reason.empty())
                    return reason;
            }

            auto contribution = RunEntry(sandbox, package, manifest, context.visible);
            if (!contribution)
                return contribution.error();

            if (options.mergeMode == MergeMode::AddOnly) {
                std::string forbidden = FindForbiddenOverrides(result, *contribution, context);
                if (!forbidden.empty())
                    return forbidden;
            }

            Merge(result, package.name, std::move(*contribution), options.mergeMode, context);

            return {};
        }

        // name を読むときの周り: require で見えるもの・型検査に見せる依存・ゲーム本体なら印なしで上書きしてよい書き手
        LoadContext MakeLoadContext(const std::map<std::string, const PackageSource*>& byName,
                                    const std::map<std::string, OrderNode>& nodes, const std::string& name,
                                    const std::string& basePackage, const std::set<std::string>& baseClosure) {
            LoadContext context;
            context.visible.emplace(name,
                                    VisiblePackage{.source = byName.at(name), .entry = nodes.at(name).manifest.entry});
            for (const std::string& depend : DependencyClosure(nodes, name)) {
                const auto node = nodes.find(depend);
                if (node == nodes.end())
                    continue;

                const PackageSource* source = byName.at(depend);
                context.visible.emplace(depend, VisiblePackage{.source = source, .entry = node->second.manifest.entry});
                context.dependencies.push_back(
                    PackageDependency{.package = source, .entry = node->second.manifest.entry});
            }

            if (!basePackage.empty() && name == basePackage)
                context.ownDependencies = &baseClosure;

            return context;
        }

    }  // namespace

    // --- 名前 ---

    bool IsValidPackageName(std::string_view name) {
        if (name.empty() || name.size() > MAX_NAME_LENGTH)
            return false;

        return std::ranges::all_of(name, [](char character) {
            return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
                   character == '_' || character == '-';
        });
    }

    // --- ディスクから読む ---

    std::expected<std::string, std::string> PackageModulePath(std::string_view name) {
        return ModulePath(name);
    }

    std::expected<DependencyRequire, std::string> SplitDependencyRequire(std::string_view name) {
        constexpr std::string_view FORM = "依存するパッケージは '@名前' か '@名前/sub/util' の形";
        if (!name.starts_with('@'))
            return std::unexpected(std::string(FORM));

        name.remove_prefix(1);
        const size_t slash = name.find('/');
        DependencyRequire require{.package = std::string(name.substr(0, slash)), .path = {}};
        if (!IsValidPackageName(require.package))
            return std::unexpected(std::string(FORM));
        if (slash == std::string_view::npos)
            return require;

        auto path = ModulePath(name.substr(slash + 1));
        if (!path)
            return std::unexpected(std::format("{}: {}", FORM, path.error()));

        require.path = std::move(*path);

        return require;
    }

    std::expected<PackageSource, std::string> ReadPackageFolder(const fs::path& folder) {
        std::error_code error;
        if (!fs::is_directory(folder, error))
            return std::unexpected(std::format("{} はフォルダではない", ToUtf8String(folder)));

        PackageSource package;
        package.name = ToUtf8String(folder.filename());

        fs::recursive_directory_iterator iterator(folder, error);
        for (; !error && iterator != fs::recursive_directory_iterator(); iterator.increment(error)) {
            const fs::path& path = iterator->path();
            if (!iterator->is_regular_file(error) || path.extension() != MODULE_EXTENSION)
                continue;

            std::ifstream file(path, std::ios::binary);
            if (!file)
                return std::unexpected(std::format("{} を開けない", ToUtf8String(path)));

            std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            package.files.emplace(ToUtf8String(path.lexically_relative(folder)), std::move(source));
        }

        if (error)
            return std::unexpected(std::format("{} を読めない: {}", ToUtf8String(folder), error.message()));

        return package;
    }

    std::expected<std::vector<PackageSource>, std::string> ReadPackageRoot(const fs::path& root) {
        std::error_code error;
        std::vector<fs::path> folders;
        for (fs::directory_iterator iterator(root, error); !error && iterator != fs::directory_iterator();
             iterator.increment(error)) {
            if (iterator->is_directory(error))
                folders.push_back(iterator->path());
        }

        if (error)
            return std::unexpected(std::format("{} を読めない: {}", ToUtf8String(root), error.message()));

        std::ranges::sort(folders);

        std::vector<PackageSource> packages;
        for (const fs::path& folder : folders) {
            auto package = ReadPackageFolder(folder);
            if (!package)
                return std::unexpected(package.error());

            packages.push_back(std::move(*package));
        }

        return packages;
    }

    // --- マニフェストだけ読む(環境ミキサーの一覧。T-0373)---

    std::expected<PackageManifestInfo, std::string> ReadPackageManifest(LuauSandbox& sandbox,
                                                                        const PackageSource& package) {
        if (!sandbox.IsSealed())
            return std::unexpected("殻を Seal してから読む");
        if (!IsValidPackageName(package.name))
            return std::unexpected(std::format("パッケージ '{}': 名前は英小文字・数字・_・- だけ", package.name));

        auto manifest = ReadManifest(sandbox, package);
        if (!manifest)
            return std::unexpected(std::format("パッケージ '{}': {}", package.name, manifest.error()));

        return PackageManifestInfo{
            .name = package.name, .title = std::move(manifest->title), .depends = std::move(manifest->depends)};
    }

    // --- 読み込みと合わせ ---

    std::expected<PackageSetResult, std::string> LoadPackages(LuauSandbox& sandbox,
                                                              std::span<const PackageSource> packages,
                                                              const PackageLoadOptions& options) {
        if (!sandbox.IsSealed())
            return std::unexpected("殻を Seal してから読む");

        // --- 名前で引けるように(入れた順に依らない)---
        std::map<std::string, const PackageSource*> byName;
        for (const PackageSource& package : packages) {
            if (!byName.emplace(package.name, &package).second)
                return std::unexpected(std::format("パッケージの名前 '{}' が 2 つある", package.name));
        }

        PackageSetResult result;

        // --- マニフェスト(名前の順)---
        std::map<std::string, OrderNode> nodes;
        for (const auto& [name, package] : byName) {
            if (options.typeChecker != nullptr && IsValidPackageName(name)) {
                std::string reason = TypeCheckReason(options.typeChecker->CheckManifest(*package), result);
                if (!reason.empty()) {
                    result.rejected.push_back(PackageRejection{.package = name, .reason = std::move(reason)});
                    continue;
                }
            }

            auto manifest = IsValidPackageName(name)
                                ? ReadManifest(sandbox, *package)
                                : std::expected<Manifest, std::string>(std::unexpect,
                                                                       "パッケージの名前は英小文字・数字・_・- だけ");
            if (!manifest) {
                result.rejected.push_back(PackageRejection{.package = name, .reason = manifest.error()});
                continue;
            }

            nodes.emplace(name, OrderNode{.manifest = std::move(*manifest)});
        }

        // --- ゲーム本体への依存(ゲーム本体のたどる依存は除く)---
        std::set<std::string> baseClosure;
        if (!options.basePackage.empty()) {
            baseClosure = DependencyClosure(nodes, options.basePackage);
            AddBaseDependency(nodes, options.basePackage, baseClosure);
        }

        // --- 順番 → 読み込み ---
        std::vector<std::string> order;
        while (OrderStep(nodes, order, result)) {
        }

        std::set<std::string> loaded;
        for (const std::string& name : order) {
            const LoadContext context = MakeLoadContext(byName, nodes, name, options.basePackage, baseClosure);
            std::string reason = LoadOne(sandbox, *byName.at(name), nodes.at(name).manifest, loaded, options, context,
                                         result);
            if (!reason.empty()) {
                result.rejected.push_back(PackageRejection{.package = name, .reason = std::move(reason)});
                continue;
            }

            loaded.insert(name);
            result.loadOrder.push_back(name);
        }

        return result;
    }

    std::string TableBytes(const PackageSetResult& result) {
        std::string bytes;
        AppendCanonicalString("bicameral-tables", bytes);
        AppendCanonicalString(std::to_string(PACKAGE_FORMAT_VERSION), bytes);

        for (const auto& [category, entries] : result.tables) {
            AppendCanonicalString(category, bytes);
            AppendCanonicalString(std::to_string(entries.size()), bytes);
            for (const auto& [key, entry] : entries) {
                AppendCanonicalString(key, bytes);
                AppendCanonicalBytes(entry.value, bytes);
            }
        }

        return bytes;
    }

    uint64_t TableVersion(const PackageSetResult& result) {
        return HashBytes(TableBytes(result));
    }

    // TableBytes と同じ並び: 見出し("bicameral-tables"・形式の版)→ (分類・鍵の数・(鍵・値)× 数)を終わりまで
    std::expected<PackageSetResult, std::string> ParseTableBytes(std::string_view bytes) {
        const auto magic = ReadCanonicalString(bytes);
        const auto formatVersion = magic ? ReadCanonicalString(bytes) : magic;
        if (!magic || *magic != "bicameral-tables" || !formatVersion ||
            *formatVersion != std::to_string(PACKAGE_FORMAT_VERSION))
            return std::unexpected("表の中身の見出しが違う(合わせた表のバイト列でないか、形式の版が違う)");

        PackageSetResult result;
        while (!bytes.empty()) {
            const auto category = ReadCanonicalString(bytes);
            const auto countText = category ? ReadCanonicalString(bytes) : category;
            uint64_t count = 0;
            const bool counted = countText &&
                                 std::from_chars(countText->data(), countText->data() + countText->size(), count).ec ==
                                     std::errc{};
            if (!counted || result.tables.contains(*category))
                return std::unexpected("表の中身の分類が壊れている");

            auto& entries = result.tables[*category];
            for (uint64_t index = 0; index < count; ++index) {
                auto key = ReadCanonicalString(bytes);
                auto value = key ? ReadCanonicalBytes(bytes) : std::expected<ScriptValue, std::string>{};
                if (!key || !value)
                    return std::unexpected(
                        std::format("表の中身の {} が壊れている: {}", *category, key ? value.error() : key.error()));

                entries[std::move(*key)] = {.value = std::move(*value), .package = std::string(REPLAY_TABLE_PACKAGE)};
            }
        }

        return result;
    }

}  // namespace bicameral::script
