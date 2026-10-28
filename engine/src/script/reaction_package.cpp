// reaction_package.cpp — Luau のパッケージの反応表(分類 elements・species・reactions)を sim::ReactionTableDefinition に読む
// (reaction_package.h。T-0021)。
// 読むだけで、中身の検査(元素の釣り合い・知らない物質・係数・速度の範囲)はベイクに任せる。ここで落とすのは「形」の誤り
// (欄の綴り・型・整数でない数・範囲の外)。どの値も Luau の倍精度から誤差なしで整数に直せるものだけ通す(04 R7)。
#include "script/reaction_package.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "script/coupling_package.h"
#include "script/environment_package.h"
#include "script/material_templates_package.h"
#include "script/package_fields.h"
#include "script/play_metrics_package.h"
#include "script/world_layout_package.h"

namespace bicameral::script {

    namespace {

        using sim::ElementCount;
        using sim::ElementDefinition;
        using sim::ReactionTableDefinition;
        using sim::RuleDefinition;
        using sim::RuleTerm;
        using sim::SpeciesDefinition;

        constexpr int64_t EXACT_INTEGER_LIMIT = PACKAGE_EXACT_INTEGER_LIMIT;  // 倍精度で誤差なしに表せる整数の上限
        constexpr int64_t UINT32_LIMIT = std::numeric_limits<uint32_t>::max();
        constexpr size_t DECIMAL_DIGITS_MAX = 19;  // 10^19 − 1 < 2^64
        constexpr int64_t DECIMAL_EXPONENT_MAX = 999;

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

        // 物質の相(phase)を読む。廃止した condensed も書いてあれば同じ向きであること(食い違いは誤り。T-0710)
        std::expected<void, std::string> ReadPhase(const ScriptValue& phase, const ScriptValue* condensed,
                                                   const std::string& path, SpeciesDefinition& species) {
            if (!phase.IsString() || (phase.text != "gas" && phase.text != "solid" && phase.text != "liquid"))
                return std::unexpected(std::format("{}.phase: \"gas\"・\"solid\"・\"liquid\" のどれかで書く", path));

            const bool phaseCondensed = phase.text != "gas";
            if (condensed != nullptr && condensed->IsBoolean() && condensed->boolean != phaseCondensed)
                return std::unexpected(std::format("{}: phase と condensed が食い違う", path));

            species.phase = phase.text == "gas"      ? sim::SpeciesPhase::Gas
                            : phase.text == "liquid" ? sim::SpeciesPhase::Liquid
                                                     : sim::SpeciesPhase::Solid;

            return {};
        }

        // --- 物質の動き方の任意の値(T-0710・土台の決定 3。相に合うかはベイクが確かめる)---

        // 孔の通しやすさの上限(µm²。1e-5 m²。砂利でも 1e-7 m² ほど)と、持つ単位(1 µm² = 10^6 nm²)
        constexpr int64_t PERMEABILITY_MAX_SQUARE_MICROMETERS = 10000000;
        constexpr uint64_t SQUARE_NANOMETERS_PER_SQUARE_MICROMETER = 1000000;

        // 孔の通しやすさ(µm²)を nm² の整数に。整数か 10 進の文字列("1e-6"・"0.001")。1 nm² より細かい端数は誤り
        std::expected<uint64_t, std::string> ReadPermeability(const ScriptValue& value, const std::string& path) {
            const std::string range = std::format("{}: 0 から {} µm² までを 1e-6 µm² 刻みで書く", path,
                                                  PERMEABILITY_MAX_SQUARE_MICROMETERS);
            if (!value.IsString()) {
                const auto integer = ReadInteger(value, path, 0, PERMEABILITY_MAX_SQUARE_MICROMETERS);
                if (!integer)
                    return std::unexpected(integer.error());

                return static_cast<uint64_t>(*integer) * SQUARE_NANOMETERS_PER_SQUARE_MICROMETER;
            }

            const auto decimal = ParseDecimal(value.text);
            if (!decimal)
                return std::unexpected(std::format("{}: {}", path, decimal.error()));

            // nm² = 仮数 × 10^(指数 + 6)
            uint64_t squareNanometers = decimal->mantissa;
            const uint64_t limit = static_cast<uint64_t>(PERMEABILITY_MAX_SQUARE_MICROMETERS) *
                                   SQUARE_NANOMETERS_PER_SQUARE_MICROMETER;
            for (int32_t shift = decimal->exponent10 + 6; shift != 0; shift += shift < 0 ? 1 : -1) {
                if (shift < 0 && squareNanometers % 10 != 0)
                    return std::unexpected(range);

                if (shift > 0 && squareNanometers > limit)
                    return std::unexpected(range);

                squareNanometers = shift < 0 ? squareNanometers / 10 : squareNanometers * 10;
            }

            if (squareNanometers > limit)
                return std::unexpected(range);

            return squareNanometers;
        }

