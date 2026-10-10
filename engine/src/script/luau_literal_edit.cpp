// luau_literal_edit.cpp — Luau のソースの字句を読み、表の鍵の並びでリテラルを探して差し替える(luau_literal_edit.h。T-0219)。
// 字句(名前・数・文字列・記号)だけを読み、コメント・長い括弧の文字列([[ ]]・[==[ ]==])は飛ばす。構文の検査はしない
// (正しい Luau かどうかは、書いた後にホットリロードの型検査と殻が確かめる。ADR-0046・ADR-0047)。
#include "script/luau_literal_edit.h"

#include <format>
#include <optional>
#include <vector>

namespace bicameral::script {

    namespace {

        enum class TokenKind : uint8_t { Name, Number, String, Symbol };

        struct Token {
            TokenKind kind = TokenKind::Symbol;
            size_t offset = 0;
            size_t length = 0;
            uint32_t line = 1;
        };

        bool IsNameStart(char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        }

        bool IsDigit(char c) {
            return c >= '0' && c <= '9';
        }

        bool IsNameChar(char c) {
            return IsNameStart(c) || IsDigit(c);
        }

        // --- 字句 ---

        class Lexer {
        public:
            explicit Lexer(std::string_view source) : m_source(source) {}

            // 全部の字句(コメントと空白を除く)。閉じていない文字列・コメントは理由
            std::expected<std::vector<Token>, std::string> Run() {
                std::vector<Token> tokens;
                while (true) {
                    if (const auto skipped = SkipSpaceAndComments(); !skipped)
                        return std::unexpected(skipped.error());

                    if (m_position >= m_source.size())
                        return tokens;

                    auto token = Read();
                    if (!token)
                        return std::unexpected(token.error());

                    tokens.push_back(*token);
                }
            }

        private:
            [[nodiscard]] char At(size_t position) const {
                return position < m_source.size() ? m_source[position] : '\0';
            }

            void Advance(size_t count) {
                for (size_t i = 0; i < count && m_position < m_source.size(); ++i) {
                    if (m_source[m_position] == '\n')
                        ++m_line;

                    ++m_position;
                }
            }

            // m_position の '[' から始まる長い括弧の段('=' の数)。長い括弧でなければ無し
            [[nodiscard]] std::optional<size_t> LongBracketLevel() const {
                if (At(m_position) != '[')
                    return std::nullopt;

                size_t level = 0;
                while (At(m_position + 1 + level) == '=')
                    ++level;

                if (At(m_position + 1 + level) != '[')
                    return std::nullopt;

                return level;
            }

            // 長い括弧を閉じるまで進める
            std::expected<void, std::string> SkipLongBracket(size_t level) {
                const uint32_t startLine = m_line;
                const std::string closing = "]" + std::string(level, '=') + "]";
                const size_t end = m_source.find(closing, m_position + level + 2);
                if (end == std::string_view::npos)
                    return std::unexpected(std::format("{} 行目の長い括弧が閉じていない", startLine));

                Advance(end + closing.size() - m_position);

                return {};
            }

            std::expected<void, std::string> SkipSpaceAndComments() {
                while (m_position < m_source.size()) {
                    const char c = m_source[m_position];
                    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                        Advance(1);
                        continue;
                    }

                    if (c != '-' || At(m_position + 1) != '-')
                        return {};

                    // --- コメント(-- から行末、または --[[ ]])---
                    Advance(2);
                    if (const auto level = LongBracketLevel()) {
                        if (const auto skipped = SkipLongBracket(*level); !skipped)
                            return skipped;

                        continue;
                    }

                    while (m_position < m_source.size() && m_source[m_position] != '\n')
                        Advance(1);
                }

                return {};
            }

