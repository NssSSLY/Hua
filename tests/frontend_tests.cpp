#include "hua/ast.hpp"
#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& label) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}
std::string read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read test file: " + path.string());
    return std::string(std::istreambuf_iterator<char>(in), {});
}
hua::NodePtr parse(const std::string& text) {
    hua::Source source("unit.hua", text);
    hua::Lexer lexer(source);
    hua::Parser parser(lexer.scan());
    return parser.parse();
}
std::string shape(const hua::Node& node) {
    std::string value(hua::node_name(node.kind));
    if (!node.text.empty()) value += ":" + node.text;
    if (!node.children.empty()) {
        value += '(';
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            if (i) value += ',';
            value += shape(*node.children[i]);
        }
        value += ')';
    }
    return value;
}
void span_tree(const hua::Node& node, const hua::Source& source) {
    const auto& span = node.span;
    const auto expected = source.span(span.start_offset, span.end_offset);
    check(span.file_id == source.filename() && span.start_offset <= span.end_offset &&
          span.end_offset <= source.text().size() && span.line == expected.line &&
          span.column == expected.column, "valid span for " + std::string(hua::node_name(node.kind)));
    for (const auto& child : node.children) {
        check(child != nullptr, "AST children are non-null");
        if (!child) continue;
        check(child->span.start_offset >= span.start_offset && child->span.end_offset <= span.end_offset,
              "child span contained by " + std::string(hua::node_name(node.kind)));
        span_tree(*child, source);
    }
}
void expression_tests() {
    const std::pair<std::string, std::string> cases[] = {
        {"10 // 3 * 2", "Binary:*(Binary://(Integer:10,Integer:3),Integer:2)"},
        {"n //= 2", "Assignment://=(Name:n,Integer:2)"},
        {"1 + 2 * 3", "Binary:+(Integer:1,Binary:*(Integer:2,Integer:3))"},
        {"-2**2", "Unary:-(Binary:**(Integer:2,Integer:2))"},
        {"2**3**2", "Binary:**(Integer:2,Binary:**(Integer:3,Integer:2))"},
        {"2**-3", "Binary:**(Integer:2,Unary:-(Integer:3))"},
        {"(-2)**2", "Binary:**(Unary:-(Integer:2),Integer:2)"},
        {"1 - 2 - 3", "Binary:-(Binary:-(Integer:1,Integer:2),Integer:3)"},
        {"a = b = c", "Assignment:=(Name:a,Assignment:=(Name:b,Name:c))"},
        {"a || b && c", "Binary:||(Name:a,Binary:&&(Name:b,Name:c))"},
        {"a == b < c", "Binary:==(Name:a,Binary:<(Name:b,Name:c))"},
        {"a < b | c", "Binary:<(Name:a,Binary:|(Name:b,Name:c))"},
        {"a | b ^ c", "Binary:|(Name:a,Binary:^(Name:b,Name:c))"},
        {"a ^ b & c", "Binary:^(Name:a,Binary:&(Name:b,Name:c))"},
        {"a & b << c", "Binary:&(Name:a,Binary:<<(Name:b,Name:c))"},
        {"a << b + c", "Binary:<<(Name:a,Binary:+(Name:b,Name:c))"},
        {"~a**2", "Unary:~(Binary:**(Name:a,Integer:2))"},
        {"!a && b", "Binary:&&(Unary:!(Name:a),Name:b)"},
        {"f(1).x[2]", "Index(Member:x(Call(Name:f,Integer:1)),Integer:2)"},
        {"xs[:2]", "Slice(Name:xs,Omitted,Integer:2)"},
        {"xs[1:]", "Slice(Name:xs,Integer:1,Omitted)"},
        {"xs[:]", "Slice(Name:xs,Omitted,Omitted)"},
        {"a[0] += 1", "Assignment:+=(Index(Name:a,Integer:0),Integer:1)"},
        {"p.x++", "Update:++(Member:x(Name:p))"}
    };
    for (const auto& [input, expected] : cases) {
        try {
            auto tree = parse(input);
            check(shape(*tree->children.at(0)->children.at(0)) == expected, "expression shape: " + input);
        } catch (const std::exception& error) { check(false, input + ": " + error.what()); }
    }
}
void lexer_tests() {
    using K = hua::TokenKind;
    hua::Source source("tokens.hua", "let x = 0xff\r\nvar y = 1.5e-3\n\"a\\n\\t\\0\\\\\\\"\"\n");
    hua::Lexer lexer(source);
    auto tokens = lexer.scan();
    const std::vector<K> expected = {K::Let,K::Identifier,K::Assign,K::Integer,K::Newline,
        K::Var,K::Identifier,K::Assign,K::Float,K::Newline,K::String,K::Newline,K::End};
    check(tokens.size() == expected.size(), "token count");
    for (std::size_t i = 0; i < std::min(tokens.size(), expected.size()); ++i)
        check(tokens[i].kind == expected[i], "token kind " + std::to_string(i));
    check(tokens[3].text == "0xff", "integer spelling retained");
    check(tokens[8].text == "1.5e-3", "float spelling retained");
    check(tokens[10].text == std::string("a\n\t\0\\\"", 6), "string escape decoding");
    check(tokens[5].span.line == 2 && tokens[5].span.column == 1, "CRLF location");
    check(tokens[3].span.start_offset == 8 && tokens[3].span.end_offset == 12, "half-open token offsets");
    check(hua::identifier_kind("by") == K::Identifier && hua::identifier_kind("mut") == K::Identifier,
          "context words are not reserved");
    const std::vector<std::string> reserved = {"interface","pub","match","enum","async","await","spawn",
        "taskgroup","parallel","simd","defer","extern"};
    for (const auto& word : reserved) check(hua::identifier_kind(word) != K::Identifier, "reserved: " + word);
    hua::Source bom("bom.hua", "\xEF\xBB\xBFlet x = 1\r\n");
    check(bom.text() == "let x = 1\n", "UTF-8 BOM and newline normalization");
    bool invalid = false;
    try { hua::Source bad("invalid.hua", std::string("\xC0\xAF", 2)); hua::Lexer(bad).scan(); }
    catch (const hua::Diagnostic& error) { invalid = error.code() == "E1009"; }
    check(invalid, "invalid UTF-8 rejected");
    hua::Source bad("diagnostic.hua", "let x = 1\n  let = 2\n");
    try { hua::Parser(hua::Lexer(bad).scan()).parse(); check(false, "invalid binding rejected"); }
    catch (const hua::Diagnostic& error) {
        check(error.span().line == 2 && error.span().column == 7, "diagnostic line and column");
        auto rendered = error.render(bad);
        check(rendered.find("diagnostic.hua:2:7") != std::string::npos &&
              rendered.find("2 |   let = 2") != std::string::npos && rendered.find('^') != std::string::npos,
              "diagnostic includes file, source and caret");
    }
}
void comment_tests() {
    using K = hua::TokenKind;
    struct Case { std::string input; std::vector<K> kinds; };
    const std::vector<Case> cases = {
        {"# comment // / % ** #* ignored", {K::End}},
        {"  # comment\n7//2#3\n", {K::Newline,K::Integer,K::FloorDivide,K::Integer,K::Newline,K::End}},
        {"#* outer #* inner *# outer *#7", {K::Integer,K::End}},
        {"#* *#", {K::End}},
        {"#* outer\n#* inner\n*#\n*#7", {K::Newline,K::Newline,K::Newline,K::Integer,K::End}},
        {"7#*x*#//2", {K::Integer,K::FloorDivide,K::Integer,K::End}},
        {"7//#*x*#2", {K::Integer,K::FloorDivide,K::Integer,K::End}},
        {"7/#*x*#2", {K::Integer,K::Slash,K::Integer,K::End}},
        {"7%#*x*#2", {K::Integer,K::Percent,K::Integer,K::End}},
        {"7**#*x*#2", {K::Integer,K::Power,K::Integer,K::End}},
        {"7*#*x*#2", {K::Integer,K::Star,K::Integer,K::End}},
        {"/#*x*#/", {K::Slash,K::Slash,K::End}},
        {"//#*x*#=", {K::FloorDivide,K::Assign,K::End}},
        {"1#*x*#2", {K::Integer,K::Integer,K::End}},
        {"#* // /= %= ** \" # ignored *#7", {K::Integer,K::End}},
        {"## documentation #* ignored\n7", {K::Newline,K::Integer,K::End}},
        {"##* still line documentation **#\n7", {K::Newline,K::Integer,K::End}},
        {"#** documentation **#7", {K::Integer,K::End}},
        {"#****#7", {K::Integer,K::End}},
        {"#** outer #** inner **# outer **#7", {K::Integer,K::End}},
        {"#* outer #** inner **# outer *#7", {K::Integer,K::End}},
        {"#** outer #* inner *# outer **#7", {K::Integer,K::End}},
        {"#** *# is text **#7", {K::Integer,K::End}},
        {"#* **#7", {K::Integer,K::End}},
        {"#* x *## tail\n7", {K::Newline,K::Integer,K::End}},
        {"7#**x**#**2", {K::Integer,K::Power,K::Integer,K::End}},
        {"#! /usr/bin/env hua #* // **\n7//2", {K::Newline,K::Integer,K::FloorDivide,K::Integer,K::End}},
        {"#! /usr/bin/env hua", {K::End}},
        {"\xEF\xBB\xBF#!/usr/bin/env hua\r\n7//2", {K::Newline,K::Integer,K::FloorDivide,K::Integer,K::End}},
        {"7\n#! later ordinary comment\n2", {K::Integer,K::Newline,K::Newline,K::Integer,K::End}},
        {"// comment", {K::FloorDivide,K::Identifier,K::End}},
        {"  //=2", {K::FloorAssign,K::Integer,K::End}},
        {"7//2/3%4**5", {K::Integer,K::FloorDivide,K::Integer,K::Slash,K::Integer,K::Percent,K::Integer,K::Power,K::Integer,K::End}},
        {"/// //// //= /= **=", {K::FloorDivide,K::Slash,K::FloorDivide,K::FloorDivide,K::FloorAssign,K::SlashAssign,K::PowerAssign,K::End}},
        {"/* */", {K::Slash,K::Star,K::Star,K::Slash,K::End}},
        {"\"# #* *# ## #** **# #! // /* */\"", {K::String,K::End}},
        {"# comment\r\n7#*\r\n nested #*\r*#\n*#//2", {K::Newline,K::Integer,K::Newline,K::Newline,K::Newline,K::FloorDivide,K::Integer,K::End}}
    };
    for (const auto& item : cases) {
        try {
            hua::Source source("comments.hua", item.input);
            auto tokens = hua::Lexer(source).scan();
            std::vector<K> actual;
            for (const auto& token : tokens) {
                actual.push_back(token.kind);
                const auto expected = source.span(token.span.start_offset, token.span.end_offset);
                check(token.span.line == expected.line && token.span.column == expected.column,
                      "comment token location");
                if (token.kind != K::End && token.kind != K::String)
                    check(token.text == source.text().substr(token.span.start_offset,
                          token.span.end_offset - token.span.start_offset), "comment token spelling/span");
            }
            check(actual == item.kinds, "comment/operator tokens: " + item.input);
        } catch (const std::exception& error) { check(false, item.input + ": " + error.what()); }
    }
    for (const auto& input : {"#* unfinished", "#* outer #* inner *#", "#** unfinished *#", "#**#", "#* #** inner *#"}) {
        try { hua::Source source("unclosed.hua", input); hua::Lexer(source).scan(); check(false, "unclosed comment rejected"); }
        catch (const hua::Diagnostic& error) {
            check(error.code() == "E1005" && error.span().start_offset == 0 &&
                  error.span().line == 1 && error.span().column == 1, "unclosed comment diagnostic at outer opener");
        }
    }
    std::string deep;
    for (int i = 0; i < 10000; ++i) deep += "#* ";
    for (int i = 0; i < 10000; ++i) deep += "*# ";
    try { hua::Source source("deep-comment.hua", deep); check(hua::Lexer(source).scan().size() == 1, "deep comments use iterative depth counting"); }
    catch (const std::exception& error) { check(false, error.what()); }
    auto returned = parse("fn f() {\nreturn#*\nnested #* x *#\n*#42\n}\n");
    const auto& body = *returned->children[0]->children.back();
    check(body.children.size() == 2 && body.children[0]->children.empty(), "block comments preserve return newline boundary");
    auto continued = parse("let n=(7\n//#* rhs *#2)# quotient\n");
    check(shape(*continued->children[0]->children.back()) == "Binary://(Integer:7,Integer:2)",
          "line-leading floor division continues an expression");
    try { parse("// old comment\n"); check(false, "old line comment rejected by parser"); }
    catch (const hua::Diagnostic& error) { check(error.code() == "E2001", "line-leading // is an operator requiring a left operand"); }
}
void boundary_tests() {
    auto tree = parse("fn f() {\nreturn\n42\n}\n");
    const auto& body = *tree->children[0]->children.back();
    check(body.children.size() == 2 && body.children[0]->children.empty(), "bare return ends at newline");
    auto nested = parse("unsafe fn f(p ptr<ref<byte>>) {}\nlet n = a >> b\n");
    check(hua::print_ast(*nested).find("Binary >>") != std::string::npos, "generic close does not alter shift parsing");
    const std::vector<std::string> deep = {
        std::string(250, '(') + "1" + std::string(250, ')'),
        std::string(250, '!') + "true"
    };
    for (const auto& input : deep) {
        bool limited = false;
        try { parse(input); } catch (const hua::Diagnostic& error) { limited = error.code() == "E2008"; }
        check(limited, "deep syntax fails safely");
    }
    std::string long_chain = "x";
    for (int i = 0; i < 250; ++i) long_chain += ".x";
    bool limited = false;
    try { parse(long_chain); } catch (const hua::Diagnostic& error) { limited = error.code() == "E2008"; }
    check(limited, "long postfix chain fails safely");
    // A deterministic sample of invalid/incomplete streams must produce a tree or Diagnostic, never hang.
    std::uint32_t seed = 7;
    const std::string alphabet = "()[]{}+*-=/\n,.:abc012 \"";
    for (int n = 0; n < 300; ++n) {
        std::string input;
        for (int j = 0; j < n % 47; ++j) { seed = seed * 1664525u + 1013904223u; input += alphabet[seed % alphabet.size()]; }
        try { parse(input); check(true, "short malformed input terminates"); }
        catch (const hua::Diagnostic&) { check(true, "short malformed input diagnosed"); }
        catch (const std::exception& error) { check(false, "unexpected frontend exception: " + std::string(error.what())); }
    }
}
int fixtures() {
    std::vector<std::filesystem::path> paths;
    for (const auto& item : std::filesystem::directory_iterator(HUA_TEST_ROOT))
        if (item.path().extension() == ".hua") paths.push_back(item.path());
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths) {
        auto expected_path = path; expected_path.replace_extension(".expect");
        auto expected = read(expected_path);
        hua::Source source(path.string(), read(path));
        try {
            auto tree = hua::Parser(hua::Lexer(source).scan()).parse();
            if (expected.starts_with("ERROR ")) { check(false, "expected diagnostic: " + path.filename().string()); continue; }
            check(expected.starts_with("OK"), "fixture has a defined success expectation");
            auto golden_path = path; golden_path.replace_extension(".ast");
            auto printed = hua::print_ast(*tree);
            if (std::filesystem::exists(golden_path)) check(printed == read(golden_path), "golden AST: " + path.filename().string());
            std::istringstream lines(expected); std::string line;
            std::getline(lines, line);
            while (std::getline(lines, line)) if (!line.empty())
                check(printed.find(line) != std::string::npos, "AST fragment " + path.filename().string() + ": " + line);
            span_tree(*tree, source);
        } catch (const hua::Diagnostic& error) {
            check(expected.starts_with("ERROR "), "unexpected diagnostic " + path.filename().string() + ": " + error.what());
            std::istringstream lines(expected); std::string line; std::getline(lines, line);
            check(line == "ERROR " + error.code(), "expected error code: " + path.filename().string());
            while (std::getline(lines, line)) if (!line.empty())
                check(std::string(error.what()).find(line) != std::string::npos, "expected error reason: " + path.filename().string());
        }
    }
    return static_cast<int>(paths.size());
}
}
int main() {
    try {
        lexer_tests(); expression_tests(); boundary_tests(); comment_tests(); auto count = fixtures();
        std::cout << count << " spec fixtures, " << checks << " checks, " << failures << " failures\n";
    } catch (const std::exception& error) { check(false, error.what()); }
    return failures == 0 ? 0 : 1;
}
