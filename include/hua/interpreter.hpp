#pragma once
#include "hua/sema.hpp"
#include "hua/stdlib.hpp"
#include <iosfwd>
namespace hua {
class Interpreter {
public:
    Interpreter(const SemanticModel& model, std::ostream& output, RuntimeContext* context = nullptr);
    void run(const Node& program);
private:
    struct Binding { Value value; std::string type; bool mutable_binding{}; };
    struct Deferred { Callable callable;std::vector<Value> arguments;const Node* site; };
    struct Environment {
        Environment* parent{};
        std::unordered_map<std::string, Binding> bindings;
        std::vector<Deferred> deferred;
        Environment(Environment* outer=nullptr,std::unordered_map<std::string,Binding> initial={}):parent(outer),bindings(std::move(initial)){}
        friend void heap_edges(const Environment& e,const HeapVisitor& visit){
            for(const auto& [_,b]:e.bindings)heap_edges(b.value,visit);
            for(const auto& d:e.deferred){heap_edges(Value(d.callable),visit);for(const auto& a:d.arguments)heap_edges(a,visit);}
        }
        Binding& lookup(const std::string& name, const SourceSpan& span);
    };
    using Location = ValueLocation;
    const SemanticModel& model_;
    std::ostream& output_;
    RuntimeContext* context_{};
    Environment builtins_, global_, *environment_{&global_};
    std::size_t steps_{};
    unsigned calls_{};
    void tick(const Node& node);
    void drain(Environment& environment);
    Callable closure(const Node& function);
    void execute(const Node& node);
    void block(const Node& node, bool scope = true);
    Value evaluate(const Node& node);
    Location locate(const Node& node);
    Value call(const Callable& callable, const std::vector<Value>& args, const Node& site);
    Value isolated(const Value& value,unsigned depth=0);
    Value launch(const Callable&,const std::vector<Value>&,const Node&,std::vector<std::int64_t> batch={},std::string element={},bool contextual=false);
    Value parallel(const std::vector<Value>&,const Node&);
    Value builtin(const std::string& name, const std::vector<Value>& args, const Node& site);
    void bind(const std::string& name, Value value, std::string type, bool mutable_binding, const SourceSpan& span);
};
}
