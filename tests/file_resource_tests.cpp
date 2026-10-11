#include "hua/file.hpp"
#include "hua/tasks.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
using namespace hua;namespace R=hua::resources;
namespace {
std::size_t checks{},cases{};
void check(bool b,const char* msg){++checks;if(!b)throw std::runtime_error(msg);}
using I=std::int64_t;
Value integer(I n){return Value(n);}Value text(const char* s){return Value(std::string(s));}
Value bytes(const char* s){return Value(BytesValue{managed<const std::string>(s)});}
const ResultValue& outcome(const Value& v){return std::get<ResultValue>(v.data);}
Value unwrap(const Value& v){check(outcome(v).ok,"expected success");return outcome(v).payload?*outcome(v).payload:Value{};}
ErrorValue err(const Value& v){check(!outcome(v).ok,"expected error");return std::get<ErrorValue>(outcome(v).payload->data);}
IOFailureValue ioerr(const Value& v){check(!outcome(v).ok,"expected IOFailure");return std::get<IOFailureValue>(outcome(v).payload->data);}
Value call(RuntimeContext& c,const char* n,std::vector<Value> a){return invoke_file_standard(*standard_function("$std$file$"+std::string(n)),a,{},&c);}
struct Stats {
    std::atomic<unsigned> reads{},writes{},closes{},destroyed{};
    I read_count{2},write_count{2};FileHostFailure read_error,write_error,close_error;
    bool released{true},throws{},block{};std::atomic<bool> entered{},resume{};
};
struct Fake final:FileBackend {
    std::shared_ptr<Stats> stats;I cursor{};
    explicit Fake(std::shared_ptr<Stats> s):stats(std::move(s)){}
    ~Fake()override{++stats->destroyed;}
    FileHostIO read(std::span<char> b)override{
        ++stats->reads;stats->entered=true;
        while(stats->block&&!stats->resume)std::this_thread::yield();
        if(stats->throws)throw std::runtime_error("injected read exception");
        auto n=std::min<I>(stats->read_count,b.size());for(I i=0;i<n;++i)b[i]="abc"[i%3];cursor+=n;
        return {n,stats->read_error};
    }
    FileHostIO write(std::span<const char>)override{++stats->writes;return {stats->write_error&&stats->write_count<0?std::optional<I>{}:std::optional<I>{stats->write_count},stats->write_error};}
    FileHostPosition position()override{return {cursor,{}};}FileHostPosition size()override{return {10,{}};}
    FileHostFailure seek(I n)override{cursor=n;return {};}
    FileHostFailure flush()override{return {};}
    FileHostClose close()override{++stats->closes;return {stats->close_error,stats->released};}
};
struct Fixture {
    ManagedHeap heap;HeapScope heap_scope{heap};RuntimeContext context;
    std::shared_ptr<Stats> stats=std::make_shared<Stats>();Value file;
    Fixture(std::optional<R::Cost> caps={},const char* mode="r+"){
        context.task_id=next_file_task_id();context.files=std::make_shared<FileScope>(context.task_id,nullptr,caps);
        file=unwrap(context.files->adopt(std::make_unique<Fake>(stats),mode));
    }
    ~Fixture(){context.files->finish();}
};
I used(RuntimeContext& c,R::Metric m){return c.files->usage().used[std::size_t(m)];}
void clean(RuntimeContext& c,bool storage=true){
    auto s=c.files->usage();for(std::size_t i=0;i<s.used.size();++i){
        check(s.reserved[i]==s.used[i]+s.returned[i],"quota conservation");
        if(i!=std::size_t(R::Metric::handles)&&(storage||i!=std::size_t(R::Metric::storage_bytes)))check(s.used[i]==0,"operation quota leaked");
    }
}
R::Cost caps(){R::Cost c{};c.fill(1<<24);return c;}
void read_progress(){
    ++cases;Fixture f;auto r=unwrap(call(f.context,"read",{f.file,integer(5)}));
    check(*std::get<BytesValue>(r.data).data=="ab","short read bytes");
    check(used(f.context,R::Metric::storage_bytes)==5,"requested capacity retained");
    clean(f.context,false);call(f.context,"close",{f.file});check(f.stats->closes==1,"one close");
    check(used(f.context,R::Metric::storage_bytes)==5,"close cannot refund retained Bytes");
    r={};check(used(f.context,R::Metric::storage_bytes)==0,"last Bytes releases allocation");clean(f.context);
}
void partial_failure(){
    ++cases;Fixture f;f.stats->read_count=3;f.stats->read_error={"io","partial read","test",7};
    auto issue=ioerr(call(f.context,"read",{f.file,integer(8)}));
    check(issue.data->transferred==3,"known failure progress");check(*issue.data->partial->data=="abc","retained partial read");
    check(issue.data->error.data->domain=="file"&&issue.data->error.data->native_code==7,"structured error identity");
    auto copied=copy_value(Value(issue),true);call(f.context,"close",{f.file});issue={};
    check(used(f.context,R::Metric::storage_bytes)==8,"copied error retains partial allocation");
    copied={};clean(f.context);
}
void write_progress(){
    ++cases;Fixture f;f.stats->write_error={"no_space","disk full","test",8};
    auto issue=ioerr(call(f.context,"write",{f.file,bytes("abcd")}));
    check(issue.data->transferred==2&&!issue.data->partial,"write failure progress");check(f.stats->writes==1,"no retry");
    f.stats->write_count=-1;issue=ioerr(call(f.context,"write",{f.file,bytes("abcd")}));
    check(!issue.data->transferred,"unknown progress is not zero");clean(f.context);
}
void zero_and_modes(){
    ++cases;Fixture f;auto zero=unwrap(call(f.context,"read",{f.file,integer(0)}));
    check(std::get<BytesValue>(zero.data).data->empty()&&f.stats->reads==0,"zero read does not prove EOF");
    check(as_int(unwrap(call(f.context,"write",{f.file,bytes("")})),{})==0&&f.stats->writes==0,"zero write skips OS");
    call(f.context,"close",{f.file});check(ioerr(call(f.context,"read",{f.file,integer(0)})).data->error.data->code=="closed","zero read validates state");
    Fixture readonly({},"r");check(ioerr(call(readonly.context,"write",{readonly.file,bytes("")})).data->error.data->code=="permission_denied","zero write validates mode");
    check(err(call(readonly.context,"flush",{readonly.file})).data->code=="permission_denied","readonly flush");clean(readonly.context);
}
void quota_before_io(){
    ++cases;auto c=caps();c[std::size_t(R::Metric::storage_bytes)]=2;Fixture f(c);
    check(ioerr(call(f.context,"read",{f.file,integer(3)})).data->transferred==0,"quota rejection no progress");check(f.stats->reads==0,"quota rejects before read");clean(f.context);
    c=caps();c[std::size_t(R::Metric::operations)]=0;Fixture no_op(c);
    check(ioerr(call(no_op.context,"read",{no_op.file,integer(3)})).data->error.data->code=="limit","op quota");check(no_op.stats->reads==0,"no OS on op limit");clean(no_op.context);
    c=caps();c[std::size_t(R::Metric::handles)]=1;Fixture handles(c);
    check(err(handles.context.files->adopt(std::make_unique<Fake>(handles.stats),"r")).data->code=="limit","handle limit");check(used(handles.context,R::Metric::handles)==1,"one handle permit");clean(handles.context);
}
void known_close_failure(){
    ++cases;Fixture f;auto alias=copy_value(f.file);f.stats->close_error={"io","close failed after release","test",9};
    check(err(call(f.context,"close",{alias})).data->native_code==9,"first close failure surfaced");
    check(show(unwrap(call(f.context,"state",{f.file})))=="closed","aliases see closed");
    check(outcome(call(f.context,"close",{f.file})).ok&&f.stats->closes==1,"idempotent known release");
    check(std::get<ErrorValue>(unwrap(call(f.context,"close_error",{f.file})).data).data->native_code==9,"first failure retained");
    check(used(f.context,R::Metric::handles)==0,"known release refunds handle");
}
void uncertain_close(){
    ++cases;Fixture f;f.stats->released=false;f.stats->close_error={"io","release unknown","test",10};
    check(!outcome(call(f.context,"close",{f.file})).ok,"uncertain close fails");
    check(show(unwrap(call(f.context,"state",{f.file})))=="poisoned","poisoned state");
    check(err(call(f.context,"close",{f.file})).data->code=="poisoned","no unsafe retry");
    f.context.files->finish();check(f.stats->closes==1&&f.stats->destroyed==0,"backend retained after unknown release");
    check(used(f.context,R::Metric::handles)==1,"unknown release retains quota");
}
void owner_checks(){
    ++cases;Fixture f;RuntimeContext other;prepare_file_scope(other);
    check(err(call(other,"tell",{f.file})).data->code=="wrong_owner","logical owner enforced");
    other.task_id=f.context.task_id;check(err(call(other,"close",{f.file})).data->code=="wrong_owner","scope identity enforced");
    check(f.stats->closes==0,"wrong owner cannot close");
}
void gc_fallback(){
    ++cases;Fixture f;auto cycle=managed<StructData>();cycle->name="Cycle";
    cycle->fields.emplace("file",f.file);cycle->fields.emplace("self",Value(StructValue{cycle,true}));
    f.file={};cycle.reset();f.heap.collect();
    check(f.stats->closes==0,"GC performs no OS close");check(used(f.context,R::Metric::handles)==1,"fallback pending quota");
    file_checkpoint(&f.context);check(f.stats->closes==1,"host checkpoint drains fallback");check(used(f.context,R::Metric::handles)==0,"fallback refund");
}
void clone_boundaries(){
    ++cases;Fixture f;auto list=managed<ListData>();list->element_type="std.file.File";list->values.push_back(f.file);
    auto map=managed<MapData>();map->key_type="string";map->item_type="std.file.File";map->entries.emplace(std::string("x"),f.file);
    auto structure=managed<StructData>();structure->name="Box";structure->fields.emplace("x",f.file);
    auto storage=managed<std::vector<Value>>();storage->push_back(f.file);
    Callable closure;closure.resource_guard=[list](unsigned d){for(const auto& x:list->values)if(contains_file(x,d+1))return true;return false;};
    std::vector<Value> values{f.file,Value(ListValue{list,true}),Value(MapValue{map,true}),Value(StructValue{structure,true}),
        Value(SliceValue{storage,0,1,true,"std.file.File"}),make_multi({f.file}),result_payload(call(f.context,"state",{f.file}),true,{}),Value(closure)};
    values.erase(values.begin()+6); // state result is plain string, so independently verify it remains copyable below.
    values.push_back(Value(ResultValue{true,managed<Value>(f.file),"std.file.File","string",true,false}));
    for(const auto& v:values){bool denied=false;try{copy_value(v,true);}catch(const Diagnostic& d){denied=d.code()=="E4007";}check(denied,"nested File clone boundary");}
    check(show(copy_value(unwrap(call(f.context,"state",{f.file})),true))=="open","ordinary values remain cloneable");
}
void host_exception(){
    ++cases;Fixture f;f.stats->throws=true;auto issue=ioerr(call(f.context,"read",{f.file,integer(4)}));
    check(!issue.data->transferred&&!issue.data->partial,"exception progress unknown");clean(f.context);
    check(std::get<FileValue>(f.file.data).data->handle.snapshot().active==0,"exception retires host operation");
}
void seek_validation(){
    ++cases;Fixture f;check(as_int(unwrap(call(f.context,"seek",{f.file,integer(4),text("start")})),{})==4,"seek start");
    check(!outcome(call(f.context,"seek",{f.file,integer(-5),text("current")})).ok,"reject negative target");
    check(as_int(unwrap(call(f.context,"tell",{f.file})),{})==4,"invalid seek leaves position unchanged");
    check(!outcome(call(f.context,"seek",{f.file,integer(INT64_MAX),text("end")})).ok,"seek overflow");clean(f.context);
}
void close_inflight(){
    ++cases;Fixture f;f.stats->block=true;Value received;
    std::jthread worker([&]{HeapScope heap(f.heap);received=call(f.context,"read",{f.file,integer(4)});});
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!f.stats->entered&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    check(f.stats->entered,"blocking host entered");
    // Protocol close revokes admission while the synchronous host still owns its buffer.
    auto file=std::get<FileValue>(f.file.data).data;
    auto ticket=f.context.files->protocol.close(file->handle,f.context.task_id);check(bool(ticket),"close admitted");
    f.context.files->pump();check(f.stats->closes==0,"cannot release an active native call");
    check(file->handle.snapshot().state==R::Lifecycle::closing&&used(f.context,R::Metric::storage_bytes)==4,"in-flight storage retained");
    f.stats->resume=true;worker.join();f.context.files->pump();check(f.stats->closes==1&&ticket.value->observe()->released,"release follows native return");
    received={};clean(f.context);
}
void task_cleanup(){
    ++cases;RuntimeContext c;prepare_file_scope(c);std::ostringstream output;TaskRuntime runtime(c,output);
    auto stats=std::make_shared<Stats>();Value retained;R::TaskId child_id{};
    auto t=runtime.start([&](RuntimeContext& child,std::ostream&){
        child_id=child.task_id;retained=unwrap(child.files->adopt(std::make_unique<Fake>(stats),"w"));
        return integer(7);
    },"int",{});
    check(as_int(runtime.await(t,{}),{})==7,"task result");
    check(child_id!=c.task_id&&stats->closes==1,"child cleanup precedes terminal publication");
    check(std::get<FileValue>(retained.data).data->handle.snapshot().state==R::Lifecycle::closed,"retained wrapper closed");
    runtime.finish();
    auto bad=runtime.start([&](RuntimeContext& child,std::ostream&)->Value{
        retained=unwrap(child.files->adopt(std::make_unique<Fake>(stats),"w"));return retained;
    },"std.file.File",{});
    bool denied=false;try{runtime.await(bad,{});}catch(const Diagnostic& d){denied=d.code()=="E4104";}
    check(denied&&stats->closes==2,"illegal File result cleaned before task failure");runtime.finish();
    auto canceled=runtime.start([&](RuntimeContext& child,std::ostream&)->Value{
        auto local=unwrap(child.files->adopt(std::make_unique<Fake>(stats),"w"));
        task_standard("sleep",{integer(1000)},{},&child);return integer(1);
    },"int",{});
    check(!outcome(runtime.await(canceled,{},std::chrono::milliseconds(20))).ok&&stats->closes==3,"timeout joins after File cleanup");
    runtime.finish();c.files->finish();
}
void external_keepalive(){
    ++cases;auto ledger=R::QuotaLedger::root(caps());R::ResourceDomain domain(ledger);
    auto h=*domain.create(1).value;auto storage=std::make_shared<std::string>("abc");std::weak_ptr<std::string> weak=storage;
    auto e=*domain.reserve(h,1,{},R::Access::read,0,3,storage).value;
    storage.reset();check(!weak.expired(),"external operation strongly owns storage");
    check(!domain.submit(e),"external submit");domain.close(h,1);domain.pump();check(!weak.expired(),"cancel not retirement");
    domain.settle(e,{R::ResultKind::success,{},3,{}});check(weak.expired(),"retirement releases storage");domain.pump();
}
}
int main(){try{
    read_progress();partial_failure();write_progress();zero_and_modes();quota_before_io();known_close_failure();uncertain_close();
    owner_checks();gc_fallback();clone_boundaries();host_exception();seek_validation();close_inflight();task_cleanup();external_keepalive();
    std::cout<<"D002-b native resource tests: "<<cases<<" cases, "<<checks<<" checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"D002-b failure: "<<e.what()<<"\n";return 1;}}
