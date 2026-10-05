#pragma once
#include "hua/sema.hpp"
#include <iosfwd>
namespace hua {
class Interpreter {
public:
    Interpreter(const SemanticModel& model, std::ostream& output);
    void run(const Node& program);
private:
    struct Binding { Value value; std::string type; bool mutable_binding{}; };
    struct Environment {
        Environment* parent{};
        std::unordered_map<std::string, Binding> bindings;
        Binding& lookup(const std::string& name, const SourceSpan& span);
    };
    struct Location { Value* value; std::string type; bool writable; std::shared_ptr<void> owner; };
    const SemanticModel& model_;
    std::ostream& output_;
    Environment builtins_, global_, *environment_{&global_};
    std::size_t steps_{};
    unsigned calls_{};
    void tick(const Node& node);
    void execute(const Node& node);
    void block(const Node& node, bool scope = true);
    Value evaluate(const Node& node);
    Location locate(const Node& node);
    Value call(const Callable& callable, const std::vector<Value>& args, const Node& site);
    Value builtin(const std::string& name, const std::vector<Value>& args, const Node& site);
    void bind(const std::string& name, Value value, std::string type, bool mutable_binding, const SourceSpan& span);
};
}
