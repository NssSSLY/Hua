#pragma once
#include "hua/token.hpp"
namespace hua {
class Lexer {
public:
    explicit Lexer(const Source& source) : source_(source) {}
    std::vector<Token> scan();
private:
    const Source& source_;
    std::size_t pos_{};
    std::vector<Token> tokens_;
    char peek(std::size_t ahead = 0) const;
    void emit(TokenKind kind, std::size_t start, std::string text);
    void number();
    void string();
    void block_comment();
    void validate_utf8() const;
    [[noreturn]] void fail(std::size_t start, std::string message, std::string code = "E1001", std::string help = {}) const;
};
}
