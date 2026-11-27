#pragma once
#include "hua/stdlib.hpp"
namespace hua {
Value invoke_compress_standard(const StandardFunction& f, const std::vector<Value>& args, const SourceSpan& s, RuntimeContext* context);
}
