#pragma once
#include "hua/ast.hpp"
#include "hua/gc.hpp"
#include "hua/tagged.hpp"
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
#include <type_traits>
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
// V1 copy-on-write. A struct body reached only through write paths is mutated in place;
// copies made by copy_value inside the VM share the body instead (share_ref, counted in
// StructData::shares). A write path pins every body on it (PinPath) so that a copy taken
// while the write is pending, or during a mut borrow / mutating method call, is eager.
// fields_readonly: a writable copy of a read-only source whose reference-typed fields
// must still read as read-only (A07-1); it is materialized before any write.
struct PinPath;
struct StructValue {
    std::shared_ptr<StructData> data; bool writable{true};
    bool fields_readonly{false}, share_ref{false};
    std::shared_ptr<PinPath> path;
    StructValue()=default;
    StructValue(std::shared_ptr<StructData> body,bool write=true):data(std::move(body)),writable(write){}
    // Every handle copied from a share is itself counted: StructData::shares is the number of
    // live share handles, so a share that changes hands (copy, then the original dies) is kept.
    StructValue(const StructValue& o):data(o.data),writable(o.writable),fields_readonly(o.fields_readonly),share_ref(o.share_ref),path(o.path){acquire();}
    StructValue(StructValue&& o)noexcept:data(std::move(o.data)),writable(o.writable),fields_readonly(o.fields_readonly),share_ref(o.share_ref),path(std::move(o.path)){o.share_ref=false;}
    StructValue& operator=(const StructValue& o){if(this!=&o){release();data=o.data;writable=o.writable;fields_readonly=o.fields_readonly;share_ref=o.share_ref;path=o.path;acquire();}return *this;}
    StructValue& operator=(StructValue&& o)noexcept{if(this!=&o){release();data=std::move(o.data);writable=o.writable;fields_readonly=o.fields_readonly;share_ref=o.share_ref;o.share_ref=false;path=std::move(o.path);}return *this;}
    ~StructValue(){release();}
    inline void release()noexcept;
    inline void acquire()noexcept;
};
// Copy policy: true while a VirtualMachine runs on this thread (AST interpreter: eager).
inline thread_local bool cow_values=false;
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
// V6: a plain function value is three pointers (copy without atomics); the rarely present
// receiver/closure/resource guard live in a shared, immutable extra (set_* copies it).
struct CallableExtra { std::optional<StructValue> receiver; std::shared_ptr<void> closure; std::function<bool(unsigned)> resource_guard; };
struct Callable { const Node* function{}; BuiltinName builtin; std::shared_ptr<const CallableExtra> extra;
    Callable(const Node* fn=nullptr,std::string name={},std::optional<StructValue> object={},std::shared_ptr<void> captured={}):function(fn),builtin(std::move(name)){
        if(object||captured){auto e=std::make_shared<CallableExtra>();e->receiver=std::move(object);e->closure=std::move(captured);extra=std::move(e);}
    }
    const std::optional<StructValue>& receiver()const{static const std::optional<StructValue> none;return extra?extra->receiver:none;}
    const std::shared_ptr<void>& closure()const{static const std::shared_ptr<void> none;return extra?extra->closure:none;}
    const std::function<bool(unsigned)>& resource_guard()const{static const std::function<bool(unsigned)> none;return extra?extra->resource_guard:none;}
    void set_receiver(std::optional<StructValue> value){edit().receiver=std::move(value);}
    void set_closure(std::shared_ptr<void> value){edit().closure=std::move(value);}
    void set_resource_guard(std::function<bool(unsigned)> value){edit().resource_guard=std::move(value);}
private:
    // An unshared extra (just created by this handle) is edited in place.
    CallableExtra& edit(){if(extra&&extra.use_count()==1)return const_cast<CallableExtra&>(*extra);auto e=extra?std::make_shared<CallableExtra>(*extra):std::make_shared<CallableExtra>();auto& out=*e;extra=std::move(e);return out;}
};
// Language strings are immutable. Short text stays inline; long text is shared
// across loads, calls, snapshots and terminal Task copies. No writable host view.
class StringValue {
    std::shared_ptr<const std::string> shared_;
public:
    explicit StringValue(std::string text):shared_(std::make_shared<const std::string>(std::move(text))){}
    const std::string& text()const{return *shared_;}
};
// V6: the large alternatives (Slice/Result/PlainRecord, 88-120 bytes) are held by a
// copy-on-write handle so a Value is 48 bytes. Copies share the body; cget* reads it and
// mget* unshares it first, so each handle still behaves as an independent value. A
// moved-from handle reads as a default T.
template<class T> class Cow {
    std::shared_ptr<T> p_;
    static const T& empty(){static const T value{};return value;}
public:
    Cow()=default;
    Cow(T value):p_(std::make_shared<T>(std::move(value))){}
    const T& get()const{return p_?*p_:empty();}
    // A body shared by several Values: one handle reaches the objects it names, so the GC
    // cannot attribute those references to one owner (see heap_edges).
    bool shared()const{return p_&&p_.use_count()>1;}
    T& mut(){if(!p_)p_=std::make_shared<T>();else if(p_.use_count()!=1)p_=std::make_shared<T>(*p_);return *p_;}
    const T* operator->()const{return &get();}
    const T& operator*()const{return get();}
};
struct Value {
    using Data = Tagged<std::monostate, bool, std::int64_t, double, std::string, Cow<SliceValue>, StructValue, Callable, MapValue, MultiValue, Cow<ResultValue>, JsonValue, BytesValue, BufferValue, ListValue, NumericValue, TaskValue, ErrorValue, VoidValue, FileValue, IOFailureValue, PlainRecord, RngValue, DequeValue, StringValue>;
    Data data;
    Value() = default;
    explicit Value(const char* text):Value(std::string(text)){}
    explicit Value(std::string text){
        if(text.size()<=std::string{}.capacity())data=std::move(text);
        else data=StringValue(std::move(text));
    }
    template<class T> explicit Value(T value) : data(std::move(value)) {}
};
template<class T> struct is_cow_alternative:std::bool_constant<std::is_same_v<T,SliceValue>||std::is_same_v<T,ResultValue>>{};
template<class T> using data_alternative=std::conditional_t<is_cow_alternative<T>::value,Cow<T>,T>;
// Read access to any alternative (works for all, boxed or not).
template<class T> const T* cget_if(const Value::Data* d){if constexpr(is_cow_alternative<T>::value){auto b=hua::get_if<Cow<T>>(d);return b?&b->get():nullptr;}else return hua::get_if<T>(d);}
template<class T> const T& cget(const Value::Data& d){if constexpr(is_cow_alternative<T>::value)return hua::get<Cow<T>>(d).get();else return hua::get<T>(d);}
// Write access: unshares a boxed body first.
template<class T> T* mget_if(Value::Data* d){if constexpr(is_cow_alternative<T>::value){auto b=hua::get_if<Cow<T>>(d);return b?&b->mut():nullptr;}else return hua::get_if<T>(d);}
template<class T> T& mget(Value::Data& d){if constexpr(is_cow_alternative<T>::value)return hua::get<Cow<T>>(d).mut();else return hua::get<T>(d);}
template<class T> bool holds(const Value::Data& d){return hua::holds_alternative<data_alternative<T>>(d);}
inline const std::string& string_value(const Value::Data& data){if(auto p=hua::get_if<std::string>(&data))return *p;return hua::get<StringValue>(data).text();}
inline const std::string* string_data(const Value::Data* data){if(auto p=hua::get_if<std::string>(data))return p;auto p=hua::get_if<StringValue>(data);return p?&p->text():nullptr;}
inline std::size_t value_kind(const Value& value){auto kind=value.data.index();return kind==24?4:kind;}
inline bool is_string(const Value::Data& data){return data.index()==4||data.index()==24;}
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
        bool exposed{},cycle_free{};
        mutable std::atomic<bool> tracked{false};
        mutable std::atomic<unsigned> copy_depth{0};
        explicit Flat(std::shared_ptr<const RecordLayout> shape,bool initialize=true):layout(std::move(shape)),slots(initialize?layout->names.size():0){}
        Flat(const Flat& other):layout(other.layout),slots(other.slots),count(other.count),exposed(other.exposed),cycle_free(other.cycle_free),copy_depth(other.copy_depth.load()){}
        Flat(Flat&& other)noexcept:layout(std::move(other.layout)),slots(std::move(other.slots)),count(other.count),exposed(other.exposed),cycle_free(other.cycle_free),copy_depth(other.copy_depth.load()){}
        Flat& operator=(Flat&& other)noexcept{layout=std::move(other.layout);slots=std::move(other.slots);count=other.count;exposed=other.exposed;cycle_free=other.cycle_free;copy_depth=other.copy_depth.load();tracked=false;return *this;}
        Flat& operator=(const Flat& other){Flat copy(other);return *this=std::move(copy);}
        void clear(){slots.clear();count=0;copy_depth=0;}
        friend void heap_edges(const Flat& f,const HeapVisitor& visit){for(const auto& slot:f.slots)if(slot)heap_edges(*slot,visit);}
    };
    mutable std::variant<Tree,Flat,std::shared_ptr<Flat>> storage_;
    Flat* flat()const{if(auto p=hua::get_if<Flat>(&storage_))return p;auto p=hua::get_if<std::shared_ptr<Flat>>(&storage_);return p?p->get():nullptr;}
    void detach();
    void expose(){detach();if(auto f=flat()){f->exposed=true;f->copy_depth=0;}}
