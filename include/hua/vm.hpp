#pragma once
#include "hua/bytecode.hpp"
#include "hua/stdlib.hpp"
#include "hua/callback.hpp"
#include <iosfwd>
#include "hua/tasks.hpp"
#include "hua/entry.hpp"
namespace hua {
struct BuiltinInfo;
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
    // Entry function name for run (hua test runs each @test function this way).
    void set_entry(std::string name){entry_name_=std::move(name);}
private:
    friend struct VmSlotTestAccess;
    VMCallObserver* observer_{};
    EntryResult entry_;
    Routine<void> run_body(const Bytecode& bytecode,ExecutionScope&);
    struct Binding { Value value; std::string type; bool mutable_binding{}; };
    struct Deferred {Callable callable;std::vector<Value> arguments;Instruction site;};
    struct Environment;
    struct ScopeLayout {
        std::unordered_map<std::string,std::size_t> indices;
        std::vector<std::string> names;
        // Retired activations of this shape, reused in O(1) by the frame arena.
        std::vector<std::unique_ptr<Environment>> spare;
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
    // A shared_ptr control block is the activation lease. The environment's
    // stable slots are recycled only by its final lease deleter; a Location or
    // suspended continuation therefore prevents reuse without special cases.
    struct FrameArena {
        struct Work {std::uint64_t activations{},created{},reused{},retired{},active{},peak{};};
        Work work;
        std::vector<std::unique_ptr<Environment>> free;
        FrameArena(){free.reserve(64);}
        std::shared_ptr<Environment> acquire(const ScopeLayout*,const std::shared_ptr<FrameArena>&);
        void retire(Environment*) noexcept;
    };
    std::shared_ptr<FrameArena> frame_arena_;
    std::shared_ptr<Environment> activation(const ScopeLayout* shape){if(!frame_arena_)frame_arena_=std::make_shared<FrameArena>();return frame_arena_->acquire(shape,frame_arena_);}
    using Location = ValueLocation;
    // A local variable's assignment target; type is read from the binding. No owner lease:
    // LocateName resolves through the frame's current environment chain, and the binding's
    // scope encloses the assignment, so that chain keeps it alive until Store/StoreMulti
    // consumes the entry (inner scopes entered by the right-hand side are left first; a late
    // binding only adds a child environment; Unwind/errors discard the entry unread).
    struct LocalRef { Binding* binding{}; };
    using Entry = std::variant<Value, Location, LocalRef>;
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
    // Call sites remember the resolved builtin; interned names make the key a pointer.
    struct BuiltinRoute { const std::string* name{};const BuiltinInfo* info{}; };
    struct FunctionLayout;struct CodeLayout;
    // Call sites remember the callee's layouts (all owned by stable VM/bytecode tables).
    struct FunctionRoute { const Node* function{};const FunctionLayout* layout{};const Code* code{};CodeLayout* slots{};std::vector<std::size_t> parameter_slots; };
    struct AccessCache { NameRoute name;FieldAccess field;std::optional<std::vector<std::string>> multi_types;BuiltinRoute builtin;FunctionRoute function; };
    struct CodeLayout {
        ScopeLayout root;
        std::map<std::size_t,ScopeLayout> scopes;
        std::vector<std::unique_ptr<AccessCache>> accesses;
        // Load-time fusion (bytecode unchanged): `!` directly followed by JumpFalse/JumpTrue.
        std::vector<bool> negated_jump;
        // Per instruction: the scope layout opened there (EnterScope/IterNext), and whether an
        // EnterScope declares nothing in a defer-free body, so no environment is needed.
        std::vector<ScopeLayout*> scope_at;
        std::vector<bool> empty_scope;
    };
    struct Frame {
        const Code* code{};
        const Node* function{};
        std::size_t pc{}, base{};
        SourceSpan site;
        std::shared_ptr<Environment> environment;
        std::vector<Iterator> iterators;
        CodeLayout* layout{};
        // Lexical scope depth and, for depths below 64, which scopes were entered without an
        // environment; LeaveScope/Unwind still pop the same number of logical scopes.
        std::uint32_t scope_depth{};
        std::uint64_t elided_scopes{};
        // Calls from dispatch point at the call instruction's span inside the loaded
        // bytecode (as stable as `code`) instead of copying its file path; other entries
        // keep the owned `site`. The callee layout is cached so returns skip the lookup.
        const SourceSpan* site_ref{};
        const FunctionLayout* callee{};
        const SourceSpan& site_span() const {return site_ref?*site_ref:site;}
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
    const Value* read_field(FieldAccess&,const StructValue&,const std::string&);
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
    std::string entry_name_{"main"};
    std::optional<Value> completed_;
    Value pop(const SourceSpan& span);
    std::vector<Value> arguments(std::size_t count, const SourceSpan& span);
    // Call argument vectors are recycled through a small pool. A lease (not one
    // shared buffer) keeps re-entrant calls, e.g. from external functions, safe.
    std::vector<std::vector<Value>> argument_pool_;
    struct ArgumentLease {
        VirtualMachine& vm; std::vector<Value> values;
        explicit ArgumentLease(VirtualMachine& owner):vm(owner){if(!vm.argument_pool_.empty()){values=std::move(vm.argument_pool_.back());vm.argument_pool_.pop_back();}}
        ~ArgumentLease(){values.clear();if(vm.argument_pool_.size()<16&&values.capacity()<=64)vm.argument_pool_.push_back(std::move(values));}
        ArgumentLease(const ArgumentLease&)=delete;ArgumentLease& operator=(const ArgumentLease&)=delete;
    };
    void pop_arguments(std::size_t count, const SourceSpan& span, std::vector<Value>& out);
    void bind(const Instruction& op, Value value);
    Routine<void> call(const Callable& callable, const std::vector<Value>& args, const Instruction& site);
    // Synchronous part of call(): user/external functions never suspend while entering.
    void enter_function(const Callable& callable, const std::vector<Value>& args, const Instruction& site, FunctionRoute* route=nullptr);
    void materialize_scope();
    void bind_named(const std::string& name,const std::string& declared,bool mutable_binding,const SourceSpan& span,Value value);
    // Return without deferred work anywhere in the returning frame: no coroutine needed.
    bool has_pending_defers(const Frame& frame) const;
    void complete_return(Value value,const Instruction& op,bool early);
    // Safe-point polling: steps are exact, cancellation/yield are checked every
    // poll_interval steps, and shared metrics are flushed in batches.
    static constexpr unsigned poll_interval=256;
    unsigned poll_countdown_{1};
    std::uint64_t pending_metric_steps_{};
    void flush_metrics(){if(pending_metric_steps_&&context_&&context_->execution_metrics)context_->execution_metrics->steps+=pending_metric_steps_;pending_metric_steps_=0;}
    Value isolated(const Value& value,unsigned depth=0);
    Routine<Value> launch(const Callable&,const std::vector<Value>&,const Instruction&,std::vector<std::int64_t> batch={},std::string element={},bool contextual=false);
    Routine<Value> parallel(const std::vector<Value>&,const Instruction&);
    // L0-1 host: callbacks re-enter this VM. The callback call site is only the
    // standard call's span: no contextual literal flags reach callback parameters.
    // `route` caches the callee layout across calls; from dispatch it is the standard
    // call site's own (otherwise unused) function route.
    struct Callbacks final:CallbackHost {
        // Callback frames outlive this host while an error unwinds, so they name
        // `stable` (the standard call's span in the loaded bytecode) or own a copy.
        VirtualMachine& vm;Instruction site;FunctionRoute own;FunctionRoute* route;const SourceSpan* stable;
        Callbacks(VirtualMachine& owner,const SourceSpan& span,FunctionRoute* cache=nullptr,const SourceSpan* bytecode_span=nullptr):vm(owner),route(cache?cache:&own),stable(bytecode_span){site.span=span;}
        Routine<Value> invoke(const Callable&,std::vector<Value>,const SourceSpan&) override;
    };
    Routine<void> dispatch(std::size_t stop_depth=0);
    Routine<void> drain(const std::shared_ptr<Environment>& env);
    Routine<void> cleanup_error();
    Routine<std::exception_ptr> cleanup_exit(std::exception_ptr exit);
    Routine<void> finish_return(Value value,const Instruction& op,bool early=false);
};
}
