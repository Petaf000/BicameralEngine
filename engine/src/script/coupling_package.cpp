// coupling_package.cpp — 分類 drags・machines を読む(coupling_package.h。T-0536・ADR-0238)。
// ここで落とすのは「形」の誤り(欄の綴り・型・流れの書き方・整数でない数・範囲の外)。物質の名前・分類で使えない流れの種類・
// X = Y・|q| > 1 の押さえはベイク(sim::BakeCouplings)が確かめる。
#include "script/coupling_package.h"

#include <array>
#include <format>

#include "script/package_fields.h"
#include "script/reaction_package.h"

namespace bicameral::script {

    namespace {

        using sim::CouplingDefinition;
        using sim::CouplingForm;
        using sim::CouplingQuantityDefinition;
        using sim::CouplingQuantityKind;

        constexpr int64_t
            COUPLING_WRITTEN_MAX = 1000;  // 書ける |q| の上限(形の誤りとの境。1 を超える分はベイクが押さえる)
        constexpr int64_t COST_MAX = int64_t{1} << 31;   // J/mol(約 2 GJ/mol。刻みの単位へ直しても 64 ビットに収まる)
        constexpr int64_t SPEED_MAX = int64_t{1} << 40;  // W
        constexpr uint64_t MANTISSA_SAFE = uint64_t{1} << 46;  // × 65536 が 63 ビットに収まる仮数
        constexpr std::string_view SPECIES_PREFIX = "species:";

        // "heat"・"work"・"gas"・"species:<物質>"
        std::expected<CouplingQuantityDefinition, std::string> ReadQuantity(const ScriptValue& table,
                                                                            std::string_view field,
                                                                            std::string_view path) {
            const ScriptValue* value = table.Find(field);
            const std::string fieldPath = std::format("{}.{}", path, field);
            if (value == nullptr || !value->IsString())
                return std::unexpected(
                    std::format("{}: \"heat\"・\"work\"・\"gas\"・\"species:<物質>\" のどれかで書く", fieldPath));

            const std::string& text = value->text;
            if (text == "heat")
                return CouplingQuantityDefinition{.kind = CouplingQuantityKind::Heat, .species = {}};

            if (text == "work")
                return CouplingQuantityDefinition{.kind = CouplingQuantityKind::Work, .species = {}};

            if (text == "gas")
                return CouplingQuantityDefinition{.kind = CouplingQuantityKind::Gas, .species = {}};

            if (text.starts_with(SPECIES_PREFIX) && text.size() > SPECIES_PREFIX.size())
                return CouplingQuantityDefinition{.kind = CouplingQuantityKind::Species,
                                                  .species = text.substr(SPECIES_PREFIX.size())};

            return std::unexpected(std::format(
                "{}: \"{}\" は流れの書き方でない(\"heat\"・\"work\"・\"gas\"・\"species:<物質>\")", fieldPath, text));
        }

        // 10 進(符号なし)を Q16 に(0 の向きに切り捨て)
        std::expected<int64_t, std::string> DecimalToQ16(DecimalNumber decimal, std::string_view path) {
            if (decimal.mantissa == 0)
                return 0;

            const std::string tooLarge = std::format("{}: |q| は {} まで(1 を超える分はベイクが押さえる)", path,
                                                     COUPLING_WRITTEN_MAX);
            if (decimal.exponent10 >= 0) {
                uint64_t whole = decimal.mantissa;
                for (int32_t step = 0; step < decimal.exponent10; ++step) {
                    if (whole > static_cast<uint64_t>(COUPLING_WRITTEN_MAX))
                        return std::unexpected(tooLarge);

                    whole *= 10;
                }

                if (whole > static_cast<uint64_t>(COUPLING_WRITTEN_MAX))
                    return std::unexpected(tooLarge);

                return static_cast<int64_t>(whole) * sim::COUPLING_ONE_Q16;
            }

            // 小数: 仮数を 63 ビットに収まるまで 10 で割り(切り捨て。0 の向きなので |q| は書いた値を超えない)、10^桁 で割る
            uint64_t mantissa = decimal.mantissa;
            int32_t digits = -decimal.exponent10;
            while (mantissa > MANTISSA_SAFE && digits > 0) {
                mantissa /= 10;
                --digits;
            }

            if (digits > 18)
                return 0;

            uint64_t divisor = 1;
            for (int32_t step = 0; step < digits; ++step)
                divisor *= 10;

            const uint64_t q16 = mantissa * static_cast<uint64_t>(sim::COUPLING_ONE_Q16) / divisor;
            if (q16 > static_cast<uint64_t>(COUPLING_WRITTEN_MAX * sim::COUPLING_ONE_Q16))
                return std::unexpected(tooLarge);

            return static_cast<int64_t>(q16);
        }

