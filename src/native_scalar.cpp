#include "hua/native_scalar.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <type_traits>

namespace hn {
namespace {
constexpr unsigned call_limit = 128;
std::array<const SourceSpan*, call_limit> sites{};
unsigned depth = 0;

void write(FILE* stream, std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stream);
}
}

[[noreturn]] void fail(const SourceSpan& site, const char* message, const char* code) {
    throw hua::Diagnostic(code, site, message);
}
unsigned enter_scalar(const SourceSpan& site) {
    if (depth >= call_limit) [[unlikely]] fail(site, "function call depth limit exceeded (128)", "E4099");
    sites[depth] = &site;
    return depth++;
}
void leave_scalar(unsigned previous) { depth = previous; }

void print(const SourceSpan&, std::initializer_list<Value> arguments) {
    bool first = true;
    for (const auto& value : arguments) {
        if (!first) std::fputc(' ', stdout);
        first = false;
        std::visit([](auto item) {
            using T = decltype(item);
            if constexpr (std::is_same_v<T, bool>) write(stdout, item ? "true" : "false");
            else if constexpr (std::is_same_v<T, I>) write(stdout, std::to_string(item));
            else write(stdout, item);
        }, value);
    }
    std::fputc('\n', stdout);
}

int run_main(int, char**, const SourceSpan& main_site, Value (*body)()) {
    sites[0] = &main_site;
    depth = 1;
    try {
        body();
        return 0;
    } catch (const hua::Diagnostic& error) {
        auto reported = error;
        for (unsigned i = depth; i-- > 0;) reported.add_context(*sites[i], "called from");
        const auto& file = reported.span().file_id;
        if (!file.empty()) {
            const std::filesystem::path path(std::u8string(file.begin(), file.end()));
            std::ifstream input(path, std::ios::binary);
            if (input) {
                const std::string text(std::istreambuf_iterator<char>(input), {});
                write(stderr, reported.render(hua::Source(file, text)));
                return 1;
            }
        }
        write(stderr, "error[" + reported.code() + "]: " + reported.what() + "\n");
        if (!file.empty()) {
            write(stderr, " --> " + file + ":" + std::to_string(reported.span().line)
                + ":" + std::to_string(reported.span().column) + "\n");
        }
        write(stderr, hua::render_source_trace(reported.trace()));
        return 1;
    } catch (const std::exception& error) {
        write(stderr, std::string("error[E0002]: internal command failure: ") + error.what() + "\n");
        return 3;
    }
}
}
