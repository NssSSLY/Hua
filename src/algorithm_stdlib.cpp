#include "hua/stdlib.hpp"
#include "hua/tasks.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>

namespace hua {
namespace {
using I = std::int64_t;
constexpr std::size_t max_items = 1000000, max_text = 16 * 1024 * 1024;
constexpr std::size_t max_work = 32000000;

struct Work {
    RuntimeContext* context;
    const SourceSpan& span;
    std::size_t count{};
    void tick() {
        if (++count > max_work)
            runtime_error(span, "algorithm work limit exceeded (32000000)", "E4099");
        if ((count & 1023) == 1) task_checkpoint(context, span);
    }
};
Value failure(std::string message, const std::string& type) {
    return standard_result(Value(std::move(message)), false, type);
}
double round_even(double x) {
    auto magnitude = std::abs(x);
    auto integral = std::floor(magnitude);
    auto fraction = magnitude - integral;
    if (fraction > 0.5 || (fraction == 0.5 && std::fmod(integral, 2.0) != 0.0))
        integral += 1.0;
    return std::copysign(integral, x);
}
// math P0 (since 12). Integer functions follow the integer operators: overflow is
// E4002 and a negative isqrt input E4003. Float functions keep the Result contract.
std::uint64_t magnitude(I x) { return x < 0 ? std::uint64_t(0) - static_cast<std::uint64_t>(x) : static_cast<std::uint64_t>(x); }
std::uint64_t binary_gcd(std::uint64_t a, std::uint64_t b) {
    if (!a) return b;
    if (!b) return a;
    auto shift = std::countr_zero(a | b);
    a >>= std::countr_zero(a);
    while (b) {
        b >>= std::countr_zero(b);
        if (a > b) std::swap(a, b);
        b -= a;
    }
    return a << shift;
}
Value math_p0(const StandardFunction& f, const std::vector<Value>& args, const SourceSpan& span) {
    const auto& name = f.name;
    if (name == "pi") return Value(std::numbers::pi);
    if (name == "e") return Value(std::numbers::e);
    if (name == "tau") return Value(2 * std::numbers::pi);
    if (name == "gcd" || name == "lcm") {
        auto a = magnitude(std::get<I>(args[0].data)), b = magnitude(std::get<I>(args[1].data));
        auto g = binary_gcd(a, b);
        std::uint64_t result = g;
        if (name == "lcm") {
            if (!a || !b) return Value(I{0});
            auto q = a / g;
            if (b && q > std::numeric_limits<std::uint64_t>::max() / b) runtime_error(span, "math.lcm overflow", "E4002");
            result = q * b;
        }
        if (result > static_cast<std::uint64_t>(std::numeric_limits<I>::max())) runtime_error(span, "math." + name + " overflow", "E4002");
        return Value(static_cast<I>(result));
    }
    if (name == "isqrt") {
        auto n = std::get<I>(args[0].data);
        if (n < 0) runtime_error(span, "math.isqrt requires a non-negative integer", "E4003");
        // The float estimate is within a few units; correct it exactly in 128-bit.
        auto x = static_cast<unsigned __int128>(std::sqrt(static_cast<double>(n)));
        const auto target = static_cast<unsigned __int128>(n);
        while (x * x > target) --x;
        while ((x + 1) * (x + 1) <= target) ++x;
        return Value(static_cast<I>(x));
    }
    for (const auto& arg : args)
        if (!std::isfinite(std::get<double>(arg.data)))
            return failure("MATH_DOMAIN: finite input required", "float");
    double x = std::get<double>(args[0].data), result{};
    if ((name == "asin" || name == "acos") && std::abs(x) > 1.0)
        return failure("MATH_DOMAIN: " + name + " requires -1 <= x <= 1", "float");
    if (name == "asin") result = std::asin(x);
    else if (name == "acos") result = std::acos(x);
    else if (name == "atan") result = std::atan(x);
    else if (name == "atan2") result = std::atan2(x, std::get<double>(args[1].data));
    else if (name == "cbrt") result = std::cbrt(x);
    else if (name == "fmod") {
        auto y = std::get<double>(args[1].data);
        if (y == 0.0) return failure("MATH_DOMAIN: fmod requires y != 0", "float");
        result = std::fmod(x, y);
    }
    else runtime_error(span, "unknown math operation", "E4003");
    if (!std::isfinite(result)) return failure("MATH_OVERFLOW: non-finite result", "float");
    return standard_result(Value(result), true, "float");
}
Value math(const StandardFunction& f, const std::vector<Value>& args, const SourceSpan& span) {
    double x = std::get<double>(args[0].data), result{};
    for (const auto& arg : args)
        if (!std::isfinite(std::get<double>(arg.data)))
            return failure("MATH_DOMAIN: finite input required", "float");
    const auto& name = f.name;
    if ((name == "log" || name == "log2" || name == "log10") && x <= 0.0)
        return failure("MATH_DOMAIN: logarithm requires x > 0", "float");
    if (name == "floor") result = std::floor(x);
    else if (name == "ceil") result = std::ceil(x);
    else if (name == "trunc") result = std::trunc(x);
    else if (name == "round") result = round_even(x);
    else if (name == "sin") result = std::sin(x);
    else if (name == "cos") result = std::cos(x);
    else if (name == "tan") result = std::tan(x);
    else if (name == "exp") result = std::exp(x);
    else if (name == "log") result = std::log(x);
    else if (name == "log2") result = std::log2(x);
    else if (name == "log10") result = std::log10(x);
    else if (name == "hypot") result = std::hypot(x, std::get<double>(args[1].data));
    else runtime_error(span, "unknown math operation", "E4003");
    if (!std::isfinite(result)) return failure("MATH_OVERFLOW: non-finite result", "float");
    return standard_result(Value(result), true, "float");
}
Value item(const SliceValue& input, std::size_t index, Work& work) {
    work.tick();
    auto result = sequence_read(input, index);
    if (value_type(result) != input.element_type)
        runtime_error(work.span, "algorithm element type mismatch", "E4003");
    if (auto text = std::get_if<std::string>(&result.data); text && text->size() > max_text)
        runtime_error(work.span, "algorithm string exceeds 16 MiB", "E4099");
    if (auto real = std::get_if<double>(&result.data); real && !std::isfinite(*real))
        runtime_error(work.span, "algorithm requires finite float values", "E4003");
    return result;
}
int compare(const Value& a, const Value& b, Work& work) {
    work.tick();
    if (auto left = std::get_if<I>(&a.data)) {
        auto right = std::get<I>(b.data);
        return *left < right ? -1 : *left > right ? 1 : 0;
    }
    if (auto left = std::get_if<double>(&a.data)) {
        auto right = std::get<double>(b.data);
        return *left < right ? -1 : *left > right ? 1 : 0;
    }
    const auto& left = std::get<std::string>(a.data);
    const auto& right = std::get<std::string>(b.data);
    for (std::size_t i = 0; i < std::min(left.size(), right.size()); ++i) {
        work.tick();
        auto x = static_cast<unsigned char>(left[i]), y = static_cast<unsigned char>(right[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return left.size() < right.size() ? -1 : left.size() > right.size() ? 1 : 0;
}
std::size_t bound(const SliceValue& input, const Value& target, bool upper, Work& work) {
    std::size_t left = 0, right = input.length;
    while (left < right) {
        auto middle = left + (right - left) / 2;
        auto order = compare(item(input, middle, work), target, work);
        if (order < 0 || (upper && order == 0)) left = middle + 1;
        else right = middle;
    }
    return left;
}
// alg P0 (since 19): unique/chunk/product. Dispatched before the generic argument
// checks, so they check argument kinds themselves.
Value read_only_slice(std::vector<Value> values, const std::string& element) {
    auto storage = managed<std::vector<Value>>(std::move(values));
    auto n = storage->size();
    return Value(SliceValue{std::move(storage), 0, n, false, element});
}
std::uint64_t mix(std::uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    return x ^ (x >> 33);
}
// Elements as one contiguous range: unpacked slices are read in place, packed arrays
// are loaded once.
struct Elements {
    std::vector<Value> loaded;
    const Value* data{};
    Elements(const SliceValue& input) {
        if (input.packed) {
            loaded.reserve(input.length);
            for (std::size_t i = 0; i < input.length; ++i) loaded.push_back(sequence_read(input, i));
            data = loaded.data();
        } else data = input.storage->data() + input.start;
    }
};
// Positions of first occurrences, in order: open addressing over input positions
// (load factor <= 1/2), expected O(n).
template <class Hash, class Same>
std::vector<std::size_t> first_positions(std::size_t n, Hash hash, Same same, Work& work) {
    std::size_t capacity = 16;
    while (capacity < n * 2) capacity *= 2;
    std::vector<std::uint32_t> slots(capacity);
    std::vector<std::size_t> kept;
    const auto mask = capacity - 1;
    for (std::size_t i = 0; i < n; ++i) {
        work.tick();
        for (auto h = mix(hash(i)) & mask;; h = (h + 1) & mask) {
            auto slot = slots[h];
            if (!slot) { slots[h] = static_cast<std::uint32_t>(i + 1); kept.push_back(i); break; }
            if (same(slot - 1, i)) break;
        }
    }
    return kept;
}
const SliceValue& slice_argument(const Value& value, const SourceSpan& span) {
    auto input = std::get_if<SliceValue>(&value.data);
    if (!input) runtime_error(span, "algorithm requires an array or slice", "E4003");
    if (input->length > max_items) runtime_error(span, "algorithm item limit exceeded (1000000)", "E4099");
    return *input;
}
Value unique(const SliceValue& input, Work& work) {
    const auto& type = input.element_type;
    Elements items(input);
    const auto* x = items.data;
    auto mismatch = [&] { runtime_error(work.span, "algorithm element type mismatch", "E4003"); };
    std::vector<std::size_t> kept;
    if (type == "int") {
        for (std::size_t i = 0; i < input.length; ++i) if (!std::holds_alternative<I>(x[i].data)) mismatch();
        kept = first_positions(input.length, [&](std::size_t i) { return static_cast<std::uint64_t>(std::get<I>(x[i].data)); },
                               [&](std::size_t a, std::size_t b) { return std::get<I>(x[a].data) == std::get<I>(x[b].data); }, work);
    } else if (type == "float") {
        for (std::size_t i = 0; i < input.length; ++i) {
            auto real = std::get_if<double>(&x[i].data);
            if (!real) mismatch();
            if (!std::isfinite(*real)) runtime_error(work.span, "algorithm requires finite float values", "E4003");
        }
        // == semantics: -0.0 and 0.0 hash alike (finite inputs, so no NaN).
        kept = first_positions(input.length, [&](std::size_t i) { auto v = std::get<double>(x[i].data); return std::bit_cast<std::uint64_t>(v == 0.0 ? 0.0 : v); },
                               [&](std::size_t a, std::size_t b) { return std::get<double>(x[a].data) == std::get<double>(x[b].data); }, work);
    } else if (type == "string") {
        for (std::size_t i = 0; i < input.length; ++i) {
            auto text = std::get_if<std::string>(&x[i].data);
            if (!text) mismatch();
            work.count += text->size() / 64;
        }
        kept = first_positions(input.length, [&](std::size_t i) { return static_cast<std::uint64_t>(std::hash<std::string_view>{}(std::get<std::string>(x[i].data))); },
                               [&](std::size_t a, std::size_t b) { return std::get<std::string>(x[a].data) == std::get<std::string>(x[b].data); }, work);
    } else runtime_error(work.span, "alg.unique requires int/float/string elements", "E4003");
    std::vector<Value> out;
    out.reserve(kept.size());
    std::size_t text_bytes = 0;
    for (auto i : kept) {
        if (auto text = std::get_if<std::string>(&x[i].data)) {
            if (text->size() > max_text - text_bytes) runtime_error(work.span, "algorithm copied text exceeds 16 MiB", "E4099");
            text_bytes += text->size();
        }
        out.push_back(x[i]);
    }
    return read_only_slice(std::move(out), type);
}
Value chunk(const SliceValue& input, const Value& width, Work& work) {
    auto size = std::get_if<I>(&width.data);
    if (!size) runtime_error(work.span, "alg.chunk size must be an int", "E4003");
    if (*size <= 0) runtime_error(work.span, "alg.chunk size must be positive", "E4003");
    const auto n = input.length, step = static_cast<std::size_t>(std::min<I>(*size, static_cast<I>(max_items)));
    std::vector<Value> blocks;
    blocks.reserve((n + step - 1) / step);
    std::size_t text_bytes = 0;
    for (std::size_t start = 0; start < n; start += step) {
        std::vector<Value> block;
        block.reserve(std::min(step, n - start));
        for (auto i = start; i < std::min(start + step, n); ++i) {
            work.tick();
            auto value = sequence_read(input, i);
            if (auto text = std::get_if<std::string>(&value.data)) {
                if (text->size() > max_text - text_bytes) runtime_error(work.span, "algorithm copied text exceeds 16 MiB", "E4099");
                text_bytes += text->size();
            }
            block.push_back(std::move(value));
        }
        blocks.push_back(read_only_slice(std::move(block), input.element_type));
    }
    return read_only_slice(std::move(blocks), "[]" + input.element_type);
}
Value product(const SliceValue& input, Work& work) {
    const auto& type = input.element_type;
    if (type != "int" && type != "float") runtime_error(work.span, "product requires int/float slice or array", "E4003");
    Elements items(input);
    if (type == "int") {
        I result = 1;
        for (std::size_t i = 0; i < input.length; ++i) {
            work.tick();
            auto x = std::get_if<I>(&items.data[i].data);
            if (!x) runtime_error(work.span, "algorithm element type mismatch", "E4003");
            if (__builtin_mul_overflow(result, *x, &result)) return failure("ALG_OVERFLOW: int64 product overflow", "int");
        }
        return standard_result(Value(result), true, "int");
    }
    double result = 1.0;
    for (std::size_t i = 0; i < input.length; ++i) {
        work.tick();
        auto x = std::get_if<double>(&items.data[i].data);
        if (!x) runtime_error(work.span, "algorithm element type mismatch", "E4003");
        result *= *x;
        if (!std::isfinite(result)) return failure("ALG_OVERFLOW: float product overflow", "float");
    }
    return standard_result(Value(result), true, "float");
}
Value copied(const StandardFunction& f, const SliceValue& input, Work& work) {
    auto values = managed<std::vector<Value>>();
    values->reserve(input.length);
    std::size_t text_bytes = 0;
    for (std::size_t i = 0; i < input.length; ++i) {
        auto value = item(input, f.name == "reverse" ? input.length - i - 1 : i, work);
        if (auto text = std::get_if<std::string>(&value.data)) {
            if (text->size() > max_text - text_bytes)
                runtime_error(work.span, "algorithm copied text exceeds 16 MiB", "E4099");
            text_bytes += text->size();
        }
        values->push_back(std::move(value));
    }
    if (f.name == "sorted") {
        // Bottom-up stable merge: bounded work, fixed order, no recursive native frames.
        std::vector<Value> scratch(input.length);
        for (std::size_t width = 1; width < input.length; width *= 2) {
            for (std::size_t start = 0; start < input.length; start += 2 * width) {
                auto middle = std::min(start + width, input.length);
                auto end = std::min(start + 2 * width, input.length);
                auto left = start, right = middle;
                for (auto out = start; out < end; ++out) {
                    work.tick();
                    if (left < middle && (right == end || compare((*values)[left], (*values)[right], work) <= 0))
                        scratch[out] = std::move((*values)[left++]);
                    else scratch[out] = std::move((*values)[right++]);
                }
            }
            values->swap(scratch);
        }
    }
    return standard_result(Value(SliceValue{values, 0, input.length, false, input.element_type}),
                           true, "[]" + input.element_type);
}
}
Value invoke_algorithm_standard(const StandardFunction& f, const std::vector<Value>& args,
                                const SourceSpan& span, RuntimeContext* context) {
    task_checkpoint(context, span);
    if (f.module == "math") return f.since >= 12 ? math_p0(f, args, span) : math(f, args, span);
    if (f.since >= 19) {
        const auto& input = slice_argument(args[0], span);
        Work work{context, span};
        if (f.name == "unique") return unique(input, work);
        if (f.name == "chunk") return chunk(input, args[1], work);
        return product(input, work);
    }
    auto input = std::get_if<SliceValue>(&args[0].data);
    if (!input || (input->element_type != "int" && input->element_type != "float" && input->element_type != "string"))
        runtime_error(span, "algorithm requires int/float/string slice or array", "E4003");
    if (f.name == "sum" && input->element_type == "string")
        runtime_error(span, "sum requires int/float slice or array", "E4003");
    if (input->length > max_items) runtime_error(span, "algorithm item limit exceeded (1000000)", "E4099");
    Work work{context, span};
    const auto& name = f.name;
    if (args.size() == 2) {
        if (value_type(args[1]) != input->element_type)
            runtime_error(span, "algorithm target type mismatch", "E4003");
        if (auto text = std::get_if<std::string>(&args[1].data); text && text->size() > max_text)
            runtime_error(span, "algorithm target exceeds 16 MiB", "E4099");
        if (auto real = std::get_if<double>(&args[1].data); real && !std::isfinite(*real))
            runtime_error(span, "algorithm requires finite float target", "E4003");
    }
    if (name == "sorted" || name == "reverse") return copied(f, *input, work);
    if (name == "contains" || name == "index") {
        for (std::size_t i = 0; i < input->length; ++i)
            if (compare(item(*input, i, work), args[1], work) == 0)
                return name == "contains" ? Value(true) : Value(static_cast<I>(i));
        return name == "contains" ? Value(false) : Value(I{-1});
    }
    if (name == "lower_bound" || name == "upper_bound" || name == "binary_search") {
        auto index = bound(*input, args[1], name == "upper_bound", work);
        if (name == "binary_search") {
            if (index == input->length || compare(item(*input, index, work), args[1], work) != 0) return {};
        }
        return Value(static_cast<I>(index));
    }
    if (name == "min" || name == "max") {
        if (!input->length) return failure("ALG_EMPTY: min/max require non-empty input", input->element_type);
        auto result = item(*input, 0, work);
        for (std::size_t i = 1; i < input->length; ++i) {
            auto value = item(*input, i, work);
            auto order = compare(value, result, work);
            if ((name == "min" && order < 0) || (name == "max" && order > 0)) result = std::move(value);
        }
        return standard_result(std::move(result), true, input->element_type);
    }
    if (name == "sum") {
        I integer = 0;
        double real = 0.0;
        for (std::size_t i = 0; i < input->length; ++i) {
            auto value = item(*input, i, work);
            if (input->element_type == "int") {
                auto x = std::get<I>(value.data);
                if ((x > 0 && integer > std::numeric_limits<I>::max() - x) ||
                    (x < 0 && integer < std::numeric_limits<I>::min() - x))
                    return failure("ALG_OVERFLOW: int64 sum overflow", "int");
                integer += x;
            } else {
                real += std::get<double>(value.data);
                if (!std::isfinite(real)) return failure("ALG_OVERFLOW: float sum overflow", "float");
            }
        }
        return standard_result(input->element_type == "int" ? Value(integer) : Value(real),
                               true, input->element_type);
    }
    runtime_error(span, "unknown algorithm operation", "E4003");
}
}
