#include "hua/runtime.hpp"
#include <charconv>
#include <cmath>
#include <ostream>
namespace hua {
using I=std::int64_t;
Value invoke_builtin(const std::string& name,const std::vector<Value>& args,const SourceSpan& span,std::ostream& output) {
    auto arity=(name=="min"||name=="max")?2u:name=="clamp"?3u:1u;
    if(name!="print" && args.size()!=arity)runtime_error(span,"builtin argument count mismatch","E4003");
    if(name=="print"){for(std::size_t i=0;i<args.size();++i){if(i)output<<' ';output<<show(args[i]);}output<<'\n';return {};}
    if(name=="str")return Value(show(args[0]));
    if(name=="type")return Value(display_type(value_type(args[0])));
    if(name=="clone")return copy_value(args[0],true);
    if(name=="len") {
        if(auto p=std::get_if<SliceValue>(&args[0].data))return Value(static_cast<I>(p->length));
        if(auto p=std::get_if<std::string>(&args[0].data))return Value(static_cast<I>(p->size()));
        runtime_error(span,"len requires a slice or string","E4003");
    }
    if(name=="int") {
        if(args[0].data.index()==2)return args[0];
        if(auto p=std::get_if<double>(&args[0].data)) {
            if(!std::isfinite(*p) || *p< -9223372036854775808.0 || *p>=9223372036854775808.0)runtime_error(span,"float is outside int64 range","E4002");return Value(static_cast<I>(*p));
        }
        if(auto p=std::get_if<std::string>(&args[0].data)) {
            I value{};auto [end,err]=std::from_chars(p->data(),p->data()+p->size(),value);
            if(err!=std::errc{} || end!=p->data()+p->size())runtime_error(span,"invalid int conversion","E4003");return Value(value);
        }
    }
    if(name=="float") {
        if(args[0].data.index()==3)return args[0];
        if(auto p=std::get_if<I>(&args[0].data))return Value(static_cast<double>(*p));
        if(auto p=std::get_if<std::string>(&args[0].data)) {
            double value{};auto [end,err]=std::from_chars(p->data(),p->data()+p->size(),value);
            if(err!=std::errc{} || end!=p->data()+p->size() || !std::isfinite(value))runtime_error(span,"invalid float conversion","E4003");return Value(value);
        }
    }
    if(name=="sqrt") {
        double x=args[0].data.index()==2 ? static_cast<double>(std::get<I>(args[0].data)) : args[0].data.index()==3 ? std::get<double>(args[0].data):-1;
        if(x<0)runtime_error(span,"sqrt requires a nonnegative number","E4005");return Value(std::sqrt(x));
    }
    if(name=="abs") {
        if(args[0].data.index()==2 || args[0].data.index()==3) {
            auto negative=binary_value("<",args[0],args[0].data.index()==2?Value(I{0}):Value(0.0),span);
            return as_bool(negative,span)?unary_value("-",args[0],span):args[0];
        }
    }
    if(name=="min" || name=="max")return as_bool(binary_value(name=="min"?"<":">",args[0],args[1],span),span)?args[0]:args[1];
    if(name=="clamp") {
        if(as_bool(binary_value(">",args[1],args[2],span),span))runtime_error(span,"clamp lower bound exceeds upper bound","E4005");
        if(as_bool(binary_value("<",args[0],args[1],span),span))return args[1];
        if(as_bool(binary_value(">",args[0],args[2],span),span))return args[2];return args[0];
    }
    runtime_error(span,"invalid builtin argument type","E4003");
}
}
