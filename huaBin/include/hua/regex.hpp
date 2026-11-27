#pragma once
#include "hua/stdlib.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace hua {
struct RegexData;
struct ProcessChildData;
Value invoke_regex_standard(const StandardFunction& function, const std::vector<Value>& arguments,
                            const SourceSpan& span);
} // namespace hua
