#pragma once
#include "hua/bytecode.hpp"
#include "hua/stdlib.hpp"
#include <iosfwd>
#include "hua/tasks.hpp"
#include "hua/entry.hpp"
namespace hua {
class VirtualMachine {
public:
    VirtualMachine(const SemanticModel& model, std::ostream& output, RuntimeContext* context = nullptr) : model_(model), output_(output), context_(context) {}
    EntryResult run(const Bytecode& bytecode);
private:
    EntryResult entry_;
    Routine<void> run_body(const Bytecode& bytecode,ExecutionScope&);
    struct Binding { Value value; std::string type; bool mutable_binding{}; };
    struct Deferred {Callable callable;std::vector<Value> arguments;Instruction site;};
    struct Environment {
        std::shared_ptr<Environment> parent;
        std::unordered_map<std::string, Binding> bindings;
        std::vector<Deferred> deferred;
        friend void heap_edges(const Environment& e,const HeapVisitor& visit){
            visit(e.parent.get());for(const auto& [_,b]:e.bindings)heap_edges(b.value,visit);
            for(const auto& d:e.deferred){heap_edges(Value(d.callable),visit);for(const auto& a:d.arguments)heap_edges(a,visit);}
        }
        Binding& lookup(const std::string& name, const SourceSpan& span);
    };
    using Location = ValueLocation;
    using Entry = std::variant<Value, Location>;
    struct Iterator {
        bool range{}, inclusive{}, done{};
        std::int64_t current{}, end{}, step{}, index{};
        SliceValue slice;
        bool map{};
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
    RuntimeContext* context_{};
    const Bytecode* bytecode_{};
    std::shared_ptr<Environment> global_;
    std::vector<Entry> stack_;
    std::vector<Frame> frames_;
    std::size_t steps_{};
    std::optional<Value> completed_;
    Value pop(const SourceSpan& span);
    std::vector<Value> arguments(std::size_t count, const SourceSpan& span);
    void bind(const Instruction& op, Value value);
    Routine<void> call(const Callable& callable, const std::vector<Value>& args, const Instruction& site);
    Value isolated(const Value& value,unsigned depth=0);
    Routine<Value> launch(const Callable&,const std::vector<Value>&,const Instruction&,std::vector<std::int64_t> batch={},std::string element={},bool contextual=false);
    Routine<Value> parallel(const std::vector<Value>&,const Instruction&);
    Routine<void> dispatch(std::size_t stop_depth=0);
    Routine<void> drain(const std::shared_ptr<Environment>& env);
    Routine<void> cleanup_error();
    Routine<void> finish_return(Value value,const Instruction& op,bool early=false);
};
}
