#include "hua/tasks.hpp"
#include "hua/file.hpp"
#include "hua/stdlib.hpp"
#include "hua/runtime.hpp"
#include <exception>
#include <algorithm>
namespace hua {
namespace {
std::atomic<unsigned> running{0};
std::shared_ptr<TaskState> task_state(const Value& v,const SourceSpan& s){auto t=std::get_if<TaskValue>(&v.data);if(!t||!t->data)runtime_error(s,"expected Task<T>","E4003");return t->data;}
std::chrono::milliseconds duration(const Value& v,const SourceSpan& s){auto ms=as_int(v,s);if(ms<0||ms>86400000)runtime_error(s,"duration must be 0..86400000 milliseconds","E4103");return std::chrono::milliseconds(ms);}
std::string failure(std::exception_ptr error){try{std::rethrow_exception(error);}catch(const Diagnostic& e){return e.code()+": "+e.what();}catch(const std::exception& e){return e.what();}catch(...){return "task failed";}}
bool observed(const std::shared_ptr<TaskState>& state){std::lock_guard lock(state->mutex);return state->observed;}

}
TaskRuntime::TaskRuntime(RuntimeContext& c,std::ostream& o):context(c),output(o){if(!context.heap)context.heap=std::make_shared<ManagedHeap>(context.execution_limits.collection_interval);}
TaskState::~TaskState(){result={};if(heap)heap->collect();}
void release_task_admission()noexcept{--running;}
TaskRuntime::~TaskRuntime(){try{finish(true);}catch(...) {}}
void cancellation_checkpoint(RuntimeContext* c,const SourceSpan& s){if(c&&!c->cleanup&&c->cancellation&&c->cancellation->stopped())runtime_error(s,"task canceled","E4101");}
bool file_pump_pending(const RuntimeContext* c) noexcept{return c&&(!c->resumable||!c->in_task)&&c->files&&c->files->created->load(std::memory_order_acquire);}
bool builtin_may_suspend(const std::string& name,const RuntimeContext* c){
    if(name=="task_await"||name=="task_group_end")return true;
    auto f=standard_function(name);if(!f)return false;
    if(f->module=="task"&&(f->name=="timeout"||f->name=="yield"||f->name=="sleep"))return true;
    if(c&&c->in_task&&((f->module=="gc"&&f->name=="collect")||f->module=="file"||f->module=="fs"||f->module=="io"||f->module=="process"))return true;
    return false;
}
void task_checkpoint(RuntimeContext* c,const SourceSpan& s){if(!c||!c->resumable||!c->in_task)file_checkpoint(c);if(c&&!c->cleanup&&c->cancellation&&c->cancellation->stopped())runtime_error(s,"task canceled","E4101");}
Value TaskRuntime::start(TaskJob job,std::string type,const SourceSpan& span,std::shared_ptr<ManagedHeap> heap){
    TaskResumeJob resumable=[job=std::move(job)](RuntimeContext& child,std::ostream& out)->Routine<Value>{
        auto h=child.heap;
        co_return co_await Blocking([&child,&out,job,h]{
            HeapScope scope(*h);TypeRelations empty;TypeRelationScope relation(child.type_relations?*child.type_relations:empty);
            auto c=child;c.resumable=false;return job(c,out);
        });
    };
    return start_resumable(std::move(resumable),std::move(type),span,std::move(heap));
}
Value TaskRuntime::start_resumable(TaskResumeJob job,std::string type,const SourceSpan& span,std::shared_ptr<ManagedHeap> heap){
    task_checkpoint(&context,span);
    if(children.size()>=4096)runtime_error(span,"task creation limit exceeded (4096 per scope)","E4099");
    prepare_file_scope(context);
    auto child=context;
    child.task_id=next_file_task_id();child.files=std::make_shared<FileScope>(child.task_id,context.files->root);
    auto old=running.fetch_add(1);if(old>=64){--running;runtime_error(span,"concurrent task limit exceeded (64)","E4099");}
    struct Admission {bool transferred{};~Admission(){if(!transferred)--running;}} admission;
    auto state=std::make_shared<TaskState>();state->heap=heap?std::move(heap):std::make_shared<ManagedHeap>(context.execution_limits.collection_interval);
    state->result_type=std::move(type);state->cancellation=std::make_shared<Cancellation>();state->cancellation->parent=context.cleanup?nullptr:context.cancellation;
    child.tasks=nullptr;child.cancellation=state->cancellation;child.in_task=true;child.resumable=true;child.cleanup=false;child.input=nullptr;
    children.push_back(state);
    try{enqueue_task(state,std::move(job),std::move(child));admission.transferred=true;}catch(...){children.pop_back();throw;}
    return Value(TaskValue{state});
}
Routine<Value> TaskRuntime::await_async(Value value,SourceSpan span,std::optional<std::chrono::milliseconds> timeout){
    auto state=task_state(value,span);auto deadline=timeout?Clock::now()+*timeout:Clock::time_point::max();
    bool timed_out=false,canceled=false;
    auto done=[state]{std::lock_guard lock(state->mutex);return state->finished;};
    while(!done()){
        if(!context.cleanup&&context.cancellation&&context.cancellation->stopped()){canceled=true;state->cancellation->requested=true;break;}
        if(Clock::now()>=deadline){timed_out=true;state->cancellation->requested=true;break;}
        co_await Suspend{[this,done]{return done()||(!context.cleanup&&context.cancellation&&context.cancellation->stopped());},deadline};
    }
    notify_task_scheduler();
    // A soft deadline requests cancellation, then waits for cleanup and confirmed native completion.
    if(!done())co_await Suspend{done};
    {std::lock_guard lock(state->mutex);state->observed=true;if(!state->emitted){output<<state->output;state->emitted=true;}}
    if(canceled)runtime_error(span,"task canceled while awaiting a dependency","E4101");
    if(timed_out)co_return standard_result(Value(std::string("timeout")),false,state->result_type);
    if(state->error){if(timeout)co_return standard_result(Value(failure(state->error)),false,state->result_type);std::rethrow_exception(state->error);}
    auto result=copy_value(state->result,true);
    co_return timeout?standard_result(std::move(result),true,state->result_type):result;
}
Value TaskRuntime::await(const Value& value,const SourceSpan& span,std::optional<std::chrono::milliseconds> timeout){return sync_wait(await_async(value,span,timeout),context);}
Routine<void> TaskRuntime::finish_async(bool cancel){
    if(cancel)for(auto& child:children)child->cancellation->requested=true;
    std::exception_ptr first;bool old=context.cleanup;context.cleanup=true;
    for(const auto& child:children)try{if(!observed(child))co_await await_async(Value(TaskValue{child}),{});}catch(...){if(!first)first=std::current_exception();for(auto& other:children)other->cancellation->requested=true;}
    context.cleanup=old;children.clear();groups.clear();if(first&&!cancel)std::rethrow_exception(first);
}
std::size_t TaskRuntime::begin_group(){auto id=next_group++;groups.emplace_back(id,children.size());return id;}
Routine<void> TaskRuntime::end_group_async(std::size_t id,SourceSpan span){
    if(groups.empty()||groups.back().first!=id)runtime_error(span,"invalid taskgroup scope","E4103");
    auto start=groups.back().second;groups.pop_back();std::exception_ptr first;
    if(context.unwinding||(context.cancellation&&context.cancellation->stopped()))for(std::size_t i=start;i<children.size();++i)children[i]->cancellation->requested=true;
    for(std::size_t i=start;i<children.size();++i)try{if(!observed(children[i]))co_await await_async(Value(TaskValue{children[i]}),span);}catch(...){if(!first)first=std::current_exception();for(std::size_t j=start;j<children.size();++j)children[j]->cancellation->requested=true;}
    if(first)std::rethrow_exception(first);
}
void TaskRuntime::finish(bool cancel){sync_wait(finish_async(cancel),context);}
void TaskRuntime::end_group(std::size_t id,const SourceSpan& span){sync_wait(end_group_async(id,span),context);}
ExecutionScope::ExecutionScope(RuntimeContext*& pointer,std::ostream& out):target(pointer),previous(pointer),context(std::make_unique<RuntimeContext>(pointer?*pointer:RuntimeContext{})),heap(context->heap?context->heap:std::make_shared<ManagedHeap>(context->execution_limits.collection_interval)),heap_scope(*heap){if(!context->in_task){context->task_id=next_file_task_id();context->files.reset();}prepare_file_scope(*context);runtime=std::make_unique<TaskRuntime>(*context,out);context->heap=heap;context->tasks=runtime.get();target=context.get();}
ExecutionScope::~ExecutionScope(){runtime.reset();try{heap->collect();finish_file_scope(context.get());}catch(...){}target=previous;}
void ExecutionScope::finish(){try{runtime->finish();}catch(...){finish_file_scope(context.get());throw;}finish_file_scope(context.get());}
Routine<void> ExecutionScope::finish_async(bool cancel){
    std::exception_ptr error;try{co_await runtime->finish_async(cancel);}catch(...){error=std::current_exception();}
    if(context->files->usage().used[std::size_t(resources::Metric::handles)])co_await Blocking([this]{finish_file_scope(context.get());return Value{};});else finish_file_scope(context.get());
    if(error)std::rethrow_exception(error);
}
std::string task_result_type(const Callable& callable){if(!callable.function)return "";for(const auto& child:callable.function->children)if(child->kind==NodeKind::ReturnTypes)return child->children.size()==1?type_name(*child->children[0]):"";return "void";}
Value task_standard(std::string_view op,const std::vector<Value>& args,const SourceSpan& span,RuntimeContext* c){
    if(!c||!c->tasks)runtime_error(span,"task runtime is unavailable","E4103");
    if(op=="sleep"||op=="yield"){
        auto until=std::chrono::steady_clock::now()+(op=="sleep"?duration(args[0],span):std::chrono::milliseconds(0));
        do{task_checkpoint(c,span);if(op=="yield")std::this_thread::yield();else if(std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::min(std::chrono::milliseconds(2),std::chrono::duration_cast<std::chrono::milliseconds>(until-std::chrono::steady_clock::now())));}while(std::chrono::steady_clock::now()<until);
        return {};
    }
    if(op=="worker_id"){std::ostringstream s;s<<std::this_thread::get_id();return Value(s.str());}
    if(op=="all"||op=="race")return task_combine(op,args,span,c);
    auto state=task_state(args[0],span);
    if(op=="cancel"){std::lock_guard lock(state->mutex);if(state->finished)return Value(false);state->cancellation->requested=true;state->changed.notify_all();notify_task_scheduler();return Value(true);}
    if(op=="done"){std::lock_guard lock(state->mutex);return Value(state->finished);}
    if(op=="state"){std::lock_guard lock(state->mutex);return Value(std::string(!state->finished?(state->cancellation->stopped()?"canceling":"running"):state->error?(state->canceled_failure?"canceled":"failed"):"completed"));}
    if(op=="timeout")return c->tasks->await(args[0],span,duration(args[1],span));
    runtime_error(span,"unknown task operation","E4103");
}
Value task_combine(std::string_view op,const std::vector<Value>& args,const SourceSpan& span,RuntimeContext* c){
    auto left=task_state(args[0],span),right=task_state(args[1],span);auto type=left->result_type;
    TaskResumeJob job=[args,op=std::string(op),span,type](RuntimeContext& child,std::ostream& out)->Routine<Value>{
        TaskRuntime runtime(child,out);child.tasks=&runtime;Value result;std::exception_ptr error;
        try{
            if(op=="all"){auto values=managed<std::vector<Value>>();values->push_back(co_await runtime.await_async(args[0],span));values->push_back(co_await runtime.await_async(args[1],span));result=Value(SliceValue{values,0,2,true,type});}
            else{
                auto done=[args,span]{for(const auto& v:args){auto s=task_state(v,span);std::lock_guard lock(s->mutex);if(s->finished)return true;}return false;};
                while(!done()){task_checkpoint(&child,span);co_await Suspend{[&child,done]{return done()||(child.cancellation&&child.cancellation->stopped());}};}
                std::size_t winner=0;{auto s=task_state(args[0],span);std::lock_guard lock(s->mutex);if(!s->finished)winner=1;}
                task_state(args[1-winner],span)->cancellation->requested=true;notify_task_scheduler();
                result=co_await runtime.await_async(args[winner],span);
            }
        }catch(...){error=std::current_exception();}
        child.cleanup=true;
        if(error||op=="race")for(const auto& v:args)task_state(v,span)->cancellation->requested=true;
        for(const auto& v:args)try{co_await runtime.await_async(v,span);}catch(...){if(!error&&op=="all")error=std::current_exception();}
        child.tasks=nullptr;if(error)std::rethrow_exception(error);co_return result;
    };
    return c->tasks->start_resumable(std::move(job),op=="all"?"[]"+type:type,span);
}
Routine<Value> invoke_resumable(std::string name,std::vector<Value> args,SourceSpan span,std::ostream& out,std::vector<bool> contextual,std::string inferred,RuntimeContext* c){
    if(name=="task_await")co_return co_await c->tasks->await_async(args.at(0),span);
    if(name=="task_group_end"){co_await c->tasks->end_group_async(static_cast<std::size_t>(as_int(args.at(0),span)),span);co_return Value{};}
    auto f=standard_function(name);
    if(f&&f->module=="task"){
        if(f->name=="timeout")co_return co_await c->tasks->await_async(args.at(0),span,duration(args.at(1),span));
        if(f->name=="yield"){task_checkpoint(c,span);co_await Yield{true};task_checkpoint(c,span);co_return Value{};}
        if(f->name=="sleep"){
            auto until=Clock::now()+duration(args.at(0),span);task_checkpoint(c,span);
            co_await Suspend{[c]{return !c->cleanup&&c->cancellation&&c->cancellation->stopped();},until};
            task_checkpoint(c,span);co_return Value{};
        }
    }
    if(c&&c->in_task&&f&&f->module=="gc"&&f->name=="collect"){
        auto result=invoke_builtin(name,args,span,out,contextual,inferred,c);
        if(c->files&&c->files->usage().used[std::size_t(resources::Metric::handles)])
            co_await Blocking([c]{file_checkpoint(c);return Value{};});
        co_return result;
    }
    // Blocking adapters never call Hua/user code. Their heap remains exclusively owned while parked.
    if(c&&c->in_task&&f&&(f->module=="file"||f->module=="fs"||f->module=="io"||f->module=="process")){
        auto heap=c->heap;co_return co_await Blocking([=,&out]{HeapScope scope(*heap);TypeRelations empty;TypeRelationScope relations(c->type_relations?*c->type_relations:empty);task_checkpoint(c,span);file_checkpoint(c);auto result=invoke_builtin(name,args,span,out,contextual,inferred,c);file_checkpoint(c);return result;});
    }
    co_return invoke_builtin(name,args,span,out,contextual,inferred,c);
}
}
