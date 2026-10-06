#pragma once
#include "hua/value.hpp"
#include "hua/gc.hpp"
#include <exception>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
namespace hua {
struct RuntimeContext;
struct Cancellation {std::atomic<bool> requested{false};std::shared_ptr<Cancellation> parent;bool stopped()const{return requested||(parent&&parent->stopped());}};
struct TaskState {
    std::mutex mutex,join_mutex;std::condition_variable changed;
    bool finished{},observed{},emitted{},canceled_failure{};Value result;std::exception_ptr error;
    std::string result_type,output;
    std::shared_ptr<Cancellation> cancellation;
    std::shared_ptr<ManagedHeap> heap;
    std::jthread worker;
    ~TaskState();
};
using TaskJob=std::function<Value(RuntimeContext&,std::ostream&)>;
struct TaskRuntime {
    RuntimeContext& context;std::ostream& output;
    std::vector<std::shared_ptr<TaskState>> children;
    std::vector<std::pair<std::size_t,std::size_t>> groups;
    std::size_t next_group{1};
    TaskRuntime(RuntimeContext& c,std::ostream& o):context(c),output(o){}
    ~TaskRuntime();
    Value start(TaskJob job,std::string type,const SourceSpan& span,std::shared_ptr<ManagedHeap> heap={});
    void finish(bool cancel=false);
    Value await(const Value& task,const SourceSpan& span,std::optional<std::chrono::milliseconds> timeout={});
    std::size_t begin_group();void end_group(std::size_t id,const SourceSpan& span);
};
struct ExecutionScope {
    RuntimeContext*& target;RuntimeContext* previous;
    std::unique_ptr<RuntimeContext> context;
    std::shared_ptr<ManagedHeap> heap;
    HeapScope heap_scope;
    std::unique_ptr<TaskRuntime> runtime;
    ExecutionScope(RuntimeContext*&,std::ostream&);
    ~ExecutionScope();
    void finish();
};
void task_checkpoint(RuntimeContext*,const SourceSpan&);
Value task_standard(std::string_view operation,const std::vector<Value>& args,const SourceSpan&,RuntimeContext*);
Value task_combine(std::string_view operation,const std::vector<Value>& args,const SourceSpan&,RuntimeContext*);
std::string task_result_type(const Callable&);
}
