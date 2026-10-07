#include "hua/stdlib.hpp"
#include "hua/tasks.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

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
    if (f.module == "math") return math(f, args, span);
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