        std::expected<int64_t, std::string> ReadCouplingField(const ScriptValue& table, std::string_view path) {
            const auto value = RequireField(table, "coupling", path);
            if (!value)
                return std::unexpected(value.error());

            return ReadCouplingQ16(**value, std::format("{}.coupling", path));
        }

        // drags.鍵 = { flow, drags, coupling, within? }
        std::expected<CouplingDefinition, std::string> ReadDrag(const std::string& key, const ScriptValue& value,
                                                                const std::string& path) {
            static constexpr std::array<std::string_view, 4> FIELDS = {"flow", "drags", "coupling", "within"};
            if (auto record = CheckRecord(value, FIELDS, path); !record)
                return std::unexpected(record.error());

            CouplingDefinition definition{.key = key, .form = CouplingForm::Drag};
            auto driver = ReadQuantity(value, "flow", path);
            if (!driver)
                return std::unexpected(driver.error());

            auto driven = ReadQuantity(value, "drags", path);
            if (!driven)
                return std::unexpected(driven.error());

            const auto coupling = ReadCouplingField(value, path);
            if (!coupling)
                return std::unexpected(coupling.error());

            definition.driver = std::move(*driver);
            definition.driven = std::move(*driven);
            definition.couplingQ16 = *coupling;
            if (const ScriptValue* within = value.Find("within")) {
                if (!within->IsString() || within->text.empty())
                    return std::unexpected(std::format("{}.within: 物質の名前(空でない文字列)で書く", path));

                definition.withinSpecies = within->text;
            }

            return definition;
        }

        // machines.鍵 = { store, push, coupling, cost_j_per_mol, speed_w }
        std::expected<CouplingDefinition, std::string> ReadMachine(const std::string& key, const ScriptValue& value,
                                                                   const std::string& path) {
            static constexpr std::array<std::string_view, 5> FIELDS = {"store", "push", "coupling", "cost_j_per_mol",
                                                                       "speed_w"};
            if (auto record = CheckRecord(value, FIELDS, path); !record)
                return std::unexpected(record.error());

            CouplingDefinition definition{.key = key, .form = CouplingForm::Machine};
            auto driver = ReadQuantity(value, "store", path);
            if (!driver)
                return std::unexpected(driver.error());

            auto driven = ReadQuantity(value, "push", path);
            if (!driven)
                return std::unexpected(driven.error());

            const auto coupling = ReadCouplingField(value, path);
            if (!coupling)
                return std::unexpected(coupling.error());

            const auto cost = ReadIntegerField(value, "cost_j_per_mol", path, -COST_MAX, COST_MAX);
            if (!cost)
                return std::unexpected(cost.error());

            const auto speed = ReadIntegerField(value, "speed_w", path, -SPEED_MAX, SPEED_MAX);
            if (!speed)
                return std::unexpected(speed.error());

            definition.driver = std::move(*driver);
            definition.driven = std::move(*driven);
            definition.couplingQ16 = *coupling;
            definition.costJoulesPerMole = *cost;
            definition.speedWatts = *speed;

            return definition;
        }

    }  // namespace

    std::expected<int64_t, std::string> ReadCouplingQ16(const ScriptValue& value, std::string_view path) {
        if (value.IsNumber()) {
            const auto whole = ReadInteger(value, path, -COUPLING_WRITTEN_MAX, COUPLING_WRITTEN_MAX);
            if (!whole)
                return std::unexpected(std::format("{}(小数は \"0.9\" のように文字列で書く)", whole.error()));

            return *whole * sim::COUPLING_ONE_Q16;
        }

        if (!value.IsString() || value.text.empty())
            return std::unexpected(std::format("{}: 10 進の文字列(\"0.9\"・\"-0.25\")か整数で書く", path));

        const bool negative = value.text.front() == '-';
        const auto decimal = ParseDecimal(negative ? std::string_view(value.text).substr(1) : value.text);
        if (!decimal)
            return std::unexpected(std::format("{}: {}", path, decimal.error()));

        const auto magnitude = DecimalToQ16(*decimal, path);
        if (!magnitude)
            return std::unexpected(magnitude.error());

        return negative ? -*magnitude : *magnitude;
    }

    std::expected<std::vector<sim::CouplingDefinition>, std::string> ReadCouplings(const PackageSetResult& packages) {
        std::vector<CouplingDefinition> definitions;
        if (auto result = ReadCategory(packages, DRAGS_CATEGORY, ReadDrag, definitions); !result)
            return std::unexpected(result.error());

        if (auto result = ReadCategory(packages, MACHINES_CATEGORY, ReadMachine, definitions); !result)
            return std::unexpected(result.error());

        return definitions;
    }

}  // namespace bicameral::script
