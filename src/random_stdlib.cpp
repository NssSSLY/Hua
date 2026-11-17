#include "hua/stdlib.hpp"
#include "hua/random_engine.hpp"
#include <cstring>
#include <limits>
namespace hua {
namespace {
using I=std::int64_t;using U=std::uint64_t;
constexpr std::size_t max_bytes=128*1024*1024;
using random_engine::seed;
using random_engine::next;
using random_engine::bounded;
// Called before the generic argument checks: validates kinds and write capability itself.
RngState& state(const Value& v,const SourceSpan& s){
    auto p=hua::get_if<RngValue>(&v.data);if(!p||!p->data)runtime_error(s,"expected std.random.Rng","E4003");
    if(!p->writable)runtime_error(s,"standard mutation requires a writable argument","E4007");
    return *p->data;
}
I integer(const Value& v,const SourceSpan& s){auto p=hua::get_if<I>(&v.data);if(!p)runtime_error(s,"expected int","E4003");return *p;}
const SliceValue& slice_arg(const Value& v,const SourceSpan& s){auto p=cget_if<SliceValue>(&v.data);if(!p)runtime_error(s,"expected a slice or array","E4003");return *p;}
}

Value invoke_random_standard(const StandardFunction& f,const std::vector<Value>& args,const SourceSpan& s){
    const auto& name=f.name;
    if(args.size()!=f.parameters.size())runtime_error(s,"standard function argument count mismatch","E4003");
    if(name=="new"){auto data=managed<RngState>();seed(*data,integer(args[0],s));return Value(RngValue{data,true});}
    auto& r=state(args[0],s);
    if(name=="int"){
        auto lo=integer(args[1],s),hi=integer(args[2],s);
        if(lo>=hi)runtime_error(s,"random.int requires lo < hi","E4003");
        return Value(static_cast<I>(static_cast<U>(lo)+bounded(r,static_cast<U>(hi)-static_cast<U>(lo))));
    }
    if(name=="float")return Value(static_cast<double>(next(r)>>11)*0x1.0p-53);
    if(name=="bool")return Value(next(r)>>63!=0);
    if(name=="bytes"){
        auto n=integer(args[1],s);if(n<0)runtime_error(s,"random.bytes requires n >= 0","E4003");
        if(static_cast<U>(n)>max_bytes)runtime_error(s,"random.bytes exceeds 128 MiB","E4099");
        std::string out(static_cast<std::size_t>(n),'\0');
        for(std::size_t i=0;i<out.size();i+=8){U word=next(r);for(std::size_t j=0;j<8&&i+j<out.size();++j)out[i+j]=static_cast<char>(word>>(8*j)&255);}
        return Value(BytesValue{managed<const std::string>(std::move(out))});
    }
    if(name=="shuffle"){
        auto list=hua::get_if<ListValue>(&args[1].data);if(!list||!list->data)runtime_error(s,"random.shuffle requires List<T>","E4003");
        if(!list->writable)runtime_error(s,"standard mutation requires a writable argument","E4007");
        auto& values=list->data->values;++list->data->revision;
        for(auto i=values.size();i>1;--i)std::swap(values[i-1],values[static_cast<std::size_t>(bounded(r,i))]);
        return {};
    }
    const auto& xs=slice_arg(args[1],s);const auto& element=xs.element_type;
    if(name=="choice"){
        if(!xs.length)return standard_result(Value(std::string("RANDOM_EMPTY: choice requires non-empty input")),false,element);
        return standard_result(sequence_read(xs,static_cast<std::size_t>(bounded(r,xs.length))),true,element);
    }
    if(name=="sample"){
        auto k=integer(args[2],s);const auto type="[]"+element;
        if(k<0)runtime_error(s,"random.sample requires k >= 0","E4003");
        if(static_cast<U>(k)>xs.length)return standard_result(Value(std::string("RANDOM_RANGE: sample size exceeds input length")),false,type);
        std::vector<std::size_t> index(xs.length);for(std::size_t i=0;i<index.size();++i)index[i]=i;
        auto out=managed<std::vector<Value>>();out->reserve(static_cast<std::size_t>(k));
        for(std::size_t t=0;t<static_cast<std::size_t>(k);++t){auto j=t+static_cast<std::size_t>(bounded(r,index.size()-t));std::swap(index[t],index[j]);out->push_back(sequence_read(xs,index[t]));}
        auto n=out->size();return standard_result(Value(SliceValue{std::move(out),0,n,false,element}),true,type);
    }
    runtime_error(s,"unknown random operation","E4003");
}
}
