#pragma once
#include "hua/value.hpp"
#include <iosfwd>
namespace hua {
Value invoke_builtin(const std::string& name, const std::vector<Value>& args,
                     const SourceSpan& span, std::ostream& output);
}