        // 固体が動くか(solid_motion = "fixed" | "loose")と安息角(repose_angle_deg。0〜90 の整数)・粒の詰まり(packing_percent。D-679)。T-0819・ADR-0359 §1。
        // どちらも任意(書かない値は無し = 今と同じ)。固体だけに書けるか・書き忘れはベイクが確かめる
        std::expected<void, std::string> ReadSolidMotion(const ScriptValue& value, const std::string& path,
                                                         sim::SpeciesTransport& transport) {
            if (const ScriptValue* declared = value.Find("solid_motion")) {
                if (!declared->IsString() || (declared->text != "fixed" && declared->text != "loose"))
                    return std::unexpected(std::format("{}.solid_motion: \"fixed\"・\"loose\" のどちらかで書く", path));

                transport.solidMotion = declared->text == "loose" ? sim::SolidMotion::Loose : sim::SolidMotion::Fixed;
            }

            if (const ScriptValue* declared = value.Find("repose_angle_deg")) {
                const auto angle = ReadInteger(*declared, path + ".repose_angle_deg", 0, sim::REPOSE_ANGLE_MAX_DEGREES);
                if (!angle)
                    return std::unexpected(angle.error());

                transport.reposeAngleDegrees = static_cast<uint32_t>(*angle);
            }

            // 粒の詰まり(packing_percent。1〜100 の整数。D-679)
            if (const ScriptValue* declared = value.Find("packing_percent")) {
                const auto packing = ReadInteger(*declared, path + ".packing_percent", sim::PACKING_PERCENT_MIN,
                                                 sim::PACKING_PERCENT_MAX);
                if (!packing)
                    return std::unexpected(packing.error());

                transport.packingPercent = static_cast<uint32_t>(*packing);
            }

            return {};
        }

        // 粘り・混ざる速さ・孔・吸い上げ・固体の動き方(どれも任意。書かない値は無し = 今と同じ動き)
        std::expected<void, std::string> ReadTransport(const ScriptValue& value, const std::string& path,
                                                       sim::SpeciesTransport& transport) {
            if (const ScriptValue* declared = value.Find("viscosity_upa_s")) {
                const auto viscosity = ReadInteger(*declared, path + ".viscosity_upa_s", 1, EXACT_INTEGER_LIMIT);
                if (!viscosity)
                    return std::unexpected(viscosity.error());

                transport.viscosityMicroPascalSeconds = static_cast<uint64_t>(*viscosity);
            }

            if (const ScriptValue* declared = value.Find("diffusivity_mm2_per_s")) {
                const auto diffusivity = ReadInteger(*declared, path + ".diffusivity_mm2_per_s", 0, UINT32_LIMIT);
                if (!diffusivity)
                    return std::unexpected(diffusivity.error());

                transport.diffusivityMm2PerS = static_cast<uint32_t>(*diffusivity);
            }

            if (const ScriptValue* declared = value.Find("permeability_um2")) {
                const auto permeability = ReadPermeability(*declared, path + ".permeability_um2");
                if (!permeability)
                    return std::unexpected(permeability.error());

                transport.permeabilityNm2 = *permeability;
            }

            if (const ScriptValue* declared = value.Find("suction_pa")) {
                const auto suction = ReadInteger(*declared, path + ".suction_pa", 0, UINT32_LIMIT);
                if (!suction)
                    return std::unexpected(suction.error());

                transport.suctionPascals = static_cast<uint32_t>(*suction);
            }

            return ReadSolidMotion(value, path, transport);
        }

        // 光の値の欄 1 つ(任意。0〜上限の整数)
        std::expected<void, std::string> ReadOptionalLight(const ScriptValue& value, std::string_view field,
                                                           const std::string& path, int64_t limit,
                                                           std::optional<uint32_t>& destination) {
            const ScriptValue* declared = value.Find(field);
            if (declared == nullptr)
                return {};

            const auto read = ReadInteger(*declared, path + "." + std::string(field), 0, limit);
            if (!read)
                return std::unexpected(read.error());

            destination = static_cast<uint32_t>(*read);

            return {};
        }

