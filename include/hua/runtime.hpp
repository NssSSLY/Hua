#pragma once
#include "hua/value.hpp"
#include "hua/stdlib.hpp"
#include <iosfwd>
#include <unordered_set>
namespace hua {
const std::unordered_set<std::string>& builtin_names();
// Resolved once per builtin name (and cached per call site by the VM): the core
// builtin id, the standard function, and when the call may suspend.
enum class CoreBuiltin : std::uint8_t { other, numeric, print, str, int_, float_, len, clone, sqrt, min, max, abs, clamp, type, has, delete_, ok, err, is_ok, is_err, unwrap, unwrap_err, unwrap_or, is_some, is_none, sizeof_, alignof_, task_start, task_await, task_group_begin, task_group_end, parallel_map, simd_check };
struct BuiltinInfo { CoreBuiltin core{CoreBuiltin::other}; const StandardFunction* standard{}; bool always_suspends{}; bool suspends_in_task{}; };
const BuiltinInfo& builtin_info(const std::string& name);
// Same decision as builtin_may_suspend(name, context).
inline bool builtin_may_suspend(const BuiltinInfo& info,const RuntimeContext* context){return info.always_suspends||(context&&context->in_task&&info.suspends_in_task);}
Value invoke_builtin(const BuiltinInfo& info, const std::string& name, const std::vector<Value>& args,
                     const SourceSpan& span, std::ostream& output, const std::vector<bool>& contextual = {}, const std::string& result_type = {}, RuntimeContext* context = nullptr);
Value invoke_builtin(const std::string& name, const std::vector<Value>& args,
                     const SourceSpan& span, std::ostream& output, const std::vector<bool>& contextual = {}, const std::string& result_type = {}, RuntimeContext* context = nullptr);
}
