// luau_type_check.cpp — パッケージの Luau を読み込む前に型検査する(luau_type_check.h。T-0140・ADR-0046)。
// Luau.Analysis の Frontend を 1 つ持ち、型の定義(定義ファイル)をグローバルに足しておく。
// 検査のたびにモジュールの状態を捨て、そのパッケージのファイルだけを見せる(ほかのパッケージには require でも届かない)。
// 戻り値の形は、型の定義の型と欄ごとに突き合わせ(ShapeWalker)、葉だけを合成したモジュールで Luau に確かめさせる。
// 誤りは欄を書いた行に付け替える。
// Luau.Analysis を知るのはこのファイルだけ(ヘッダが大きいので pch.h にも入れない)。
#include "script/luau_type_check.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include "core/paths.h"
#include "script/luau_package.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)  // Luau のヘッダの警告(使わない引数など)は自分のコードではないので止める
#endif
#include "Luau/Ast.h"
#include "Luau/BuiltinDefinitions.h"
#include "Luau/Config.h"
#include "Luau/ConfigResolver.h"
#include "Luau/Error.h"
#include "Luau/FileResolver.h"
#include "Luau/Frontend.h"
#include "Luau/ToString.h"
#include "Luau/Type.h"
#include "Luau/TypeArena.h"
#include "Luau/TypePack.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace bicameral::script {

    namespace {

        constexpr std::string_view MANIFEST_FILE = "package.luau";
        constexpr std::string_view MODULE_EXTENSION = ".luau";
        constexpr std::string_view
            RETURN_CHECK_MODULE = "@check/return";  // パッケージの名前・パスに @ は無いので重ならない
        constexpr std::string_view DEFINITIONS_PACKAGE = "@bicameral";

        // 殻(ADR-0030)で見えないグローバル。検査でも見えないようにして、走らせる前に「知らない名前」で落とす
        constexpr const char* HIDDEN_GLOBALS[] = {"os",      "debug",    "gcinfo",     "getfenv",
                                                  "setfenv", "newproxy", "loadstring", "collectgarbage"};

        std::string ToUtf8String(const fs::path& path) {
            const std::u8string text = path.generic_u8string();

            return {text.begin(), text.end()};
        }

        // --- 型検査に見せるファイル(1 つのパッケージ + 合成したモジュール)---

        class PackageFileResolver final : public Luau::FileResolver {
        public:
            void Reset(const PackageSource& package) {
                m_package = package.name;
                m_sources.clear();
                for (const auto& [path, source] : package.files)
                    m_sources.emplace(package.name + "/" + path, source);
            }

            void AddModule(std::string name, std::string source) {
                m_sources.insert_or_assign(std::move(name), std::move(source));
            }

            std::optional<Luau::SourceCode> readSource(const Luau::ModuleName& name) override {
                const auto found = m_sources.find(name);
                if (found == m_sources.end())
                    return std::nullopt;

                return Luau::SourceCode{found->second, Luau::SourceCode::Module};
            }

            // require("sub/util") → "パッケージ/sub/util.luau"(殻の require と同じ規則。luau_package の PackageModulePath)。
            // 規則に合わない・無いファイルも名前を返す(readSource が無いと答え、Luau が「require の先が無い」を出す)
            std::optional<Luau::ModuleInfo> resolveModule(const Luau::ModuleInfo* context, Luau::AstExpr* expr,
                                                          const Luau::TypeCheckLimits& /*limits*/) override {
                const auto* constant = expr ? expr->as<Luau::AstExprConstantString>() : nullptr;
                if (context == nullptr || constant == nullptr)
                    return std::nullopt;

                const std::string_view name(constant->value.data, constant->value.size);
                const auto path = PackageModulePath(name);

                return Luau::ModuleInfo{m_package + "/" + (path ? *path : std::string(name))};
            }

        private:
            std::string m_package;
            std::map<std::string, std::string> m_sources;  // "パッケージ/相対パス" → ソース
        };

        // 全部のファイルを strict で見る(ファイルの先頭の --!nonstrict などはそのファイルだけに効く)
        class StrictConfigResolver final : public Luau::ConfigResolver {
        public:
            StrictConfigResolver() { m_config.mode = Luau::Mode::Strict; }

            const Luau::Config& getConfig(const Luau::ModuleName& /*name*/,
                                          const Luau::TypeCheckLimits& /*limits*/) const override {
                return m_config;
            }

        private:
            Luau::Config m_config;
        };

        // --- 誤り → 診断 ---

        std::string KindText(const Luau::TypeError& error) {
            if (Luau::get_if<Luau::SyntaxError>(&error.data))
                return "文法の誤り";
            if (Luau::get_if<Luau::UnknownSymbol>(&error.data))
                return "知らない名前";
            if (Luau::get_if<Luau::UnknownProperty>(&error.data))
                return "知らない欄";
            if (Luau::get_if<Luau::MissingProperties>(&error.data))
                return "欄が足りない";
            if (Luau::get_if<Luau::TypeMismatch>(&error.data))
                return "型が合わない";
            if (Luau::get_if<Luau::UnknownRequire>(&error.data))
                return "require の先が無い";
            if (Luau::get_if<Luau::CountMismatch>(&error.data))
                return "数が合わない";

            return "型の誤り";
        }

        // 戻り値の検査: どのファイルが返す表を、型の定義のどの型で確かめるか
        struct ReturnCheck {
            std::string targetModule;  // "パッケージ/init.luau"
            std::string typeName;
        };

        // そのモジュールの最後の return の場所(無ければ 1 行目)
        Luau::Location ReturnLocation(Luau::Frontend& frontend, const std::string& moduleName) {
            Luau::Location location;
            const Luau::SourceModule* source = frontend.getSourceModule(moduleName);
            if (source == nullptr || source->root == nullptr)
                return location;

            for (Luau::AstStat* statement : source->root->body) {
                if (statement->is<Luau::AstStatReturn>())
                    location = statement->location;
            }

            return location;
        }

        // Luau の説明は改行・タブを含むことがある。ログの 1 行に収まるよう空白 1 つにまとめる
        std::string OneLine(std::string_view text) {
            std::string line;
            bool space = false;
            for (const char character : text) {
                const bool isSpace = character == '\n' || character == '\r' || character == '\t' || character == ' ';
                if (isSpace) {
                    space = !line.empty();
                    continue;
                }

                if (space)
                    line += ' ';
                line += character;
                space = false;
            }

            return line;
        }

        TypeDiagnostic MakeDiagnostic(std::string file, const Luau::Location& location, std::string kind,
                                      std::string_view message) {
            return TypeDiagnostic{.file = std::move(file),
                                  .line = location.begin.line + 1,
                                  .column = location.begin.column + 1,
                                  .kind = std::move(kind),
                                  .message = OneLine(message)};
        }

        // --- 型の道具 ---

        std::string TypeText(Luau::TypeId type) {
            Luau::ToStringOptions options;
            options.exhaustive = true;

            return Luau::toString(type, options);
        }

        bool IsNil(Luau::TypeId type) {
            const auto* primitive = Luau::get<Luau::PrimitiveType>(Luau::follow(type));

            return primitive != nullptr && primitive->type == Luau::PrimitiveType::NilType;
        }

        bool IsOptional(Luau::TypeId type) {
            const auto* union_ = Luau::get<Luau::UnionType>(Luau::follow(type));

            return union_ != nullptr && std::ranges::any_of(union_->options, IsNil);
        }

        // X? → X(nil を除いた選択肢が 1 つのときだけ。値があるときに使う)
        Luau::TypeId StripOptional(Luau::TypeId type) {
            type = Luau::follow(type);
            const auto* union_ = Luau::get<Luau::UnionType>(type);
            if (union_ == nullptr)
                return type;

            std::vector<Luau::TypeId> options;
            for (Luau::TypeId option : union_->options) {
                if (!IsNil(option))
                    options.push_back(option);
            }

            return options.size() == 1 ? Luau::follow(options[0]) : type;
        }

        std::string QuoteKey(std::string_view key) {
            std::string text = "[\"";
            for (const char character : key) {
                if (character == '"' || character == '\\')
                    text += '\\';
                text += character;
            }

            return text + "\"]";
        }

        // --- 戻り値の形を欄ごとに突き合わせる ---
        // Luau の新しい型ソルバーは「省ける欄を書かなかった表」を、省ける欄つきの型(entry: string? など)へ渡せない
        // (表の欄の型を不変として扱うため。T-0140 の実験)。そこで表は欄ごとに降り、欄が無い・知らない欄はここで診断にし、
        // 葉(数・文字列など)だけを合成したモジュールの 1 行「local _n: <型> = value["..."]」で Luau に確かめさせる。
        // 誤りは葉の欄を書いた行に付け替える。
        class ShapeWalker {
        public:
            struct Leaf {
                std::string expression;  // value["species"]["water"]["formation_enthalpy_j_per_mol"]
                std::string typeText;    // number
                std::string path;        // species.water.formation_enthalpy_j_per_mol
                std::string module;      // 欄を書いたファイル
                Luau::Location location;
            };

            struct Place {
                std::string expression;
                std::string path;
                std::string module;
                Luau::Location location;
            };

            void Walk(Luau::TypeId expected, Luau::TypeId actual, const Place& place, int depth) {
                expected = StripOptional(expected);
                actual = Luau::follow(actual);
                if (Luau::get<Luau::AnyType>(expected))
                    return;

                const auto* expectedTable = Luau::get<Luau::TableType>(expected);
                const auto* actualTable = Luau::get<Luau::TableType>(actual);
                if (expectedTable == nullptr || actualTable == nullptr || actualTable->indexer || depth > MAX_DEPTH) {
                    leaves.push_back(Leaf{.expression = place.expression,
                                          .typeText = TypeText(expected),
                                          .path = place.path,
                                          .module = place.module,
                                          .location = place.location});
                    return;
                }

                // 表の欄を書いたファイル(require した先の表なら、そのファイル)
                const std::string tableModule = actualTable->definitionModuleName.empty()
                                                    ? place.module
                                                    : actualTable->definitionModuleName;
                WalkExpectedFields(*expectedTable, *actualTable, place, tableModule, depth);
                WalkExtraFields(*expectedTable, *actualTable, place, tableModule, depth);
            }

            std::vector<Leaf> leaves;
            std::vector<TypeDiagnostic> diagnostics;

        private:
            static constexpr int MAX_DEPTH = 32;

            static std::string ChildPath(const Place& place, const std::string& name) {
                return place.path.empty() ? name : place.path + "." + name;
            }

            void WalkProperty(Luau::TypeId expected, const std::string& name, const Luau::Property& property,
                              const Place& parent, const std::string& tableModule, int depth) {
                if (!property.readTy)
                    return;

                const Place child{.expression = parent.expression + QuoteKey(name),
                                  .path = ChildPath(parent, name),
                                  .module = tableModule,
                                  .location = property.location.value_or(parent.location)};
                Walk(expected, *property.readTy, child, depth + 1);
            }

            // 型の定義にある欄: 書いてあれば降りる。書いていない欄が省けない欄なら「欄が足りない」
            void WalkExpectedFields(const Luau::TableType& expected, const Luau::TableType& actual, const Place& place,
                                    const std::string& tableModule, int depth) {
                for (const auto& [name, property] : expected.props) {
                    if (!property.readTy)
                        continue;

                    const auto found = actual.props.find(name);
                    if (found != actual.props.end()) {
                        WalkProperty(*property.readTy, name, found->second, place, tableModule, depth);
                        continue;
                    }

                    if (!IsOptional(*property.readTy))
                        diagnostics.push_back(MakeDiagnostic(
                            place.module, place.location, "欄が足りない",
                            std::format("{} に {} が無い(型 {})", place.path.empty() ? "返す表" : place.path, name,
                                        TypeText(*property.readTy))));
                }
            }

            // 型の定義に無い欄: 鍵の型(例 { [string]: Species })があればそれで降りる。無ければ「知らない欄」(綴りの誤りなど)
            void WalkExtraFields(const Luau::TableType& expected, const Luau::TableType& actual, const Place& place,
                                 const std::string& tableModule, int depth) {
                for (const auto& [name, property] : actual.props) {
                    if (expected.props.contains(name))
                        continue;

                    if (expected.indexer) {
                        WalkProperty(expected.indexer->indexResultType, name, property, place, tableModule, depth);
                        continue;
                    }

                    diagnostics.push_back(
                        MakeDiagnostic(tableModule, property.location.value_or(place.location), "知らない欄",
                                       std::format("{} は型の定義に無い欄(綴りの誤り?)", ChildPath(place, name))));
                }
            }
        };

        // 合成したモジュールのソース: 1 行目 --!strict、2 行目 require、3 行目から葉が 1 行ずつ
        constexpr uint32_t FIRST_LEAF_LINE = 2;  // 0 始まり

        std::string ShapeCheckSource(const std::string& requireName, const std::vector<ShapeWalker::Leaf>& leaves) {
            std::string source = std::format("--!strict\nlocal value = require(\"{}\")\n", requireName);
            for (size_t index = 0; index < leaves.size(); ++index)
                source += std::format("local _{}: {} = value{}\n", index, leaves[index].typeText,
                                      leaves[index].expression);

            return source + "return nil\n";
        }

        void HideGlobals(Luau::GlobalTypes& globals) {
            for (const char* name : HIDDEN_GLOBALS) {
                const Luau::AstName astName = globals.globalNames.names->get(name);
                if (astName.value != nullptr)
                    globals.globalScope->bindings.erase(Luau::Symbol(astName));
            }
        }

        std::string DefinitionErrors(const Luau::LoadDefinitionFileResult& loaded) {
            std::string text;
            for (const Luau::ParseError& error : loaded.parseResult.errors)
                text += std::format(" {}:{}: {}", error.getLocation().begin.line + 1,
                                    error.getLocation().begin.column + 1, error.getMessage());
            if (loaded.module) {
                for (const Luau::TypeError& error : loaded.module->errors)
                    text += std::format(" {}:{}: {}", error.location.begin.line + 1, error.location.begin.column + 1,
                                        Luau::toString(error));
            }

            return text;
        }

    }  // namespace

    struct LuauTypeChecker::Impl {
        PackageFileResolver fileResolver;
        StrictConfigResolver configResolver;
        Luau::Frontend frontend{Luau::SolverMode::New, &fileResolver, &configResolver};

        // 1 つのパッケージを検査する。files はパッケージの中の相対パス、returnCheck があれば戻り値の形も確かめる
        std::vector<TypeDiagnostic> Check(const PackageSource& package, const std::vector<std::string>& files,
                                          const std::optional<ReturnCheck>& returnCheck);

        // returnCheck のファイルが返す表を、型の定義の型と欄ごとに突き合わせる(ShapeWalker)
        void CheckReturnShape(const PackageSource& package, const ReturnCheck& returnCheck,
                              std::set<TypeDiagnostic>& found);
    };

    std::vector<TypeDiagnostic> LuauTypeChecker::Impl::Check(const PackageSource& package,
                                                             const std::vector<std::string>& files,
                                                             const std::optional<ReturnCheck>& returnCheck) {
        // --- 前の検査の状態を捨てる(順番・回数に依らない診断にする)---
        frontend.clear();
        fileResolver.Reset(package);

        // --- ファイルごとの検査(ファイル → 行 → 列 → 種類 → 文の順。重複は 1 つ)---
        std::set<TypeDiagnostic> found;
        bool targetHasSyntaxError = false;
        for (const std::string& file : files) {
            const Luau::CheckResult result = frontend.check(package.name + "/" + file);
            for (const Luau::TypeError& error : result.errors) {
                // 「引数に型を付けるとよい」は誤りではなく提案なので出さない
                if (Luau::get_if<Luau::ExplicitFunctionAnnotationRecommended>(&error.data))
                    continue;

                if (returnCheck && error.moduleName == returnCheck->targetModule &&
                    Luau::get_if<Luau::SyntaxError>(&error.data))
                    targetHasSyntaxError = true;

                found.insert(MakeDiagnostic(error.moduleName, error.location, KindText(error),
                                            Luau::toString(error, Luau::TypeErrorToStringOptions{&fileResolver})));
            }
        }

        // 文法の誤りで読めないファイルの戻り値の形は見ない(誤りが重なって読みにくくなる)
        if (returnCheck && !targetHasSyntaxError)
            CheckReturnShape(package, *returnCheck, found);

        return {found.begin(), found.end()};
    }

    void LuauTypeChecker::Impl::CheckReturnShape(const PackageSource& package, const ReturnCheck& returnCheck,
                                                 std::set<TypeDiagnostic>& found) {
        const Luau::ModulePtr module = frontend.moduleResolver.getModule(returnCheck.targetModule);
        const auto expectedType = frontend.globals.globalScope->lookupType(returnCheck.typeName);
        if (!module || !expectedType)
            return;

        const Luau::Location returnLocation = ReturnLocation(frontend, returnCheck.targetModule);
        const std::optional<Luau::TypeId> actual = Luau::first(module->returnType);
        if (!actual) {
            found.insert(MakeDiagnostic(returnCheck.targetModule, returnLocation, "返す表が無い",
                                        std::format("{} の形の表を 1 つ返す", returnCheck.typeName)));
            return;
        }

        // --- 欄ごとに降りる → 葉を合成したモジュールで確かめる ---
        ShapeWalker walker;
        walker.Walk(expectedType->type, *actual,
                    ShapeWalker::Place{
                        .expression = "", .path = "", .module = returnCheck.targetModule, .location = returnLocation},
                    0);
        found.insert(walker.diagnostics.begin(), walker.diagnostics.end());
        if (walker.leaves.empty())
            return;

        std::string requireName = returnCheck.targetModule.substr(package.name.size() + 1);
        requireName.resize(requireName.size() - MODULE_EXTENSION.size());
        const std::string shapeModule = package.name + "/" + std::string(RETURN_CHECK_MODULE);
        fileResolver.AddModule(shapeModule, ShapeCheckSource(requireName, walker.leaves));

        // --- 合成したモジュールの誤り → 葉の欄を書いた行 ---
        const Luau::CheckResult result = frontend.check(shapeModule);
        for (const Luau::TypeError& error : result.errors) {
            if (error.moduleName != shapeModule)
                continue;

            const std::string message = Luau::toString(error, Luau::TypeErrorToStringOptions{&fileResolver});
            const uint32_t line = error.location.begin.line;
            if (line >= FIRST_LEAF_LINE && line - FIRST_LEAF_LINE < walker.leaves.size()) {
                const ShapeWalker::Leaf& leaf = walker.leaves[line - FIRST_LEAF_LINE];
                found.insert(MakeDiagnostic(leaf.module, leaf.location, KindText(error),
                                            std::format("{}: {}", leaf.path, message)));
                continue;
            }

            found.insert(MakeDiagnostic(returnCheck.targetModule, returnLocation,
                                        std::format("返す表が {} の形でない", returnCheck.typeName), message));
        }
    }

    LuauTypeChecker::LuauTypeChecker(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

    LuauTypeChecker::~LuauTypeChecker() = default;

    std::expected<std::unique_ptr<LuauTypeChecker>, std::string> LuauTypeChecker::Create(std::string_view definitions) {
        auto impl = std::make_unique<Impl>();
        Luau::Frontend& frontend = impl->frontend;

        // --- 標準の型 → 殻で見えないものを外す → 型の定義を足す ---
        Luau::registerBuiltinGlobals(frontend, frontend.globals);
        HideGlobals(frontend.globals);

        const Luau::LoadDefinitionFileResult loaded = frontend.loadDefinitionFile(
            frontend.globals, frontend.globals.globalScope, definitions, std::string(DEFINITIONS_PACKAGE), false,
            false);
        Luau::freeze(frontend.globals.globalTypes);
        if (!loaded.success)
            return std::unexpected(std::format("型の定義に誤りがある:{}", DefinitionErrors(loaded)));

        for (const std::string_view name : {MANIFEST_TYPE_NAME, ENTRY_TYPE_NAME}) {
            if (!frontend.globals.globalScope->lookupType(std::string(name)))
                return std::unexpected(std::format("型の定義に export type {} が無い", name));
        }

        return std::unique_ptr<LuauTypeChecker>(new LuauTypeChecker(std::move(impl)));
    }

    std::vector<TypeDiagnostic> LuauTypeChecker::CheckManifest(const PackageSource& package) {
        if (!package.files.contains(std::string(MANIFEST_FILE)))
            return {};  // 無いことは読み込みが理由を出す

        const std::string manifest = package.name + "/" + std::string(MANIFEST_FILE);

        return m_impl->Check(package, {std::string(MANIFEST_FILE)},
                             ReturnCheck{.targetModule = manifest, .typeName = std::string(MANIFEST_TYPE_NAME)});
    }

    std::vector<TypeDiagnostic> LuauTypeChecker::CheckPackage(const PackageSource& package, std::string_view entry) {
        std::vector<std::string> files;
        for (const auto& [path, source] : package.files)
            files.push_back(path);

        // entry のファイルが無い・名前の規則に合わないときは、戻り値の検査をしない(読み込みが理由を出す)
        std::optional<ReturnCheck> returnCheck;
        const auto entryPath = PackageModulePath(entry);
        if (entryPath && package.files.contains(*entryPath))
            returnCheck = ReturnCheck{.targetModule = package.name + "/" + *entryPath,
                                      .typeName = std::string(ENTRY_TYPE_NAME)};

        return m_impl->Check(package, files, returnCheck);
    }

    // --- 表示 ---

    std::string FormatDiagnostic(const TypeDiagnostic& diagnostic) {
        return std::format("{}:{}:{}: {}: {}", diagnostic.file, diagnostic.line, diagnostic.column, diagnostic.kind,
                           diagnostic.message);
    }

    std::string SummarizeDiagnostics(const std::vector<TypeDiagnostic>& diagnostics, size_t maxShown) {
        std::string text;
        for (size_t index = 0; index < diagnostics.size() && index < maxShown; ++index) {
            if (index > 0)
                text += " / ";
            text += FormatDiagnostic(diagnostics[index]);
        }

        if (diagnostics.size() > maxShown)
            text += std::format("(ほか {} 件)", diagnostics.size() - maxShown);

        return text;
    }

    // --- ファイルから ---

    fs::path DefaultTypeDefinitionsPath() {
        return ExecutableDirectory() / "data" / "types" / "bicameral.d.luau";
    }

    std::expected<std::unique_ptr<LuauTypeChecker>, std::string> CreateTypeCheckerFromFile(
        const fs::path& definitionsFile) {
        std::ifstream file(definitionsFile, std::ios::binary);
        if (!file)
            return std::unexpected(std::format("型の定義 {} を開けない", ToUtf8String(definitionsFile)));

        const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        auto checker = LuauTypeChecker::Create(source);
        if (!checker)
            return std::unexpected(std::format("{}: {}", ToUtf8String(definitionsFile), checker.error()));

        return checker;
    }

}  // namespace bicameral::script
