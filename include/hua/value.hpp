#pragma once
#include "hua/ast.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>
namespace hua {
struct Value;
struct StructData;
struct SliceValue {
    std::shared_ptr<std::vector<Value>> storage;
    std::size_t start{}, length{};
    bool writable{true};
    std::string element_type;
};
struct StructValue { std::shared_ptr<StructData> data; bool writable{true}; };
struct Callable { const Node* function{}; std::string builtin; std::optional<StructValue> receiver; };
struct Value {
    using Data = std::variant<std::monostate, bool, std::int64_t, double, std::string, SliceValue, StructValue, Callable>;
    Data data;
    Value() = default;
    template<class T> explicit Value(T value) : data(std::move(value)) {}
};
struct StructData { std::string name; std::map<std::string, Value> fields; };
std::string declared_name(const Node& node);
std::string type_name(const Node& node);
std::string value_type(const Value& value);
std::string display_type(std::string type);
std::string show(const Value& value);
bool compatible(std::string_view expected, std::string_view actual);
bool numeric_literal(const Node& node);
Value literal(const Node& node);
Value unary_value(std::string_view op, const Value& value, const SourceSpan& span);
Value binary_value(std::string_view op, const Value& left, const Value& right, const SourceSpan& span);
Value copy_value(const Value& value, bool deep_slices = false, unsigned depth = 0);
Value read_only(Value value);
Value enforce_type(Value value, const std::string& expected, const SourceSpan& span, bool contextual_number = false);
std::int64_t as_int(const Value& value, const SourceSpan& span);
bool as_bool(const Value& value, const SourceSpan& span);
[[noreturn]] void runtime_error(const SourceSpan& span, std::string message, std::string code = "E4001");
}
