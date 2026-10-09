// script_value.cpp — ScriptValue(Luau の値の C++ の木)の並べ方・バイト列・ハッシュ(T-0138、ADR-0031)。
#include "script/script_value.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <optional>
#include <utility>

namespace bicameral::script {

    namespace {

        void AppendUint32(uint32_t value, std::string& out) {
            for (int shift = 0; shift < 32; shift += 8)
                out.push_back(static_cast<char>((value >> shift) & 0xFFu));
        }

        void AppendUint64(uint64_t value, std::string& out) {
            for (int shift = 0; shift < 64; shift += 8)
                out.push_back(static_cast<char>((value >> shift) & 0xFFu));
        }

        // 作り直す表の深さの上限(殻が Luau から受け取る値と同じ。ADR-0031 の 5)
        constexpr int MAX_READ_DEPTH = 64;

        // リトルエンディアンの整数を bytes の先頭から読んで進める
        template <typename T>
        std::optional<T> ReadUint(std::string_view& bytes) {
            if (bytes.size() < sizeof(T))
                return std::nullopt;

            T value = 0;
            for (size_t index = 0; index < sizeof(T); ++index)
                value |= static_cast<T>(static_cast<T>(static_cast<uint8_t>(bytes[index])) << (index * 8));

            bytes.remove_prefix(sizeof(T));

            return value;
        }

        std::expected<ScriptValue, std::string> ReadValue(std::string_view& bytes, int depth);

        // 表: 数 u32 → (鍵・値)× 数。鍵は数か文字列で、正準な並び(KeyLess の狭義の昇順)
        std::expected<ScriptValue, std::string> ReadTable(std::string_view& bytes, int depth) {
            if (depth >= MAX_READ_DEPTH)
                return std::unexpected("表が深すぎる");

            const std::optional<uint32_t> count = ReadUint<uint32_t>(bytes);
            if (!count || *count > bytes.size())  // 1 つの欄は 2 バイト以上なので、残りより多い数は壊れている
                return std::unexpected("表の欄の数が途中で切れている");

            ScriptValue table;
            table.kind = ScriptValue::Kind::Table;
            table.fields.reserve(*count);
            for (uint32_t index = 0; index < *count; ++index) {
                auto key = ReadValue(bytes, depth + 1);
                if (!key)
                    return std::unexpected(key.error());

                if (!key->IsNumber() && !key->IsString())
                    return std::unexpected("表の鍵が数か文字列でない");

                if (!table.fields.empty() && !KeyLess(table.fields.back().key, *key))
                    return std::unexpected("表の鍵の並びが正準でない");

                auto value = ReadValue(bytes, depth + 1);
                if (!value)
                    return std::unexpected(value.error());

                table.fields.push_back({.key = std::move(*key), .value = std::move(*value)});
            }

            return table;
        }

        std::expected<ScriptValue, std::string> ReadValue(std::string_view& bytes, int depth) {
            if (bytes.empty())
                return std::unexpected("値が途中で切れている");

            const auto kind = static_cast<ScriptValue::Kind>(static_cast<uint8_t>(bytes.front()));
            bytes.remove_prefix(1);

            switch (kind) {
                case ScriptValue::Kind::Nil: return ScriptValue{};
                case ScriptValue::Kind::Boolean: {
                    const std::optional<uint8_t> flag = ReadUint<uint8_t>(bytes);
                    if (!flag || *flag > 1)
                        return std::unexpected("真偽の値が壊れている");

                    return ScriptValue::MakeBoolean(*flag == 1);
                }
                case ScriptValue::Kind::Number: {
                    const std::optional<uint64_t> bits = ReadUint<uint64_t>(bytes);
                    if (!bits)
                        return std::unexpected("数が途中で切れている");

                    const auto number = std::bit_cast<double>(*bits);
                    if (std::isnan(number))
                        return std::unexpected("数が NaN");

                    return ScriptValue::MakeNumber(number);
                }
                case ScriptValue::Kind::String: {
                    auto text = ReadCanonicalString(bytes);
                    if (!text)
                        return std::unexpected(text.error());

                    return ScriptValue::MakeString(std::move(*text));
                }
                case ScriptValue::Kind::Table: return ReadTable(bytes, depth);
            }

            return std::unexpected(std::format("知らない値の種類 {}", static_cast<uint32_t>(kind)));
        }

    }  // namespace

