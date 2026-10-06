#include <exception>
#include "hua/interpreter.hpp"
#include "hua/runtime.hpp"
#include "hua/tasks.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <ostream>
#include <unordered_set>
namespace hua {
namespace {
using N=NodeKind;using I=std::int64_t;
struct Returned { Value value; bool contextual{}; };
struct Broken {};struct Continued {};
template<class T> struct Restore {T& value;T old;Restore(T& v,T next):value(v),old(v){value=next;}~Restore(){value=old;}};
std::vector<const Node*> params(const Node& n) {std::vector<const Node*>out;for(const auto&c:n.children)if(c->kind==N::Parameter)out.push_back(c.get());return out;}
bool mutable_param(const Node& p){return p.text.find(" mut")!=std::string::npos || (!p.children.empty()&&p.children[0]->kind==N::MutableType);}
bool writable_value(const Value& v){if(auto p=std::get_if<BufferValue>(&v.data))return p->writable;if(auto p=std::get_if<ListValue>(&v.data))return p->writable;if(auto p=std::get_if<SliceValue>(&v.data))return p->writable;if(auto p=std::get_if<StructValue>(&v.data))return p->writable;if(auto p=std::get_if<MapValue>(&v.data))return p->writable;if(auto p=std::get_if<ResultValue>(&v.data))return p->writable;return false;}

Value read_member(const Value& v,bool writable){return writable ? v : read_only(v);}
}
Interpreter::Interpreter(const SemanticModel& model,std::ostream& output,RuntimeContext* context):model_(model),output_(output),context_(context) {}
Interpreter::Binding& Interpreter::Environment::lookup(const std::string& name,const SourceSpan& span) {
    for(auto env=this;env;env=env->parent)if(auto found=env->bindings.find(name);found!=env->bindings.end())return found->second;
    runtime_error(span,"name `"+name+"` is not initialized","E4001");
}
void Interpreter::tick(const Node& n) {task_checkpoint(context_,n.span);if(++steps_>1000000)runtime_error(n.span,"execution step limit exceeded (1000000)","E4099");}
void Interpreter::bind(const std::string& name,Value value,std::string type,bool mutable_binding,const SourceSpan& span) {
    if(std::holds_alternative<MultiValue>(value.data))runtime_error(span,"multiple values require positional binding","E4003");
    if(type.empty())type=value_type(value);
    value=copy_value(value);
    if(!mutable_binding)value=read_only(std::move(value));
    if(!environment_->bindings.emplace(name,Binding{std::move(value),std::move(type),mutable_binding}).second)
        runtime_error(span,"duplicate runtime binding `"+name+"`","E4001");
}
void Interpreter::run(const Node& program) {
    ExecutionScope execution(context_,output_);TypeRelationScope relation_scope(model_.relations);
    if(model_.external)model_.external->reset();
    steps_=0;calls_=0;global_.bindings.clear();builtins_.bindings.clear();builtins_.parent=nullptr;global_.parent=&builtins_;environment_=&global_;
    for(const auto& name:builtin_names()){builtins_.bindings.emplace(name,Binding{Value(Callable{nullptr,name,{}}),"builtin:"+name,false});builtins_.bindings.emplace("$core$"+name,Binding{Value(Callable{nullptr,name,{}}),"builtin:"+name,false});}
    for(const auto& [name,fn]:model_.functions)if(!model_.nested.contains(fn))global_.bindings.insert_or_assign(name,Binding{Value(Callable{fn,"",{}}),"fn:"+name,false});
    try{
        for(const auto& n:program.children)if(n->kind!=N::Function && n->kind!=N::Struct)execute(*n);
        if(auto found=model_.functions.find("main");found!=model_.functions.end()){auto result=call(Callable{found->second,"",{}},{},*found->second);if(std::holds_alternative<TaskValue>(result.data))context_->tasks->await(result,found->second->span);}
        execution.finish();global_.bindings.clear();builtins_.bindings.clear();
    }catch(...){global_.bindings.clear();builtins_.bindings.clear();throw;}
}
void Interpreter::block(const Node& n,bool scope) {
    Environment local{environment_,{}};Restore guard(environment_,scope?&local:environment_);
    try{for(const auto& c:n.children)execute(*c);}
    catch(const Diagnostic&){Restore failed(context_->unwinding,true);auto original=std::current_exception();try{drain(*environment_);}catch(...){}std::rethrow_exception(original);}
    catch(...){auto original=std::current_exception();drain(*environment_);std::rethrow_exception(original);}
    drain(*environment_);
}
Callable Interpreter::closure(const Node& function){
    auto captured=managed<Environment>();captured->parent=&global_;
    for(auto env=environment_;env&&env!=&global_;env=env->parent)for(const auto& [name,binding]:env->bindings)
        if(!captured->bindings.contains(name))captured->bindings.emplace(name,Binding{read_only(copy_value(binding.value)),binding.type,false});
    return Callable{&function,"",{},captured};
}
void Interpreter::drain(Environment& env){
    Restore cleanup(context_->cleanup,true);auto deferred=std::move(env.deferred);env.deferred.clear();std::exception_ptr first;
    for(auto i=deferred.rbegin();i!=deferred.rend();++i)try{call(i->callable,i->arguments,*i->site);}catch(...){if(!first)first=std::current_exception();}
    if(first)std::rethrow_exception(first);
}
void Interpreter::execute(const Node& n) {
    tick(n);
    switch(n.kind) {
    case N::Function:bind(declared_name(n),Value(closure(n)),"fn:"+declared_name(n),false,n.span);break;
    case N::Defer:{const auto& expression=*n.children[0];auto callee=evaluate(*expression.children[0]);auto fn=std::get_if<Callable>(&callee.data);if(!fn)runtime_error(n.span,"defer requires a callable","E4003");std::vector<Value> args;for(std::size_t i=1;i<expression.children.size();++i)args.push_back(copy_value(evaluate(*expression.children[i])));environment_->deferred.push_back({*fn,std::move(args),&expression});break;}
    case N::ExternalInit:if(context_&&context_->in_task)runtime_error(n.span,"Native/WASM initialization cannot run in tasks","E4104");model_.external->initialize(n.text,n.span);break;
    case N::Block:block(n);break;
    case N::Let:case N::Var:case N::Const: {
        auto type=model_.types.at(&n);
        auto value=n.kind==N::Const ? model_.constants.at(&n):evaluate(*n.children.back());
        value=enforce_type(std::move(value),type,n.span,contextual_literal(*n.children.back()));
        bind(n.text,std::move(value),type,n.kind==N::Var,n.span);break;
    }
    case N::MultiBinding:case N::MultiAssignment: {
        const auto& list=*n.children[0];std::vector<Location> locations;
        if(n.kind==N::MultiAssignment)for(const auto& t:list.children){if(t->kind==N::Name&&t->text=="_")locations.push_back({});else {auto l=locate(*t);if(!l.writable)runtime_error(t->span,"cannot mutate a read-only value/view","E4007");locations.push_back(std::move(l));}}
        auto pack=evaluate(*n.children[1]);auto values=unpack_multi(pack,list.children.size(),n.span);auto data=std::get<MultiValue>(pack.data).data;
        for(std::size_t i=0;i<values.size();++i){auto type=n.kind==N::MultiBinding?(list.children[i]->text=="_"?"":model_.types.at(list.children[i].get())):locations[i].type;values[i]=copy_value(enforce_type(std::move(values[i]),type,n.span,i<data->contextual.size()&&data->contextual[i]));}
        if(n.kind==N::MultiBinding){for(std::size_t i=0;i<values.size();++i)if(list.children[i]->text!="_")bind(list.children[i]->text,values[i],model_.types.at(list.children[i].get()),n.text=="var",n.span);}
        else for(std::size_t i=0;i<values.size();++i)if(locations[i].value||locations[i].map||locations[i].sequence)location_write(locations[i],values[i]);break;
    }
    case N::ExpressionStatement:evaluate(*n.children[0]);break;
    case N::Match:{auto value=evaluate(*n.children[0]);Value tag=value;auto object=std::get_if<StructValue>(&value.data);if(object&&object->data->fields.contains("$case"))tag=object->data->fields.at("$case");for(std::size_t i=1;i<n.children.size();++i){const auto& arm=*n.children[i];const auto& pattern=*arm.children[0];bool any=pattern.kind==N::Name&&display_type(pattern.text)=="_";if(!any&&!as_bool(binary_value("==",tag,model_.constants.at(&pattern),pattern.span),pattern.span))continue;Environment local{environment_,{}};Restore guard(environment_,&local);if(!any&&object){auto name=show(tag);for(std::size_t j=0;j<arm.children[1]->children.size();++j){const auto& binding=*arm.children[1]->children[j];if(binding.text=="_")continue;auto field="$"+name+"#"+std::to_string(j)+"#"+model_.types.at(&binding);bind(binding.text,enforce_type(copy_value(object->data->fields.at(field)),model_.types.at(&binding),binding.span),model_.types.at(&binding),false,binding.span);}}block(*arm.children[2],false);break;}break;}
    case N::If:if(as_bool(evaluate(*n.children[0]),n.span))execute(*n.children[1]);else if(n.children.size()==3)execute(*n.children[2]);break;
    case N::While:
        while(as_bool(evaluate(*n.children[0]),n.span)){try{execute(*n.children[1]);}catch(const Continued&){continue;}catch(const Broken&){break;}}break;
    case N::For: {
        auto count=n.children.size()-2;const auto& iterable=*n.children[count];
        auto iteration=[&](Value index,Value item) {
            Environment local{environment_,{}};Restore guard(environment_,&local);
            if(count==2)bind(n.children[0]->text,std::move(index),"",false,n.span);
            bind(n.children[count-1]->text,std::move(item),"",false,n.span);
            try{block(*n.children.back(),false);}catch(const Continued&){return true;}catch(const Broken&){return false;}return true;
        };
        if(iterable.kind==N::Range) {
            I start=as_int(evaluate(*iterable.children[0]),n.span),end=as_int(evaluate(*iterable.children[1]),n.span);
            I step=iterable.children.size()==3 ? as_int(evaluate(*iterable.children[2]),n.span):1;
            if(!step)runtime_error(iterable.span,"range step cannot be zero","E4005");
            const bool inclusive=iterable.text=="..=";I index=0;
            for(I value=start;step>0 ? (inclusive ? value<=end:value<end):(inclusive ? value>=end:value>end);) {
                tick(n);if(!iteration(Value(index),Value(value)))break;
                if(inclusive && value==end)break;
                // A mathematically overflowing next step is outside this bounded int64 range, so terminate.
                if((step>0 && value>std::numeric_limits<I>::max()-step)||(step<0 && value<std::numeric_limits<I>::min()-step))break;
                value+=step;++index;
            }
        } else {
            auto value=evaluate(iterable);
            if(auto m=std::get_if<MapValue>(&value.data)){std::vector<std::pair<Value,Value>> entries;for(const auto& [k,v]:m->data->entries)entries.emplace_back(std::visit([](const auto& x){return Value(x);},k),copy_value(m->writable?v:read_only(v)));for(const auto& [k,v]:entries){tick(n);if(!iteration(k,count==1?k:v))break;}break;}
            auto slice=std::get_if<SliceValue>(&value.data);
            if(!slice)runtime_error(iterable.span,"for requires a slice or range","E4003");
            const auto view=*slice;
            for(std::size_t i=0;i<view.length;++i){tick(n);if(!iteration(Value(static_cast<I>(i)),read_member(sequence_read(view,i),view.writable)))break;}
        }
        break;
    }
    case N::Return:
        if(n.children.size()>1){std::vector<Value> values;std::vector<bool> contextual;for(const auto& c:n.children){values.push_back(copy_value(evaluate(*c)));contextual.push_back(contextual_literal(*c));}throw Returned{make_multi(std::move(values),std::move(contextual)),false};}
        throw Returned{n.children.empty()?Value{}:copy_value(evaluate(*n.children[0])),!n.children.empty()&&contextual_literal(*n.children[0])};
    case N::Break:throw Broken{};case N::Continue:throw Continued{};
    case N::Import:runtime_error(n.span,"module loading requires the ModuleLoader entry point","E4008");
    case N::Unsafe:runtime_error(n.span,"unsafe execution/FFI is not implemented","E4008");
    default:runtime_error(n.span,"statement is not executable in Phase 2","E4008");
    }
}
Interpreter::Location Interpreter::locate(const Node& n) {
    tick(n);
    if(n.kind==N::Name){auto& b=environment_->lookup(n.text,n.span);return {&b.value,b.type,b.mutable_binding,{},{},{}};}
    if(n.kind==N::Member) {
        auto base=evaluate(*n.children[0]);auto object=std::get_if<StructValue>(&base.data);
        if(!object)runtime_error(n.span,"field assignment requires a struct","E4003");
        auto field=object->data->fields.find(n.text);if(field==object->data->fields.end())runtime_error(n.span,"unknown struct field","E4003");
        auto declaration=model_.structures.at(object->data->name);std::string type;
        for(const auto& f:declaration->children)if(f->text==n.text)type=type_name(*f->children[0]);
        return {&field->second,type,object->writable,object->data,{},{}};
    }
    if(n.kind==N::Index) {
        auto base=evaluate(*n.children[0]);if(!std::holds_alternative<SliceValue>(base.data)&&!std::holds_alternative<MapValue>(base.data))runtime_error(n.span,"index assignment requires a slice or map","E4003");
        auto index=evaluate(*n.children[1]);return index_location(std::move(base),std::move(index),n.span);
    }
    runtime_error(n.span,"invalid assignment target","E4003");
}
Value Interpreter::evaluate(const Node& n) {
    tick(n);
    switch(n.kind) {
    case N::Integer:case N::Float:case N::String:case N::Boolean:case N::Nil:return literal(n);
    case N::Name:{auto& b=environment_->lookup(n.text,n.span);return b.value;}
    case N::Pack:{std::vector<Value> values;std::vector<bool> contextual;for(const auto& c:n.children){values.push_back(copy_value(evaluate(*c)));contextual.push_back(contextual_literal(*c));}return make_multi(std::move(values),std::move(contextual));}
    case N::MapLiteral:{auto value=make_map(type_name(*n.children[0]),n.span);auto& map=std::get<MapValue>(value.data);for(std::size_t i=1;i<n.children.size();++i){auto key=evaluate(*n.children[i]->children[0]);auto item=evaluate(*n.children[i]->children[1]);map_put(map,key,std::move(item),n.children[i]->span,contextual_literal(*n.children[i]->children[1]));}return value;}
    case N::Propagate:{auto value=evaluate(*n.children[0]);auto r=std::get_if<ResultValue>(&value.data);if(!r)runtime_error(n.span,"expected Result","E4003");if(!r->ok)throw Returned{propagated_error(value),false};return result_payload(value,true,n.span);}
    case N::Unary: {
        if(n.text=="&")runtime_error(n.span,"safe reference execution is not implemented","E4008");
        if(numeric_literal(n))return literal(n);
        return unary_value(n.text,evaluate(*n.children[0]),n.span);
    }
    case N::Binary: {
        auto left=evaluate(*n.children[0]);
        if(n.text=="&&" && !as_bool(left,n.span))return Value(false);
        if(n.text=="||" && as_bool(left,n.span))return Value(true);
        return binary_value(n.text,left,evaluate(*n.children[1]),n.span,numeric_literal(*n.children[1]));
    }
    case N::Assignment:case N::Update: {
        auto where=locate(*n.children[0]);
        if(!where.writable)runtime_error(n.span,"cannot mutate immutable binding or read-only view","E4007");
        Value value;
        if(n.kind==N::Update)value=binary_value(n.text=="++"?"+":"-",location_read(where,n.span),Value(I{1}),n.span,true);
        else {
            value=evaluate(*n.children[1]);
            if(n.text!="=")value=binary_value(n.text.substr(0,n.text.size()-1),location_read(where,n.span),value,n.span,numeric_literal(*n.children[1]));
        }
        value=enforce_type(std::move(value),where.type,n.span,n.kind==N::Update||n.text!="="||contextual_literal(*n.children[1]));
        location_write(where,value);return value;
    }
    case N::Array: {
        auto storage=managed<std::vector<Value>>();std::string type;
        for(const auto& c:n.children){auto value=copy_value(evaluate(*c));if(type.empty())type=value_type(value);else value=enforce_type(value,type,c->span,numeric_literal(*c));storage->push_back(std::move(value));}
        SliceValue array{storage,0,storage->size(),true,type};for(const auto& child:n.children)array.contextual.push_back(contextual_literal(*child));return Value(array);
    }
    case N::Index:{auto base=evaluate(*n.children[0]);if(!std::holds_alternative<SliceValue>(base.data)&&!std::holds_alternative<MapValue>(base.data))runtime_error(n.span,"index/slice requires a slice, array or map","E4003");auto key=evaluate(*n.children[1]);return index_value(base,key,n.span);}
    case N::Slice: {
        auto base=evaluate(*n.children[0]);auto slice=std::get_if<SliceValue>(&base.data);
        if(!slice)runtime_error(n.span,"index/slice requires a slice or array","E4003");
        I start=n.children[1]->kind==N::Omitted ? 0:as_int(evaluate(*n.children[1]),n.span);
        I end=n.children[2]->kind==N::Omitted ? static_cast<I>(slice->length):as_int(evaluate(*n.children[2]),n.span);
        if(start<0 || end<start || static_cast<std::size_t>(end)>slice->length)runtime_error(n.span,"slice bounds must satisfy 0 <= start <= end <= len","E4006");
        auto view=*slice;view.start+=static_cast<std::size_t>(start);view.length=static_cast<std::size_t>(end-start);view.fixed=false;return Value(view);
    }
    case N::StructLiteral: {
        auto object=managed<StructData>();object->name=n.text;auto definition=model_.structures.at(n.text);
        for(const auto& init:n.children) {
            std::string type;for(const auto& field:definition->children)if(field->text==init->text)type=type_name(*field->children[0]);
            auto value=enforce_type(evaluate(*init->children[0]),type,init->span,contextual_literal(*init->children[0]));
            object->fields.emplace(init->text,copy_value(value));
        }
        return Value(StructValue{object,true});
    }
    case N::Member: {
        auto base=evaluate(*n.children[0]);
        if(n.text=="len" && (std::holds_alternative<SliceValue>(base.data)||std::holds_alternative<MapValue>(base.data)||std::holds_alternative<std::string>(base.data)))return aggregate_member(base,n.text,n.span);
        auto object=std::get_if<StructValue>(&base.data);if(!object)runtime_error(n.span,"member access requires a struct","E4003");
        if(auto field=object->data->fields.find(n.text);field!=object->data->fields.end())return read_member(field->second,object->writable);
        auto key=object->data->name+"."+n.text;auto method=model_.functions.find(key);
        if(method==model_.functions.end())runtime_error(n.span,"unknown struct member","E4003");
        if(method->second->span.file_id!=n.span.file_id && method->second->text.find(" pub")==std::string::npos)
            runtime_error(n.span,"method is private to its defining module","E5003");
        return Value(Callable{method->second,"",*object});
    }
    case N::Call: {
        auto value=evaluate(*n.children[0]);auto callable=std::get_if<Callable>(&value.data);
        if(!callable)runtime_error(n.span,"value is not callable","E4003");
        std::vector<Value> args;for(std::size_t i=1;i<n.children.size();++i)args.push_back(evaluate(*n.children[i]));return call(*callable,args,n);
    }
    default:runtime_error(n.span,"expression is not executable in Phase 2","E4008");
    }
}
Value Interpreter::call(const Callable& callable,const std::vector<Value>& args,const Node& site) {
    if(!callable.builtin.empty())return builtin(callable.builtin,args,site);
    const auto& fn=*callable.function;auto name=declared_name(fn);
    if(fn.text.find(" external")!=std::string::npos) {
        if(context_&&context_->in_task)runtime_error(site.span,"Native/WASM calls cannot run in tasks","E4104");
        std::vector<bool> contextual;for(std::size_t i=1;i<site.children.size();++i)contextual.push_back(contextual_literal(*site.children[i]));
        return model_.external->call(name,args,contextual,site.span);
    }
    if(fn.text.find(" abstract")!=std::string::npos)runtime_error(site.span,"interface prototype requires a concrete receiver","E4003");
    if(fn.text.find(" unsafe")!=std::string::npos)runtime_error(site.span,"unsafe function execution/FFI is not implemented","E4008");
    if(calls_>=128)runtime_error(site.span,"function call depth limit exceeded (128)","E4099");
    Restore call_guard(calls_,calls_+1);
    auto ps=params(fn);if(ps.size()!=args.size())runtime_error(site.span,"function argument count mismatch","E4003");
    if(context_&&context_->in_task&&callable.closure)std::static_pointer_cast<Environment>(callable.closure)->parent=&global_;
    Environment local{callable.closure?static_cast<Environment*>(callable.closure.get()):&global_,{}};
    if(callable.closure)local.bindings.emplace(name,Binding{Value(callable),"fn:"+name,false});
    Restore env_guard(environment_,&local);
    if(callable.receiver) {
        if(model_.mutating.at(name) && !callable.receiver->writable)runtime_error(site.span,"mutating method requires mutable receiver","E4007");
        local.bindings.emplace("self",Binding{Value(*callable.receiver),callable.receiver->data->name,false});
        // self is not rebindable; reading its fields retains the receiver's capability.
    }
    for(std::size_t i=0;i<ps.size();++i) {
        auto type=ps[i]->children.empty()?"":type_name(*ps[i]->children[0]);
        bool mutable_view=mutable_param(*ps[i]);
        if(mutable_view && !writable_value(args[i]))runtime_error(site.span,"mut parameter requires mutable argument","E4007");
        bool contextual=site.kind==N::Call && i+1<site.children.size() && contextual_literal(*site.children[i+1]);
        auto value=mutable_view?enforce_borrow(args[i],type,site.span):enforce_type(copy_value(args[i]),type,site.span,contextual);
        if(!mutable_view)value=read_only(std::move(value));
        local.bindings.emplace(declared_name(*ps[i]),Binding{std::move(value),type,false});
    }
    Value result;bool contextual=false;
    try{block(*fn.children.back(),false);}catch(const Returned& returned){result=returned.value;contextual=returned.contextual;}
    return enforce_return(std::move(result),fn,site.span,contextual);
}
Value Interpreter::builtin(const std::string& name,const std::vector<Value>& args,const Node& site) {
    if(name=="task_start"){if(args.empty()||!std::holds_alternative<Callable>(args[0].data))runtime_error(site.span,"spawn requires a Hua function","E4104");auto nested=Node(NodeKind::Call,site.span);for(std::size_t i=1;i<site.children.size();++i)nested.add(std::make_unique<Node>(site.children[i]->kind,site.children[i]->span,site.children[i]->text));return launch(std::get<Callable>(args[0].data),std::vector<Value>(args.begin()+1,args.end()),nested);}
    if(name=="parallel_map")return parallel(args,site);
    std::vector<bool> contextual;for(std::size_t i=1;i<site.children.size();++i)contextual.push_back(contextual_literal(*site.children[i]));auto inferred=model_.types.find(&site);return invoke_builtin(name,args,site.span,output_,contextual,inferred==model_.types.end()?"":inferred->second,context_);
}
}
