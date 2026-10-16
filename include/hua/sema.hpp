#pragma once
#include "hua/value.hpp"
#include "hua/sema_profile.hpp"
#include "hua/external.hpp"
#include <unordered_map>
#include <unordered_set>
#include <tuple>
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
    struct Origin {
        const Node* owner{};std::string path;bool readonly{},copied{};
        HUA_SEMA_MEMBER(origin)
        bool operator==(const Origin&) const=default;
        bool operator<(const Origin& x) const{if(owner!=x.owner)return std::less<const Node*>{}(owner,x.owner);return std::tie(path,readonly,copied)<std::tie(x.path,x.readonly,x.copied);}
    };
    struct Info { std::string type; bool writable{}, readonly_children{}; std::set<Origin> origins{};bool deep_owned{},record_copy{}; HUA_SEMA_MEMBER(info) bool operator==(const Info&) const = default; };
    using Views=std::unordered_map<std::string,Info>;
    struct ReturnFacts {Info value;Views views;std::vector<Info> positions;std::vector<Views> position_views;bool operator==(const ReturnFacts&) const=default;};
    struct Effects {bool unknown{};std::set<Origin> required;std::unordered_map<const Node*,Views> writes;bool operator==(const Effects&) const=default;};
    bool probing_{};const Node* defer_site_{};
    std::unordered_map<const Node*,Views> node_views_;
    std::unordered_map<const Node*,std::vector<Info>> node_positions_;
    std::unordered_map<const Node*,std::vector<Views>> node_position_views_;
    std::unordered_map<std::string,ReturnFacts> return_facts_;
    std::unordered_map<std::string,Effects> effects_;
    ReturnFacts pending_return_;bool saw_return_{};
    void capture_return(const Node&);
    void seed(const std::string&,const Node*,bool);
    std::unordered_map<const Node*,Views> actual_views_;
    static Info meet(Info,const Info&);
    static Views meet(Views,const Views&,const Info&,const Info&);
    Effects pending_effects_;
    std::unordered_set<const Node*> effect_roots_;
    void select_effect_roots();
    std::vector<std::unordered_map<const Node*,Views>> exit_views_;
    void capture_effect_exit();
    void finish_effects();
    std::unordered_map<const Node*,Info> actual_origins_;
    void bound(Views&,Info&) const;
    Info projected(Info,const std::string&) const;
    Views fields(Info,unsigned depth=0) const;
    Views field_shape(Info,unsigned) const;
    mutable std::unordered_map<std::string,Views> field_shapes_;
    void infer_field_capabilities(const Node&);
    void note_write(const Node&);
    void update_aliases(const Info&,const std::string&,Info,const Views&);
    Info instantiate(Info,const Node&) const;
    Views instantiate(Views,const Node&) const;
    void apply_effects(const Effects&,const Node&);
    void forget_unknown(const Node&);
    struct Symbol { std::string type; bool mutable_binding{}, writable{}; std::optional<Value> constant; std::string declared;
        bool readonly_children{};std::set<Origin> origins{};bool deep_owned{},record_copy{};const Node* declaration{};
        std::unordered_map<std::string,Info> views;
        HUA_SEMA_MEMBER(symbol)
        Symbol(std::string t={},bool mut=false,bool write=false,std::optional<Value> folded={}):type(std::move(t)),mutable_binding(mut),writable(write),constant(std::move(folded)),declared(type){}
    };
    bool record(std::string type) const;
    Info copied(Info value) const;
    std::pair<Symbol*,std::string> projection(const Node& node);
    void remember(const Node& destination,const Node& source,Info value);
    std::unordered_map<const Node*,Info> infos_;
    SemanticModel model_;
    std::vector<std::unordered_map<std::string, Symbol>> scopes_;
    const Node* current_function_{};
    Symbol& lookup(const std::string& name, const SourceSpan& span);
    void define(const std::string& name, Symbol symbol, const SourceSpan& span);
    void validate_type(const Node& node,bool allow_void=false);
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
