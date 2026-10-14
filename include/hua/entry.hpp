#pragma once
#include "hua/value.hpp"
#include <functional>
#include <iosfwd>
namespace hua {
// Host boundary result. It carries no live language values or resources.
struct EntryResult {
    int exit_code{};
    std::string message;
    SourceSpan span;
    SourceTrace failure_source{};
};
EntryResult entry_result(const Value& value, const SourceSpan& span);
int report_entry(const EntryResult& result, std::ostream& error);
int diagnostic_exit_code(std::string_view code);
int command_boundary(const std::function<int()>& command, std::ostream& error);
}