            std::expected<Token, std::string> Read() {
                Token token{.offset = m_position, .line = m_line};
                const char c = m_source[m_position];

                if (IsNameStart(c)) {
                    token.kind = TokenKind::Name;
                    while (IsNameChar(At(m_position)))
                        Advance(1);
                } else if (IsDigit(c) || (c == '.' && IsDigit(At(m_position + 1)))) {
                    token.kind = TokenKind::Number;
                    ReadNumber();
                } else if (c == '"' || c == '\'' || c == '`') {
                    token.kind = TokenKind::String;
                    if (const auto read = ReadShortString(c); !read)
                        return std::unexpected(read.error());
                } else if (const auto level = LongBracketLevel()) {
                    token.kind = TokenKind::String;
                    if (const auto skipped = SkipLongBracket(*level); !skipped)
                        return std::unexpected(skipped.error());
                } else {
                    token.kind = TokenKind::Symbol;
                    Advance(SymbolLength());
                }

                token.length = m_position - token.offset;

                return token;
            }

            // 0x1F・1e5・1_000・2.5e-3
            void ReadNumber() {
                const bool hex = At(m_position) == '0' && (At(m_position + 1) == 'x' || At(m_position + 1) == 'X');
                while (true) {
                    const char c = At(m_position);
                    const char previous = m_position > 0 ? m_source[m_position - 1] : '\0';
                    const bool sign = (c == '+' || c == '-') && !hex && (previous == 'e' || previous == 'E');
                    if (!IsNameChar(c) && c != '.' && !sign)
                        return;

                    Advance(1);
                }
            }

            std::expected<void, std::string> ReadShortString(char quote) {
                const uint32_t startLine = m_line;
                Advance(1);
                while (m_position < m_source.size()) {
                    const char c = m_source[m_position];
                    if (c == '\\') {
                        Advance(2);
                        continue;
                    }

                    Advance(1);
                    if (c == quote)
                        return {};
                }

                return std::unexpected(std::format("{} 行目の文字列が閉じていない", startLine));
            }

            // 記号の長さ(== ~= <= >= += など・.. ... ..=・// //=・:: ->)
            [[nodiscard]] size_t SymbolLength() const {
                const char c = At(m_position);
                const char next = At(m_position + 1);
                if (c == '.' && next == '.') {
                    if (At(m_position + 2) == '.' || At(m_position + 2) == '=')
                        return 3;

                    return 2;
                }

                if (c == '/' && next == '/')
                    return At(m_position + 2) == '=' ? 3 : 2;

                if (next == '=' && std::string_view("=~<>+-*/%^").contains(c))
                    return 2;

                if ((c == ':' && next == ':') || (c == '-' && next == '>'))
                    return 2;

                return 1;
            }

            std::string_view m_source;
            size_t m_position = 0;
            uint32_t m_line = 1;
        };

        // --- 表の入れ子をたどる ---

        enum class FrameKind : uint8_t { Table, Paren, Bracket, Block };

        struct Frame {
            FrameKind kind = FrameKind::Table;
            std::string key;  // 表のとき、それを値に持つ鍵(位置で並べた表・return { など鍵が無ければ空)
        };

        struct Match {
            LuauLiteralSpan span;
            bool literal = false;  // false なら値が式
        };

        class Walker {
        public:
            Walker(std::string_view source, const std::vector<Token>& tokens, std::span<const std::string> keyPath)
                : m_source(source), m_tokens(tokens), m_keyPath(keyPath) {}

            std::expected<std::vector<Match>, std::string> Run() {
                bool fieldStart = false;
                for (size_t i = 0; i < m_tokens.size();) {
                    if (fieldStart && TopIs(FrameKind::Table)) {
                        if (const auto next = ReadField(i)) {
                            // 値が表なら、入れ子の表の欄の始めから
                            i = *next;
                            fieldStart = m_enteredTable;
                            m_enteredTable = false;
                            continue;
                        }
                    }

                    const auto step = Step(i, fieldStart);
                    if (!step)
                        return std::unexpected(step.error());

                    ++i;
                }

                return m_matches;
            }

        private:
            [[nodiscard]] std::string_view TextOf(size_t index) const {
                const Token& token = m_tokens[index];

                return m_source.substr(token.offset, token.length);
            }

            [[nodiscard]] bool Is(size_t index, std::string_view symbol) const {
                return index < m_tokens.size() && m_tokens[index].kind == TokenKind::Symbol && TextOf(index) == symbol;
            }

            [[nodiscard]] bool TopIs(FrameKind kind) const { return !m_frames.empty() && m_frames.back().kind == kind; }

