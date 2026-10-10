// reaction_table_edit.cpp — 反応表のパネルの中身(reaction_table_edit.h。T-0219・D-449)。
// 一覧の値は表の中身(TableBytes → 定義)から取り、書き戻す所だけをパッケージのフォルダのソースから探す
// (表の中身が正しい値で、ソースは書き換える場所を知るためだけに読む)。
#include "script/reaction_table_edit.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <ranges>
#include <utility>

#include "script/luau_literal_edit.h"
#include "script/reaction_package.h"

namespace bicameral::script {

    namespace {

        // 読み手(reaction_package.cpp)の範囲と同じ
        constexpr int64_t EXACT_INTEGER_LIMIT = int64_t{1} << 53;
        constexpr int64_t UINT32_LIMIT = std::numeric_limits<uint32_t>::max();

        std::string ReadText(const fs::path& path) {
            std::ifstream file(path, std::ios::binary);

            return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        }

        std::string PathText(const fs::path& path) {
            const std::u8string text = path.generic_u8string();

            return {text.begin(), text.end()};
        }

        // --- 書き戻す所を探す ---

        // パッケージのフォルダの中身(読んだ順の後ろから。後のパッケージが前の値を上書きするので)
        class SourceIndex {
        public:
            SourceIndex(fs::path root, std::vector<PackageSource> packages, const std::vector<std::string>& loadOrder)
                : m_root(std::move(root)) {
                for (const std::string& name : loadOrder | std::views::reverse) {
                    const auto package = std::ranges::find(packages, name, &PackageSource::name);
                    if (package != packages.end())
                        m_packages.push_back(std::move(*package));
                }
            }

            // value.keyPath(分類から)か、分類を除いた並びで探す。最初に見つかったパッケージで決める
            void Locate(ReactionValue& value) const {
                const std::span<const std::string> full = value.keyPath;
                const std::span<const std::string> withoutCategory = full.subspan(1);
                for (const PackageSource& package : m_packages) {
                    for (const auto path : {full, withoutCategory}) {
                        if (LocateIn(package, path, value))
                            return;
                    }
                }

                value.readOnly = "パッケージのソースに見つからない";
            }

        private:
            // package の中で path を探す。見つかった(書き戻せる・書き戻せない理由が決まった)なら true
            bool LocateIn(const PackageSource& package, std::span<const std::string> path, ReactionValue& value) const {
                size_t count = 0;
                for (const auto& [relative, source] : package.files) {
                    const auto found = FindLuauLiterals(source, path);
                    if (!found || found->Count() == 0)
                        continue;

                    count += found->Count();
                    if (!found->expressions.empty()) {
                        value.readOnly = std::format("{}/{} の {} 行目は式なので書き戻せない", package.name, relative,
                                                     found->expressions.front().line);
                    } else if (count == 1) {
                        value.file = m_root / package.name / fs::path(std::u8string(relative.begin(), relative.end()));
                        value.sourceKeyPath.assign(path.begin(), path.end());
                    }
                }

                if (count == 0)
                    return false;

                if (count > 1 || !value.readOnly.empty()) {
                    if (value.readOnly.empty())
                        value.readOnly = std::format("{} の中の {} か所にある", package.name, count);

                    value.file.clear();
                    value.sourceKeyPath.clear();
                }

                return true;
            }

            fs::path m_root;
            std::vector<PackageSource> m_packages;
        };

        // --- 行を作る ---

        ReactionValue MakeInteger(std::vector<std::string> keyPath, std::string label, int64_t current, int64_t minimum,
                                  int64_t maximum) {
            return ReactionValue{.label = std::move(label),
                                 .keyPath = std::move(keyPath),
                                 .kind = ReactionValueKind::Integer,
                                 .minimum = minimum,
                                 .maximum = maximum,
                                 .text = std::format("{}", current)};
        }

