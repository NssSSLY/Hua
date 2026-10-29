// std.strings additions of HUA-D038 slice 1 (since 9): searching, splitting, trimming,
// padding, parsing and formatting. Strings are valid UTF-8 by language invariant; offsets
// and lengths are bytes, "width" counts Unicode scalars, whitespace is ASCII (as trim).
// Each call is one host pass with the result sized once; limits follow std.strings.
#include "hua/stdlib.hpp"
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace hua {
namespace {
using I = std::int64_t;
constexpr std::size_t max_text = 16 * 1024 * 1024, max_items = 1000000;
constexpr const char* blank = " \t\r\n\v\f";

bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }
bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }
std::size_t scalars(const std::string& text) {
    std::size_t count = 0;
    for (char c : text) count += !continuation(c);
    return count;
}
std::size_t scalar_end(const std::string& text, std::size_t at) {
    std::size_t next = at + 1;
    while (next < text.size() && continuation(text[next])) ++next;
    return next;
}
void limit_text(std::size_t size, const SourceSpan& span, const char* what) {
    if (size > max_text) runtime_error(span, std::string(what) + " text exceeds 16 MiB", "E4099");
}
Value string_list(std::vector<std::string> values, const SourceSpan& span) {
    if (values.size() > max_items) runtime_error(span, "split item limit exceeded", "E4099");
    auto data = managed<std::vector<Value>>();
    data->reserve(values.size());
    for (auto& value : values) data->emplace_back(std::move(value));
    return Value(SliceValue{data, 0, data->size(), false, "string"});
}
I integer(const Value& value, const SourceSpan& span) { return as_int(value, span); }

// Byte-wise digit value for bases up to 36, or 36 when the byte is not a digit.
unsigned digit(char c) {
    if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
    if (c >= 'a' && c <= 'z') return static_cast<unsigned>(c - 'a' + 10);
    if (c >= 'A' && c <= 'Z') return static_cast<unsigned>(c - 'A' + 10);
    return 36;
}
}

