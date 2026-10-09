// script_value.cpp — ScriptValue(Luau の値の C++ の木)の並べ方・バイト列・ハッシュ(T-0138、ADR-0031)。
#include "script/script_value.h"

#include <algorithm>
#include <bit>
#include <format>
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