        // 仮数 × 10^指数 → "2.8e19"(仮数の 1 桁目の後に小数点。パッケージの書き方に合わせる)
        std::string FormatDecimal(uint64_t mantissa, int32_t exponent10) {
            const std::string digits = std::format("{}", mantissa);
            const auto exponent = exponent10 + static_cast<int32_t>(digits.size()) - 1;
            if (digits.size() == 1)
                return std::format("{}e{}", digits, exponent);

            return std::format("{}.{}e{}", digits.front(), std::string_view(digits).substr(1), exponent);
        }

        // 同じ値か(10 進の数は仮数と指数で、整数は数で比べる。字面の違い "2.8e19" と "28e18" は同じ)
        bool SameValue(const ReactionValue& value, std::string_view existingLiteral, std::string_view text) {
            const auto existing = LuauLiteralValue(existingLiteral);
            if (!existing)
                return false;

            if (value.kind == ReactionValueKind::Integer)
                return *existing == text;

            const auto before = ParseDecimal(*existing);
            const auto after = ParseDecimal(text);

            return before && after && before->mantissa == after->mantissa && before->exponent10 == after->exponent10;
        }

        std::string FormatTerms(const std::vector<sim::RuleTerm>& terms) {
            std::string text;
            for (const sim::RuleTerm& term : terms) {
                if (!text.empty())
                    text += " + ";

                if (term.coefficient != 1)
                    text += std::format("{} ", term.coefficient);

                text += term.species;
            }

            return text;
        }

        using SpeciesByName = std::map<std::string, const sim::SpeciesDefinition*, std::less<>>;

        // 元素ごとの数(反応物 − 生成物)と、生成エンタルピーの差(生成物 − 反応物)
        void CheckConservation(const sim::RuleDefinition& rule, const SpeciesByName& species, RuleRow& row) {
            std::map<std::string, std::pair<int64_t, int64_t>> elements;  // 元素 → (反応物, 生成物)
            int64_t enthalpy = 0;
            for (const auto& [terms, sign] : {std::pair{&rule.reactants, -1}, {&rule.products, 1}}) {
                for (const sim::RuleTerm& term : *terms) {
                    const auto found = species.find(term.species);
                    if (found == species.end()) {
                        row.elementImbalance.push_back(std::format("知らない物質 {}", term.species));
                        continue;
                    }

                    const auto coefficient = static_cast<int64_t>(term.coefficient);
                    enthalpy += sign * coefficient * found->second->formationEnthalpy;
                    for (const sim::ElementCount& count : found->second->composition) {
                        auto& [reactantCount, productCount] = elements[count.element];
                        (sign < 0 ? reactantCount : productCount) += coefficient * count.count;
                    }
                }
            }

            row.enthalpyJoulesPerMol = enthalpy;
            for (const auto& [element, counts] : elements) {
                if (counts.first != counts.second)
                    row.elementImbalance.push_back(std::format("{}: {} → {}", element, counts.first, counts.second));
            }
        }

        RuleRow MakeRuleRow(const sim::RuleDefinition& rule, const SpeciesByName& species) {
            const std::string category(REACTION_RULES_CATEGORY);
            const sim::ArrheniusRate& rate = rule.rate;
            RuleRow row{.name = rule.name,
                        .equation = std::format("{} → {}", FormatTerms(rule.reactants), FormatTerms(rule.products)),
                        .preExponentialMantissa = rate.preExponentialMantissa,
                        .preExponentialExponent10 = rate.preExponentialExponent10,
                        .activationEnergyJoulesPerMol = rate.activationEnergy};

            row.preExponential = ReactionValue{
                .label = "rate.a",
                .keyPath = {category, rule.name, "rate", "a"},
                .kind = ReactionValueKind::Decimal,
                .text = FormatDecimal(rate.preExponentialMantissa, rate.preExponentialExponent10)};
            row.activationEnergy = MakeInteger({category, rule.name, "rate", "activation_energy_j_per_mol"},
                                               "rate.activation_energy_j_per_mol", rate.activationEnergy, 0,
                                               EXACT_INTEGER_LIMIT);
            if (rule.declaredReactionEnthalpy) {
                row.declaredEnthalpy = MakeInteger({category, rule.name, "reaction_enthalpy_j_per_mol"},
                                                   "reaction_enthalpy_j_per_mol", *rule.declaredReactionEnthalpy,
                                                   -EXACT_INTEGER_LIMIT, EXACT_INTEGER_LIMIT);
            }

            for (const auto& [field, terms] : {std::pair{"reactants", &rule.reactants}, {"products", &rule.products}}) {
                for (const sim::RuleTerm& term : *terms) {
                    row.coefficients.push_back(MakeInteger({category, rule.name, field, term.species},
                                                           std::format("{}.{}", field, term.species), term.coefficient,
                                                           1, UINT32_LIMIT));
                }
            }

            CheckConservation(rule, species, row);

            return row;
        }

