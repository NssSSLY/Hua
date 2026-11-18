#include "hua/native_rt.hpp"
#include <sstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace hn {
[[noreturn]] void step_limit_exceeded() {
    hua::runtime_error({}, "execution step limit exceeded (" + std::to_string(current_state().step_limit) + ")", "E4099");
}
[[noreturn]] void depth_limit_exceeded(const SourceSpan& site) {
    hua::runtime_error(site, "function call depth limit exceeded (" + std::to_string(current_state().call_limit) + ")", "E4099");
}
[[noreturn]] void bad_unbox(const char* expected){hua::runtime_error({},"native bridge: library result is not "+std::string(expected),"E8002");}
[[noreturn]] void fail(const SourceSpan& span,const char* message,const char* code){hua::runtime_error(span,message,code);}

Value call_builtin(const char* name, const SourceSpan& span, std::vector<Value> args, const std::vector<bool>& contextual, const char* result_type) {
    auto& running = current_state();
    return hua::invoke_builtin(name, args, span, *running.output, contextual, result_type, running.context);
}
Value call_bridge(const BridgeInfo& site, const SourceSpan& span, std::initializer_list<Value> args) {
    auto& running = current_state();
    return hua::invoke_builtin(site.info, site.name, std::vector<Value>(args), span, *running.output,
                               site.contextual, site.result_type, running.context);
}
Value call_bridge(Bridge& site, const SourceSpan& span, std::initializer_list<Value> args) {
    if (site.busy) return call_bridge(static_cast<const BridgeInfo&>(site), span, args);
    struct Lease {
        Bridge& site;
        ~Lease() { site.args.clear(); site.busy = false; }
    } lease{site};
    site.busy = true;
    site.args.assign(args.begin(), args.end());
    return hua::invoke_builtin(site.info, site.name, site.args, span, *state.output, site.contextual, site.result_type, state.context);
}
void print(const SourceSpan& span, std::vector<Value> args) {
    call_builtin("print", span, std::move(args), {}, "void");
}

static hua::Diagnostic with_trace(const hua::Diagnostic& failure, const State& running) {
    auto reported = failure;
    for (unsigned i = running.depth; i-- > 0;) reported.add_context(*running.sites[i], "called from");
    return reported;
}

#ifdef _WIN32
namespace {
struct FiberThread {
    bool converted = !IsThreadAFiber();
    void* controller = converted ? ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH) : GetCurrentFiber();
    ~FiberThread() { if (converted && controller) ConvertFiberToThread(); }
};
thread_local FiberThread fiber_thread;

struct BindState {
    State* previous;
    explicit BindState(State& next) : previous(std::exchange(active_task_state, &next)) {}
    ~BindState() { active_task_state = previous; }
};
}

// The fiber only runs generated synchronous C++. Its scheduler coroutine owns
// every wait, so VM tasks and native tasks use the same completion and deadline rules.
class NativeFiber {
    State running;
    TaskBody& body;
    void* fiber{};
    void* controller{};
    DWORD owner{};
    bool completed{};
    std::optional<hua::Routine<Value>> pending;
    Value value;
    std::exception_ptr error;

    static void WINAPI entry(void* raw) noexcept {
        auto& self = *static_cast<NativeFiber*>(raw);
        try {
            try {
                self.value = self.body();
            } catch (const hua::Diagnostic& failure) {
                throw with_trace(failure, self.running);
            }
        } catch (...) {
            self.error = std::current_exception();
        }
        self.completed = true;
        SwitchToFiber(self.controller);
        std::terminate(); // Returning from a Windows fiber callback exits the thread.
    }

public:
    NativeFiber(TaskBody& body, hua::RuntimeContext& context, std::ostream& output, const SourceSpan& site)
        : body(body) {
        running.steps_left = static_cast<std::int64_t>(context.execution_limits.steps);
        running.step_limit = context.execution_limits.steps;
        running.call_limit = context.execution_limits.calls;
        running.context = &context;
        running.output = &output;
        running.fiber = this;
        running.sites[0] = &site;
        running.depth = 1;
        owner = GetCurrentThreadId();
        controller = fiber_thread.controller;
        if (!controller) fail(site, "cannot initialize native Task fiber thread", "E4103");
        fiber = CreateFiberEx(64 * 1024, 8 * 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, entry, this);
        if (!fiber) fail(site, "cannot allocate native Task stack", "E4099");
    }
    ~NativeFiber() { DeleteFiber(fiber); }

    Value wait(hua::Routine<Value> operation) {
        pending.emplace(std::move(operation));
        SwitchToFiber(controller);
        if (error) std::rethrow_exception(std::exchange(error, {}));
        return std::exchange(value, {});
    }

    hua::Routine<Value> run() {
        for (;;) {
            // Root runs on the entry thread. All child fibers use the single
            // compute worker. Win32 TLS and C++ unwinding remain thread-affine.
            if (GetCurrentThreadId() != owner) std::terminate();
            {
                BindState bind(running);
                SwitchToFiber(fiber);
            }
            if (completed) break;
            if (!pending) std::terminate();
            try {
                value = co_await std::move(*pending);
            } catch (...) {
                error = std::current_exception();
            }
            pending.reset();
        }
        if (error) std::rethrow_exception(error);
        co_return std::move(value);
    }
};

