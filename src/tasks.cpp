#include "hua/tasks.hpp"
#include "hua/stdlib.hpp"
#include <exception>
#include <algorithm>
namespace hua {
namespace {
std::atomic<unsigned> running{0};
std::shared_ptr<TaskState> task_state(const Value& v,const SourceSpan& s){auto t=std::get_if<TaskValue>(&v.data);if(!t||!t->data)runtime_error(s,"expected Task<T>","E4003");return t->data;}
std::chrono::milliseconds duration(const Value& v,const SourceSpan& s){auto ms=as_int(v,s);if(ms<0||ms>86400000)runtime_error(s,"duration must be 0..86400000 milliseconds","E4103");return std::chrono::milliseconds(ms);}
std::string failure(std::exception_ptr error){try{std::rethrow_exception(error);}catch(const Diagnostic& e){return e.code()+": "+e.what();}catch(const std::exception& e){return e.what();}catch(...){return "task failed";}}
bool observed(const std::shared_ptr<TaskState>& state){std::lock_guard lock(state->mutex);return state->observed;}
void join(const std::shared_ptr<TaskState>& state){std::lock_guard lock(state->join_mutex);if(state->worker.joinable())state->worker.join();}
}
TaskState::~TaskState(){if(worker.joinable()){cancellation->requested=true;worker.join();}result={};if(heap)heap->collect();}
TaskRuntime::~TaskRuntime(){try{finish(true);}catch(...) {}}
void task_checkpoint(RuntimeContext* c,const SourceSpan& s){if(c&&!c->cleanup&&c->cancellation&&c->cancellation->stopped())runtime_error(s,"task canceled","E4101");}
Value TaskRuntime::start(TaskJob job,std::string type,const SourceSpan& span,std::shared_ptr<ManagedHeap> heap){
    task_checkpoint(&context,span);
    if(children.size()>=4096)runtime_error(span,"task creation limit exceeded (4096 per scope)","E4099");
    auto old=running.fetch_add(1);if(old>=64){--running;runtime_error(span,"concurrent task limit exceeded (64)","E4099");}
    auto state=std::make_shared<TaskState>();state->heap=heap?std::move(heap):std::make_shared<ManagedHeap>();state->result_type=std::move(type);state->cancellation=std::make_shared<Cancellation>();state->cancellation->parent=context.cancellation;
    auto child=context;child.tasks=nullptr;child.cancellation=state->cancellation;child.in_task=true;child.cleanup=false;child.input=nullptr;
    children.push_back(state);
    try{state->worker=std::jthread([state,job=std::move(job),child=std::move(child)]()mutable{
        std::ostringstream out;child.heap=state->heap;HeapScope heap_scope(*state->heap);
        Value result;std::exception_ptr error;
        try{task_checkpoint(&child,{});result=job(child,out);task_checkpoint(&child,{});}catch(...){error=std::current_exception();}
        {std::lock_guard lock(state->mutex);state->result=std::move(result);state->error=error;if(error)try{std::rethrow_exception(error);}catch(const Diagnostic& d){state->canceled_failure=d.code()=="E4101";}catch(...) {}state->output=out.str();state->finished=true;}
        --running;state->changed.notify_all();
    });}catch(...){--running;children.pop_back();throw;}
    return Value(TaskValue{state});
}
Value TaskRuntime::await(const Value& value,const SourceSpan& span,std::optional<std::chrono::milliseconds> timeout){
    auto state=task_state(value,span);auto deadline=timeout?std::chrono::steady_clock::now()+*timeout:std::chrono::steady_clock::time_point::max();
    bool timed_out=false,canceled=false;
    {std::unique_lock lock(state->mutex);while(!state->finished){
        if(!context.cleanup&&context.cancellation&&context.cancellation->stopped()){canceled=true;state->cancellation->requested=true;break;}
        if(std::chrono::steady_clock::now()>=deadline){timed_out=true;state->cancellation->requested=true;break;}
        state->changed.wait_for(lock,std::chrono::milliseconds(2));
    }}
    // Timeout cancels and joins: no background write continues after the caller regains control.
    join(state);{std::lock_guard lock(state->mutex);state->observed=true;
    if(!state->emitted){output<<state->output;state->emitted=true;}}
    if(canceled)runtime_error(span,"task canceled while awaiting a dependency","E4101");
    if(timed_out)return standard_result(Value(std::string("timeout")),false,state->result_type);
    if(state->error){if(timeout)return standard_result(Value(failure(state->error)),false,state->result_type);std::rethrow_exception(state->error);}
    auto result=copy_value(state->result,true);
    return timeout?standard_result(std::move(result),true,state->result_type):result;
}
void TaskRuntime::finish(bool cancel){
    if(cancel)for(auto& child:children)child->cancellation->requested=true;
    std::exception_ptr first;bool old=context.cleanup;context.cleanup=true;
    for(const auto& child:children)try{if(!observed(child))await(Value(TaskValue{child}),{});else join(child);}catch(...){if(!first)first=std::current_exception();for(auto& other:children)other->cancellation->requested=true;}
    context.cleanup=old;children.clear();groups.clear();if(first&&!cancel)std::rethrow_exception(first);
}
std::size_t TaskRuntime::begin_group(){auto id=next_group++;groups.emplace_back(id,children.size());return id;}
void TaskRuntime::end_group(std::size_t id,const SourceSpan& span){
    if(groups.empty()||groups.back().first!=id)runtime_error(span,"invalid taskgroup scope","E4103");
    auto start=groups.back().second;groups.pop_back();std::exception_ptr first;
    if(context.unwinding||(context.cancellation&&context.cancellation->stopped()))for(std::size_t i=start;i<children.size();++i)children[i]->cancellation->requested=true;
    for(std::size_t i=start;i<children.size();++i)try{if(!observed(children[i]))await(Value(TaskValue{children[i]}),span);}catch(...){if(!first)first=std::current_exception();for(std::size_t j=start;j<children.size();++j)children[j]->cancellation->requested=true;}
    if(first)std::rethrow_exception(first);
}
ExecutionScope::ExecutionScope(RuntimeContext*& pointer,std::ostream& out):target(pointer),previous(pointer),context(std::make_unique<RuntimeContext>(pointer?*pointer:RuntimeContext{})),heap(context->heap?context->heap:std::make_shared<ManagedHeap>()),heap_scope(*heap){runtime=std::make_unique<TaskRuntime>(*context,out);context->heap=heap;context->tasks=runtime.get();target=context.get();}
ExecutionScope::~ExecutionScope(){runtime.reset();heap->collect();target=previous;}
void ExecutionScope::finish(){runtime->finish();}
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
    if(op=="cancel"){std::lock_guard lock(state->mutex);if(state->finished)return Value(false);state->cancellation->requested=true;state->changed.notify_all();return Value(true);}
    if(op=="done"){std::lock_guard lock(state->mutex);return Value(state->finished);}
    if(op=="state"){std::lock_guard lock(state->mutex);return Value(std::string(!state->finished?(state->cancellation->stopped()?"canceling":"running"):state->error?(state->canceled_failure?"canceled":"failed"):"completed"));}
    if(op=="timeout")return c->tasks->await(args[0],span,duration(args[1],span));
    runtime_error(span,"unknown task operation","E4103");
}
Value task_combine(std::string_view op,const std::vector<Value>& args,const SourceSpan& span,RuntimeContext* c){
    auto left=task_state(args[0],span),right=task_state(args[1],span);auto type=left->result_type;
    return c->tasks->start([args,op=std::string(op),span,type](RuntimeContext& child,std::ostream& out){
        TaskRuntime runtime(child,out);child.tasks=&runtime;
        struct Inputs {TaskRuntime& runtime;RuntimeContext& context;const std::vector<Value>& args;SourceSpan span;bool complete{};~Inputs(){if(complete)return;context.cleanup=true;for(const auto& v:args)task_state(v,span)->cancellation->requested=true;for(const auto& v:args)try{runtime.await(v,span);}catch(...) {}}} inputs{runtime,child,args,span};
        if(op=="all"){auto values=managed<std::vector<Value>>();values->push_back(runtime.await(args[0],span));values->push_back(runtime.await(args[1],span));inputs.complete=true;return Value(SliceValue{values,0,2,true,type});}
        std::size_t winner=0;for(;;){task_checkpoint(&child,span);bool found=false;for(std::size_t i=0;i<2;++i){auto state=task_state(args[i],span);std::lock_guard lock(state->mutex);if(state->finished){winner=i;found=true;break;}}if(found)break;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        auto loser=task_state(args[1-winner],span);loser->cancellation->requested=true;auto result=runtime.await(args[winner],span);try{runtime.await(args[1-winner],span);}catch(...){}inputs.complete=true;return result;
    },op=="all"?"[]"+type:type,span);
}
}
