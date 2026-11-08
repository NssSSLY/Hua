#pragma once
#include <string_view>
#include <cstdint>
#include "hua/value.hpp"
#include "hua/execution.hpp"
#include <filesystem>
#include <iosfwd>
namespace hua {
class FileScope;
struct TaskRuntime;struct Cancellation;class ManagedHeap;
struct RuntimeContext {
    ExecutionLimits execution_limits;
    std::shared_ptr<ExecutionMetrics> execution_metrics;
    std::istream* input{};
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
    TaskRuntime* tasks{};
    std::shared_ptr<Cancellation> cancellation;
    std::shared_ptr<ManagedHeap> heap;
    std::uint64_t task_id{};std::shared_ptr<FileScope> files;
    const TypeRelations* type_relations{};
    bool resumable{},in_task{},cleanup{},unwinding{},exiting{};
};
struct StandardFunction { std::string module, name; std::vector<std::string> parameters; std::string result; bool mutates_first{}; bool writable_result{}; unsigned since{3}; bool callback{};
    // Dispatch cache: module_code(module), filled on first call (0 = not yet resolved).
    mutable std::uint8_t module_id{}; };
// Small integer per standard module, so call dispatch compares bytes, not strings.
inline constexpr std::string_view standard_modules[]={"alg","buffer","bytes","crypto","deque","encoding","error","file","fs","gc","heap","io","json","list","log","maps","math","os","path","process","random","sets","simd","strings","task","test","time"};
constexpr std::uint8_t module_lookup(std::string_view name){for(std::size_t i=0;i<std::size(standard_modules);++i)if(standard_modules[i]==name)return static_cast<std::uint8_t>(i+1);return 255;}
// Literal form: always folded at compile time.
consteval std::uint8_t module_code(std::string_view name){return module_lookup(name);}
inline std::uint8_t standard_module_id(const StandardFunction& f){if(!f.module_id)f.module_id=module_lookup(f.module);return f.module_id;}
struct StandardSignature { std::vector<std::string> parameters; std::string result; };
StandardSignature standard_signature(const StandardFunction& function,const std::vector<std::string>& argument_types);
std::string format_error(const ErrorValue& value);
Value invoke_error_standard(const StandardFunction&,const std::vector<Value>&,const SourceSpan&);
Value invoke_math_arithmetic(const StandardFunction&,const std::vector<Value>&,const SourceSpan&);
Value invoke_algorithm_standard(const StandardFunction&,const std::vector<Value>&,const SourceSpan&,RuntimeContext*);
Value invoke_filesystem_standard(const StandardFunction&,const std::vector<Value>&,const SourceSpan&,RuntimeContext*);
Value invoke_system_standard(const StandardFunction&,const std::vector<Value>&,const SourceSpan&,RuntimeContext*);
Value invoke_binary_standard(const StandardFunction& function,const std::vector<Value>& args,const SourceSpan& span);
Value invoke_text_standard(const StandardFunction& function,const std::vector<Value>& args,const SourceSpan& span);
Value invoke_collection_standard(const StandardFunction& function,const std::vector<Value>& args,const SourceSpan& span,RuntimeContext* context);
std::int64_t monotonic_now_ns(const SourceSpan& span);
std::string format_rfc3339_utc(std::int64_t ms);
void reset_report_state();
Value invoke_report_standard(const StandardFunction& function,const std::vector<Value>& args,const SourceSpan& span);
Value invoke_os_standard(const StandardFunction& function,const std::vector<Value>& args,const SourceSpan& span,RuntimeContext* context);
Value invoke_time_standard(const StandardFunction& function,const std::vector<Value>& args,const SourceSpan& span);
Value invoke_deque_standard(const StandardFunction& function,const std::vector<Value>& args,const SourceSpan& span);
Value invoke_heap_standard(const StandardFunction& function,const std::vector<Value>& args,const SourceSpan& span);
Value invoke_random_standard(const StandardFunction& function,const std::vector<Value>& args,const SourceSpan& span);
Value invoke_crypto_standard(const StandardFunction& function,const std::vector<Value>& args,const SourceSpan& span,RuntimeContext* context);
Value invoke_unicode_case(const StandardFunction& function,const std::vector<Value>& args,const SourceSpan& span);
Value invoke_process_standard(const StandardFunction& function,const std::vector<Value>& args,const SourceSpan& span,RuntimeContext* context);
Value invoke_encoding_standard(const StandardFunction& function,const std::vector<Value>& args,const SourceSpan& span);
Value invoke_list_extra(const StandardFunction& function,const std::vector<Value>& args,const SourceSpan& span,RuntimeContext* context);
const std::vector<StandardFunction>& standard_functions();
std::string standard_name(const StandardFunction& function);
const StandardFunction* standard_function(std::string_view name);
bool standard_module(std::string_view name);
Value invoke_standard(const StandardFunction& function, const std::vector<Value>& arguments,
                      const SourceSpan& span, std::ostream& output, RuntimeContext* context);
Value standard_result(Value payload, bool ok, const std::string& success_type);
std::string text_utf8_error(std::string_view text);
// Appends one Unicode scalar as UTF-8; false (nothing appended) for surrogates and values outside 0..10FFFF.
bool append_utf8(std::string& out,std::int64_t scalar);
}
