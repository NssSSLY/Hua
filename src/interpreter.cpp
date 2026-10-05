#include "hua/interpreter.hpp"
#include "hua/runtime.hpp"
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
bool writable_value(const Value& v){if(auto p=std::get_if<SliceValue>(&v.data))return p->writable;if(auto p=std::get_if<StructValue>(&v.data))return p->writable;return false;}
const std::unordered_set<std::string> builtins={"print","str","int","float","len","clone","sqrt","min","max","abs","clamp","type"};
Value read_member(const Value& v,bool writable){return writable ? v : read_only(v);}
}
Interpreter::Interpreter(const SemanticModel& model,std::ostream& output):model_(model),output_(output) {}
Interpreter::Binding& Interpreter::Environment::lookup(const std::string& name,const SourceSpan& span) {
    for(auto env=this;env;env=env->parent)if(auto found=env->bindings.find(name);found!=env->bindings.end())return found->second;
    runtime_error(span,"name `"+name+"` is not initialized","E4001");
}
void Interpreter::tick(const Node& n) {if(++steps_>1000000)runtime_error(n.span,"execution step limit exceeded (1000000)","E4099");}
void Interpreter::bind(const std::string& name,Value value,std::string type,bool mutable_binding,const SourceSpan& span) {
    if(type.empty())type=value_type(value);
    value=copy_value(value);
    if(!mutable_binding)value=read_only(std::move(value));
    if(!environment_->bindings.emplace(name,Binding{std::move(value),std::move(type),mutable_binding}).second)
        runtime_error(span,"duplicate runtime binding `"+name+"`","E4001");
}
void Interpreter::run(const Node& program) {
    if(model_.external)model_.external->reset();
    steps_=0;calls_=0;global_.bindings.clear();builtins_.bindings.clear();builtins_.parent=nullptr;global_.parent=&builtins_;environment_=&global_;
    for(const auto& name:builtins){builtins_.bindings.emplace(name,Binding{Value(Callable{nullptr,name,{}}),"builtin:"+name,false});builtins_.bindings.emplace("$core$"+name,Binding{Value(Callable{nullptr,name,{}}),"builtin:"+name,false});}
    for(const auto& [name,fn]:model_.functions)global_.bindings.insert_or_assign(name,Binding{Value(Callable{fn,"",{}}),"fn:"+name,false});
    for(const auto& n:program.children)if(n->kind!=N::Function && n->kind!=N::Struct)execute(*n);
    if(auto found=model_.functions.find("main");found!=model_.functions.end())call(Callable{found->second,"",{}},{},*found->second);
}
void Interpreter::block(const Node& n,bool scope) {
    Environment local{environment_,{}};Restore guard(environment_,scope?&local:environment_);
    for(const auto& c:n.children)execute(*c);
}
void Interpreter::execute(const Node& n) {
    tick(n);
    switch(n.kind) {
    case N::ExternalInit:model_.external->initialize(n.text,n.span);break;
    case N::Block:block(n);break;
    case N::Let:case N::Var:case N::Const: {
        auto type=model_.types.at(&n);
        auto value=n.kind==N::Const ? model_.constants.at(&n):evaluate(*n.children.back());
        value=enforce_type(std::move(value),type,n.span,numeric_literal(*n.children.back()));
        bind(n.text,std::move(value),type,n.kind==N::Var,n.span);break;
    }
    case N::ExpressionStatement:evaluate(*n.children[0]);break;
    case N::If:if(as_bool(evaluate(*n.children[0]),n.span))execute(*n.children[1]);else if(n.children.size()==3)execute(*n.children[2]);break;
    case N::While:
        while(as_bool(evaluate(*n.children[0]),n.span)){try{execute(*n.children[1]);}catch(const Continued&){continue;}catch(const Broken&){break;}}break;
    case N::For: {
        auto count=n.children.size()-2;const auto& iterable=*n.children[count];
        auto iteration=[&](I index,Value item) {
            Environment local{environment_,{}};Restore guard(environment_,&local);
            if(count==2)bind(n.children[0]->text,Value(index),"int",false,n.span);
            bind(n.children[count-1]->text,std::move(item),"",false,n.span);
            try{block(*n.children.back(),false);}catch(const Continued&){return true;}catch(const Broken&){return false;}return true;
        };
        if(iterable.kind==N::Range) {
            I start=as_int(evaluate(*iterable.children[0]),n.span),end=as_int(evaluate(*iterable.children[1]),n.span);
            I step=iterable.children.size()==3 ? as_int(evaluate(*iterable.children[2]),n.span):1;
            if(!step)runtime_error(iterable.span,"range step cannot be zero","E4005");
            const bool inclusive=iterable.text=="..=";I index=0;
            for(I value=start;step>0 ? (inclusive ? value<=end:value<end):(inclusive ? value>=end:value>end);) {
                tick(n);if(!iteration(index,Value(value)))break;
                if(inclusive && value==end)break;
                // A mathematically overflowing next step is outside this bounded int64 range, so terminate.
                if((step>0 && value>std::numeric_limits<I>::max()-step)||(step<0 && value<std::numeric_limits<I>::min()-step))break;
                value+=step;++index;
            }
        } else {
            auto value=evaluate(iterable);auto slice=std::get_if<SliceValue>(&value.data);
            if(!slice)runtime_error(iterable.span,"for requires a slice or range","E4003");
            const auto view=*slice;
            for(std::size_t i=0;i<view.length;++i){tick(n);if(!iteration(static_cast<I>(i),read_member((*view.storage)[view.start+i],view.writable)))break;}
        }
        break;
    }
    case N::Return:
        if(n.children.size()>1)runtime_error(n.span,"multiple return values are not executed yet","E4008");
        throw Returned{n.children.empty()?Value{}:copy_value(evaluate(*n.children[0])),!n.children.empty()&&numeric_literal(*n.children[0])};
    case N::Break:throw Broken{};case N::Continue:throw Continued{};
    case N::Import:runtime_error(n.span,"module loading requires the ModuleLoader entry point","E4008");
    case N::Unsafe:runtime_error(n.span,"unsafe execution/FFI is not implemented","E4008");
    default:runtime_error(n.span,"statement is not executable in Phase 2","E4008");
    }
}
Interpreter::Location Interpreter::locate(const Node& n) {
    tick(n);
    if(n.kind==N::Name){auto& b=environment_->lookup(n.text,n.span);return {&b.value,b.type,b.mutable_binding,{}};}
    if(n.kind==N::Member) {
        auto base=evaluate(*n.children[0]);auto object=std::get_if<StructValue>(&base.data);
        if(!object)runtime_error(n.span,"field assignment requires a struct","E4003");
        auto field=object->data->fields.find(n.text);if(field==object->data->fields.end())runtime_error(n.span,"unknown struct field","E4003");
        auto declaration=model_.structures.at(object->data->name);std::string type;
        for(const auto& f:declaration->children)if(f->text==n.text)type=type_name(*f->children[0]);
        return {&field->second,type,object->writable,object->data};
    }
    if(n.kind==N::Index) {
        auto base=evaluate(*n.children[0]);auto slice=std::get_if<SliceValue>(&base.data);
        if(!slice)runtime_error(n.span,"index assignment requires a slice","E4003");
        auto index=as_int(evaluate(*n.children[1]),n.span);
        if(index<0 || static_cast<std::size_t>(index)>=slice->length)runtime_error(n.span,"index out of bounds","E4006");
        return {&(*slice->storage)[slice->start+static_cast<std::size_t>(index)],slice->element_type,slice->writable,slice->storage};
    }
    runtime_error(n.span,"invalid assignment target","E4003");
}
Value Interpreter::evaluate(const Node& n) {
    tick(n);
    switch(n.kind) {
    case N::Integer:case N::Float:case N::String:case N::Boolean:case N::Nil:return literal(n);
    case N::Name:{auto& b=environment_->lookup(n.text,n.span);return b.value;}
    case N::Unary: {
        if(n.text=="&")runtime_error(n.span,"safe reference execution is not implemented","E4008");
        if(numeric_literal(n))return literal(n);
        return unary_value(n.text,evaluate(*n.children[0]),n.span);
    }
    case N::Binary: {
        auto left=evaluate(*n.children[0]);
        if(n.text=="&&" && !as_bool(left,n.span))return Value(false);
        if(n.text=="||" && as_bool(left,n.span))return Value(true);
        return binary_value(n.text,left,evaluate(*n.children[1]),n.span);
    }
    case N::Assignment:case N::Update: {
        auto where=locate(*n.children[0]);
        if(!where.writable)runtime_error(n.span,"cannot mutate immutable binding or read-only view","E4007");
        Value value;
        if(n.kind==N::Update)value=binary_value(n.text=="++"?"+":"-",*where.value,Value(I{1}),n.span);
        else {
            value=evaluate(*n.children[1]);
            if(n.text!="=")value=binary_value(n.text.substr(0,n.text.size()-1),*where.value,value,n.span);
        }
        value=enforce_type(std::move(value),where.type,n.span,n.kind==N::Assignment&&numeric_literal(*n.children[1]));
        *where.value=copy_value(value);return value;
    }
    case N::Array: {
        auto storage=std::make_shared<std::vector<Value>>();std::string type;
        for(const auto& c:n.children){auto value=copy_value(evaluate(*c));if(type.empty())type=value_type(value);else enforce_type(value,type,c->span);storage->push_back(std::move(value));}
        return Value(SliceValue{storage,0,storage->size(),true,type});
    }
    case N::Slice:case N::Index: {
        auto base=evaluate(*n.children[0]);auto slice=std::get_if<SliceValue>(&base.data);
        if(!slice)runtime_error(n.span,"index/slice requires a slice or array","E4003");
        if(n.kind==N::Index) {
            auto i=as_int(evaluate(*n.children[1]),n.span);
            if(i<0 || static_cast<std::size_t>(i)>=slice->length)runtime_error(n.span,"index out of bounds","E4006");
            return read_member((*slice->storage)[slice->start+static_cast<std::size_t>(i)],slice->writable);
        }
        I start=n.children[1]->kind==N::Omitted ? 0:as_int(evaluate(*n.children[1]),n.span);
        I end=n.children[2]->kind==N::Omitted ? static_cast<I>(slice->length):as_int(evaluate(*n.children[2]),n.span);
        if(start<0 || end<start || static_cast<std::size_t>(end)>slice->length)runtime_error(n.span,"slice bounds must satisfy 0 <= start <= end <= len","E4006");
        auto view=*slice;view.start+=static_cast<std::size_t>(start);view.length=static_cast<std::size_t>(end-start);return Value(view);
    }
    case N::StructLiteral: {
        auto object=std::make_shared<StructData>();object->name=n.text;auto definition=model_.structures.at(n.text);
        for(const auto& init:n.children) {
            std::string type;for(const auto& field:definition->children)if(field->text==init->text)type=type_name(*field->children[0]);
            auto value=enforce_type(evaluate(*init->children[0]),type,init->span,numeric_literal(*init->children[0]));
            object->fields.emplace(init->text,copy_value(value));
        }
        return Value(StructValue{object,true});
    }
    case N::Member: {
        auto base=evaluate(*n.children[0]);
        if(n.text=="len") {
            if(auto slice=std::get_if<SliceValue>(&base.data))return Value(static_cast<I>(slice->length));
            if(auto text=std::get_if<std::string>(&base.data))return Value(static_cast<I>(text->size()));
        }
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
        std::vector<bool> contextual;for(std::size_t i=1;i<site.children.size();++i)contextual.push_back(numeric_literal(*site.children[i]));
        return model_.external->call(name,args,contextual,site.span);
    }
    if(fn.text.find(" unsafe")!=std::string::npos)runtime_error(site.span,"unsafe function execution/FFI is not implemented","E4008");
    if(calls_>=128)runtime_error(site.span,"function call depth limit exceeded (128)","E4099");
    Restore call_guard(calls_,calls_+1);
    auto ps=params(fn);if(ps.size()!=args.size())runtime_error(site.span,"function argument count mismatch","E4003");
    Environment local{&global_,{}};Restore env_guard(environment_,&local);
    if(callable.receiver) {
        if(model_.mutating.at(name) && !callable.receiver->writable)runtime_error(site.span,"mutating method requires mutable receiver","E4007");
        local.bindings.emplace("self",Binding{Value(*callable.receiver),callable.receiver->data->name,false});
        // self is not rebindable; reading its fields retains the receiver's capability.
    }
    for(std::size_t i=0;i<ps.size();++i) {
        auto type=ps[i]->children.empty()?"":type_name(*ps[i]->children[0]);
        bool mutable_view=mutable_param(*ps[i]);
        if(mutable_view && !writable_value(args[i]))runtime_error(site.span,"mut parameter requires mutable argument","E4007");
        bool contextual=site.kind==N::Call && i+1<site.children.size() && numeric_literal(*site.children[i+1]);
        auto value=enforce_type(copy_value(args[i]),type,site.span,contextual);
        if(!mutable_view)value=read_only(std::move(value));
        local.bindings.emplace(declared_name(*ps[i]),Binding{std::move(value),type,false});
    }
    Value result;bool contextual=false;
    try{block(*fn.children.back(),false);}catch(const Returned& returned){result=returned.value;contextual=returned.contextual;}
    for(const auto& c:fn.children)if(c->kind==N::ReturnTypes) {
        if(c->children.size()!=1)runtime_error(site.span,"multiple return values are not executed yet","E4008");
        result=enforce_type(std::move(result),type_name(*c->children[0]),site.span,contextual);
    }
    return copy_value(result);
}
Value Interpreter::builtin(const std::string& name,const std::vector<Value>& args,const Node& site) {
    return invoke_builtin(name,args,site.span,output_);
}
}
