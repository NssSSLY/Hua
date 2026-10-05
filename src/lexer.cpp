#include "hua/lexer.hpp"
#include <cstdint>
#include <utility>
namespace hua {
namespace {
bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool decimal(char c) { return c >= '0' && c <= '9'; }
int digit(char c) {
    if (decimal(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}
char Lexer::peek(std::size_t ahead) const {
    return pos_ + ahead < source_.text().size() ? source_.text()[pos_ + ahead] : '\0';
}
[[noreturn]] void Lexer::fail(std::size_t start, std::string message, std::string code, std::string help) const {
    throw Diagnostic(std::move(code), source_.span(start, start + 1), std::move(message), std::move(help));
}
void Lexer::emit(TokenKind kind, std::size_t start, std::string text) {
    tokens_.push_back({kind, std::move(text), source_.span(start, pos_)});
}
void Lexer::validate_utf8() const {
    const auto& text = source_.text();
    for (std::size_t i = 0; i < text.size();) {
        const auto start = i;
        auto byte = static_cast<unsigned char>(text[i++]);
        if (byte < 0x80) continue;
        unsigned count;
        std::uint32_t value, minimum;
        if (byte >= 0xC2 && byte <= 0xDF) { count = 1; value = byte & 0x1F; minimum = 0x80; }
        else if (byte >= 0xE0 && byte <= 0xEF) { count = 2; value = byte & 0x0F; minimum = 0x800; }
        else if (byte >= 0xF0 && byte <= 0xF4) { count = 3; value = byte & 0x07; minimum = 0x10000; }
        else fail(start, "source is not valid UTF-8", "E1009");
        for (unsigned j = 0; j < count; ++j) {
            if (i == text.size()) fail(start, "incomplete UTF-8 sequence", "E1009");
            auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xC0) != 0x80) fail(start, "invalid UTF-8 continuation byte", "E1009");
            value = (value << 6) | (next & 0x3F);
        }
        if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
            fail(start, "invalid UTF-8 code point", "E1009");
    }
}
void Lexer::number() {
    const auto start = pos_;
    auto digits = [&](int base) {
        bool seen = false, underscore = false;
        while (pos_ < source_.text().size()) {
            if (peek() == '_') {
                if (!seen || underscore) fail(pos_, "underscore must separate numeric digits", "E1002");
                underscore = true; ++pos_;
            } else if (digit(peek()) >= 0 && digit(peek()) < base) {
                seen = true; underscore = false; ++pos_;
            } else break;
        }
        if (!seen || underscore) fail(pos_, "expected a digit in numeric literal", "E1002");
    };
    TokenKind kind = TokenKind::Integer;
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'b' || peek(1) == 'o')) {
        const char prefix = peek(1);
        pos_ += 2;
        digits(prefix == 'x' ? 16 : prefix == 'b' ? 2 : 8);
    } else {
        digits(10);
        if (peek() == '.' && decimal(peek(1))) {
            kind = TokenKind::Float; ++pos_; digits(10);
        }
        if (peek() == 'e' || peek() == 'E') {
            kind = TokenKind::Float; ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            digits(10);
        }
    }
    if (alpha(peek()) || decimal(peek())) fail(pos_, "invalid digit or suffix in numeric literal", "E1002");
    emit(kind, start, source_.text().substr(start, pos_ - start));
}
void Lexer::string() {
    const auto start = pos_++;
    std::string value;
    while (pos_ < source_.text().size() && peek() != '"') {
        char c = peek(); ++pos_;
        if (c == '\n') fail(start, "unterminated string literal", "E1003");
        if (c == '\\') {
            if (pos_ == source_.text().size()) fail(start, "unterminated string escape", "E1003");
            c = peek(); ++pos_;
            switch (c) {
            case '\\': value += '\\'; break;
            case '"': value += '"'; break;
            case 'n': value += '\n'; break;
            case 'r': value += '\r'; break;
            case 't': value += '\t'; break;
            case '0': value += '\0'; break;
            default: fail(pos_ - 1, "unsupported string escape", "E1004", "supported escapes: backslash, double quote, n, r, t and 0");
            }
        } else {
            if (static_cast<unsigned char>(c) < 0x20)
                fail(pos_ - 1, "unescaped control character in string", "E1003");
            value += c;
        }
    }
    if (pos_ == source_.text().size()) fail(start, "unterminated string literal", "E1003");
    ++pos_; emit(TokenKind::String, start, std::move(value));
}
void Lexer::block_comment() {
    const auto start = pos_;
    std::size_t depth = 1;
    std::vector<bool> documentation = {peek(2) == '*'};
    pos_ += documentation.back() ? 3 : 2;
    while (pos_ < source_.text().size()) {
        if (peek() == '#' && peek(1) == '*') {
            documentation.push_back(peek(2) == '*');
            ++depth;
            pos_ += documentation.back() ? 3 : 2;
        } else if (peek() == '*' &&
                   (documentation.back() ? peek(1) == '*' && peek(2) == '#' : peek(1) == '#')) {
            pos_ += documentation.back() ? 3 : 2;
            documentation.pop_back();
            if (--depth == 0) return;
        } else if (peek() == '\n') {
            const auto newline = pos_++;
            emit(TokenKind::Newline, newline, "\n");
        } else ++pos_;
    }
    fail(start, "unterminated block comment", "E1005");
}
std::vector<Token> Lexer::scan() {
    pos_ = 0; tokens_.clear();
    validate_utf8();
    using K = TokenKind;
    static constexpr std::pair<std::string_view, K> operators[] = {
        {"..=",K::RangeInclusive},{"**=",K::PowerAssign},{"//=",K::FloorAssign},
        {"..",K::Range},{"**",K::Power},{"//",K::FloorDivide},{"++",K::Increment},{"--",K::Decrement},
        {"+=",K::PlusAssign},{"-=",K::MinusAssign},{"*=",K::StarAssign},
        {"/=",K::SlashAssign},{"%=",K::PercentAssign},
        {"==",K::Equal},{"!=",K::NotEqual},{"<=",K::LessEqual},{">=",K::GreaterEqual},
        {"&&",K::And},{"||",K::Or},{"<<",K::ShiftLeft},{">>",K::ShiftRight},
        {"(",K::LParen},{")",K::RParen},{"[",K::LBracket},{"]",K::RBracket},
        {"{",K::LBrace},{"}",K::RBrace},{",",K::Comma},{":",K::Colon},{".",K::Dot},
        {"?",K::Question},{"@",K::At},{"+",K::Plus},{"-",K::Minus},{"*",K::Star},
        {"/",K::Slash},{"%",K::Percent},{"=",K::Assign},{"<",K::Less},{">",K::Greater},
        {"!",K::Not},{"&",K::BitAnd},{"|",K::BitOr},{"^",K::BitXor},{"~",K::BitNot}
    };
    while (pos_ < source_.text().size()) {
        const auto start = pos_;
        const char c = peek();
        if (c == ' ' || c == '\t') { ++pos_; continue; }
        if (c == '\n') { ++pos_; emit(K::Newline, start, "\n"); continue; }
        if (c == '#') {
            if (peek(1) == '*') { block_comment(); continue; }
            // Includes reserved ## documentation and the first-line #! shebang.
            while (pos_ < source_.text().size() && peek() != '\n') ++pos_;
            continue;
        }
        if (alpha(c)) {
            ++pos_;
            while (alpha(peek()) || decimal(peek())) ++pos_;
            auto text = source_.text().substr(start, pos_ - start);
            const auto kind = identifier_kind(text);
            emit(kind, start, std::move(text)); continue;
        }
        if (decimal(c)) { number(); continue; }
        if (c == '"') { string(); continue; }
        bool found = false;
        for (auto [op, kind] : operators) {
            if (std::string_view(source_.text()).substr(pos_).starts_with(op)) {
                pos_ += op.size(); emit(kind, start, std::string(op)); found = true; break;
            }
        }
        if (!found) fail(start, c == ';' ? "semicolons are not part of Phase 1 grammar" : "unexpected source character", "E1001");
    }
    emit(K::End, pos_, "");
    return std::move(tokens_);
}
}
