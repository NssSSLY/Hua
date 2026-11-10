#pragma once
#include "hua/ast.hpp"
#include "hua/sema.hpp"
#include <string>
namespace hua {
// Linked AST with the checker's type (and [w] writability) per expression node.
std::string dump_typed_ast(const Node& tree,const SemanticModel& model);
// A construct outside the native backend's current subset; the whole program is rejected.
struct NativeUnsupported { std::string what; SourceSpan span; };
// C++ source for the whole program (links against hua_semantics; see native_rt.hpp).
std::string emit_native(const Node& tree,const SemanticModel& model);
}
