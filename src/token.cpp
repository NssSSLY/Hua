#include "hua/token.hpp"
#include <utility>
namespace hua {
TokenKind identifier_kind(std::string_view text) {
    using K = TokenKind;
    static constexpr std::pair<std::string_view, K> keywords[] = {
        {"let",K::Let},{"var",K::Var},{"const",K::Const},{"fn",K::Fn},{"return",K::Return},
        {"struct",K::Struct},{"interface",K::Interface},{"pub",K::Pub},{"if",K::If},
        {"else",K::Else},{"match",K::Match},{"enum",K::Enum},{"for",K::For},{"while",K::While},
        {"break",K::Break},{"continue",K::Continue},{"in",K::In},{"import",K::Import},
        {"as",K::As},{"async",K::Async},{"await",K::Await},{"spawn",K::Spawn},
        {"taskgroup",K::Taskgroup},{"parallel",K::Parallel},{"simd",K::Simd},
        {"defer",K::Defer},{"unsafe",K::Unsafe},{"extern",K::Extern},
        {"true",K::True},{"false",K::False},{"nil",K::Nil}
    };
    for (auto [word, kind] : keywords) if (word == text) return kind;
    return K::Identifier;
}
std::string_view token_name(TokenKind kind) {
    using K = TokenKind;
    switch (kind) {
    case TokenKind::Arrow:return "=>";
    case K::End: return "end of file";
    case K::Newline: return "newline";
    case K::Identifier: return "identifier";
    case K::Integer: return "integer";
    case K::Float: return "float";
    case K::String: return "string";
    default: return "token";
    }
}
}
