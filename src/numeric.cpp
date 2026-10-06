#include "hua/value.hpp"
#include <charconv>
#include <cmath>
#include <limits>
namespace hua {
using I = std::int64_t;
using U = std::uint64_t;
std::optional<NumericSpec> numeric_spec(std::string_view type) {
  if (type == "int" || type == "i64" || type == "isize")
    return NumericSpec{'i', 64};
  if (type == "float" || type == "f64")
    return NumericSpec{'f', 64};
  if (type == "byte" || type == "u8")
    return NumericSpec{'u', 8};
  if (type == "usize" || type == "u64" || type == "@integer")
    return NumericSpec{'u', 64};
  if (type == "i8")
    return NumericSpec{'i', 8};
  if (type == "i16")
    return NumericSpec{'i', 16};
  if (type == "i32")
    return NumericSpec{'i', 32};
  if (type == "u16")
    return NumericSpec{'u', 16};
  if (type == "u32")
    return NumericSpec{'u', 32};
  if (type == "f32")
    return NumericSpec{'f', 32};
  return {};
}
bool numeric_widening(std::string_view expected, std::string_view actual) {
  auto a = numeric_spec(expected), b = numeric_spec(actual);
  if (!a || !b)
    return false;
  return (a->category == b->category && a->bits >= b->bits) ||
         (a->category == 'i' && b->category == 'u' && a->bits > b->bits);
}
namespace {
U umax(unsigned bits) {
  return bits == 64 ? std::numeric_limits<U>::max() : (U{1} << bits) - 1;
}
I imin(unsigned bits) {
  return bits == 64 ? std::numeric_limits<I>::min()
                    : -static_cast<I>(U{1} << (bits - 1));
}
I imax(unsigned bits) {
  return bits == 64 ? std::numeric_limits<I>::max()
                    : static_cast<I>((U{1} << (bits - 1)) - 1);
}
NumericValue number(const Value &v, const SourceSpan &s) {
  if (auto p = std::get_if<NumericValue>(&v.data))
    return *p;
  if (auto p = std::get_if<I>(&v.data))
    return {"int", *p};
  if (auto p = std::get_if<double>(&v.data))
    return {"float", *p};
  runtime_error(s, "expected a numeric value", "E4003");
}
Value wrap(std::string_view type, std::variant<I, U, double> value) {
  if (type == "int")
    return Value(std::get<I>(value));
  if (type == "float")
    return Value(std::get<double>(value));
  return Value(NumericValue{std::string(type), value});
}
} // namespace
Value convert_numeric(const Value &value, std::string_view type,
                      const SourceSpan &s, bool explicit_conversion) {
  auto spec = numeric_spec(type);
  if (!spec)
    runtime_error(s, "unknown numeric type", "E4003");
  auto source = number(value, s);
  if (!explicit_conversion && !numeric_widening(type, source.type))
    runtime_error(s, "numeric narrowing requires an explicit conversion",
                  "E4003");
  if (spec->category == 'i') {
    I out{};
    if (auto p = std::get_if<I>(&source.number))
      out = *p;
    else if (auto p = std::get_if<U>(&source.number)) {
      if (*p > static_cast<U>(imax(spec->bits)))
        runtime_error(s, "integer conversion overflow", "E4002");
      out = static_cast<I>(*p);
    } else {
      auto x = std::trunc(std::get<double>(source.number));
      if (!std::isfinite(x) || x < static_cast<double>(imin(spec->bits)) ||
          (spec->bits == 64 ? x >= 9223372036854775808.0
                            : x > static_cast<double>(imax(spec->bits))))
        runtime_error(s, "integer conversion overflow", "E4002");
      out = static_cast<I>(x);
    }
    if (out < imin(spec->bits) || out > imax(spec->bits))
      runtime_error(s, "integer conversion overflow", "E4002");
    return wrap(type, out);
  }
  if (spec->category == 'u') {
    U out{};
    if (auto p = std::get_if<U>(&source.number))
      out = *p;
    else if (auto p = std::get_if<I>(&source.number)) {
      if (*p < 0)
        runtime_error(s, "unsigned conversion rejects negative values",
                      "E4002");
      out = static_cast<U>(*p);
    } else {
      auto x = std::trunc(std::get<double>(source.number));
      if (!std::isfinite(x) || x < 0 ||
          (spec->bits == 64 ? x >= 18446744073709551616.0
                            : x > static_cast<double>(umax(spec->bits))))
        runtime_error(s, "unsigned conversion overflow", "E4002");
      out = static_cast<U>(x);
    }
    if (out > umax(spec->bits))
      runtime_error(s, "unsigned conversion overflow", "E4002");
    return wrap(type, out);
  }
  double out =
      std::visit([](auto x) { return static_cast<double>(x); }, source.number);
  if (spec->bits == 32)
    out = static_cast<double>(static_cast<float>(out));
  if (!std::isfinite(out))
    runtime_error(s, "floating conversion overflow", "E4002");
  return wrap(type, out);
}
Value unary_numeric(std::string_view op, const Value &value,
                    const SourceSpan &s) {
  auto n = number(value, s);
  auto spec = *numeric_spec(n.type);
  if (op == "+")
    return value;
  if (spec.category == 'u') {
    auto x = std::get<U>(n.number);
    if (op == "~")
      return wrap(n.type, (~x) & umax(spec.bits));
    if (op == "-" && x == 0)
      return value;
    runtime_error(s, "unsigned negation is not defined", "E4003");
  }
  Value scalar = spec.category == 'i' ? Value(std::get<I>(n.number))
                                      : Value(std::get<double>(n.number));
  return convert_numeric(unary_value(op, scalar, s), n.type, s, true);
}
Value binary_numeric(std::string_view op, const Value &a, const Value &b,
                     const SourceSpan &s) {
  if (a.data.index() == 0 || b.data.index() == 0) {
    if (op == "==" || op == "!=")
      return Value(op == "!=");
    runtime_error(s, "nil is not a numeric operand", "E4003");
  }
  auto left = number(a, s), right = number(b, s);
  auto ls = *numeric_spec(left.type), rs = *numeric_spec(right.type);
  bool compare = op == "==" || op == "!=" || op == "<" || op == "<=" ||
                 op == ">" || op == ">=";
  if (ls.category == 'f' && rs.category == 'i' && op == "**") {
    auto x = convert_numeric(a, "float", s, true),
         y = convert_numeric(b, "int", s, true);
    return convert_numeric(binary_value(op, x, y, s), left.type, s, true);
  }
  std::string target = left.type;
  if (target == "@integer")
    target = "int";
  if (!numeric_widening(target, right.type)) {
    if (numeric_widening(right.type, target))
      target = right.type;
    else if ((op == "<<" || op == ">>") && rs.category != 'f') {
    } else
      runtime_error(s, "mixed numeric categories require explicit conversion",
                    "E4003");
  }
  auto spec = *numeric_spec(target);
  if (spec.category == 'u') {
    auto x = std::get<U>(
        std::get<NumericValue>(convert_numeric(a, target, s, true).data)
            .number);
    auto y = std::visit([](auto v) { return static_cast<U>(v); },
                        number(b, s).number);
    auto limit = umax(spec.bits);
    U z{};
    if (compare) {
      bool answer = op == "=="   ? x == y
                    : op == "!=" ? x != y
                    : op == "<"  ? x < y
                    : op == "<=" ? x <= y
                    : op == ">"  ? x > y
                                 : x >= y;
      return Value(answer);
    }
    if (op == "+") {
      if (y > limit - x)
        runtime_error(s, "unsigned arithmetic overflow", "E4002");
      z = x + y;
    } else if (op == "-") {
      if (y > x)
        runtime_error(s, "unsigned arithmetic underflow", "E4002");
      z = x - y;
    } else if (op == "*") {
      if (y && x > limit / y)
        runtime_error(s, "unsigned arithmetic overflow", "E4002");
      z = x * y;
    } else if (op == "/" || op == "//" || op == "%") {
      if (!y)
        runtime_error(s, "division by zero", "E4004");
      if (op == "/")
        return Value(static_cast<double>(x) / static_cast<double>(y));
      z = op == "//" ? x / y : x % y;
    } else if (op == "&")
      z = x & y;
    else if (op == "|")
      z = x | y;
    else if (op == "^")
      z = x ^ y;
    else if (op == "<<" || op == ">>") {
      if (y >= spec.bits)
        runtime_error(s, "shift count exceeds operand width", "E4005");
      if (op == "<<") {
        if (x > (limit >> y))
          runtime_error(s, "left shift overflow", "E4002");
        z = x << y;
      } else
        z = x >> y;
    } else if (op == "**") {
      z = 1;
      while (y) {
        if (y & 1) {
          if (x && z > limit / x)
            runtime_error(s, "unsigned arithmetic overflow", "E4002");
          z *= x;
        }
        y >>= 1;
        if (y) {
          if (x && x > limit / x)
            runtime_error(s, "unsigned arithmetic overflow", "E4002");
          x *= x;
        }
      }
    } else
      runtime_error(s, "unsupported unsigned operator", "E4003");
    return wrap(target, z);
  }
  if (spec.category == 'i') {
    auto x = convert_numeric(a, "int", s, true),
         y = convert_numeric(b, "int", s, true);
    if (op == "<<" || op == ">>") {
      auto shift = as_int(y, s);
      if (shift < 0 || shift >= spec.bits)
        runtime_error(s, "shift count exceeds operand width", "E4005");
    }
    auto result = binary_value(op, x, y, s);
    if (compare || op == "/")
      return result;
    return convert_numeric(result, target, s, true);
  }
  auto x = convert_numeric(a, "float", s, true),
       y = convert_numeric(b, "float", s, true);
  auto result = binary_value(op, x, y, s);
  if (compare)
    return result;
  return convert_numeric(result, target, s, true);
}
} // namespace hua
