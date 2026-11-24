#pragma once
#include "hua/value.hpp"
#include <span>
namespace hua {
// Check the complete positional write set before any target is committed.
void prepare_map_writes(std::span<const ValueLocation> locations, const SourceSpan& span);
void location_write(const ValueLocation& where, const Value& value, const SourceSpan& span);
}