        std::expected<void, std::string> ReadSpeciesLight(const ScriptValue& value, const std::string& path,
                                                          sim::SpeciesDefinition& species) {
            constexpr int64_t PERMILLE_LIMIT = 1000;
            // 屈折の値は 1.000〜4.000(屈折率。千分率で書く。T-0878)
            constexpr int64_t REFRACTION_LIMIT = 4000;
            const std::array<std::expected<void, std::string>, 5> results = {
                ReadOptionalLight(value, "light_absorption_short_mm2_per_mol", path, UINT32_LIMIT,
                                  species.lightAbsorptionShort),
                ReadOptionalLight(value, "light_absorption_long_mm2_per_mol", path, UINT32_LIMIT,
                                  species.lightAbsorptionLong),
                ReadOptionalLight(value, "light_reflectance_short_permille", path, PERMILLE_LIMIT,
                                  species.lightReflectanceShortPermille),
                ReadOptionalLight(value, "light_reflectance_long_permille", path, PERMILLE_LIMIT,
                                  species.lightReflectanceLongPermille),
                ReadOptionalLight(value, "light_refraction_permille", path, REFRACTION_LIMIT,
                                  species.lightRefractionPermille)};
            for (const auto& result : results) {
                if (!result)
                    return std::unexpected(result.error());
            }

            if (species.lightRefractionPermille && *species.lightRefractionPermille < 1000)
                return std::unexpected(path + ".light_refraction_permille: 1000(屈折率 1)以上で書く");

            return {};
        }

