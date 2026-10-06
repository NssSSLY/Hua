#pragma once
#include "hua/ast.hpp"
#include "hua/token.hpp"
namespace hua {
class Parser {
public:
    explicit Parser(std::vector<Token> tokens);
    NodePtr parse();
private:
    std::vector<Token> tokens_;
    std::size_t pos_{}, depth_{};
    bool in_header_{};
    int function_depth_{}, loop_depth_{}, unsafe_depth_{};
    const Token& peek(std::size_t ahead = 0) const;
    bool at(TokenKind kind) const;
    Token take();
    bool accept(TokenKind kind);
    Token expect(TokenKind kind, std::string_view reason);
    void newlines();
    void statement_end();
    NodePtr make(NodeKind kind, const Token& token, std::string text = {});
    void finish(Node& node);
    [[noreturn]] void fail(const Token& token, std::string message, std::string code = "E2001", std::string help = {}) const;
    NodePtr statement();
    NodePtr variable();
    NodePtr function(bool is_public, bool is_unsafe, bool prototype=false);
    NodePtr structure(bool is_public);
    NodePtr generic_parameters();
    NodePtr interface_declaration(bool is_public);
    NodePtr enum_declaration(bool is_public);
    NodePtr match_statement();
    bool specialization_ahead() const;
    NodePtr block();
    NodePtr conditional();
    NodePtr while_loop();
    NodePtr for_loop();
    NodePtr type();
    NodePtr expression(int minimum = 1, bool multiline = false);
    NodePtr primary(bool multiline);
    NodePtr postfix(NodePtr left, bool multiline);
    bool struct_literal_ahead() const;
};
}