            // 鍵の無い字句を 1 つ進める(括弧と block の入れ子だけ数える)
            std::expected<void, std::string> Step(size_t i, bool& fieldStart) {
                const Token& token = m_tokens[i];
                const std::string_view text = TextOf(i);
                fieldStart = false;

                if (token.kind == TokenKind::Name) {
                    if (text == "function" || text == "do" || text == "if" || text == "repeat")
                        m_frames.push_back({.kind = FrameKind::Block});
                    else if (text == "end" || text == "until")
                        return Pop(FrameKind::Block, token);

                    return {};
                }

                if (token.kind != TokenKind::Symbol)
                    return {};

                if (text == "{") {
                    m_frames.push_back({.kind = FrameKind::Table});
                    fieldStart = true;
                } else if (text == "(")
                    m_frames.push_back({.kind = FrameKind::Paren});
                else if (text == "[")
                    m_frames.push_back({.kind = FrameKind::Bracket});
                else if (text == "}")
                    return Pop(FrameKind::Table, token);
                else if (text == ")")
                    return Pop(FrameKind::Paren, token);
                else if (text == "]")
                    return Pop(FrameKind::Bracket, token);
                else if (text == "," || text == ";")
                    fieldStart = TopIs(FrameKind::Table);

                return {};
            }

            std::expected<void, std::string> Pop(FrameKind kind, const Token& token) {
                if (!TopIs(kind))
                    return std::unexpected(std::format("{} 行目の括弧か end が合わない", token.line));

                m_frames.pop_back();

                return {};
            }

            // 表の欄の始め i が「鍵 = 値」なら読む。値が表なら入れ子に入り、次に見る字句の位置を返す。鍵が無ければ無し
            std::optional<size_t> ReadField(size_t i) {
                std::string key;
                size_t value = 0;
                if (m_tokens[i].kind == TokenKind::Name && Is(i + 1, "=")) {
                    key = std::string(TextOf(i));
                    value = i + 2;
                } else if (Is(i, "[") && i + 3 < m_tokens.size() && m_tokens[i + 1].kind == TokenKind::String &&
                           Is(i + 2, "]") && Is(i + 3, "=")) {
                    const auto unquoted = LuauLiteralValue(TextOf(i + 1));
                    if (!unquoted)
                        return std::nullopt;

                    key = *unquoted;
                    value = i + 4;
                } else
                    return std::nullopt;

                if (value >= m_tokens.size())
                    return value;

                // --- 値が表 → 入れ子へ ---
                if (Is(value, "{")) {
                    m_frames.push_back({.kind = FrameKind::Table, .key = std::move(key)});
                    m_enteredTable = true;

                    return value + 1;
                }

                // --- 値がリテラル(続きが , ; } なら)か式 ---
                const size_t literalEnd = LiteralEnd(value);
                const bool literal = literalEnd != 0 &&
                                     (Is(literalEnd, ",") || Is(literalEnd, ";") || Is(literalEnd, "}"));
                if (PathMatches(key) && !InsideBlock()) {
                    const Token& first = m_tokens[value];
                    const Token& last = m_tokens[literal ? literalEnd - 1 : value];
                    m_matches.push_back({.span = {.offset = first.offset,
                                                  .length = last.offset + last.length - first.offset,
                                                  .line = first.line,
                                                  .isString = literal && last.kind == TokenKind::String},
                                         .literal = literal});
                }

                return value;
            }

            // value から始まるリテラル(数・-数・文字列)の次の字句の位置。リテラルでなければ 0
            [[nodiscard]] size_t LiteralEnd(size_t value) const {
                const TokenKind kind = m_tokens[value].kind;
                if (kind == TokenKind::Number || kind == TokenKind::String)
                    return value + 1;

                if (Is(value, "-") && value + 1 < m_tokens.size() && m_tokens[value + 1].kind == TokenKind::Number)
                    return value + 2;

                return 0;
            }

            [[nodiscard]] bool InsideBlock() const {
                for (const Frame& frame : m_frames) {
                    if (frame.kind == FrameKind::Block)
                        return true;
                }

                return false;
            }