        std::expected<SpeciesDefinition, std::string> ReadSpecies(const std::string& name, const ScriptValue& value,
                                                                  const std::string& path) {
            static constexpr std::string_view FIELDS[] = {"composition",
                                                          "formation_enthalpy_j_per_mol",
                                                          "heat_capacity_mj_per_mol_k",
                                                          "thermal_conductivity_mw_per_m_k",
                                                          "standard_entropy_mj_per_mol_k",
                                                          "condensed",
                                                          "radiation_absorption_mm2_per_mol",
                                                          "light_absorption_short_mm2_per_mol",
                                                          "light_absorption_long_mm2_per_mol",
                                                          "light_reflectance_short_permille",
                                                          "light_reflectance_long_permille",
                                                          "light_refraction_permille",
                                                          "phase",
                                                          "density_kg_per_m3",
                                                          "viscosity_upa_s",
                                                          "diffusivity_mm2_per_s",
                                                          "permeability_um2",
                                                          "suction_pa",
                                                          "solid_motion",
                                                          "repose_angle_deg",
                                                          "packing_percent"};
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

            SpeciesDefinition species{.name = name,
                                      .composition = std::move(*composition),
                                      .formationEnthalpy = *enthalpy,
                                      .heatCapacity = static_cast<uint32_t>(*heatCapacity),
                                      .thermalConductivity = static_cast<uint32_t>(*conductivity)};

            // 標準エントロピー(任意。坂の検査だけが読む。T-0491)。第三法則で負にはならない
            if (const ScriptValue* declared = value.Find("standard_entropy_mj_per_mol_k")) {
                const auto entropy = ReadInteger(*declared, path + ".standard_entropy_mj_per_mol_k", 0,
                                                 EXACT_INTEGER_LIMIT);
                if (!entropy)
                    return std::unexpected(entropy.error());

                species.standardEntropy = *entropy;
            }

            // 廃止した condensed(T-0710): true は固体・false は気体と読み替え、ベイクが警告する(phase で書く)
            if (const ScriptValue* condensed = value.Find("condensed")) {
                if (condensed->kind != ScriptValue::Kind::Boolean)
                    return std::unexpected(std::format("{}.condensed: true か false で書く", path));

                species.phase = condensed->boolean ? sim::SpeciesPhase::Solid : sim::SpeciesPhase::Gas;
                species.legacyCondensed = true;
            }

            // 1 mol が放射を遮る面積(任意。既定 0 = 放射しない。放射の CPU リファレンスだけが読む。T-0583・ADR-0252)
            if (const ScriptValue* declared = value.Find("radiation_absorption_mm2_per_mol")) {
                const auto absorption = ReadInteger(*declared, path + ".radiation_absorption_mm2_per_mol", 0,
                                                    UINT32_LIMIT);
                if (!absorption)
                    return std::unexpected(absorption.error());

                species.radiationAbsorption = static_cast<uint32_t>(*absorption);
            }

            // 光の値(任意。2 つの帯。名前は仮。書かない値はベイクが決める: sim::BakeSpeciesLight・D-610。T-0910)
            if (auto light = ReadSpeciesLight(value, path, species); !light)
                return std::unexpected(light.error());

            // 相(任意。既定 gas。T-0609・T-0710): "gas" / "solid" / "liquid"。condensed と食い違えば断る
            if (const ScriptValue* phase = value.Find("phase")) {
                if (auto result = ReadPhase(*phase, value.Find("condensed"), path, species); !result)
                    return std::unexpected(result.error());
            }

            // 密度(任意。kg/m³。液体には要る〔ベイクが断る〕)
            if (const ScriptValue* declared = value.Find("density_kg_per_m3")) {
                const auto density = ReadInteger(*declared, path + ".density_kg_per_m3", 1, UINT32_LIMIT);
                if (!density)
                    return std::unexpected(density.error());

                species.density = static_cast<uint32_t>(*density);
            }

            if (auto transport = ReadTransport(value, path, species.transport); !transport)
                return std::unexpected(transport.error());

            return species;
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

        // balance = { temperature_mk, enthalpy_j_per_mol, concentration_mol_per_m3(気体の係数が両辺で違う時)}(T-0623・ADR-0290)
        std::expected<sim::BalanceDefinition, std::string> ReadBalance(const ScriptValue& value,
                                                                       const std::string& path) {
            static constexpr std::string_view FIELDS[] = {"temperature_mk", "enthalpy_j_per_mol",
                                                          "concentration_mol_per_m3"};
            if (auto record = CheckRecord(value, FIELDS, path); !record)
                return std::unexpected(record.error());

            const auto temperature = ReadIntegerField(value, "temperature_mk", path, 1, UINT32_LIMIT);
            if (!temperature)
                return std::unexpected(temperature.error());

            const auto enthalpy = ReadIntegerField(value, "enthalpy_j_per_mol", path, -EXACT_INTEGER_LIMIT,
                                                   EXACT_INTEGER_LIMIT);
            if (!enthalpy)
                return std::unexpected(enthalpy.error());

            sim::BalanceDefinition balance{.temperatureMilliKelvin = static_cast<uint32_t>(*temperature),
                                           .enthalpy = *enthalpy};
            if (const ScriptValue* concentration = value.Find("concentration_mol_per_m3")) {
                const auto parsed = ReadPreExponential(*concentration, path + ".concentration_mol_per_m3");
                if (!parsed)
                    return std::unexpected(parsed.error());

                balance.concentrationMantissa = parsed->mantissa;
                balance.concentrationExponent10 = parsed->exponent10;
            }

            return balance;
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
            static constexpr std::string_view FIELDS[] = {
                "reactants", "products", "rate", "orders", "reaction_enthalpy_j_per_mol", "balance"};
            if (auto record = CheckRecord(value, FIELDS, path); !record)
                return std::unexpected(record.error());

            RuleDefinition rule{.name = name};
            if (const ScriptValue* balance = value.Find("balance")) {
                if (value.Find("orders") != nullptr)
                    return std::unexpected(
                        std::format("{}: 釣り合いの規則に orders は書けない(濃さは物質の相で決まる)", path));

                auto read = ReadBalance(*balance, path + ".balance");
                if (!read)
                    return std::unexpected(read.error());

                rule.balance = *read;
            }

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

        // --- 同じ読み込みに足した分類(世界のデータ。T-0398〜T-0400)。分類ごとに別のファイルで読み、ここで 1 行ずつつなぐ ---
        auto worldLayout = ReadWorldLayout(packages);
        if (!worldLayout)
            return std::unexpected(worldLayout.error());

        definition.worldLayout = std::move(*worldLayout);

        auto templates = ReadMaterialTemplates(packages);
        if (!templates)
            return std::unexpected(templates.error());

        definition.templates = std::move(*templates);

        auto environment = ReadWorldEnvironment(packages);
        if (!environment)
            return std::unexpected(environment.error());

        definition.environment = *environment;

        auto atmosphere = ReadWorldAtmosphere(packages);
        if (!atmosphere)
            return std::unexpected(atmosphere.error());

        definition.atmosphere = std::move(*atmosphere);

        auto playMetrics = ReadPlayMetrics(packages);
        if (!playMetrics)
            return std::unexpected(playMetrics.error());

        definition.playMetrics = std::move(*playMetrics);

        auto couplings = ReadCouplings(packages);  // 分類 drags・machines(1 つの読み手。T-0536)
        if (!couplings)
            return std::unexpected(couplings.error());

        definition.couplings = std::move(*couplings);

        return definition;
    }

}  // namespace bicameral::script
