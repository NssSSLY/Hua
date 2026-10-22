#pragma once
#include "hua/routine.hpp"
#include "hua/value.hpp"
#include <chrono>
#include <functional>
#include <memory>
namespace hua {
struct RuntimeContext;
using Clock=std::chrono::steady_clock;
struct Suspend {
    std::function<bool()> ready;
    Clock::time_point deadline=Clock::time_point::max();
    bool await_ready()const{return ready()||Clock::now()>=deadline;}
    void await_suspend(std::coroutine_handle<>);
    void await_resume()const noexcept{}
};
struct Yield {
    bool force{};
    unsigned weight{1}; // safe-point ticks represented by this check (VM polls every N steps)
    bool await_ready()const;
    void await_suspend(std::coroutine_handle<>);
    void await_resume()const noexcept{}
};
struct BlockingState;
struct Blocking {
    std::function<Value()> job;std::shared_ptr<BlockingState> state;
    explicit Blocking(std::function<Value()>);
    bool await_ready()const noexcept{return false;}
    void await_suspend(std::coroutine_handle<>);
    Value await_resume();
};
Value sync_wait(Routine<Value>,RuntimeContext&);
void sync_wait(Routine<void>,RuntimeContext&);
unsigned task_worker_count();
void notify_task_scheduler()noexcept;
}
