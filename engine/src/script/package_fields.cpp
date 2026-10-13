// package_fields.cpp — パッケージの欄を読む道具(package_fields.h。T-0398 で reaction_package.cpp から出した)。
#include "script/package_fields.h"

#include <algorithm>
#include <cmath>

namespace bicameral::script {

    std::expected<void, std::string> CheckFields(const ScriptValue& table, std::span<const std::string_view> allowed,
                                                 std::string_view path) {
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

    std::expected<int64_t, std::string> ReadInteger(const ScriptValue& value, std::string_view path, int64_t minimum,
                                                    int64_t maximum) {
        constexpr auto LIMIT = static_cast<double>(PACKAGE_EXACT_INTEGER_LIMIT);
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

    std::expected<void, std::string> CheckRecord(const ScriptValue& value, std::span<const std::string_view> allowed,
                                                 std::string_view path) {
        if (auto table = RequireTable(value, path); !table)
            return table;

        return CheckFields(value, allowed, path);
    }

}  // namespace bicameral::script
