// script_value.h — Luau の値を、アドレスに依らない C++ の木にしたもの(T-0138、13 §2・ADR-0031)。
//
// パッケージ(luau_package.*)が Luau から受け取る表(マニフェスト・反応則などの定義)は、Luau の外ではこの形で持つ。
//   形: nil・真偽・数(倍精度)・文字列・表(鍵 → 値。鍵は数か文字列で、並べた順に持つ)。関数・userdata・スレッドは持たない。
//   鍵の順番: 数(小さい順)→ 文字列(バイト順)。Luau の pairs の順番(アドレス・挿入の履歴で変わる)に依らない。
// データの流れ: LuauSandbox::Run(captureValues)が Luau の戻り値をこれに直す → パッケージの読み込みが合わせる → ベイク(T-0021)へ。
// AppendCanonicalBytes は同じ木から必ず同じバイト列を作る(決定性の検査と、表の版のハッシュに使う)。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bicameral::script {

    struct ScriptField;

    struct ScriptValue {
        enum class Kind : uint8_t {
            Nil,
            Boolean,
            Number,
            String,
            Table,
        };

        Kind kind = Kind::Nil;
        bool boolean = false;
        double number = 0.0;
        std::string text;
        std::vector<ScriptField> fields;  // 表の中身。鍵は数(小さい順)→ 文字列(バイト順)に並んでいる

        // --- 作る ---
        [[nodiscard]] static ScriptValue MakeBoolean(bool value);
        [[nodiscard]] static ScriptValue MakeNumber(double value);
        [[nodiscard]] static ScriptValue MakeString(std::string value);
        [[nodiscard]] static ScriptValue MakeTable(std::vector<ScriptField> fields);  // 鍵を並べ直す

        // --- 読む ---
        [[nodiscard]] bool IsTable() const { return kind == Kind::Table; }
        [[nodiscard]] bool IsString() const { return kind == Kind::String; }
        [[nodiscard]] bool IsNumber() const { return kind == Kind::Number; }

        // 文字列の鍵 key の値(無ければ nullptr)
        [[nodiscard]] const ScriptValue* Find(std::string_view key) const;
    };

    struct ScriptField {
        ScriptValue key;
        ScriptValue value;
    };

    // 鍵の順番(数 → 文字列。数は小さい順、文字列はバイト順)
    [[nodiscard]] bool KeyLess(const ScriptValue& left, const ScriptValue& right);

    // 同じ木なら同じバイト列(種類の印 + 中身。数はビット列のまま、長さは 4 バイトのリトルエンディアン)
    void AppendCanonicalBytes(const ScriptValue& value, std::string& out);
    void AppendCanonicalString(std::string_view text, std::string& out);

    // バイト列の FNV-1a(64 ビット)。表の版に使う
    [[nodiscard]] uint64_t HashBytes(std::string_view bytes);

    // 人が読む形(テスト・ログ用。鍵の順番は fields のまま)
    [[nodiscard]] std::string ToDebugText(const ScriptValue& value);

}  // namespace bicameral::script
