#include "hua/stdlib.hpp"
#include "hua/tasks.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
namespace hua {
namespace {
using I=std::int64_t;
using Entries=std::map<MapKey,Value>;
constexpr std::size_t max_entries=1000000,max_items=1000000,max_work=32000000;
// HUA-D038 slice 2 (since 10). Every call does bounded native work and checks
// cancellation every 1024 units, like std.alg.
struct Work {
    const SourceSpan& span;RuntimeContext* context;std::size_t count{};
    void tick(std::size_t n=1){
        auto before=count;count+=n;
        if(count>max_work)runtime_error(span,"collection work limit exceeded (32000000)","E4099");
        if((before>>10)!=(count>>10))task_checkpoint(context,span);
    }
};
const MapValue& map_arg(const Value& v,const SourceSpan& s){auto p=std::get_if<MapValue>(&v.data);if(!p||!p->data)runtime_error(s,"expected map","E4003");return *p;}
void writable(const MapValue& m,const SourceSpan& s){if(!m.writable)runtime_error(s,"cannot mutate a read-only map","E4007");}
Value key_value(const MapKey& k){return std::visit([](const auto& x){return Value(x);},k);}
Value slice(std::shared_ptr<std::vector<Value>> values,const std::string& element){auto n=values->size();return Value(SliceValue{std::move(values),0,n,false,element});}
// Map reads follow index_value: the stored value, read-only through a read-only map.
Value read_entry(const MapValue& m,Value& v){retain_record_alias(v);return m.writable?v:read_only(v);}
Value new_set(const std::string& key,Entries entries){auto data=managed<MapData>();data->key_type=key;data->item_type="bool";data->entries=std::move(entries);return Value(MapValue{data,true});}
void add_key(Entries& out,const MapKey& k,const SourceSpan& s){if(out.size()>=max_entries)runtime_error(s,"map entry limit exceeded","E4099");out.emplace_hint(out.end(),k,Value(true));}

Value maps_standard(const StandardFunction& f,const std::vector<Value>& args,const SourceSpan& s,RuntimeContext* context){
    const auto& m=map_arg(args[0],s);auto& entries=m.data->entries;Work work{s,context};
    const auto& name=f.name;
    if(name=="keys"||name=="values"||name=="entries"){
        // Ascending key order is the contract of these three functions (D1).
        std::shared_ptr<std::vector<Value>> keys,values;
        if(name!="values"){keys=managed<std::vector<Value>>();keys->reserve(entries.size());}
        if(name!="keys"){values=managed<std::vector<Value>>();values->reserve(entries.size());}
        for(auto& [k,v]:entries){work.tick();if(keys)keys->push_back(key_value(k));if(values)values->push_back(copy_value(read_only(v)));}
        if(name=="keys")return slice(keys,m.data->key_type);
        if(name=="values")return slice(values,m.data->item_type);
        return make_multi({slice(keys,m.data->key_type),slice(values,m.data->item_type)});
    }
    if(name=="get_or"){auto found=map_find(m,args[1],s);if(found!=entries.end())return read_entry(m,found->second);return enforce_type(args[2],m.data->item_type,s);}
    if(name=="pop"){
        writable(m,s);auto found=map_find(m,args[1],s);if(found==entries.end())return {};
        auto value=std::move(found->second);entries.erase(found);return value;
    }
    if(name=="merge"){
        writable(m,s);const auto& source=map_arg(args[1],s);
        if(source.data->key_type!=m.data->key_type||source.data->item_type!=m.data->item_type)runtime_error(s,"merge requires identical map types","E4003");
        if(source.data==m.data)return Value(I{0});
        // Count first so an over-limit merge leaves dst unchanged; both passes walk
        // the two ordered trees together, so each is linear in their sizes.
        std::size_t added=0;auto d=entries.begin();
        for(const auto& [k,v]:source.data->entries){
            while(d!=entries.end()&&d->first<k){work.tick();++d;}
            work.tick();if(d==entries.end()||k<d->first)++added;
        }
        if(added>max_entries-entries.size())runtime_error(s,"map entry limit exceeded","E4099");
        d=entries.begin();
        for(auto& [k,v]:source.data->entries){
            while(d!=entries.end()&&d->first<k)++d;
            auto value=copy_value(source.writable?v:read_only(v));
            if(d!=entries.end()&&!(k<d->first)){d->second=std::move(value);++d;}
            else d=std::next(entries.emplace_hint(d,k,std::move(value)));
        }
        return Value(static_cast<I>(added));
    }
    runtime_error(s,"unknown map operation","E4003");
}

Value sets_standard(const StandardFunction& f,const std::vector<Value>& args,const SourceSpan& s,RuntimeContext* context){
    Work work{s,context};const auto& name=f.name;
    if(name=="from_slice"){
        auto input=std::get_if<SliceValue>(&args[0].data);
        if(!input||(input->element_type!="bool"&&input->element_type!="int"&&input->element_type!="string"))runtime_error(s,"set from_slice requires a bool/int/string slice or array","E4003");
        Entries out;
        for(std::size_t i=0;i<input->length;++i){
            work.tick();auto v=sequence_read(*input,i);MapKey k;
            if(auto p=std::get_if<bool>(&v.data))k=*p;else if(auto p=std::get_if<I>(&v.data))k=*p;else if(auto p=std::get_if<std::string>(&v.data))k=std::move(*p);else runtime_error(s,"set element type mismatch","E4003");
            // Ascending runs (common for generated keys) append at the end in O(1).
            if(!out.empty()&&std::prev(out.end())->first<k)add_key(out,k,s);
            else if(!out.contains(k)){if(out.size()>=max_entries)runtime_error(s,"map entry limit exceeded","E4099");out.emplace(std::move(k),Value(true));}
        }
        return new_set(input->element_type,std::move(out));
    }
    const auto& a=map_arg(args[0],s);const auto& b=map_arg(args[1],s);
    if(a.data->key_type!=b.data->key_type)runtime_error(s,"set operation requires maps with identical key types","E4003");
    const auto& x=a.data->entries;const auto& y=b.data->entries;
    auto i=x.begin();auto j=y.begin();
    if(name=="is_subset"||name=="is_disjoint"){
        const bool subset=name=="is_subset";
        while(i!=x.end()&&j!=y.end()){
            work.tick();
            if(i->first<j->first){if(subset)return Value(false);++i;}
            else if(j->first<i->first)++j;
            else{if(!subset)return Value(false);++i;++j;}
        }
        return Value(subset?i==x.end():true);
    }
    Entries out;
    const bool keep_a=name=="union"||name=="difference",keep_b=name=="union",keep_both=name!="difference";
    if(name!="union"&&name!="intersection"&&name!="difference")runtime_error(s,"unknown set operation","E4003");
    while(i!=x.end()||j!=y.end()){
        work.tick();
        if(j==y.end()||(i!=x.end()&&i->first<j->first)){if(keep_a)add_key(out,i->first,s);++i;}
        else if(i==x.end()||j->first<i->first){if(keep_b)add_key(out,j->first,s);++j;}
        else{if(keep_both)add_key(out,i->first,s);++i;++j;}
    }
    return new_set(a.data->key_type,std::move(out));
}

bool same_scalar(const Value& a,const Value& b,const SourceSpan& s,Work& work){
    if(auto x=std::get_if<I>(&a.data))if(auto y=std::get_if<I>(&b.data))return *x==*y;
    if(auto x=std::get_if<std::string>(&a.data))if(auto y=std::get_if<std::string>(&b.data)){work.tick(x->size()/64);return *x==*y;}
    if(auto x=std::get_if<double>(&a.data))if(auto y=std::get_if<double>(&b.data))return *x==*y;
    if(auto x=std::get_if<bool>(&a.data))if(auto y=std::get_if<bool>(&b.data))return *x==*y;
    return as_bool(binary_value("==",a,b,s),s);
}
int compare_sorted(const Value& a,const Value& b,Work& work){
    work.tick();
    if(auto x=std::get_if<I>(&a.data)){auto y=std::get<I>(b.data);return *x<y?-1:*x>y?1:0;}
    if(auto x=std::get_if<double>(&a.data)){auto y=std::get<double>(b.data);return *x<y?-1:*x>y?1:0;}
    const auto& x=std::get<std::string>(a.data);const auto& y=std::get<std::string>(b.data);
    auto n=std::min(x.size(),y.size());work.tick(n/64);
    if(auto c=n?std::memcmp(x.data(),y.data(),n):0)return c<0?-1:1;
    return x.size()<y.size()?-1:x.size()>y.size()?1:0;
}
}

Value invoke_collection_standard(const StandardFunction& f,const std::vector<Value>& args,const SourceSpan& s,RuntimeContext* context){
    if(args.size()!=f.parameters.size())runtime_error(s,"standard function argument count mismatch","E4003");
    if(f.module=="maps")return maps_standard(f,args,s,context);
    if(f.module=="sets")return sets_standard(f,args,s,context);
    runtime_error(s,"unknown collection operation","E4003");
}

// list P0 (since 10). invoke_standard has already specialized and enforced the
// List<T> signature and checked the write capability of mutating functions.
Value invoke_list_extra(const StandardFunction& f,const std::vector<Value>& args,const SourceSpan& s,RuntimeContext* context){
    auto handle=std::get_if<ListValue>(&args[0].data);if(!handle||!handle->data)runtime_error(s,"expected List<T>","E4003");
    auto& data=*handle->data;auto& values=data.values;Work work{s,context};const auto& name=f.name;
    auto ok=[&](Value v,const std::string& type){return standard_result(std::move(v),true,type);};
    auto err=[&](const std::string& text,const std::string& type){return standard_result(Value(text),false,type);};
    auto index=[&](std::size_t i){return as_int(args[i],s);};
    auto in_bounds=[&](I at,std::size_t limit){return at>=0&&static_cast<std::size_t>(at)<limit;};
    if(name=="insert"){
        auto at=index(1);if(!in_bounds(at,values.size()+1))return err("list index out of bounds","int");
        if(values.size()>=max_items)return err("list item limit exceeded","int");
        auto value=copy_value(enforce_type(args[2],data.element_type,s));work.tick(values.size()-static_cast<std::size_t>(at));
        values.insert(values.begin()+at,std::move(value));return ok(Value(static_cast<I>(values.size())),"int");
    }
    if(name=="remove_at"){
        auto at=index(1);if(!in_bounds(at,values.size()))return err("list index out of bounds",data.element_type);
        auto value=copy_value(read_only(values[static_cast<std::size_t>(at)]));work.tick(values.size()-static_cast<std::size_t>(at));
        values.erase(values.begin()+at);return ok(std::move(value),data.element_type);
    }
    if(name=="index_of"||name=="contains"){
        auto target=enforce_type(args[1],data.element_type,s);
        for(std::size_t i=0;i<values.size();++i){work.tick();if(same_scalar(values[i],target,s,work))return name=="contains"?Value(true):Value(static_cast<I>(i));}
        return name=="contains"?Value(false):Value(I{-1});
    }
    if(name=="dedup"){
        // Keep the first of each run of == neighbours (Rust Vec::dedup). Compare
        // first and compact after, so an error raised by == leaves the List unchanged.
        std::vector<char> keep(values.size(),1);std::size_t last=0;
        for(std::size_t i=1;i<values.size();++i){work.tick();if(same_scalar(values[i],values[last],s,work))keep[i]=0;else last=i;}
        std::size_t kept=0;
        for(std::size_t i=0;i<values.size();++i)if(keep[i]){if(i!=kept)values[kept]=std::move(values[i]);++kept;}
        auto removed=values.size()-kept;values.erase(values.begin()+static_cast<std::ptrdiff_t>(kept),values.end());
        return Value(static_cast<I>(removed));
    }
    if(name=="reverse"){work.tick(values.size());std::reverse(values.begin(),values.end());return {};}
    if(name=="sort"){
        const auto& type=data.element_type;
        if(type!="int"&&type!="float"&&type!="string")runtime_error(s,"list.sort requires int/float/string elements","E4003");
        for(const auto& v:values){if(value_type(v)!=type)runtime_error(s,"list element type mismatch","E4003");if(auto d=std::get_if<double>(&v.data);d&&!std::isfinite(*d))runtime_error(s,"list.sort requires finite float values","E4003");}
        // Sort a permutation, then move once: a work-limit or cancellation error
        // raised by the comparator leaves the List unchanged.
        std::vector<std::size_t> order(values.size());for(std::size_t i=0;i<order.size();++i)order[i]=i;
        std::stable_sort(order.begin(),order.end(),[&](std::size_t a,std::size_t b){return compare_sorted(values[a],values[b],work)<0;});
        std::vector<Value> sorted;sorted.reserve(values.size());for(auto i:order)sorted.push_back(std::move(values[i]));
        values.swap(sorted);return {};
    }
    if(name=="swap"){
        auto i=index(1),j=index(2);if(!in_bounds(i,values.size())||!in_bounds(j,values.size()))return err("list index out of bounds","bool");
        std::swap(values[static_cast<std::size_t>(i)],values[static_cast<std::size_t>(j)]);return ok(Value(true),"bool");
    }
    if(name=="truncate"){
        auto n=index(1);if(n<0)runtime_error(s,"list truncate length must not be negative","E4003");
        if(static_cast<std::uint64_t>(n)<values.size()){work.tick(values.size()-static_cast<std::size_t>(n));values.erase(values.begin()+n,values.end());}
        return {};
    }
    if(name=="slice"){
        auto start=index(1),end=index(2);auto type="[]"+data.element_type;
        if(start<0||end<start||static_cast<std::uint64_t>(end)>values.size())return err("list slice out of bounds",type);
        auto out=managed<std::vector<Value>>();out->reserve(static_cast<std::size_t>(end-start));
        for(auto i=start;i<end;++i){work.tick();out->push_back(copy_value(read_only(values[static_cast<std::size_t>(i)])));}
        return ok(slice(out,data.element_type),type);
    }
    runtime_error(s,"unknown list operation","E4003");
}
}
