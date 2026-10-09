#pragma once
#include "hua/ast.hpp"
#include "hua/gc.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <set>
#include <vector>
namespace hua {
using TypeRelations=std::map<std::string,std::set<std::string>>;
struct TypeRelationScope { const TypeRelations* previous;explicit TypeRelationScope(const TypeRelations& relations);~TypeRelationScope(); };
std::vector<std::pair<std::string,std::vector<std::string>>> enum_cases(const Node& structure);
struct Value;
struct VoidValue {};
struct TaskState;
struct TaskValue {std::shared_ptr<TaskState> data;};
struct StructData;
struct MapData;
struct MultiData;
struct JsonData;
struct ErrorData;
struct ErrorValue { std::shared_ptr<const ErrorData> data; };
struct ListData;
struct NumericValue { std::string type;std::variant<std::int64_t,std::uint64_t,double> number; };
struct NumericSpec {char category;unsigned bits;};
std::optional<NumericSpec> numeric_spec(std::string_view type);
bool numeric_widening(std::string_view expected,std::string_view actual);
Value convert_numeric(const Value& value,std::string_view type,const SourceSpan& span,bool explicit_conversion=false);
Value binary_numeric(std::string_view op,const Value& left,const Value& right,const SourceSpan& span);
Value unary_numeric(std::string_view op,const Value& value,const SourceSpan& span);
struct BytesValue { std::shared_ptr<const std::string> data; };
struct BufferValue { std::shared_ptr<std::string> data; bool writable{true}; };
struct ListValue { std::shared_ptr<ListData> data; bool writable{true}; };
struct JsonValue { std::shared_ptr<const JsonData> data; };
using MapKey = std::variant<bool, std::int64_t, std::string>;
struct MapValue { std::shared_ptr<MapData> data; bool writable{true}; };
struct MultiValue { std::shared_ptr<MultiData> data; };
struct ResultValue { bool ok{}; std::shared_ptr<Value> payload; std::string success_type, error_type; bool writable{true}, contextual{}; };
struct SliceValue {
    std::shared_ptr<std::vector<Value>> storage;
    std::size_t start{}, length{};
    bool writable{true};
    std::string element_type;
    std::shared_ptr<std::string> packed;
    std::size_t byte_offset{};
    bool fixed{};
    std::vector<bool> contextual;
    SliceValue()=default;
    SliceValue(std::shared_ptr<std::vector<Value>> values,std::size_t offset,std::size_t count,bool write,std::string element):storage(std::move(values)),start(offset),length(count),writable(write),element_type(std::move(element)){}
};
struct StructValue { std::shared_ptr<StructData> data; bool writable{true}; };
struct Callable { const Node* function{}; std::string builtin; std::optional<StructValue> receiver; std::shared_ptr<void> closure;
    Callable(const Node* fn=nullptr,std::string name={},std::optional<StructValue> object={},std::shared_ptr<void> captured={}):function(fn),builtin(std::move(name)),receiver(std::move(object)),closure(std::move(captured)){}
};
struct Value {
    using Data = std::variant<std::monostate, bool, std::int64_t, double, std::string, SliceValue, StructValue, Callable, MapValue, MultiValue, ResultValue, JsonValue, BytesValue, BufferValue, ListValue, NumericValue, TaskValue, ErrorValue, VoidValue>;
    Data data;
    Value() = default;
    template<class T> explicit Value(T value) : data(std::move(value)) {}
};
struct ErrorData {
    std::string domain, code, message, origin, native_domain;
    std::optional<std::int64_t> native_code;
    std::vector<std::string> contexts;
    std::shared_ptr<const ErrorData> cause;
    bool truncated{};
};
struct ListData { std::string element_type; std::vector<Value> values; };
struct StructData { std::string name; std::map<std::string, Value> fields; };
struct MapData { std::string key_type, item_type; std::map<MapKey, Value> entries; };
struct MultiData { std::vector<Value> values; std::vector<bool> contextual; };
struct ValueLocation {
    Value* value{}; std::string type; bool writable{true}; std::shared_ptr<void> owner;
    std::shared_ptr<MapData> map; std::optional<MapKey> key;
    std::optional<SliceValue> sequence;std::size_t index{};mutable Value scratch;
    ValueLocation()=default;
    ValueLocation(Value* slot,std::string annotation,bool write,std::shared_ptr<void> backing={},std::shared_ptr<MapData> mapping={},std::optional<MapKey> mapkey={}):value(slot),type(std::move(annotation)),writable(write),owner(std::move(backing)),map(std::move(mapping)),key(std::move(mapkey)){}
};
std::optional<std::size_t> packed_width(std::string_view type);
Value sequence_read(const SliceValue& slice,std::size_t index);
void sequence_write(const SliceValue& slice,std::size_t index,const Value& value);
SliceValue fixed_array(SliceValue slice,const std::string& element,const SourceSpan& span,bool contextual);
std::size_t value_size(const Value& value,const SourceSpan& span);
std::size_t value_alignment(const Value& value,const SourceSpan& span);
std::vector<std::string> type_arguments(std::string_view type, std::string_view constructor);
Value make_multi(std::vector<Value> values, std::vector<bool> contextual = {});
std::vector<Value> unpack_multi(const Value& value, std::size_t count, const SourceSpan& span);
Value make_map(const std::string& type, const SourceSpan& span);
MapKey map_key(const MapValue& map, const Value& key, const SourceSpan& span);
void map_put(MapValue& map, const Value& key, Value value, const SourceSpan& span, bool contextual = false);
Value index_value(const Value& base, const Value& index, const SourceSpan& span);
ValueLocation index_location(Value base, Value index, const SourceSpan& span);
const Value& location_read(const ValueLocation& where, const SourceSpan& span);
void location_write(const ValueLocation& where, const Value& value);
Value enforce_borrow(Value value,const std::string& type,const SourceSpan& span);
Value enforce_return(Value value, const Node& function, const SourceSpan& span, bool contextual = false);
Value aggregate_member(const Value& value, const std::string& name, const SourceSpan& span);
Value propagated_error(const Value& value);
Value result_payload(const Value& value, bool success, const SourceSpan& span);
std::string declared_name(const Node& node);
std::string type_name(const Node& node);
std::string value_type(const Value& value);
std::string display_type(std::string type);
std::string show(const Value& value);
bool compatible(std::string_view expected, std::string_view actual);
bool numeric_literal(const Node& node);
bool contextual_literal(const Node& node);
Value literal(const Node& node);
Value unary_value(std::string_view op, const Value& value, const SourceSpan& span);
Value binary_value(std::string_view op, const Value& left, const Value& right, const SourceSpan& span, bool contextual_right=false);
Value copy_value(const Value& value, bool deep_slices = false, unsigned depth = 0);
Value read_only(Value value);
Value enforce_type(Value value, const std::string& expected, const SourceSpan& span, bool contextual_number = false);
std::int64_t as_int(const Value& value, const SourceSpan& span);
bool as_bool(const Value& value, const SourceSpan& span);
[[noreturn]] void runtime_error(const SourceSpan& span, std::string message, std::string code = "E4001");
}
