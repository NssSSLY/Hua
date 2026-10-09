#include "hua/interpreter.hpp"
#include "hua/vm.hpp"
#include "hua/tasks.hpp"
#include <algorithm>
#include <limits>
namespace hua {namespace {
using I=std::int64_t;
Value snapshot(const Value& input,const std::function<Callable(const Callable&,unsigned)>& copy,unsigned depth=0){
    if(depth>64)runtime_error({},"task snapshot contains a cycle or is too deep","E4104");
    if(auto f=std::get_if<Callable>(&input.data))return Value(copy(*f,depth+1));
    if(auto p=std::get_if<StructValue>(&input.data)){auto d=managed<StructData>();d->name=p->data->name;for(const auto& [k,v]:p->data->fields)d->fields.emplace(k,snapshot(v,copy,depth+1));return Value(StructValue{d,false});}
    if(auto p=std::get_if<MapValue>(&input.data)){auto d=managed<MapData>();d->key_type=p->data->key_type;d->item_type=p->data->item_type;for(const auto& [k,v]:p->data->entries)d->entries.emplace(k,snapshot(v,copy,depth+1));return Value(MapValue{d,false});}
    if(auto p=std::get_if<ListValue>(&input.data)){auto d=managed<ListData>();d->element_type=p->data->element_type;for(const auto& v:p->data->values)d->values.push_back(snapshot(v,copy,depth+1));return Value(ListValue{d,false});}
    if(auto p=std::get_if<ResultValue>(&input.data)){auto r=*p;if(p->payload)r.payload=managed<Value>(snapshot(*p->payload,copy,depth+1));r.writable=false;return Value(r);}
    if(auto p=std::get_if<MultiValue>(&input.data)){auto d=managed<MultiData>();d->contextual=p->data->contextual;for(const auto& v:p->data->values)d->values.push_back(snapshot(v,copy,depth+1));return Value(MultiValue{d});}
    if(auto p=std::get_if<SliceValue>(&input.data);p&&!p->packed){auto d=managed<std::vector<Value>>();for(std::size_t i=0;i<p->length;++i)d->push_back(snapshot(sequence_read(*p,i),copy,depth+1));auto s=*p;s.storage=d;s.start=0;s.writable=false;return Value(s);}
    return read_only(copy_value(input,true));
}
void transferable(const Value& v,unsigned depth=0){
    if(depth>64)runtime_error({},"task result is cyclic or too deep","E4104");
    if(std::holds_alternative<Callable>(v.data))runtime_error({},"task results cannot contain closures","E4104");
    if(auto p=std::get_if<StructValue>(&v.data))for(const auto& [_,x]:p->data->fields)transferable(x,depth+1);
    if(auto p=std::get_if<MapValue>(&v.data))for(const auto& [_,x]:p->data->entries)transferable(x,depth+1);
    if(auto p=std::get_if<ListValue>(&v.data))for(const auto& x:p->data->values)transferable(x,depth+1);
    if(auto p=std::get_if<SliceValue>(&v.data))for(std::size_t i=0;i<p->length;++i)transferable(sequence_read(*p,i),depth+1);
    if(auto p=std::get_if<ResultValue>(&v.data);p&&p->payload)transferable(*p->payload,depth+1);
    if(auto p=std::get_if<MultiValue>(&v.data))for(const auto& x:p->data->values)transferable(x,depth+1);
}
std::vector<I> indices(const std::vector<Value>& args,const SourceSpan& span){
    if(args.size()!=7)runtime_error(span,"invalid parallel kernel metadata","E4104");
    auto array=std::get_if<SliceValue>(&args[4].data);if(!array||!numeric_spec(array->element_type))runtime_error(span,"parallel/SIMD requires a numeric array","E4104");
    I start=as_int(args[1],span),end=as_int(args[2],span),step=as_int(args[3],span);bool inclusive=as_bool(args[5],span);
    if(!step)runtime_error(span,"parallel range step cannot be zero","E4005");
    std::vector<I> out;for(I i=start;step>0?(inclusive?i<=end:i<end):(inclusive?i>=end:i>end);){
        if(i<0||static_cast<std::uint64_t>(i)>=array->length)runtime_error(span,"parallel output index out of bounds","E4006");
        if(out.size()>=1000000)runtime_error(span,"parallel iteration limit exceeded","E4099");out.push_back(i);
        if(inclusive&&i==end)break;if((step>0&&i>std::numeric_limits<I>::max()-step)||(step<0&&i<std::numeric_limits<I>::min()-step))break;i+=step;
    }return out;
}
void validate_callable(const Callable& f,const SourceSpan& span){
    if(!f.function||!f.builtin.empty())runtime_error(span,"spawn requires a Hua function","E4104");
    if(f.function->text.find(" external")!=std::string::npos||f.function->text.find(" unsafe")!=std::string::npos)runtime_error(span,"Native/WASM calls cannot run in tasks","E4104");
    for(const auto& p:f.function->children)if(p->kind==NodeKind::Parameter&&(p->text.find(" mut")!=std::string::npos||(!p->children.empty()&&p->children[0]->kind==NodeKind::MutableType)))runtime_error(span,"spawn cannot pass mutable borrows","E4104");
}
}
Value Interpreter::isolated(const Value& value,unsigned depth){
    return snapshot(value,[&](const Callable& f,unsigned d){auto out=f;if(out.receiver)out.receiver=std::get<StructValue>(isolated(Value(*out.receiver),depth+d).data);
        if(out.closure){auto env=managed<Environment>();auto old=std::static_pointer_cast<Environment>(out.closure);for(const auto& [name,b]:old->bindings)env->bindings.emplace(name,Binding{isolated(b.value,depth+d),b.type,false});out.closure=env;}return out;},depth);
}
Value Interpreter::launch(const Callable& callable,const std::vector<Value>& args,const Node& site,std::vector<I> batch,std::string element,bool contextual){
    validate_callable(callable,site.span);auto type=task_result_type(callable);
    if(batch.empty()&&element.empty()&&type.starts_with("Task<")&&!declared_name(*callable.function).starts_with("$async$"))return call(callable,args,site);
    auto heap=std::make_shared<ManagedHeap>();HeapScope heap_scope(*heap);
    auto globals=std::make_shared<Environment>();
    for(const auto& [name,b]:global_.bindings)globals->bindings.emplace(name,Binding{isolated(b.value),b.type,false});
    for(const auto& [name,b]:builtins_.bindings)globals->bindings.emplace(name,b);
    auto fn=std::get<Callable>(isolated(Value(callable)).data);std::vector<Value> values;
    for(const auto& arg:args)values.push_back(isolated(arg));auto copy_site=std::make_shared<Node>(site.kind,site.span);for(const auto& c:site.children)copy_site->add(std::make_unique<Node>(c->kind,c->span,c->text));
    auto* model=&model_;
    return context_->tasks->start([model,globals,fn,values=std::move(values),copy_site,batch=std::move(batch),element,contextual](RuntimeContext& context,std::ostream& output)mutable{
        Interpreter child(*model,output,&context);ExecutionScope execution(child.context_,output);TypeRelationScope relations(model->relations);
        child.global_.bindings=std::move(globals->bindings);child.global_.parent=&child.builtins_;child.environment_=&child.global_;
        // Closures are snapshot environments; only their private global parent is reattached.
        auto attach=[&](const Callable& f){if(f.closure)std::static_pointer_cast<Environment>(f.closure)->parent=&child.global_;};attach(fn);
        for(auto& [_,b]:child.global_.bindings)if(auto f=std::get_if<Callable>(&b.value.data))attach(*f);
        Value result;
        try{if(element.empty())result=child.call(fn,values,*copy_site);else{auto storage=managed<std::vector<Value>>();for(auto i:batch){task_checkpoint(child.context_,copy_site->span);storage->push_back(enforce_type(child.call(fn,{Value(i)},*copy_site),element,copy_site->span,contextual));}result=Value(SliceValue{storage,0,storage->size(),true,element});}
            transferable(result);execution.finish();child.global_.bindings.clear();return result;
        }catch(...){child.global_.bindings.clear();throw;}
    },element.empty()?type:"[]"+element,site.span,heap);
}
Value Interpreter::parallel(const std::vector<Value>& args,const Node& site){
    auto work=indices(args,site.span);auto f=std::get_if<Callable>(&args[0].data);if(!f)runtime_error(site.span,"parallel requires a Hua kernel","E4104");
    auto element=std::get<SliceValue>(args[4].data).element_type;auto count=std::min<std::size_t>(8,work.size());std::vector<Value> tasks;
    for(std::size_t i=0;i<count;++i){auto begin=i*work.size()/count,end=(i+1)*work.size()/count;tasks.push_back(launch(*f,{},site,std::vector<I>(work.begin()+begin,work.begin()+end),element,as_bool(args[6],site.span)));}
    auto storage=managed<std::vector<Value>>();for(const auto& task:tasks){auto result=context_->tasks->await(task,site.span);auto slice=std::get<SliceValue>(result.data);for(std::size_t i=0;i<slice.length;++i)storage->push_back(sequence_read(slice,i));}
    return Value(SliceValue{storage,0,storage->size(),true,element});
}
Value VirtualMachine::isolated(const Value& value,unsigned depth){
    return snapshot(value,[&](const Callable& f,unsigned d){auto out=f;if(out.receiver)out.receiver=std::get<StructValue>(isolated(Value(*out.receiver),depth+d).data);
        if(out.closure){auto env=managed<Environment>();auto old=std::static_pointer_cast<Environment>(out.closure);for(const auto& [name,b]:old->bindings)env->bindings.emplace(name,Binding{isolated(b.value,depth+d),b.type,false});out.closure=env;}return out;},depth);
}
Value VirtualMachine::launch(const Callable& callable,const std::vector<Value>& args,const Instruction& site,std::vector<I> batch,std::string element,bool contextual){
    validate_callable(callable,site.span);auto type=task_result_type(callable);
    if(batch.empty()&&element.empty()&&type.starts_with("Task<")&&!declared_name(*callable.function).starts_with("$async$")){auto depth=frames_.size(),base=stack_.size();call(callable,args,site);dispatch(depth);if(stack_.size()!=base+1)runtime_error(site.span,"invalid task wrapper result","E6001");return pop(site.span);}
    auto heap=std::make_shared<ManagedHeap>();HeapScope heap_scope(*heap);auto globals=std::make_shared<Environment>();
    for(auto env=global_;env;env=env->parent)for(const auto& [name,b]:env->bindings)if(!globals->bindings.contains(name))globals->bindings.emplace(name,Binding{isolated(b.value),b.type,false});
    auto fn=std::get<Callable>(isolated(Value(callable)).data);std::vector<Value> values;for(const auto& arg:args)values.push_back(isolated(arg));auto* model=&model_;auto* bytecode=bytecode_;
    return context_->tasks->start([model,bytecode,globals,fn,values=std::move(values),site,batch=std::move(batch),element,contextual](RuntimeContext& context,std::ostream& output)mutable{
        VirtualMachine child(*model,output,&context);ExecutionScope execution(child.context_,output);TypeRelationScope relations(model->relations);child.bytecode_=bytecode;child.global_=globals;
        auto attach=[&](const Callable& f){if(f.closure)std::static_pointer_cast<Environment>(f.closure)->parent=child.global_;};attach(fn);for(auto& [_,b]:globals->bindings)if(auto f=std::get_if<Callable>(&b.value.data))attach(*f);
        auto invoke=[&](const std::vector<Value>& args){child.call(fn,args,site);child.dispatch();if(!child.completed_)runtime_error(site.span,"invalid task return stack","E6001");auto result=std::move(*child.completed_);child.completed_.reset();return result;};
        Value result;
        try{if(element.empty())result=invoke(values);else{auto storage=managed<std::vector<Value>>();for(auto i:batch){task_checkpoint(child.context_,site.span);storage->push_back(enforce_type(invoke({Value(i)}),element,site.span,contextual));}result=Value(SliceValue{storage,0,storage->size(),true,element});}
            transferable(result);execution.finish();globals->bindings.clear();return result;
        }catch(...){child.cleanup_error();globals->bindings.clear();throw;}
    },element.empty()?type:"[]"+element,site.span,heap);
}
Value VirtualMachine::parallel(const std::vector<Value>& args,const Instruction& site){
    auto work=indices(args,site.span);auto f=std::get_if<Callable>(&args[0].data);if(!f)runtime_error(site.span,"parallel requires a Hua kernel","E4104");
    auto element=std::get<SliceValue>(args[4].data).element_type;auto count=std::min<std::size_t>(8,work.size());std::vector<Value> tasks;
    for(std::size_t i=0;i<count;++i){auto begin=i*work.size()/count,end=(i+1)*work.size()/count;tasks.push_back(launch(*f,{},site,std::vector<I>(work.begin()+begin,work.begin()+end),element,as_bool(args[6],site.span)));}
    auto storage=managed<std::vector<Value>>();for(const auto& task:tasks){auto result=context_->tasks->await(task,site.span);auto slice=std::get<SliceValue>(result.data);for(std::size_t i=0;i<slice.length;++i)storage->push_back(sequence_read(slice,i));}
    return Value(SliceValue{storage,0,storage->size(),true,element});
}
}
