#pragma once
#include "hua/bytecode.hpp"
#include <compare>
#include <concepts>
#include <variant>
#include <cstdint>
#include <vector>
namespace hua::ir {
// IDs are deterministic within one module revision, never object addresses.
// Zero is invalid. Source/type/symbol/function/HIR IDs are module-local;
// block/value IDs are function-local. They are not a persistent file format.
template<class Tag> struct Id {
    std::uint32_t value{};
    explicit operator bool() const { return value != 0; }
    auto operator<=>(const Id&) const = default;
};
struct SourceTag; struct ModuleTag; struct TypeTag; struct SymbolTag;
struct FunctionTag; struct HirNodeTag; struct BlockTag; struct ValueTag;
using SourceId=Id<SourceTag>; using ModuleId=Id<ModuleTag>;
using TypeId=Id<TypeTag>; using SymbolId=Id<SymbolTag>;
using FunctionId=Id<FunctionTag>; using HirNodeId=Id<HirNodeTag>;
using BlockId=Id<BlockTag>; using ValueId=Id<ValueTag>;
inline constexpr TypeId void_type{1}, int_type{2}, float_type{3}, bool_type{4};
enum Effect : std::uint32_t {
    Read=1, Write=2, Allocate=4, MayFail=8, MaySuspend=16,
    Io=32, External=64, Unknown=128, Nondeterministic=256
};
enum class TypeKind { Void, Int, Float, Bool, Callable };
enum class Helper { None, Print, Int, Float };
struct Type {
    TypeId id; TypeKind kind{}; std::string name;
    std::vector<TypeId> parameters; TypeId result;
    Helper helper{}; bool variadic{};
};
struct SourceInfo { SourceId id; std::string filename; std::size_t bytes{}; };
struct ModuleInfo { ModuleId id; SourceId source; std::string name; };
enum class SymbolKind { Global, Local, Parameter, Function, Helper };
struct Symbol {
    SymbolId id; std::string name; TypeId type; SymbolKind kind{};
    FunctionId owner, function; bool mutable_binding{}; SourceSpan span;
};
struct Tables {
    std::vector<SourceInfo> sources;
    std::vector<ModuleInfo> modules;
    std::vector<Type> types;
    std::vector<Symbol> symbols;
};
struct FunctionInfo {
    FunctionId id; SymbolId symbol; std::string name; SourceSpan span;
    std::vector<SymbolId> parameters; TypeId result{void_type};
    std::uint32_t effects{}; bool initializer{};
};
// IR constants cannot contain Callable/Node*, containers, or runtime handles.
struct ScalarConstant {
    using Data=std::variant<std::monostate,bool,std::int64_t,double>;
    Data data;
    ScalarConstant()=default;
    template<class T> requires (std::same_as<T,bool>||std::same_as<T,std::int64_t>||std::same_as<T,double>)
    explicit ScalarConstant(T value):data(value){}
};
ScalarConstant unbox_constant(const Value&,const SourceSpan&);
Value box_constant(const ScalarConstant&);
enum class HirKind {
    Block, Constant, Load, Unary, Binary, Store, Call, Bind,
    Expression, If, While, For, Return, Break, Continue
};
struct HirNode {
    HirNodeId id; HirKind kind{}; SourceId source; TypeId type{void_type};
    SymbolId symbol; SourceSpan span, control_span; std::string text;
    std::vector<HirNodeId> children; std::vector<SymbolId> names;
    ScalarConstant constant; std::string failure_code, failure_message;
    std::uint32_t effects{}; bool contextual{}, flag{};
};
struct HirFunction { FunctionInfo info; HirNodeId body; };
struct HirModule {
    Tables tables; std::vector<HirNode> nodes; std::vector<HirFunction> functions;
};
HirModule build_hir(const Node& program, const SemanticModel& model,
                    const std::vector<const Source*>& sources = {});
void verify_hir(const HirModule& module);
std::string dump_hir(const HirModule& module);

enum class ValueKind { Scalar, Callable, Location };
struct MirValue { ValueId id; TypeId type; ValueKind kind{}; };
enum class MirOp {
    Constant, Load, Locate, Bind, Drop, Unary, Binary, Store, Call,
    EnterScope, LeaveScope, Unwind, RangeInit, IterEnd, Raise
};
struct MirInstruction {
    MirOp op{}; SourceSpan span; SymbolId symbol; TypeId type{void_type};
    std::vector<ValueId> operands; ValueId result;
    ScalarConstant constant; std::string text, failure_code;
    std::vector<bool> contextual; bool flag{}; std::size_t count{};
    std::uint32_t effects{}; BlockId failure;
    // Each emitted legacy instruction charges exactly one VM step.
    std::uint32_t reference_steps{1}; bool cancellation_checkpoint{true};
};
struct Edge { BlockId target; std::vector<ValueId> arguments; };
enum class TermKind { Fallthrough, Jump, Branch, IterNext, Return, PropagateError };
struct Terminator {
    TermKind kind{TermKind::PropagateError}; SourceSpan span;
    std::vector<ValueId> operands; Edge taken, next;
    std::vector<SymbolId> names; bool flag{}, implicit_return{};
    BlockId failure;
    std::uint32_t reference_steps{}; bool cancellation_checkpoint{};
};
struct MirBlock {
    BlockId id; std::vector<ValueId> parameters;
    std::vector<MirInstruction> instructions; Terminator terminator;
};
struct MirFunction {
    FunctionInfo info; BlockId entry, error;
    std::vector<MirValue> values; std::vector<MirBlock> blocks;
};
struct MirModule { Tables tables; std::vector<MirFunction> functions; };
MirModule lower_mir(const HirModule& module);
void verify_mir(const MirModule& module);
std::string dump_mir(const MirModule& module);
std::string explain_mir(const MirModule& module);
// Only this adapter borrows existing AST signature metadata. HIR/MIR own no Node*.
// Caller keeps SemanticModel and its ModuleLoader alive through VM/archive use.
Bytecode lower_bytecode(const MirModule& module, const SemanticModel& model);

const Type& type(const Tables&, TypeId);
const Symbol& symbol(const Tables&, SymbolId);
[[noreturn]] void invalid(const SourceSpan&, const std::string&);
}