            // 外から内への表の鍵の並び + key が keyPath で終わるか
            [[nodiscard]] bool PathMatches(std::string_view key) const {
                if (m_keyPath.empty() || m_keyPath.back() != key)
                    return false;

                size_t remaining = m_keyPath.size() - 1;
                for (auto frame = m_frames.rbegin(); frame != m_frames.rend() && remaining > 0; ++frame) {
                    if (frame->kind != FrameKind::Table)
                        continue;

                    if (frame->key != m_keyPath[remaining - 1])
                        return false;

                    --remaining;
                }

                return remaining == 0;
            }

            std::string_view m_source;
            const std::vector<Token>& m_tokens;
            std::span<const std::string> m_keyPath;
            std::vector<Frame> m_frames;
            std::vector<Match> m_matches;
            bool m_enteredTable = false;  // ReadField が入れ子の表に入った(次の字句は欄の始め)
        };

        std::string JoinPath(std::span<const std::string> keyPath) {
            std::string text;
            for (const std::string& key : keyPath) {
                if (!text.empty())
                    text += '.';

                text += key;
            }

            return text;
        }

    }  // namespace

    std::expected<LuauLiteralMatches, std::string> FindLuauLiterals(std::string_view source,
                                                                    std::span<const std::string> keyPath) {
        const auto tokens = Lexer(source).Run();
        if (!tokens)
            return std::unexpected(tokens.error());

        const auto matches = Walker(source, *tokens, keyPath).Run();
        if (!matches)
            return std::unexpected(matches.error());

        LuauLiteralMatches found;
        for (const Match& match : *matches) {
            if (match.literal)
                found.literals.push_back(match.span);
            else
                found.expressions.push_back(match.span);
        }

        return found;
    }

    std::expected<LuauLiteralSpan, std::string> FindLuauLiteral(std::string_view source,
                                                                std::span<const std::string> keyPath) {
        const auto found = FindLuauLiterals(source, keyPath);
        if (!found)
            return std::unexpected(found.error());

        const std::string path = JoinPath(keyPath);
        if (found->Count() == 0)
            return std::unexpected(std::format("{} が見つからない", path));

        if (found->Count() > 1) {
            return std::unexpected(
                std::format("{} が {} か所にある(どれを書き換えるか決まらない)", path, found->Count()));
        }

        if (!found->expressions.empty()) {
            const LuauLiteralSpan& expression = found->expressions.front();

            return std::unexpected(std::format("{} 行目の {} は式なので書き戻せない", expression.line, path));
        }

        return found->literals.front();
    }

    std::string ReplaceLuauLiteral(std::string_view source, const LuauLiteralSpan& span, std::string_view literal) {
        std::string text(source.substr(0, span.offset));
        text += literal;
        text += source.substr(span.offset + span.length);

        return text;
    }

    std::string FormatLuauInteger(int64_t value) {
        return std::format("{}", value);
    }

    std::string FormatLuauString(std::string_view text) {
        std::string literal = "\"";
        for (const char c : text) {
            if (c == '\\' || c == '"')
                literal += '\\';

            if (c == '\n')
                literal += "\\n";
            else
                literal += c;
        }

        literal += '"';

        return literal;
    }

    std::expected<std::string, std::string> LuauLiteralValue(std::string_view literal) {
        if (literal.empty())
            return std::unexpected("空のリテラル");

        const char quote = literal.front();
        if (quote != '"' && quote != '\'') {
            std::string number;
            for (const char c : literal) {
                if (c != '_')
                    number += c;
            }

            return number;
        }

        if (literal.size() < 2 || literal.back() != quote)
            return std::unexpected(std::format("文字列が閉じていない: {}", literal));

        std::string text;
        for (size_t i = 1; i + 1 < literal.size(); ++i) {
            char c = literal[i];
            if (c == '\\' && i + 2 < literal.size()) {
                c = literal[++i];
                if (c == 'n')
                    c = '\n';
                else if (c == 't')
                    c = '\t';
                else if (c != '\\' && c != '"' && c != '\'')
                    return std::unexpected(std::format("読めない逃がし \\{}: {}", c, literal));
            }

            text += c;
        }

        return text;
    }

}  // namespace bicameral::script
