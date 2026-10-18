#pragma once
#include "hua/bytecode.hpp"
#include "hua/stdlib.hpp"
#include <iosfwd>
#include "hua/tasks.hpp"
#include "hua/entry.hpp"
namespace hua {
// Optional diagnostics borrow values only for the duration of a boundary callback.
// They do not change bytecode, instruction budgets, or child Task executors.
struct VMCallObserver {
    virtual ~VMCallObserver() = default;
    virtual void enter(const Node*, const Node*, const std::vector<Value>&) = 0;
    virtual void leave(const Node*, const Value*) = 0;
};
class VirtualMachine {
public:
    VirtualMachine(const SemanticModel& model, std::ostream& output, RuntimeContext* context = nullptr, VMCallObserver* observer = nullptr) : observer_(observer), model_(model), output_(output), context_(context) {}
    EntryResult run(const Bytecode& bytecode);
private:
    friend struct VmSlotTestAccess;
    VMCallObserver* observer_{};
    EntryResult entry_;
    Routine<void> run_body(const Bytecode& bytecode,ExecutionScope&);
    struct Binding { Value value; std::string type; bool mutable_binding{}; };
    struct Deferred {Callable callable;std::vector<Value> arguments;Instruction site;};
    struct ScopeLayout {
        std::unordered_map<std::string,std::size_t> indices;
        std::vector<std::string> names;
        void add(const std::string& name) { if(!indices.contains(name)){indices.emplace(name,names.size());names.push_back(name);} }
    };
    struct Environment {
        std::shared_ptr<Environment> parent;
        std::unordered_map<std::string, Binding> bindings;
        std::vector<Deferred> deferred;
        const ScopeLayout* layout{};
        // Fixed-size storage: locations remain valid through later bindings and suspension.
        std::vector<std::optional<Binding>> slots;
        explicit Environment(const ScopeLayout* shape=nullptr):layout(shape),slots(shape?shape->names.size():0){}
        Binding* own(const std::string& name);
        bool insert(const std::string& name,Binding binding);
        void clear(){bindings.clear();for(auto& slot:slots)slot.reset();deferred.clear();}
        template<class F> void each(F&& visit) const {
            if(layout)for(std::size_t i=0;i<slots.size();++i)if(slots[i])visit(layout->names[i],*slots[i]);
            for(const auto& [name,binding]:bindings)visit(name,binding);
        }
        friend void heap_edges(const Environment& e,const HeapVisitor& visit){
            visit(e.parent.get());for(const auto& slot:e.slots)if(slot)heap_edges(slot->value,visit);
            for(const auto& [_,b]:e.bindings)heap_edges(b.value,visit);
            for(const auto& d:e.deferred){heap_edges(Value(d.callable),visit);for(const auto& a:d.arguments)heap_edges(a,visit);}
        }

    };
    using Location = ValueLocation;
    using Entry = std::variant<Value, Location>;
    struct Iterator {
        bool range{}, inclusive{}, done{};
        std::int64_t current{}, end{}, step{}, index{};
        SliceValue slice;
        bool map{};
    };
    static constexpr std::size_t no_slot=static_cast<std::size_t>(-1);
    struct NameRoute {
        struct Guard { const ScopeLayout* layout{};std::size_t shadow{no_slot}; };
        std::vector<Guard> guards;
        std::size_t slot{no_slot};
    };
    struct FieldAccess {
        std::shared_ptr<const RecordLayout> layout;std::size_t index{RecordLayout::missing};
        std::weak_ptr<StructData> owner;
        StructData* identity{};
        Value* value{};
        std::string type_owner;
        const std::string* type{};
    };
    struct AccessCache { NameRoute name;FieldAccess field;std::optional<std::vector<std::string>> multi_types; };
    struct CodeLayout {
        ScopeLayout root;
        std::map<std::size_t,ScopeLayout> scopes;
        std::vector<std::unique_ptr<AccessCache>> accesses;
    };
    struct Frame {
        const Code* code{};
        const Node* function{};
        std::size_t pc{}, base{};
        SourceSpan site;
        std::shared_ptr<Environment> environment;
        std::vector<Iterator> iterators;
        CodeLayout* layout{};
    };
    struct ParameterLayout { std::string name,type;bool mutable_view{}; };
    struct FunctionLayout { std::string name;std::vector<ParameterLayout> parameters;std::vector<std::string> returns;bool external{},unsafe{}; };
    std::unordered_map<const Node*,FunctionLayout> function_layouts_;
    std::unordered_map<std::string,std::unordered_map<std::string,std::string>> field_types_;
    const FunctionLayout& function_layout(const Node&);
    std::unordered_map<const Code*,std::unique_ptr<CodeLayout>> code_layouts_;
    ScopeLayout builtin_layout_;
    CodeLayout& code_layout(const Code&,const Node* function=nullptr);
    AccessCache& access(Frame&);
    Binding& lookup(Frame&,const Instruction&);
    Value* field(FieldAccess&,const StructValue&,const std::string&);
    const std::string& field_type(FieldAccess&,const std::string&,const std::string&,const SourceSpan&);
    const std::string& field_type(const std::string&,const std::string&,const SourceSpan&);
    std::unordered_map<std::string,std::shared_ptr<const EnumLayout>> enum_layouts_;
    std::unordered_map<std::string,std::shared_ptr<const RecordLayout>> record_layouts_;
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
