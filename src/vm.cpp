#include "hua/enum_value.hpp"
#include "hua/performance.hpp"
#include "hua/location.hpp"
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

}
const VirtualMachine::FunctionLayout& VirtualMachine::function_layout(const Node& fn) {
    auto [entry,inserted]=function_layouts_.try_emplace(&fn);
    if(inserted){HUA_PROFILE_COUNT(function_metadata_builds);auto& layout=entry->second;layout.name=declared_name(fn);
        layout.external=fn.text.find(" external")!=std::string::npos;layout.unsafe=fn.text.find(" unsafe")!=std::string::npos;
        for(const auto& child:fn.children){HUA_PROFILE_COUNT(parameter_scan_nodes);
            if(child->kind==N::Parameter)layout.parameters.push_back({declared_name(*child),child->children.empty()?"":type_name(*child->children[0]),child->text.find(" mut")!=std::string::npos||(!child->children.empty()&&child->children[0]->kind==N::MutableType)});
            else if(child->kind==N::ReturnTypes)for(const auto& type:child->children)layout.returns.push_back(type_name(*type));
        }
    }
    return entry->second;
}
const std::string& VirtualMachine::field_type(const std::string& owner,const std::string& field,const SourceSpan& span) {
    HUA_PROFILE_COUNT(field_lookups);auto found=field_types_.find(owner);
    if(found==field_types_.end()){
        auto structure=model_.structures.find(owner);if(structure==model_.structures.end())invalid(span);
        std::unordered_map<std::string,std::string> fields;
        for(const auto& item:structure->second->children){HUA_PROFILE_COUNT(field_scan_nodes);fields.emplace(item->text,type_name(*item->children[0]));}
        found=field_types_.emplace(owner,std::move(fields)).first;
    }
    auto type=found->second.find(field);if(type==found->second.end())runtime_error(span,"unknown struct field","E4003");return type->second;
}
VirtualMachine::Binding* VirtualMachine::Environment::own(const std::string& name) {
    if(layout){auto found=layout->indices.find(name);if(found!=layout->indices.end()&&slots[found->second])return &*slots[found->second];}
    auto found=bindings.find(name);return found==bindings.end()?nullptr:&found->second;
}
bool VirtualMachine::Environment::insert(const std::string& name,Binding binding) {
    if(layout){auto found=layout->indices.find(name);if(found!=layout->indices.end()){
        auto& slot=slots[found->second];if(slot)return false;slot.emplace(std::move(binding));HUA_PROFILE_COUNT(vm_slot_writes);return true;
    }}
    return bindings.emplace(name,std::move(binding)).second;
}
VirtualMachine::CodeLayout& VirtualMachine::code_layout(const Code& code,const Node* function) {
    auto [where,inserted]=code_layouts_.try_emplace(&code);
    if(!inserted)return *where->second;
    where->second=std::make_unique<CodeLayout>();auto& result=*where->second;result.accesses.resize(code.instructions.size());
    HUA_PROFILE_COUNT(vm_code_layouts);HUA_PROFILE_ADD(vm_access_index_bytes,result.accesses.size()*sizeof(std::unique_ptr<AccessCache>));
    if(function){const auto& f=function_layout(*function);for(const auto& p:f.parameters)result.root.add(p.name);
        if(model_.nested.contains(function))result.root.add(f.name);
        if(f.name.find('.')!=std::string::npos)result.root.add("self");
    }
    // Source compilers emit balanced lexical scopes even across early Unwind/Return.
    // For other validated control flow this is only a storage hint: missing names
    // use the dictionary, and access routes guard every actually entered scope.
    std::vector<ScopeLayout*> scopes{&result.root};
    for(std::size_t pc=0;pc<code.instructions.size();++pc){const auto& op=code.instructions[pc];
        if(op.op==Op::EnterScope||op.op==Op::IterNext){auto& scope=result.scopes[pc];scopes.push_back(&scope);}
        if(op.op==Op::Bind)scopes.back()->add(op.text);
        else if(op.op==Op::BindMulti||op.op==Op::IterNext)for(const auto& name:op.names)if(name!="_")scopes.back()->add(name);
        if(op.op==Op::LeaveScope&&scopes.size()>1)scopes.pop_back();
    }
    return result;
}
VirtualMachine::AccessCache& VirtualMachine::access(Frame& frame) {
    auto& item=frame.layout->accesses.at(frame.pc-1);
    if(!item){item=std::make_unique<AccessCache>();HUA_PROFILE_COUNT(vm_access_caches);}
    return *item;
}
VirtualMachine::Binding& VirtualMachine::lookup(Frame& frame,const Instruction& op) {
    HUA_PROFILE_COUNT(environment_lookups);auto& route=access(frame).name;
    if(route.slot!=no_slot){auto* env=frame.environment.get();bool valid=true;
        for(std::size_t i=0;i<route.guards.size();++i){const auto& guard=route.guards[i];HUA_PROFILE_COUNT(vm_slot_guards);
            if(!env||env->layout!=guard.layout||!env->bindings.empty()){valid=false;break;}
            if(i+1==route.guards.size()){
                if(route.slot<env->slots.size()&&env->slots[route.slot]){HUA_PROFILE_COUNT(vm_slot_hits);return *env->slots[route.slot];}
                valid=false;break;
            }
            // A late binding in an intervening scope must shadow an earlier route.
            if(guard.shadow!=no_slot&&env->slots[guard.shadow]){valid=false;break;}
            env=env->parent.get();
        }
        (void)valid;
    }
    HUA_PROFILE_COUNT(vm_slot_misses);std::vector<NameRoute::Guard> guards;bool cacheable=true;
    for(auto* env=frame.environment.get();env;env=env->parent.get()){
        HUA_PROFILE_COUNT(environment_probes);std::size_t slot=no_slot;
        if(env->layout){HUA_PROFILE_COUNT(vm_lookup_hashes);auto item=env->layout->indices.find(op.text);if(item!=env->layout->indices.end())slot=item->second;}
        cacheable=cacheable&&env->layout&&env->bindings.empty();
        guards.push_back({env->layout,slot});
        if(slot!=no_slot&&env->slots[slot]){
            if(cacheable){route.guards=std::move(guards);route.slot=slot;}
            return *env->slots[slot];
        }
        if(!env->bindings.empty()){HUA_PROFILE_COUNT(vm_lookup_hashes);auto found=env->bindings.find(op.text);if(found!=env->bindings.end())return found->second;}
    }
    runtime_error(op.span,"name `"+op.text+"` is not initialized","E4001");
}
Value* VirtualMachine::field(FieldAccess& cache,const StructValue& object,const std::string& name) {
    // Retain the shape too: Task results may carry a layout from another VM.
    // An index never points into another instance or aliases an initialized nil.
    if(auto* layout=object.data->fields.layout()){
        if(cache.layout.get()==layout){HUA_PROFILE_COUNT(vm_field_hits);return object.data->fields.value_at(cache.index);}
        HUA_PROFILE_COUNT(vm_field_misses);cache.layout=object.data->fields.retain_layout();cache.index=layout->index(name);
        return object.data->fields.value_at(cache.index);
    }
    // Ordinary record fields never erase nodes after construction. Enum tag
    // initialization can erase inactive fields, so enum accesses always resolve.
    if(!object.data->enumeration&&cache.identity==object.data.get()&&!cache.owner.expired()){
        HUA_PROFILE_COUNT(vm_field_hits);return cache.value;
    }
    HUA_PROFILE_COUNT(vm_field_misses);auto found=object.data->fields.find(name);
    if(found==object.data->fields.end())return nullptr;
    if(!object.data->enumeration){cache.owner=object.data;cache.identity=object.data.get();cache.value=&found->second;}
    return &found->second;
}
const std::string& VirtualMachine::field_type(FieldAccess& cache,const std::string& owner,const std::string& name,const SourceSpan& span){
    if(cache.type&&cache.type_owner==owner){HUA_PROFILE_COUNT(vm_field_type_hits);return *cache.type;}
    cache.type=&field_type(owner,name,span);cache.type_owner=owner;return *cache.type;
}
Value VirtualMachine::pop(const SourceSpan& span) {
    if(stack_.empty() || (!frames_.empty() && stack_.size()<=frames_.back().base) || !std::holds_alternative<Value>(stack_.back()))invalid(span);
    auto value=std::move(std::get<Value>(stack_.back()));stack_.pop_back();
    if(auto object=std::get_if<StructValue>(&value.data))require_complete_enum(*object->data,span);
    return value;
}
std::vector<Value> VirtualMachine::arguments(std::size_t count,const SourceSpan& span) {
    if(count>stack_.size() || (!frames_.empty() && count>stack_.size()-frames_.back().base))invalid(span);std::vector<Value> args(count);
    for(std::size_t i=count;i>0;--i)args[i-1]=pop(span);return args;
}
void VirtualMachine::bind(const Instruction& op,Value value) {
    if(std::holds_alternative<MultiValue>(value.data))runtime_error(op.span,"multiple values require positional binding","E4003");
    auto type=op.type.empty()?value_type(value):op.type;value=copy_value(std::move(value));
    if(!op.flag)value=read_only(std::move(value));
    if(!frames_.back().environment->insert(op.text,Binding{std::move(value),type,op.flag}))
        runtime_error(op.span,"duplicate runtime binding `"+op.text+"`","E4001");
}
EntryResult VirtualMachine::run(const Bytecode& bytecode){
    ExecutionScope execution(context_,output_);context_->type_relations=&model_.relations;context_->resumable=true;
    sync_wait(run_body(bytecode,execution),*context_);return std::move(entry_);
}
Routine<void> VirtualMachine::run_body(const Bytecode& bytecode,ExecutionScope& execution){
    if(model_.external)model_.external->reset();
    bytecode_=&bytecode;steps_=0;frames_.clear();stack_.clear();function_layouts_.clear();field_types_.clear();enum_layouts_.clear();record_layouts_.clear();code_layouts_.clear();builtin_layout_={};
    auto& initial_layout=code_layout(bytecode.initializer);
    for(const auto& name:builtin_names()){builtin_layout_.add(name);builtin_layout_.add(std::string("$core$")+name);}
    for(const auto& [name,fn]:model_.functions)if(!model_.nested.contains(fn))initial_layout.root.add(name);
    auto builtins=managed<Environment>(&builtin_layout_);global_=managed<Environment>(&initial_layout.root);global_->parent=builtins;
    for(const auto& name:builtin_names()){
        builtins->insert(name,Binding{Value(Callable{nullptr,name,{}}),std::string("builtin:")+name,false});
        builtins->insert(std::string("$core$")+name,Binding{Value(Callable{nullptr,name,{}}),std::string("builtin:")+name,false});
    }
    for(const auto& [name,fn]:model_.functions)if(!model_.nested.contains(fn))global_->insert(name,Binding{Value(Callable{fn,"",{}}),"fn:"+name,false});
    std::exception_ptr error;Value returned;SourceSpan entry_span;
    try{
        frames_.push_back({&bytecode.initializer,nullptr,0,0,{},global_,{},&initial_layout});(co_await dispatch());
        if(auto main=model_.functions.find("main");main!=model_.functions.end()){
            entry_span=main->second->span;completed_.reset();
            Instruction site{};site.span=entry_span;(co_await call(Callable{main->second,"",{}},{},site));if(frames_.empty())stack_.clear();else (co_await dispatch());
            if(completed_)returned=std::move(*completed_);
            if(std::holds_alternative<TaskValue>(returned.data))returned=co_await context_->tasks->await_async(returned,entry_span);
        }
    }catch(...){error=std::current_exception();}
    if(error)(co_await cleanup_error());
    try{co_await execution.finish_async(bool(error));}catch(...){if(!error)error=std::current_exception();}
    global_->clear();stack_.clear();frames_.clear();completed_.reset();if(error)std::rethrow_exception(error);
    entry_=entry_result(returned,entry_span);co_return;
}
Routine<void> VirtualMachine::call(const Callable& callable,const std::vector<Value>& args,const Instruction& site) {
    if(callable.builtin=="task_start"){if(args.empty()||!std::holds_alternative<Callable>(args[0].data))runtime_error(site.span,"spawn requires a Hua function","E4104");auto nested=site;if(!nested.contextual.empty())nested.contextual.erase(nested.contextual.begin());stack_.emplace_back(co_await launch(std::get<Callable>(args[0].data),std::vector<Value>(args.begin()+1,args.end()),nested));co_return;}
    if(callable.builtin=="parallel_map"){stack_.emplace_back(co_await parallel(args,site));co_return;}
    if(!callable.builtin.empty()){HUA_PROFILE_COUNT(builtin_calls);stack_.emplace_back(co_await invoke_resumable(callable.builtin,args,site.span,output_,site.contextual,site.type,context_));co_return;}
    if(!callable.function)invalid(site.span);
    HUA_PROFILE_COUNT(user_calls);
    const auto& fn=*callable.function;const auto& layout=function_layout(fn);const auto& key=layout.name;
    if(layout.external){if(context_&&context_->in_task)runtime_error(site.span,"Native/WASM calls cannot run in tasks","E4104");stack_.emplace_back(model_.external->call(key,args,site.contextual,site.span));co_return;}
    if(layout.unsafe)runtime_error(site.span,"unsafe function execution/FFI is not implemented","E4008");
    const auto user_calls=frames_.size()-(!frames_.empty() && !frames_.front().function?1:0);
    if(user_calls>=context_->execution_limits.calls)runtime_error(site.span,"function call depth limit exceeded ("+std::to_string(context_->execution_limits.calls)+")","E4099");
    if(context_->execution_metrics)context_->execution_metrics->call(static_cast<unsigned>(user_calls+1));
    const auto& ps=layout.parameters;
    if(ps.size()!=args.size())runtime_error(site.span,"function argument count mismatch","E4003");
    if(context_&&context_->in_task&&callable.closure)std::static_pointer_cast<Environment>(callable.closure)->parent=global_;
    auto code=bytecode_->functions.find(&fn);auto* slots=code==bytecode_->functions.end()?nullptr:&code_layout(code->second,&fn);
    auto local=managed<Environment>(slots?&slots->root:nullptr);local->parent=callable.closure?std::static_pointer_cast<Environment>(callable.closure):global_;
    if(callable.closure)local->insert(key,Binding{Value(callable),"fn:"+key,false});
    if(callable.receiver) {
        if(model_.mutating.at(key) && !callable.receiver->writable)runtime_error(site.span,"mutating method requires mutable receiver","E4007");
        local->insert("self",Binding{Value(*callable.receiver),callable.receiver->data->name,false});
    }
    for(std::size_t i=0;i<ps.size();++i) {
        const auto& p=ps[i];const auto& type=p.type;bool mut=p.mutable_view;
        if(mut && !writable(args[i]))runtime_error(site.span,"mut parameter requires mutable argument","E4007");
        HUA_PROFILE_CONSTRAINT_SOURCE(argument);
        auto v=mut?enforce_borrow(args[i],type,site.span):enforce_type(copy_value(args[i]),type,site.span,i<site.contextual.size()&&site.contextual[i]);
        if(!mut)v=read_only(std::move(v));local->insert(p.name,Binding{std::move(v),type,false});
    }
    if(code==bytecode_->functions.end())invalid(site.span);
    if(observer_)observer_->enter(&fn,frames_.empty()?nullptr:frames_.back().function,args);
    frames_.push_back({&code->second,&fn,0,stack_.size(),site.span,std::move(local),{},slots});
    co_return;
}
Routine<void> VirtualMachine::finish_return(Value value,const Instruction& op,bool early) {
    // Save the language value before deferred calls can mutate its source.
    value=copy_value(std::move(value));
    auto f=frames_.back();
    for(auto env=f.environment;env&&env!=global_;env=env->parent)(co_await drain(env));if(!f.function)(co_await drain(global_));if(early&&!f.function)invalid(op.span);if(f.function)value=enforce_return(std::move(value),function_layout(*f.function).returns,f.site,op.flag);
    auto base=f.base;if(!early&&stack_.size()!=base)invalid(op.span);frames_.pop_back();stack_.resize(base);// enforce_return already makes the language value copy at the function boundary.
    if(!f.function)value=copy_value(value);
    if(!frames_.empty())stack_.emplace_back(std::move(value));else completed_=std::move(value);
    if(observer_)observer_->leave(f.function,frames_.empty()?(completed_?&*completed_:nullptr):std::get_if<Value>(&stack_.back()));
    co_return;
}
Routine<void> VirtualMachine::drain(const std::shared_ptr<Environment>& env){
    struct Cleanup {bool& flag;bool previous;Cleanup(bool& f):flag(f),previous(f){flag=true;}~Cleanup(){flag=previous;}} cleanup(context_->cleanup);
    auto deferred=std::move(env->deferred);env->deferred.clear();std::exception_ptr first;
    for(auto i=deferred.rbegin();i!=deferred.rend();++i){
        auto depth=frames_.size(),base=stack_.size();bool failed=false;
        try{(co_await call(i->callable,i->arguments,i->site));(co_await dispatch(depth));}catch(...){if(!first)first=std::current_exception();failed=true;}
        if(failed)while(frames_.size()>depth){
            auto environment=frames_.back().environment;
            for(auto e=environment;e&&e!=env&&e!=global_;e=e->parent)try{(co_await drain(e));}catch(...){}
            frames_.pop_back();
        }
        stack_.resize(base);
    }
    if(first)std::rethrow_exception(first);
    co_return;
}
Routine<void> VirtualMachine::cleanup_error(){
    struct Unwinding {bool& flag;bool old;Unwinding(bool& f):flag(f),old(f){flag=true;}~Unwinding(){flag=old;}} failed(context_->unwinding);
    std::unordered_set<Environment*> seen;auto frames=frames_;
    for(auto i=frames.rbegin();i!=frames.rend();++i)for(auto env=i->environment;env;env=env->parent)if(seen.insert(env.get()).second)try{(co_await drain(env));}catch(...){}
    co_return;
}
Routine<void> VirtualMachine::dispatch(std::size_t stop_depth) {
    while(frames_.size()>stop_depth) {
        auto& f=frames_.back();if(f.pc>=f.code->instructions.size())invalid(f.site);
        const auto& op=f.code->instructions[f.pc++];const auto& s=op.span;
        HUA_PROFILE_COUNT(vm_steps);co_await Yield{};task_checkpoint(context_,s);if(context_->execution_metrics)++context_->execution_metrics->steps;if(++steps_>context_->execution_limits.steps)runtime_error(s,"execution step limit exceeded ("+std::to_string(context_->execution_limits.steps)+")","E4099");
        try{
        switch(op.op) {
        case Op::Closure:{auto fn=model_.functions.find(op.text);if(fn==model_.functions.end())invalid(s);auto captured=managed<Environment>();captured->parent=global_;for(auto env=f.environment;env&&env!=global_;env=env->parent)env->each([&](const auto& name,const auto& binding){if(!captured->bindings.contains(name))captured->bindings.emplace(name,Binding{read_only(copy_value(binding.value)),binding.type,false});});Callable callable{fn->second,"",{},captured};std::weak_ptr<Environment> weak=captured;callable.resource_guard=[weak](unsigned d){if(auto env=weak.lock())for(const auto& [_,b]:env->bindings)if(contains_file(b.value,d+1))return true;return false;};stack_.emplace_back(Value(std::move(callable)));break;}
        case Op::Defer:{auto args=arguments(op.argument,s);auto value=pop(s);auto fn=std::get_if<Callable>(&value.data);if(!fn)runtime_error(s,"defer requires a callable","E4003");for(auto& arg:args)arg=copy_value(arg);f.environment->deferred.push_back({*fn,std::move(args),op});break;}
        case Op::ExternalInit:if(context_&&context_->in_task)runtime_error(s,"Native/WASM initialization cannot run in tasks","E4104");model_.external->initialize(op.text,s);break;
        case Op::Constant:stack_.emplace_back(op.constant);break;
        case Op::Load:stack_.emplace_back(lookup(f,op).value);break;
        case Op::Bind:{HUA_PROFILE_CONSTRAINT_SOURCE(binding);auto v=enforce_type(pop(s),op.type,s,!op.contextual.empty()&&op.contextual[0]);bind(op,std::move(v));break;}
        case Op::Pop:pop(s);break;
        case Op::Unary:stack_.emplace_back(unary_value(op.text,pop(s),s));break;
        case Op::Binary:{auto right=pop(s),left=pop(s);stack_.emplace_back(binary_value(op.text,left,right,s,op.flag));break;}
        case Op::Jump:if(op.target>=f.code->instructions.size())invalid(s);f.pc=op.target;break;
        case Op::JumpFalse:case Op::JumpTrue: {
            auto condition=as_bool(pop(s),s);
            if(condition==(op.op==Op::JumpTrue)){if(op.target>=f.code->instructions.size())invalid(s);f.pc=op.target;}break;
        }
        case Op::EnterScope:{auto env=managed<Environment>(&f.layout->scopes.at(f.pc-1));HUA_PROFILE_COUNT(vm_scopes);HUA_PROFILE_ADD(vm_scope_slot_bytes,env->slots.size()*sizeof(std::optional<Binding>));env->parent=f.environment;f.environment=std::move(env);break;}
        case Op::LeaveScope:case Op::Unwind: {
            auto count=op.op==Op::LeaveScope?1:op.argument;
            for(std::size_t i=0;i<count;++i){if(!frames_.back().environment->parent || frames_.back().environment==global_)invalid(s);auto env=frames_.back().environment;(co_await drain(env));frames_.back().environment=env->parent;}break;
        }
        case Op::LocateName: {
            if(op.text=="_"){stack_.emplace_back(Location{});break;}
            auto& b=lookup(f,op);if(!b.mutable_binding)runtime_error(s,"cannot mutate immutable binding or read-only view","E4007");
            stack_.emplace_back(Location{&b.value,b.type,true,f.environment,{},{}});break;
        }
        case Op::LocateField: {
            auto value=pop(s);auto object=std::get_if<StructValue>(&value.data);
            if(!object)runtime_error(s,"field assignment requires a struct","E4003");
            if(object->data->enumeration)runtime_error(s,"enum fields are private","E4003");
            auto& cache=access(f).field;auto* item=field(cache,*object,op.text);if(!item)runtime_error(s,"unknown struct field","E4003");
            if(!object->writable)runtime_error(s,"cannot mutate immutable binding or read-only view","E4007");
            stack_.emplace_back(Location{item,field_type(cache,object->data->name,op.text,s),true,object->data,{},{}});break;
        }
        case Op::LocateIndex:case Op::Index: {
            auto index=pop(s),value=pop(s);if(op.op==Op::Index)stack_.emplace_back(index_value(value,index,s));else {auto l=index_location(std::move(value),std::move(index),s);if(!l.writable)runtime_error(s,"cannot mutate immutable binding or read-only view","E4007");stack_.emplace_back(std::move(l));}break;
        }
        case Op::Store: {
            auto value=pop(s);if(stack_.empty() || !std::holds_alternative<Location>(stack_.back()))invalid(s);
            if(stack_.size()<=f.base)invalid(s);
            auto where=std::get<Location>(stack_.back());stack_.pop_back();
            if(op.text!="=")value=binary_value(op.text.substr(0,op.text.size()-1),location_read(where,s),value,s,op.flag);
            value=enforce_type(std::move(value),where.type,s,op.flag||op.text!="=");location_write(where,value,s);stack_.emplace_back(std::move(value));break;
        }
        case Op::MakeMulti:{auto values=arguments(op.argument,s);for(auto& v:values)v=copy_value(v);stack_.emplace_back(make_multi(std::move(values),op.contextual));break;}
        case Op::BindMulti:{auto pack=pop(s);auto values=unpack_multi(pack,op.argument,s);auto& cached=access(f).multi_types;if(!cached)cached=type_arguments(op.type,"multi");else HUA_PROFILE_COUNT(vm_multi_type_hits);const auto& types=*cached;if(types.size()!=values.size()||op.names.size()!=values.size())invalid(s);auto data=std::get<MultiValue>(pack.data).data;
            for(std::size_t i=0;i<values.size();++i){values[i]=enforce_type(std::move(values[i]),types[i],s,i<data->contextual.size()&&data->contextual[i]);if(op.names[i]!="_"&&f.environment->own(op.names[i]))runtime_error(s,"duplicate runtime binding","E4001");}
            for(std::size_t i=0;i<values.size();++i)if(op.names[i]!="_"){auto item=op;item.text=op.names[i];item.type=types[i];bind(item,values[i]);}break;}
        case Op::StoreMulti:{auto pack=pop(s);auto values=unpack_multi(pack,op.argument,s);auto data=std::get<MultiValue>(pack.data).data;std::vector<Location> ls(op.argument);
            for(std::size_t i=op.argument;i>0;--i){if(stack_.size()<=f.base||!std::holds_alternative<Location>(stack_.back()))invalid(s);ls[i-1]=std::get<Location>(stack_.back());stack_.pop_back();}
            for(std::size_t i=0;i<values.size();++i)values[i]=copy_value(enforce_type(std::move(values[i]),ls[i].type,s,i<data->contextual.size()&&data->contextual[i]));
            prepare_map_writes(ls,s);for(std::size_t i=0;i<values.size();++i)if(ls[i].value||ls[i].map||ls[i].sequence)location_write(ls[i],values[i],s);break;}
        case Op::MakeMap:stack_.emplace_back(make_map(op.type,s));break;
        case Op::MapInsert:{auto item=pop(s),key=pop(s);if(stack_.empty()||!std::holds_alternative<Value>(stack_.back()))invalid(s);auto m=std::get_if<MapValue>(&std::get<Value>(stack_.back()).data);if(!m)invalid(s);map_put(*m,key,std::move(item),s,op.flag);break;}
        case Op::Propagate:{auto value=pop(s);auto r=std::get_if<ResultValue>(&value.data);if(!r)runtime_error(s,"expected Result","E4003");if(r->ok)stack_.emplace_back(result_payload(value,true,s));else (co_await finish_return(propagated_error(value,s),op,true));break;}
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
            if(op.text=="std.error.Value")runtime_error(s,"standard Error must be constructed through std.error","E4003");
        if(op.text=="std.file.File"||op.text=="std.file.IOFailure")runtime_error(s,"opaque File/IOFailure must be constructed through std.file","E4003");
            auto object=managed<StructData>();object->name=op.text;
            auto [layout,inserted]=enum_layouts_.try_emplace(op.text);
            if(inserted)layout->second=enum_layout(*model_.structures.at(op.text));
            object->enumeration=layout->second;
            if(!object->enumeration){auto [shape,fresh]=record_layouts_.try_emplace(op.text);if(fresh)shape->second=record_layout(*model_.structures.at(op.text));object->fields=RecordFields(shape->second);}
            stack_.emplace_back(Value(StructValue{object,true}));break;
        }
        case Op::InitField: {
            auto item=pop(s);if(stack_.empty() || !std::holds_alternative<Value>(stack_.back()))invalid(s);
            auto object=std::get_if<StructValue>(&std::get<Value>(stack_.back()).data);if(!object)invalid(s);
            auto value=enforce_type(std::move(item),field_type(access(f).field,object->data->name,op.text,s),s,op.flag);
            initialize_field(*object->data,op.text,std::move(value),s);break;
        }
        case Op::Member: {
            auto value=pop(s);
            if(op.text=="len"&&(std::holds_alternative<SliceValue>(value.data)||std::holds_alternative<MapValue>(value.data)||std::holds_alternative<std::string>(value.data))){stack_.emplace_back(aggregate_member(value,op.text,s));break;}
            auto object=std::get_if<StructValue>(&value.data);if(!object)runtime_error(s,"member access requires a struct","E4003");
            if(auto* item=field(access(f).field,*object,op.text)){stack_.emplace_back(object->writable?*item:read_only(*item));break;}
            auto method=model_.functions.find(object->data->name+"."+op.text);
            if(method==model_.functions.end())runtime_error(s,"unknown struct member","E4003");
            if(method->second->span.file_id!=s.file_id && method->second->text.find(" pub")==std::string::npos)
                runtime_error(s,"method is private to its defining module","E5003");
            stack_.emplace_back(Value(Callable{method->second,"",*object}));break;
        }
        case Op::Call: {
            auto args=arguments(op.argument,s);auto callee=pop(s);auto p=std::get_if<Callable>(&callee.data);
            if(!p)runtime_error(s,"value is not callable","E4003");(co_await call(*p,args,op));break;
        }
        case Op::Return:co_await finish_return(pop(s),op);break;
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
            auto env=managed<Environment>(&f.layout->scopes.at(f.pc-1));HUA_PROFILE_COUNT(vm_scopes);HUA_PROFILE_ADD(vm_scope_slot_bytes,env->slots.size()*sizeof(std::optional<Binding>));env->parent=f.environment;f.environment=std::move(env);
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
        }catch(const Diagnostic& error){
            auto reported=error.located(s);
            for(auto frame=frames_.rbegin();frame!=frames_.rend();++frame)if(frame->function)reported.add_context(frame->site,"called from");
            throw reported;
        }
    }
    if(stop_depth==0&&!stack_.empty())invalid({});
    co_return;
}
}
