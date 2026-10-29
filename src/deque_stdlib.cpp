#include "hua/stdlib.hpp"
#include <algorithm>
#include <cstring>
namespace hua {
namespace {
using I=std::int64_t;
constexpr std::size_t max_items=1000000;
// std.deque and the natural-order std.heap functions run before the generic
// argument checks (one-element operations): they validate kinds, element types
// and write capability themselves.
DequeData& deque_arg(const Value& v,const SourceSpan& s,bool write){
    auto p=std::get_if<DequeValue>(&v.data);if(!p||!p->data)runtime_error(s,"expected Deque<T>","E4003");
    if(write&&!p->writable)runtime_error(s,"standard mutation requires a writable argument","E4007");
    return *p->data;
}
ListData& list_arg(const Value& v,const SourceSpan& s,bool write){
    auto p=std::get_if<ListValue>(&v.data);if(!p||!p->data)runtime_error(s,"expected List<T>","E4003");
    if(write&&!p->writable)runtime_error(s,"standard mutation requires a writable argument","E4007");
    return *p->data;
}
I integer(const Value& v,const SourceSpan& s){auto p=std::get_if<I>(&v.data);if(!p)runtime_error(s,"expected int","E4003");return *p;}
Value result(Value v,bool ok,const std::string& type){return standard_result(std::move(v),ok,type);}
Value read_item(const Value& v){return copy_value(read_only(v));}

// Natural order of int/float/string (floats are always finite in Hua).
enum class Kind{integer,real,text};
Kind kind_of(const std::string& type,const SourceSpan& s){
    if(type=="int")return Kind::integer;if(type=="float")return Kind::real;if(type=="string")return Kind::text;
    runtime_error(s,"heap requires int/float/string elements","E4003");
}
bool less(const Value& a,const Value& b,Kind k){
    if(k==Kind::integer)return std::get<I>(a.data)<std::get<I>(b.data);
    if(k==Kind::real)return std::get<double>(a.data)<std::get<double>(b.data);
    const auto& x=hua::string_value(a.data);const auto& y=hua::string_value(b.data);
    auto n=std::min(x.size(),y.size());auto c=n?std::memcmp(x.data(),y.data(),n):0;
    return c<0||(c==0&&x.size()<y.size());
}
void sift_up(std::vector<Value>& h,std::size_t i,Kind k){
    auto item=std::move(h[i]);
    while(i>0){auto parent=(i-1)/2;if(!less(item,h[parent],k))break;h[i]=std::move(h[parent]);i=parent;}
    h[i]=std::move(item);
}
void sift_down(std::vector<Value>& h,std::size_t i,Kind k){
    auto n=h.size();auto item=std::move(h[i]);
    while(2*i+1<n){
        auto child=2*i+1;if(child+1<n&&less(h[child+1],h[child],k))++child;
        if(!less(h[child],item,k))break;
        h[i]=std::move(h[child]);i=child;
    }
    h[i]=std::move(item);
}
}

Value invoke_deque_standard(const StandardFunction& f,const std::vector<Value>& args,const SourceSpan& s){
    if(args.size()!=f.parameters.size())runtime_error(s,"standard function argument count mismatch","E4003");
    const auto& name=f.name;
    if(name=="from_slice"){
        auto source=std::get_if<SliceValue>(&args[0].data);if(!source||source->element_type.empty())runtime_error(s,"from_slice requires a typed slice/array","E4003");
        if(source->length>max_items)runtime_error(s,"deque item limit exceeded","E4099");
        auto data=managed<DequeData>();data->element_type=source->element_type;
        for(std::size_t i=0;i<source->length;++i)data->values.push_back(copy_value(sequence_read(*source,i),true,0,true));
        return Value(DequeValue{data,true});
    }
    const bool write=name=="push_back"||name=="push_front"||name=="pop_back"||name=="pop_front"||name=="clear";
    auto& d=deque_arg(args[0],s,write);auto& values=d.values;const auto& element=d.element_type;
    if(name=="len")return Value(static_cast<I>(values.size()));
    if(name=="push_back"||name=="push_front"){
        if(values.size()>=max_items)return result(Value(std::string("deque item limit exceeded")),false,"int");
        auto item=copy_value(enforce_type(args[1],element,s));
        if(name=="push_back")values.push_back(std::move(item));else values.push_front(std::move(item));
        return result(Value(static_cast<I>(values.size())),true,"int");
    }
    if(name=="pop_back"||name=="pop_front"||name=="front"||name=="back"){
        if(values.empty())return result(Value(std::string("DEQUE_EMPTY: deque is empty")),false,element);
        const bool at_back=name=="pop_back"||name=="back";
        auto item=read_item(at_back?values.back():values.front());
        if(name=="pop_back")values.pop_back();else if(name=="pop_front")values.pop_front();
        return result(std::move(item),true,element);
    }
    if(name=="get"){
        auto i=integer(args[1],s);if(i<0||static_cast<std::size_t>(i)>=values.size())return result(Value(std::string("deque index out of bounds")),false,element);
        return result(read_item(values[static_cast<std::size_t>(i)]),true,element);
    }
    if(name=="clear"){values.clear();return {};}
    if(name=="snapshot"){auto out=managed<std::vector<Value>>();out->reserve(values.size());for(const auto& v:values)out->push_back(read_item(v));auto n=out->size();return Value(SliceValue{std::move(out),0,n,false,element});}
    runtime_error(s,"unknown deque operation","E4003");
}

Value invoke_heap_standard(const StandardFunction& f,const std::vector<Value>& args,const SourceSpan& s){
    if(args.size()!=f.parameters.size())runtime_error(s,"standard function argument count mismatch","E4003");
    const auto& name=f.name;
    if(name=="smallest"||name=="largest"){
        auto xs=std::get_if<SliceValue>(&args[0].data);if(!xs)runtime_error(s,"expected a slice or array","E4003");
        auto k=integer(args[1],s);if(k<0)runtime_error(s,"heap top-k requires k >= 0","E4003");
        auto kind=kind_of(xs->element_type,s);const auto type="[]"+xs->element_type;
        std::vector<Value> items;items.reserve(xs->length);for(std::size_t i=0;i<xs->length;++i)items.push_back(sequence_read(*xs,i));
        auto take=std::min<std::size_t>(static_cast<std::size_t>(k),items.size());
        // Scalar elements: equal values are indistinguishable, so stability does not matter.
        if(name=="smallest")std::partial_sort(items.begin(),items.begin()+static_cast<std::ptrdiff_t>(take),items.end(),[&](const Value& a,const Value& b){return less(a,b,kind);});
        else std::partial_sort(items.begin(),items.begin()+static_cast<std::ptrdiff_t>(take),items.end(),[&](const Value& a,const Value& b){return less(b,a,kind);});
        items.resize(take);auto out=managed<std::vector<Value>>(std::move(items));auto n=out->size();
        return result(Value(SliceValue{std::move(out),0,n,false,xs->element_type}),true,type);
    }
    const bool write=name!="peek";
    auto& h=list_arg(args[0],s,write);auto& values=h.values;
    auto kind=kind_of(h.element_type,s);const auto& element=h.element_type;
    if(write)++h.revision;
    if(name=="heapify"){for(auto i=values.size()/2;i-->0;)sift_down(values,i,kind);return {};}
    if(name=="push"){
        if(values.size()>=max_items)return result(Value(std::string("heap item limit exceeded")),false,"int");
        values.push_back(copy_value(enforce_type(args[1],element,s)));sift_up(values,values.size()-1,kind);
        return result(Value(static_cast<I>(values.size())),true,"int");
    }
    if(name=="pop"||name=="peek"){
        if(values.empty())return result(Value(std::string("HEAP_EMPTY: heap is empty")),false,element);
        auto top=read_item(values.front());
        if(name=="pop"){values.front()=std::move(values.back());values.pop_back();if(!values.empty())sift_down(values,0,kind);}
        return result(std::move(top),true,element);
    }
    runtime_error(s,"unknown heap operation","E4003");
}
}
