#include "hua/callback.hpp"
#include "hua/tasks.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace hua {
namespace {
using I=std::int64_t;
constexpr std::size_t max_work=32000000,max_entries=1000000;
const Callable& callable_arg(const Value& v,const SourceSpan& s){auto p=std::get_if<Callable>(&v.data);if(!p||!p->function||!p->builtin.empty())runtime_error(s,"callback requires a Hua function","E4003");return *p;}
const SliceValue& slice_arg(const Value& v,const SourceSpan& s){auto p=std::get_if<SliceValue>(&v.data);if(!p)runtime_error(s,"expected a slice or array","E4003");return *p;}
bool mutable_parameter(const Node& p){return p.text.find(" mut")!=std::string::npos||(!p.children.empty()&&p.children[0]->kind==NodeKind::MutableType);}
struct Work {
    const SourceSpan& span;RuntimeContext* context;std::size_t count{};
    void tick(std::size_t n=1){
        auto before=count;count+=n;
        if(count>max_work)runtime_error(span,"callback work limit exceeded (32000000)","E4099");
        if((before>>10)!=(count>>10))task_checkpoint(context,span);
    }
};
void require_result(const CallbackShape& shape,std::string_view expected,const SourceSpan& s){if(shape.result!=expected)runtime_error(s,"callback must return "+std::string(expected),"E4003");}
void require_key(const CallbackShape& shape,const SourceSpan& s){if(shape.result!="int"&&shape.result!="float"&&shape.result!="string")runtime_error(s,"key callback must return int/float/string","E4003");}
Value checked_key(Value key,const SourceSpan& s){if(auto d=std::get_if<double>(&key.data);d&&!std::isfinite(*d))runtime_error(s,"sort keys require finite float values","E4003");return key;}
// Total order of int/float/string keys (finite floats), as std.alg/list.sort.
int compare_keys(const Value& a,const Value& b,Work& work){
    work.tick();
    if(auto x=std::get_if<I>(&a.data)){auto y=std::get<I>(b.data);return *x<y?-1:*x>y?1:0;}
    if(auto x=std::get_if<double>(&a.data)){auto y=std::get<double>(b.data);return *x<y?-1:*x>y?1:0;}
    const auto& x=hua::string_value(a.data);const auto& y=hua::string_value(b.data);
    auto n=std::min(x.size(),y.size());work.tick(n/64);
    if(auto c=n?std::memcmp(x.data(),y.data(),n):0)return c<0?-1:1;
    return x.size()<y.size()?-1:x.size()>y.size()?1:0;
}
std::vector<std::size_t> identity(std::size_t n){std::vector<std::size_t> out(n);for(std::size_t i=0;i<n;++i)out[i]=i;return out;}
std::vector<std::size_t> key_order(const std::vector<Value>& keys,Work& work){
    auto order=identity(keys.size());
    std::stable_sort(order.begin(),order.end(),[&](std::size_t a,std::size_t b){return compare_keys(keys[a],keys[b],work)<0;});
    return order;
}
// Bottom-up stable merge with a Hua `less`: the left run wins unless less(right,left).
// `after` runs after every callback (the List revision check of the in-place sorts).
template<class After> Routine<std::vector<std::size_t>> less_order(const std::vector<Value>& items,const Callable& less,CallbackHost& host,const SourceSpan& s,Work& work,After after){
    auto order=identity(items.size());std::vector<std::size_t> scratch(items.size());
    for(std::size_t width=1;width<items.size();width*=2){
        for(std::size_t start=0;start<items.size();start+=2*width){
            auto middle=std::min(start+width,items.size()),end=std::min(start+2*width,items.size());auto left=start,right=middle;
            for(auto out=start;out<end;++out){
                bool take_right=false;
                if(left<middle&&right<end){work.tick();take_right=as_bool(co_await host.invoke(less,{items[order[right]],items[order[left]]},s),s);after();}
                else take_right=left>=middle;
                scratch[out]=take_right?order[right++]:order[left++];
            }
        }
        order.swap(scratch);
    }
    co_return order;
}
std::vector<Value> elements(const SliceValue& xs){std::vector<Value> out;out.reserve(xs.length);for(std::size_t i=0;i<xs.length;++i)out.push_back(sequence_read(xs,i));return out;}
Value read_only_slice(std::vector<Value> values,const std::string& element){auto storage=managed<std::vector<Value>>(std::move(values));auto n=storage->size();return Value(SliceValue{std::move(storage),0,n,false,element});}
Value permuted(const std::vector<Value>& items,const std::vector<std::size_t>& order,const std::string& element){std::vector<Value> out;out.reserve(order.size());for(auto i:order)out.push_back(items[i]);return read_only_slice(std::move(out),element);}

Routine<Value> alg_callback(const StandardFunction& f,std::vector<Value> args,const SourceSpan& s,CallbackHost& host,Work& work){
    const auto& name=f.name;const auto xs=slice_arg(args[0],s);const auto& element=xs.element_type;
    if(name=="zip_with"){
        // Pairs up to the shorter input (Python zip, Kotlin zip).
        const auto ys=slice_arg(args[1],s);const auto& fn=callable_arg(args[2],s);auto shape=callback_shape(fn,2,s);
        const auto n=std::min(xs.length,ys.length);std::vector<Value> out;out.reserve(n);
        for(std::size_t i=0;i<n;++i){work.tick();out.push_back(co_await host.invoke(fn,{sequence_read(xs,i),sequence_read(ys,i)},s));}
        co_return read_only_slice(std::move(out),shape.result);
    }
    const bool fold=name=="fold",binary=name=="sorted_by";
    const auto& fn=callable_arg(args[fold?2:1],s);auto shape=callback_shape(fn,fold||binary?2:1,s);
    if(name=="map"){
        std::vector<Value> out;out.reserve(xs.length);
        for(std::size_t i=0;i<xs.length;++i){work.tick();out.push_back(co_await host.invoke(fn,{sequence_read(xs,i)},s));}
        co_return read_only_slice(std::move(out),shape.result);
    }
    if(fold){
        auto acc=std::move(args[1]);
        for(std::size_t i=0;i<xs.length;++i){work.tick();acc=co_await host.invoke(fn,{std::move(acc),sequence_read(xs,i)},s);}
        co_return acc;
    }
    if(name=="partition"){
        // Stable: matching elements, then the rest, each in input order.
        require_result(shape,"bool",s);std::vector<Value> yes,no;
        for(std::size_t i=0;i<xs.length;++i){work.tick();auto x=sequence_read(xs,i);(as_bool(co_await host.invoke(fn,{x},s),s)?yes:no).push_back(std::move(x));}
        co_return make_multi({read_only_slice(std::move(yes),element),read_only_slice(std::move(no),element)});
    }
    if(name=="filter"||name=="any"||name=="all"||name=="find"||name=="find_index"){
        require_result(shape,"bool",s);std::vector<Value> kept;
        for(std::size_t i=0;i<xs.length;++i){
            work.tick();auto x=sequence_read(xs,i);bool hit=as_bool(co_await host.invoke(fn,{x},s),s);
            if(name=="filter"){if(hit)kept.push_back(std::move(x));continue;}
            if(name=="all"){if(!hit)co_return Value(false);continue;}
            if(!hit)continue;
            if(name=="any")co_return Value(true);
            if(name=="find_index")co_return Value(static_cast<I>(i));
            co_return x;
        }
        if(name=="filter")co_return read_only_slice(std::move(kept),element);
        if(name=="any")co_return Value(false);
        if(name=="all")co_return Value(true);
        if(name=="find_index")co_return Value(I{-1});
        co_return Value{};
    }
    auto items=elements(xs);
    if(name=="sorted_by"){require_result(shape,"bool",s);auto order=co_await less_order(items,fn,host,s,work,[]{});co_return permuted(items,order,element);}
    require_key(shape,s);
    std::vector<Value> keys;keys.reserve(items.size());
    for(const auto& x:items){work.tick();keys.push_back(checked_key(co_await host.invoke(fn,{x},s),s));}
    if(name=="sorted_by_key")co_return permuted(items,key_order(keys,work),element);
    if(name=="min_by_key"||name=="max_by_key"){
        if(items.empty())co_return standard_result(Value(std::string("ALG_EMPTY: ")+name+" requires non-empty input"),false,element);
        std::size_t best=0;const int sign=name=="min_by_key"?1:-1;
        for(std::size_t i=1;i<keys.size();++i)if(sign*compare_keys(keys[i],keys[best],work)<0)best=i;
        co_return standard_result(items[best],true,element);
    }
    runtime_error(s,"unknown callback operation","E4003");
}

// heap.push_by_key/pop_by_key on List<T>: the sift path is found with key
// callbacks first (each followed by the List revision check), then applied once.
Routine<Value> heap_callback(const StandardFunction& f,std::vector<Value> args,const SourceSpan& s,CallbackHost& host,Work& work){
    auto handle=std::get_if<ListValue>(&args[0].data);if(!handle||!handle->data)runtime_error(s,"expected List<T>","E4003");
    if(!handle->writable)runtime_error(s,"standard mutation requires a writable argument","E4007");
    auto data=handle->data;auto& values=data->values;const auto& element=data->element_type;const bool push=f.name=="push_by_key";
    const auto& fn=callable_arg(args[push?2:1],s);auto shape=callback_shape(fn,1,s);require_key(shape,s);
    const auto revision=data->revision;
    auto unchanged=[data,revision,&s]{if(data->revision!=revision)runtime_error(s,"list modified during callback","E4003");};
    if(push){
        if(values.size()>=max_entries)co_return standard_result(Value(std::string("heap item limit exceeded")),false,"int");
        auto item=copy_value(enforce_type(args[1],element,s));
        auto key=checked_key(co_await host.invoke(fn,{item},s),s);unchanged();
        std::vector<std::size_t> path;
        for(auto i=values.size();i>0;){
            auto parent=(i-1)/2;work.tick();
            auto above=checked_key(co_await host.invoke(fn,{values[parent]},s),s);unchanged();
            if(compare_keys(key,above,work)>=0)break;
            path.push_back(parent);i=parent;
        }
        values.emplace_back();auto hole=values.size()-1;
        for(auto parent:path){values[hole]=std::move(values[parent]);hole=parent;}
        values[hole]=std::move(item);++data->revision;
        co_return standard_result(Value(static_cast<I>(values.size())),true,"int");
    }
    if(values.empty())co_return standard_result(Value(std::string("HEAP_EMPTY: heap is empty")),false,element);
    const auto n=values.size()-1;std::vector<std::size_t> path;
    if(n){
        auto key=checked_key(co_await host.invoke(fn,{values[n]},s),s);unchanged();
        for(std::size_t i=0;2*i+1<n;){
            auto child=2*i+1;work.tick();
            auto best=checked_key(co_await host.invoke(fn,{values[child]},s),s);unchanged();
            if(child+1<n){auto other=checked_key(co_await host.invoke(fn,{values[child+1]},s),s);unchanged();if(compare_keys(other,best,work)<0){++child;best=std::move(other);}}
            if(compare_keys(best,key,work)>=0)break;
            path.push_back(child);i=child;
        }
    }
    auto top=copy_value(read_only(values[0]));auto item=std::move(values[n]);values.pop_back();
    if(n){std::size_t hole=0;for(auto child:path){values[hole]=std::move(values[child]);hole=child;}values[hole]=std::move(item);}
    ++data->revision;
    co_return standard_result(std::move(top),true,element);
}

Routine<Value> list_callback(const StandardFunction& f,std::vector<Value> args,const SourceSpan& s,CallbackHost& host,Work& work){
    auto handle=std::get_if<ListValue>(&args[0].data);if(!handle||!handle->data)runtime_error(s,"expected List<T>","E4003");
    if(!handle->writable)runtime_error(s,"standard mutation requires a writable argument","E4007");
    auto data=handle->data;const auto& name=f.name;const auto& fn=callable_arg(args[1],s);
    auto shape=callback_shape(fn,name=="sort_by"?2:1,s);
    // E5: the result is computed first and written back once. Each callback is
    // followed by a revision check, so a callback that changes this List (through
    // a module global) stops the call before its elements are read again.
    const auto revision=data->revision;const auto& items=data->values;
    auto unchanged=[data,revision,&s]{if(data->revision!=revision)runtime_error(s,"list modified during callback","E4003");};
    if(name=="retain"){
        require_result(shape,"bool",s);std::vector<bool> keep(items.size());
        for(std::size_t i=0;i<items.size();++i){work.tick();keep[i]=as_bool(co_await host.invoke(fn,{items[i]},s),s);unchanged();}std::vector<Value> out;out.reserve(items.size());
        for(std::size_t i=0;i<items.size();++i)if(keep[i])out.push_back(std::move(data->values[i]));
        auto removed=items.size()-out.size();data->values.swap(out);++data->revision;co_return Value(static_cast<I>(removed));
    }
    std::vector<std::size_t> order;
    if(name=="sort_by"){require_result(shape,"bool",s);order=co_await less_order(items,fn,host,s,work,unchanged);}
    else{
        require_key(shape,s);std::vector<Value> keys;keys.reserve(items.size());
        for(std::size_t i=0;i<items.size();++i){work.tick();keys.push_back(checked_key(co_await host.invoke(fn,{items[i]},s),s));unchanged();}
        order=key_order(keys,work);
    }std::vector<Value> sorted;sorted.reserve(order.size());
    for(auto i:order)sorted.push_back(std::move(data->values[i]));
    data->values.swap(sorted);++data->revision;co_return Value{};
}

}

