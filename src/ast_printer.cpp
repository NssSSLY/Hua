#include "hua/ast.hpp"
#include <sstream>
namespace hua {
std::string_view node_name(NodeKind kind) {
    switch (kind) {
    case NodeKind::ExternalInit: return "ExternalInit";
#define HUA_NODE(x) case NodeKind::x: return #x;
    HUA_NODE(Program) HUA_NODE(Block) HUA_NODE(Let) HUA_NODE(Var) HUA_NODE(Const)
    HUA_NODE(Function) HUA_NODE(Parameter) HUA_NODE(ReturnTypes) HUA_NODE(Struct) HUA_NODE(Field)
    HUA_NODE(Import) HUA_NODE(If) HUA_NODE(While) HUA_NODE(For) HUA_NODE(Return)
    HUA_NODE(Break) HUA_NODE(Continue) HUA_NODE(ExpressionStatement) HUA_NODE(Unsafe)
    HUA_NODE(Integer) HUA_NODE(Float) HUA_NODE(String) HUA_NODE(Boolean) HUA_NODE(Nil)
    HUA_NODE(Name) HUA_NODE(Unary) HUA_NODE(Binary) HUA_NODE(Assignment) HUA_NODE(Update)
    HUA_NODE(Call) HUA_NODE(Index) HUA_NODE(Slice) HUA_NODE(Member) HUA_NODE(Array)
    HUA_NODE(StructLiteral) HUA_NODE(FieldInit) HUA_NODE(Range) HUA_NODE(Omitted)
    HUA_NODE(TypeName) HUA_NODE(SliceType) HUA_NODE(ArrayType) HUA_NODE(GenericType)
    HUA_NODE(OptionalType) HUA_NODE(MutableType)
#undef HUA_NODE
    }
    return "Unknown";
}
namespace {
std::string quote(std::string_view text) {
    std::string result = "\"";
    for (unsigned char c : text) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        case '\0': result += "\\0"; break;
        default: result += static_cast<char>(c);
        }
    }
    return result + "\"";
}
void write(std::ostringstream& out, const Node& node, std::size_t indent) {
    out << std::string(indent, ' ') << '(' << node_name(node.kind);
    if (!node.text.empty() || node.kind == NodeKind::String)
        out << ' ' << (node.kind == NodeKind::String ? quote(node.text) : node.text);
    if (node.children.empty()) { out << ")\n"; return; }
    out << '\n';
    for (const auto& child : node.children) write(out, *child, indent + 2);
    out << std::string(indent, ' ') << ")\n";
}
}
std::string print_ast(const Node& node) {
    std::ostringstream out; write(out, node, 0); return out.str();
}
}
