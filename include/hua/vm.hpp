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
// One register of the typed register executor (src/register_vm.cpp); its type is static.
union RegisterValue { std::int64_t i; double f; };
struct RegisterTranslator;
class VirtualMachine {
    friend struct RegisterTranslator;
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
        // Cleared activations that keep their lease, reused without touching the control block.
        // Declared after spare: destroying a lease here retires its environment into spare.
        std::vector<std::shared_ptr<Environment>> ready;
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
        void clear(){if(!bindings.empty())bindings.clear();for(auto& slot:slots)slot.reset();if(!deferred.empty())deferred.clear();}
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
    std::shared_ptr<Environment> activation(const ScopeLayout* shape){
        if(!frame_arena_)frame_arena_=std::make_shared<FrameArena>();
        if(shape){auto& ready=const_cast<ScopeLayout*>(shape)->ready;if(!ready.empty()){auto env=std::move(ready.back());ready.pop_back();++frame_arena_->work.reused;++frame_arena_->work.activations;return env;}}
        return frame_arena_->acquire(shape,frame_arena_);
    }
    // Gives back the frame's lease. Only an environment nothing else holds is parked with its
    // lease; a Location or suspended continuation keeps it alive through the ordinary deleter.
    void recycle_scope(std::shared_ptr<Environment> env);
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
    // Member call sites remember the method resolved for the last receiver type (privacy is
    // per call site, so a cached entry has already passed its check here).
    struct MethodRoute { std::string owner;const Node* method{};bool mutating{}; };
    struct AccessCache { NameRoute name;FieldAccess field;std::optional<std::vector<std::string>> multi_types;BuiltinRoute builtin;FunctionRoute function;MethodRoute method; };
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
        // Per Bind/IterNext: the scope the load-time layout places its names in, and their
        // slots (no_slot when not predicted); checked against the entered scope before use.
        struct BindSlot { const ScopeLayout* scope{};std::size_t slot{no_slot},key_slot{no_slot}; };
        std::vector<BindSlot> bind_slots;
        // V1 COW, per Load/Member/Index: 0 the struct it pushes is only read, 1 it starts or
        // continues a write path (prepare + pin), 2 a write path ending as a Call's callee.
        std::vector<std::uint8_t> write_mode;
    };
    void classify_writes(const Code&,CodeLayout&);
    struct MethodInfo { bool mutating{};std::uint64_t mut_params{}; };
    std::unordered_map<std::string,MethodInfo> method_info_;bool method_info_ready_{};
    const MethodInfo* method_info(const std::string& name);
    // Makes the struct body held by `slot` private to it (unsharing a shared body, applying a
    // pending read-only projection), pins it on `path`, and returns the handle to push.
    Value prepare_write(Value& slot,std::shared_ptr<PinPath> path);
    [[gnu::noinline]] bool load_for_write(Value& value);
    [[gnu::noinline]] bool member_for_write(FieldAccess& cache,const StructValue& object,const std::string& name);
    [[gnu::noinline]] bool index_for_write(const Value& value,const Value& index,const SourceSpan& span);
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
    struct FunctionLayout { std::string name;std::vector<ParameterLayout> parameters;std::vector<std::string> returns;bool external{},unsafe{};
        // model.mutating for this name: present (an entry exists) and its value.
        bool has_mutating{},mutating{}; };
    std::unordered_map<const Node*,FunctionLayout> function_layouts_;
    std::unordered_map<std::string,std::unordered_map<std::string,std::string>> field_types_;
    const FunctionLayout& function_layout(const Node&);
    std::unordered_map<const Code*,std::unique_ptr<CodeLayout>> code_layouts_;
    ScopeLayout builtin_layout_;
    CodeLayout& code_layout(const Code&,const Node* function=nullptr);
    AccessCache& access(Frame&);
    // Superinstruction tail: a bool on top followed by JumpFalse/JumpTrue branches now.
    void branch_on_bool(Frame&);
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
    // Typed register execution (src/register_vm.cpp). A function whose body translates runs
    // synchronously on registers: 8-byte scalars and Value registers for everything else;
    // anything else keeps the frame-based path above. A null entry marks a function that
    // does not translate.
    struct RegisterCode;
    std::unordered_map<const Node*,std::shared_ptr<RegisterCode>> register_codes_;
    std::vector<RegisterValue> register_file_;
    std::vector<Value> value_file_; // registers holding non-scalar values, windowed like register_file_
    std::size_t register_user_base_{},register_max_{},value_max_{};
    RegisterCode* register_code(const Node& function);
    bool register_call(const Callable&,const std::vector<Value>&,const Instruction&,const FunctionLayout&,std::size_t user_calls);
    std::int64_t run_registers(const RegisterCode&,RegisterValue* registers,Value* values,std::size_t depth,const SourceSpan& site,Value* result);
    [[gnu::noinline]] void register_value_op(const RegisterCode&,std::size_t at,RegisterValue*,Value*,std::vector<ValueLocation>&);
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
    void bind_named(const std::string& name,const std::string& declared,bool mutable_binding,const SourceSpan& span,Value value,std::optional<Binding>* slot=nullptr);
    void bind_slot(const ScopeLayout* scope,std::size_t index,const std::string& name,const std::string& declared,bool mutable_binding,const SourceSpan& span,Value value);
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
