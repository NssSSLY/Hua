#pragma once

// The generated scalar entry uses the same source locations and checked
// integer operations as the full native runtime, without boxed containers.
#include "hua/source.hpp"
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <variant>

namespace hn {
using I = std::int64_t;
using Str = std::string;
using hua::SourceSpan;
using Value = std::variant<I, bool, std::string_view>;

template<class T> T capture_value(T value) { return value; }
template<class T> T clone(const T& value) { return value; }
inline Value to_value(I value) { return value; }
inline Value to_value(bool value) { return value; }
inline Value to_value(const Str& value) { return std::string_view(value); }

[[noreturn]] void fail(const SourceSpan& site, const char* message, const char* code);
unsigned enter_scalar(const SourceSpan& site);
void leave_scalar(unsigned depth);
template<bool CountSteps, bool Tasks> unsigned enter(const SourceSpan& site) {
    static_assert(!CountSteps && !Tasks);
    return enter_scalar(site);
}
template<bool Tasks> void leave(unsigned depth) {
    static_assert(!Tasks);
    leave_scalar(depth);
}

inline I add(I a, I b, const SourceSpan& site) {
    I result;
    if (__builtin_add_overflow(a, b, &result)) [[unlikely]] fail(site, "integer overflow", "E4002");
    return result;
}
inline I sub(I a, I b, const SourceSpan& site) {
    I result;
    if (__builtin_sub_overflow(a, b, &result)) [[unlikely]] fail(site, "integer overflow", "E4002");
    return result;
}
inline I mul(I a, I b, const SourceSpan& site) {
    I result;
    if (__builtin_mul_overflow(a, b, &result)) [[unlikely]] fail(site, "integer overflow", "E4002");
    return result;
}
inline I neg(I value, const SourceSpan& site) { return sub(0, value, site); }
inline I floor_div(I a, I b, const SourceSpan& site) {
    if (!b) [[unlikely]] fail(site, "division by zero", "E4004");
    if (a == std::numeric_limits<I>::min() && b == -1) [[unlikely]]
        fail(site, "integer division overflow", "E4002");
    const I r = a % b;
    return a / b - (r != 0 && ((r < 0) != (b < 0)));
}
inline I mod(I a, I b, const SourceSpan& site) {
    if (!b) [[unlikely]] fail(site, "division by zero", "E4004");
    if (a == std::numeric_limits<I>::min() && b == -1) return 0;
    const I r = a % b;
    return r != 0 && ((r < 0) != (b < 0)) ? r + b : r;
}
inline Str concat(const Str& a, const Str& b, const SourceSpan&) { return a + b; }
void print(const SourceSpan& site, std::initializer_list<Value> arguments);
int run_main(int argc, char** argv, const SourceSpan& main_site, Value (*body)());
}
