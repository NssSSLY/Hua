#pragma once
#include "hua/value.hpp"
#include "hua/stdlib.hpp"
#include <iosfwd>
#include <unordered_set>
namespace hua {
const std::unordered_set<std::string>& builtin_names();
Value invoke_builtin(const std::string& name, const std::vector<Value>& args,
                     const SourceSpan& span, std::ostream& output, const std::vector<bool>& contextual = {}, const std::string& result_type = {}, RuntimeContext* context = nullptr);
}
