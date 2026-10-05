#include "hua/vm.hpp"
#include "hua/runtime.hpp"
#include <limits>
#include <unordered_set>
namespace hua {
namespace {
using N=NodeKind;using I=std::int64_t;
[[noreturn]] void invalid(const SourceSpan& s){runtime_error(s,"invalid VM instruction/stack state","E6001");}
bool writable(const Value& v){if(auto p=std::get_if<SliceValue>(&v.data))return p->writable;if(auto p=std::get_if<StructValue>(&v.data))return p->writable;return false;}
std::string field_type(const SemanticModel& model,const std::string& owner,const std::string& field,const SourceSpan& span) {
    auto s=model.structures.find(owner);if(s==model.structures.end())invalid(span);
    for(const auto& f:s->second->children)if(f->text==field)return type_name(*f->children[0]);
    runtime_error(span,"unknown struct field","E4003");
}
}
VirtualMachine::Binding& VirtualMachine::Environment::lookup(const std::string& name,const SourceSpan& span) {
    for(auto env=this;env;env=env->parent.get())if(auto found=env->bindings.find(name);found!=env->bindings.end())return found->second;
    runtime_error(span,"name `"+name+"` is not initialized","E4001");
}
Value VirtualMachine::pop(const SourceSpan& span) {
    if(stack_.empty() || (!frames_.empty() && stack_.size()<=frames_.back().base) || !std::holds_alternative<Value>(stack_.back()))invalid(span);
    auto value=std::move(std::get<Value>(stack_.back()));stack_.pop_back();return value;
}
std::vector<Value> VirtualMachine::arguments(std::size_t count,const SourceSpan& span) {
    if(count>stack_.size() || (!frames_.empty() && count>stack_.size()-frames_.back().base))invalid(span);std::vector<Value> args(count);
    for(std::size_t i=count;i>0;--i)args[i-1]=pop(span);return args;
}
void VirtualMachine::bind(const Instruction& op,Value value) {
    auto type=op.type.empty()?value_type(value):op.type;value=copy_value(value);
    if(!op.flag)value=read_only(std::move(value));
    if(!frames_.back().environment->bindings.emplace(op.text,Binding{std::move(value),type,op.flag}).second)
        runtime_error(op.span,"duplicate runtime binding `"+op.text+"`","E4001");
}
void VirtualMachine::run(const Bytecode& bytecode) {
    if(model_.external)model_.external->reset();
    bytecode_=&bytecode;steps_=0;frames_.clear();stack_.clear();
    auto builtins=std::make_shared<Environment>();global_=std::make_shared<Environment>();global_->parent=builtins;
    for(const auto* name:{"print","str","int","float","len","clone","sqrt","min","max","abs","clamp","type"})
        {builtins->bindings.emplace(name,Binding{Value(Callable{nullptr,name,{}}),std::string("builtin:")+name,false});
         builtins->bindings.emplace(std::string("$core$")+name,Binding{Value(Callable{nullptr,name,{}}),std::string("builtin:")+name,false});}
    for(const auto& [name,fn]:model_.functions)global_->bindings.emplace(name,Binding{Value(Callable{fn,"",{}}),"fn:"+name,false});
    frames_.push_back({&bytecode.initializer,nullptr,0,0,{},global_,{}});dispatch();
    if(auto main=model_.functions.find("main");main!=model_.functions.end()) {
        Instruction site{};site.span=main->second->span;call(Callable{main->second,"",{}},{},site);if(frames_.empty())stack_.clear();else dispatch();
    }
}
void VirtualMachine::call(const Callable& callable,const std::vector<Value>& args,const Instruction& site) {
    if(!callable.builtin.empty()){stack_.emplace_back(invoke_builtin(callable.builtin,args,site.span,output_));return;}
    if(!callable.function)invalid(site.span);
    const auto& fn=*callable.function;auto key=declared_name(fn);
    if(fn.text.find(" external")!=std::string::npos){stack_.emplace_back(model_.external->call(key,args,site.contextual,site.span));return;}
    if(fn.text.find(" unsafe")!=std::string::npos)runtime_error(site.span,"unsafe function execution/FFI is not implemented","E4008");
    const auto user_calls=frames_.size()-(!frames_.empty() && !frames_.front().function?1:0);
    if(user_calls>=128)runtime_error(site.span,"function call depth limit exceeded (128)","E4099");
    std::vector<const Node*> ps;for(const auto& c:fn.children)if(c->kind==N::Parameter)ps.push_back(c.get());
    if(ps.size()!=args.size())runtime_error(site.span,"function argument count mismatch","E4003");
    auto local=std::make_shared<Environment>();local->parent=global_;
    if(callable.receiver) {
        if(model_.mutating.at(key) && !callable.receiver->writable)runtime_error(site.span,"mutating method requires mutable receiver","E4007");
        local->bindings.emplace("self",Binding{Value(*callable.receiver),callable.receiver->data->name,false});
    }
    for(std::size_t i=0;i<ps.size();++i) {
        const auto& p=*ps[i];auto type=p.children.empty()?"":type_name(*p.children[0]);
        bool mut=p.text.find(" mut")!=std::string::npos || (!p.children.empty() && p.children[0]->kind==N::MutableType);
        if(mut && !writable(args[i]))runtime_error(site.span,"mut parameter requires mutable argument","E4007");
        auto v=enforce_type(copy_value(args[i]),type,site.span,i<site.contextual.size()&&site.contextual[i]);
        if(!mut)v=read_only(std::move(v));local->bindings.emplace(declared_name(p),Binding{std::move(v),type,false});
    }
    auto code=bytecode_->functions.find(&fn);if(code==bytecode_->functions.end())invalid(site.span);
    frames_.push_back({&code->second,&fn,0,stack_.size(),site.span,std::move(local),{}});
}
void VirtualMachine::dispatch() {
    while(!frames_.empty()) {
        auto& f=frames_.back();if(f.pc>=f.code->instructions.size())invalid(f.site);
        const auto& op=f.code->instructions[f.pc++];const auto& s=op.span;
        if(++steps_>1000000)runtime_error(s,"execution step limit exceeded (1000000)","E4099");
        switch(op.op) {
        case Op::ExternalInit:model_.external->initialize(op.text,s);break;
        case Op::Constant:stack_.emplace_back(op.constant);break;
        case Op::Load:stack_.emplace_back(f.environment->lookup(op.text,s).value);break;
        case Op::Bind:{auto v=enforce_type(pop(s),op.type,s,!op.contextual.empty()&&op.contextual[0]);bind(op,std::move(v));break;}
        case Op::Pop:pop(s);break;
        case Op::Unary:stack_.emplace_back(unary_value(op.text,pop(s),s));break;
        case Op::Binary:{auto right=pop(s),left=pop(s);stack_.emplace_back(binary_value(op.text,left,right,s));break;}
        case Op::Jump:if(op.target>=f.code->instructions.size())invalid(s);f.pc=op.target;break;
        case Op::JumpFalse:case Op::JumpTrue: {
            auto condition=as_bool(pop(s),s);
            if(condition==(op.op==Op::JumpTrue)){if(op.target>=f.code->instructions.size())invalid(s);f.pc=op.target;}break;
        }
        case Op::EnterScope:{auto env=std::make_shared<Environment>();env->parent=f.environment;f.environment=std::move(env);break;}
        case Op::LeaveScope:case Op::Unwind: {
            auto count=op.op==Op::LeaveScope?1:op.argument;
            for(std::size_t i=0;i<count;++i){if(!f.environment->parent || f.environment==global_)invalid(s);f.environment=f.environment->parent;}break;
        }
        case Op::LocateName: {
            auto& b=f.environment->lookup(op.text,s);if(!b.mutable_binding)runtime_error(s,"cannot mutate immutable binding or read-only view","E4007");
            stack_.emplace_back(Location{&b.value,b.type,f.environment});break;
        }
        case Op::LocateField: {
            auto value=pop(s);auto object=std::get_if<StructValue>(&value.data);
            if(!object)runtime_error(s,"field assignment requires a struct","E4003");
            auto field=object->data->fields.find(op.text);if(field==object->data->fields.end())runtime_error(s,"unknown struct field","E4003");
            if(!object->writable)runtime_error(s,"cannot mutate immutable binding or read-only view","E4007");
            stack_.emplace_back(Location{&field->second,field_type(model_,object->data->name,op.text,s),object->data});break;
        }
        case Op::LocateIndex:case Op::Index: {
            auto index=as_int(pop(s),s);auto value=pop(s);auto slice=std::get_if<SliceValue>(&value.data);
            if(!slice)runtime_error(s,op.op==Op::Index?"index/slice requires a slice or array":"index assignment requires a slice","E4003");
            if(index<0 || static_cast<std::size_t>(index)>=slice->length)runtime_error(s,"index out of bounds","E4006");
            auto& item=(*slice->storage)[slice->start+static_cast<std::size_t>(index)];
            if(op.op==Op::Index)stack_.emplace_back(slice->writable?item:read_only(item));
            else{if(!slice->writable)runtime_error(s,"cannot mutate immutable binding or read-only view","E4007");stack_.emplace_back(Location{&item,slice->element_type,slice->storage});}break;
        }
        case Op::Store: {
            auto value=pop(s);if(stack_.empty() || !std::holds_alternative<Location>(stack_.back()))invalid(s);
            if(stack_.size()<=f.base)invalid(s);
            auto where=std::get<Location>(stack_.back());stack_.pop_back();
            if(op.text!="=")value=binary_value(op.text.substr(0,op.text.size()-1),*where.value,value,s);
            value=enforce_type(std::move(value),where.type,s,op.flag);*where.value=copy_value(value);stack_.emplace_back(std::move(value));break;
        }
        case Op::CheckSlice:
            if(stack_.empty() || !std::holds_alternative<Value>(stack_.back()))invalid(s);
            if(!std::holds_alternative<SliceValue>(std::get<Value>(stack_.back()).data))runtime_error(s,"index/slice requires a slice or array","E4003");break;
        case Op::MakeArray: {
            auto storage=std::make_shared<std::vector<Value>>();stack_.emplace_back(Value(SliceValue{storage,0,0,true,""}));break;
        }
        case Op::ArrayAppend: {
            auto item=copy_value(pop(s));
            if(stack_.empty() || !std::holds_alternative<Value>(stack_.back()))invalid(s);
            auto slice=std::get_if<SliceValue>(&std::get<Value>(stack_.back()).data);if(!slice)invalid(s);
            if(slice->element_type.empty())slice->element_type=value_type(item);else enforce_type(item,slice->element_type,s);
            slice->storage->push_back(std::move(item));++slice->length;break;
        }
        case Op::Slice: {
            auto end=pop(s),start=pop(s),value=pop(s);auto slice=std::get_if<SliceValue>(&value.data);
            if(!slice)runtime_error(s,"index/slice requires a slice or array","E4003");
            I a=op.contextual.at(0)?0:as_int(start,s),b=op.contextual.at(1)?static_cast<I>(slice->length):as_int(end,s);
            if(a<0 || b<a || static_cast<std::size_t>(b)>slice->length)runtime_error(s,"slice bounds must satisfy 0 <= start <= end <= len","E4006");
            auto view=*slice;view.start+=static_cast<std::size_t>(a);view.length=static_cast<std::size_t>(b-a);stack_.emplace_back(Value(view));break;
        }
        case Op::MakeStruct: {
            auto object=std::make_shared<StructData>();object->name=op.text;stack_.emplace_back(Value(StructValue{object,true}));break;
        }
        case Op::InitField: {
            auto item=pop(s);if(stack_.empty() || !std::holds_alternative<Value>(stack_.back()))invalid(s);
            auto object=std::get_if<StructValue>(&std::get<Value>(stack_.back()).data);if(!object)invalid(s);
            auto value=enforce_type(std::move(item),field_type(model_,object->data->name,op.text,s),s,op.flag);
            object->data->fields.emplace(op.text,copy_value(value));break;
        }
        case Op::Member: {
            auto value=pop(s);
            if(op.text=="len") {
                if(auto p=std::get_if<SliceValue>(&value.data)){stack_.emplace_back(Value(static_cast<I>(p->length)));break;}
                if(auto p=std::get_if<std::string>(&value.data)){stack_.emplace_back(Value(static_cast<I>(p->size())));break;}
            }
            auto object=std::get_if<StructValue>(&value.data);if(!object)runtime_error(s,"member access requires a struct","E4003");
            if(auto field=object->data->fields.find(op.text);field!=object->data->fields.end()){stack_.emplace_back(object->writable?field->second:read_only(field->second));break;}
            auto method=model_.functions.find(object->data->name+"."+op.text);
            if(method==model_.functions.end())runtime_error(s,"unknown struct member","E4003");
            if(method->second->span.file_id!=s.file_id && method->second->text.find(" pub")==std::string::npos)
                runtime_error(s,"method is private to its defining module","E5003");
            stack_.emplace_back(Value(Callable{method->second,"",*object}));break;
        }
        case Op::Call: {
            auto args=arguments(op.argument,s);auto callee=pop(s);auto p=std::get_if<Callable>(&callee.data);
            if(!p)runtime_error(s,"value is not callable","E4003");call(*p,args,op);break;
        }
        case Op::Return: {
            auto value=pop(s);
            if(f.function)for(const auto& c:f.function->children)if(c->kind==N::ReturnTypes) {
                if(c->children.size()!=1)runtime_error(f.site,"multiple return values are not executed yet","E4008");
                value=enforce_type(std::move(value),type_name(*c->children[0]),f.site,op.flag);
            }
            auto base=f.base;if(stack_.size()!=base)invalid(s);frames_.pop_back();stack_.resize(base);
            if(!frames_.empty())stack_.emplace_back(copy_value(value));break;
        }
        case Op::RangeInit: {
            I step=as_int(pop(s),s),end=as_int(pop(s),s),start=as_int(pop(s),s);
            if(!step)runtime_error(s,"range step cannot be zero","E4005");
            f.iterators.push_back({true,op.flag,false,start,end,step,0,{}});break;
        }
        case Op::SliceInit: {
            auto value=pop(s);auto p=std::get_if<SliceValue>(&value.data);
            if(!p)runtime_error(s,"for requires a slice or range","E4003");Iterator it{};it.slice=*p;f.iterators.push_back(std::move(it));break;
        }
        case Op::IterNext: {
            if(f.iterators.empty())invalid(s);auto& it=f.iterators.back();bool has=!it.done;Value item;I index=it.index;
            if(it.range) {
                has=has && (it.step>0?(it.inclusive?it.current<=it.end:it.current<it.end):(it.inclusive?it.current>=it.end:it.current>it.end));
                if(has) {
                    item=Value(it.current);
                    if((it.inclusive && it.current==it.end)||(it.step>0 && it.current>std::numeric_limits<I>::max()-it.step)||(it.step<0 && it.current<std::numeric_limits<I>::min()-it.step))it.done=true;
                    else it.current+=it.step;
                }
            }else {
                has=has && static_cast<std::uint64_t>(index)<it.slice.length;
                if(has){auto v=(*it.slice.storage)[it.slice.start+static_cast<std::size_t>(index)];item=it.slice.writable?v:read_only(v);}
            }
            if(!has){if(op.target>=f.code->instructions.size())invalid(s);f.pc=op.target;break;}
            if(it.index==std::numeric_limits<I>::max())it.done=true;else ++it.index;
            auto env=std::make_shared<Environment>();env->parent=f.environment;f.environment=std::move(env);
            Instruction binding=op;binding.flag=false;binding.type="int";
            if(op.names.size()==2){binding.text=op.names[0];bind(binding,Value(index));}
            if(op.names.empty() || op.names.size()>2)invalid(s);
            binding.text=op.names.back();binding.type="";bind(binding,std::move(item));break;
        }
        case Op::IterEnd:if(f.iterators.empty())invalid(s);f.iterators.pop_back();break;
        case Op::Fail:runtime_error(s,op.text,op.type.empty()?"E4008":op.type);
        default:invalid(s);
        }
    }
    if(!stack_.empty())invalid({});
}
}
