#pragma once
#include <coroutine>
#include <exception>
#include <optional>
#include <utility>
namespace hua {
// Internal, move-only continuation. Child calls use symmetric transfer; no replay.
template<class T> class Routine {
public:
    struct promise_type {
        std::optional<T> result;std::exception_ptr error;std::coroutine_handle<> parent=std::noop_coroutine();
        Routine get_return_object(){return Routine(std::coroutine_handle<promise_type>::from_promise(*this));}
        std::suspend_always initial_suspend()noexcept{return {};}
        struct Final {
            bool await_ready()noexcept{return false;}
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h)noexcept{return h.promise().parent;}
            void await_resume()noexcept{}
        };
        Final final_suspend()noexcept{return {};}
        template<class U> void return_value(U&& value){result.emplace(std::forward<U>(value));}
        void unhandled_exception()noexcept{error=std::current_exception();}
    };
    using Handle=std::coroutine_handle<promise_type>;
    Handle handle{};
    explicit Routine(Handle h):handle(h){}
    Routine(Routine&& r)noexcept:handle(std::exchange(r.handle,{})){}
    Routine(const Routine&)=delete;
    ~Routine(){if(handle)handle.destroy();}
    T take(){if(handle.promise().error)std::rethrow_exception(handle.promise().error);return std::move(*handle.promise().result);}
    struct Awaiter {
        Handle handle;
        bool await_ready()noexcept{return false;}
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent)noexcept{handle.promise().parent=parent;return handle;}
        T await_resume(){if(handle.promise().error)std::rethrow_exception(handle.promise().error);return std::move(*handle.promise().result);}
    };
    Awaiter operator co_await()&& noexcept{return {handle};}
};
template<> class Routine<void> {
public:
    struct promise_type {
        std::exception_ptr error;std::coroutine_handle<> parent=std::noop_coroutine();
        Routine get_return_object(){return Routine(std::coroutine_handle<promise_type>::from_promise(*this));}
        std::suspend_always initial_suspend()noexcept{return {};}
        struct Final {
            bool await_ready()noexcept{return false;}
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h)noexcept{return h.promise().parent;}
            void await_resume()noexcept{}
        };
        Final final_suspend()noexcept{return {};}
        void return_void()noexcept{}
        void unhandled_exception()noexcept{error=std::current_exception();}
    };
    using Handle=std::coroutine_handle<promise_type>;Handle handle{};
    explicit Routine(Handle h):handle(h){}
    Routine(Routine&& r)noexcept:handle(std::exchange(r.handle,{})){}
    Routine(const Routine&)=delete;
    ~Routine(){if(handle)handle.destroy();}
    void take(){if(handle.promise().error)std::rethrow_exception(handle.promise().error);}
    struct Awaiter {
        Handle handle;
        bool await_ready()noexcept{return false;}
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent)noexcept{handle.promise().parent=parent;return handle;}
        void await_resume(){if(handle.promise().error)std::rethrow_exception(handle.promise().error);}
    };
    Awaiter operator co_await()&& noexcept{return {handle};}
};
}
