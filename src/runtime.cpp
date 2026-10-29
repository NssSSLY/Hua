#include "hua/runtime.hpp"
#include "hua/tasks.hpp"
#include <charconv>
#include <cmath>
#include <ostream>
#include <mutex>
#include <string_view>
#include <unordered_map>
namespace hua {
using I=std::int64_t;
const std::unordered_set<std::string>& builtin_names() {
    static const std::unordered_set<std::string> names=[] {std::unordered_set<std::string> out={"print","str","int","float","len","clone","sqrt","min","max","abs","clamp","type","has","delete","ok","err","is_ok","is_err","unwrap","unwrap_err","unwrap_or","is_some","is_none","byte","i8","i16","i32","i64","u8","u16","u32","u64","f32","f64","usize","isize","sizeof","alignof","task_start","task_await","task_group_begin","task_group_end","parallel_map","simd_check"};for(const auto& f:standard_functions())out.insert(standard_name(f));return out;}();return names;
}
const BuiltinInfo& builtin_info(const std::string& name){
    static std::mutex mutex;static std::unordered_map<std::string,BuiltinInfo> table; // node-based: references stay valid
    std::lock_guard lock(mutex);
    if(auto found=table.find(name);found!=table.end())return found->second;
    static const std::unordered_map<std::string_view,CoreBuiltin> core={{"print",CoreBuiltin::print},{"str",CoreBuiltin::str},{"int",CoreBuiltin::int_},{"float",CoreBuiltin::float_},{"len",CoreBuiltin::len},{"clone",CoreBuiltin::clone},{"sqrt",CoreBuiltin::sqrt},{"min",CoreBuiltin::min},{"max",CoreBuiltin::max},{"abs",CoreBuiltin::abs},{"clamp",CoreBuiltin::clamp},{"type",CoreBuiltin::type},{"has",CoreBuiltin::has},{"delete",CoreBuiltin::delete_},{"ok",CoreBuiltin::ok},{"err",CoreBuiltin::err},{"is_ok",CoreBuiltin::is_ok},{"is_err",CoreBuiltin::is_err},{"unwrap",CoreBuiltin::unwrap},{"unwrap_err",CoreBuiltin::unwrap_err},{"unwrap_or",CoreBuiltin::unwrap_or},{"is_some",CoreBuiltin::is_some},{"is_none",CoreBuiltin::is_none},{"sizeof",CoreBuiltin::sizeof_},{"alignof",CoreBuiltin::alignof_},{"task_start",CoreBuiltin::task_start},{"task_await",CoreBuiltin::task_await},{"task_group_begin",CoreBuiltin::task_group_begin},{"task_group_end",CoreBuiltin::task_group_end},{"parallel_map",CoreBuiltin::parallel_map},{"simd_check",CoreBuiltin::simd_check}};
    BuiltinInfo info;
    if(auto c=core.find(name);c!=core.end())info.core=c->second;
    else if(numeric_spec(name))info.core=CoreBuiltin::numeric;
    info.standard=standard_function(name);
    // Mirrors builtin_may_suspend(name, context) in tasks.cpp.
    info.always_suspends=(info.standard&&info.standard->callback)||name=="task_await"||name=="task_group_end"||(info.standard&&info.standard->module=="task"&&(info.standard->name=="timeout"||info.standard->name=="yield"||info.standard->name=="sleep"));
    info.suspends_in_task=info.standard&&((info.standard->module=="gc"&&info.standard->name=="collect")||info.standard->module=="file"||info.standard->module=="fs"||info.standard->module=="io"||info.standard->module=="process");
    return table.emplace(name,info).first->second;
}
Value invoke_builtin(const std::string& name,const std::vector<Value>& args,const SourceSpan& span,std::ostream& output,const std::vector<bool>& contextual,const std::string& result_type,RuntimeContext* context) {
    return invoke_builtin(builtin_info(name),name,args,span,output,contextual,result_type,context);
}
Value invoke_builtin(const BuiltinInfo& info,const std::string& name,const std::vector<Value>& args,const SourceSpan& span,std::ostream& output,const std::vector<bool>& contextual,const std::string& result_type,RuntimeContext* context) {
    for(const auto& a:args)if(std::holds_alternative<MultiValue>(a.data))runtime_error(span,"multiple values require positional binding","E4003");
    if(info.core==CoreBuiltin::task_await){if(args.size()!=1||!context||!context->tasks)runtime_error(span,"invalid task await","E4103");return context->tasks->await(args[0],span);}
    if(info.core==CoreBuiltin::task_group_begin){if(!args.empty()||!context||!context->tasks)runtime_error(span,"invalid taskgroup","E4103");return Value(static_cast<I>(context->tasks->begin_group()));}
    if(info.core==CoreBuiltin::task_group_end){if(args.size()!=1||!context||!context->tasks)runtime_error(span,"invalid taskgroup","E4103");context->tasks->end_group(static_cast<std::size_t>(as_int(args[0],span)),span);return {};}
    if(info.core==CoreBuiltin::simd_check){if(args.size()!=1)runtime_error(span,"invalid SIMD metadata","E4104");auto array=std::get_if<SliceValue>(&args[0].data);if(!array||!numeric_spec(array->element_type))runtime_error(span,"SIMD requires a numeric array","E4104");return {};}
    if(info.standard)return record_failure_source(invoke_standard(*info.standard,args,span,output,context),span);
    auto arity=(info.core==CoreBuiltin::min||info.core==CoreBuiltin::max||info.core==CoreBuiltin::has||info.core==CoreBuiltin::delete_||info.core==CoreBuiltin::unwrap_or)?2u:info.core==CoreBuiltin::clamp?3u:1u;
    if(info.core!=CoreBuiltin::print && !(info.core==CoreBuiltin::ok&&args.empty()) && args.size()!=arity)runtime_error(span,"builtin argument count mismatch","E4003");
    if(info.core==CoreBuiltin::numeric){
        if(auto text=hua::string_data(&args[0].data)){auto spec=*numeric_spec(name);if(spec.category=='u'){std::uint64_t value{};auto [end,err]=std::from_chars(text->data(),text->data()+text->size(),value);if(err!=std::errc{}||end!=text->data()+text->size())runtime_error(span,"invalid unsigned conversion","E4003");return convert_numeric(Value(NumericValue{"u64",value}),name,span,true);}return convert_numeric(invoke_builtin(spec.category=='i'?"int":"float",args,span,output),name,span,true);}
        return convert_numeric(args[0],name,span,true);
    }
    if((info.core==CoreBuiltin::int_||info.core==CoreBuiltin::float_)&&std::holds_alternative<NumericValue>(args[0].data))return convert_numeric(args[0],name,span,true);
    if(info.core==CoreBuiltin::ok&&args.empty()){auto types=type_arguments(result_type,"Result");if(types.size()!=2||types[0]!="void"||types[1].empty())runtime_error(span,"ok() requires Result<void,E> metadata","E4003");return Value(ResultValue{true,nullptr,"void",types[1],false,false});}
    if(info.core==CoreBuiltin::ok||info.core==CoreBuiltin::err){bool ok=info.core==CoreBuiltin::ok;auto v=copy_value(args[0]);auto type=value_type(v);return record_failure_source(Value(ResultValue{ok,managed<Value>(std::move(v)),ok?type:"",ok?"":type,true,!contextual.empty()&&contextual[0]}),span);}
    if(info.core==CoreBuiltin::is_ok||info.core==CoreBuiltin::is_err){auto r=std::get_if<ResultValue>(&args[0].data);if(!r)runtime_error(span,"expected Result","E4003");return Value(r->ok==(info.core==CoreBuiltin::is_ok));}
    if(info.core==CoreBuiltin::is_some||info.core==CoreBuiltin::is_none)return Value((args[0].data.index()!=0)==(info.core==CoreBuiltin::is_some));
    if(info.core==CoreBuiltin::unwrap_err)return result_payload(args[0],false,span);
    if(info.core==CoreBuiltin::unwrap) {if(std::holds_alternative<ResultValue>(args[0].data))return result_payload(args[0],true,span);if(args[0].data.index()==0)runtime_error(span,"cannot unwrap nil Optional","E4009");return copy_value(args[0]);}
    if(info.core==CoreBuiltin::unwrap_or) {
        if(auto r=std::get_if<ResultValue>(&args[0].data)){auto fallback=enforce_type(args[1],r->success_type,span,contextual.size()>1&&contextual[1]);return r->ok?result_payload(args[0],true,span):copy_value(fallback);}
        if(args[0].data.index()==0)return copy_value(enforce_type(args[1],result_type,span,contextual.size()>1&&contextual[1]));enforce_type(args[1],value_type(args[0]),span,contextual.size()>1&&contextual[1]);return copy_value(args[0]);
    }
    if(info.core==CoreBuiltin::has||info.core==CoreBuiltin::delete_) {
        auto m=std::get_if<MapValue>(&args[0].data);if(!m)runtime_error(span,"expected map","E4003");if(info.core==CoreBuiltin::delete_&&!m->writable)runtime_error(span,"cannot mutate a read-only map","E4007");auto found=map_find(*m,args[1],span);if(found==m->data->entries.end())return Value(false);if(info.core==CoreBuiltin::delete_)m->data->entries.erase(found);return Value(true);
    }
    if(info.core==CoreBuiltin::print){for(std::size_t i=0;i<args.size();++i){if(i)output<<' ';output<<show(args[i]);}output<<'\n';return {};}
    if(info.core==CoreBuiltin::str)return Value(show(args[0]));
    if(info.core==CoreBuiltin::type)return Value(display_type(value_type(args[0])));
    if(info.core==CoreBuiltin::sizeof_||info.core==CoreBuiltin::alignof_)return Value(static_cast<I>(info.core==CoreBuiltin::sizeof_?value_size(args[0],span):value_alignment(args[0],span)));
    if(info.core==CoreBuiltin::clone){
        try{return copy_value(args[0],true);}
        catch(const Diagnostic& error){
            if(error.span().file_id.empty())throw error.located(span);
            throw;
        }
    }
    if(info.core==CoreBuiltin::len) {
        if(auto p=std::get_if<MapValue>(&args[0].data))return Value(static_cast<I>(p->data->entries.size()));
        if(auto p=std::get_if<SliceValue>(&args[0].data))return Value(static_cast<I>(p->length));
        if(auto p=hua::string_data(&args[0].data))return Value(static_cast<I>(p->size()));
        runtime_error(span,"len requires a slice or string","E4003");
    }
    if(info.core==CoreBuiltin::int_) {
        if(args[0].data.index()==2)return args[0];
        if(auto p=std::get_if<double>(&args[0].data)) {
            if(!std::isfinite(*p) || *p< -9223372036854775808.0 || *p>=9223372036854775808.0)runtime_error(span,"float is outside int64 range","E4002");return Value(static_cast<I>(*p));
        }
        if(auto p=hua::string_data(&args[0].data)) {
            I value{};auto [end,err]=std::from_chars(p->data(),p->data()+p->size(),value);
            if(err!=std::errc{} || end!=p->data()+p->size())runtime_error(span,"invalid int conversion","E4003");return Value(value);
        }
    }
    if(info.core==CoreBuiltin::float_) {
        if(args[0].data.index()==3)return args[0];
        if(auto p=std::get_if<I>(&args[0].data))return Value(static_cast<double>(*p));
        if(auto p=hua::string_data(&args[0].data)) {
            double value{};auto [end,err]=std::from_chars(p->data(),p->data()+p->size(),value);
            if(err!=std::errc{} || end!=p->data()+p->size() || !std::isfinite(value))runtime_error(span,"invalid float conversion","E4003");return Value(value);
        }
    }
    if(info.core==CoreBuiltin::sqrt) {
        double x=std::holds_alternative<NumericValue>(args[0].data)?std::get<double>(convert_numeric(args[0],"float",span,true).data):args[0].data.index()==2 ? static_cast<double>(std::get<I>(args[0].data)) : args[0].data.index()==3 ? std::get<double>(args[0].data):-1;
        if(x<0)runtime_error(span,"sqrt requires a nonnegative number","E4005");return Value(std::sqrt(x));
    }
    if(info.core==CoreBuiltin::abs) {
        if(std::holds_alternative<NumericValue>(args[0].data)){auto zero=convert_numeric(Value(I{0}),value_type(args[0]),span,true);return as_bool(binary_value("<",args[0],zero,span),span)?unary_value("-",args[0],span):args[0];}
        if(args[0].data.index()==2 || args[0].data.index()==3) {
            auto negative=binary_value("<",args[0],args[0].data.index()==2?Value(I{0}):Value(0.0),span);
            return as_bool(negative,span)?unary_value("-",args[0],span):args[0];
        }
    }
    if((info.core==CoreBuiltin::min||info.core==CoreBuiltin::max||info.core==CoreBuiltin::clamp)&&std::holds_alternative<NumericValue>(args[0].data)){auto numbers=args;for(std::size_t i=1;i<numbers.size();++i)numbers[i]=enforce_type(numbers[i],value_type(numbers[0]),span,i<contextual.size()&&contextual[i]);if(info.core==CoreBuiltin::min||info.core==CoreBuiltin::max)return as_bool(binary_value(info.core==CoreBuiltin::min?"<":">",numbers[0],numbers[1],span),span)?numbers[0]:numbers[1];if(as_bool(binary_value(">",numbers[1],numbers[2],span),span))runtime_error(span,"clamp lower bound exceeds upper bound","E4005");if(as_bool(binary_value("<",numbers[0],numbers[1],span),span))return numbers[1];if(as_bool(binary_value(">",numbers[0],numbers[2],span),span))return numbers[2];return numbers[0];}
    if(info.core==CoreBuiltin::min || info.core==CoreBuiltin::max)return as_bool(binary_value(info.core==CoreBuiltin::min?"<":">",args[0],args[1],span),span)?args[0]:args[1];
    if(info.core==CoreBuiltin::clamp) {
        if(as_bool(binary_value(">",args[1],args[2],span),span))runtime_error(span,"clamp lower bound exceeds upper bound","E4005");
        if(as_bool(binary_value("<",args[0],args[1],span),span))return args[1];
        if(as_bool(binary_value(">",args[0],args[2],span),span))return args[2];return args[0];
    }
    runtime_error(span,"invalid builtin argument type","E4003");
}
}
