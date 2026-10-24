#pragma once
#include "hua/ast.hpp"
#include "hua/gc.hpp"
#include <cstdint>
#include <atomic>
#include <mutex>
#include <array>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <set>
#include <unordered_set>
#include <vector>
namespace hua {
using TypeRelations=std::map<std::string,std::set<std::string>>;
struct TypeRelationScope { const TypeRelations* previous;explicit TypeRelationScope(const TypeRelations& relations);~TypeRelationScope(); };
std::vector<std::pair<std::string,std::vector<std::string>>> enum_cases(const Node& structure);
struct Value;
struct PlainRecordLayout;
struct VoidValue {};
struct TaskState;
struct TaskValue {std::shared_ptr<TaskState> data;};
struct StructData;
struct EnumLayout;struct EnumVariant;
struct MapData;
struct MultiData;
struct JsonData;
struct ErrorData;
struct FileData;struct IOFailureData;
struct FileValue {std::shared_ptr<FileData> data;};
struct IOFailureValue {std::shared_ptr<const IOFailureData> data;};
struct ErrorValue { std::shared_ptr<const ErrorData> data; };
struct ListData;
struct DequeData;
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
// std.os.exit (since 16): unwinds like a return through every pending defer and the
// entry's Task/File cleanup; command_boundary turns it into the process exit code.
struct ProgramExit { int code{}; };
inline bool is_program_exit(const std::exception_ptr& error){if(!error)return false;try{std::rethrow_exception(error);}catch(const ProgramExit&){return true;}catch(...){return false;}}
struct DequeValue { std::shared_ptr<DequeData> data; bool writable{true}; };
// std.random.Rng (since 13): xoshiro256** state behind a shared handle, like Buffer.
struct RngState { std::uint64_t s[4]{}; };
struct RngValue { std::shared_ptr<RngState> data; bool writable{true}; };
struct JsonValue { std::shared_ptr<const JsonData> data; };
using MapKey = std::variant<bool, std::int64_t, std::string>;
struct MapValue { std::shared_ptr<MapData> data; bool writable{true}; };
struct MultiValue { std::shared_ptr<MultiData> data; };
struct ResultValue { bool ok{}; std::shared_ptr<Value> payload; std::string success_type, error_type; bool writable{true}, contextual{}; std::shared_ptr<const SourceTrace> failure_source{}; };
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
// Eight scalar cells cover common identifier/span records without widening
// the existing SliceValue-sized Value storage. Nominal/type metadata is owned.
struct PlainRecord {std::shared_ptr<const PlainRecordLayout> layout;std::array<std::uint64_t,8> cells{};bool writable{true};};
struct StructValue { std::shared_ptr<StructData> data; bool writable{true}; };
// Builtin names are interned for the process lifetime: function values are
// loaded and copied very often, and copying the name is then a pointer copy.
class BuiltinName {
    const std::string* text_;
    static const std::string* intern(std::string text);
public:
    BuiltinName();
    explicit BuiltinName(std::string text):text_(intern(std::move(text))){}
    const std::string& str() const {return *text_;}
    operator const std::string&() const {return *text_;}
    bool empty() const {return text_->empty();}
    friend bool operator==(const BuiltinName& a,std::string_view b){return *a.text_==b;}
};
struct Callable { const Node* function{}; BuiltinName builtin; std::optional<StructValue> receiver; std::shared_ptr<void> closure;std::function<bool(unsigned)> resource_guard;
    Callable(const Node* fn=nullptr,std::string name={},std::optional<StructValue> object={},std::shared_ptr<void> captured={}):function(fn),builtin(std::move(name)),receiver(std::move(object)),closure(std::move(captured)){}
};
struct Value {
    using Data = std::variant<std::monostate, bool, std::int64_t, double, std::string, SliceValue, StructValue, Callable, MapValue, MultiValue, ResultValue, JsonValue, BytesValue, BufferValue, ListValue, NumericValue, TaskValue, ErrorValue, VoidValue, FileValue, IOFailureValue, PlainRecord, RngValue, DequeValue>;
    Data data;
    Value() = default;
    template<class T> explicit Value(T value) : data(std::move(value)) {}
};
// Names and field positions are immutable and own their storage independently of a model.
struct RecordLayout {
    const std::vector<std::string> names;
    const std::string nominal_name;
    const std::map<std::string,std::string> annotations;
    mutable std::mutex plain_mutex;
    mutable std::weak_ptr<const PlainRecordLayout> plain_cache;
    mutable std::atomic<bool> plain_rejected{false};
    explicit RecordLayout(std::vector<std::string> fields,std::string name={},std::map<std::string,std::string> types={});
    static constexpr std::size_t missing=static_cast<std::size_t>(-1);
    std::size_t index(const std::string& name) const;
};
struct PlainRecordLayout {
    struct Field {std::string name,type;unsigned offset{},tag{};std::shared_ptr<const PlainRecordLayout> child;};
    std::string name;std::shared_ptr<const RecordLayout> shape;std::vector<Field> fields;unsigned slots{},depth{};
};
bool pack_plain_record(const StructValue&,PlainRecord&,unsigned depth=0);
std::optional<Value> plain_record_field(const PlainRecord&,const std::string&);
StructValue* materialize_record(Value&);
void retain_record_alias(Value&);
std::shared_ptr<const RecordLayout> record_layout(const Node& declaration);
class RecordFields {
    using Tree=std::map<std::string,Value>;
    struct Flat {
        std::shared_ptr<const RecordLayout> layout;
        std::vector<std::optional<Value>> slots;
        std::size_t count{};
        explicit Flat(std::shared_ptr<const RecordLayout> shape):layout(std::move(shape)),slots(layout->names.size()){}
    };
    std::variant<Tree,Flat> storage_;
public:
    template<bool Const> class Iterator {
        using Owner=std::conditional_t<Const,const RecordFields,RecordFields>;
        using TreeIterator=std::conditional_t<Const,Tree::const_iterator,Tree::iterator>;
        Owner* owner_{};std::size_t position_{};TreeIterator tree_{};
        friend class RecordFields;
        Iterator(Owner* owner,std::size_t position):owner_(owner),position_(position){skip();}
        Iterator(Owner* owner,TreeIterator tree):owner_(owner),tree_(tree){}
        void skip(){if(auto* f=std::get_if<Flat>(&owner_->storage_))while(position_<f->slots.size()&&!f->slots[position_])++position_;}
    public:
        using reference=std::pair<const std::string&,std::conditional_t<Const,const Value&,Value&>>;
        struct Arrow {reference entry;const reference* operator->()const{return &entry;}};
        Iterator()=default;
        reference operator*()const{
            if(auto* f=std::get_if<Flat>(&owner_->storage_))return {f->layout->names[position_],*f->slots[position_]};
            return {tree_->first,tree_->second};
        }
        Arrow operator->()const{return {**this};}
        Iterator& operator++(){if(std::holds_alternative<Flat>(owner_->storage_)){++position_;skip();}else ++tree_;return *this;}
        bool operator==(const Iterator& other)const{
            if(owner_!=other.owner_)return false;
            if(!owner_)return true;
            return std::holds_alternative<Flat>(owner_->storage_)?position_==other.position_:tree_==other.tree_;
        }
    };
    using iterator=Iterator<false>;using const_iterator=Iterator<true>;
    RecordFields()=default;
    explicit RecordFields(std::shared_ptr<const RecordLayout> layout):storage_(std::in_place_type<Flat>,std::move(layout)){}
    const RecordLayout* layout()const{auto* f=std::get_if<Flat>(&storage_);return f?f->layout.get():nullptr;}
    std::shared_ptr<const RecordLayout> retain_layout()const{auto* f=std::get_if<Flat>(&storage_);return f?f->layout:nullptr;}
    RecordFields empty_copy()const{auto* f=std::get_if<Flat>(&storage_);return f?RecordFields(f->layout):RecordFields();}
    Value* value_at(std::size_t index){auto* f=std::get_if<Flat>(&storage_);return f&&index<f->slots.size()&&f->slots[index]?&*f->slots[index]:nullptr;}
    const Value* value_at(std::size_t index)const{auto* f=std::get_if<Flat>(&storage_);return f&&index<f->slots.size()&&f->slots[index]?&*f->slots[index]:nullptr;}
    iterator begin(){if(std::holds_alternative<Flat>(storage_))return iterator(this,std::size_t{0});return iterator(this,std::get<Tree>(storage_).begin());}
    iterator end(){if(auto* f=std::get_if<Flat>(&storage_))return iterator(this,f->slots.size());return iterator(this,std::get<Tree>(storage_).end());}
    const_iterator begin()const{if(std::holds_alternative<Flat>(storage_))return const_iterator(this,std::size_t{0});return const_iterator(this,std::get<Tree>(storage_).begin());}
    const_iterator end()const{if(auto* f=std::get_if<Flat>(&storage_))return const_iterator(this,f->slots.size());return const_iterator(this,std::get<Tree>(storage_).end());}
    iterator find(const std::string& name);
    const_iterator find(const std::string& name)const;
    bool contains(const std::string& name)const{return find(name)!=end();}
    Value& at(const std::string& name);
    const Value& at(const std::string& name)const;
    std::pair<iterator,bool> emplace(const std::string& name,Value value);
    iterator emplace_hint(iterator,const std::string& name,Value value){if(auto* tree=std::get_if<Tree>(&storage_))return iterator(this,tree->emplace_hint(tree->end(),name,std::move(value)));return emplace(name,std::move(value)).first;}
    iterator erase(iterator where);
    std::size_t size()const{auto* f=std::get_if<Flat>(&storage_);return f?f->count:std::get<Tree>(storage_).size();}
    bool empty()const{return size()==0;}
    void clear(){if(auto* f=std::get_if<Flat>(&storage_)){f->slots.clear();f->count=0;}else std::get<Tree>(storage_).clear();}
    template<class F> RecordFields transform(F&& copy)const{
        auto result=empty_copy();
        if(auto* f=std::get_if<Flat>(&storage_)){
            auto& target=std::get<Flat>(result.storage_);
            for(std::size_t i=0;i<f->slots.size();++i)if(f->slots[i]){target.slots[i].emplace(copy(*f->slots[i]));++target.count;}
        }else{
            auto& target=std::get<Tree>(result.storage_);
            for(const auto& [name,value]:std::get<Tree>(storage_))target.emplace_hint(target.end(),name,copy(value));
        }
        return result;
    }
    template<class F> void each_value(F&& visit){
        if(auto* f=std::get_if<Flat>(&storage_)){for(auto& slot:f->slots)if(slot)visit(*slot);}
        else for(auto& [_,value]:std::get<Tree>(storage_))visit(value);
    }
    template<class F> void each_value(F&& visit)const{
        if(auto* f=std::get_if<Flat>(&storage_)){for(const auto& slot:f->slots)if(slot)visit(*slot);}
        else for(const auto& [_,value]:std::get<Tree>(storage_))visit(value);
    }
};
struct ErrorData {
    std::string domain, code, message, origin, native_domain;
    std::optional<std::int64_t> native_code;
    std::vector<std::string> contexts;
    std::shared_ptr<const ErrorData> cause;
    bool truncated{};
};
struct IOFailureData {ErrorValue error;std::optional<std::int64_t> transferred;std::optional<BytesValue> partial;};
// revision changes on every content mutation (std.list writers); in-place
// callback functions use it to detect a callback that changed the same List.
struct ListData { std::string element_type; std::vector<Value> values; std::uint64_t revision{}; };
// std.deque Deque<T> (since 14): List rules with O(1) operations at both ends.
struct DequeData { std::string element_type; std::deque<Value> values; };
struct StructData {
    std::string name; RecordFields fields;
    std::shared_ptr<const EnumLayout> enumeration;
    const EnumVariant* variant{}; // Owned by enumeration, never points into an AST/model.
};
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
// Internal copy/scanner source: does not publish a borrowed aggregate alias.
Value sequence_copy_source(const SliceValue& slice,std::size_t index);
void sequence_write(const SliceValue& slice,std::size_t index,const Value& value);
SliceValue fixed_array(SliceValue slice,const std::string& element,const SourceSpan& span,bool contextual);
std::size_t value_size(const Value& value,const SourceSpan& span);
std::size_t value_alignment(const Value& value,const SourceSpan& span);
std::vector<std::string> type_arguments(std::string_view type, std::string_view constructor);
Value make_multi(std::vector<Value> values, std::vector<bool> contextual = {});
std::vector<Value> unpack_multi(const Value& value, std::size_t count, const SourceSpan& span);
Value make_map(const std::string& type, const SourceSpan& span);
MapKey map_key(const MapValue& map, const Value& key, const SourceSpan& span);
// Single lookup entry for reads (m[k], has, delete, maps.get_or/pop).
std::map<MapKey, Value>::iterator map_find(const MapValue& map, const Value& key, const SourceSpan& span);
void map_put(MapValue& map, const Value& key, Value value, const SourceSpan& span, bool contextual = false);
Value index_value(const Value& base, const Value& index, const SourceSpan& span);
ValueLocation index_location(Value base, Value index, const SourceSpan& span);
const Value& location_read(const ValueLocation& where, const SourceSpan& span);
void location_write(const ValueLocation& where, const Value& value);
Value enforce_borrow(Value value,const std::string& type,const SourceSpan& span);
Value enforce_return(Value value, const Node& function, const SourceSpan& span, bool contextual = false);
Value enforce_return(Value value, const std::vector<std::string>& types, const SourceSpan& span, bool contextual = false);
Value aggregate_member(const Value& value, const std::string& name, const SourceSpan& span);
Value record_failure_source(Value,const SourceSpan&,std::string message="Result error created here");
Value propagated_error(const Value& value,const SourceSpan& span={});
Value result_payload(const Value& value, bool success, const SourceSpan& span);
std::string declared_name(const Node& node);
std::optional<std::uint64_t> fixed_extent(std::string_view text);
std::string canonical_type(std::string_view type);
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
bool contains_file(const Value&,unsigned depth=0);
Value copy_value(const Value& value, bool deep_slices = false, unsigned depth = 0, bool resource_alias = false);
Value copy_value(Value&& value, bool deep_slices = false, unsigned depth = 0, bool resource_alias = false);
Value read_only(Value value);
Value enforce_type(Value value, const std::string& expected, const SourceSpan& span, bool contextual_number = false);
std::int64_t as_int(const Value& value, const SourceSpan& span);
bool as_bool(const Value& value, const SourceSpan& span);
[[noreturn]] void runtime_error(const SourceSpan& span, std::string message, std::string code = "E4001");
}