        SpeciesRow MakeSpeciesRow(const sim::SpeciesDefinition& species) {
            const std::string category(REACTION_SPECIES_CATEGORY);
            std::string composition;
            for (const sim::ElementCount& count : species.composition) {
                if (!composition.empty())
                    composition += ' ';

                composition += std::format("{}{}", count.element, count.count);
            }

            return SpeciesRow{
                .name = species.name,
                .composition = std::move(composition),
                .formationEnthalpy = MakeInteger({category, species.name, "formation_enthalpy_j_per_mol"},
                                                 "formation_enthalpy_j_per_mol", species.formationEnthalpy,
                                                 -EXACT_INTEGER_LIMIT, EXACT_INTEGER_LIMIT),
                .heatCapacity = MakeInteger({category, species.name, "heat_capacity_mj_per_mol_k"},
                                            "heat_capacity_mj_per_mol_k", species.heatCapacity, 1, UINT32_LIMIT),
                .thermalConductivity = MakeInteger({category, species.name, "thermal_conductivity_mw_per_m_k"},
                                                   "thermal_conductivity_mw_per_m_k", species.thermalConductivity, 0,
                                                   UINT32_LIMIT)};
        }

        // 書き戻す所を全部の値で探す
        void LocateAll(const SourceIndex& index, ReactionTableDocument& document) {
            for (ElementRow& row : document.elements)
                index.Locate(row.atomicMass);

            for (SpeciesRow& row : document.species) {
                for (ReactionValue* value : {&row.formationEnthalpy, &row.heatCapacity, &row.thermalConductivity})
                    index.Locate(*value);
            }

            for (RuleRow& row : document.rules) {
                index.Locate(row.preExponential);
                index.Locate(row.activationEnergy);
                if (row.declaredEnthalpy)
                    index.Locate(*row.declaredEnthalpy);

                for (ReactionValue& coefficient : row.coefficients)
                    index.Locate(coefficient);
            }
        }

        void MarkAllReadOnly(ReactionTableDocument& document, const std::string& reason) {
            const auto mark = [&](ReactionValue& value) {
                value.readOnly = reason;
            };
            for (ElementRow& row : document.elements)
                mark(row.atomicMass);

            for (SpeciesRow& row : document.species) {
                for (ReactionValue* value : {&row.formationEnthalpy, &row.heatCapacity, &row.thermalConductivity})
                    mark(*value);
            }

            for (RuleRow& row : document.rules) {
                mark(row.preExponential);
                mark(row.activationEnergy);
                if (row.declaredEnthalpy)
                    mark(*row.declaredEnthalpy);

                std::ranges::for_each(row.coefficients, mark);
            }
        }

    }  // namespace

