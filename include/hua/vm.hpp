#pragma once
#include "hua/bytecode.hpp"
#include <iosfwd>
namespace hua {
class VirtualMachine {
public:
    VirtualMachine(const SemanticModel& model, std::ostream& output) : model_(model), output_(output) {}
    void run(const Bytecode& bytecode);
private:
    struct Binding { Value value; std::string type; bool mutable_binding{}; };
    struct Environment {
        std::shared_ptr<Environment> parent;
        std::unordered_map<std::string, Binding> bindings;
        Binding& lookup(const std::string& name, const SourceSpan& span);
    };
    struct Location { Value* value; std::string type; std::shared_ptr<void> owner; };
    using Entry = std::variant<Value, Location>;
    struct Iterator {
        bool range{}, inclusive{}, done{};
        std::int64_t current{}, end{}, step{}, index{};
        SliceValue slice;
    };
    struct Frame {
        const Code* code{};
        const Node* function{};
        std::size_t pc{}, base{};
        SourceSpan site;
        std::shared_ptr<Environment> environment;
        std::vector<Iterator> iterators;
    };
    const SemanticModel& model_;
    std::ostream& output_;
    const Bytecode* bytecode_{};
    std::shared_ptr<Environment> global_;
    std::vector<Entry> stack_;
    std::vector<Frame> frames_;
    std::size_t steps_{};
    Value pop(const SourceSpan& span);
    std::vector<Value> arguments(std::size_t count, const SourceSpan& span);
    void bind(const Instruction& op, Value value);
    void call(const Callable& callable, const std::vector<Value>& args, const Instruction& site);
    void dispatch();
};
}
