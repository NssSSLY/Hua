#pragma once
#include "hua/source.hpp"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
namespace hua {
enum class NodeKind {
    Program, Block, Let, Var, Const, Function, Parameter, ReturnTypes, Struct, Field,
    Import, If, While, For, Return, Break, Continue, ExpressionStatement, Unsafe,
    Integer, Float, String, Boolean, Nil, Name, Unary, Binary, Assignment, Update,
    Call, Index, Slice, Member, Array, StructLiteral, FieldInit, Range, Omitted,
    TypeName, SliceType, ArrayType, GenericType, OptionalType, MutableType, ExternalInit, MapLiteral, MapEntry, Pack, BindingList, MultiBinding, MultiAssignment, Propagate, Defer, Attribute, GenericParameters, Specialize, Interface, Enum, Variant, Match, MatchArm
};
// Syntax only. Owned children and source spans form the interface to later semantic/runtime passes.
// Child ordering is documented in docs/语法树与源码位置.md; no parser-side execution or evaluated values.
struct Node {
    NodeKind kind;
    SourceSpan span;
    std::string text;
    std::vector<std::unique_ptr<Node>> children;
    std::size_t height{1};
    Node(NodeKind kind, SourceSpan span, std::string text = {})
        : kind(kind), span(std::move(span)), text(std::move(text)) {}
    void add(std::unique_ptr<Node> child) {
        if (!child) throw std::logic_error("AST child must be non-null");
        if (child->height >= 384) throw Diagnostic("E2008", child->span, "AST nesting limit exceeded");
        height = std::max(height, child->height + 1);
        children.push_back(std::move(child));
    }
};
using NodePtr = std::unique_ptr<Node>;
std::string_view node_name(NodeKind kind);
std::string print_ast(const Node& node);
}
