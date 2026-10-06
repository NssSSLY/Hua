#pragma once
#include "hua/source.hpp"
#include <string>
#include <vector>
namespace hua {
enum class TokenKind {
    End, Newline, Identifier, Integer, Float, String,
    Let, Var, Const, Fn, Return, Struct, Interface, Pub, If, Else, Match, Enum,
    For, While, Break, Continue, In, Import, As, Async, Await, Spawn, Taskgroup,
    Parallel, Simd, Defer, Unsafe, Extern, True, False, Nil,
    LParen, RParen, LBracket, RBracket, LBrace, RBrace, Comma, Colon, Dot,
    Range, RangeInclusive, Question, At,
    Plus, Minus, Star, Slash, FloorDivide, Percent, Power,
    Assign, PlusAssign, MinusAssign, StarAssign, SlashAssign, PercentAssign,
    PowerAssign, FloorAssign, Increment, Decrement,
    Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual,
    And, Or, Not, BitAnd, BitOr, BitXor, BitNot, ShiftLeft, ShiftRight, Arrow
};
struct Token { TokenKind kind; std::string text; SourceSpan span; };
std::string_view token_name(TokenKind kind);
TokenKind identifier_kind(std::string_view text);
}
