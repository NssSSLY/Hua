#pragma once

#include "hua/value.hpp"

namespace hua::text_format {
inline constexpr std::size_t max_text=16*1024*1024;
inline bool continuation(char c){return (static_cast<unsigned char>(c)&0xC0)==0x80;}
inline std::size_t scalars(std::string_view text){
    std::size_t count=0;
    for(char c:text)count+=!continuation(c);
    return count;
}

// Appends `value` padded by a placeholder spec "[[fill]align][width]"; false when the spec is malformed.
inline bool append_with_spec(std::string& out, const std::string& value, std::string_view spec, const SourceSpan& span) {
    std::string_view fill = " ";
    char align = '<';
    auto is_align = [](char c) { return c == '<' || c == '>' || c == '^'; };
    if (!spec.empty()) {
        std::size_t lead = 1;
        while (lead < spec.size() && continuation(spec[lead])) ++lead;
        if (lead < spec.size() && is_align(spec[lead])) {
            fill = spec.substr(0, lead);
            align = spec[lead];
            spec.remove_prefix(lead + 1);
        } else if (is_align(spec[0])) {
            align = spec[0];
            spec.remove_prefix(1);
        }
    }
    std::uint64_t width = 0;
    for (char c : spec) {
        if (c < '0' || c > '9') return false;
        width = width * 10 + static_cast<unsigned>(c - '0');
        if (width > max_text) runtime_error(span, "format text exceeds 16 MiB", "E4099");
    }
    const auto have = scalars(value);
    const auto missing = width > have ? static_cast<std::size_t>(width) - have : 0;
    if (missing > (max_text - std::min(max_text, out.size() + value.size())) / fill.size()) runtime_error(span, "format text exceeds 16 MiB", "E4099");
    const auto before = align == '>' ? missing : align == '^' ? missing / 2 : 0;
    for (std::size_t i = 0; i < before; ++i) out += fill;
    out += value;
    for (std::size_t i = before; i < missing; ++i) out += fill;
    return true;
}
// Shared by format and template. `resolve(key)` returns the value or nullptr after setting `problem`.
template <class Resolve>
bool expand_placeholders(const std::string& a, std::string& out, std::string& problem, const SourceSpan& s, Resolve resolve) {
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char c = a[i];
        if (c == '}') {
            if (i + 1 < a.size() && a[i + 1] == '}') { out += '}'; ++i; continue; }
            problem = "FORMAT_ARGS: unmatched '}'";
            return false;
        }
        if (c != '{') { out += c; continue; }
        if (i + 1 < a.size() && a[i + 1] == '{') { out += '{'; ++i; continue; }
        const auto close = a.find('}', i + 1);
        if (close == std::string::npos) { problem = "FORMAT_ARGS: unclosed '{'"; return false; }
        const std::string_view body(a.data() + i + 1, close - i - 1);
        const auto colon = body.find(':');
        const auto* value = resolve(body.substr(0, colon));
        if (!value) return false;
        if (colon == std::string_view::npos) out += *value;
        else if (!append_with_spec(out, *value, body.substr(colon + 1), s)) { problem = "FORMAT_ARGS: invalid format spec"; return false; }
        if (out.size() > max_text) runtime_error(s, "format text exceeds 16 MiB", "E4099");
        i = close;
    }
    return true;
}

}