static hua::Routine<Value> yield_native() {
    co_await hua::Yield{true};
    co_return Value{};
}
#endif

void task_poll(State& running, const SourceSpan& site) {
    hua::cancellation_checkpoint(running.context, site);
#ifdef _WIN32
    if (running.context->in_task && ++running.ticks % 1024 == 0) {
        running.fiber->wait(yield_native());
        hua::cancellation_checkpoint(running.context, site);
    }
#endif
}
void task_loop(State& running) {
    task_poll(running, *running.sites[running.depth - 1]);
}

#ifdef _WIN32
static hua::Routine<Value> execute_native(TaskBody body, hua::RuntimeContext& context, std::ostream& out, SourceSpan site) {
    NativeFiber fiber(body, context, out, site);
    co_return co_await fiber.run();
}

static hua::Routine<Value> execute_child(TaskBody& body, hua::RuntimeContext& child, std::ostream& out, SourceSpan site) {
    hua::TaskRuntime tasks(child, out);
    child.tasks = &tasks;
    Value returned;
    std::exception_ptr error;
    try {
        returned = co_await execute_native(std::move(body), child, out, site);
    } catch (...) {
        error = std::current_exception();
    }
    try {
        co_await tasks.finish_async(bool(error));
    } catch (...) {
        if (!error) error = std::current_exception();
    }
    child.tasks = nullptr;
    if (error) std::rethrow_exception(error);
    co_return returned;
}
#endif

TaskHandle start_task(TaskBody body, const char* type, const SourceSpan& span) {
#ifdef _WIN32
    auto job = [body = std::move(body), span](hua::RuntimeContext& child, std::ostream& out) mutable {
        return execute_child(body, child, out, span);
    };
    return {current_state().context->tasks->start_resumable(std::move(job), type, span)};
#else
    fail(span, "native Task requires Windows", "E4103");
#endif
}
Value await_task(const TaskHandle& task, const SourceSpan& span) {
#ifdef _WIN32
    auto& running = current_state();
    return running.fiber->wait(running.context->tasks->await_async(task.value, span));
#else
    fail(span, "native Task requires Windows", "E4103");
#endif
}
void task_wait(const char* name, std::vector<Value> args, const SourceSpan& span) {
#ifdef _WIN32
    auto& running = current_state();
    running.fiber->wait(hua::invoke_resumable(name, std::move(args), span, *running.output, {}, "void", running.context));
#else
    fail(span, "native Task requires Windows", "E4103");
#endif
}

int run_main(int argc, char** argv, const SourceSpan& main_site, Value (*body)(), bool tasks) {
    hua::RuntimeContext base;
    base.input = &std::cin;
    base.working_directory = std::filesystem::current_path();
    base.execution_limits = {state.step_limit, state.call_limit, 65536};
    for (int i = 1; i < argc; ++i) {
        if (i == 1 && std::string(argv[i]) == "--") continue;
        base.arguments.emplace_back(argv[i]);
    }
    try {
        Value returned;
        std::exception_ptr error;
        {
            hua::RuntimeContext* context = &base;
            hua::ExecutionScope execution(context, std::cout);
            static const hua::TypeRelations no_relations;
            context->type_relations = &no_relations;
            state.context = context;
            state.output = &std::cout;
            state.sites[0] = &main_site;
            state.depth = 1;
            if (tasks) {
#ifdef _WIN32
                hua::use_single_task_worker();
                native_tasks_enabled = true;
                context->resumable = true;
                try {
                    returned = hua::sync_wait(execute_native(body, *context, std::cout, main_site), *context);
                } catch (...) {
                    error = std::current_exception();
                }
                try {
                    hua::sync_wait(execution.finish_async(bool(error)), *context);
                } catch (...) {
                    if (!error) error = std::current_exception();
                }
#else
                fail(main_site, "native Task requires Windows", "E4103");
#endif
            } else {
                try { returned = body(); } catch (...) { error = std::current_exception(); }
                try { execution.finish(); } catch (...) { if (!error) error = std::current_exception(); }
            }
            state.context = nullptr;
            state.output = nullptr;
            if (error) returned = Value{};
        }
        if (error) std::rethrow_exception(error);
        return hua::report_entry(hua::entry_result(returned,main_site),std::cerr);
    }catch(const hua::ProgramExit& exit){return exit.code;}
    catch(const hua::Diagnostic& failure){
        auto reported = tasks ? failure : with_trace(failure, state);
        const auto& file=reported.span().file_id;std::ifstream input(std::filesystem::path(std::u8string(file.begin(),file.end())),std::ios::binary);
        if(!file.empty()&&input){
            std::ostringstream text;text<<input.rdbuf();hua::Source source(file,text.str());std::cerr<<reported.render(source);
        }else{
            std::cerr<<"error["<<reported.code()<<"]: "<<reported.what()<<'\n';
            if(!file.empty())std::cerr<<" --> "<<file<<':'<<reported.span().line<<':'<<reported.span().column<<'\n';
            std::cerr<<hua::render_source_trace(reported.trace());
        }
        return hua::diagnostic_exit_code(reported.code());
    }catch(const std::exception& failure){std::cerr<<"error[E0002]: internal command failure: "<<failure.what()<<'\n';return 3;}
}
}
