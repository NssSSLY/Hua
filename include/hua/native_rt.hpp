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
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>
namespace hn {
using I=std::int64_t;using F=double;
using hua::SourceSpan;using hua::Value;
struct Unit { bool operator==(const Unit&)const{return true;} };
struct Nil {};

// ---- execution state: budget, call depth and the call-site stack used for traces
struct State {
    std::int64_t steps_left{100000000000};std::uint64_t step_limit{100000000000};
    unsigned depth{0},call_limit{128};
    const SourceSpan* sites[160];
    hua::RuntimeContext* context{};
};
inline State state;
[[noreturn]] void step_limit_exceeded();
[[noreturn]] void depth_limit_exceeded(const SourceSpan&);
inline void step(){if(--state.steps_left<0)[[unlikely]]step_limit_exceeded();}
// A call from `site`: counted, depth-limited, and recorded for "called from" traces. The
// depth is restored by the caller on normal return only, so a diagnostic still sees the
// whole chain when it reaches main.
inline unsigned enter(const SourceSpan& site){
    const unsigned d=state.depth;if(d>=state.call_limit)[[unlikely]]depth_limit_exceeded(site);
    state.sites[d]=&site;state.depth=d+1;step();return d;
}
inline void leave(unsigned d){state.depth=d;}

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
inline void* pool_new(std::size_t size){return size<=Pool::classes*Pool::granule?pool.take(size):(::operator new(size));}
inline void pool_delete(void* p,std::size_t size){if(size<=Pool::classes*Pool::granule)pool.give(p,size);else ::operator delete(p);}
struct Pooled {
    static void* operator new(std::size_t size){return pool_new(size);}
    static void operator delete(void* p,std::size_t size){pool_delete(p,size);}
};
// ---- reference counting for one thread (the prototype has no tasks)
template<class T> class Ref {
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
// Immutable text. Literals are static and never freed.
struct StrRep:Pooled { std::uint32_t rc; std::string text; std::uint64_t hash{}; StrRep(std::uint32_t count,std::string t):rc(count),text(std::move(t)){} };
class Str {
    StrRep* p_;
    static StrRep* empty_rep(){static StrRep e(1u<<30,{});return &e;}
    void drop(){if(p_&&--p_->rc==0)delete p_;}
public:
    Str():p_(empty_rep()){++p_->rc;}
    explicit Str(std::string text):p_(new StrRep{1,std::move(text)}){}
    explicit Str(StrRep* literal):p_(literal){++p_->rc;}
    Str(const Str& o):p_(o.p_){++p_->rc;}
    Str(Str&& o)noexcept:p_(o.p_){o.p_=nullptr;}
    Str& operator=(const Str& o){if(p_!=o.p_){++o.p_->rc;drop();p_=o.p_;}return *this;}
    Str& operator=(Str&& o)noexcept{if(this!=&o){drop();p_=o.p_;o.p_=nullptr;}return *this;}
    ~Str(){drop();}
    const std::string& text()const{return p_->text;}
    bool operator==(const Str& o)const{return p_==o.p_||p_->text==o.p_->text;}
    // Cached FNV-1a over the bytes (0 is reserved for "not computed yet").
    std::uint64_t hash()const{
        if(!p_->hash){std::uint64_t h=0xcbf29ce484222325ULL;for(unsigned char c:p_->text){h^=c;h*=0x100000001b3ULL;}p_->hash=h?h:1;}
        return p_->hash;}
    bool operator<(const Str& o)const{return p_->text<o.p_->text;}
};
template<class T> using Opt=std::optional<T>;
// `trace` is set only on errors: where the error was created and each `?` it passed, as
// the VM records them for diagnostics.
using Trace=Ref<hua::SourceTrace>;
inline Trace traced(const Trace& before,const SourceSpan& span,const char* message){
    auto out=before?Trace::make(*before):Trace::make();out->add(span,message);return out;}
template<class T,class E> struct Result {
    bool ok{};T value{};E error{};Trace trace{};
};
template<class T> struct OkV { T value;template<class E> operator Result<T,E>()&&{return Result<T,E>{true,std::move(value),E{}};} };
template<class E> struct ErrV { E error;Trace trace;template<class T> operator Result<T,E>()&&{return Result<T,E>{false,T{},std::move(error),std::move(trace)};} };
template<class T> struct Slice {
    Ref<std::vector<T>> storage;I start{},length{};
    static Slice of(std::vector<T> items){Slice s;s.length=static_cast<I>(items.size());s.storage=Ref<std::vector<T>>::make(std::move(items));return s;}
};
template<class T> struct List { Ref<std::vector<T>> data; };
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
    std::pair<K,V>* find(const K& k){
        if(items.empty())return nullptr;const auto h=hash_of(k);const std::size_t mask=index.size()-1;
        for(std::size_t i=h&mask;;i=(i+1)&mask){const auto at=index[i];if(!at)return nullptr;if(hashes[at-1]==h&&items[at-1].first==k)return &items[at-1];}}
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
    auto begin(){ensure_sorted();return items.begin();}
    auto end(){return items.end();}
};
template<class K,class V> struct Map { Ref<MapRep<K,V>> data; };

// ---- static type names (the boxed forms carry them)
template<class T> struct TypeName;
template<> struct TypeName<I>{static std::string get(){return "int";}};
template<> struct TypeName<F>{static std::string get(){return "float";}};
template<> struct TypeName<bool>{static std::string get(){return "bool";}};
template<> struct TypeName<Str>{static std::string get(){return "string";}};
template<> struct TypeName<Unit>{static std::string get(){return "void";}};
template<class T> struct TypeName<Slice<T>>{static std::string get(){return "[]"+TypeName<T>::get();}};
template<class T> struct TypeName<List<T>>{static std::string get(){return "List<"+TypeName<T>::get()+">";}};
template<class T> struct TypeName<Opt<T>>{static std::string get(){return TypeName<T>::get()+"?";}};
template<class K,class V> struct TypeName<Map<K,V>>{static std::string get(){return "map["+TypeName<K>::get()+"]"+TypeName<V>::get();}};
template<class T,class E> struct TypeName<Result<T,E>>{static std::string get(){return "Result<"+TypeName<T>::get()+","+TypeName<E>::get()+">";}};

// ---- boxing: typed value -> VM Value (printing, cold library calls, failure paths)
inline Value to_value(I v){return Value(v);}
inline Value to_value(F v){return Value(v);}
inline Value to_value(bool v){return Value(v);}
inline Value to_value(const Str& v){return Value(v.text());}
inline Value to_value(Unit){return Value{};}
template<class T> Value to_value(const Opt<T>& v);
template<class T,class E> Value to_value(const Result<T,E>& v);
template<class T> Value to_value(const List<T>& v);
template<class K,class V> Value to_value(const Map<K,V>& v);
template<class T> Value to_value(const Slice<T>& v){
    auto storage=hua::managed<std::vector<Value>>();storage->reserve(static_cast<std::size_t>(v.length));
    for(I i=0;i<v.length;++i)storage->push_back(to_value((*v.storage)[static_cast<std::size_t>(v.start+i)]));
    return Value(hua::SliceValue{storage,0,static_cast<std::size_t>(v.length),true,TypeName<T>::get()});
}
template<class T> Value to_value(const Opt<T>& v){return v?to_value(*v):Value{};}
template<class T,class E> Value to_value(const Result<T,E>& v){
    Value payload=v.ok?to_value(v.value):to_value(v.error);
    hua::ResultValue out{v.ok,hua::result_box(std::move(payload)),TypeName<T>::get(),TypeName<E>::get(),true,false};
    if(v.trace)out.failure_source=std::make_shared<const hua::SourceTrace>(*v.trace);
    return Value(std::move(out));
}
template<class T> Value to_value(const List<T>& v){
    auto data=hua::managed<hua::ListData>();data->element_type=TypeName<T>::get();
    for(const auto& x:*v.data)data->values.push_back(to_value(x));
    return Value(hua::ListValue{data,true});
}
inline hua::MapKey map_key(bool k){return hua::MapKey(k);}
inline hua::MapKey map_key(I k){return hua::MapKey(k);}
inline hua::MapKey map_key(const Str& k){return hua::MapKey(k.text());}
template<class K,class V> Value to_value(const Map<K,V>& v){
    auto data=hua::managed<hua::MapData>();data->key_type=TypeName<K>::get();data->item_type=TypeName<V>::get();
    for(const auto& [k,x]:v.data->items)data->entries.emplace(map_key(k),to_value(x));
    return Value(hua::MapValue{data,true});
}
// ---- unboxing: VM Value -> typed value (results of cold library calls)
[[noreturn]] void bad_unbox(const char* expected);
template<class T> struct From;
template<class T> T from_value(const Value& v){return From<T>::get(v);}
template<> struct From<I>{static I get(const Value& v){if(auto p=hua::get_if<I>(&v.data))return *p;bad_unbox("int");}};
template<> struct From<F>{static F get(const Value& v){if(auto p=hua::get_if<F>(&v.data))return *p;bad_unbox("float");}};
template<> struct From<bool>{static bool get(const Value& v){if(auto p=hua::get_if<bool>(&v.data))return *p;bad_unbox("bool");}};
template<> struct From<Str>{static Str get(const Value& v){if(auto p=hua::string_data(&v.data))return Str(*p);bad_unbox("string");}};
template<> struct From<Unit>{static Unit get(const Value&){return {};}};
template<class T> struct From<Slice<T>>{static Slice<T> get(const Value& v){
    auto p=hua::cget_if<hua::SliceValue>(&v.data);if(!p)bad_unbox("slice");
    std::vector<T> items;items.reserve(p->length);for(std::size_t i=0;i<p->length;++i)items.push_back(from_value<T>(hua::sequence_read(*p,i)));
    return Slice<T>::of(std::move(items));}};
template<class T> struct From<Opt<T>>{static Opt<T> get(const Value& v){if(v.data.index()==0)return std::nullopt;return from_value<T>(v);}};
template<class T,class E> struct From<Result<T,E>>{static Result<T,E> get(const Value& v){
    auto p=hua::cget_if<hua::ResultValue>(&v.data);if(!p||!p->payload)bad_unbox("Result");
    if(p->ok)return Result<T,E>{true,from_value<T>(*p->payload),E{}};
    return Result<T,E>{false,T{},from_value<E>(*p->payload),p->failure_source?Trace::make(*p->failure_source):Trace{}};}};
template<class T> struct From<List<T>>{static List<T> get(const Value& v){
    auto p=hua::get_if<hua::ListValue>(&v.data);if(!p)bad_unbox("List");
    std::vector<T> items;items.reserve(p->data->values.size());for(const auto& x:p->data->values)items.push_back(from_value<T>(x));
    return List<T>{Ref<std::vector<T>>::make(std::move(items))};}};

// ---- library calls through the VM's implementation (boxed)
Value call_builtin(const char* name,const SourceSpan& span,std::vector<Value> args,const std::vector<bool>& contextual,const char* result_type);
// One bridged call site: the builtin is resolved once and its metadata kept, and the
// argument vector is reused, so a call costs the boxing of its arguments and nothing else.
struct Bridge {
    std::string name,result_type;std::vector<bool> contextual;const hua::BuiltinInfo* info;std::vector<Value> args;bool busy{};
    Bridge(const char* n,std::initializer_list<bool> flags,const char* result):name(n),result_type(result),contextual(flags),info(&hua::builtin_info(name)){}
};
Value call_bridge(Bridge& site,const SourceSpan& span,std::initializer_list<Value> args);
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
template<class T,class E,class D> T unwrap_or(Result<T,E> r,D fallback){return r.ok?std::move(r.value):T(std::move(fallback));}
template<class T,class D> T unwrap_or(Opt<T> v,D fallback){return v?std::move(*v):T(std::move(fallback));}

// ---- slices, maps
template<class T> const T& at(const Slice<T>& v,I i,const SourceSpan& s){
    if(static_cast<std::uint64_t>(i)>=static_cast<std::uint64_t>(v.length))[[unlikely]]{hua::index_value(to_value(v),Value(i),s);fail(s,"index out of bounds","E4006");}
    return (*v.storage)[static_cast<std::size_t>(v.start+i)];}
template<class T> T& slot(Slice<T>& v,I i,const SourceSpan& s){
    if(static_cast<std::uint64_t>(i)>=static_cast<std::uint64_t>(v.length))[[unlikely]]{hua::index_location(to_value(v),Value(i),s);fail(s,"index out of bounds","E4006");}
    return (*v.storage)[static_cast<std::size_t>(v.start+i)];}
template<class K,class V> Map<K,V> new_map(){return Map<K,V>{Ref<MapRep<K,V>>::make()};}
template<class K,class V> Opt<V> at(const Map<K,V>& m,const K& k,const SourceSpan&){auto f=m.data->find(k);if(!f)return std::nullopt;return f->second;}
template<class K,class V> bool has(const Map<K,V>& m,const K& k){return m.data->find(k)!=nullptr;}
template<class K,class V> void put(Map<K,V>& m,K k,V v,const SourceSpan& s){
    if(m.data->size()>=1000000&&!m.data->find(k))[[unlikely]]fail(s,"map entry limit exceeded","E4099");
    m.data->insert_or_assign(std::move(k),std::move(v));}
template<class K,class V> V& slot(Map<K,V>& m,const K& k,const SourceSpan& s){
    auto f=m.data->find(k);if(!f)[[unlikely]]fail(s,"compound assignment requires an existing map key","E4006");return f->second;}
// clone: a deep copy (handles get fresh storage)
inline I clone(I v){return v;}inline F clone(F v){return v;}inline bool clone(bool v){return v;}inline Str clone(const Str& v){return v;}
template<class T> Slice<T> clone(const Slice<T>& v){std::vector<T> items;items.reserve(static_cast<std::size_t>(v.length));for(I i=0;i<v.length;++i)items.push_back(clone((*v.storage)[static_cast<std::size_t>(v.start+i)]));return Slice<T>::of(std::move(items));}
template<class K,class V> Map<K,V> clone(const Map<K,V>& v){auto out=new_map<K,V>();for(const auto& [k,x]:v.data->items)out.data->insert_or_assign(k,clone(x));return out;}
template<class T> List<T> clone(const List<T>& v){std::vector<T> items;for(const auto& x:*v.data)items.push_back(clone(x));return List<T>{Ref<std::vector<T>>::make(std::move(items))};}
template<class T> Opt<T> clone(const Opt<T>& v){if(!v)return std::nullopt;return clone(*v);}

// ---- typed std.list / std.strings (the rest goes through call_builtin)
template<class T> List<T> list_from_slice(const Slice<T>& v){return List<T>{clone(v).storage};}
template<class T> I list_len(const List<T>& l){return static_cast<I>(l.data->size());}
template<class T> Result<T,Str> list_get(const List<T>& l,I i,const SourceSpan& s){
    if(static_cast<std::uint64_t>(i)>=l.data->size())[[unlikely]]{static StrRep text(1u<<30,"list index out of bounds");return Result<T,Str>{false,T{},Str(&text),traced({},s,"Result error created here")};}
    return Result<T,Str>{true,(*l.data)[static_cast<std::size_t>(i)],Str{}};}
template<class T> Slice<T> list_snapshot(const List<T>& l){return Slice<T>::of(*l.data);}
template<class T> Result<I,Str> list_append(List<T>& l,T v,const SourceSpan& s){
    if(l.data->size()>=1000000)[[unlikely]]{static StrRep text(1u<<30,"list item limit exceeded");return Result<I,Str>{false,0,Str(&text),traced({},s,"Result error created here")};}
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

// ---- entry
int run_main(int argc,char** argv,const SourceSpan& main_site,Value (*body)());
}
