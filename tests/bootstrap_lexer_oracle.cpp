#include "hua/lexer.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
static void quoted(const std::string& text) {
    static constexpr char hex[] = "0123456789abcdef";
    std::cout << '"';
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') std::cout << '\\' << c;
        else if (c < 32) std::cout << "\\u00" << hex[c >> 4] << hex[c & 15];
        else std::cout << c;
    }
    std::cout << '"';
}
static int run(const std::string& filename) {
    std::ifstream input(std::filesystem::path(std::u8string(filename.begin(), filename.end())), std::ios::binary);
    if (!input) return 2;
    hua::Source source(filename, std::string(std::istreambuf_iterator<char>(input), {}));
    try {
        for (const auto& token : hua::Lexer(source).scan()) {
            std::string kind;
            using K = hua::TokenKind;
            switch (token.kind) {
            case K::End: kind="end"; break;
            case K::Newline: kind="newline"; break;
            case K::Identifier: kind="identifier"; break;
            case K::Integer: kind="integer"; break;
            case K::Float: kind="float"; break;
            case K::String: kind="string"; break;
            default: kind=token.text;
            }
            std::cout << kind << ' ' << token.span.line << ' ' << token.span.column << ' ';
            quoted(token.text); std::cout << '\n';
        }
    } catch (const hua::Diagnostic& error) {
        std::cout << "error " << error.code() << '@' << error.span().line << ':' << error.span().column << '\n';
    }
    return 0;
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    if (argc!=2) return 2;
    int n=WideCharToMultiByte(CP_UTF8,0,argv[1],-1,nullptr,0,nullptr,nullptr);
    if(n<=0) return 2;
    std::string text(n,'\0');
    WideCharToMultiByte(CP_UTF8,0,argv[1],-1,text.data(),n,nullptr,nullptr);
    text.pop_back(); return run(text);
}
#else
int main(int argc,char** argv) { return argc==2 ? run(argv[1]) : 2; }
#endif
