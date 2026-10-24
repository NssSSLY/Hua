#pragma once
#include "hua/routine.hpp"
#include "hua/stdlib.hpp"
namespace hua {
// L0-1 (HUA-D038, since 11): standard functions that call Hua function values.
// Each engine supplies the host; the algorithms are written once, so the AST
// interpreter, the source VM and HUAB execution share one implementation.
struct CallbackShape { std::size_t parameters{}; std::string result; };
// Declared parameter/result types of a callback; rejects (E4003) a callable that
// is not a plain Hua function with `arity` annotated non-mut parameters and one
// annotated, non-Task result.
CallbackShape callback_shape(const Callable& callable, std::size_t arity, const SourceSpan& span);
class CallbackHost {
public:
    // One ordinary call: parameter checks/copies, steps, call depth, cancellation
    // and diagnostics all follow the engine's normal call path.
    virtual Routine<Value> invoke(const Callable& callable, std::vector<Value> args, const SourceSpan& span) = 0;
protected:
    ~CallbackHost() = default;
};
Routine<Value> invoke_callback_standard(const StandardFunction& function, std::vector<Value> args,
                                        const SourceSpan& span, CallbackHost& host, RuntimeContext* context);
}
