// A05 independent reference: existing C++ Source and Parser, test-only.
#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
static void node(const hua::Node& n) {
    std::cout << "node\t" << hua::node_name(n.kind) << '\t' << n.text << '\t'
              << n.span.start_offset << '\t' << n.span.end_offset << '\t'
              << n.span.line << '\t' << n.span.column << '\n';
    for (const auto& child : n.children) node(*child);
}
static int run(const std::string& mode, const std::string& filename) {
    std::ifstream input(std::filesystem::path(std::u8string(filename.begin(), filename.end())), std::ios::binary);
    if (!input) return 2;
    hua::Source source("input.hua", std::string(std::istreambuf_iterator<char>(input), {}));
    try {
        if (mode == "positions") {
            std::cout << "size\t" << source.text().size() << '\n';
            std::cout << "data\t[";
            bool first = true;
            for (unsigned char byte : source.text()) {
                if (!first) std::cout << ", ";
                first = false; std::cout << static_cast<unsigned>(byte);
            }
            std::cout << "]\n";
            for (std::size_t i = 0; i <= source.text().size(); ++i) {
                const auto s = source.span(i, i);
                std::cout << "position\t" << i << '\t' << s.line << '\t' << s.column << '\n';
            }
        } else if (mode == "ast") {
            node(*hua::Parser(hua::Lexer(source).scan()).parse());
        } else return 2;
    } catch (const hua::Diagnostic& e) {
        std::cerr << e.code() << ": " << e.what() << '\n'; return 1;
    }
    return 0;
}

#ifdef _WIN32
static std::string utf8(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    result.pop_back(); return result;
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 2;
    return run(utf8(argv[1]), utf8(argv[2]));
}
#else
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    return run(argv[1], argv[2]);
}
#endif
