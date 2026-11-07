#include "hua/stdlib.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <string_view>

namespace hua {
namespace {
using I = std::int64_t;
using U = std::uint64_t;
using Wide = __int128;
constexpr I low = std::numeric_limits<I>::min(), high = std::numeric_limits<I>::max();
enum class Mode { checked, wrapping, saturating };
Value overflow(Mode mode, bool negative, const std::string& name) {
    if (mode == Mode::saturating) return Value(negative ? low : high);
    return standard_result(Value("MATH_OVERFLOW: math." + name + " result is outside int range"), false, "int");
}
Value integer_power(Mode mode, I base, I exponent, const StandardFunction& f, const SourceSpan& span) {
    if (exponent < 0) {
        if (mode == Mode::checked)
            return standard_result(Value(std::string("MATH_DOMAIN: exponent must be non-negative")), false, "int");
        runtime_error(span, "math integer exponent must be non-negative", "E4003");
    }
    U n = static_cast<U>(exponent);
    if (mode == Mode::wrapping) {
        U value = std::bit_cast<U>(base), result = 1;
        // Unsigned arithmetic is modulo 2^64; at most 63 exponent bits.
        while (n) {
            if (n & 1) result *= value;
            n >>= 1;
            if (n) value *= value;
        }
        return Value(std::bit_cast<I>(result));
    }
    const bool negative = base < 0 && (n & 1);
    const U limit = negative ? U{1} << 63 : static_cast<U>(high);
    U value = base < 0 ? U{0} - static_cast<U>(base) : static_cast<U>(base), result = 1;
    while (n) {
        if (n & 1) {
            if (value && result > limit / value) return overflow(mode, negative, f.name);
            result *= value;
        }
        n >>= 1;
        if (n) {
            if (value && value > limit / value) return overflow(mode, negative, f.name);
            value *= value;
        }
    }
    // The negative limit is representable, even though +2^63 is not an int.
    const I answer = negative ? (result == (U{1} << 63) ? low : -static_cast<I>(result)) : static_cast<I>(result);
    return mode == Mode::checked ? standard_result(Value(answer), true, "int") : Value(answer);
}
Value integer_operation(const StandardFunction& f, const std::vector<Value>& args, const SourceSpan& span) {
    std::string_view name = f.name;
    const auto split = name.find('_');
    const auto operation = name.substr(split + 1);
    const Mode mode = name.starts_with("checked_") ? Mode::checked
                    : name.starts_with("wrapping_") ? Mode::wrapping : Mode::saturating;
    const I a = std::get<I>(args[0].data);
    if (operation == "pow") return integer_power(mode, a, std::get<I>(args[1].data), f, span);
    Wide result;
    if (operation == "neg") result = -static_cast<Wide>(a);
    else if (operation == "abs") result = a < 0 ? -static_cast<Wide>(a) : static_cast<Wide>(a);
    else {
        const I b = std::get<I>(args[1].data);
        if (operation == "add") result = static_cast<Wide>(a) + b;
        else if (operation == "sub") result = static_cast<Wide>(a) - b;
        else if (operation == "mul") result = static_cast<Wide>(a) * b;
        else runtime_error(span, "unknown integer math operation", "E4003");
    }
    // Conversion to unsigned is defined modulo 2^64, including negative values.
    if (mode == Mode::wrapping) return Value(std::bit_cast<I>(static_cast<U>(result)));
    if (result < low || result > high) return overflow(mode, result < 0, f.name);
    const auto answer = static_cast<I>(result);
    return mode == Mode::checked ? standard_result(Value(answer), true, "int") : Value(answer);
}
}
Value invoke_math_arithmetic(const StandardFunction& f, const std::vector<Value>& args, const SourceSpan& span) {
    if (f.name.starts_with("checked_") || f.name.starts_with("wrapping_") || f.name.starts_with("saturating_"))
        return integer_operation(f, args, span);
    const double x = std::get<double>(args[0].data);
    auto failure = [](std::string text) { return standard_result(Value(std::move(text)), false, "float"); };
    if (!std::isfinite(x)) return failure("MATH_DOMAIN: finite input required");
    if (f.name == "acosh" && x < 1.0) return failure("MATH_DOMAIN: acosh requires x >= 1");
    if (f.name == "atanh" && (x <= -1.0 || x >= 1.0)) return failure("MATH_DOMAIN: atanh requires -1 < x < 1");
    double result;
    if (f.name == "sinh") result = std::sinh(x);
    else if (f.name == "cosh") result = std::cosh(x);
    else if (f.name == "tanh") result = std::tanh(x);
    else if (f.name == "asinh") result = std::asinh(x);
    else if (f.name == "acosh") result = std::acosh(x);
    else if (f.name == "atanh") result = std::atanh(x);
    else runtime_error(span, "unknown hyperbolic operation", "E4003");
    if (!std::isfinite(result)) return failure("MATH_OVERFLOW: non-finite result");
    return standard_result(Value(result), true, "float");
}
}