CallbackShape callback_shape(const Callable& callable,std::size_t arity,const SourceSpan& s){
    CallbackShape out;const auto& fn=*callable.function;
    for(const auto& c:fn.children){
        if(c->kind==NodeKind::Parameter){
            if(c->children.empty()||mutable_parameter(*c))runtime_error(s,"callback parameters require non-mut type annotations","E4003");
            ++out.parameters;
        }
        if(c->kind==NodeKind::ReturnTypes){if(c->children.size()!=1)runtime_error(s,"callback must return exactly one value","E4003");out.result=type_name(*c->children[0]);}
    }
    if(out.parameters!=arity)runtime_error(s,"callback parameter count mismatch","E4003");
    if(out.result.empty()||out.result.starts_with("Task<"))runtime_error(s,"callback requires an annotated non-async result","E4003");
    return out;
}

Routine<Value> invoke_callback_standard(const StandardFunction& f,std::vector<Value> args,const SourceSpan& s,CallbackHost& host,RuntimeContext* context){
    if(args.size()!=f.parameters.size())runtime_error(s,"standard function argument count mismatch","E4003");
    // A callback diagnostic propagates unchanged; its trace already names the
    // standard call site as "called from".
    Work work{s,context};
    if(f.module=="alg")co_return co_await alg_callback(f,std::move(args),s,host,work);
    if(f.module=="list")co_return co_await list_callback(f,std::move(args),s,host,work);
    if(f.module=="heap")co_return co_await heap_callback(f,std::move(args),s,host,work);
    if(f.module=="maps"&&f.name=="update"){
        // Inline (one call per element in counting loops): read, call, then write;
        // a callback that changes this map meanwhile is overwritten for `key`.
        auto m=std::get_if<MapValue>(&args[0].data);if(!m||!m->data)runtime_error(s,"expected map","E4003");
        if(!m->writable)runtime_error(s,"cannot mutate a read-only map","E4007");
        const auto& fn=callable_arg(args[3],s);callback_shape(fn,1,s);
        auto data=m->data;auto key=map_key(*m,args[1],s);Value current;
        if(auto found=data->entries.find(key);found!=data->entries.end()){retain_record_alias(found->second);current=found->second;}
        else current=enforce_type(args[2],data->item_type,s);
        auto next=enforce_type(co_await host.invoke(fn,{std::move(current)},s),data->item_type,s);
        if(data->entries.size()>=max_entries&&!data->entries.contains(key))runtime_error(s,"map entry limit exceeded","E4099");
        data->entries.insert_or_assign(std::move(key),copy_value(next));
        co_return next;
    }
    runtime_error(s,"unknown callback operation","E4003");
}
}
