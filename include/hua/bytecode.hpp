#pragma once
#include "hua/sema.hpp"
namespace hua {
// In-memory instruction storage; archive.cpp defines the versioned HUAB v1 encoding.
enum class Op {
    Constant, Load, Bind, Pop, Unary, Binary, Jump, JumpFalse, JumpTrue,
    EnterScope, LeaveScope, Unwind, LocateName, LocateField, LocateIndex, Store,
    MakeArray, Index, Slice, MakeStruct, Member, Call, Return,
    RangeInit, SliceInit, IterNext, IterEnd, Fail, CheckSlice, ArrayAppend, InitField, ExternalInit
};
struct Instruction {
    Op op;
    SourceSpan span;
    std::string text, type;
    std::size_t argument{}, target{};
    Value constant;
    bool flag{};
    std::vector<std::string> names;
    std::vector<bool> contextual;
};
struct Code { std::string name; std::vector<Instruction> instructions; };
struct Bytecode {
    Code initializer;
    std::unordered_map<const Node*, Code> functions;
};
class BytecodeCompiler {
public:
    Bytecode compile(const Node& program, const SemanticModel& model);
private:
    struct Loop { std::size_t depth, next; std::vector<std::size_t> breaks; };
    const SemanticModel* model_{};
    Code* code_{};
    std::size_t depth_{};
    std::vector<Loop> loops_;
    std::size_t emit(Op op, const Node& n, std::string text = {});
    void patch(std::size_t instruction, std::size_t destination);
    void statement(const Node& n);
    void block(const Node& n, bool scope = true);
    void expression(const Node& n);
    void location(const Node& n);
};
std::string disassemble(const Bytecode& bytecode);
}