    // --- 作る ---

    ScriptValue ScriptValue::MakeBoolean(bool value) {
        ScriptValue result;
        result.kind = Kind::Boolean;
        result.boolean = value;

        return result;
    }

    ScriptValue ScriptValue::MakeNumber(double value) {
        ScriptValue result;
        result.kind = Kind::Number;
        result.number = value;

        return result;
    }

    ScriptValue ScriptValue::MakeString(std::string value) {
        ScriptValue result;
        result.kind = Kind::String;
        result.text = std::move(value);

        return result;
    }

    ScriptValue ScriptValue::MakeTable(std::vector<ScriptField> fields) {
        std::ranges::sort(
            fields, [](const ScriptField& left, const ScriptField& right) { return KeyLess(left.key, right.key); });

        ScriptValue result;
        result.kind = Kind::Table;
        result.fields = std::move(fields);

        return result;
    }

    // --- 読む ---

    const ScriptValue* ScriptValue::Find(std::string_view key) const {
        for (const ScriptField& field : fields) {
            if (field.key.kind == Kind::String && field.key.text == key)
                return &field.value;
        }

        return nullptr;
    }

    bool KeyLess(const ScriptValue& left, const ScriptValue& right) {
        if (left.kind != right.kind)
            return left.kind < right.kind;  // Number < String(Kind の並び)
        if (left.kind == ScriptValue::Kind::Number)
            return left.number < right.number;

        return left.text < right.text;  // char_traits<char> の比較はバイト(unsigned char)順
    }

    // --- バイト列 ---

    void AppendCanonicalString(std::string_view text, std::string& out) {
        AppendUint32(static_cast<uint32_t>(text.size()), out);
        out.append(text);
    }

    void AppendCanonicalBytes(const ScriptValue& value, std::string& out) {
        out.push_back(static_cast<char>(value.kind));

        switch (value.kind) {
            case ScriptValue::Kind::Nil: break;
            case ScriptValue::Kind::Boolean: out.push_back(value.boolean ? 1 : 0); break;
            case ScriptValue::Kind::Number: AppendUint64(std::bit_cast<uint64_t>(value.number), out); break;
            case ScriptValue::Kind::String: AppendCanonicalString(value.text, out); break;
            case ScriptValue::Kind::Table:
                AppendUint32(static_cast<uint32_t>(value.fields.size()), out);
                for (const ScriptField& field : value.fields) {
                    AppendCanonicalBytes(field.key, out);
                    AppendCanonicalBytes(field.value, out);
                }
                break;
        }
    }

    std::expected<std::string, std::string> ReadCanonicalString(std::string_view& bytes) {
        const std::optional<uint32_t> length = ReadUint<uint32_t>(bytes);
        if (!length || *length > bytes.size())
            return std::unexpected("文字列が途中で切れている");

        std::string text(bytes.substr(0, *length));
        bytes.remove_prefix(*length);

        return text;
    }

    std::expected<ScriptValue, std::string> ReadCanonicalBytes(std::string_view& bytes) {
        return ReadValue(bytes, 0);
    }

    uint64_t HashBytes(std::string_view bytes) {
        constexpr uint64_t FNV_OFFSET = 14695981039346656037ull;
        constexpr uint64_t FNV_PRIME = 1099511628211ull;

        uint64_t hash = FNV_OFFSET;
        for (const char byte : bytes) {
            hash ^= static_cast<uint8_t>(byte);
            hash *= FNV_PRIME;
        }

        return hash;
    }

    // --- 人が読む形 ---

    std::string ToDebugText(const ScriptValue& value) {
        switch (value.kind) {
            case ScriptValue::Kind::Nil: return "nil";
            case ScriptValue::Kind::Boolean: return value.boolean ? "true" : "false";
            case ScriptValue::Kind::Number: return std::format("{}", value.number);
            case ScriptValue::Kind::String: return std::format("\"{}\"", value.text);
            case ScriptValue::Kind::Table: break;
        }

        std::string text = "{";
        for (size_t index = 0; index < value.fields.size(); ++index) {
            if (index > 0)
                text += ", ";
            text += std::format("[{}]={}", ToDebugText(value.fields[index].key),
                                ToDebugText(value.fields[index].value));
        }

        return text + "}";
    }

}  // namespace bicameral::script
