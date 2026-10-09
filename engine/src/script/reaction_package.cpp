// reaction_package.cpp — Luau のパッケージの反応表(分類 elements・species・reactions)を sim::ReactionTableDefinition に読む
// (reaction_package.h。T-0021)。
// 読むだけで、中身の検査(元素の釣り合い・知らない物質・係数・速度の範囲)はベイクに任せる。ここで落とすのは「形」の誤り
// (欄の綴り・型・整数でない数・範囲の外)。どの値も Luau の倍精度から誤差なしで整数に直せるものだけ通す(04 R7)。
#include "script/reaction_package.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace bicameral::script {

    namespace {

        using sim::ElementCount;
        using sim::ElementDefinition;
        using sim::ReactionTableDefinition;
        using sim::RuleDefinition;
        using sim::RuleTerm;
        using sim::SpeciesDefinition;

        constexpr int64_t EXACT_INTEGER_LIMIT = int64_t{1} << 53;  // 倍精度で誤差なしに表せる整数の上限
        constexpr int64_t UINT32_LIMIT = std::numeric_limits<uint32_t>::max();
        constexpr size_t DECIMAL_DIGITS_MAX = 19;  // 10^19 − 1 < 2^64
        constexpr int64_t DECIMAL_EXPONENT_MAX = 999;

        // --- 欄を読む道具 ---

        // 表の鍵が全部 allowed の中の文字列か(知らない欄は綴りの誤りとして落とす)
        std::expected<void, std::string> CheckFields(const ScriptValue& table,
                                                     std::span<const std::string_view> allowed, std::string_view path) {
            for (const ScriptField& field : table.fields) {
                const bool known = field.key.IsString() &&
                                   std::find(allowed.begin(), allowed.end(), field.key.text) != allowed.end();
                if (!known)
                    return std::unexpected(std::format("{}: 知らない欄 {}", path, ToDebugText(field.key)));
            }

            return {};
        }

        std::expected<const ScriptValue*, std::string> RequireField(const ScriptValue& table, std::string_view field,
                                                                    std::string_view path) {
            const ScriptValue* value = table.Find(field);
            if (value == nullptr)
                return std::unexpected(std::format("{}: 欄 {} がない", path, field));

            return value;
        }

        std::expected<void, std::string> RequireTable(const ScriptValue& value, std::string_view path) {
            if (!value.IsTable())
                return std::unexpected(std::format("{}: 表で書く", path));

            return {};
        }

        // 整数で書いた数を読む(小数・±2^53 の外・[minimum, maximum] の外は誤り)
        std::expected<int64_t, std::string> ReadInteger(const ScriptValue& value, std::string_view path,
                                                        int64_t minimum, int64_t maximum) {
            constexpr auto LIMIT = static_cast<double>(EXACT_INTEGER_LIMIT);
            const bool exact = value.IsNumber() && value.number >= -LIMIT && value.number <= LIMIT &&
                               std::trunc(value.number) == value.number;
            if (!exact)
                return std::unexpected(std::format("{}: 整数(±2^53 まで)で書く", path));

            const auto integer = static_cast<int64_t>(value.number);
            if (integer < minimum || integer > maximum)
                return std::unexpected(std::format("{}: {} は範囲 {}〜{} の外", path, integer, minimum, maximum));

            return integer;
        }

        std::expected<int64_t, std::string> ReadIntegerField(const ScriptValue& table, std::string_view field,
                                                             std::string_view path, int64_t minimum, int64_t maximum) {
            const auto value = RequireField(table, field, path);
            if (!value)
                return std::unexpected(value.error());

            return ReadInteger(**value, std::format("{}.{}", path, field), minimum, maximum);
        }

        // 表であることと、知らない欄が無いことを確かめる
        std::expected<void, std::string> CheckRecord(const ScriptValue& value,
                                                     std::span<const std::string_view> allowed, std::string_view path) {
            if (auto table = RequireTable(value, path); !table)
                return table;

            return CheckFields(value, allowed, path);
        }

        // --- 元素 ---

        std::expected<ElementDefinition, std::string> ReadElement(const std::string& name, const ScriptValue& value,
                                                                  const std::string& path) {
            static constexpr std::string_view FIELDS[] = {"atomic_mass_mg_per_mol"};
            if (auto record = CheckRecord(value, FIELDS, path); !record)
                return std::unexpected(record.error());

            const auto mass = ReadIntegerField(value, "atomic_mass_mg_per_mol", path, 1, UINT32_LIMIT);
            if (!mass)
                return std::unexpected(mass.error());

            return ElementDefinition{.name = name, .atomicMass = static_cast<uint32_t>(*mass)};
        }

        // --- 物質 ---

        std::expected<std::vector<ElementCount>, std::string> ReadComposition(const ScriptValue& value,
                                                                              const std::string& path) {
            if (auto table = RequireTable(value, path); !table)
                return std::unexpected(table.error());

            std::vector<ElementCount> composition;
            for (const ScriptField& field : value.fields) {
                if (!field.key.IsString())
                    return std::unexpected(std::format("{}: 鍵は元素の名前", path));

                const auto count = ReadInteger(field.value, std::format("{}.{}", path, field.key.text), 1,
                                               UINT32_LIMIT);
                if (!count)
                    return std::unexpected(count.error());

                composition.push_back({.element = field.key.text, .count = static_cast<uint32_t>(*count)});
            }

            return composition;
        }

        std::expected<SpeciesDefinition, std::string> ReadSpecies(const std::string& name, const ScriptValue& value,
                                                                  const std::string& path) {
            static constexpr std::string_view FIELDS[] = {"composition", "formation_enthalpy_j_per_mol",
                                                          "heat_capacity_mj_per_mol_k",
                                                          "thermal_conductivity_mw_per_m_k"};
            if (auto record = CheckRecord(value, FIELDS, path); !record)
                return std::unexpected(record.error());

            const auto compositionValue = RequireField(value, "composition", path);
            if (!compositionValue)
                return std::unexpected(compositionValue.error());

            auto composition = ReadComposition(**compositionValue, path + ".composition");
            if (!composition)
                return std::unexpected(composition.error());

            const auto enthalpy = ReadIntegerField(value, "formation_enthalpy_j_per_mol", path, -EXACT_INTEGER_LIMIT,
                                                   EXACT_INTEGER_LIMIT);
            const auto heatCapacity = ReadIntegerField(value, "heat_capacity_mj_per_mol_k", path, 1, UINT32_LIMIT);
            const auto conductivity = ReadIntegerField(value, "thermal_conductivity_mw_per_m_k", path, 0, UINT32_LIMIT);
            for (const auto* result : {&enthalpy, &heatCapacity, &conductivity}) {
                if (!*result)
                    return std::unexpected(result->error());
            }

            return SpeciesDefinition{.name = name,
                                     .composition = std::move(*composition),
                                     .formationEnthalpy = *enthalpy,
                                     .heatCapacity = static_cast<uint32_t>(*heatCapacity),
                                     .thermalConductivity = static_cast<uint32_t>(*conductivity)};
        }

        // --- 規則 ---

        // { 物質 = 係数 } を項の並びに。次数は ReadOrders が後で書く
        std::expected<std::vector<RuleTerm>, std::string> ReadTerms(const ScriptValue& value, const std::string& path) {
            if (auto table = RequireTable(value, path); !table)
                return std::unexpected(table.error());

            std::vector<RuleTerm> terms;
            for (const ScriptField& field : value.fields) {
                if (!field.key.IsString())
                    return std::unexpected(std::format("{}: 鍵は物質の名前", path));

                const auto coefficient = ReadInteger(field.value, std::format("{}.{}", path, field.key.text), 1,
                                                     UINT32_LIMIT);
                if (!coefficient)
                    return std::unexpected(coefficient.error());

                terms.push_back({.species = field.key.text,
                                 .coefficient = static_cast<uint32_t>(*coefficient),
                                 .firstOrder = false});
            }

            return terms;
        }

        // orders = { 物質 = 0 か 1 }。省けば反応物は全部次数 1。書いたら、書かない反応物は次数 0
        std::expected<void, std::string> ReadOrders(const ScriptValue* orders, std::vector<RuleTerm>& reactants,
                                                    const std::string& path) {
            if (orders == nullptr) {
                for (RuleTerm& term : reactants)
                    term.firstOrder = true;

                return {};
            }

            if (auto table = RequireTable(*orders, path); !table)
                return table;

            for (const ScriptField& field : orders->fields) {
                const auto term = std::find_if(reactants.begin(), reactants.end(), [&](const RuleTerm& reactant) {
                    return field.key.IsString() && reactant.species == field.key.text;
                });
                if (term == reactants.end())
                    return std::unexpected(std::format("{}: {} は反応物ではない", path, ToDebugText(field.key)));

                const auto order = ReadInteger(field.value, std::format("{}.{}", path, field.key.text), 0, 1);
                if (!order)
                    return std::unexpected(order.error());

                term->firstOrder = *order == 1;
            }

            return {};
        }

        // A は 10 進の文字列か整数
        std::expected<DecimalNumber, std::string> ReadPreExponential(const ScriptValue& value,
                                                                     const std::string& path) {
            if (value.IsString()) {
                auto parsed = ParseDecimal(value.text);
                if (!parsed)
                    return std::unexpected(std::format("{}: {}", path, parsed.error()));

                return parsed;
            }

            const auto integer = ReadInteger(value, path, 1, EXACT_INTEGER_LIMIT);
            if (!integer)
                return std::unexpected(std::format("{}(10 進の文字列 \"2.8e19\" でもよい)", integer.error()));

            return ParseDecimal(std::to_string(*integer));
        }

        std::expected<sim::ArrheniusRate, std::string> ReadRate(const ScriptValue& value, const std::string& path) {
            static constexpr std::string_view FIELDS[] = {"a", "activation_energy_j_per_mol"};
            if (auto record = CheckRecord(value, FIELDS, path); !record)
                return std::unexpected(record.error());

            const auto preExponentialValue = RequireField(value, "a", path);
            if (!preExponentialValue)
                return std::unexpected(preExponentialValue.error());

            const auto preExponential = ReadPreExponential(**preExponentialValue, path + ".a");
            if (!preExponential)
                return std::unexpected(preExponential.error());

            const auto activation = ReadIntegerField(value, "activation_energy_j_per_mol", path, 0,
                                                     EXACT_INTEGER_LIMIT);
            if (!activation)
                return std::unexpected(activation.error());

            return sim::ArrheniusRate{.preExponentialMantissa = preExponential->mantissa,
                                      .preExponentialExponent10 = preExponential->exponent10,
                                      .activationEnergy = *activation};
        }

        // reactants・products を読む
        std::expected<void, std::string> ReadRuleTerms(const ScriptValue& value, const std::string& path,
                                                       RuleDefinition& rule) {
            for (const auto& [field, terms] : {std::pair{"reactants", &rule.reactants}, {"products", &rule.products}}) {
                const auto termsValue = RequireField(value, field, path);
                if (!termsValue)
                    return std::unexpected(termsValue.error());

                auto read = ReadTerms(**termsValue, std::format("{}.{}", path, field));
                if (!read)
                    return std::unexpected(read.error());

                *terms = std::move(*read);
            }

            return ReadOrders(value.Find("orders"), rule.reactants, path + ".orders");
        }

        std::expected<RuleDefinition, std::string> ReadRule(const std::string& name, const ScriptValue& value,
                                                            const std::string& path) {
            static constexpr std::string_view FIELDS[] = {"reactants", "products", "rate", "orders",
                                                          "reaction_enthalpy_j_per_mol"};
            if (auto record = CheckRecord(value, FIELDS, path); !record)
                return std::unexpected(record.error());

            RuleDefinition rule{.name = name};
            if (auto terms = ReadRuleTerms(value, path, rule); !terms)
                return std::unexpected(terms.error());

            const auto rateValue = RequireField(value, "rate", path);
            if (!rateValue)
                return std::unexpected(rateValue.error());

            const auto rate = ReadRate(**rateValue, path + ".rate");
            if (!rate)
                return std::unexpected(rate.error());

            rule.rate = *rate;
            if (const ScriptValue* declared = value.Find("reaction_enthalpy_j_per_mol")) {
                const auto enthalpy = ReadInteger(*declared, path + ".reaction_enthalpy_j_per_mol",
                                                  -EXACT_INTEGER_LIMIT, EXACT_INTEGER_LIMIT);
                if (!enthalpy)
                    return std::unexpected(enthalpy.error());

                rule.declaredReactionEnthalpy = *enthalpy;
            }

            return rule;
        }

        // --- 分類ごと ---

        // 分類 category の鍵を全部 read で読み、out に足す(鍵はバイト順。ベイクも名前の順に並べ直す)
        template <typename Definition, typename Reader>
        std::expected<void, std::string> ReadCategory(const PackageSetResult& packages, std::string_view category,
                                                      Reader read, std::vector<Definition>& out) {
            const auto found = packages.tables.find(std::string(category));
            if (found == packages.tables.end())
                return {};

            for (const auto& [key, entry] : found->second) {
                const std::string path = std::format("{}.{}", category, key);
                auto definition = read(key, entry.value, path);
                if (!definition)
                    return std::unexpected(std::format("パッケージ {}: {}", entry.package, definition.error()));

                out.push_back(std::move(*definition));
            }

            return {};
        }

        // --- 10 進の数 ---

        // 仮数の数字の並び(先頭と末尾の 0 を落とし、落とした末尾の数を指数へ)を数にする
        std::expected<DecimalNumber, std::string> DigitsToDecimal(std::string digits, int64_t exponent10) {
            const size_t first = digits.find_first_not_of('0');
            if (first == std::string::npos)
                return std::unexpected("0 より大きい数で書く");

            digits.erase(0, first);
            while (digits.back() == '0') {
                digits.pop_back();
                exponent10 += 1;
            }

            if (digits.size() > DECIMAL_DIGITS_MAX)
                return std::unexpected(std::format("有効数字は {} 桁まで", DECIMAL_DIGITS_MAX));

            if (exponent10 < -DECIMAL_EXPONENT_MAX || exponent10 > DECIMAL_EXPONENT_MAX)
                return std::unexpected(std::format("指数は ±{} まで", DECIMAL_EXPONENT_MAX));

            uint64_t mantissa = 0;
            for (const char digit : digits)
                mantissa = (mantissa * 10) + static_cast<uint64_t>(digit - '0');

            return DecimalNumber{.mantissa = mantissa, .exponent10 = static_cast<int32_t>(exponent10)};
        }

        bool IsDigit(char character) {
            return character >= '0' && character <= '9';
        }

        // "e" の後(符号と 1〜4 桁)を読む
        std::expected<int64_t, std::string> ParseExponent(std::string_view text) {
            const bool negative = !text.empty() && text.front() == '-';
            if (!text.empty() && (text.front() == '-' || text.front() == '+'))
                text.remove_prefix(1);

            if (text.empty() || text.size() > 4 || !std::ranges::all_of(text, IsDigit))
                return std::unexpected("指数の書き方の誤り");

            int64_t written = 0;
            for (const char digit : text)
                written = (written * 10) + (digit - '0');

            return negative ? -written : written;
        }

    }  // namespace

    std::expected<DecimalNumber, std::string> ParseDecimal(std::string_view text) {
        const std::string error = std::format("10 進の数(例 \"2.8e19\")で書く: \"{}\"", text);
        std::string digits;
        int64_t exponent10 = 0;
        size_t position = 0;
        bool seenPoint = false;
        for (; position < text.size() && text[position] != 'e' && text[position] != 'E'; ++position) {
            const char character = text[position];
            if (character == '.' && !seenPoint) {
                seenPoint = true;
                continue;
            }

            if (!IsDigit(character))
                return std::unexpected(error);

            digits.push_back(character);
            exponent10 -= seenPoint ? 1 : 0;
        }

        if (digits.empty())
            return std::unexpected(error);

        if (position < text.size()) {
            const auto written = ParseExponent(text.substr(position + 1));
            if (!written)
                return std::unexpected(error);

            exponent10 += *written;
        }

        auto decimal = DigitsToDecimal(std::move(digits), exponent10);
        if (!decimal)
            return std::unexpected(std::format("{}: \"{}\"", decimal.error(), text));

        return decimal;
    }

    std::expected<ReactionTableDefinition, std::string> ReadReactionTableDefinition(const PackageSetResult& packages) {
        ReactionTableDefinition definition;
        if (auto result = ReadCategory(packages, REACTION_ELEMENTS_CATEGORY, ReadElement, definition.elements); !result)
            return std::unexpected(result.error());

        if (auto result = ReadCategory(packages, REACTION_SPECIES_CATEGORY, ReadSpecies, definition.species); !result)
            return std::unexpected(result.error());

        if (auto result = ReadCategory(packages, REACTION_RULES_CATEGORY, ReadRule, definition.rules); !result)
            return std::unexpected(result.error());

        return definition;
    }

}  // namespace bicameral::script
