#pragma once
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
    bool resumable{},in_task{},cleanup{},unwinding{};
};
struct StandardFunction { std::string module, name; std::vector<std::string> parameters; std::string result; bool mutates_first{}; bool writable_result{}; unsigned since{3}; };
struct StandardSignature { std::vector<std::string> parameters; std::string result; };
StandardSignature standard_signature(const StandardFunction& function,const std::vector<std::string>& argument_types);
std::string format_error(const ErrorValue& value);
Value invoke_error_standard(const StandardFunction&,const std::vector<Value>&,const SourceSpan&);
Value invoke_algorithm_standard(const StandardFunction&,const std::vector<Value>&,const SourceSpan&,RuntimeContext*);
Value invoke_filesystem_standard(const StandardFunction&,const std::vector<Value>&,const SourceSpan&,RuntimeContext*);
Value invoke_system_standard(const StandardFunction&,const std::vector<Value>&,const SourceSpan&,RuntimeContext*);
Value invoke_binary_standard(const StandardFunction& function,const std::vector<Value>& args,const SourceSpan& span);
const std::vector<StandardFunction>& standard_functions();
std::string standard_name(const StandardFunction& function);
const StandardFunction* standard_function(std::string_view name);
bool standard_module(std::string_view name);
Value invoke_standard(const StandardFunction& function, const std::vector<Value>& arguments,
                      const SourceSpan& span, std::ostream& output, RuntimeContext* context);
Value standard_result(Value payload, bool ok, const std::string& success_type);
std::string text_utf8_error(std::string_view text);
}