    std::expected<ReactionTableDocument, std::string> BuildReactionTableDocument(const LoadedReactionTable& table) {
        // --- 表の中身 → 定義(Luau は走らせない)---
        const auto parsed = ParseTableBytes(table.tableBytes);
        if (!parsed)
            return std::unexpected(std::format("表の中身を読めない: {}", parsed.error()));

        const auto definition = ReadReactionTableDefinition(*parsed);
        if (!definition)
            return std::unexpected(definition.error());

        ReactionTableDocument document{
            .packageRoot = table.packageRoot, .version = table.tableVersion, .warnings = table.table.warnings};

        // --- 行 ---
        for (const sim::ElementDefinition& element : definition->elements) {
            document.elements.push_back(
                {.name = element.name,
                 .atomicMass = MakeInteger(
                     {std::string(REACTION_ELEMENTS_CATEGORY), element.name, "atomic_mass_mg_per_mol"},
                     "atomic_mass_mg_per_mol", element.atomicMass, 1, UINT32_LIMIT)});
        }

        SpeciesByName speciesByName;
        for (const sim::SpeciesDefinition& species : definition->species) {
            speciesByName.emplace(species.name, &species);
            document.species.push_back(MakeSpeciesRow(species));
        }

        for (const sim::RuleDefinition& rule : definition->rules)
            document.rules.push_back(MakeRuleRow(rule, speciesByName));

        // --- 書き戻す所(フォルダが無い表 = 再生ファイルの表は書き戻せない)---
        if (table.packageRoot.empty()) {
            MarkAllReadOnly(document, "再生ファイルの表(パッケージのフォルダが無い)");
            return document;
        }

        auto packages = ReadPackageRoot(table.packageRoot);
        if (!packages) {
            MarkAllReadOnly(document, std::format("パッケージのフォルダを読めない: {}", packages.error()));
            return document;
        }

        LocateAll(SourceIndex(table.packageRoot, std::move(*packages), table.loadOrder), document);

        return document;
    }

    std::expected<std::string, std::string> ReactionValueLiteral(const ReactionValue& value, std::string_view text) {
        if (value.kind == ReactionValueKind::Decimal) {
            if (const auto decimal = ParseDecimal(text); !decimal)
                return std::unexpected(decimal.error());

            return FormatLuauString(text);
        }

        int64_t integer = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), integer);
        if (error != std::errc{} || end != text.data() + text.size())
            return std::unexpected(std::format("整数で書く: \"{}\"", text));

        if (integer < value.minimum || integer > value.maximum)
            return std::unexpected(std::format("{} は {}〜{} の範囲で書く", value.label, value.minimum, value.maximum));

        return FormatLuauInteger(integer);
    }

    std::expected<void, std::string> WriteReactionValue(const ReactionValue& value, std::string_view text) {
        if (value.file.empty())
            return std::unexpected(std::format("{} は書き戻せない: {}", value.label, value.readOnly));

        const auto literal = ReactionValueLiteral(value, text);
        if (!literal)
            return std::unexpected(literal.error());

        // --- 今のファイルの中でもう一度探す(パネルを作った後に人が書き換えたかもしれない)---
        const std::string source = ReadText(value.file);
        const auto span = FindLuauLiteral(source, value.sourceKeyPath);
        if (!span)
            return std::unexpected(std::format("{}: {}", PathText(value.file), span.error()));

        if (SameValue(value, span->Text(source), LuauLiteralValue(*literal).value_or("")))
            return {};

        const std::string edited = ReplaceLuauLiteral(source, *span, *literal);
        std::ofstream file(value.file, std::ios::binary | std::ios::trunc);
        file << edited;
        file.close();
        if (!file)
            return std::unexpected(std::format("{} に書けない", PathText(value.file)));

        return {};
    }

    const ReactionValue* FindReactionValue(const ReactionTableDocument& document,
                                           std::span<const std::string> keyPath) {
        const auto matches = [&](const ReactionValue& value) {
            return std::ranges::equal(value.keyPath, keyPath);
        };
        for (const ElementRow& row : document.elements) {
            if (matches(row.atomicMass))
                return &row.atomicMass;
        }

        for (const SpeciesRow& row : document.species) {
            for (const ReactionValue* value : {&row.formationEnthalpy, &row.heatCapacity, &row.thermalConductivity}) {
                if (matches(*value))
                    return value;
            }
        }

        for (const RuleRow& row : document.rules) {
            for (const ReactionValue* value : {&row.preExponential, &row.activationEnergy}) {
                if (matches(*value))
                    return value;
            }

            if (row.declaredEnthalpy && matches(*row.declaredEnthalpy))
                return &*row.declaredEnthalpy;

            for (const ReactionValue& coefficient : row.coefficients) {
                if (matches(coefficient))
                    return &coefficient;
            }
        }

        return nullptr;
    }

}  // namespace bicameral::script
