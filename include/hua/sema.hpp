#pragma once
#include "hua/value.hpp"
#include "hua/external.hpp"
#include <unordered_map>
namespace hua {
struct SemanticModel {
    std::shared_ptr<ExternalRegistry> external;
    TypeRelations relations;
    std::unordered_map<std::string, const Node*> functions, structures;
    std::unordered_map<std::string, bool> mutating;
    std::unordered_map<std::string,bool> return_writable;
    std::unordered_map<const Node*,bool> nested;
    std::unordered_map<const Node*, std::string> types;
    std::unordered_map<const Node*, bool> writable;
    std::unordered_map<const Node*, Value> constants;
};
TypeRelations interface_relations(const SemanticModel& model);
class SemanticAnalyzer {
public:
    SemanticModel analyze(const Node& program, std::shared_ptr<ExternalRegistry> external = {});
private:
    struct Symbol { std::string type; bool mutable_binding{}, writable{}; std::optional<Value> constant; std::string declared;
        Symbol(std::string t={},bool mut=false,bool write=false,std::optional<Value> folded={}):type(std::move(t)),mutable_binding(mut),writable(write),constant(std::move(folded)),declared(type){}
    };
    struct Info { std::string type; bool writable{}; };
    SemanticModel model_;
    std::vector<std::unordered_map<std::string, Symbol>> scopes_;
    const Node* current_function_{};
    Symbol& lookup(const std::string& name, const SourceSpan& span);
    void define(const std::string& name, Symbol symbol, const SourceSpan& span);
    void validate_type(const Node& node);
    void statements(const Node& block, bool scope = true);
    void statement(const Node& node);
    Info expression(const Node& node);
    Info target(const Node& node);
    Value constant(const Node& node);
    void require(const std::string& expected, const Info& actual, const Node& node);
    void infer_mutating_methods();
    void infer_return_capabilities();
    void join_flow(const std::vector<std::unordered_map<std::string,Symbol>>& other);
    void invalidate_loop(const Node& body);
    std::vector<std::pair<Symbol*,std::string>> narrow(const Node& condition,bool truth);
    void restore_types(const std::vector<std::pair<Symbol*,std::string>>& previous);
};
}
