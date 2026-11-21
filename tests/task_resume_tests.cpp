#include "hua/tasks.hpp"
#include "hua/file.hpp"
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
using namespace hua;using namespace std::chrono_literals;
namespace {
unsigned checks{},cases{};
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
Value number(std::int64_t n){return Value(n);}
Value unwrap(const Value& value){auto& r=cget<ResultValue>(value.data);if(!r.ok)throw std::runtime_error(show(value));return r.payload?*r.payload:Value{};}
Routine<Value> nested_wait(TaskRuntime& runtime,Value value){co_await Yield{true};co_return co_await runtime.await_async(value,{});}
void nested_and_races(){
    ++cases;RuntimeContext context;std::ostringstream out;TaskRuntime runtime(context,out);context.tasks=&runtime;
    std::atomic<unsigned> calls{};
    TaskResumeJob parent=[&](RuntimeContext& c,std::ostream& o)->Routine<Value>{
        auto* heap=&managed_heap();check(heap==c.heap.get(),"entry heap");
        auto roots=managed<std::vector<Value>>();roots->push_back(Value(std::string("root")));
        TaskRuntime local(c,o);c.tasks=&local;
        TaskResumeJob child=[&](RuntimeContext&,std::ostream& output)->Routine<Value>{
            ++calls;output<<"child\n";co_await Yield{true};co_return number(9);
        };
        auto t=local.start_resumable(std::move(child),"int",{});
        auto v=co_await nested_wait(local,t);
        check(&managed_heap()==heap,"heap rebound after ordinary nested await");managed_heap().collect();
        check(show(roots->at(0))=="root","suspended GC root retained");
        check(as_int(co_await local.await_async(t,{}),{})==9,"repeat await");
        co_await local.finish_async();c.tasks=nullptr;co_return v;
    };
    auto t=runtime.start_resumable(parent,"int",{});check(as_int(runtime.await(t,{}),{})==9,"single-worker nested result");runtime.finish();
    check(calls==1&&out.str()=="child\n","no replay, one stdout delivery");
    for(unsigned i=0;i<200;++i){
        TaskResumeJob instant=[](RuntimeContext&,std::ostream&)->Routine<Value>{co_await Yield{true};co_return number(3);};
        auto value=runtime.start_resumable(std::move(instant),"int",{});
        check(as_int(runtime.await(value,{}),{})==3,"completion/registration race");
        check(as_int(runtime.await(value,{}),{})==3,"already-completed await");runtime.finish();
    }
}
void discard_task_results() {
    ++cases;
    RuntimeContext context;
    std::ostringstream output;
    TaskRuntime runtime(context, output);
    context.tasks = &runtime;
    TaskResumeJob payload = [](RuntimeContext&, std::ostream& out) -> Routine<Value> {
        auto items = managed<std::vector<Value>>();
        for (int i = 0; i < 8; ++i) {
            auto list = managed<ListData>();
            list->element_type = "int";
            list->values.push_back(number(i));
            items->push_back(Value(ListValue{std::move(list), true}));
        }
        out << "child\\n";
        co_return Value(SliceValue{items, 0, items->size(), true, "List<int>"});
    };
    auto start = [&] { return runtime.start_resumable(payload, "[]List<int>", {}); };
    auto group = runtime.begin_group();
    auto retained = start();
    auto before = context.heap->allocated();
    runtime.end_group(group, {});
    check(context.heap->allocated() == before, "taskgroup makes no payload allocations");

    auto first = runtime.await(retained, {});
    auto& first_list = hua::get<ListValue>(cget<SliceValue>(first.data).storage->at(0).data);
    first_list.data->values[0] = number(99);
    auto second = runtime.await(retained, {});
    auto& second_list = cget<ListValue>(cget<SliceValue>(second.data).storage->at(0).data);
    check(as_int(second_list.data->values[0], {}) == 0, "await after join still delivers isolated copies");
    check(output.str() == "child\\n", "taskgroup and repeated await emit once");

    start();
    before = context.heap->allocated();
    runtime.finish();
    check(context.heap->allocated() == before, "scope finish makes no payload allocations");
    check(output.str() == "child\\nchild\\n", "scope finish delivers unobserved output");

    // Complete both children before combining. race must discard a successful loser.
    for (std::string_view op : {"all", "race"}) {
        std::vector<Value> args{start(), start()};
        for (const auto& arg : args) {
            auto state = cget<TaskValue>(arg.data).data;
            std::unique_lock lock(state->mutex);
            check(state->changed.wait_for(lock, 5s, [&] { return state->finished; }),
                  "both tasks completed before combination");
        }
        auto combined = task_combine(op, args, {}, &context);
        auto value = runtime.await(combined, {});
        const auto state = cget<TaskValue>(combined.data).data;
        const auto length = cget<SliceValue>(value.data).length;
        check(length == (op == "all" ? 2 : 8), "combined result correctly delivered");
        // One Slice backing plus eight Lists per retained result, and all's outer Slice.
        check(state->heap->allocated() == (op == "all" ? 19 : 9),
              "only retained results allocate in combinator heap");
        runtime.finish();
    }

    // Values outside the proven subset still go through the previous diagnostics.
    auto invalid_group = runtime.begin_group();
    runtime.start_resumable([](RuntimeContext&, std::ostream&) -> Routine<Value> {
        co_return Value(VoidValue{});
    }, "void", {});
    bool rejected = false;
    try { runtime.end_group(invalid_group, {}); }
    catch (const Diagnostic& diagnostic) { rejected = diagnostic.code() == "E4003"; }
    check(rejected, "an uncopyable result still fails at implicit join");
}
void completion_delivery() {
    ++cases;
    RuntimeContext context;
    std::ostringstream output;
    TaskRuntime runtime(context, output);
    CompletionEvent early;
    early.signal();
    CompletionEvent pending;
    std::atomic<unsigned> resumed{};
    auto waiter = runtime.start_resumable([&](RuntimeContext&, std::ostream&) -> Routine<Value> {
        co_await early;
        co_await pending;
        ++resumed;
        co_return number(42);
    }, "int", {});
    // With one worker, running this producer proves the waiter released its worker.
    auto producer = runtime.start_resumable([&](RuntimeContext&, std::ostream&) -> Routine<Value> {
        pending.signal();
        co_return number(1);
    }, "int", {});
    check(as_int(unwrap(runtime.await(producer, {}, 5s)), {}) == 1, "completion producer finished");
    check(as_int(unwrap(runtime.await(waiter, {}, 5s)), {}) == 42 && resumed == 1,
          "completion before and after suspension resumes once");
    runtime.finish();
}
void limits(){
    ++cases;RuntimeContext c;std::ostringstream out;TaskRuntime runtime(c,out);c.tasks=&runtime;
    TaskResumeJob sleeping=[](RuntimeContext& child,std::ostream&)->Routine<Value>{
        co_await Suspend{[&child]{return child.cancellation->stopped();},Clock::now()+5s};
        task_checkpoint(&child,{});co_return number(1);
    };
    for(unsigned i=0;i<64;++i)runtime.start_resumable(sleeping,"int",{});
    bool bound=false;try{runtime.start_resumable(sleeping,"int",{});}catch(const Diagnostic& d){bound=d.code()=="E4099";}
    check(bound,"64 live Task admission preserved");runtime.finish(true);
    TaskResumeJob immediate=[](RuntimeContext&,std::ostream&)->Routine<Value>{co_return number(1);};
    for(unsigned i=0;i<4096;++i){auto t=runtime.start_resumable(immediate,"int",{});runtime.await(t,{});}
    bound=false;try{runtime.start_resumable(immediate,"int",{});}catch(const Diagnostic& d){bound=d.code()=="E4099";}
    check(bound,"4096 cumulative scope creations preserved");runtime.finish();
}
struct Stats {std::atomic<bool> entered{},release{};std::atomic<unsigned> reads{},closes{};std::atomic<bool> other_progress{};};
struct Backend:FileBackend {
    std::shared_ptr<Stats> stats;explicit Backend(std::shared_ptr<Stats> s):stats(std::move(s)){}
    FileHostIO read(std::span<char> data)override{
        ++stats->reads;stats->entered=true;auto until=Clock::now()+5s;
        while(!stats->release&&Clock::now()<until)std::this_thread::sleep_for(1ms);
        if(!stats->release)throw std::runtime_error("test release deadline");
        data[0]='a';data[1]='b';return {2,{}};
    }
    FileHostIO write(std::span<const char> data)override{return {static_cast<std::int64_t>(data.size()),{}};}
    FileHostPosition position()override{return {0,{}};}FileHostPosition size()override{return {2,{}};}
    FileHostFailure seek(std::int64_t)override{return {};}FileHostFailure flush()override{return {};}
    FileHostClose close()override{++stats->closes;return {{},true};}
};
void blocking_file_and_timeout(){
    ++cases;RuntimeContext context;std::ostringstream out;TaskRuntime runtime(context,out);context.tasks=&runtime;
    auto stats=std::make_shared<Stats>();Value retained;std::shared_ptr<FileScope> scope;
    TaskResumeJob job=[&](RuntimeContext& c,std::ostream& o)->Routine<Value>{
        scope=c.files;retained=unwrap(c.files->adopt(std::make_unique<Backend>(stats),"r"));
        auto result=co_await invoke_resumable("$std$file$read",{retained,number(8)},{},o,{},"",&c);
        co_return unwrap(result);
    };
    auto task=runtime.start_resumable(job,"Bytes?",{});
    auto until=Clock::now()+5s;while(!stats->entered&&Clock::now()<until)std::this_thread::sleep_for(1ms);
    check(stats->entered,"native I/O admitted");
    TaskResumeJob progress=[stats](RuntimeContext&,std::ostream&)->Routine<Value>{stats->other_progress=true;co_return number(1);};
    auto other=runtime.start_resumable(std::move(progress),"int",{});
    check(as_int(runtime.await(other,{}),{})==1&&stats->other_progress,"blocking I/O released computation worker");
    check(scope->usage().used[std::size_t(resources::Metric::operations)]==1,"inflight operation retained");
    std::jthread release([stats]{std::this_thread::sleep_for(80ms);stats->release=true;});
    auto begin=Clock::now();auto timed=runtime.await(task,{},1ms);
    check(show(timed)=="err(timeout)"&&Clock::now()-begin>=60ms,"soft timeout waits native retirement");
    check(stats->reads==1&&stats->closes==1,"no replay and exactly one physical close");
    check(hua::get<FileValue>(retained.data).data->handle.snapshot().state==resources::Lifecycle::closed,"terminal after File close");
    auto usage=scope->usage();for(auto n:usage.used)check(n==0,"resource quota returned after cancel");
    runtime.finish();
}
#ifdef _WIN32
void iocp_cancel_inflight() {
    ++cases;
    struct OwnedHandle {
        HANDLE value=INVALID_HANDLE_VALUE;
        ~OwnedHandle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
        HANDLE release(){return std::exchange(value,INVALID_HANDLE_VALUE);}
    };
    const auto name=std::wstring(LR"(\\.\pipe\hua-n2-cancel-)")+std::to_wstring(GetCurrentProcessId());
    OwnedHandle server{CreateNamedPipeW(name.c_str(),PIPE_ACCESS_INBOUND|FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT,1,4096,4096,0,nullptr)};
    check(server.value!=INVALID_HANDLE_VALUE,"create overlapped pipe for IOCP fixture");
    OwnedHandle client{CreateFileW(name.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr)};
    check(client.value!=INVALID_HANDLE_VALUE,"open pipe writer without sending data");

    RuntimeContext context;
    std::ostringstream out;
    TaskRuntime runtime(context,out);
    context.tasks=&runtime;
    auto probe=std::make_shared<FileCompletionProbe>();
    std::atomic<unsigned> submitted{},cancel_calls{},packets{},retired{};
    std::atomic<bool> cancel_accepted{},release_packet{},packet_gate_timeout{};
    std::atomic<unsigned long> packet_status{};
    probe->pending=[&]{++submitted;};
    probe->canceled=[&](bool accepted){cancel_accepted=accepted;++cancel_calls;};
    probe->packet=[&](unsigned long error){
        packet_status=error;
        ++packets;
        const auto deadline=Clock::now()+5s;
        while(!release_packet && Clock::now()<deadline)std::this_thread::sleep_for(1ms);
        if(!release_packet)packet_gate_timeout=true;
    };
    probe->retired=[&]{++retired;};
    std::shared_ptr<FileScope> scope;
    std::shared_ptr<FileData> retained;
    auto child=runtime.start_resumable([&](RuntimeContext& c,std::ostream& output)->Routine<Value>{
        scope=c.files;
        auto file=unwrap(adopt_completion_file_for_test(*scope,server.release(),probe));
        retained=cget<FileValue>(file.data).data;
        co_return co_await invoke_resumable("$std$file$read",{file,number(32)},{},output,{},"",&c);
    },"Result<Bytes?,std.file.IOFailure>",{});
    const auto deadline=Clock::now()+5s;
    while(submitted==0 && Clock::now()<deadline)std::this_thread::sleep_for(1ms);
    const bool was_pending=submitted==1;
    const bool requested=was_pending && as_bool(task_standard("cancel",{child},{},&context),{});
    while(requested && (cancel_calls==0 || packets==0) && Clock::now()<deadline)
        std::this_thread::sleep_for(1ms);
    bool was_retained=false;
    if(requested && packets==1 && cancel_calls==1){
        const auto quota=scope->usage().used;
        was_retained=!as_bool(task_standard("done",{child},{},&context),{}) && retired==0 &&
            quota[std::size_t(resources::Metric::handles)]==1 &&
            quota[std::size_t(resources::Metric::operations)]==1 &&
            quota[std::size_t(resources::Metric::inflight_bytes)]==32 &&
            retained->handle.snapshot().state==resources::Lifecycle::open;
    }
    release_packet=true;
    if(packets==0)CloseHandle(client.release()); // unblock even a failed fixture setup
    auto observed=runtime.await(child,{},5s);
    check(was_pending && requested && cancel_accepted,"CancelIoEx accepted an actual pending read");
    check(was_retained,"File and buffers retained while IOCP packet publication is held");
    check(packet_status==ERROR_OPERATION_ABORTED && !packet_gate_timeout,"real canceled kernel completion delivered");
    check(!cget<ResultValue>(observed.data).ok && show(observed).find("E4101")!=std::string::npos,"canceled Task observed");
    check(submitted==1 && cancel_calls==1 && packets==1 && retired==1,"one submit, cancel, completion and retirement");
    check(retained->handle.snapshot().state==resources::Lifecycle::closed,"File closed after completion");
    for(auto n:scope->usage().used)check(n==0,"all IOCP operation quotas returned");
    runtime.finish();
}
#endif
void migration(){
    if(task_worker_count()<2)return;
    ++cases;RuntimeContext context;std::ostringstream out;TaskRuntime runtime(context,out);context.tasks=&runtime;
    TypeRelations relations{{"Contract",{"Concrete"}}};context.type_relations=&relations;
    std::atomic<bool> started{},go{},blocked{},release{};std::thread::id first;
    TaskResumeJob migrating=[&](RuntimeContext& c,std::ostream&)->Routine<Value>{
        auto* heap=&managed_heap();first=std::this_thread::get_id();
        auto object=managed<StructData>();object->name="Concrete";auto value=Value(StructValue{object,true});
        enforce_type(value,"Contract",{});started=true;
        co_await Suspend{[&]{return go.load();}};
        check(std::this_thread::get_id()!=first,"continuation migrated to another worker");
        check(&managed_heap()==heap&&heap==c.heap.get(),"migrated heap binding");
        enforce_type(value,"Contract",{});managed_heap().collect();check(object->name=="Concrete","migrated type relation and roots");
        release=true;co_return number(7);
    };
    auto t=runtime.start_resumable(migrating,"int",{});auto until=Clock::now()+5s;
    while(!started&&Clock::now()<until)std::this_thread::sleep_for(1ms);check(started,"migration start");
    for(unsigned i=0;i<32;++i){
        TaskResumeJob gate=[&](RuntimeContext&,std::ostream&)->Routine<Value>{
            if(std::this_thread::get_id()==first&&!blocked.exchange(true)){
                auto deadline=Clock::now()+5s;while(!release&&Clock::now()<deadline)std::this_thread::yield();
            }co_return number(0);
        };
        runtime.start_resumable(std::move(gate),"int",{});
    }
    while(!blocked&&Clock::now()<until)std::this_thread::sleep_for(1ms);
    check(blocked,"original worker occupied");go=true;notify_task_scheduler();
    auto result=runtime.await(t,{},5000ms);release=true;
    check(show(result)=="ok(7)","cross-worker continuation result");runtime.finish();
}
}
int main(){try{
    nested_and_races();discard_task_results();completion_delivery();limits();blocking_file_and_timeout();
#ifdef _WIN32
    iocp_cancel_inflight();
#endif
    migration();
    std::cout<<"D002-c continuation tests: "<<cases<<" cases, "<<checks<<" checks, "<<task_worker_count()<<" workers\n";return 0;
}catch(const std::exception& e){
    if(auto p=std::getenv("HUA_TASK_WORKERS");p&&std::string_view(p)=="2"&&std::string_view(e.what()).find("HUA_TASK_WORKERS")!=std::string_view::npos){
        std::cerr<<"SKIP: migration requires two available CPUs\n";return 77;
    }
    std::cerr<<"D002-c: "<<e.what()<<"\n";return 1;
}}
