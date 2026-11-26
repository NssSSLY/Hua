#ifndef HUA_NATIVE_RT_HPP_INCLUDED
#define HUA_NATIVE_RT_HPP_INCLUDED
#pragma once
// Runtime for natively compiled Hua (prototype, 2026-11-11). Generated code is statically
// typed: no value carries a type tag and nothing re-checks what the checker proved.
// What stays at run time is what the language defines at run time: overflow, bounds,
// unwrap, the step budget and the call depth limit. Every failure path and every library
// call without a typed implementation goes through the VM's own functions (binary_value,
// index_value, invoke_builtin) on boxed copies, so diagnostics and formatting are the VM's.
#include "hua/value.hpp"
#include "hua/runtime.hpp"
#include "hua/entry.hpp"
#include "hua/tasks.hpp"
#include "hua/execution.hpp"
#include "hua/enum_value.hpp"
#include "hua/random_engine.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <tuple>
#include <vector>
namespace hn {
using I=std::int64_t;using F=double;
using hua::SourceSpan;using hua::Value;
struct Unit { bool operator==(const Unit&)const{return true;} };
struct Nil {};
struct TaskHandle { Value value; };

// ---- execution state: budget, call depth and the call-site stack used for traces
class NativeFiber;
struct State {
    std::int64_t steps_left{100000000000};std::uint64_t step_limit{100000000000};
    unsigned depth{0},call_limit{128};
    std::array<const SourceSpan*,160> sites{};
    hua::RuntimeContext* context{};
    std::ostream* output{};
    NativeFiber* fiber{};
    void* module_data{};
    unsigned ticks{};
};
inline State state;
inline thread_local State* active_task_state = nullptr;
inline State& current_state() noexcept { return active_task_state ? *active_task_state : state; }
struct TaskModuleScope {
    void* previous;
    explicit TaskModuleScope(void* data) : previous(std::exchange(current_state().module_data,data)) {}
    ~TaskModuleScope(){current_state().module_data=previous;}
};
void check_global_write(const SourceSpan&);
template<bool Tasks> inline State& execution_state() noexcept {
    if constexpr (Tasks) return current_state();
    else return state;
}
void task_poll(State&, const SourceSpan&);
void task_loop(State&);
[[noreturn]] void step_limit_exceeded();
[[noreturn]] void depth_limit_exceeded(const SourceSpan&);
template<bool Tasks=false> inline void step() {
    if (--execution_state<Tasks>().steps_left < 0) [[unlikely]] step_limit_exceeded();
}
template<bool CountSteps, bool Tasks>
inline void loop_step() {
    if constexpr (CountSteps) step<Tasks>();
    if constexpr (Tasks) task_loop(execution_state<Tasks>());
}
// A call from `site`: counted, depth-limited, and recorded for "called from" traces. The
// depth is restored by the caller on normal return only, so a diagnostic still sees the
// whole chain when it reaches main.
template<bool CountSteps=true, bool Tasks=false> inline unsigned enter(const SourceSpan& site) {
    auto& running = execution_state<Tasks>();
    if constexpr (Tasks) task_poll(running, site);
    const unsigned depth = running.depth;
    if (depth >= running.call_limit) [[unlikely]] depth_limit_exceeded(site);
    running.sites[depth] = &site;
    running.depth = depth + 1;
    if constexpr (CountSteps) step<Tasks>();
    return depth;
}
template<bool Tasks=false> inline void leave(unsigned depth) { execution_state<Tasks>().depth = depth; }

// ---- small-object pool: strings, boxes and list/map bodies are allocated and freed
// constantly; size-class free lists avoid the system allocator for blocks up to 256 bytes.
struct Pool {
    static constexpr std::size_t classes=16,granule=16;
    struct Free { Free* next; };
    Free* heads[classes]{};char* chunk{};std::size_t left{};
    void* take(std::size_t size){
        const std::size_t c=(size+granule-1)/granule-1;
        if(auto* f=heads[c]){heads[c]=f->next;return f;}
        const std::size_t bytes=(c+1)*granule;
        if(left<bytes){chunk=static_cast<char*>(::operator new(1<<16));left=1<<16;}
        void* p=chunk;chunk+=bytes;left-=bytes;return p;
    }
    void give(void* p,std::size_t size){const std::size_t c=(size+granule-1)/granule-1;auto* f=static_cast<Free*>(p);f->next=heads[c];heads[c]=f;}
};
inline Pool pool;
// Task executables use the thread-safe allocator for these untracked objects.
// The flag is selected before either the entry or a worker runs user code.
inline bool native_tasks_enabled = false;
inline void* pool_new(std::size_t size) {
    if (!native_tasks_enabled && size <= Pool::classes * Pool::granule) return pool.take(size);
    return ::operator new(size);
}
inline void pool_delete(void* p, std::size_t size) {
    if (!native_tasks_enabled && size <= Pool::classes * Pool::granule) pool.give(p, size);
    else ::operator delete(p);
}
struct Pooled {
    static void* operator new(std::size_t size){return pool_new(size);}
    static void operator delete(void* p,std::size_t size){pool_delete(p,size);}
};
// ---- untracked objects are confined to a single Task or the synchronous entry
template<class T> inline constexpr bool tracks_native_edges=false;
template<class T> inline constexpr bool tracks_native_edges<std::vector<T>> = tracks_native_edges<T>;
template<class T,bool Tracked=tracks_native_edges<T>> class Ref;
template<class T> class Ref<T,false> {
    struct Box:Pooled { std::uint32_t rc; T value; template<class... A> explicit Box(A&&... a):rc(1),value(std::forward<A>(a)...){} };
    Box* b_{};
    void drop(){if(b_&&--b_->rc==0)delete b_;b_=nullptr;}
public:
    Ref()=default;
    template<class... A> static Ref make(A&&... a){Ref r;r.b_=new Box(std::forward<A>(a)...);return r;}
    Ref(const Ref& o):b_(o.b_){if(b_)++b_->rc;}
    Ref(Ref&& o)noexcept:b_(o.b_){o.b_=nullptr;}
    Ref& operator=(const Ref& o){if(b_!=o.b_){if(o.b_)++o.b_->rc;drop();b_=o.b_;}return *this;}
    Ref& operator=(Ref&& o)noexcept{if(this!=&o){drop();b_=o.b_;o.b_=nullptr;}return *this;}
    ~Ref(){drop();}
    T* get()const{return b_?&b_->value:nullptr;}
    T& operator*()const{return b_->value;}
    T* operator->()const{return &b_->value;}
    explicit operator bool()const{return b_!=nullptr;}
    bool same(const Ref& o)const{return b_==o.b_;}
};
template<class T> class Ref<T,true> {
    std::shared_ptr<T> object_;
public:
    template<class... A> static Ref make(A&&... args){
        Ref result;
        result.object_=hua::managed<T>(std::forward<A>(args)...);
        return result;
    }
    static Ref immutable_empty(){
        // Used only for zero-length backing stores that never accept insertion.
        static const auto empty=std::make_shared<T>();
        Ref result;
        result.object_=empty;
        return result;
    }
    T* get() const {return object_.get();}
    T& operator*() const {return *object_;}
    T* operator->() const {return get();}
    explicit operator bool() const {return static_cast<bool>(object_);}
    bool same(const Ref& other) const {return object_==other.object_;}
    friend void heap_edges(const Ref& ref,const hua::HeapVisitor& visit){
        if(ref)visit(ref.get());
    }
};
template<class T> void trace_native(const T& value,const hua::HeapVisitor& visit){
    if constexpr(requires{heap_edges(value,visit);})heap_edges(value,visit);
}
template<class T> void heap_edges(const std::vector<T>& values,const hua::HeapVisitor& visit){
    if constexpr(tracks_native_edges<T>)
        for(const auto& value:values)trace_native(value,visit);
}

// Immutable text. Literals are static and never freed.
// Promote only records whose fields are used as locations across another evaluation.
template<class T> class RecordStorage {
    std::variant<T,Ref<T>> storage_;
public:
    RecordStorage()=default;
    RecordStorage(const RecordStorage& other):storage_(other.get()){}
    RecordStorage(RecordStorage&&)=default;
    RecordStorage& operator=(RecordStorage&&)=default;
    RecordStorage& operator=(const RecordStorage& other){
        if(this!=&other)*this=RecordStorage(other);
        return *this;
    }
    const T& get() const {
        if(const auto* body=std::get_if<T>(&storage_))return *body;
        return *std::get<Ref<T>>(storage_);
    }
    T& get(){return const_cast<T&>(std::as_const(*this).get());}
    static RecordStorage alias(const Ref<T>& owner){
        RecordStorage result;
        result.storage_.template emplace<Ref<T>>(owner);
        return result;
    }
    friend void heap_edges(const RecordStorage& record,const hua::HeapVisitor& visit){
        if(const auto* inline_body=std::get_if<T>(&record.storage_))trace_native(*inline_body,visit);
        else trace_native(std::get<Ref<T>>(record.storage_),visit);
    }
    Ref<T> pin(){
        if(auto* body=std::get_if<T>(&storage_)){
            auto owner=Ref<T>::make(std::move(*body));
            storage_.template emplace<Ref<T>>(std::move(owner));
        }
        return std::get<Ref<T>>(storage_);
    }
};
template<class T> struct BoundMethod {
    Ref<T> receiver;
    bool writable{};
    friend void heap_edges(const BoundMethod& method,const hua::HeapVisitor& visit){
        trace_native(method.receiver,visit);
    }
};
template<class T> inline constexpr bool tracks_native_edges<BoundMethod<T>> = tracks_native_edges<T>;
template<class R,class T> struct FieldPlace {
    Ref<R> owner;
    T R::*member;
    T& read() const {return (*owner).*member;}
    void write(T value,const SourceSpan&) const {read()=std::move(value);}
};

struct StrRep:Pooled {
    static constexpr std::uint32_t immortal = 1u << 30;
    std::uint32_t rc;
    std::string text;
    std::uint64_t hash{};
    StrRep(std::uint32_t count, std::string value);
};
class Str {
    StrRep* p_;
    static StrRep* empty_rep(){static StrRep e(StrRep::immortal,{});return &e;}
    static void retain(StrRep* p) { if (p && p->rc != StrRep::immortal) ++p->rc; }
    void drop() { if (p_ && p_->rc != StrRep::immortal && --p_->rc == 0) delete p_; }
public:
    Str():p_(empty_rep()){}
    explicit Str(std::string text):p_(new StrRep{1,std::move(text)}){}
    explicit Str(StrRep* literal):p_(literal){retain(p_);}
    Str(const Str& o):p_(o.p_){retain(p_);}
    Str(Str&& o)noexcept:p_(o.p_){o.p_=nullptr;}
    Str& operator=(const Str& o){if(p_!=o.p_){retain(o.p_);drop();p_=o.p_;}return *this;}
    Str& operator=(Str&& o)noexcept{if(this!=&o){drop();p_=o.p_;o.p_=nullptr;}return *this;}
    ~Str(){drop();}
    const std::string& text()const{return p_->text;}
    bool operator==(const Str& o)const{return p_==o.p_||p_->text==o.p_->text;}
    bool append_if_unique(const Str& suffix){
        if(p_->rc!=1)return false;
        p_->text.append(suffix.text());
        p_->hash=0;
        return true;
    }
    static std::uint64_t hash_text(std::string_view text){
        // FNV-1a with zero reserved for "not computed yet".
        std::uint64_t h=0xcbf29ce484222325ULL;
        for(unsigned char c:text){h^=c;h*=0x100000001b3ULL;}
        return h?h:1;
    }
    std::uint64_t hash()const{
        if(p_->rc==StrRep::immortal)return p_->hash;
        if(!p_->hash)p_->hash=hash_text(p_->text);
        return p_->hash;
    }
    bool operator<(const Str& o)const{return p_->text<o.p_->text;}
};
inline StrRep::StrRep(std::uint32_t count, std::string value)
    : rc(count), text(std::move(value)), hash(count == immortal ? Str::hash_text(text) : 0) {}

// The semantic model identifies a function value's declaration in its type.
template<class Tag,bool Nested> struct FunctionValue {};
template<class Tag> struct FunctionValue<Tag,true> {
    const void* data{};
    std::shared_ptr<const void> owner;
    friend void heap_edges(const FunctionValue& f,const hua::HeapVisitor& visit){
        if(f.owner)visit(f.owner.get());
    }
};
template<class Tag> inline constexpr bool tracks_native_edges<FunctionValue<Tag,true>> = true;
template<class Function,class Environment> Function make_function(Environment env){
    std::shared_ptr<const Environment> owner;
    if constexpr(requires(const Environment& e,const hua::HeapVisitor& v){heap_edges(e,v);})
        owner=hua::managed<Environment>(std::move(env));
    else owner=std::make_shared<const Environment>(std::move(env));
    return Function{owner.get(),std::move(owner)};
}
template<class Function,class Environment>
Function make_function_in(std::optional<Environment>* slot,Environment env){
    if(!slot)return make_function<Function>(std::move(env));
    return Function{&slot->emplace(std::move(env)),{}};
}
template<class F,std::size_t N> F stack_function_at(const std::array<F,N>& values,I index,const SourceSpan& site){
    if(index<0||static_cast<std::uint64_t>(index)>=N)
        hua::runtime_error(site,"index out of bounds","E4006");
    return values[static_cast<std::size_t>(index)];
}
struct FileScan {
    struct Key {
        const void* token;
        const void* object;
        std::size_t start{},length{};
        bool operator<(const Key& other) const {
            const std::less<const void*> less;
            if(token!=other.token)return less(token,other.token);
            if(object!=other.object)return less(object,other.object);
            return std::tie(start,length)<std::tie(other.start,other.length);
        }
    };
    std::set<Key> visited;
    template<class T> static inline const char type_token=0;
    template<class T> bool first(const void* object,std::size_t start=0,std::size_t length=0){
        return object&&visited.emplace(Key{&type_token<T>,object,start,length}).second;
    }
};
template<class T> bool file_in(const T& value,FileScan& scan,unsigned depth){
    if constexpr(requires{resource_edges(value,scan,depth);}){
        if(depth>128)hua::runtime_error({},"value nesting limit exceeded","E4099");
        return resource_edges(value,scan,depth);
    }
    return false;
}
template<class Tag> struct ClosureFile {
    static bool scan(const void*,FileScan&,unsigned){return false;}
};
template<class Tag> bool resource_edges(const FunctionValue<Tag,true>& value,FileScan& scan,unsigned depth){
    if(!scan.first<FunctionValue<Tag,true>>(value.data))return false;
    return ClosureFile<Tag>::scan(value.data,scan,depth+1);
}
template<class Tag,bool Nested> FunctionValue<Tag,Nested> clone(const FunctionValue<Tag,Nested>& value){return value;}
template<class Signature> struct IndirectFunction;
template<class R,class... A> struct IndirectFunction<R(A...)> {
    using Entry=R (*)(const void*,const std::shared_ptr<const void>&,A...);
    Entry entry{};
    const void* data{};
    std::shared_ptr<const void> owner;
    std::string_view type;
    bool (*resources)(const void*,FileScan&,unsigned){};
    R invoke(A... args) const {return entry(data,owner,std::forward<A>(args)...);}
    friend void heap_edges(const IndirectFunction& f,const hua::HeapVisitor& visit){
        if(f.owner)visit(f.owner.get());
    }
};
template<class S> inline constexpr bool tracks_native_edges<IndirectFunction<S>> = true;
template<class S> bool resource_edges(const IndirectFunction<S>& value,FileScan& scan,unsigned depth){
    return value.resources&&value.resources(value.data,scan,depth+1);
}
template<class S> IndirectFunction<S> clone(const IndirectFunction<S>& value){return value;}
template<class Signature> void assign_function(IndirectFunction<Signature>& target,IndirectFunction<Signature> value,const SourceSpan& site){
    if(target.type!=value.type)hua::runtime_error(site,"expected "+std::string(target.type)+", got "+std::string(value.type),"E4003");
    target=std::move(value);
}
struct DynamicValue;
struct DynamicFunction {
    using Entry=DynamicValue (*)(const DynamicFunction&,std::span<const DynamicValue>,std::span<const bool>,const SourceSpan&);
    Entry entry{};
    const void* data{};
    std::shared_ptr<const void> owner;
    std::string type;
    std::size_t arity{};
    bool (*resources)(const DynamicFunction&,FileScan&,unsigned){};
    void clear(){*this={};}
    friend void heap_edges(const DynamicFunction& f,const hua::HeapVisitor& visit){
        if(f.owner)visit(f.owner.get());
    }
};
template<> inline constexpr bool tracks_native_edges<DynamicFunction> = true;
inline bool resource_edges(const DynamicFunction& value,FileScan& scan,unsigned depth){
    return value.resources&&value.resources(value,scan,depth+1);
}
inline DynamicFunction clone(const DynamicFunction& value){return value;}
inline void check_function_type(const DynamicFunction& value,const std::string& expected,const SourceSpan& site){
    if(expected!=value.type)hua::runtime_error(site,"expected "+expected+", got "+value.type,"E4003");
}
inline void assign_function(DynamicFunction& target,DynamicFunction value,const SourceSpan& site){
    check_function_type(value,target.type,site);
    target=std::move(value);
}
inline void append_function(std::vector<DynamicFunction>& items,DynamicFunction value,const SourceSpan& site){
    if(!items.empty())check_function_type(value,items.front().type,site);
    items.push_back(std::move(value));
}
template<class T> struct DynamicHolder {
    T value;
    explicit DynamicHolder(T item):value(std::move(item)){}
    void clear(){value={};}
    friend void heap_edges(const DynamicHolder& holder,const hua::HeapVisitor& visit){trace_native(holder.value,visit);}
};
struct DynamicValue {
    Value scalar;
    std::shared_ptr<void> owner;
    const void* token{};
    std::string type;
    Value (*box)(const void*){};
    DynamicValue (*copy)(const DynamicValue&,bool){};
    DynamicValue (*as_slice)(const DynamicValue&){};
    DynamicValue (*deep_copy)(const DynamicValue&){};
    bool (*resources)(const void*,FileScan&,unsigned){};
    bool writable{};
    std::string actual_type() const {return token?type:hua::value_type(scalar);}
    friend void heap_edges(const DynamicValue& value,const hua::HeapVisitor& visit){
        if(value.owner)visit(value.owner.get());
        else hua::heap_edges(value.scalar,visit);
    }
};
template<> inline constexpr bool tracks_native_edges<DynamicValue> = true;
inline bool resource_edges(const DynamicValue& value,FileScan& scan,unsigned depth){
    if(!value.token)return hua::contains_file(value.scalar,depth);
    return value.resources&&value.resources(value.owner.get(),scan,depth+1);
}
template<class T> using Opt=std::optional<T>;
template<class T> inline constexpr bool tracks_native_edges<Opt<T>> = tracks_native_edges<T>;
template<class T> void heap_edges(const Opt<T>& value,const hua::HeapVisitor& visit){
    if(value)trace_native(*value,visit);
}
// `trace` is set only on errors: where the error was created and each `?` it passed, as
// the VM records them for diagnostics.
using Trace=Ref<hua::SourceTrace>;
inline Trace traced(const Trace& before,const SourceSpan& span,const char* message){
    auto out=before?Trace::make(*before):Trace::make();out->add(span,message);return out;}
template<class T,class E> struct Result {
    bool ok{};T value{};E error{};Trace trace{};
    friend void heap_edges(const Result& result,const hua::HeapVisitor& visit){
        if(result.ok)trace_native(result.value,visit);
        else trace_native(result.error,visit);
    }
};
template<class T,class E> inline constexpr bool tracks_native_edges<Result<T,E>> = tracks_native_edges<T>||tracks_native_edges<E>;
template<class T> struct OkV {
    T value;
    template<class E> operator Result<T,E>()&&{return {true,std::move(value),E{}};}
    template<class E> operator Result<Opt<T>,E>()&&{return {true,Opt<T>{std::move(value)},E{}};}
};
template<class E> struct ErrV { E error;Trace trace;template<class T> operator Result<T,E>()&&{return Result<T,E>{false,T{},std::move(error),std::move(trace)};} };
// `[]` before its context gives it an element type.
struct EmptySlice {};
template<class T> struct Slice {
    Ref<std::vector<T>> storage;I start{},length{};
    bool writable{true};
    Slice()=default;
    Slice(EmptySlice){
        if constexpr(tracks_native_edges<T>)storage=Ref<std::vector<T>>::immutable_empty();
        else storage=Ref<std::vector<T>>::make();
    }
    static Slice of(std::vector<T> items){
        if constexpr(tracks_native_edges<T>){
            if(items.empty())return Slice{EmptySlice{}};
        }
        Slice result;
        result.length=static_cast<I>(items.size());
        result.storage=Ref<std::vector<T>>::make(std::move(items));
        return result;
    }
    friend void heap_edges(const Slice& slice,const hua::HeapVisitor& visit){trace_native(slice.storage,visit);}
};
template<class T> inline constexpr bool tracks_native_edges<Slice<T>> = tracks_native_edges<T>;
// Fixed arrays copy their own elements, but an explicit slice keeps the storage alive.
template<class T,std::size_t N> struct Fixed {
    Slice<T> view;
    Fixed()=default;
    explicit Fixed(Slice<T> v):view(std::move(v)){}
    Fixed(const Fixed& other) {
        if(other.view.storage){
            view=Slice<T>::of(*other.view.storage);
            view.writable=other.view.writable;
        }
    }
    friend void heap_edges(const Fixed& array,const hua::HeapVisitor& visit){trace_native(array.view,visit);}
    Fixed(Fixed&&)=default;
    Fixed& operator=(Fixed&&)=default;
    Fixed& operator=(const Fixed& other){
        if(this!=&other)*this=Fixed(other);
        return *this;
    }
};
template<class T,std::size_t N> inline constexpr bool tracks_native_edges<Fixed<T,N>> = tracks_native_edges<T>;
template<class T,std::size_t N> Slice<T> sequence_view(const Fixed<T,N>& input){return input.view;}
template<class T> Slice<T> sequence_view(const Slice<T>& input){return input;}
template<class T,std::size_t N> Fixed<T,N> capture_value(Fixed<T,N> value){value.view.writable=false;return value;}

enum class ExactName { Byte,I8,I16,I32,I64,Isize,U8,U16,U32,U64,Usize,F32,F64,Integer };
template<ExactName Name,class Rep> struct Exact {
    Rep value{};
    bool operator==(const Exact&)const=default;
    bool operator<(const Exact& other)const{return value<other.value;}
};
template<ExactName Name> inline constexpr const char* exact_name(){
    constexpr const char* names[]={"byte","i8","i16","i32","i64","isize","u8","u16","u32","u64","usize","f32","f64","@integer"};
    return names[static_cast<std::size_t>(Name)];
}

template<class T> struct ListData:std::vector<T> {
    explicit ListData(std::vector<T> items):std::vector<T>(std::move(items)){}
    std::uint64_t revision{};
};
template<class T> inline constexpr bool tracks_native_edges<ListData<T>> = tracks_native_edges<T>;
template<class T> void heap_edges(const ListData<T>& list,const hua::HeapVisitor& visit){
    for(const auto& value:list)trace_native(value,visit);
}
template<class T> struct List {
    Ref<ListData<T>> data;
    bool writable{true};
    friend void heap_edges(const List& list,const hua::HeapVisitor& visit){trace_native(list.data,visit);}
};
template<class T> inline constexpr bool tracks_native_edges<List<T>> = tracks_native_edges<T>;
template<class T> struct Deque { Ref<std::deque<T>> data; };
template<class T> List<T> make_list(std::vector<T> items){
    return {Ref<ListData<T>>::make(std::move(items))};
}
// Types the VM implements as immutable values or shared handles (Bytes, Buffer, Json):
// carried as the VM's own Value, so library calls receive the very same handle.
template<class Tag> struct Opaque { Value v; };
struct Rng {
    std::shared_ptr<hua::RngState> data;
    bool writable{true};
};
struct BytesTag{static constexpr const char* name="Bytes";};
struct BufferTag{static constexpr const char* name="Buffer";};
struct JsonTag{static constexpr const char* name="Json";};
struct FileTag{static constexpr const char* name="std.file.File";};
struct IOFailureTag{static constexpr const char* name="std.file.IOFailure";};
struct ErrorTag{static constexpr const char* name="std.error.Value";};
struct NativeHandleTag{static constexpr const char* name="NativeHandle";};

template<class T> T capture_value(T value){return value;}
template<class T> Slice<T> capture_value(Slice<T> value){value.writable=false;return value;}
template<class T> List<T> capture_value(List<T> value){value.writable=false;return value;}
template<class Tag> Opaque<Tag> capture_value(Opaque<Tag> value){value.v=hua::read_only(std::move(value.v));return value;}


inline std::uint64_t mix64(std::uint64_t x){x^=x>>33;x*=0xff51afd7ed558ccdULL;x^=x>>33;x*=0xc4ceb9fe1a85ec53ULL;x^=x>>33;return x;}
inline std::uint64_t hash_of(bool k){return k?0x9e3779b97f4a7c15ULL:0x6a09e667f3bcc909ULL;}
inline std::uint64_t hash_of(I k){return mix64(static_cast<std::uint64_t>(k)+0x243f6a8885a308d3ULL);}
inline std::uint64_t hash_of(const Str& k){return k.hash();}
// Map body: hashed lookup over a dense entry array. Iteration is in ascending key order
// (the language's order): entries appended in ascending order stay sorted, anything else
// is sorted once, on the next iteration.
template<class K,class V> struct MapRep {
    std::vector<std::pair<K,V>> items;std::vector<std::uint64_t> hashes;std::vector<std::uint32_t> index;bool sorted{true};
    std::size_t size()const{return items.size();}
    void rebuild(std::size_t capacity){index.assign(capacity,0);const std::size_t mask=capacity-1;
        for(std::size_t p=0;p<items.size();++p){std::size_t i=hashes[p]&mask;while(index[i])i=(i+1)&mask;index[i]=static_cast<std::uint32_t>(p+1);}}
    template<class Match> std::size_t bucket(std::uint64_t hash,Match match){
        const auto mask=index.size()-1;
        auto i=static_cast<std::size_t>(hash)&mask;
        while(index[i]){
            const auto at=index[i]-1;
            if(hashes[at]==hash&&match(items[at].first))break;
            i=(i+1)&mask;
        }
        return i;
    }
    std::pair<K,V>* find(const K& k){
        if(items.empty())return nullptr;
        const auto entry=index[bucket(hash_of(k),[&](const K& key){return key==k;})];
        return entry?&items[entry-1]:nullptr;
    }
    const V* find_text(std::string_view text) requires std::is_same_v<K,Str> {
        if(items.empty())return nullptr;
        const auto entry=index[bucket(Str::hash_text(text),[&](const Str& key){return key.text()==text;})];
        return entry?&items[entry-1].second:nullptr;
    }
    void insert_or_assign(K k,V v){
        if(auto* found=find(k)){found->second=std::move(v);return;}
        if((items.size()+1)*2>index.size())rebuild(index.empty()?8:index.size()*2);
        const auto h=hash_of(k);const std::size_t mask=index.size()-1;std::size_t i=h&mask;while(index[i])i=(i+1)&mask;
        if(sorted&&!items.empty()&&!(items.back().first<k))sorted=false;
        items.emplace_back(std::move(k),std::move(v));hashes.push_back(h);index[i]=static_cast<std::uint32_t>(items.size());}
    void ensure_sorted(){
        if(sorted)return;
        std::vector<std::uint32_t> order(items.size());for(std::size_t i=0;i<order.size();++i)order[i]=static_cast<std::uint32_t>(i);
        std::sort(order.begin(),order.end(),[&](std::uint32_t a,std::uint32_t b){return items[a].first<items[b].first;});
        std::vector<std::pair<K,V>> next;std::vector<std::uint64_t> nh;next.reserve(items.size());nh.reserve(items.size());
        for(auto i:order){next.push_back(std::move(items[i]));nh.push_back(hashes[i]);}
        items=std::move(next);hashes=std::move(nh);rebuild(index.size());sorted=true;}
    void remove_bucket(std::size_t hole){
        const auto mask=index.size()-1;
        const auto at=static_cast<std::size_t>(index[hole]-1);
        const auto last=items.size()-1;
        if(at!=last){
            auto moved=static_cast<std::size_t>(hashes[last])&mask;
            while(index[moved]!=last+1)moved=(moved+1)&mask;
            items[at]=std::move(items.back());
            hashes[at]=hashes.back();
            index[moved]=static_cast<std::uint32_t>(at+1);
            sorted=false;
        }
        items.pop_back();
        hashes.pop_back();
        // Maintain every linear probe sequence across the vacated bucket.
        for(auto scan=(hole+1)&mask;index[scan];scan=(scan+1)&mask){
            const auto home=static_cast<std::size_t>(hashes[index[scan]-1])&mask;
            if(((hole-home)&mask)<((scan-home)&mask)){
                index[hole]=index[scan];hole=scan;
            }
        }
        index[hole]=0;
    }
    bool erase(const K& k){
        if(items.empty())return false;
        const auto i=bucket(hash_of(k),[&](const K& key){return key==k;});
        if(!index[i])return false;
        remove_bucket(i);
        return true;
    }
    Opt<V> take(const K& k){
        if(items.empty())return std::nullopt;
        const auto i=bucket(hash_of(k),[&](const K& key){return key==k;});
        if(!index[i])return std::nullopt;
        V value=std::move(items[index[i]-1].second);
        remove_bucket(i);
        return value;
    }
    void clear(){items.clear();hashes.clear();index.clear();sorted=true;}
    auto begin(){ensure_sorted();return items.begin();}
    auto end(){return items.end();}
};
template<class K,class V> inline constexpr bool tracks_native_edges<MapRep<K,V>> = tracks_native_edges<V>;
template<class K,class V> void heap_edges(const MapRep<K,V>& map,const hua::HeapVisitor& visit){
    for(const auto& entry:map.items)trace_native(entry.second,visit);
}
template<class K,class V> struct Map {
    Ref<MapRep<K,V>> data;
    bool writable{true};
    friend void heap_edges(const Map& map,const hua::HeapVisitor& visit){trace_native(map.data,visit);}
};
template<class K,class V> inline constexpr bool tracks_native_edges<Map<K,V>> = tracks_native_edges<V>;
template<class K,class V> Map<K,V> capture_value(Map<K,V> value){value.writable=false;return value;}
template<class T> Opt<T> capture_value(Opt<T> value);
template<class T,class E> Result<T,E> capture_value(Result<T,E> value);
template<class T> Opt<T> capture_value(Opt<T> value){
    if(value)*value=capture_value(std::move(*value));
    return value;
}
template<class T,class E> Result<T,E> capture_value(Result<T,E> value){
    if(value.ok)value.value=capture_value(std::move(value.value));
    else value.error=capture_value(std::move(value.error));
    return value;
}


// The erased record is used only at interface boundaries. Its tag selects a typed
// dispatch arm, and its copy operation implements Hua's record value semantics.
struct InterfaceObject {
    std::size_t tag{};
    void* data{};
    std::shared_ptr<void> owner;
    std::shared_ptr<void> (*copy)(const void*){};
    std::shared_ptr<void> (*deep_copy)(const void*){};
    std::shared_ptr<void> (*capture_copy)(const void*){};
    bool (*resources)(const void*,FileScan&,unsigned){};
    Value (*box)(const void*){};
    bool writable{};
    InterfaceObject()=default;
    InterfaceObject(InterfaceObject&&)=default;
    InterfaceObject& operator=(InterfaceObject&&)=default;
    InterfaceObject(const InterfaceObject& other)
        :tag(other.tag),copy(other.copy),deep_copy(other.deep_copy),capture_copy(other.capture_copy),resources(other.resources),box(other.box),writable(other.writable){
        if(other.data){owner=copy(other.data);data=owner.get();}
    }
    InterfaceObject& operator=(const InterfaceObject& other){
        if(this!=&other)*this=InterfaceObject(other);
        return *this;
    }
    InterfaceObject alias() const {
        InterfaceObject out;
        out.tag=tag;out.data=data;out.owner=owner;out.copy=copy;out.deep_copy=deep_copy;out.capture_copy=capture_copy;out.resources=resources;out.box=box;out.writable=writable;
        return out;
    }
};
template<class Tag> struct Interface {
    InterfaceObject object;
    friend void heap_edges(const Interface& value,const hua::HeapVisitor& visit){
        if(value.object.owner)visit(value.object.owner.get());
    }
};
template<class Tag> inline constexpr bool tracks_native_edges<Interface<Tag>> = true;
template<class T> struct BoundInterface {
    T receiver;
    std::string type;
    BoundInterface()=default;
    BoundInterface(T object,std::string identity):receiver(std::move(object)),type(std::move(identity)){}
    BoundInterface(const BoundInterface& other):receiver{other.receiver.object.alias()},type(other.type){}
    BoundInterface(BoundInterface&&)=default;
    BoundInterface& operator=(BoundInterface&&)=default;
    BoundInterface& operator=(const BoundInterface& other){
        if(this!=&other)*this=BoundInterface(other);
        return *this;
    }
    friend void heap_edges(const BoundInterface& value,const hua::HeapVisitor& visit){trace_native(value.receiver,visit);}
};
template<class T> inline constexpr bool tracks_native_edges<BoundInterface<T>> = true;
template<class T> BoundInterface<T> clone(const BoundInterface<T>& value){return value;}
template<class T> BoundInterface<T> check_bound_type(BoundInterface<T> value,const std::string& expected,const SourceSpan& site){
    if(value.type!=expected)hua::runtime_error(site,"expected "+expected+", got "+value.type,"E4003");
    return value;
}
template<class T> bool resource_edges(const BoundInterface<T>& value,FileScan& scan,unsigned depth){
    return file_in(value.receiver,scan,depth+1);
}
template<class T,class Arg> std::shared_ptr<void> interface_owner(Arg&& value){
    if constexpr(tracks_native_edges<T>)return hua::managed<T>(std::forward<Arg>(value));
    else return std::make_shared<T>(std::forward<Arg>(value));
}
template<class T> void interface_operations(InterfaceObject& object){
    object.copy=[](const void* p){return interface_owner<T>(*static_cast<const T*>(p));};
    object.deep_copy=[](const void* p){return interface_owner<T>(clone(*static_cast<const T*>(p)));};
    object.capture_copy=[](const void* p){return interface_owner<T>(capture_value(*static_cast<const T*>(p)));};
    object.box=[](const void* p){return to_value(*static_cast<const T*>(p));};
    object.resources=[](const void* p,FileScan& scan,unsigned depth){return file_in(*static_cast<const T*>(p),scan,depth);};
}
template<class Tag> Interface<Tag> capture_value(const Interface<Tag>& value){
    auto object=value.object.alias();
    if(object.data){
        object.owner=object.capture_copy(object.data);
        object.data=object.owner.get();
    }
    object.writable=false;
    return {std::move(object)};
}
template<class Tag> Value to_value(const Interface<Tag>& value){
    if(!value.object.data)return {};
    auto out=value.object.box(value.object.data);
    return value.object.writable?out:hua::read_only(std::move(out));
}
template<class Tag> Interface<Tag> clone(const Interface<Tag>& value){
    auto object=value.object.alias();
    if(object.data){object.owner=object.deep_copy(object.data);object.data=object.owner.get();}
    object.writable=true;
    return {std::move(object)};
}

template<class To,class T> To interface_value(const T& value,std::size_t tag,bool writable){
    InterfaceObject o;
    o.tag=tag;o.writable=writable;
    interface_operations<T>(o);
    o.owner=o.copy(&value);o.data=o.owner.get();
    return {std::move(o)};
}
template<class To,class T> To interface_borrow(T& value,std::size_t tag){
    InterfaceObject o;
    o.tag=tag;o.data=&value;o.writable=true;
    if constexpr(requires{value.fields.pin();}){
        using Storage=std::remove_reference_t<decltype(value.fields)>;
        o.owner=interface_owner<T>(T{Storage::alias(value.fields.pin())});
        o.data=o.owner.get();
    }
    interface_operations<T>(o);
    return {std::move(o)};
}
template<class To,class Tag> To interface_convert(const Interface<Tag>& value,bool writable){
    auto object=InterfaceObject(value.object);
    object.writable=object.writable&&writable;
    return {std::move(object)};
}
template<class Tag> Interface<Tag> readonly_interface(Interface<Tag> value){
    value.object.writable=false;
    return value;
}
template<class To,class Tag> To interface_alias(Interface<Tag>& value){return {value.object.alias()};}

// ---- static type names (the boxed forms carry them)
template<class T> struct TypeName;
template<> struct TypeName<I>{static std::string get(){return "int";}};
template<> struct TypeName<F>{static std::string get(){return "float";}};
template<> struct TypeName<bool>{static std::string get(){return "bool";}};
template<> struct TypeName<Str>{static std::string get(){return "string";}};
template<> struct TypeName<Unit>{static std::string get(){return "void";}};
template<ExactName Name,class Rep> struct TypeName<Exact<Name,Rep>>{
    static std::string get(){return exact_name<Name>();}
};
template<class T,std::size_t N> struct TypeName<Fixed<T,N>>{
    static std::string get(){return "["+std::to_string(N)+"]"+TypeName<T>::get();}
};
template<class T> struct TypeName<Slice<T>>{static std::string get(){return "[]"+TypeName<T>::get();}};
template<class T> struct TypeName<List<T>>{static std::string get(){return "List<"+TypeName<T>::get()+">";}};
template<class T> struct TypeName<Opt<T>>{static std::string get(){return TypeName<T>::get()+"?";}};
template<class Tag> struct TypeName<Opaque<Tag>>{static std::string get(){return Tag::name;}};
template<class K,class V> struct TypeName<Map<K,V>>{static std::string get(){return "map["+TypeName<K>::get()+"]"+TypeName<V>::get();}};
template<class T,class E> struct TypeName<Result<T,E>>{static std::string get(){return "Result<"+TypeName<T>::get()+","+TypeName<E>::get()+">";}};

// ---- boxing: typed value -> VM Value (printing, cold library calls, failure paths)
inline Value to_value(const Value& value){return value;}
inline Value to_value(I v){return Value(v);}
inline Value to_value(F v){return Value(v);}
inline Value to_value(bool v){return Value(v);}
inline Value to_value(const Str& v){return Value(v.text());}
inline Value to_value(Unit){return Value{};}
inline Value to_value(const TaskHandle& task) { return task.value; }
inline Value to_value(std::nullopt_t){return Value{};}
inline Value to_value(EmptySlice){return Value(hua::SliceValue{hua::managed<std::vector<Value>>(),0,0,true,""});}
inline Value to_value(const Rng& rng){return Value(hua::RngValue{rng.data,rng.writable});}
template<class Tag> Value to_value(const Opaque<Tag>& v){return v.v;}
template<ExactName Name,class Rep> Value to_value(const Exact<Name,Rep>& v){
    using Boxed=std::conditional_t<std::is_floating_point_v<Rep>,F,std::conditional_t<std::is_signed_v<Rep>,I,std::uint64_t>>;
    return Value(hua::NumericValue{exact_name<Name>(),static_cast<Boxed>(v.value)});
}
template<class T,std::size_t N> Value to_value(const Fixed<T,N>& v);
template<class T> Value to_value(const Opt<T>& v);
template<class T,class E> Value to_value(const Result<T,E>& v);
template<class T> Value to_value(const List<T>& v);
template<class K,class V> Value to_value(const Map<K,V>& v);
template<class T> Value to_value(const Slice<T>& v){
    auto storage=hua::managed<std::vector<Value>>();storage->reserve(static_cast<std::size_t>(v.length));
    for(I i=0;i<v.length;++i)storage->push_back(to_value((*v.storage)[static_cast<std::size_t>(v.start+i)]));
    return Value(hua::SliceValue{storage,0,static_cast<std::size_t>(v.length),v.writable,TypeName<T>::get()});
}
template<class T,std::size_t N> Value to_value(const Fixed<T,N>& v){
    const auto boxed=to_value(v.view);
    return Value(hua::fixed_array(hua::cget<hua::SliceValue>(boxed.data),TypeName<T>::get(),{},false));
}
template<class T> Value to_value(const Opt<T>& v){return v?to_value(*v):Value{};}
template<class T,class E> Value to_value(const Result<T,E>& v){
    auto payload=v.ok&&std::is_same_v<T,Unit>?std::shared_ptr<Value>{}:
        hua::result_box(v.ok?to_value(v.value):to_value(v.error));
    hua::ResultValue out{v.ok,std::move(payload),TypeName<T>::get(),TypeName<E>::get(),true,false};
    if(v.trace)out.failure_source=std::make_shared<const hua::SourceTrace>(*v.trace);
    return Value(std::move(out));
}
template<class T> Value to_value(const List<T>& v){
    auto data=hua::managed<hua::ListData>();data->element_type=TypeName<T>::get();
    for(const auto& x:*v.data)data->values.push_back(to_value(x));
    return Value(hua::ListValue{data,v.writable});
}
inline hua::MapKey map_key(bool k){return hua::MapKey(k);}
inline hua::MapKey map_key(I k){return hua::MapKey(k);}
inline hua::MapKey map_key(const Str& k){return hua::MapKey(k.text());}
template<class K,class V> Value to_value(const Map<K,V>& v){
    auto data=hua::managed<hua::MapData>();data->key_type=TypeName<K>::get();data->item_type=TypeName<V>::get();
    for(const auto& [k,x]:v.data->items)data->entries.emplace(map_key(k),to_value(x));
    return Value(hua::MapValue{data,v.writable});
}
// ---- unboxing: VM Value -> typed value (results of cold library calls)
[[noreturn]] void bad_unbox(const char* expected);
template<class T> struct From;
template<class T> T from_value(const Value& v){return From<T>::get(v);}
template<> inline Value from_value<Value>(const Value& value){return value;}
inline DynamicValue dynamic_scalar(Value value){
    DynamicValue result;
    result.scalar=std::move(value);
    return result;
}
template<> inline DynamicValue from_value<DynamicValue>(const Value& value){return dynamic_scalar(value);}
inline Value to_value(const DynamicValue& value){
    if(!value.token)return value.scalar;
    if(value.box)return value.box(value.owner.get());
    return {};
}
template<class T> inline const char dynamic_type_token=0;
template<class T> T clone_dynamic_object(const T& value);
template<class T> struct DynamicOptional {};
template<class T> struct DynamicOptional<Opt<T>> {using Element=T;};
template<class T> DynamicValue dynamic_object(T value,std::string type,bool writable){
    DynamicValue result;
    result.owner=interface_owner<T>(std::move(value));
    result.token=&dynamic_type_token<T>;
    result.type=std::move(type);
    const auto& stored=*static_cast<const T*>(result.owner.get());
    if constexpr(requires{stored.writable;})writable=writable&&stored.writable;
    else if constexpr(requires{stored.view.writable;})writable=writable&&stored.view.writable;
    else if constexpr(requires{stored.object.writable;})writable=writable&&stored.object.writable;
    result.writable=writable;
    if constexpr(requires(const T& sample){to_value(sample);})
        result.box=[](const void* ptr){return to_value(*static_cast<const T*>(ptr));};
    result.resources=[](const void* ptr,FileScan& scan,unsigned depth){
        return file_in(*static_cast<const T*>(ptr),scan,depth);
    };
    if constexpr(requires{stored.view;sequence_view(stored);}){
        result.as_slice=[](const DynamicValue& source){
            const auto& original=*static_cast<const T*>(source.owner.get());
            const auto end=source.type.find(']');
            return dynamic_object(sequence_view(original),"[]"+source.type.substr(end+1),source.writable);
        };
    }
    result.deep_copy=[](const DynamicValue& source){
        return dynamic_object<T>(clone_dynamic_object(*static_cast<const T*>(source.owner.get())),source.type,true);
    };
    result.copy=[](const DynamicValue& source,bool readonly){
        const auto& stored=*static_cast<const T*>(source.owner.get());
        return dynamic_object<T>(readonly?capture_value(stored):T(stored),source.type,source.writable&&!readonly);
    };
    return result;
}
inline DynamicValue dynamic_object(DynamicFunction value){
    auto identity=std::string(value.type);
    return dynamic_object(std::move(value),std::move(identity),false);
}
inline DynamicValue capture_value(const DynamicValue& value){
    if(value.token==&dynamic_type_token<DynamicFunction>)return value;
    if(value.copy)return value.copy(value,true);
    return dynamic_scalar(hua::read_only(value.scalar));
}
inline DynamicValue dynamic_mut(const DynamicValue& value,const SourceSpan& site){
    if(!value.writable)hua::runtime_error(site,"mut parameter requires mutable argument","E4007");
    return value;
}
inline const DynamicFunction& dynamic_callee(const DynamicValue& value,const SourceSpan& site){
    if(value.token!=&dynamic_type_token<DynamicFunction>)hua::runtime_error(site,"value is not callable","E4003");
    return *static_cast<const DynamicFunction*>(value.owner.get());
}
template<class T> bool dynamic_is(const DynamicValue& value){return value.token==&dynamic_type_token<T>;}
template<class T> const T& dynamic_exact(const DynamicValue& value,const std::string& expected,const SourceSpan& site){
    if(value.token!=&dynamic_type_token<T>)
        hua::runtime_error(site,"expected "+expected+", got "+value.actual_type(),"E4003");
    return *static_cast<const T*>(value.owner.get());
}
template<class T> T dynamic_record_alias(T& object){
    return T{std::remove_reference_t<decltype(object.fields)>::alias(object.fields.pin())};
}
inline DynamicValue copy_dynamic(const DynamicValue& value){
    return value.copy&&value.token!=&dynamic_type_token<DynamicFunction>?value.copy(value,false):value;
}
template<class T> T& dynamic_borrow(const DynamicValue& value,const std::string& expected,const SourceSpan& site){
    if(!value.writable)hua::runtime_error(site,"mut parameter requires mutable argument","E4007");
    return const_cast<T&>(dynamic_exact<T>(value,expected,site));
}
template<class T> T dynamic_take(const DynamicValue& value,const std::string& expected,const SourceSpan& site){
    if(value.token==&dynamic_type_token<T>)return dynamic_exact<T>(value,expected,site);
    if constexpr(requires{typename DynamicOptional<T>::Element;}){
        using Element=typename DynamicOptional<T>::Element;
        if(!value.token){
            if(hua::value_type(value.scalar)=="nil")return std::nullopt;
            if constexpr(requires{From<Element>::get(value.scalar);})
                return from_value<Element>(hua::enforce_type(value.scalar,expected.substr(0,expected.size()-1),site));
        }
        if(value.token==&dynamic_type_token<Element>)return dynamic_exact<Element>(value,expected,site);
    }
    if(value.as_slice&&expected.starts_with("[]")){
        auto slice=value.as_slice(value);
        return dynamic_exact<T>(slice,expected,site);
    }
    return dynamic_exact<T>(value,expected,site);
}
template<class T> T dynamic_mut_value(const DynamicValue& value,const std::string& expected,const SourceSpan& site){
    (void)dynamic_mut(value,site);
    return dynamic_take<T>(value,expected,site);
}
inline DynamicValue prepare_dynamic(const DynamicValue& target,DynamicValue value,const SourceSpan& site){
    const auto expected=target.actual_type();
    if(expected==value.actual_type())return copy_dynamic(value);
    if(!target.token&&!value.token)
        return dynamic_scalar(hua::enforce_type(std::move(value.scalar),expected,site));
    hua::runtime_error(site,"expected "+expected+", got "+value.actual_type(),"E4003");
}
inline void assign_dynamic(DynamicValue& target,DynamicValue value,const SourceSpan& site){
    target=prepare_dynamic(target,std::move(value),site);
}
struct DynamicMultiple {
    std::vector<DynamicValue> values;
    void clear(){values.clear();}
    friend void heap_edges(const DynamicMultiple& value,const hua::HeapVisitor& visit){
        for(const auto& item:value.values)trace_native(item,visit);
    }
    friend bool resource_edges(const DynamicMultiple& value,FileScan& scan,unsigned depth){
        for(const auto& item:value.values)if(file_in(item,scan,depth+1))return true;
        return false;
    }
};
template<> inline constexpr bool tracks_native_edges<DynamicMultiple> = true;
inline DynamicValue dynamic_multiple(std::vector<DynamicValue> values){
    std::string type="multi<";
    for(std::size_t i=0;i<values.size();++i){
        if(i)type+=",";
        type+=values[i].actual_type();
    }
    return dynamic_object(DynamicMultiple{std::move(values)},type+">",false);
}
inline const std::vector<DynamicValue>& unpack_dynamic(const DynamicValue& pack,std::size_t count,const SourceSpan& site){
    if(pack.token!=&dynamic_type_token<DynamicMultiple>)
        hua::runtime_error(site,"multiple value count mismatch","E4003");
    const auto& values=static_cast<const DynamicMultiple*>(pack.owner.get())->values;
    if(values.size()!=count)hua::runtime_error(site,"multiple value count mismatch","E4003");
    return values;
}
template<class T> T dynamic_convert(Value value,const std::string& expected,const SourceSpan& site,bool contextual=false){
    return from_value<T>(hua::enforce_type(std::move(value),expected,site,contextual));
}
template<class T> T dynamic_convert(const DynamicValue& value,const std::string& expected,const SourceSpan& site,bool contextual=false){
    if(value.token)hua::runtime_error(site,"expected "+expected+", got "+value.actual_type(),"E4003");
    return dynamic_convert<T>(value.scalar,expected,site,contextual);
}
template<> struct From<I>{static I get(const Value& v){if(auto p=hua::get_if<I>(&v.data))return *p;bad_unbox("int");}};
template<> struct From<F>{static F get(const Value& v){if(auto p=hua::get_if<F>(&v.data))return *p;bad_unbox("float");}};
template<> struct From<bool>{static bool get(const Value& v){if(auto p=hua::get_if<bool>(&v.data))return *p;bad_unbox("bool");}};
template<> struct From<Str>{static Str get(const Value& v){if(auto p=hua::string_data(&v.data))return Str(*p);bad_unbox("string");}};
template<ExactName Name,class Rep> struct From<Exact<Name,Rep>>{
    static Exact<Name,Rep> get(const Value& v){
        const auto numeric=hua::get_if<hua::NumericValue>(&v.data);
        if(!numeric)bad_unbox(exact_name<Name>());
        return {hua::visit([](auto n){return static_cast<Rep>(n);},numeric->number)};
    }
};
template<> struct From<Unit>{static Unit get(const Value&){return {};}};
template<> struct From<Rng>{
    static Rng get(const Value& v){
        const auto p=hua::get_if<hua::RngValue>(&v.data);
        if(!p)bad_unbox("std.random.Rng");
        return {p->data,p->writable};
    }
};
template<class Tag> struct From<Opaque<Tag>>{static Opaque<Tag> get(const Value& v){return {v};}};
template<class T> struct From<Slice<T>>{static Slice<T> get(const Value& v){
    auto p=hua::cget_if<hua::SliceValue>(&v.data);if(!p)bad_unbox("slice");
    std::vector<T> items;items.reserve(p->length);for(std::size_t i=0;i<p->length;++i)items.push_back(from_value<T>(hua::sequence_read(*p,i)));
    auto out=Slice<T>::of(std::move(items));
    out.writable=p->writable;
    return out;
}};
template<class T> struct From<Opt<T>>{static Opt<T> get(const Value& v){if(v.data.index()==0)return std::nullopt;return from_value<T>(v);}};
template<class T,class E> struct From<Result<T,E>>{static Result<T,E> get(const Value& v){
    auto p=hua::cget_if<hua::ResultValue>(&v.data);if(!p)bad_unbox("Result");
    if constexpr(std::is_same_v<T,Unit>){if(p->ok)return Result<T,E>{true,Unit{},E{}};}
    if(!p->payload)bad_unbox("Result payload");
    if(p->ok)return Result<T,E>{true,from_value<T>(*p->payload),E{}};
    return Result<T,E>{false,T{},from_value<E>(*p->payload),p->failure_source?Trace::make(*p->failure_source):Trace{}};}};
template<class T,std::size_t N> struct From<Fixed<T,N>>{
    static Fixed<T,N> get(const Value& v){return Fixed<T,N>(from_value<Slice<T>>(v));}
};
template<class T> struct From<List<T>>{static List<T> get(const Value& v){
    auto p=hua::get_if<hua::ListValue>(&v.data);if(!p)bad_unbox("List");
    std::vector<T> items;items.reserve(p->data->values.size());for(const auto& x:p->data->values)items.push_back(from_value<T>(x));
    return make_list<T>(std::move(items));}};

template<class... T> struct From<std::tuple<T...>> {
    template<std::size_t... N>
    static std::tuple<T...> unpack(const std::vector<Value>& items,std::index_sequence<N...>){
        return {from_value<T>(items[N])...};
    }
    static std::tuple<T...> get(const Value& v){
        const auto pack=hua::cget_if<hua::MultiValue>(&v.data);
        if(!pack||!pack->data||pack->data->values.size()!=sizeof...(T))bad_unbox("multiple values");
        return unpack(pack->data->values,std::index_sequence_for<T...>{});
    }
};

// Used only for bridge results that are new maps and have no external aliases.
template<class K,class V> struct From<Map<K,V>> {
    static Map<K,V> get(const Value& v){
        const auto map=hua::cget_if<hua::MapValue>(&v.data);
        if(!map||!map->data)bad_unbox("map");
        Map<K,V> out{Ref<MapRep<K,V>>::make()};
        for(const auto& [key,item]:map->data->entries){
            auto key_value=hua::visit([](const auto& value){return Value(value);},key);
            out.data->insert_or_assign(from_value<K>(key_value),from_value<V>(item));
        }
        return out;
    }
};

// ---- library calls through the VM's implementation (boxed)
Value call_builtin(const char* name,const SourceSpan& span,std::vector<Value> args,const std::vector<bool>& contextual,const char* result_type);
// One bridged call site: the builtin is resolved once and its metadata kept, and the
// argument vector is reused, so a call costs the boxing of its arguments and nothing else.
struct BridgeInfo {
    const std::string name, result_type;
    const std::vector<bool> contextual;
    const hua::BuiltinInfo& info;
    BridgeInfo(const char* name, std::initializer_list<bool> flags, const char* result)
        : name(name), result_type(result), contextual(flags), info(hua::builtin_info(this->name)) {}
};
struct Bridge : BridgeInfo {
    using BridgeInfo::BridgeInfo;
    std::vector<Value> args;
    bool busy{};
};
Value call_bridge(Bridge& site, const SourceSpan& span, std::initializer_list<Value> args);
Value call_bridge(const BridgeInfo& site, const SourceSpan& span, std::initializer_list<Value> args);
void print(const SourceSpan& span,std::vector<Value> args);
[[noreturn]] void fail(const SourceSpan& span,const char* message,const char* code);
// The VM evaluates the operator on boxed operands: used for every case the inline
// arithmetic does not cover, so the value or the diagnostic is the VM's.
template<class R,class A,class B> R general_binary(const char* op,const A& a,const B& b,const SourceSpan& s,bool contextual=false){
    return from_value<R>(hua::binary_value(op,to_value(a),to_value(b),s,contextual));
}
template<class R,class A> R general_unary(const char* op,const A& a,const SourceSpan& s){return from_value<R>(hua::unary_value(op,to_value(a),s));}

// ---- inline arithmetic: the common, plainly representable cases
inline I add(I a,I b,const SourceSpan& s){I r;if(__builtin_add_overflow(a,b,&r))[[unlikely]]return general_binary<I>("+",a,b,s);return r;}
inline I sub(I a,I b,const SourceSpan& s){I r;if(__builtin_sub_overflow(a,b,&r))[[unlikely]]return general_binary<I>("-",a,b,s);return r;}
inline I mul(I a,I b,const SourceSpan& s){I r;if(__builtin_mul_overflow(a,b,&r))[[unlikely]]return general_binary<I>("*",a,b,s);return r;}
inline I floor_div(I a,I b,const SourceSpan& s){
    if(b==0||(a==std::numeric_limits<I>::min()&&b==-1))[[unlikely]]return general_binary<I>("//",a,b,s);
    I q=a/b,r=a%b;if(r&&((r<0)!=(b<0)))--q;return q;}
inline I mod(I a,I b,const SourceSpan& s){
    if(b==0||(a==std::numeric_limits<I>::min()&&b==-1))[[unlikely]]return general_binary<I>("%",a,b,s);
    I r=a%b;if(r&&((r<0)!=(b<0)))r+=b;return r;}
inline I neg(I a,const SourceSpan& s){if(a==std::numeric_limits<I>::min())[[unlikely]]return general_unary<I>("-",a,s);return -a;}
inline F fadd(F a,F b,const SourceSpan& s){F r=a+b;if(!std::isfinite(r))[[unlikely]]return general_binary<F>("+",a,b,s);return r;}
inline F fsub(F a,F b,const SourceSpan& s){F r=a-b;if(!std::isfinite(r))[[unlikely]]return general_binary<F>("-",a,b,s);return r;}
inline F fmul(F a,F b,const SourceSpan& s){F r=a*b;if(!std::isfinite(r))[[unlikely]]return general_binary<F>("*",a,b,s);return r;}
inline F fdiv(F a,F b,const SourceSpan& s){F r=b!=0?a/b:std::numeric_limits<F>::quiet_NaN();if(!std::isfinite(r))[[unlikely]]return general_binary<F>("/",a,b,s);return r;}
inline Str concat(const Str& a,const Str& b,const SourceSpan& s){
    if(a.text().size()+b.text().size()>(1u<<20))[[unlikely]]return general_binary<Str>("+",a,b,s);
    std::string out;out.reserve(a.text().size()+b.text().size());out+=a.text();out+=b.text();return Str(std::move(out));}

inline void append_text(Str& target,const Str& suffix,const SourceSpan& site){
    if(target.text().size()+suffix.text().size()>(1u<<20)||!target.append_if_unique(suffix))
        target=concat(target,suffix,site);
}

// ---- typed core builtins
inline I len(const Str& v){return static_cast<I>(v.text().size());}
template<class T> I len(const Slice<T>& v){return v.length;}
template<class K,class V> I len(const Map<K,V>& v){return static_cast<I>(v.data->size());}
inline Str str(I v){return Str(std::to_string(v));}
inline Str str(const Str& v){return v;}
template<class T> Str str(const T& v){static const SourceSpan none;return from_value<Str>(call_builtin("str",none,{to_value(v)},{false},"string"));}
template<class T> OkV<T> ok(T v){return OkV<T>{std::move(v)};}
inline OkV<Unit> ok(){return OkV<Unit>{};}
template<class E> ErrV<E> err(E e,const SourceSpan& s){return ErrV<E>{std::move(e),traced({},s,"Result error created here")};}
// `?` on an error: returned to the caller with the propagation site added.
template<class E> ErrV<E> propagated(E e,const Trace& trace,const SourceSpan& s){return ErrV<E>{std::move(e),traced(trace,s,"Result propagated here")};}
template<class T,class E> bool is_ok(const Result<T,E>& r){return r.ok;}
template<class T,class E> bool is_err(const Result<T,E>& r){return !r.ok;}
template<class T> bool is_some(const Opt<T>& v){return v.has_value();}
template<class T> bool is_none(const Opt<T>& v){return !v.has_value();}
template<class T,class E> T unwrap(Result<T,E> r,const SourceSpan& s){
    if(!r.ok)[[unlikely]]{call_builtin("unwrap",s,{to_value(r)},{false},"");fail(s,"unwrap failed","E4009");}
    return std::move(r.value);}
template<class T> T unwrap(Opt<T> v,const SourceSpan& s){
    if(!v)[[unlikely]]{call_builtin("unwrap",s,{Value{}},{false},"");fail(s,"unwrap failed","E4009");}
    return std::move(*v);}
template<class T,class E> E unwrap_err(Result<T,E> r,const SourceSpan& s){
    if(r.ok)[[unlikely]]{call_builtin("unwrap_err",s,{to_value(r)},{false},"");fail(s,"unwrap_err failed","E4009");}
    return std::move(r.error);}
template<class T,class E,class D> T unwrap_or(Result<T,E> r,D fallback){return r.ok?std::move(r.value):T(std::move(fallback));}
template<class T,class D> T unwrap_or(Opt<T> v,D fallback){return v?std::move(*v):T(std::move(fallback));}

template<class View>
void require_writable(const View& view,const SourceSpan& site,const char* message="standard mutation requires a writable argument"){
    if(!view.writable)[[unlikely]]fail(site,message,"E4007");
}

// ---- slices, maps
template<class T> Slice<T> sub_slice(Slice<T> input,I start,I end,const SourceSpan& site){
    if(start<0||end<start||end>input.length)[[unlikely]]
        fail(site,"slice bounds must satisfy 0 <= start <= end <= len","E4006");
    input.start+=start;
    input.length=end-start;
    return input;
}
template<class T> typename std::vector<T>::const_reference at(const Slice<T>& v,I i,const SourceSpan& s){
    if(static_cast<std::uint64_t>(i)>=static_cast<std::uint64_t>(v.length))[[unlikely]]
        fail(s,"index out of bounds","E4006");
    return (*v.storage)[static_cast<std::size_t>(v.start+i)];}
template<class T> typename std::vector<T>::reference slot(Slice<T>& v,I i,const SourceSpan& s){
    require_writable(v,s,"cannot mutate a read-only slice or array");
    if(static_cast<std::uint64_t>(i)>=static_cast<std::uint64_t>(v.length))[[unlikely]]
        fail(s,"index out of bounds","E4006");
    return (*v.storage)[static_cast<std::size_t>(v.start+i)];}
template<class K,class V> Map<K,V> new_map(){return Map<K,V>{Ref<MapRep<K,V>>::make()};}
template<class K,class V> Opt<V> at(const Map<K,V>& m,const K& k,const SourceSpan&){
    auto* entry=m.data->find(k);
    if(!entry)return std::nullopt;
    return m.writable?entry->second:capture_value(entry->second);
}
template<class K,class V> bool has(const Map<K,V>& m,const K& k){return m.data->find(k)!=nullptr;}
template<class K,class V> void put(Map<K,V>& m,K k,V v,const SourceSpan& s){
    require_writable(m,s,"cannot mutate a read-only map");
    if(m.data->size()>=1000000&&!m.data->find(k))[[unlikely]]fail(s,"map entry limit exceeded","E4099");
    m.data->insert_or_assign(std::move(k),std::move(v));}
template<class K,class V> bool erase(Map<K,V>& m,const K& k,const SourceSpan& s){
    require_writable(m,s,"cannot mutate a read-only map");return m.data->erase(k);
}
template<class K,class V> V& slot(Map<K,V>& m,const K& k,const SourceSpan& s){
    require_writable(m,s,"cannot mutate a read-only map");auto f=m.data->find(k);if(!f)[[unlikely]]fail(s,"compound assignment requires an existing map key","E4006");return f->second;}
template<class T> struct SequencePlace {
    Slice<T> sequence;
    std::size_t offset;
    decltype(auto) read() const {return (*sequence.storage)[offset];}
    void write(T value,const SourceSpan&) const {read()=std::move(value);}
};
template<class T> SequencePlace<T> locate_sequence(Slice<T> sequence,I index,const SourceSpan& site){
    if(index<0||index>=sequence.length)fail(site,"index out of bounds","E4006");
    require_writable(sequence,site,"cannot mutate immutable binding or read-only view");
    const auto offset=static_cast<std::size_t>(sequence.start+index);
    return {std::move(sequence),offset};
}
template<class R,class T> FieldPlace<R,T> locate_field(RecordStorage<R>& record,T R::*member){
    return {record.pin(),member};
}
template<class K,class V> struct MapPlace {
    Map<K,V> map;
    K key;
    V& read(const SourceSpan& site){return slot(map,key,site);}
    void write(V value,const SourceSpan& site){put(map,std::move(key),std::move(value),site);}
};
template<class K,class V> MapPlace<K,V> locate_map(Map<K,V> map,K key,const SourceSpan& site){
    require_writable(map,site,"cannot mutate immutable binding or read-only view");
    return {std::move(map),std::move(key)};
}
template<class T> T& sequence_record(Slice<T>& seq,I index,const SourceSpan& site){
    if(index<0||index>=seq.length)hua::runtime_error(site,"index out of bounds","E4006");
    return (*seq.storage)[static_cast<std::size_t>(seq.start+index)];
}
template<class K,class V> V& map_record(Map<K,V>& map,const K& key,const SourceSpan& site){
    auto* entry=map.data->find(key);
    if(!entry)fail(site,"field assignment requires a struct","E4003");
    return entry->second;
}
struct PendingMapKey {
    const void* map{};
    const void* key{};
    bool (*equal)(const void*,const void*){};
    std::size_t capacity_left{};
};
template<class K,class V> PendingMapKey pending_map_key(const MapPlace<K,V>& place,std::size_t count){
    const auto remaining=1000000-place.map.data->size();
    if(remaining>=count||place.map.data->find(place.key))return {};
    return {place.map.data.get(),&place.key,
        [](const void* a,const void* b){return *static_cast<const K*>(a)==*static_cast<const K*>(b);},
        remaining};
}
template<class... Place> void prepare_map_writes(const SourceSpan& site,const Place&... places){
    const std::array<PendingMapKey,sizeof...(Place)> keys{pending_map_key(places,sizeof...(Place))...};
    // Only keys from nearly full maps enter this path. Count each distinct new key once.
    std::array<std::size_t,sizeof...(Place)> counts{};
    for(std::size_t i=0;i<keys.size();++i){
        const auto& key=keys[i];
        if(!key.map)continue;
        std::size_t first=i;
        bool duplicate=false;
        for(std::size_t j=0;j<i;++j){
            if(keys[j].map!=key.map)continue;
            if(first==i)first=j;
            if(key.equal(key.key,keys[j].key)){duplicate=true;break;}
        }
        if(!duplicate&&++counts[first]>key.capacity_left)fail(site,"map entry limit exceeded","E4099");
    }
}
// clone: a deep copy (handles get fresh storage)
inline I clone(I v){return v;}inline F clone(F v){return v;}inline bool clone(bool v){return v;}inline Str clone(const Str& v){return v;}
template<ExactName Name,class Rep> Exact<Name,Rep> clone(Exact<Name,Rep> value){return value;}
template<class T,std::size_t N> Fixed<T,N> clone(const Fixed<T,N>& v);
template<class T> Slice<T> clone(const Slice<T>& v){std::vector<T> items;items.reserve(static_cast<std::size_t>(v.length));for(I i=0;i<v.length;++i)items.push_back(clone((*v.storage)[static_cast<std::size_t>(v.start+i)]));return Slice<T>::of(std::move(items));}
template<class T,std::size_t N> Fixed<T,N> clone(const Fixed<T,N>& v){return Fixed<T,N>(clone(v.view));}
template<class K,class V> Map<K,V> clone(const Map<K,V>& v){auto out=new_map<K,V>();for(const auto& [k,x]:v.data->items)out.data->insert_or_assign(k,clone(x));return out;}
template<class T> List<T> clone(const List<T>& v){std::vector<T> items;for(const auto& x:*v.data)items.push_back(clone(x));return make_list<T>(std::move(items));}
template<class T> Opt<T> clone(const Opt<T>& v){if(!v)return std::nullopt;return clone(*v);}
template<class Tag> Opaque<Tag> clone(const Opaque<Tag>& v){return {hua::copy_value(v.v,true)};}
template<class T,class E> Result<T,E> clone(const Result<T,E>& v){return Result<T,E>{v.ok,clone(v.value),clone(v.error),v.trace};}

template<class Tag> bool resource_edges(const Opaque<Tag>& value,FileScan&,unsigned depth){
    return hua::contains_file(value.v,depth);
}
template<class Tag> bool resource_edges(const Interface<Tag>& value,FileScan& scan,unsigned depth){
    const auto& object=value.object;
    return object.resources&&scan.first<Interface<Tag>>(object.data)&&object.resources(object.data,scan,depth);
}
template<class T> bool resource_edges(const Opt<T>& value,FileScan& scan,unsigned depth){
    return value&&file_in(*value,scan,depth+1);
}
template<class T,class E> bool resource_edges(const Result<T,E>& value,FileScan& scan,unsigned depth){
    return value.ok?file_in(value.value,scan,depth+1):file_in(value.error,scan,depth+1);
}
template<class T> bool resource_edges(const Slice<T>& value,FileScan& scan,unsigned depth){
    if(!scan.first<Slice<T>>(value.storage.get(),value.start,value.length))return false;
    for(I i=0;i<value.length;++i)
        if(file_in((*value.storage)[static_cast<std::size_t>(value.start+i)],scan,depth+1))return true;
    return false;
}
template<class T,std::size_t N> bool resource_edges(const Fixed<T,N>& value,FileScan& scan,unsigned depth){
    return file_in(value.view,scan,depth);
}
template<class T> bool resource_edges(const List<T>& value,FileScan& scan,unsigned depth){
    if(!scan.first<List<T>>(value.data.get()))return false;
    for(const auto& item:*value.data)if(file_in(item,scan,depth+1))return true;
    return false;
}
template<class K,class V> bool resource_edges(const Map<K,V>& value,FileScan& scan,unsigned depth){
    if(!scan.first<Map<K,V>>(value.data.get()))return false;
    for(const auto& item:value.data->items)if(file_in(item.second,scan,depth+1))return true;
    return false;
}
template<class T> bool resource_edges(const BoundMethod<T>& value,FileScan& scan,unsigned depth){
    return scan.first<BoundMethod<T>>(value.receiver.get())&&file_in(*value.receiver,scan,depth+1);
}
template<class T> BoundMethod<T> clone(const BoundMethod<T>& value){return value;}
inline DynamicValue clone(const DynamicValue& value){
    if(value.deep_copy)return value.deep_copy(value);
    return dynamic_scalar(hua::copy_value(value.scalar,true));
}
inline DynamicMultiple clone(const DynamicMultiple& value){
    DynamicMultiple result;
    result.values.reserve(value.values.size());
    for(const auto& item:value.values)result.values.push_back(clone(item));
    return result;
}
template<class T> T clone_dynamic_object(const T& value){return clone(value);}
template<class T> T clone_checked(const T& value,const SourceSpan& site){
    FileScan scan;
    if(file_in(value,scan,0))hua::runtime_error(site,"File cannot be cloned, including inside containers or closures","E4007");
    return clone(value);
}

// ---- typed std.list / std.strings (the rest goes through call_builtin)
template<class T> List<T> list_from_slice(const Slice<T>& v){
    std::vector<T> items;items.reserve(static_cast<std::size_t>(v.length));
    for(I i=0;i<v.length;++i)items.push_back(clone(T((*v.storage)[static_cast<std::size_t>(v.start+i)])));
    return make_list<T>(std::move(items));
}
template<class T> I list_len(const List<T>& l){return static_cast<I>(l.data->size());}
template<class T> Result<T,Str> list_get(const List<T>& l,I i,const SourceSpan& s){
    if(static_cast<std::uint64_t>(i)>=l.data->size())[[unlikely]]{static StrRep text(1u<<30,"list index out of bounds");return Result<T,Str>{false,T{},Str(&text),traced({},s,"Result error created here")};}
    const T item=(*l.data)[static_cast<std::size_t>(i)];
    return Result<T,Str>{true,l.writable?item:capture_value(item),Str{}};}
template<class T> Slice<T> list_snapshot(const List<T>& l){return Slice<T>::of(*l.data);}
inline Str static_text(const char* text){return Str(std::string(text));}
template<class T> Result<bool,Str> list_set(List<T>& l,I i,T v,const SourceSpan& s){
    require_writable(l,s);
    if(static_cast<std::uint64_t>(i)>=l.data->size())[[unlikely]]return Result<bool,Str>{false,false,static_text("list index out of bounds"),traced({},s,"Result error created here")};
    ++l.data->revision;(*l.data)[static_cast<std::size_t>(i)]=std::move(v);return Result<bool,Str>{true,true,Str{}};}
template<class T> Result<T,Str> list_pop(List<T>& l,const SourceSpan& s){
    require_writable(l,s);
    if(l.data->empty())[[unlikely]]return Result<T,Str>{false,T{},static_text("cannot pop an empty list"),traced({},s,"Result error created here")};
    ++l.data->revision;T v=std::move(l.data->back());l.data->pop_back();return Result<T,Str>{true,std::move(v),Str{}};}
template<class T> void list_clear(List<T>& l,const SourceSpan& s){require_writable(l,s);++l.data->revision;l.data->clear();}
// Typed std.list/std.maps for the common functions: the bridge would box the whole
// container on every call. Semantics follow collections_stdlib/binary_stdlib.
template<class T> I list_index_of(const List<T>& l,const T& v){for(std::size_t i=0;i<l.data->size();++i)if((*l.data)[i]==v)return static_cast<I>(i);return -1;}
template<class T> bool list_contains(const List<T>& l,const T& v){return list_index_of(l,v)>=0;}
template<class T> Result<I,Str> list_insert(List<T>& l,I at,T v,const SourceSpan& s){
    require_writable(l,s);
    if(at<0||static_cast<std::size_t>(at)>l.data->size())[[unlikely]]return Result<I,Str>{false,0,static_text("list index out of bounds"),traced({},s,"Result error created here")};
    if(l.data->size()>=1000000)[[unlikely]]return Result<I,Str>{false,0,static_text("list item limit exceeded"),traced({},s,"Result error created here")};
    ++l.data->revision;
    l.data->insert(l.data->begin()+at,std::move(v));return Result<I,Str>{true,static_cast<I>(l.data->size()),Str{}};}
template<class T> Result<T,Str> list_remove_at(List<T>& l,I at,const SourceSpan& s){
    require_writable(l,s);
    if(at<0||static_cast<std::size_t>(at)>=l.data->size())[[unlikely]]return Result<T,Str>{false,T{},static_text("list index out of bounds"),traced({},s,"Result error created here")};
    ++l.data->revision;
    T v=std::move((*l.data)[static_cast<std::size_t>(at)]);l.data->erase(l.data->begin()+at);return Result<T,Str>{true,std::move(v),Str{}};}
template<class T> Result<I,Str> list_extend(List<T>& l,const Slice<T>& items,const SourceSpan& s){
    require_writable(l,s);
    if(static_cast<std::size_t>(items.length)>1000000-l.data->size())[[unlikely]]return Result<I,Str>{false,0,static_text("list item limit exceeded"),traced({},s,"Result error created here")};
    ++l.data->revision;
    for(I i=0;i<items.length;++i)l.data->push_back((*items.storage)[static_cast<std::size_t>(items.start+i)]);
    return Result<I,Str>{true,static_cast<I>(l.data->size()),Str{}};}
template<class T> Result<bool,Str> list_reserve(List<T>& l,I n,const SourceSpan& s){
    require_writable(l,s);
    if(n<0||n>1000000)[[unlikely]]return Result<bool,Str>{false,false,static_text("list capacity outside 0..1000000"),traced({},s,"Result error created here")};
    ++l.data->revision;
    const bool changed=static_cast<std::size_t>(n)>l.data->capacity();if(changed)l.data->reserve(static_cast<std::size_t>(n));return Result<bool,Str>{true,changed,Str{}};}
template<class T> void list_reverse(List<T>& l,const SourceSpan& s){require_writable(l,s);++l.data->revision;std::reverse(l.data->begin(),l.data->end());}
template<class K,class V> Slice<K> map_keys(const Map<K,V>& m){std::vector<K> out;out.reserve(m.data->size());for(const auto& [k,v]:*m.data)out.push_back(k);return Slice<K>::of(std::move(out));}
template<class K,class V> Slice<V> map_values(const Map<K,V>& m){std::vector<V> out;out.reserve(m.data->size());for(const auto& [k,v]:*m.data)out.push_back(v);return Slice<V>::of(std::move(out));}
template<class K,class V> V map_get_or(const Map<K,V>& m,const K& k,V fallback){if(auto* f=m.data->find(k))return f->second;return fallback;}
template<class K,class V> Opt<V> map_pop(Map<K,V>& m,const K& k,const SourceSpan& s){
    require_writable(m,s,"cannot mutate a read-only map");
    return m.data->take(k);
}
template<class T> Result<I,Str> list_append(List<T>& l,T v,const SourceSpan& s){
    require_writable(l,s);
    if(l.data->size()>=1000000)[[unlikely]]{static StrRep text(1u<<30,"list item limit exceeded");return Result<I,Str>{false,0,Str(&text),traced({},s,"Result error created here")};}
    ++l.data->revision;
    l.data->push_back(std::move(v));return Result<I,Str>{true,static_cast<I>(l.data->size()),Str{}};}
// std.strings.split with a non-empty delimiter; the empty one (per character) and the item
// limit go through the VM's implementation.
inline Slice<Str> split(const Str& text,const Str& delimiter,const SourceSpan& s){
    const auto& a=text.text();const auto& d=delimiter.text();
    if(d.empty())[[unlikely]]return from_value<Slice<Str>>(call_builtin("$std$strings$split",s,{to_value(text),to_value(delimiter)},{false,false},"[]string"));
    std::vector<Str> out;std::size_t at=0;
    for(;;){auto next=a.find(d,at);out.emplace_back(a.substr(at,next==std::string::npos?next:next-at));
        if(out.size()>1000000)[[unlikely]]return from_value<Slice<Str>>(call_builtin("$std$strings$split",s,{to_value(text),to_value(delimiter)},{false,false},"[]string"));
        if(next==std::string::npos)break;at=next+d.size();}
    return Slice<Str>::of(std::move(out));}
inline bool starts_with(const Str& s,const Str& p){return s.text().size()>=p.text().size()&&std::memcmp(s.text().data(),p.text().data(),p.text().size())==0;}
inline bool ends_with(const Str& s,const Str& p){return s.text().size()>=p.text().size()&&std::memcmp(s.text().data()+s.text().size()-p.text().size(),p.text().data(),p.text().size())==0;}

// ---- defer: runs when the enclosing block exits (normally, by return/break/continue, or
// while an error unwinds; an error from the deferred call itself is then dropped, as the
// first error wins).
template<class F> struct Deferred {
    F f;int unwinding=std::uncaught_exceptions();
    explicit Deferred(F g):f(std::move(g)){}
    Deferred(const Deferred&)=delete;
    ~Deferred()noexcept(false){
        if(std::uncaught_exceptions()>unwinding){try{f();}catch(...){}return;}
        f();
    }
};
// A deferred call may suspend on Windows. Drain after leaving the catch
// handler, so the worker's C++ exception state does not cross a fiber switch.
struct TaskDefers {
    std::vector<std::function<void()>> calls;
    unsigned base_depth = current_state().depth;
    int unwinding = std::uncaught_exceptions();
    template<class F> Unit add(F&& call) { calls.emplace_back(std::forward<F>(call)); return {}; }
    ~TaskDefers() noexcept(false) {
        if (!calls.empty() && std::uncaught_exceptions() == unwinding) drain({});
    }
    void drain(std::exception_ptr primary);
};
struct SyncDefers {
    template<class F> auto add(F&& call) { return Deferred(std::forward<F>(call)); }
    static void drain(std::exception_ptr primary) { if (primary) std::rethrow_exception(primary); }
};
template<bool Tasks> using Defers = std::conditional_t<Tasks,TaskDefers,SyncDefers>;

inline Value task_record_field(const Value& input, const std::string& name) {
    if (auto record = hua::cget_if<hua::PlainRecord>(&input.data)) {
        if (auto field = hua::plain_record_field(*record, name)) return std::move(*field);
    }
    if (auto record = hua::cget_if<hua::StructValue>(&input.data)) {
        const auto& fields = std::as_const(record->data->fields);
        if (auto field = fields.find(name); field != fields.end()) return field->second;
    }
    bad_unbox("Task record field");
}

// Only supported leaves and acyclic composites can cross this native
// boundary. Build an independent typed tree before publishing the task:
// the native string and container reference counts are not atomic.
template<class T> requires std::is_arithmetic_v<T>
T task_input(T value) { return value; }
template<ExactName Name, class Rep>
Exact<Name, Rep> task_input(Exact<Name, Rep> value) { return value; }
inline Str task_input(const Str& value) { return Str(value.text()); }
inline Unit task_input(Unit value) { return value; }
inline Opaque<BytesTag> task_input(const Opaque<BytesTag>& value) { return value; }
template<class T> Opt<T> task_input(const Opt<T>& value);
template<class T, class E> Result<T,E> task_input(const Result<T,E>& value);
template<class T> Slice<T> task_input(const Slice<T>& value);
template<class T, std::size_t N> Fixed<T, N> task_input(const Fixed<T, N>& value);
template<class T> List<T> task_input(const List<T>& value);
template<class K, class V> Map<K, V> task_input(const Map<K, V>& value);

template<class T> Slice<T> task_input(const Slice<T>& value) {
    std::vector<T> items;
    items.reserve(static_cast<std::size_t>(value.length));
    for (I i = 0; i < value.length; ++i) {
        const auto& item = std::as_const(*value.storage)[static_cast<std::size_t>(value.start + i)];
        items.push_back(task_input(item));
    }
    return capture_value(Slice<T>::of(std::move(items)));
}
template<class T, std::size_t N>
Fixed<T, N> task_input(const Fixed<T, N>& value) {
    return Fixed<T, N>(task_input(value.view));
}
template<class T> List<T> task_input(const List<T>& value) {
    std::vector<T> items;
    items.reserve(value.data->size());
    for (const auto& item : std::as_const(*value.data)) items.push_back(task_input(item));
    return capture_value(make_list<T>(std::move(items)));
}
template<class K, class V> Map<K, V> task_input(const Map<K, V>& value) {
    auto result = new_map<K, V>();
    for (const auto& [key, item] : value.data->items)
        result.data->insert_or_assign(task_input(key), task_input(item));
    return capture_value(std::move(result));
}

template<class T> Opt<T> task_input(const Opt<T>& value) {
    if (!value) return std::nullopt;
    return task_input(*value);
}
template<class T, class E> Result<T,E> task_input(const Result<T,E>& value) {
    if (value.ok) return {true, task_input(value.value), E{}, {}};
    auto trace = value.trace ? Trace::make(*value.trace) : Trace{};
    return {false, T{}, task_input(value.error), std::move(trace)};
}

template<class T> std::optional<T> task_module_input(const std::optional<T>& slot) {
    if(!slot)return std::nullopt;
    return task_input(*slot);
}
template<bool Tasks,class Slot>
decltype(auto) module_value(Slot& slot,const SourceSpan& site,const char* name) {
    if constexpr(Tasks){
        if(!slot)hua::runtime_error(site,"name `" + std::string(name) + "` is not initialized","E4001");
        return *slot;
    }else return (slot);
}
using TaskBody = std::function<Value()>;
TaskHandle start_task(TaskBody body, const char* type, const SourceSpan& span);
Value await_task(const TaskHandle&, const SourceSpan&);
Value task_wait(const char* name, std::vector<Value> args, const SourceSpan&);
I task_group_begin();
struct ExternalExportSpec {
    const char* name;
    const char* linked_name;
    const char* result;
    std::vector<std::string> parameters;
};
void register_external_module(const char* id, const char* artifact, std::uint32_t abi_version,
                              std::vector<ExternalExportSpec> exports, const SourceSpan& site);
void init_external_module(const char* id, const SourceSpan& site);
Value call_external(const char* linked_name, const SourceSpan& site,
                    std::vector<Value> args, const std::vector<bool>& contextual);
int run_main(int argc,char** argv,const SourceSpan& main_site,Value (*body)(),bool tasks=false);
}

#include "hua/detail/native_types.hpp"
#include "hua/detail/native_algorithms.hpp"

#endif
