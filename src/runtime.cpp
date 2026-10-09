#include "hua/runtime.hpp"
#include "hua/tasks.hpp"
#include <charconv>
#include <cmath>
#include <ostream>
namespace hua {
using I=std::int64_t;
const std::unordered_set<std::string>& builtin_names() {
    static const std::unordered_set<std::string> names=[] {std::unordered_set<std::string> out={"print","str","int","float","len","clone","sqrt","min","max","abs","clamp","type","has","delete","ok","err","is_ok","is_err","unwrap","unwrap_err","unwrap_or","is_some","is_none","byte","i8","i16","i32","i64","u8","u16","u32","u64","f32","f64","usize","isize","sizeof","alignof","task_start","task_await","task_group_begin","task_group_end","parallel_map","simd_check"};for(const auto& f:standard_functions())out.insert(standard_name(f));return out;}();return names;
}
Value invoke_builtin(const std::string& name,const std::vector<Value>& args,const SourceSpan& span,std::ostream& output,const std::vector<bool>& contextual,const std::string& result_type,RuntimeContext* context) {
    for(const auto& a:args)if(std::holds_alternative<MultiValue>(a.data))runtime_error(span,"multiple values require positional binding","E4003");
    if(name=="task_await"){if(args.size()!=1||!context||!context->tasks)runtime_error(span,"invalid task await","E4103");return context->tasks->await(args[0],span);}
    if(name=="task_group_begin"){if(!args.empty()||!context||!context->tasks)runtime_error(span,"invalid taskgroup","E4103");return Value(static_cast<I>(context->tasks->begin_group()));}
    if(name=="task_group_end"){if(args.size()!=1||!context||!context->tasks)runtime_error(span,"invalid taskgroup","E4103");context->tasks->end_group(static_cast<std::size_t>(as_int(args[0],span)),span);return {};}
    if(name=="simd_check"){if(args.size()!=1)runtime_error(span,"invalid SIMD metadata","E4104");auto array=std::get_if<SliceValue>(&args[0].data);if(!array||!numeric_spec(array->element_type))runtime_error(span,"SIMD requires a numeric array","E4104");return {};}
    if(auto standard=standard_function(name))return invoke_standard(*standard,args,span,output,context);
    auto arity=(name=="min"||name=="max"||name=="has"||name=="delete"||name=="unwrap_or")?2u:name=="clamp"?3u:1u;
    if(name!="print" && !(name=="ok"&&args.empty()) && args.size()!=arity)runtime_error(span,"builtin argument count mismatch","E4003");
    if(numeric_spec(name)&&name!="int"&&name!="float"){
        if(auto text=std::get_if<std::string>(&args[0].data)){auto spec=*numeric_spec(name);if(spec.category=='u'){std::uint64_t value{};auto [end,err]=std::from_chars(text->data(),text->data()+text->size(),value);if(err!=std::errc{}||end!=text->data()+text->size())runtime_error(span,"invalid unsigned conversion","E4003");return convert_numeric(Value(NumericValue{"u64",value}),name,span,true);}return convert_numeric(invoke_builtin(spec.category=='i'?"int":"float",args,span,output),name,span,true);}
        return convert_numeric(args[0],name,span,true);
    }
    if((name=="int"||name=="float")&&std::holds_alternative<NumericValue>(args[0].data))return convert_numeric(args[0],name,span,true);
    if(name=="ok"&&args.empty()){auto types=type_arguments(result_type,"Result");if(types.size()!=2||types[0]!="void"||types[1].empty())runtime_error(span,"ok() requires Result<void,E> metadata","E4003");return Value(ResultValue{true,nullptr,"void",types[1],false,false});}
    if(name=="ok"||name=="err"){bool ok=name=="ok";auto v=copy_value(args[0]);return Value(ResultValue{ok,managed<Value>(v),ok?value_type(v):"",ok?"":value_type(v),true,!contextual.empty()&&contextual[0]});}
    if(name=="is_ok"||name=="is_err"){auto r=std::get_if<ResultValue>(&args[0].data);if(!r)runtime_error(span,"expected Result","E4003");return Value(r->ok==(name=="is_ok"));}
    if(name=="is_some"||name=="is_none")return Value((args[0].data.index()!=0)==(name=="is_some"));
    if(name=="unwrap_err")return result_payload(args[0],false,span);
    if(name=="unwrap") {if(std::holds_alternative<ResultValue>(args[0].data))return result_payload(args[0],true,span);if(args[0].data.index()==0)runtime_error(span,"cannot unwrap nil Optional","E4009");return copy_value(args[0]);}
    if(name=="unwrap_or") {
        if(auto r=std::get_if<ResultValue>(&args[0].data)){auto fallback=enforce_type(args[1],r->success_type,span,contextual.size()>1&&contextual[1]);return r->ok?result_payload(args[0],true,span):copy_value(fallback);}
        if(args[0].data.index()==0)return copy_value(enforce_type(args[1],result_type,span,contextual.size()>1&&contextual[1]));enforce_type(args[1],value_type(args[0]),span,contextual.size()>1&&contextual[1]);return copy_value(args[0]);
    }
    if(name=="has"||name=="delete") {
        auto m=std::get_if<MapValue>(&args[0].data);if(!m)runtime_error(span,"expected map","E4003");if(name=="delete"&&!m->writable)runtime_error(span,"cannot mutate a read-only map","E4007");auto key=map_key(*m,args[1],span);return Value(name=="has"?m->data->entries.contains(key):m->data->entries.erase(key)!=0);
    }
    if(name=="print"){for(std::size_t i=0;i<args.size();++i){if(i)output<<' ';output<<show(args[i]);}output<<'\n';return {};}
    if(name=="str")return Value(show(args[0]));
    if(name=="type")return Value(display_type(value_type(args[0])));
    if(name=="sizeof"||name=="alignof")return Value(static_cast<I>(name=="sizeof"?value_size(args[0],span):value_alignment(args[0],span)));
    if(name=="clone")return copy_value(args[0],true);
    if(name=="len") {
        if(auto p=std::get_if<MapValue>(&args[0].data))return Value(static_cast<I>(p->data->entries.size()));
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
        double x=std::holds_alternative<NumericValue>(args[0].data)?std::get<double>(convert_numeric(args[0],"float",span,true).data):args[0].data.index()==2 ? static_cast<double>(std::get<I>(args[0].data)) : args[0].data.index()==3 ? std::get<double>(args[0].data):-1;
        if(x<0)runtime_error(span,"sqrt requires a nonnegative number","E4005");return Value(std::sqrt(x));
    }
    if(name=="abs") {
        if(std::holds_alternative<NumericValue>(args[0].data)){auto zero=convert_numeric(Value(I{0}),value_type(args[0]),span,true);return as_bool(binary_value("<",args[0],zero,span),span)?unary_value("-",args[0],span):args[0];}
        if(args[0].data.index()==2 || args[0].data.index()==3) {
            auto negative=binary_value("<",args[0],args[0].data.index()==2?Value(I{0}):Value(0.0),span);
            return as_bool(negative,span)?unary_value("-",args[0],span):args[0];
        }
    }
    if((name=="min"||name=="max"||name=="clamp")&&std::holds_alternative<NumericValue>(args[0].data)){auto numbers=args;for(std::size_t i=1;i<numbers.size();++i)numbers[i]=enforce_type(numbers[i],value_type(numbers[0]),span,i<contextual.size()&&contextual[i]);if(name=="min"||name=="max")return as_bool(binary_value(name=="min"?"<":">",numbers[0],numbers[1],span),span)?numbers[0]:numbers[1];if(as_bool(binary_value(">",numbers[1],numbers[2],span),span))runtime_error(span,"clamp lower bound exceeds upper bound","E4005");if(as_bool(binary_value("<",numbers[0],numbers[1],span),span))return numbers[1];if(as_bool(binary_value(">",numbers[0],numbers[2],span),span))return numbers[2];return numbers[0];}
    if(name=="min" || name=="max")return as_bool(binary_value(name=="min"?"<":">",args[0],args[1],span),span)?args[0]:args[1];
    if(name=="clamp") {
        if(as_bool(binary_value(">",args[1],args[2],span),span))runtime_error(span,"clamp lower bound exceeds upper bound","E4005");
        if(as_bool(binary_value("<",args[0],args[1],span),span))return args[1];
        if(as_bool(binary_value(">",args[0],args[2],span),span))return args[2];return args[0];
    }
    runtime_error(span,"invalid builtin argument type","E4003");
}
}