Value invoke_text_standard(const StandardFunction& f, const std::vector<Value>& args, const SourceSpan& s) {
    auto result = type_arguments(f.result, "Result");
    const std::string success = result.empty() ? "" : result[0];
    auto ok = [&](Value v) { return standard_result(std::move(v), true, success); };
    auto err = [&](std::string text) { return standard_result(Value(std::move(text)), false, success); };
    auto text = [&](std::size_t i) -> const std::string& { return hua::string_value(args[i].data); };
    const auto& n = f.name;

    if (n == "format_int") {
        auto value = integer(args[0], s), base = integer(args[1], s);
        if (base < 2 || base > 36) runtime_error(s, "format_int base must be 2..36", "E4003");
        char buffer[72];
        auto done = std::to_chars(buffer, buffer + sizeof buffer, value, static_cast<int>(base));
        return Value(std::string(buffer, done.ptr));
    }
    if (n == "format_float") {
        auto value = std::get<double>(args[0].data);
        auto decimals = integer(args[1], s);
        if (decimals < 0 || decimals > 17) runtime_error(s, "format_float decimals must be 0..17", "E4003");
        // Fixed notation of the exact binary value, ties to even (as printf/round).
        char buffer[400];
        auto done = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::fixed, static_cast<int>(decimals));
        if (done.ec != std::errc()) runtime_error(s, "format_float output exceeds its buffer", "E4099");
        return Value(std::string(buffer, done.ptr));
    }

    const auto& a = text(0);
    if (n == "is_empty") return Value(a.empty());
    if (n == "trim_start") {
        auto begin = a.find_first_not_of(blank);
        return Value(begin == std::string::npos ? std::string{} : a.substr(begin));
    }
    if (n == "trim_end") {
        auto end = a.find_last_not_of(blank);
        return Value(end == std::string::npos ? std::string{} : a.substr(0, end + 1));
    }
    if (n == "trim_prefix") { const auto& p = text(1); return Value(a.starts_with(p) ? a.substr(p.size()) : a); }
    if (n == "trim_suffix") { const auto& p = text(1); return Value(a.ends_with(p) ? a.substr(0, a.size() - p.size()) : a); }
    if (n == "last_index") {
        const auto& b = text(1);
        if (b.empty()) return Value(static_cast<I>(a.size()));
        auto found = a.rfind(b);
        return Value(found == std::string::npos ? I{-1} : static_cast<I>(found));
    }
    if (n == "count") {
        const auto& b = text(1);
        if (b.empty()) return Value(static_cast<I>(scalars(a) + 1));
        I count = 0;
        for (auto at = a.find(b); at != std::string::npos; at = a.find(b, at + b.size())) ++count;
        return Value(count);
    }
    if (n == "repeat") {
        auto times = integer(args[1], s);
        if (times < 0) runtime_error(s, "repeat count must not be negative", "E4003");
        if (a.empty() || times == 0) return Value(std::string{});
        if (static_cast<std::uint64_t>(times) > max_text / a.size()) runtime_error(s, "repeat text exceeds 16 MiB", "E4099");
        std::string out;
        out.reserve(a.size() * static_cast<std::size_t>(times));
        for (I i = 0; i < times; ++i) out += a;
        return Value(std::move(out));
    }
    if (n == "split_n") {
        const auto& separator = text(1);
        auto limit = integer(args[2], s);
        std::vector<std::string> out;
        if (limit == 0) return string_list(std::move(out), s);
        // Cuts allowed before the remainder; negative means unlimited, as split.
        const auto cuts = limit < 0 ? std::numeric_limits<std::size_t>::max() : static_cast<std::size_t>(limit - 1);
        std::size_t at = 0;
        if (separator.empty()) {
            while (at < a.size()) {
                if (out.size() == cuts) { out.push_back(a.substr(at)); return string_list(std::move(out), s); }
                auto next = scalar_end(a, at);
                out.push_back(a.substr(at, next - at));
                at = next;
                if (out.size() > max_items) runtime_error(s, "split item limit exceeded", "E4099");
            }
            return string_list(std::move(out), s);
        }
        while (true) {
            auto next = out.size() == cuts ? std::string::npos : a.find(separator, at);
            out.push_back(a.substr(at, next == std::string::npos ? std::string::npos : next - at));
            if (out.size() > max_items) runtime_error(s, "split item limit exceeded", "E4099");
            if (next == std::string::npos) break;
            at = next + separator.size();
        }
        return string_list(std::move(out), s);
    }
    if (n == "fields") {
        std::vector<std::string> out;
        std::size_t at = 0;
        while (at < a.size()) {
            while (at < a.size() && space(a[at])) ++at;
            if (at == a.size()) break;
            auto end = at;
            while (end < a.size() && !space(a[end])) ++end;
            out.push_back(a.substr(at, end - at));
            if (out.size() > max_items) runtime_error(s, "split item limit exceeded", "E4099");
            at = end;
        }
        return string_list(std::move(out), s);
    }
    if (n == "lines") {
        std::vector<std::string> out;
        std::size_t at = 0;
        while (at < a.size()) {
            auto end = a.find('\n', at);
            auto stop = end == std::string::npos ? a.size() : end;
            auto length = stop - at;
            if (length > 0 && a[stop - 1] == '\r') --length;
            out.push_back(a.substr(at, length));
            if (out.size() > max_items) runtime_error(s, "split item limit exceeded", "E4099");
            if (end == std::string::npos) break;
            at = end + 1;
        }
        return string_list(std::move(out), s);
    }
    if (n == "pad_left" || n == "pad_right") {
        auto width = integer(args[1], s);
        const auto& fill = text(2);
        if (width < 0) runtime_error(s, "pad width must not be negative", "E4003");
        if (fill.empty() || scalar_end(fill, 0) != fill.size()) runtime_error(s, "pad fill must be exactly one Unicode scalar", "E4003");
        auto have = scalars(a);
        if (have >= static_cast<std::uint64_t>(width)) return Value(a);
        auto missing = static_cast<std::size_t>(width) - have;
        if (missing > (max_text - a.size()) / fill.size()) runtime_error(s, "pad text exceeds 16 MiB", "E4099");
        std::string out;
        out.reserve(a.size() + missing * fill.size());
        if (n == "pad_right") out += a;
        for (std::size_t i = 0; i < missing; ++i) out += fill;
        if (n == "pad_left") out += a;
        return Value(std::move(out));
    }
    if (n == "replace_n") {
        const auto& before = text(1);
        const auto& after = text(2);
        auto limit = integer(args[3], s);
        if (before.empty() || limit == 0) return Value(a);
        std::string out;
        std::size_t at = 0;
        I done = 0;
        while (limit < 0 || done < limit) {
            auto next = a.find(before, at);
            if (next == std::string::npos) break;
            out.append(a, at, next - at);
            out += after;
            limit_text(out.size(), s, "replace");
            at = next + before.size();
            ++done;
        }
        out.append(a, at, std::string::npos);
        limit_text(out.size(), s, "replace");
        return Value(std::move(out));
    }
    if (n == "parse_int") {
        auto base = integer(args[1], s);
        if (base < 2 || base > 36) runtime_error(s, "parse_int base must be 2..36", "E4003");
        std::size_t at = 0;
        bool negative = false;
        if (at < a.size() && (a[at] == '+' || a[at] == '-')) { negative = a[at] == '-'; ++at; }
        if (at == a.size()) return err("PARSE_SYNTAX: no digits in integer text");
        // Accumulate the magnitude unsigned so -9223372036854775808 parses exactly.
        const std::uint64_t bound = negative ? std::uint64_t{1} << 63 : static_cast<std::uint64_t>(std::numeric_limits<I>::max());
        std::uint64_t magnitude = 0;
        bool overflow = false;
        for (; at < a.size(); ++at) {
            auto value = digit(a[at]);
            if (value >= static_cast<unsigned>(base)) return err("PARSE_SYNTAX: invalid digit for base " + std::to_string(base));
            if (!overflow && magnitude > (bound - value) / static_cast<std::uint64_t>(base)) overflow = true;
            if (!overflow) magnitude = magnitude * static_cast<std::uint64_t>(base) + value;
        }
        if (overflow) return err("PARSE_RANGE: integer is outside int64");
        if (!negative) return ok(Value(static_cast<I>(magnitude)));
        return ok(Value(magnitude == (std::uint64_t{1} << 63) ? std::numeric_limits<I>::min() : -static_cast<I>(magnitude)));
    }
    if (n == "parse_float") {
        std::size_t at = 0;
        if (at < a.size() && a[at] == '+') ++at;
        // Decimal digits with optional fraction and exponent only; no inf/nan/hex forms.
        std::size_t i = at;
        if (at == 0 && i < a.size() && a[i] == '-') ++i;  // at most one sign
        bool digits = false;
        while (i < a.size() && a[i] >= '0' && a[i] <= '9') { ++i; digits = true; }
        if (i < a.size() && a[i] == '.') { ++i; while (i < a.size() && a[i] >= '0' && a[i] <= '9') { ++i; digits = true; } }
        if (!digits) return err("PARSE_SYNTAX: invalid float text");
        if (i < a.size() && (a[i] == 'e' || a[i] == 'E')) {
            ++i;
            if (i < a.size() && (a[i] == '+' || a[i] == '-')) ++i;
            bool exponent = false;
            while (i < a.size() && a[i] >= '0' && a[i] <= '9') { ++i; exponent = true; }
            if (!exponent) return err("PARSE_SYNTAX: invalid float exponent");
        }
        if (i != a.size()) return err("PARSE_SYNTAX: invalid float text");
        double value{};
        auto done = std::from_chars(a.data() + at, a.data() + a.size(), value);
        if (done.ec == std::errc::result_out_of_range) {
            // Overflow is a range error; underflow rounds to the nearest value (0 or subnormal).
            std::string copy(a, at);
            value = std::strtod(copy.c_str(), nullptr);
            if (!std::isfinite(value)) return err("PARSE_RANGE: float is outside the finite range");
            return ok(Value(value));
        }
        if (done.ec != std::errc() || done.ptr != a.data() + a.size()) return err("PARSE_SYNTAX: invalid float text");
        if (!std::isfinite(value)) return err("PARSE_RANGE: float is outside the finite range");
        return ok(Value(value));
    }
    if (n == "format") {
        const auto& items = std::get<SliceValue>(args[1].data);
        std::string out;
        std::size_t next = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            char c = a[i];
            if (c == '}') {
                if (i + 1 < a.size() && a[i + 1] == '}') { out += '}'; ++i; continue; }
                return err("FORMAT_ARGS: unmatched '}'");
            }
            if (c != '{') { out += c; continue; }
            if (i + 1 < a.size() && a[i + 1] == '{') { out += '{'; ++i; continue; }
            auto close = a.find('}', i + 1);
            if (close == std::string::npos) return err("FORMAT_ARGS: unclosed '{'");
            std::size_t index = next;
            if (close == i + 1) ++next;
            else {
                std::uint64_t value = 0;
                for (auto k = i + 1; k < close; ++k) {
                    if (a[k] < '0' || a[k] > '9') return err("FORMAT_ARGS: placeholder must be {} or {index}");
                    value = value * 10 + static_cast<unsigned>(a[k] - '0');
                    if (value > max_items) return err("FORMAT_ARGS: placeholder index out of range");
                }
                index = static_cast<std::size_t>(value);
            }
            if (index >= items.length) return err("FORMAT_ARGS: placeholder index out of range");
            out += hua::string_value((*items.storage)[items.start + index].data);
            if (out.size() > max_text) runtime_error(s, "format text exceeds 16 MiB", "E4099");
            i = close;
        }
        limit_text(out.size(), s, "format");
        return ok(Value(std::move(out)));
    }
    runtime_error(s, "unknown standard function", "E4003");
}
}
