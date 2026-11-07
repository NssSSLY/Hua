// std.strings upper/lower/casefold/equal_fold (since 26): locale-independent full Unicode
// case mappings, including the final sigma rule of lower. Strings are valid UTF-8 by
// language invariant. ASCII takes a fast path; everything else binary-searches the
// generated tables.
#include "hua/stdlib.hpp"
#include <algorithm>
#include <array>
#include <iterator>
namespace hua {
namespace {
struct CaseMapping { std::uint32_t from, to; };
struct CaseRange { std::uint32_t first, last; };
#include "unicode_case_data.inc"
constexpr std::size_t max_text = 16 * 1024 * 1024;
constexpr std::uint32_t capital_sigma = 0x3A3, final_sigma = 0x3C2, multi_flag = 0x80000000u;

std::uint32_t decode(std::string_view s, std::size_t& at) {
    const auto lead = static_cast<unsigned char>(s[at++]);
    if (lead < 0x80) return lead;
    const unsigned extra = lead < 0xE0 ? 1 : lead < 0xF0 ? 2 : 3;
    std::uint32_t value = extra == 1 ? lead & 0x1F : extra == 2 ? lead & 0x0F : lead & 0x07;
    for (unsigned i = 0; i < extra; ++i) value = value << 6 | (static_cast<unsigned char>(s[at++]) & 0x3F);
    return value;
}
std::uint32_t decode_before(std::string_view s, std::size_t& at) {
    do --at; while (at > 0 && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80);
    auto copy = at;
    return decode(s, copy);
}
template <std::size_t N> bool in_ranges(const CaseRange (&table)[N], std::uint32_t c) {
    auto found = std::upper_bound(std::begin(table), std::end(table), c, [](std::uint32_t v, const CaseRange& r) { return v < r.first; });
    return found != std::begin(table) && c <= std::prev(found)->last;
}
template <std::size_t N> const CaseMapping* find_mapping(const CaseMapping (&table)[N], std::uint32_t c) {
    auto found = std::lower_bound(std::begin(table), std::end(table), c, [](const CaseMapping& m, std::uint32_t v) { return m.from < v; });
    return found != std::end(table) && found->from == c ? found : nullptr;
}
// Up to three scalars for one input scalar; returns how many.
template <std::size_t N> unsigned mapped(const CaseMapping (&table)[N], std::uint32_t c, std::array<std::uint32_t, 3>& out) {
    const auto* m = find_mapping(table, c);
    if (!m) { out[0] = c; return 1; }
    if (!(m->to & multi_flag)) { out[0] = m->to; return 1; }
    const auto& seq = case_multi[m->to & ~multi_flag];
    unsigned n = 0;
    while (n < 3 && seq[n]) { out[n] = seq[n]; ++n; }
    return n;
}
bool is_final_sigma(std::string_view s, std::size_t start, std::size_t end) {
    auto before = start;
    bool cased_before = false;
    while (before > 0) {
        const auto c = decode_before(s, before);
        if (in_ranges(case_ignorable, c)) continue;
        cased_before = in_ranges(case_cased, c);
        break;
    }
    if (!cased_before) return false;
    for (auto after = end; after < s.size();) {
        const auto c = decode(s, after);
        if (in_ranges(case_ignorable, c)) continue;
        return !in_ranges(case_cased, c);
    }
    return true;
}
enum class Mode { upper, lower, fold };
std::string convert(std::string_view s, Mode mode, const SourceSpan& span) {
    std::string out;
    out.reserve(s.size());
    std::array<std::uint32_t, 3> scalars{};
    for (std::size_t at = 0; at < s.size();) {
        const auto byte = static_cast<unsigned char>(s[at]);
        if (byte < 0x80) {
            ++at;
            if (mode == Mode::upper) out += byte >= 'a' && byte <= 'z' ? static_cast<char>(byte - 32) : static_cast<char>(byte);
            else out += byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte + 32) : static_cast<char>(byte);
            continue;
        }
        const auto start = at;
        const auto c = decode(s, at);
        unsigned count;
        if (mode == Mode::lower && c == capital_sigma && is_final_sigma(s, start, at)) { scalars[0] = final_sigma; count = 1; }
        else count = mode == Mode::upper ? mapped(case_upper, c, scalars) : mode == Mode::lower ? mapped(case_lower, c, scalars) : mapped(case_fold, c, scalars);
        for (unsigned i = 0; i < count; ++i) append_utf8(out, scalars[i]);
        if (out.size() > max_text) runtime_error(span, "case mapping text exceeds 16 MiB", "E4099");
    }
    if (out.size() > max_text) runtime_error(span, "case mapping text exceeds 16 MiB", "E4099");
    return out;
}
// Streams the case-folded scalars of a string, so equal_fold allocates nothing.
class FoldStream {
public:
    explicit FoldStream(std::string_view s) : text(s) {}
    bool next(std::uint32_t& out) {
        if (held == used) {
            if (at >= text.size()) return false;
            const auto byte = static_cast<unsigned char>(text[at]);
            if (byte < 0x80) { ++at; out = byte >= 'A' && byte <= 'Z' ? byte + 32u : byte; return true; }
            const auto c = decode(text, at);
            held = mapped(case_fold, c, pending);
            used = 0;
        }
        out = pending[used++];
        return true;
    }
private:
    std::string_view text;
    std::size_t at = 0;
    std::array<std::uint32_t, 3> pending{};
    unsigned held = 0, used = 0;
};
}
Value invoke_unicode_case(const StandardFunction& f, const std::vector<Value>& args, const SourceSpan& span) {
    const std::string_view a = string_value(args[0].data);
    if (f.name == "upper") return Value(convert(a, Mode::upper, span));
    if (f.name == "lower") return Value(convert(a, Mode::lower, span));
    if (f.name == "casefold") return Value(convert(a, Mode::fold, span));
    const std::string_view b = string_value(args[1].data);
    if (a == b) return Value(true);
    FoldStream left(a), right(b);
    for (;;) {
        std::uint32_t x, y;
        const bool has_x = left.next(x), has_y = right.next(y);
        if (has_x != has_y) return Value(false);
        if (!has_x) return Value(true);
        if (x != y) return Value(false);
    }
}
}
