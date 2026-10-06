#include <exception>
#include "hua/vm.hpp"
#include "hua/runtime.hpp"
#include "hua/tasks.hpp"
#include <limits>
#include <unordered_set>
namespace hua {
namespace {
using N=NodeKind;using I=std::int64_t;
[[noreturn]] void invalid(const SourceSpan& s){runtime_error(s,"invalid VM instruction/stack state","E6001");}
bool writable(const Value& v){if(auto p=std::get_if<BufferValue>(&v.data))return p->writable;if(auto p=std::get_if<ListValue>(&v.data))return p->writable;if(auto p=std::get_if<SliceValue>(&v.data))return p->writable;if(auto p=std::get_if<StructValue>(&v.data))return p->writable;if(auto p=std::get_if<MapValue>(&v.data))return p->writable;if(auto p=std::get_if<ResultValue>(&v.data))return p->writable;return false;}
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
    if(std::holds_alternative<MultiValue>(value.data))runtime_error(op.span,"multiple values require positional binding","E4003");
    auto type=op.type.empty()?value_type(value):op.type;value=copy_value(value);
    if(!op.flag)value=read_only(std::move(value));
    if(!frames_.back().environment->bindings.emplace(op.text,Binding{std::move(value),type,op.flag}).second)
        runtime_error(op.span,"duplicate runtime binding `"+op.text+"`","E4001");
}
void VirtualMachine::run(const Bytecode& bytecode) {
    ExecutionScope execution(context_,output_);TypeRelationScope relation_scope(model_.relations);
    if(model_.external)model_.external->reset();
    bytecode_=&bytecode;steps_=0;frames_.clear();stack_.clear();
    auto builtins=managed<Environment>();global_=managed<Environment>();global_->parent=builtins;
    for(const auto& name:builtin_names())
        {builtins->bindings.emplace(name,Binding{Value(Callable{nullptr,name,{}}),std::string("builtin:")+name,false});
         builtins->bindings.emplace(std::string("$core$")+name,Binding{Value(Callable{nullptr,name,{}}),std::string("builtin:")+name,false});}
    for(const auto& [name,fn]:model_.functions)if(!model_.nested.contains(fn))global_->bindings.emplace(name,Binding{Value(Callable{fn,"",{}}),"fn:"+name,false});
    frames_.push_back({&bytecode.initializer,nullptr,0,0,{},global_,{}});try{dispatch();}catch(...){auto original=std::current_exception();cleanup_error();global_->bindings.clear();stack_.clear();frames_.clear();completed_.reset();std::rethrow_exception(original);}
    if(auto main=model_.functions.find("main");main!=model_.functions.end()) {
        Instruction site{};site.span=main->second->span;call(Callable{main->second,"",{}},{},site);if(frames_.empty())stack_.clear();else try{dispatch();}catch(...){auto original=std::current_exception();cleanup_error();global_->bindings.clear();stack_.clear();frames_.clear();completed_.reset();std::rethrow_exception(original);}
    }
    try{if(completed_&&std::holds_alternative<TaskValue>(completed_->data))context_->tasks->await(*completed_,{});execution.finish();}catch(...){global_->bindings.clear();stack_.clear();frames_.clear();completed_.reset();throw;}
    global_->bindings.clear();completed_.reset();
}
void VirtualMachine::call(const Callable& callable,const std::vector<Value>& args,const Instruction& site) {
    if(callable.builtin=="task_start"){if(args.empty()||!std::holds_alternative<Callable>(args[0].data))runtime_error(site.span,"spawn requires a Hua function","E4104");auto nested=site;if(!nested.contextual.empty())nested.contextual.erase(nested.contextual.begin());stack_.emplace_back(launch(std::get<Callable>(args[0].data),std::vector<Value>(args.begin()+1,args.end()),nested));return;}
    if(callable.builtin=="parallel_map"){stack_.emplace_back(parallel(args,site));return;}
    if(!callable.builtin.empty()){stack_.emplace_back(invoke_builtin(callable.builtin,args,site.span,output_,site.contextual,site.type,context_));return;}
    if(!callable.function)invalid(site.span);
    const auto& fn=*callable.function;auto key=declared_name(fn);
    if(fn.text.find(" external")!=std::string::npos){if(context_&&context_->in_task)runtime_error(site.span,"Native/WASM calls cannot run in tasks","E4104");stack_.emplace_back(model_.external->call(key,args,site.contextual,site.span));return;}
    if(fn.text.find(" unsafe")!=std::string::npos)runtime_error(site.span,"unsafe function execution/FFI is not implemented","E4008");
    const auto user_calls=frames_.size()-(!frames_.empty() && !frames_.front().function?1:0);
    if(user_calls>=128)runtime_error(site.span,"function call depth limit exceeded (128)","E4099");
    std::vector<const Node*> ps;for(const auto& c:fn.children)if(c->kind==N::Parameter)ps.push_back(c.get());
    if(ps.size()!=args.size())runtime_error(site.span,"function argument count mismatch","E4003");
    if(context_&&context_->in_task&&callable.closure)std::static_pointer_cast<Environment>(callable.closure)->parent=global_;
    auto local=managed<Environment>();local->parent=callable.closure?std::static_pointer_cast<Environment>(callable.closure):global_;
    if(callable.closure)local->bindings.emplace(key,Binding{Value(callable),"fn:"+key,false});
    if(callable.receiver) {
        if(model_.mutating.at(key) && !callable.receiver->writable)runtime_error(site.span,"mutating method requires mutable receiver","E4007");
        local->bindings.emplace("self",Binding{Value(*callable.receiver),callable.receiver->data->name,false});
    }
    for(std::size_t i=0;i<ps.size();++i) {
        const auto& p=*ps[i];auto type=p.children.empty()?"":type_name(*p.children[0]);
        bool mut=p.text.find(" mut")!=std::string::npos || (!p.children.empty() && p.children[0]->kind==N::MutableType);
        if(mut && !writable(args[i]))runtime_error(site.span,"mut parameter requires mutable argument","E4007");
        auto v=mut?enforce_borrow(args[i],type,site.span):enforce_type(copy_value(args[i]),type,site.span,i<site.contextual.size()&&site.contextual[i]);
        if(!mut)v=read_only(std::move(v));local->bindings.emplace(declared_name(p),Binding{std::move(v),type,false});
    }
    auto code=bytecode_->functions.find(&fn);if(code==bytecode_->functions.end())invalid(site.span);
    frames_.push_back({&code->second,&fn,0,stack_.size(),site.span,std::move(local),{}});
}
void VirtualMachine::finish_return(Value value,const Instruction& op,bool early) {
    auto f=frames_.back();
    for(auto env=f.environment;env&&env!=global_;env=env->parent)drain(env);if(!f.function)drain(global_);if(early&&!f.function)invalid(op.span);if(f.function)value=enforce_return(std::move(value),*f.function,f.site,op.flag);
    auto base=f.base;if(!early&&stack_.size()!=base)invalid(op.span);frames_.pop_back();stack_.resize(base);if(!frames_.empty())stack_.emplace_back(copy_value(value));else completed_=copy_value(value);
}
void VirtualMachine::drain(const std::shared_ptr<Environment>& env){
    struct Cleanup {bool& flag;bool previous;Cleanup(bool& f):flag(f),previous(f){flag=true;}~Cleanup(){flag=previous;}} cleanup(context_->cleanup);
    auto deferred=std::move(env->deferred);env->deferred.clear();std::exception_ptr first;
    for(auto i=deferred.rbegin();i!=deferred.rend();++i){auto depth=frames_.size(),base=stack_.size();try{call(i->callable,i->arguments,i->site);dispatch(depth);}catch(...){if(!first)first=std::current_exception();while(frames_.size()>depth){auto environment=frames_.back().environment;for(auto e=environment;e&&e!=env&&e!=global_;e=e->parent)try{drain(e);}catch(...){}frames_.pop_back();}}stack_.resize(base);}
    if(first)std::rethrow_exception(first);
}
void VirtualMachine::cleanup_error(){
    struct Unwinding {bool& flag;bool old;Unwinding(bool& f):flag(f),old(f){flag=true;}~Unwinding(){flag=old;}} failed(context_->unwinding);
    std::unordered_set<Environment*> seen;auto frames=frames_;
    for(auto i=frames.rbegin();i!=frames.rend();++i)for(auto env=i->environment;env;env=env->parent)if(seen.insert(env.get()).second)try{drain(env);}catch(...){}
}
void VirtualMachine::dispatch(std::size_t stop_depth) {
    while(frames_.size()>stop_depth) {
        auto& f=frames_.back();if(f.pc>=f.code->instructions.size())invalid(f.site);
        const auto& op=f.code->instructions[f.pc++];const auto& s=op.span;
        task_checkpoint(context_,s);if(++steps_>1000000)runtime_error(s,"execution step limit exceeded (1000000)","E4099");
        switch(op.op) {
        case Op::Closure:{auto fn=model_.functions.find(op.text);if(fn==model_.functions.end())invalid(s);auto captured=managed<Environment>();captured->parent=global_;for(auto env=f.environment;env&&env!=global_;env=env->parent)for(const auto& [name,binding]:env->bindings)if(!captured->bindings.contains(name))captured->bindings.emplace(name,Binding{read_only(copy_value(binding.value)),binding.type,false});stack_.emplace_back(Value(Callable{fn->second,"",{},captured}));break;}
        case Op::Defer:{auto args=arguments(op.argument,s);auto value=pop(s);auto fn=std::get_if<Callable>(&value.data);if(!fn)runtime_error(s,"defer requires a callable","E4003");for(auto& arg:args)arg=copy_value(arg);f.environment->deferred.push_back({*fn,std::move(args),op});break;}
        case Op::ExternalInit:if(context_&&context_->in_task)runtime_error(s,"Native/WASM initialization cannot run in tasks","E4104");model_.external->initialize(op.text,s);break;
        case Op::Constant:stack_.emplace_back(op.constant);break;
        case Op::Load:stack_.emplace_back(f.environment->lookup(op.text,s).value);break;
        case Op::Bind:{auto v=enforce_type(pop(s),op.type,s,!op.contextual.empty()&&op.contextual[0]);bind(op,std::move(v));break;}
        case Op::Pop:pop(s);break;
        case Op::Unary:stack_.emplace_back(unary_value(op.text,pop(s),s));break;
        case Op::Binary:{auto right=pop(s),left=pop(s);stack_.emplace_back(binary_value(op.text,left,right,s,op.flag));break;}
        case Op::Jump:if(op.target>=f.code->instructions.size())invalid(s);f.pc=op.target;break;
        case Op::JumpFalse:case Op::JumpTrue: {
            auto condition=as_bool(pop(s),s);
            if(condition==(op.op==Op::JumpTrue)){if(op.target>=f.code->instructions.size())invalid(s);f.pc=op.target;}break;
        }
        case Op::EnterScope:{auto env=managed<Environment>();env->parent=f.environment;f.environment=std::move(env);break;}
        case Op::LeaveScope:case Op::Unwind: {
            auto count=op.op==Op::LeaveScope?1:op.argument;
            for(std::size_t i=0;i<count;++i){if(!frames_.back().environment->parent || frames_.back().environment==global_)invalid(s);auto env=frames_.back().environment;drain(env);frames_.back().environment=env->parent;}break;
        }
        case Op::LocateName: {
            if(op.text=="_"){stack_.emplace_back(Location{});break;}
            auto& b=f.environment->lookup(op.text,s);if(!b.mutable_binding)runtime_error(s,"cannot mutate immutable binding or read-only view","E4007");
            stack_.emplace_back(Location{&b.value,b.type,true,f.environment,{},{}});break;
        }
        case Op::LocateField: {
            auto value=pop(s);auto object=std::get_if<StructValue>(&value.data);
            if(!object)runtime_error(s,"field assignment requires a struct","E4003");
            auto field=object->data->fields.find(op.text);if(field==object->data->fields.end())runtime_error(s,"unknown struct field","E4003");
            if(!object->writable)runtime_error(s,"cannot mutate immutable binding or read-only view","E4007");
            stack_.emplace_back(Location{&field->second,field_type(model_,object->data->name,op.text,s),true,object->data,{},{}});break;
        }
        case Op::LocateIndex:case Op::Index: {
            auto index=pop(s),value=pop(s);if(op.op==Op::Index)stack_.emplace_back(index_value(value,index,s));else {auto l=index_location(std::move(value),std::move(index),s);if(!l.writable)runtime_error(s,"cannot mutate immutable binding or read-only view","E4007");stack_.emplace_back(std::move(l));}break;
        }
        case Op::Store: {
            auto value=pop(s);if(stack_.empty() || !std::holds_alternative<Location>(stack_.back()))invalid(s);
            if(stack_.size()<=f.base)invalid(s);
            auto where=std::get<Location>(stack_.back());stack_.pop_back();
            if(op.text!="=")value=binary_value(op.text.substr(0,op.text.size()-1),location_read(where,s),value,s,op.flag);
            value=enforce_type(std::move(value),where.type,s,op.flag||op.text!="=");location_write(where,value);stack_.emplace_back(std::move(value));break;
        }
        case Op::MakeMulti:{auto values=arguments(op.argument,s);for(auto& v:values)v=copy_value(v);stack_.emplace_back(make_multi(std::move(values),op.contextual));break;}
        case Op::BindMulti:{auto pack=pop(s);auto values=unpack_multi(pack,op.argument,s);auto types=type_arguments(op.type,"multi");if(types.size()!=values.size()||op.names.size()!=values.size())invalid(s);auto data=std::get<MultiValue>(pack.data).data;
            for(std::size_t i=0;i<values.size();++i){values[i]=enforce_type(std::move(values[i]),types[i],s,i<data->contextual.size()&&data->contextual[i]);if(op.names[i]!="_"&&f.environment->bindings.contains(op.names[i]))runtime_error(s,"duplicate runtime binding","E4001");}
            for(std::size_t i=0;i<values.size();++i)if(op.names[i]!="_"){auto item=op;item.text=op.names[i];item.type=types[i];bind(item,values[i]);}break;}
        case Op::StoreMulti:{auto pack=pop(s);auto values=unpack_multi(pack,op.argument,s);auto data=std::get<MultiValue>(pack.data).data;std::vector<Location> ls(op.argument);
            for(std::size_t i=op.argument;i>0;--i){if(stack_.size()<=f.base||!std::holds_alternative<Location>(stack_.back()))invalid(s);ls[i-1]=std::get<Location>(stack_.back());stack_.pop_back();}
            for(std::size_t i=0;i<values.size();++i)values[i]=copy_value(enforce_type(std::move(values[i]),ls[i].type,s,i<data->contextual.size()&&data->contextual[i]));
            for(std::size_t i=0;i<values.size();++i)if(ls[i].value||ls[i].map||ls[i].sequence)location_write(ls[i],values[i]);break;}
        case Op::MakeMap:stack_.emplace_back(make_map(op.type,s));break;
        case Op::MapInsert:{auto item=pop(s),key=pop(s);if(stack_.empty()||!std::holds_alternative<Value>(stack_.back()))invalid(s);auto m=std::get_if<MapValue>(&std::get<Value>(stack_.back()).data);if(!m)invalid(s);map_put(*m,key,std::move(item),s,op.flag);break;}
        case Op::Propagate:{auto value=pop(s);auto r=std::get_if<ResultValue>(&value.data);if(!r)runtime_error(s,"expected Result","E4003");if(r->ok)stack_.emplace_back(result_payload(value,true,s));else finish_return(propagated_error(value),op,true);break;}
        case Op::CheckIndex:
            if(stack_.empty()||!std::holds_alternative<Value>(stack_.back()))invalid(s);
            if(!std::holds_alternative<SliceValue>(std::get<Value>(stack_.back()).data)&&!std::holds_alternative<MapValue>(std::get<Value>(stack_.back()).data))runtime_error(s,"index/slice requires a slice, array or map","E4003");break;
        case Op::CheckSlice:
            if(stack_.empty() || !std::holds_alternative<Value>(stack_.back()))invalid(s);
            if(!std::holds_alternative<SliceValue>(std::get<Value>(stack_.back()).data))runtime_error(s,"index/slice requires a slice or array","E4003");break;
        case Op::MakeArray: {
            auto storage=managed<std::vector<Value>>();stack_.emplace_back(Value(SliceValue{storage,0,0,true,""}));break;
        }
        case Op::ArrayAppend: {
            auto item=copy_value(pop(s));
            if(stack_.empty() || !std::holds_alternative<Value>(stack_.back()))invalid(s);
            auto slice=std::get_if<SliceValue>(&std::get<Value>(stack_.back()).data);if(!slice)invalid(s);
            if(slice->element_type.empty())slice->element_type=value_type(item);else item=enforce_type(item,slice->element_type,s,op.flag);
            slice->storage->push_back(std::move(item));slice->contextual.push_back(op.flag);++slice->length;break;
        }
        case Op::Slice: {
            auto end=pop(s),start=pop(s),value=pop(s);auto slice=std::get_if<SliceValue>(&value.data);
            if(!slice)runtime_error(s,"index/slice requires a slice or array","E4003");
            I a=op.contextual.at(0)?0:as_int(start,s),b=op.contextual.at(1)?static_cast<I>(slice->length):as_int(end,s);
            if(a<0 || b<a || static_cast<std::size_t>(b)>slice->length)runtime_error(s,"slice bounds must satisfy 0 <= start <= end <= len","E4006");
            auto view=*slice;view.start+=static_cast<std::size_t>(a);view.length=static_cast<std::size_t>(b-a);view.fixed=false;stack_.emplace_back(Value(view));break;
        }
        case Op::MakeStruct: {
            auto object=managed<StructData>();object->name=op.text;stack_.emplace_back(Value(StructValue{object,true}));break;
        }
        case Op::InitField: {
            auto item=pop(s);if(stack_.empty() || !std::holds_alternative<Value>(stack_.back()))invalid(s);
            auto object=std::get_if<StructValue>(&std::get<Value>(stack_.back()).data);if(!object)invalid(s);
            auto value=enforce_type(std::move(item),field_type(model_,object->data->name,op.text,s),s,op.flag);
            object->data->fields.emplace(op.text,copy_value(value));break;
        }
        case Op::Member: {
            auto value=pop(s);
            if(op.text=="len"&&(std::holds_alternative<SliceValue>(value.data)||std::holds_alternative<MapValue>(value.data)||std::holds_alternative<std::string>(value.data))){stack_.emplace_back(aggregate_member(value,op.text,s));break;}
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
        case Op::Return:finish_return(pop(s),op);break;
        case Op::RangeInit: {
            I step=as_int(pop(s),s),end=as_int(pop(s),s),start=as_int(pop(s),s);
            if(!step)runtime_error(s,"range step cannot be zero","E4005");
            f.iterators.push_back({true,op.flag,false,start,end,step,0,{}});break;
        }
        case Op::SliceInit: {
            auto value=pop(s);
            if(auto m=std::get_if<MapValue>(&value.data)){auto entries=managed<std::vector<Value>>();for(const auto& [k,v]:m->data->entries)entries->push_back(make_multi({std::visit([](const auto& x){return Value(x);},k),copy_value(m->writable?v:read_only(v))}));Iterator it{};it.map=true;it.slice={entries,0,entries->size(),false,""};f.iterators.push_back(std::move(it));break;}
            auto p=std::get_if<SliceValue>(&value.data);
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
                if(has){auto v=sequence_read(it.slice,static_cast<std::size_t>(index));item=it.slice.writable?v:read_only(v);}
            }
            if(!has){if(op.target>=f.code->instructions.size())invalid(s);f.pc=op.target;break;}
            if(it.index==std::numeric_limits<I>::max())it.done=true;else ++it.index;
            auto env=managed<Environment>();env->parent=f.environment;f.environment=std::move(env);
            Instruction binding=op;binding.flag=false;binding.type="int";
            Value key(index);if(it.map){auto parts=unpack_multi(item,2,s);key=parts[0];item=op.names.size()==1?key:parts[1];binding.type="";}
            if(op.names.size()==2){binding.text=op.names[0];bind(binding,key);}
            if(op.names.empty() || op.names.size()>2)invalid(s);
            binding.text=op.names.back();binding.type="";bind(binding,std::move(item));break;
        }
        case Op::IterEnd:if(f.iterators.empty())invalid(s);f.iterators.pop_back();break;
        case Op::Fail:runtime_error(s,op.text,op.type.empty()?"E4008":op.type);
        default:invalid(s);
        }
    }
    if(stop_depth==0&&!stack_.empty())invalid({});
}
}
