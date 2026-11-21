#include "hua/resume.hpp"
#include "hua/tasks.hpp"
#include "hua/stdlib.hpp"
#include "hua/file.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <bit>
#include <unordered_map>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace hua {
struct BlockingState {CompletionEvent done;Value value;std::exception_ptr error;};
namespace {
struct Frame {
    RuntimeContext context;std::shared_ptr<TaskState> state;TaskResumeJob job;
    std::ostringstream output;std::optional<Routine<Value>> root;
    std::coroutine_handle<> next;std::function<bool()> ready;Clock::time_point deadline=Clock::time_point::max();
    unsigned ticks{};
    const CompletionEvent* completion{};
    bool yielded{},poll_completion{};
    std::optional<unsigned> worker;
};
thread_local Frame* active_frame=nullptr;
struct Active {Frame* previous;explicit Active(Frame* f):previous(active_frame){active_frame=f;}~Active(){active_frame=previous;}};
unsigned workers(){
    auto available=std::max(std::thread::hardware_concurrency(),1u);
#ifdef _WIN32
    DWORD_PTR allowed{},system{};
    if(GetProcessAffinityMask(GetCurrentProcess(),&allowed,&system)&&allowed)available=std::min(available,static_cast<unsigned>(std::popcount(allowed)));
#elif defined(__linux__)
    cpu_set_t allowed;CPU_ZERO(&allowed);
    if(sched_getaffinity(0,sizeof(allowed),&allowed)==0&&CPU_COUNT(&allowed)>0)available=std::min(available,static_cast<unsigned>(CPU_COUNT(&allowed)));
#endif
    auto n=std::clamp(available,1u,8u);
    if(auto p=std::getenv("HUA_TASK_WORKERS")){unsigned v{};auto end=p+std::char_traits<char>::length(p);auto r=std::from_chars(p,end,v);if(r.ec!=std::errc{}||r.ptr!=end||v<1||v>n)throw std::runtime_error("HUA_TASK_WORKERS must be 1..min(8, available CPUs)");n=v;}
    return n;
}
Routine<Value> execute_frame(Frame* f){
    Value value;std::exception_ptr error;
    try{task_checkpoint(&f->context,{});value=co_await f->job(f->context,f->output);task_checkpoint(&f->context,{});if(contains_file(value))runtime_error({},"task results cannot contain File","E4104");}
    catch(...){error=std::current_exception();}
    try{if(f->context.files->usage().used[std::size_t(resources::Metric::handles)])co_await Blocking([f]{finish_file_scope(&f->context);return Value{};});else finish_file_scope(&f->context);}catch(...){if(!error)error=std::current_exception();}
    if(error)std::rethrow_exception(error);
    co_return value;
}
struct Scheduler {
    std::mutex mutex;std::condition_variable changed;bool stopping{};
    unsigned count;
    std::array<std::condition_variable,8> worker_changed;
    std::vector<std::shared_ptr<Frame>> ready,waiting;
    std::unordered_map<const CompletionEvent*,std::shared_ptr<Frame>> completions;
    unsigned interruptible_completions{};
    std::vector<std::jthread> threads;