public:
    template<bool Const> class Iterator {
        using Owner=std::conditional_t<Const,const RecordFields,RecordFields>;
        using TreeIterator=std::conditional_t<Const,Tree::const_iterator,Tree::iterator>;
        Owner* owner_{};std::size_t position_{};TreeIterator tree_{};
        friend class RecordFields;
        Iterator(Owner* owner,std::size_t position):owner_(owner),position_(position){skip();}
        Iterator(Owner* owner,TreeIterator tree):owner_(owner),tree_(tree){}
        void skip(){if(auto* f=owner_->flat())while(position_<f->slots.size()&&!f->slots[position_])++position_;}
    public:
        using reference=std::pair<const std::string&,std::conditional_t<Const,const Value&,Value&>>;
        struct Arrow {reference entry;const reference* operator->()const{return &entry;}};
        Iterator()=default;
        reference operator*()const{
            if constexpr(!Const)owner_->expose();
            if(auto* f=owner_->flat())return {f->layout->names[position_],*f->slots[position_]};
            return {tree_->first,tree_->second};
        }
        Arrow operator->()const{return {**this};}
        Iterator& operator++(){if(owner_->flat()!=nullptr){++position_;skip();}else ++tree_;return *this;}
        bool operator==(const Iterator& other)const{
            if(owner_!=other.owner_)return false;
            if(!owner_)return true;
            return owner_->flat()!=nullptr?position_==other.position_:tree_==other.tree_;
        }
    };
    using iterator=Iterator<false>;using const_iterator=Iterator<true>;
    RecordFields()=default;
    explicit RecordFields(std::shared_ptr<const RecordLayout> layout):storage_(std::in_place_type<Flat>,std::move(layout)){}
    const RecordLayout* layout()const{auto* f=flat();return f?f->layout.get():nullptr;}
    std::shared_ptr<const RecordLayout> retain_layout()const{auto* f=flat();return f?f->layout:nullptr;}
    RecordFields empty_copy()const{auto* f=flat();return f?RecordFields(f->layout):RecordFields();}
    Value* value_at(std::size_t index){expose();auto* f=flat();return f&&index<f->slots.size()&&f->slots[index]?&*f->slots[index]:nullptr;}
    const Value* value_at(std::size_t index)const{auto* f=flat();return f&&index<f->slots.size()&&f->slots[index]?&*f->slots[index]:nullptr;}
    iterator begin(){detach();if(flat()!=nullptr)return iterator(this,std::size_t{0});return iterator(this,hua::get<Tree>(storage_).begin());}
    iterator end(){detach();if(auto* f=flat())return iterator(this,f->slots.size());return iterator(this,hua::get<Tree>(storage_).end());}
    const_iterator begin()const{if(flat()!=nullptr)return const_iterator(this,std::size_t{0});return const_iterator(this,hua::get<Tree>(storage_).begin());}
    const_iterator end()const{if(auto* f=flat())return const_iterator(this,f->slots.size());return const_iterator(this,hua::get<Tree>(storage_).end());}
    iterator find(const std::string& name);
    const_iterator find(const std::string& name)const;
    bool contains(const std::string& name)const{return find(name)!=end();}
    Value& at(const std::string& name);
    const Value& at(const std::string& name)const;
    std::pair<iterator,bool> emplace(const std::string& name,Value value);
    iterator emplace_hint(iterator,const std::string& name,Value value){detach();if(auto* tree=hua::get_if<Tree>(&storage_))return iterator(this,tree->emplace_hint(tree->end(),name,std::move(value)));return emplace(name,std::move(value)).first;}
    iterator erase(iterator where);
    std::size_t size()const{auto* f=flat();return f?f->count:hua::get<Tree>(storage_).size();}
    bool empty()const{return size()==0;}
    void clear(){if(auto* f=flat()){if(auto p=hua::get_if<std::shared_ptr<Flat>>(&storage_);p&&p->use_count()!=1)storage_=Flat(f->layout,false);else f->clear();}else hua::get<Tree>(storage_).clear();}
    // Sharing is allowed only until a mutable field reference escapes. Such a
    // reference can outlive a later snapshot, so exposed bodies use eager copies.
    RecordFields snapshot(unsigned depth)const;
    const void* backing_identity()const{return flat();}
    friend void heap_edges(const RecordFields& f,const HeapVisitor& visit){
        if(auto body=f.flat();body&&body->cycle_free)return;
        if(auto body=f.flat();body&&body->tracked.load(std::memory_order_acquire))visit(body);
        else f.each_value([&](const Value& v){heap_edges(v,visit);});
    }
    template<class F> RecordFields transform(F&& copy)const{
        auto result=empty_copy();
        if(auto* f=flat()){
            auto& target=*result.flat();
            for(std::size_t i=0;i<f->slots.size();++i)if(f->slots[i]){target.slots[i].emplace(copy(*f->slots[i]));++target.count;}
        }else{
            auto& target=hua::get<Tree>(result.storage_);
            for(const auto& [name,value]:hua::get<Tree>(storage_))target.emplace_hint(target.end(),name,copy(value));
        }
        return result;
    }
    // Internal consuming normalization does not publish field references.
    void normalize_owned(bool readonly,unsigned depth,bool resource_alias);
    template<class F> void each_value(F&& visit){
        expose();
        if(auto* f=flat()){for(auto& slot:f->slots)if(slot)visit(*slot);}
        else for(auto& [_,value]:hua::get<Tree>(storage_))visit(value);
    }
    template<class F> void each_value(F&& visit)const{
        if(auto* f=flat()){for(const auto& slot:f->slots)if(slot)visit(*slot);}
        else for(const auto& [_,value]:hua::get<Tree>(storage_))visit(value);
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
    // V1 COW bookkeeping (see StructValue). no_share: a long-lived alias (stored mutating
    // method value, slice view of a fixed-array field) exists, so copies are always eager.
    std::uint32_t pins{},shares{};bool no_share{};
};
inline void StructValue::release()noexcept{if(share_ref){if(data&&data->shares)--data->shares;share_ref=false;}}
inline void StructValue::acquire()noexcept{if(share_ref){if(data)++data->shares;else share_ref=false;}}
struct PinPath {
    // Write paths are short (x.a.b = v pins three bodies): the first few sit inline, so a
    // path costs one allocation (itself) instead of two.
    std::array<std::shared_ptr<StructData>,4> first;std::uint8_t count{};
    std::vector<std::shared_ptr<StructData>> rest;
    void add(const std::shared_ptr<StructData>& body){++body->pins;if(count<first.size())first[count++]=body;else rest.push_back(body);}
    template<class F> void each(F&& f){for(std::uint8_t i=0;i<count;++i)f(first[i]);for(auto& body:rest)f(body);}
    ~PinPath(){each([](auto& body){--body->pins;});}
};
}
#include "hua/map_entries.hpp"
namespace hua {
struct MapData { std::string key_type, item_type; MapEntries entries; };
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
// Cached per thread; the reference stays valid for the thread's lifetime.
const std::vector<std::string>& type_arguments(std::string_view type, std::string_view constructor);
Value make_multi(std::vector<Value> values, std::vector<bool> contextual = {});
std::vector<Value> unpack_multi(const Value& value, std::size_t count, const SourceSpan& span);
Value make_map(const std::string& type, const SourceSpan& span);
MapKey map_key(const MapValue& map, const Value& key, const SourceSpan& span);
// Single lookup entry for reads (m[k], has, delete, maps.get_or/pop).
MapEntries::iterator map_find(const MapValue& map, const Value& key, const SourceSpan& span);
void map_put(MapValue& map, const Value& key, Value value, const SourceSpan& span, bool contextual = false);
Value index_value(const Value& base, const Value& index, const SourceSpan& span);
ValueLocation index_location(Value base, Value index, const SourceSpan& span);
const Value& location_read(const ValueLocation& where, const SourceSpan& span);
void location_write(const ValueLocation& where, const Value& value);
Value enforce_borrow(Value value,const std::string& type,const SourceSpan& span);
Value enforce_return(Value value, const Node& function, const SourceSpan& span, bool contextual = false);
Value enforce_return(Value value, const std::vector<std::string>& types, const SourceSpan& span, bool contextual = false);
Value aggregate_member(const Value& value, const std::string& name, const SourceSpan& span);
Value record_failure_source(Value,const SourceSpan&,std::string_view message="Result error created here");
Value propagated_error(const Value& value,const SourceSpan& span={});
Value result_payload(const Value& value, bool success, const SourceSpan& span);
std::string declared_name(const Node& node);
std::optional<std::uint64_t> fixed_extent(std::string_view text);
std::string canonical_type(std::string_view type);
// canonical_type, cached per thread; the reference stays valid for the thread's lifetime.
const std::string& canonical_spelling(std::string_view type);
std::string type_name(const Node& node);
std::string value_type(const Value& value);
// value_type(value)==type without building the type text for common kinds.
bool value_type_is(const Value& value,std::string_view type);
// True when enforce_type(value,type) is certain to return value unchanged (cheap, no allocation).
bool enforce_identity(const Value& value,const std::string& type);
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
// Result payload storage: untracked by the collector when the payload cannot form a cycle.
std::shared_ptr<Value> result_box(Value payload);
Value copy_value(Value&& value, bool deep_slices = false, unsigned depth = 0, bool resource_alias = false);
Value read_only(Value value);
Value enforce_type(Value value, const std::string& expected, const SourceSpan& span, bool contextual_number = false);
std::int64_t as_int(const Value& value, const SourceSpan& span);
bool as_bool(const Value& value, const SourceSpan& span);
[[noreturn]] void runtime_error(const SourceSpan& span, std::string message, std::string code = "E4001");
}
