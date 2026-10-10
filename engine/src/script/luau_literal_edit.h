// luau_literal_edit.h — Luau のソースの中の 1 つの値(数・文字列のリテラル)を、表の鍵の並びで探して差し替える(T-0219・14 §2)。
//
// エディタの反応表のパネルが「値をその場で変えて Luau に書き戻す」ために使う。データの流れ:
//   パッケージの .luau のソース → FindLuauLiteral(字句だけ読む。Luau は走らせない)→ 値の位置(バイトの範囲)
//   → ReplaceLuauLiteral(その範囲だけ差し替える)→ ファイルへ書く → ホットリロード(ADR-0047)が読み直す。
// 差し替えるのはリテラルの字面だけで、コメント・空白・並び・ほかの値は 1 バイトも変えない。
// 探し方: 表の構築子 { } の入れ子をたどり、「鍵 = リテラル」の鍵の並び(外から内へ)が keyPath で終わるものを探す。
//   例 keyPath = { "cellulose_combustion", "rate", "a" } は reactions.luau の `cellulose_combustion = { rate = { a = "2e10" } }` に当たる。
//   値が式(`solid(120)`・`1 + 2`・変数)のときは書き戻せない(理由を返す)。関数の本体(function … end など)の中は見ない。
// ここは CPU だけ(エディタとツール。原則 2)。
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bicameral::script {

    // ソースの中の 1 つのリテラルの位置
    struct LuauLiteralSpan {
        size_t offset = 0;  // バイトの位置(負の数は '-' から)
        size_t length = 0;
        uint32_t line = 0;  // 1 から(誤りの表示用)
        bool isString = false;

        [[nodiscard]] std::string_view Text(std::string_view source) const { return source.substr(offset, length); }
    };

    // keyPath で終わる「鍵 = 値」の全部(値がリテラルのものと式のもの)。字句が読めなければ理由
    struct LuauLiteralMatches {
        std::vector<LuauLiteralSpan> literals;
        std::vector<LuauLiteralSpan> expressions;  // 値が式(span は式の最初の字句)

        [[nodiscard]] size_t Count() const { return literals.size() + expressions.size(); }
    };

    [[nodiscard]] std::expected<LuauLiteralMatches, std::string> FindLuauLiterals(std::string_view source,
                                                                                  std::span<const std::string> keyPath);

    // FindLuauLiterals で 1 つだけ、値がリテラルのものを探す。無い・2 つ以上ある・値が式なら理由
    [[nodiscard]] std::expected<LuauLiteralSpan, std::string> FindLuauLiteral(std::string_view source,
                                                                              std::span<const std::string> keyPath);

    // span の字面を literal に替えたソース(literal は FormatLuauInteger・FormatLuauString で作る)
    [[nodiscard]] std::string ReplaceLuauLiteral(std::string_view source, const LuauLiteralSpan& span,
                                                 std::string_view literal);

    // 整数のリテラル(10 進)と、文字列のリテラル("…"。\ と " と制御文字は逃がす)
    [[nodiscard]] std::string FormatLuauInteger(int64_t value);
    [[nodiscard]] std::string FormatLuauString(std::string_view text);

    // リテラルの字面から中身を読む(文字列は引用符と逃がしを外す。数はそのまま)。読めなければ理由
    [[nodiscard]] std::expected<std::string, std::string> LuauLiteralValue(std::string_view literal);

}  // namespace bicameral::script