    void wake_all(){
        changed.notify_all();
        for(unsigned i=0;i<count;++i)worker_changed[i].notify_one();
    }
    void make_ready(std::shared_ptr<Frame> frame){
        const auto owner=frame->worker;
        ready.push_back(std::move(frame));
        if(owner)worker_changed[*owner].notify_one();
        else for(unsigned i=0;i<count;++i)worker_changed[i].notify_one();
    }
    Scheduler():count(workers()){
        ready.reserve(64);waiting.reserve(64);completions.reserve(64);threads.reserve(count);
        try{for(unsigned i=0;i<count;++i)threads.emplace_back([this,i]{worker(i);});}
        catch(...){std::lock_guard lock(mutex);stopping=true;wake_all();throw;}
    }
    ~Scheduler(){{std::lock_guard lock(mutex);stopping=true;}wake_all();}
    void worker(unsigned index){
        for(;;){
            std::shared_ptr<Frame> f;
            {
                std::unique_lock lock(mutex);
                for(;;){
                    for(std::size_t i=0;i<waiting.size();){
                        auto& w=waiting[i];
                        if(w->ready()||Clock::now()>=w->deadline){
                            make_ready(std::move(w));waiting.erase(waiting.begin()+i);
                        }else ++i;
                    }
                    if(interruptible_completions){
                        for(auto item=completions.begin();item!=completions.end();){
                            auto& frame=item->second;
                            if(frame->poll_completion&&frame->ready()){
                                --interruptible_completions;
                                make_ready(std::move(frame));
                                item=completions.erase(item);
                            }else ++item;
                        }
                    }
                    auto next=std::find_if(ready.begin(),ready.end(),[&](const auto& frame){
                        return !frame->worker || *frame->worker==index;
                    });
                    if(next!=ready.end()){
                        f=std::move(*next);ready.erase(next);
                        if(f->context.task_worker_affine)f->worker=index;
                        break;
                    }
                    if(stopping&&waiting.empty()&&ready.empty()&&completions.empty())return;
                    // Cancellation ancestors can change without a direct dependency notification.
                    if(waiting.empty()&&!interruptible_completions)worker_changed[index].wait(lock);
                    else worker_changed[index].wait_for(lock,std::chrono::milliseconds(1));
                }
            }
            {
                Active active(f.get());HeapScope heap(*f->context.heap);
                TypeRelations empty;TypeRelationScope relations(f->context.type_relations?*f->context.type_relations:empty);
                f->next.resume();
            }
            if(f->root->handle.done()){
                Value result;std::exception_ptr error;
                try{result=f->root->take();}catch(...){error=std::current_exception();}
                // Destroy every coroutine/local/defer owner before publishing a terminal Task.
                f->root.reset();f->job={};
                auto state=f->state;std::string text;
                try{text=f->output.str();}catch(...){if(!error)error=std::current_exception();}
                {std::lock_guard lock(state->mutex);state->result=error?Value{}:std::move(result);state->error=error;
                    if(error)try{std::rethrow_exception(error);}catch(const Diagnostic& d){state->canceled_failure=d.code()=="E4101";}catch(...){}
                    state->output=std::move(text);state->finished=true;}
                release_task_admission();state->changed.notify_all();wake_all();
            }else{
                std::lock_guard lock(mutex);
                if(f->yielded){
                    f->yielded=false;
                    make_ready(std::move(f));
                }else if(const auto* completion=f->completion){
                    f->completion=nullptr;
                    if(completion->await_ready()||(f->poll_completion&&f->ready())){
                        make_ready(std::move(f));
                    }else{
                        const bool interruptible=f->poll_completion;
                        if(!completions.emplace(completion,std::move(f)).second)std::terminate();
                        if(interruptible)++interruptible_completions;
                    }
                }else{
                    if(!f->ready)std::terminate(); // Every external suspension registers its continuation.
                    waiting.push_back(std::move(f));
                    wake_all();
                }
            }
        }
    }
};
Scheduler& scheduler(){static Scheduler instance;return instance;}
struct BlockPool {
    std::mutex mutex;std::condition_variable changed;bool stopping{};
    struct Job {std::shared_ptr<BlockingState> state;std::function<Value()> function;};
    std::array<Job,64> queue;std::size_t head{},size{},admitted{};
    std::vector<std::jthread> threads;
    BlockPool(){threads.reserve(8);try{for(unsigned i=0;i<8;++i)threads.emplace_back([this]{
        for(;;){Job job;{
            std::unique_lock lock(mutex);changed.wait(lock,[&]{return stopping||size;});if(!size&&stopping)return;
            job=std::move(queue[head]);head=(head+1)%queue.size();--size;
        }
        try{job.state->value=job.function();}catch(...){job.state->error=std::current_exception();}
        job.function={};{std::lock_guard lock(mutex);--admitted;}
        job.state->done.signal();
        }
    });}catch(...){std::lock_guard lock(mutex);stopping=true;changed.notify_all();throw;}}
    ~BlockPool(){{std::lock_guard lock(mutex);stopping=true;}changed.notify_all();}
    void submit(Job job){
        std::lock_guard lock(mutex);if(admitted>=64)runtime_error({},"blocking operation limit exceeded (64)","E4099");
        queue[(head+size)%queue.size()]=std::move(job);++size;++admitted;changed.notify_one();
    }
};
BlockPool& blockers(){static BlockPool instance;return instance;}
Routine<Value> lift(Routine<void> r){co_await std::move(r);co_return Value{};}
}
void enqueue_task(const std::shared_ptr<TaskState>& state,TaskResumeJob job,RuntimeContext child){
    auto& pool=scheduler();
    auto f=std::make_shared<Frame>();f->state=state;f->job=std::move(job);
    f->context=std::move(child);f->context.heap=state->heap;
    f->root.emplace(execute_frame(f.get()));f->next=f->root->handle;
    std::lock_guard lock(pool.mutex);
    pool.make_ready(std::move(f));
}
void Suspend::await_suspend(std::coroutine_handle<> h){
    if(!active_frame)throw std::logic_error("suspension outside Hua execution frame");
    active_frame->next=h;
    active_frame->ready=std::move(ready);
    active_frame->deadline=deadline;
    active_frame->completion=completion;
    active_frame->poll_completion=poll_completion;
}
void CompletionEvent::await_suspend(std::coroutine_handle<> handle) const {
    Suspend{[this]{return await_ready();}, Clock::time_point::max(), this}.await_suspend(handle);
}
void CompletionEvent::signal() noexcept {
    auto& pool=scheduler();
    std::lock_guard lock(pool.mutex);
    const auto found=pool.completions.find(this);
    completed.store(true,std::memory_order_release);
    if(found!=pool.completions.end()){
        auto frame=std::move(found->second);
        if(frame->poll_completion)--pool.interruptible_completions;
        pool.completions.erase(found);
        pool.make_ready(std::move(frame));
    }else{
        pool.changed.notify_all(); // Root wait or completion before registration.
    }
    if(pool.stopping)pool.wake_all();
}
bool Yield::await_ready()const{if(force)return false;if(!active_frame)return true;auto before=active_frame->ticks;active_frame->ticks+=weight;return before/1024==active_frame->ticks/1024;}
void Yield::await_suspend(std::coroutine_handle<> h){
    Suspend{[]{return true;},Clock::now()}.await_suspend(h);
    active_frame->yielded=true;
}
Blocking::Blocking(std::function<Value()> f):job(std::move(f)),state(std::make_shared<BlockingState>()){}
void Blocking::await_suspend(std::coroutine_handle<> h){
    auto state_copy=state;
    // Registration occurs before submission; an immediate completion cannot be lost.
    state_copy->done.await_suspend(h);
    blockers().submit({state,std::move(job)});
}
Value Blocking::await_resume(){if(state->error)std::rethrow_exception(state->error);return std::move(state->value);}
unsigned task_worker_count(){return scheduler().count;}
void notify_task_scheduler()noexcept{scheduler().wake_all();}
Value sync_wait(Routine<Value> routine,RuntimeContext& context){
    Frame frame;frame.context=context;frame.root.emplace(std::move(routine));frame.next=frame.root->handle;
    for(;;){
        {Active active(&frame);HeapScope heap(*context.heap);TypeRelations empty;TypeRelationScope relation(context.type_relations?*context.type_relations:empty);frame.next.resume();}
        if(frame.root->handle.done())return frame.root->take();
        if(!frame.ready)throw std::logic_error("unregistered root suspension");
        auto& pool=scheduler();
        std::unique_lock lock(pool.mutex);
        while(!frame.ready()&&Clock::now()<frame.deadline)
            pool.changed.wait_until(lock,std::min(frame.deadline,Clock::now()+std::chrono::milliseconds(1)));
    }
}
void sync_wait(Routine<void> routine,RuntimeContext& context){(void)sync_wait(lift(std::move(routine)),context);}
}
