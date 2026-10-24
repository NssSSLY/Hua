#include "hua/enum_value.hpp"
#include "hua/performance.hpp"
#include "hua/location.hpp"
#include <exception>
#include "hua/vm.hpp"
#include "hua/runtime.hpp"
#include "hua/tasks.hpp"
#include "hua/file.hpp"
#include <limits>
#include <unordered_set>
namespace hua {
#ifdef HUA_INTERNAL_PROFILE
static_assert(performance::op_count==static_cast<std::size_t>(Op::Defer)+1,"performance::op_names must follow Op");
#endif
namespace {
using N=NodeKind;using I=std::int64_t;
[[noreturn]] void invalid(const SourceSpan& s){runtime_error(s,"invalid VM instruction/stack state","E6001");}
bool writable(const Value& v){if(auto p=std::get_if<PlainRecord>(&v.data))return p->writable;if(auto p=std::get_if<RngValue>(&v.data))return p->writable;if(auto p=std::get_if<DequeValue>(&v.data))return p->writable;if(auto p=std::get_if<BufferValue>(&v.data))return p->writable;if(auto p=std::get_if<ListValue>(&v.data))return p->writable;if(auto p=std::get_if<SliceValue>(&v.data))return p->writable;if(auto p=std::get_if<StructValue>(&v.data))return p->writable;if(auto p=std::get_if<MapValue>(&v.data))return p->writable;if(auto p=std::get_if<ResultValue>(&v.data))return p->writable;return false;}

}
const VirtualMachine::FunctionLayout& VirtualMachine::function_layout(const Node& fn) {
    auto [entry,inserted]=function_layouts_.try_emplace(&fn);
    if(inserted){HUA_PROFILE_COUNT(function_metadata_builds);auto& layout=entry->second;layout.name=declared_name(fn);
        layout.external=fn.text.find(" external")!=std::string::npos;layout.unsafe=fn.text.find(" unsafe")!=std::string::npos;
        for(const auto& child:fn.children){HUA_PROFILE_COUNT(parameter_scan_nodes);
            if(child->kind==N::Parameter)layout.parameters.push_back({declared_name(*child),child->children.empty()?"":type_name(*child->children[0]),child->text.find(" mut")!=std::string::npos||(!child->children.empty()&&child->children[0]->kind==N::MutableType)});
            else if(child->kind==N::ReturnTypes)for(const auto& type:child->children)layout.returns.push_back(type_name(*type));
        }
    }
    return entry->second;
}
const std::string& VirtualMachine::field_type(const std::string& owner,const std::string& field,const SourceSpan& span) {
    HUA_PROFILE_COUNT(field_lookups);auto found=field_types_.find(owner);
    if(found==field_types_.end()){
        auto structure=model_.structures.find(owner);if(structure==model_.structures.end())invalid(span);
        std::unordered_map<std::string,std::string> fields;
        for(const auto& item:structure->second->children){HUA_PROFILE_COUNT(field_scan_nodes);fields.emplace(item->text,type_name(*item->children[0]));}
        found=field_types_.emplace(owner,std::move(fields)).first;
    }
    auto type=found->second.find(field);if(type==found->second.end())runtime_error(span,"unknown struct field","E4003");return type->second;
}
namespace {
// Activation leases allocate one shared_ptr control block per scope/call. Reuse
// those fixed-size blocks through a small per-thread free list; a block freed on
// another thread simply joins that thread's list. After the list is destroyed at
// thread exit, late releases go straight back to the global allocator.
// lease_state: 0 not created, 1 alive, 2 destroyed (a trivially destructible flag
// stays readable after the list itself is gone).
template<std::size_t Size> int& lease_state(){thread_local int state=0;return state;}
template<std::size_t Size> struct LeaseBlocks {
    std::vector<void*> free;
    LeaseBlocks(){free.reserve(256);lease_state<Size>()=1;} // capacity covers the cap: release never allocates
    ~LeaseBlocks(){lease_state<Size>()=2;for(auto* p:free)::operator delete(p);free.clear();}
};
template<std::size_t Size> LeaseBlocks<Size>& lease_blocks(){thread_local LeaseBlocks<Size> blocks;return blocks;}
// Binary fast path: same-type int/bool/string comparisons and int +/- give the
// same result as binary_value; every other case (and overflow, which must raise
// E4002 there) returns nullopt and takes the general path.
std::optional<Value> fast_binary(std::string_view op,const Value& left,const Value& right){
    const auto index=left.data.index();if(index!=right.data.index()||op.empty()||op.size()>2)return std::nullopt;
    const char c0=op[0],c1=op.size()==2?op[1]:'\0';
    const bool eq=c0=='='&&c1=='=',ne=c0=='!'&&c1=='=',lt=c0=='<'&&!c1,le=c0=='<'&&c1=='=',gt=c0=='>'&&!c1,ge=c0=='>'&&c1=='=';
    if(index==2){auto x=std::get<std::int64_t>(left.data),y=std::get<std::int64_t>(right.data);
        if(eq)return Value(x==y);if(ne)return Value(x!=y);if(lt)return Value(x<y);if(le)return Value(x<=y);if(gt)return Value(x>y);if(ge)return Value(x>=y);
        std::int64_t out;
        if(c0=='+'&&!c1&&!__builtin_add_overflow(x,y,&out))return Value(out);
        if(c0=='-'&&!c1&&!__builtin_sub_overflow(x,y,&out))return Value(out);
        if(c0=='*'&&!c1&&!__builtin_mul_overflow(x,y,&out))return Value(out);
        // Floor division/modulo exactly as binary_value; zero divisor and MIN/-1 keep their errors there.
        if(((c0=='%'&&!c1)||(c0=='/'&&c1=='/'))&&y!=0&&!(x==std::numeric_limits<std::int64_t>::min()&&y==-1)){
            std::int64_t q=x/y,r=x%y;if(r&&((r<0)!=(y<0))){--q;r+=y;}return Value(c0=='%'?r:q);}
        return std::nullopt;}
    if(index==1){auto x=std::get<bool>(left.data),y=std::get<bool>(right.data);if(eq)return Value(x==y);if(ne)return Value(x!=y);return std::nullopt;}
    if(index==4){const auto& x=std::get<std::string>(left.data);const auto& y=std::get<std::string>(right.data);
        if(eq)return Value(x==y);if(ne)return Value(x!=y);if(lt)return Value(x<y);if(le)return Value(x<=y);if(gt)return Value(x>y);if(ge)return Value(x>=y);}
    return std::nullopt;
}
// In-place form of fast_binary's int/int cases: writes the same result into `left`
// without building a temporary optional<Value>; false leaves `left` untouched.
inline bool int_binary_into(std::string_view op,Value& left,const Value& right){
    auto* xp=std::get_if<std::int64_t>(&left.data);auto* yp=std::get_if<std::int64_t>(&right.data);
    if(!xp||!yp||op.empty()||op.size()>2)return false;
    const auto x=*xp,y=*yp;const char c0=op[0],c1=op.size()==2?op[1]:'\0';std::int64_t out;
    switch(c0){
    case '+':if(c1||__builtin_add_overflow(x,y,&out))return false;*xp=out;return true;
    case '-':if(c1||__builtin_sub_overflow(x,y,&out))return false;*xp=out;return true;
    case '*':if(c1||__builtin_mul_overflow(x,y,&out))return false;*xp=out;return true;
    case '<':if(c1&&c1!='=')return false;left.data.emplace<bool>(c1?x<=y:x<y);return true;
    case '>':if(c1&&c1!='=')return false;left.data.emplace<bool>(c1?x>=y:x>y);return true;
    case '=':if(c1!='=')return false;left.data.emplace<bool>(x==y);return true;
    case '!':if(c1!='=')return false;left.data.emplace<bool>(x!=y);return true;
    case '%':case '/':
        if((c0=='%'&&c1)||(c0=='/'&&c1!='/')||y==0||(x==std::numeric_limits<std::int64_t>::min()&&y==-1))return false;
        {std::int64_t q=x/y,rem=x%y;if(rem&&((rem<0)!=(y<0))){--q;rem+=y;}*xp=c0=='%'?rem:q;}return true;
    default:return false;
    }
}
// Scalar constants/loads build the stack entry directly instead of a generic Value copy.
template<class Stack> inline bool push_scalar(Stack& stack,const Value& value){
    switch(value.data.index()){
    case 1:stack.emplace_back(std::in_place_type<Value>,std::get<bool>(value.data));return true;
    case 2:stack.emplace_back(std::in_place_type<Value>,std::get<std::int64_t>(value.data));return true;
    case 3:stack.emplace_back(std::in_place_type<Value>,std::get<double>(value.data));return true;
    default:return false;
    }
}
// Inline copy of file_pump_pending (tasks.cpp) for the per-step check.
inline bool pump_pending(const RuntimeContext* c) noexcept{return c&&(!c->resumable||!c->in_task)&&c->files&&c->files->created->load(std::memory_order_acquire);}
template<class T> struct LeaseAllocator {
    using value_type=T;
    LeaseAllocator()=default;
    template<class U> LeaseAllocator(const LeaseAllocator<U>&) noexcept {}
    T* allocate(std::size_t n){
        if(n==1&&lease_state<sizeof(T)>()!=2){auto& blocks=lease_blocks<sizeof(T)>();
            if(!blocks.free.empty()){auto* p=blocks.free.back();blocks.free.pop_back();return static_cast<T*>(p);}}
        return static_cast<T*>(::operator new(n*sizeof(T)));
    }
    void deallocate(T* p,std::size_t n) noexcept {
        if(n==1&&lease_state<sizeof(T)>()==1){auto& blocks=lease_blocks<sizeof(T)>();
            if(blocks.free.size()<256){blocks.free.push_back(p);return;}}
        ::operator delete(p);
    }
    template<class U> bool operator==(const LeaseAllocator<U>&) const noexcept {return true;}
};
}
std::shared_ptr<VirtualMachine::Environment> VirtualMachine::FrameArena::acquire(const ScopeLayout* shape,const std::shared_ptr<FrameArena>& arena){
    std::unique_ptr<Environment> item;
    if(shape){auto& spare=const_cast<ScopeLayout*>(shape)->spare;if(!spare.empty()){item=std::move(spare.back());spare.pop_back();++work.reused;}}
    else for(auto i=free.size();i>0;--i)if(free[i-1]->layout==shape){
        item=std::move(free[i-1]);free.erase(free.begin()+static_cast<std::ptrdiff_t>(i-1));++work.reused;break;
    }
    if(!item){item=std::make_unique<Environment>(shape);++work.created;}
    // Transfer ownership before constructing the lease: shared_ptr invokes its
    // deleter if allocating the control block fails.
    auto* pointer=item.release();++work.activations;++work.active;work.peak=std::max(work.peak,work.active);
    return std::shared_ptr<Environment>(pointer,[weak=std::weak_ptr<FrameArena>(arena)](Environment* environment) noexcept {
        if(auto owner=weak.lock())owner->retire(environment);else delete environment;
    },LeaseAllocator<Environment>{});
}
void VirtualMachine::FrameArena::retire(Environment* environment) noexcept {
    // No Value, parent, deferred argument or resource is retained in the cache.
    // Closures never capture these activation environments: Closure snapshots
    // into a separately managed Environment before exposing the function value.
    environment->clear();environment->parent.reset();++work.retired;--work.active;
    std::unique_ptr<Environment> item(environment);
    // Bounded empty capacity, not a language quota. Large/dynamic frames simply
    // fall back to ordinary destruction after their final lease disappears.
    if(item->slots.capacity()<=256 && item->bindings.bucket_count()<=256 && item->deferred.capacity()<=32){
        // Keep recently retired storage, rather than permanently pinning the
        // first 64 shapes and starving every later hot activation.
        if(item->layout){auto& spare=const_cast<ScopeLayout*>(item->layout)->spare;
            if(spare.size()<16){spare.push_back(std::move(item));return;} // per-shape cap; overflow is destroyed
            return;}
        if(free.size()==64)free.erase(free.begin());
        free.push_back(std::move(item)); // constructor reserved all 64 entries
    }
}
VirtualMachine::Binding* VirtualMachine::Environment::own(const std::string& name) {
    if(layout){auto found=layout->indices.find(name);if(found!=layout->indices.end()&&slots[found->second])return &*slots[found->second];}
    auto found=bindings.find(name);return found==bindings.end()?nullptr:&found->second;
}
bool VirtualMachine::Environment::insert(const std::string& name,Binding binding) {
    if(layout){auto found=layout->indices.find(name);if(found!=layout->indices.end()){
        auto& slot=slots[found->second];if(slot)return false;slot.emplace(std::move(binding));HUA_PROFILE_COUNT(vm_slot_writes);return true;
    }}
    return bindings.emplace(name,std::move(binding)).second;
}
VirtualMachine::CodeLayout& VirtualMachine::code_layout(const Code& code,const Node* function) {
    auto [where,inserted]=code_layouts_.try_emplace(&code);
    if(!inserted)return *where->second;
    where->second=std::make_unique<CodeLayout>();auto& result=*where->second;result.accesses.resize(code.instructions.size());
    result.negated_jump.resize(code.instructions.size());
    for(std::size_t pc=0;pc+1<code.instructions.size();++pc){const auto& op=code.instructions[pc];const auto next=code.instructions[pc+1].op;
        result.negated_jump[pc]=op.op==Op::Unary&&op.text=="!"&&(next==Op::JumpFalse||next==Op::JumpTrue);}
    HUA_PROFILE_COUNT(vm_code_layouts);HUA_PROFILE_ADD(vm_access_index_bytes,result.accesses.size()*sizeof(std::unique_ptr<AccessCache>));
    if(function){const auto& f=function_layout(*function);for(const auto& p:f.parameters)result.root.add(p.name);
        if(model_.nested.contains(function))result.root.add(f.name);
        if(f.name.find('.')!=std::string::npos)result.root.add("self");
    }
    // Source compilers emit balanced lexical scopes even across early Unwind/Return.
    // For other validated control flow this is only a storage hint: missing names
    // use the dictionary, and access routes guard every actually entered scope.
    std::vector<ScopeLayout*> scopes{&result.root};
    for(std::size_t pc=0;pc<code.instructions.size();++pc){const auto& op=code.instructions[pc];
        if(op.op==Op::EnterScope||op.op==Op::IterNext){auto& scope=result.scopes[pc];scopes.push_back(&scope);}
        if(op.op==Op::Bind)scopes.back()->add(op.text);
        else if(op.op==Op::BindMulti||op.op==Op::IterNext)for(const auto& name:op.names)if(name!="_")scopes.back()->add(name);
        if(op.op==Op::LeaveScope&&scopes.size()>1)scopes.pop_back();
    }
    result.scope_at.assign(code.instructions.size(),nullptr);result.empty_scope.assign(code.instructions.size(),false);
    bool has_defer=false;for(const auto& op:code.instructions)has_defer=has_defer||op.op==Op::Defer;
    for(auto& [pc,scope]:result.scopes){result.scope_at[pc]=&scope;
        result.empty_scope[pc]=!has_defer&&code.instructions[pc].op==Op::EnterScope&&scope.names.empty();}
    return result;
}
VirtualMachine::AccessCache& VirtualMachine::access(Frame& frame) {
    auto& item=frame.layout->accesses.at(frame.pc-1);
    if(!item){item=std::make_unique<AccessCache>();HUA_PROFILE_COUNT(vm_access_caches);}
    return *item;
}
VirtualMachine::Binding& VirtualMachine::lookup(Frame& frame,const Instruction& op) {
    HUA_PROFILE_COUNT(environment_lookups);auto& route=access(frame).name;
    if(route.slot!=no_slot){auto* env=frame.environment.get();bool valid=true;
        for(std::size_t i=0;i<route.guards.size();++i){const auto& guard=route.guards[i];HUA_PROFILE_COUNT(vm_slot_guards);
            if(!env||env->layout!=guard.layout||!env->bindings.empty()){valid=false;break;}
            if(i+1==route.guards.size()){
                if(route.slot<env->slots.size()&&env->slots[route.slot]){HUA_PROFILE_COUNT(vm_slot_hits);return *env->slots[route.slot];}
                valid=false;break;
            }
            // A late binding in an intervening scope must shadow an earlier route.
            if(guard.shadow!=no_slot&&env->slots[guard.shadow]){valid=false;break;}
            env=env->parent.get();
        }
        (void)valid;
    }
    HUA_PROFILE_COUNT(vm_slot_misses);std::vector<NameRoute::Guard> guards;bool cacheable=true;
    for(auto* env=frame.environment.get();env;env=env->parent.get()){
        HUA_PROFILE_COUNT(environment_probes);std::size_t slot=no_slot;
        if(env->layout){HUA_PROFILE_COUNT(vm_lookup_hashes);auto item=env->layout->indices.find(op.text);if(item!=env->layout->indices.end())slot=item->second;}
        cacheable=cacheable&&env->layout&&env->bindings.empty();
        guards.push_back({env->layout,slot});
        if(slot!=no_slot&&env->slots[slot]){
            if(cacheable){route.guards=std::move(guards);route.slot=slot;}
            return *env->slots[slot];
        }
        if(!env->bindings.empty()){HUA_PROFILE_COUNT(vm_lookup_hashes);auto found=env->bindings.find(op.text);if(found!=env->bindings.end())return found->second;}
    }
    runtime_error(op.span,"name `"+op.text+"` is not initialized","E4001");
}
Value* VirtualMachine::field(FieldAccess& cache,const StructValue& object,const std::string& name) {
    // Retain the shape too: Task results may carry a layout from another VM.
    // An index never points into another instance or aliases an initialized nil.
    if(auto* layout=object.data->fields.layout()){
        if(cache.layout.get()==layout){HUA_PROFILE_COUNT(vm_field_hits);return object.data->fields.value_at(cache.index);}
        HUA_PROFILE_COUNT(vm_field_misses);cache.layout=object.data->fields.retain_layout();cache.index=layout->index(name);
        return object.data->fields.value_at(cache.index);
    }
    // Ordinary record fields never erase nodes after construction. Enum tag
    // initialization can erase inactive fields, so enum accesses always resolve.
    if(!object.data->enumeration&&cache.identity==object.data.get()&&!cache.owner.expired()){
        HUA_PROFILE_COUNT(vm_field_hits);return cache.value;
    }
    HUA_PROFILE_COUNT(vm_field_misses);auto found=object.data->fields.find(name);
    if(found==object.data->fields.end())return nullptr;
    if(!object.data->enumeration){cache.owner=object.data;cache.identity=object.data.get();cache.value=&found->second;}
    return &found->second;
}
const std::string& VirtualMachine::field_type(FieldAccess& cache,const std::string& owner,const std::string& name,const SourceSpan& span){
    if(cache.type&&cache.type_owner==owner){HUA_PROFILE_COUNT(vm_field_type_hits);return *cache.type;}
    cache.type=&field_type(owner,name,span);cache.type_owner=owner;return *cache.type;
}
Value VirtualMachine::pop(const SourceSpan& span) {
    if(stack_.empty() || (!frames_.empty() && stack_.size()<=frames_.back().base) || !std::holds_alternative<Value>(stack_.back()))invalid(span);
    auto value=std::move(std::get<Value>(stack_.back()));stack_.pop_back();
    if(auto object=std::get_if<StructValue>(&value.data))require_complete_enum(*object->data,span);
    return value;
}
std::vector<Value> VirtualMachine::arguments(std::size_t count,const SourceSpan& span) {
    if(count>stack_.size() || (!frames_.empty() && count>stack_.size()-frames_.back().base))invalid(span);std::vector<Value> args(count);
    for(std::size_t i=count;i>0;--i)args[i-1]=pop(span);return args;
}
void VirtualMachine::pop_arguments(std::size_t count,const SourceSpan& span,std::vector<Value>& out) {
    if(count>stack_.size() || (!frames_.empty() && count>stack_.size()-frames_.back().base))invalid(span);out.resize(count);
    for(std::size_t i=count;i>0;--i)out[i-1]=pop(span);
}
void VirtualMachine::bind(const Instruction& op,Value value) {bind_named(op.text,op.type,op.flag,op.span,std::move(value));}
void VirtualMachine::materialize_scope() {
    // A binding the load-time layout did not predict lands in an elided scope: give it a real environment now.
    auto& f=frames_.back();if(f.scope_depth==0||f.scope_depth>64)return;auto bit=std::uint64_t{1}<<(f.scope_depth-1);
    if(!(f.elided_scopes&bit))return;f.elided_scopes&=~bit;auto env=activation(nullptr);env->parent=f.environment;f.environment=std::move(env);
}
void VirtualMachine::bind_named(const std::string& name,const std::string& declared,bool mutable_binding,const SourceSpan& span,Value value) {
    materialize_scope();
    if(std::holds_alternative<MultiValue>(value.data))runtime_error(span,"multiple values require positional binding","E4003");
    auto type=declared.empty()?value_type(value):declared;value=copy_value(std::move(value));
    if(!mutable_binding)value=read_only(std::move(value));
    if(!frames_.back().environment->insert(name,Binding{std::move(value),type,mutable_binding}))
        runtime_error(span,"duplicate runtime binding `"+name+"`","E4001");
}
EntryResult VirtualMachine::run(const Bytecode& bytecode){
    ExecutionScope execution(context_,output_);context_->type_relations=&model_.relations;context_->resumable=true;
    sync_wait(run_body(bytecode,execution),*context_);return std::move(entry_);
}
Routine<void> VirtualMachine::run_body(const Bytecode& bytecode,ExecutionScope& execution){
    if(model_.external)model_.external->reset();
    bytecode_=&bytecode;steps_=0;frames_.clear();stack_.clear();function_layouts_.clear();field_types_.clear();enum_layouts_.clear();record_layouts_.clear();code_layouts_.clear();builtin_layout_={};
    frame_arena_=std::make_shared<FrameArena>();
    auto& initial_layout=code_layout(bytecode.initializer);
    for(const auto& name:builtin_names()){builtin_layout_.add(name);builtin_layout_.add(std::string("$core$")+name);}
    for(const auto& [name,fn]:model_.functions)if(!model_.nested.contains(fn))initial_layout.root.add(name);
    auto builtins=managed<Environment>(&builtin_layout_);global_=managed<Environment>(&initial_layout.root);global_->parent=builtins;
    for(const auto& name:builtin_names()){
        builtins->insert(name,Binding{Value(Callable{nullptr,name,{}}),std::string("builtin:")+name,false});
        builtins->insert(std::string("$core$")+name,Binding{Value(Callable{nullptr,name,{}}),std::string("builtin:")+name,false});
    }
    for(const auto& [name,fn]:model_.functions)if(!model_.nested.contains(fn))global_->insert(name,Binding{Value(Callable{fn,"",{}}),"fn:"+name,false});
    std::exception_ptr error;Value returned;SourceSpan entry_span;
    try{
        frames_.push_back({&bytecode.initializer,nullptr,0,0,{},global_,{},&initial_layout});(co_await dispatch());
        if(auto main=model_.functions.find("main");main!=model_.functions.end()){
            entry_span=main->second->span;completed_.reset();
            Instruction site{};site.span=entry_span;(co_await call(Callable{main->second,"",{}},{},site));if(frames_.empty())stack_.clear();else (co_await dispatch());
            if(completed_)returned=std::move(*completed_);
            if(std::holds_alternative<TaskValue>(returned.data))returned=co_await context_->tasks->await_async(returned,entry_span);
        }
    }catch(...){error=std::current_exception();}
    if(error)(co_await cleanup_error());
    try{co_await execution.finish_async(bool(error));}catch(...){if(!error)error=std::current_exception();}
    global_->clear();stack_.clear();frames_.clear();completed_.reset();if(error)std::rethrow_exception(error);
    entry_=entry_result(returned,entry_span);co_return;
}
Routine<void> VirtualMachine::call(const Callable& callable,const std::vector<Value>& args,const Instruction& site) {
    if(callable.builtin=="task_start"){if(args.empty()||!std::holds_alternative<Callable>(args[0].data))runtime_error(site.span,"spawn requires a Hua function","E4104");auto nested=site;if(!nested.contextual.empty())nested.contextual.erase(nested.contextual.begin());stack_.emplace_back(co_await launch(std::get<Callable>(args[0].data),std::vector<Value>(args.begin()+1,args.end()),nested));co_return;}
    if(callable.builtin=="parallel_map"){stack_.emplace_back(co_await parallel(args,site));co_return;}
    if(!callable.builtin.empty()){if(const auto& info=builtin_info(callable.builtin);info.standard&&info.standard->callback){Callbacks host(*this,site.span);stack_.emplace_back(co_await invoke_callback_standard(*info.standard,args,site.span,host,context_));co_return;}}
    if(!callable.builtin.empty()){HUA_PROFILE_COUNT(builtin_calls);HUA_PROFILE_BUILTIN(callable.builtin);stack_.emplace_back(co_await invoke_resumable(callable.builtin,args,site.span,output_,site.contextual,site.type,context_));co_return;}
    enter_function(callable,args,site);
    co_return;
}
Routine<Value> VirtualMachine::Callbacks::invoke(const Callable& callable,std::vector<Value> args,const SourceSpan&){
    // Same re-entry as a task wrapper: push the callee frame, run it to its return.
    auto depth=vm.frames_.size();auto base=vm.stack_.size();
    vm.enter_function(callable,args,site,route);
    if(vm.frames_.size()>depth&&vm.frames_.back().site_ref==&site.span){auto& frame=vm.frames_.back();if(stable)frame.site_ref=stable;else{frame.site=site.span;frame.site_ref=nullptr;}}
    co_await vm.dispatch(depth);
    if(vm.stack_.size()!=base+1)runtime_error(site.span,"invalid callback return stack","E6001");
    co_return vm.pop(site.span);
}
void VirtualMachine::enter_function(const Callable& callable,const std::vector<Value>& args,const Instruction& site,FunctionRoute* route) {
    if(!callable.function)invalid(site.span);
    HUA_PROFILE_COUNT(user_calls);
    const auto& fn=*callable.function;
    if(route&&route->function!=&fn)*route={&fn,&function_layout(fn),nullptr,nullptr};
    const auto& layout=route?*route->layout:function_layout(fn);const auto& key=layout.name;
    if(layout.external){if(context_&&context_->in_task)runtime_error(site.span,"Native/WASM calls cannot run in tasks","E4104");stack_.emplace_back(model_.external->call(key,args,site.contextual,site.span));return;}
    if(layout.unsafe)runtime_error(site.span,"unsafe function execution/FFI is not implemented","E4008");
    const auto user_calls=frames_.size()-(!frames_.empty() && !frames_.front().function?1:0);
    if(user_calls>=context_->execution_limits.calls)runtime_error(site.span,"function call depth limit exceeded ("+std::to_string(context_->execution_limits.calls)+")","E4099");
    if(context_->execution_metrics)context_->execution_metrics->call(static_cast<unsigned>(user_calls+1));
    const auto& ps=layout.parameters;
    if(ps.size()!=args.size())runtime_error(site.span,"function argument count mismatch","E4003");
    if(context_&&context_->in_task&&callable.closure)std::static_pointer_cast<Environment>(callable.closure)->parent=global_;
    const Code* body=route?route->code:nullptr;CodeLayout* slots=route?route->slots:nullptr;
    if(!body){auto code=bytecode_->functions.find(&fn);if(code!=bytecode_->functions.end()){body=&code->second;slots=&code_layout(code->second,&fn);if(route){route->code=body;route->slots=slots;}}}
    if(route&&slots&&route->parameter_slots.size()!=layout.parameters.size()){
        route->parameter_slots.clear();
        for(const auto& p:layout.parameters){auto found=slots->root.indices.find(p.name);route->parameter_slots.push_back(found==slots->root.indices.end()?no_slot:found->second);}
    }
    auto local=activation(slots?&slots->root:nullptr);local->parent=callable.closure?std::static_pointer_cast<Environment>(callable.closure):global_;
    if(callable.closure)local->insert(key,Binding{Value(callable),"fn:"+key,false});
    if(callable.receiver) {
        if(model_.mutating.at(key) && !callable.receiver->writable)runtime_error(site.span,"mutating method requires mutable receiver","E4007");
        local->insert("self",Binding{Value(*callable.receiver),callable.receiver->data->name,false});
    }
    for(std::size_t i=0;i<ps.size();++i) {
        const auto& p=ps[i];const auto& type=p.type;bool mut=p.mutable_view;
        if(mut && !writable(args[i]))runtime_error(site.span,"mut parameter requires mutable argument","E4007");
        HUA_PROFILE_CONSTRAINT_SOURCE(argument);
        auto v=mut?enforce_borrow(args[i],type,site.span):enforce_type(copy_value(args[i]),type,site.span,i<site.contextual.size()&&site.contextual[i]);
        if(!mut)v=read_only(std::move(v));
        // Same effect as insert(): parameter slots of the function's root scope, cached per call site.
        const auto slot=route&&slots&&i<route->parameter_slots.size()?route->parameter_slots[i]:no_slot;
        if(slot!=no_slot&&slot<local->slots.size()&&!local->slots[slot])local->slots[slot].emplace(Binding{std::move(v),type,false});
        else local->insert(p.name,Binding{std::move(v),type,false});
    }
    if(!body)invalid(site.span);
    if(observer_){flush_metrics();observer_->enter(&fn,frames_.empty()?nullptr:frames_.back().function,args);}
    // A route exists only for Call from dispatch, whose site lives in the loaded bytecode.
    if(route){frames_.push_back({body,&fn,0,stack_.size(),{},std::move(local),{},slots});frames_.back().site_ref=&site.span;}
    else frames_.push_back({body,&fn,0,stack_.size(),site.span,std::move(local),{},slots});
    frames_.back().callee=&layout;
}
bool VirtualMachine::has_pending_defers(const Frame& f) const {
    for(auto* env=f.environment.get();env&&env!=global_.get();env=env->parent.get())if(!env->deferred.empty())return true;
    return !f.function&&!global_->deferred.empty();
}
Routine<void> VirtualMachine::finish_return(Value value,const Instruction& op,bool early) {
    // Save the language value before deferred calls can mutate its source.
    value=copy_value(std::move(value));
    auto f=frames_.back();
    for(auto env=f.environment;env&&env!=global_;env=env->parent)(co_await drain(env));if(!f.function)(co_await drain(global_));
    complete_return(std::move(value),op,early);
    co_return;
}
// Shared tail of finish_return: the caller has already copied the value and drained
// every deferred call of the returning frame (or proved there were none).
void VirtualMachine::complete_return(Value value,const Instruction& op,bool early) {
    auto& top=frames_.back();const Node* function=top.function;
    if(early&&!function)invalid(op.span);if(function)value=enforce_return(std::move(value),(top.callee?*top.callee:function_layout(*function)).returns,top.site_span(),op.flag);
    auto base=top.base;if(!early&&stack_.size()!=base)invalid(op.span);
    auto environment=std::move(top.environment); // released after the frame is popped, as before
    frames_.pop_back();stack_.resize(base);// enforce_return already makes the language value copy at the function boundary.
    if(!function)value=copy_value(value);
    if(!frames_.empty())stack_.emplace_back(std::move(value));else completed_=std::move(value);
    if(observer_){flush_metrics();observer_->leave(function,frames_.empty()?(completed_?&*completed_:nullptr):std::get_if<Value>(&stack_.back()));}
}
Routine<void> VirtualMachine::drain(const std::shared_ptr<Environment>& env){
    struct Cleanup {bool& flag;bool previous;Cleanup(bool& f):flag(f),previous(f){flag=true;}~Cleanup(){flag=previous;}} cleanup(context_->cleanup);
    auto deferred=std::move(env->deferred);env->deferred.clear();std::exception_ptr first;
    for(auto i=deferred.rbegin();i!=deferred.rend();++i){
        auto depth=frames_.size(),base=stack_.size();bool failed=false;
        try{(co_await call(i->callable,i->arguments,i->site));(co_await dispatch(depth));}catch(...){if(!first)first=std::current_exception();failed=true;}
        if(failed)while(frames_.size()>depth){
            auto environment=frames_.back().environment;
            for(auto e=environment;e&&e!=env&&e!=global_;e=e->parent)try{(co_await drain(e));}catch(...){}
            frames_.pop_back();
        }
        stack_.resize(base);
    }
    if(first)std::rethrow_exception(first);
    co_return;
}
Routine<void> VirtualMachine::cleanup_error(){
    struct Unwinding {bool& flag;bool old;Unwinding(bool& f):flag(f),old(f){flag=true;}~Unwinding(){flag=old;}} failed(context_->unwinding);
    std::unordered_set<Environment*> seen;auto frames=frames_;
    for(auto i=frames.rbegin();i!=frames.rend();++i)for(auto env=i->environment;env;env=env->parent)if(seen.insert(env.get()).second)try{(co_await drain(env));}catch(...){}
    co_return;
}
Routine<void> VirtualMachine::dispatch(std::size_t stop_depth) {
    struct Flush {VirtualMachine& vm;~Flush(){vm.flush_metrics();}} flush{*this};
    poll_countdown_=1;
    while(frames_.size()>stop_depth) {
        auto& f=frames_.back();if(f.pc>=f.code->instructions.size())invalid(f.site_span());
        const auto& op=f.code->instructions[f.pc++];const auto& s=op.span;
        HUA_PROFILE_COUNT(vm_steps);HUA_PROFILE_OP(op.op);
        // Exact per-step work: budget and pending File pumps. Cancellation and the
        // cooperative yield are polled every poll_interval steps (and on the first
        // step after entering dispatch or resuming from a suspension).
        if(--poll_countdown_==0){poll_countdown_=poll_interval;flush_metrics();co_await Yield{false,poll_interval};cancellation_checkpoint(context_,s);}
        if(pump_pending(context_))file_checkpoint(context_);
        ++pending_metric_steps_;if(++steps_>context_->execution_limits.steps){flush_metrics();runtime_error(s,"execution step limit exceeded ("+std::to_string(context_->execution_limits.steps)+")","E4099");}
        try{
        switch(op.op) {
        case Op::Closure:{auto fn=model_.functions.find(op.text);if(fn==model_.functions.end())invalid(s);auto captured=managed<Environment>();captured->parent=global_;for(auto env=f.environment;env&&env!=global_;env=env->parent)env->each([&](const auto& name,const auto& binding){if(!captured->bindings.contains(name))captured->bindings.emplace(name,Binding{read_only(copy_value(binding.value)),binding.type,false});});Callable callable{fn->second,"",{},captured};std::weak_ptr<Environment> weak=captured;callable.resource_guard=[weak](unsigned d){if(auto env=weak.lock())for(const auto& [_,b]:env->bindings)if(contains_file(b.value,d+1))return true;return false;};stack_.emplace_back(Value(std::move(callable)));break;}
        case Op::Defer:{auto args=arguments(op.argument,s);auto value=pop(s);auto fn=std::get_if<Callable>(&value.data);if(!fn)runtime_error(s,"defer requires a callable","E4003");for(auto& arg:args)arg=copy_value(arg);f.environment->deferred.push_back({*fn,std::move(args),op});break;}
        case Op::ExternalInit:if(context_&&context_->in_task)runtime_error(s,"Native/WASM initialization cannot run in tasks","E4104");model_.external->initialize(op.text,s);break;
        case Op::Constant:HUA_PROFILE_KIND(constant_kinds,op.constant);if(!push_scalar(stack_,op.constant))stack_.emplace_back(op.constant);break;
        case Op::Load:{auto& value=lookup(f,op).value;HUA_PROFILE_KIND(load_kinds,value);if(push_scalar(stack_,value))break;if(auto p=std::get_if<PlainRecord>(&value.data);p&&p->writable)materialize_record(value);stack_.emplace_back(value);break;}
        case Op::Bind:{HUA_PROFILE_CONSTRAINT_SOURCE(binding);auto v=enforce_type(pop(s),op.type,s,!op.contextual.empty()&&op.contextual[0]);bind(op,std::move(v));break;}
        case Op::Pop:
            // Only struct values need pop()'s enum completeness check.
            if(stack_.size()>f.base)if(auto* v=std::get_if<Value>(&stack_.back());v&&!std::holds_alternative<StructValue>(v->data)){stack_.pop_back();break;}
            pop(s);break;
        case Op::Unary:{
            // `!` on a bool is the hottest unary form; anything else takes the general path.
            if(op.text=="!"&&stack_.size()>f.base)if(auto* v=std::get_if<Value>(&stack_.back()))if(auto* b=std::get_if<bool>(&v->data)){
                // Fused `!` + conditional jump. The jump is still one counted step; fuse only
                // when that step could not yield, pump files or exceed the step limit.
                if(f.layout&&f.layout->negated_jump[f.pc-1]&&poll_countdown_>1&&!pump_pending(context_)&&steps_<context_->execution_limits.steps){
                    const auto& jump=f.code->instructions[f.pc];
                    --poll_countdown_;++pending_metric_steps_;++steps_;HUA_PROFILE_COUNT(vm_steps);HUA_PROFILE_OP(jump.op);HUA_PROFILE_ADD(fused_steps,1);
                    const bool condition=!*b;stack_.pop_back();++f.pc;
                    if(condition==(jump.op==Op::JumpTrue)){if(jump.target>=f.code->instructions.size())invalid(jump.span);f.pc=jump.target;}
                    break;}
                *b=!*b;break;}
            stack_.emplace_back(unary_value(op.text,pop(s),s));break;}
        case Op::Binary:{
            if(stack_.size()>=f.base+2)if(auto* r=std::get_if<Value>(&stack_.back()))if(auto* l=std::get_if<Value>(&stack_[stack_.size()-2])){
                if(int_binary_into(op.text,*l,*r)){stack_.pop_back();break;}
                if(auto fast=fast_binary(op.text,*l,*r)){stack_.pop_back();std::get<Value>(stack_.back())=std::move(*fast);break;}}
            auto right=pop(s),left=pop(s);stack_.emplace_back(binary_value(op.text,left,right,s,op.flag));break;}
        case Op::Jump:if(op.target>=f.code->instructions.size())invalid(s);f.pc=op.target;break;
        case Op::JumpFalse:case Op::JumpTrue: {
            bool condition;
            if(auto* v=stack_.size()>f.base?std::get_if<Value>(&stack_.back()):nullptr;v&&std::holds_alternative<bool>(v->data)){condition=std::get<bool>(v->data);stack_.pop_back();}
            else condition=as_bool(pop(s),s);
            if(condition==(op.op==Op::JumpTrue)){if(op.target>=f.code->instructions.size())invalid(s);f.pc=op.target;}break;
        }
        case Op::EnterScope:{
            if(f.layout->empty_scope[f.pc-1]&&f.scope_depth<64){f.elided_scopes|=std::uint64_t{1}<<f.scope_depth;++f.scope_depth;break;}
            if(f.scope_depth<64)f.elided_scopes&=~(std::uint64_t{1}<<f.scope_depth);++f.scope_depth;
            auto env=activation(f.layout->scope_at[f.pc-1]);HUA_PROFILE_COUNT(vm_scopes);HUA_PROFILE_ADD(vm_scope_slot_bytes,env->slots.size()*sizeof(std::optional<Binding>));env->parent=f.environment;f.environment=std::move(env);break;}
        case Op::LeaveScope:case Op::Unwind: {
            auto count=op.op==Op::LeaveScope?1:op.argument;
            for(std::size_t i=0;i<count;++i){
                auto& top=frames_.back();if(top.scope_depth>0){--top.scope_depth;if(top.scope_depth<64&&(top.elided_scopes>>top.scope_depth)&1)continue;}
                if(!frames_.back().environment->parent || frames_.back().environment==global_)invalid(s);auto env=frames_.back().environment;if(!env->deferred.empty())(co_await drain(env));frames_.back().environment=env->parent;}break;
        }
        case Op::LocateName: {
            if(op.text=="_"){stack_.emplace_back(Location{});break;}
            auto& b=lookup(f,op);if(!b.mutable_binding)runtime_error(s,"cannot mutate immutable binding or read-only view","E4007");
            stack_.emplace_back(LocalRef{&b});break;
        }
        case Op::LocateField: {
            auto value=pop(s);auto object=materialize_record(value);
            if(!object)runtime_error(s,"field assignment requires a struct","E4003");
            if(object->data->enumeration)runtime_error(s,"enum fields are private","E4003");
            auto& cache=access(f).field;auto* item=field(cache,*object,op.text);if(!item)runtime_error(s,"unknown struct field","E4003");
            if(!object->writable)runtime_error(s,"cannot mutate immutable binding or read-only view","E4007");
            stack_.emplace_back(Location{item,field_type(cache,object->data->name,op.text,s),true,object->data,{},{}});break;
        }
        case Op::LocateIndex:case Op::Index: {
            auto index=pop(s),value=pop(s);if(op.op==Op::Index)stack_.emplace_back(index_value(value,index,s));else {auto l=index_location(std::move(value),std::move(index),s);if(!l.writable)runtime_error(s,"cannot mutate immutable binding or read-only view","E4007");stack_.emplace_back(std::move(l));}break;
        }
        case Op::Store: {
            auto value=pop(s);if(stack_.size()<=f.base)invalid(s);
            if(auto* local=std::get_if<LocalRef>(&stack_.back())){
                // Same steps as the Location path below, without building a ValueLocation.
                auto& b=*local->binding;
                // int op= int on an `int` binding: enforce_type/copy_value are identities there.
                if(op.text.size()>1&&b.type=="int"&&std::holds_alternative<std::int64_t>(b.value.data)){
                    Value result=b.value;if(int_binary_into(std::string_view(op.text).substr(0,op.text.size()-1),result,value)&&std::holds_alternative<std::int64_t>(result.data)){
                        b.value=result;stack_.back()=std::move(result);break;}}
                if(op.text.size()>1){const auto name=std::string_view(op.text).substr(0,op.text.size()-1);
                    if(auto fast=fast_binary(name,b.value,value))value=std::move(*fast);else value=binary_value(name,b.value,value,s,op.flag);}
                value=enforce_type(std::move(value),b.type,s,op.flag||op.text.size()>1);b.value=copy_value(value);
                stack_.back()=std::move(value);break;
            }
            if(!std::holds_alternative<Location>(stack_.back()))invalid(s);
            auto where=std::move(std::get<Location>(stack_.back()));stack_.pop_back();
            if(op.text!="=")value=binary_value(std::string_view(op.text).substr(0,op.text.size()-1),location_read(where,s),value,s,op.flag);
            value=enforce_type(std::move(value),where.type,s,op.flag||op.text!="=");location_write(where,value,s);stack_.emplace_back(std::move(value));break;
        }
        case Op::MakeMulti:{auto values=arguments(op.argument,s);for(auto& v:values)v=copy_value(v);stack_.emplace_back(make_multi(std::move(values),op.contextual));break;}
        case Op::BindMulti:{auto pack=pop(s);auto values=unpack_multi(pack,op.argument,s);auto& cached=access(f).multi_types;if(!cached)cached=type_arguments(op.type,"multi");else HUA_PROFILE_COUNT(vm_multi_type_hits);const auto& types=*cached;if(types.size()!=values.size()||op.names.size()!=values.size())invalid(s);materialize_scope();auto data=std::get<MultiValue>(pack.data).data;
            for(std::size_t i=0;i<values.size();++i){values[i]=enforce_type(std::move(values[i]),types[i],s,i<data->contextual.size()&&data->contextual[i]);if(op.names[i]!="_"&&f.environment->own(op.names[i]))runtime_error(s,"duplicate runtime binding","E4001");}
            for(std::size_t i=0;i<values.size();++i)if(op.names[i]!="_"){auto item=op;item.text=op.names[i];item.type=types[i];bind(item,values[i]);}break;}
        case Op::StoreMulti:{auto pack=pop(s);auto values=unpack_multi(pack,op.argument,s);auto data=std::get<MultiValue>(pack.data).data;std::vector<Location> ls(op.argument);
            for(std::size_t i=op.argument;i>0;--i){if(stack_.size()<=f.base)invalid(s);
                if(auto* local=std::get_if<LocalRef>(&stack_.back()))ls[i-1]=Location{&local->binding->value,local->binding->type,true,f.environment,{},{}};
                else if(auto* where=std::get_if<Location>(&stack_.back()))ls[i-1]=std::move(*where);else invalid(s);stack_.pop_back();}
            for(std::size_t i=0;i<values.size();++i)values[i]=copy_value(enforce_type(std::move(values[i]),ls[i].type,s,i<data->contextual.size()&&data->contextual[i]));
            prepare_map_writes(ls,s);for(std::size_t i=0;i<values.size();++i)if(ls[i].value||ls[i].map||ls[i].sequence)location_write(ls[i],values[i],s);break;}
        case Op::MakeMap:stack_.emplace_back(make_map(op.type,s));break;
        case Op::MapInsert:{auto item=pop(s),key=pop(s);if(stack_.empty()||!std::holds_alternative<Value>(stack_.back()))invalid(s);auto m=std::get_if<MapValue>(&std::get<Value>(stack_.back()).data);if(!m)invalid(s);map_put(*m,key,std::move(item),s,op.flag);break;}
        case Op::Propagate:{auto value=pop(s);auto r=std::get_if<ResultValue>(&value.data);if(!r)runtime_error(s,"expected Result","E4003");if(r->ok)stack_.emplace_back(result_payload(value,true,s));else (co_await finish_return(propagated_error(value,s),op,true));break;}
        case Op::CheckIndex:
            if(stack_.empty()||!std::holds_alternative<Value>(stack_.back()))invalid(s);
            if(!std::holds_alternative<SliceValue>(std::get<Value>(stack_.back()).data)&&!std::holds_alternative<MapValue>(std::get<Value>(stack_.back()).data))runtime_error(s,"index/slice requires a slice, array or map","E4003");break;
        case Op::CheckSlice:
            if(stack_.empty() || !std::holds_alternative<Value>(stack_.back()))invalid(s);
            if(!std::holds_alternative<SliceValue>(std::get<Value>(stack_.back()).data))runtime_error(s,"index/slice requires a slice or array","E4003");break;
        case Op::MakeArray: {
            auto storage=managed<std::vector<Value>>();stack_.emplace_back(Value(SliceValue{storage,0,0,true,""}));break;
        }
        case Op::ArrayAppend: {
            auto item=copy_value(pop(s));
            if(stack_.empty() || !std::holds_alternative<Value>(stack_.back()))invalid(s);
            auto slice=std::get_if<SliceValue>(&std::get<Value>(stack_.back()).data);if(!slice)invalid(s);
            if(slice->element_type.empty())slice->element_type=value_type(item);else item=enforce_type(item,slice->element_type,s,op.flag);
            slice->storage->push_back(std::move(item));slice->contextual.push_back(op.flag);++slice->length;break;
        }
        case Op::Slice: {
            auto end=pop(s),start=pop(s),value=pop(s);auto slice=std::get_if<SliceValue>(&value.data);
            if(!slice)runtime_error(s,"index/slice requires a slice or array","E4003");
            I a=op.contextual.at(0)?0:as_int(start,s),b=op.contextual.at(1)?static_cast<I>(slice->length):as_int(end,s);
            if(a<0 || b<a || static_cast<std::size_t>(b)>slice->length)runtime_error(s,"slice bounds must satisfy 0 <= start <= end <= len","E4006");
            auto view=*slice;view.start+=static_cast<std::size_t>(a);view.length=static_cast<std::size_t>(b-a);view.fixed=false;stack_.emplace_back(Value(view));break;
        }
        case Op::MakeStruct: {
            if(op.text=="std.error.Value")runtime_error(s,"standard Error must be constructed through std.error","E4003");
        if(op.text=="std.file.File"||op.text=="std.file.IOFailure")runtime_error(s,"opaque File/IOFailure must be constructed through std.file","E4003");
            auto object=managed<StructData>();object->name=op.text;
            auto [layout,inserted]=enum_layouts_.try_emplace(op.text);
            if(inserted)layout->second=enum_layout(*model_.structures.at(op.text));
            object->enumeration=layout->second;
            if(!object->enumeration){auto [shape,fresh]=record_layouts_.try_emplace(op.text);if(fresh)shape->second=record_layout(*model_.structures.at(op.text));object->fields=RecordFields(shape->second);}
            stack_.emplace_back(Value(StructValue{object,true}));break;
        }
        case Op::InitField: {
            auto item=pop(s);if(stack_.empty() || !std::holds_alternative<Value>(stack_.back()))invalid(s);
            auto object=std::get_if<StructValue>(&std::get<Value>(stack_.back()).data);if(!object)invalid(s);
            auto value=enforce_type(std::move(item),field_type(access(f).field,object->data->name,op.text,s),s,op.flag);
            initialize_field(*object->data,op.text,std::move(value),s);break;
        }
        case Op::Member: {
            auto value=pop(s);
            if(op.text=="len"&&(std::holds_alternative<SliceValue>(value.data)||std::holds_alternative<MapValue>(value.data)||std::holds_alternative<std::string>(value.data))){stack_.emplace_back(aggregate_member(value,op.text,s));break;}
            if(auto p=std::get_if<PlainRecord>(&value.data)){if(auto item=plain_record_field(*p,op.text)){stack_.emplace_back(std::move(*item));break;}}
            auto object=materialize_record(value);if(!object)runtime_error(s,"member access requires a struct","E4003");
            if(auto* item=field(access(f).field,*object,op.text)){retain_record_alias(*item);stack_.emplace_back(object->writable?*item:read_only(*item));break;}
            auto method=model_.functions.find(object->data->name+"."+op.text);
            if(method==model_.functions.end())runtime_error(s,"unknown struct member","E4003");
            if(method->second->span.file_id!=s.file_id && method->second->text.find(" pub")==std::string::npos)
                runtime_error(s,"method is private to its defining module","E5003");
            stack_.emplace_back(Value(Callable{method->second,"",*object}));break;
        }
        case Op::Call: {
            ArgumentLease lease(*this);auto& args=lease.values;pop_arguments(op.argument,s,args);auto callee=pop(s);auto p=std::get_if<Callable>(&callee.data);
            if(!p)runtime_error(s,"value is not callable","E4003");
            if(p->builtin.empty()){enter_function(*p,args,op,&access(f).function);break;}
            HUA_PROFILE_BUILTIN(p->builtin);
            auto& route=access(f).builtin;
            if(route.name!=&p->builtin.str()){route.name=&p->builtin.str();route.info=&builtin_info(p->builtin);}
            const auto& info=*route.info;
            // L0-1 callback functions: run here with the resolved route (no call() frame or lookup).
            if(info.standard&&info.standard->callback){Callbacks host(*this,s,&access(f).function,&op.span);stack_.emplace_back(co_await invoke_callback_standard(*info.standard,std::move(args),s,host,context_));poll_countdown_=1;break;}
            if(info.core!=CoreBuiltin::task_start&&info.core!=CoreBuiltin::parallel_map&&!builtin_may_suspend(info,context_)){
                HUA_PROFILE_COUNT(builtin_calls);stack_.emplace_back(invoke_builtin(info,p->builtin,args,s,output_,op.contextual,op.type,context_));break;}
            (co_await call(*p,args,op));poll_countdown_=1;break;
        }
        case Op::Return:{auto value=pop(s);if(!has_pending_defers(f)){complete_return(copy_value(std::move(value)),op,false);break;}co_await finish_return(std::move(value),op);poll_countdown_=1;break;}
        case Op::RangeInit: {
            I step=as_int(pop(s),s),end=as_int(pop(s),s),start=as_int(pop(s),s);
            if(!step)runtime_error(s,"range step cannot be zero","E4005");
            f.iterators.push_back({true,op.flag,false,start,end,step,0,{}});break;
        }
        case Op::SliceInit: {
            auto value=pop(s);
            if(auto m=std::get_if<MapValue>(&value.data)){auto entries=managed<std::vector<Value>>();for(const auto& [k,v]:m->data->entries)entries->push_back(make_multi({std::visit([](const auto& x){return Value(x);},k),copy_value(m->writable?v:read_only(v))}));Iterator it{};it.map=true;it.slice={entries,0,entries->size(),false,""};f.iterators.push_back(std::move(it));break;}
            auto p=std::get_if<SliceValue>(&value.data);
            if(!p)runtime_error(s,"for requires a slice or range","E4003");Iterator it{};it.slice=*p;f.iterators.push_back(std::move(it));break;
        }
        case Op::IterNext: {
            if(f.iterators.empty())invalid(s);auto& it=f.iterators.back();bool has=!it.done;Value item;I index=it.index;
            if(it.range) {
                has=has && (it.step>0?(it.inclusive?it.current<=it.end:it.current<it.end):(it.inclusive?it.current>=it.end:it.current>it.end));
                if(has) {
                    item=Value(it.current);
                    if((it.inclusive && it.current==it.end)||(it.step>0 && it.current>std::numeric_limits<I>::max()-it.step)||(it.step<0 && it.current<std::numeric_limits<I>::min()-it.step))it.done=true;
                    else it.current+=it.step;
                }
            }else {
                has=has && static_cast<std::uint64_t>(index)<it.slice.length;
                if(has){auto v=sequence_read(it.slice,static_cast<std::size_t>(index));item=it.slice.writable?v:read_only(v);}
            }
            if(!has){if(op.target>=f.code->instructions.size())invalid(s);f.pc=op.target;break;}
            if(it.index==std::numeric_limits<I>::max())it.done=true;else ++it.index;
            if(f.scope_depth<64)f.elided_scopes&=~(std::uint64_t{1}<<f.scope_depth);++f.scope_depth;
            auto env=activation(f.layout->scope_at[f.pc-1]);HUA_PROFILE_COUNT(vm_scopes);HUA_PROFILE_ADD(vm_scope_slot_bytes,env->slots.size()*sizeof(std::optional<Binding>));env->parent=f.environment;f.environment=std::move(env);
            // Loop variables bind read-only: index/key first (int for sequences), then the item.
            static const std::string int_type="int",inferred_type;
            const std::string* key_type=&int_type;
            Value key(index);if(it.map){auto parts=unpack_multi(item,2,s);key=parts[0];item=op.names.size()==1?key:parts[1];key_type=&inferred_type;}
            if(op.names.size()==2)bind_named(op.names[0],*key_type,false,s,std::move(key));
            if(op.names.empty() || op.names.size()>2)invalid(s);
            bind_named(op.names.back(),inferred_type,false,s,std::move(item));break;
        }
        case Op::IterEnd:if(f.iterators.empty())invalid(s);f.iterators.pop_back();break;
        case Op::Fail:runtime_error(s,op.text,op.type.empty()?"E4008":op.type);
        default:invalid(s);
        }
        }catch(const Diagnostic& error){
            auto reported=error.located(s);
            for(auto frame=frames_.rbegin();frame!=frames_.rend();++frame)if(frame->function)reported.add_context(frame->site_span(),"called from");
            throw reported;
        }
    }
    if(stop_depth==0&&!stack_.empty())invalid({});
    co_return;
}
}
