// Typed register execution of verified bytecode (R1 scalars, R2 values).
//
// A function is translated once, at its first call, from its stack bytecode into code for
// two register files whose use is known statically: 8-byte scalar registers (int, float,
// bool) and Value registers for every other kind. Parameters, locals and iterators get
// fixed registers, operand-stack depth d maps to temporary T(d) in the file its entry
// uses, scalar constants sit in a per-function block copied in at entry, and other
// constants are read in place from the function's pool. Block scopes vanish (names
// resolve at translation time). Scalar instructions check no types at run time (D5,
// 2026-11-08); Value instructions run the frame VM's own steps for that bytecode
// instruction (enforce_type, copy_value, read_only, read_field, index_value,
// invoke_builtin, enforce_return, the enum completeness check pop() makes ...), so their
// results and diagnostics are the frame VM's.
//
// Supported: constants; locals and parameters of any kind (not `mut` parameters);
// Bind/Load/LocateName+Store; Binary/Unary; jumps; scopes; int ranges and for-loops over
// slices and maps; struct field reads (Member) and index reads (Index) outside write
// paths; Check{Index,Slice}; array, struct and map literals; `?` propagation; direct
// calls to translated functions; builtin and standard calls that neither call back nor
// suspend; Return. Anything else (write paths through fields or indexes, method calls,
// closures, defer, multiple values, tasks, external functions) rejects the whole
// function, and a function calling a rejected one is rejected too.
//
// Errors are located at the failing instruction and gain one "called from" per register
// frame, as frame VM traces do. Budget steps count register instructions (D4).
#include "hua/vm.hpp"
#include "hua/runtime.hpp"
#include "hua/tasks.hpp"
#include "hua/file.hpp"
#include "hua/enum_value.hpp"
#include "hua/location.hpp"
#include <set>
#include <bit>
#include <cstdlib>
#include <cmath>
#include <functional>
#include <limits>
namespace hua {
namespace {
// The frame VM checks the step budget and cancellation outside its per-instruction
// handler, so those diagnostics carry no "called from" trace; this carrier passes them
// through the register frames' handlers unchanged.
struct Untraced { Diagnostic error; };
enum class Kind : std::uint8_t { Void, Int, Float, Bool, Any, Function, Builtin, Target, Location, Bad };
bool scalar(Kind k){return k==Kind::Int||k==Kind::Float||k==Kind::Bool;}
bool data(Kind k){return scalar(k)||k==Kind::Any;}
Kind scalar_kind(std::string_view type){
    if(type=="int")return Kind::Int;if(type=="float")return Kind::Float;if(type=="bool")return Kind::Bool;return Kind::Bad;
}
// Kind of a declared parameter/return type: a scalar register or a Value register.
Kind declared_kind(std::string_view type){if(type.empty()||type=="void")return Kind::Bad;auto k=scalar_kind(type);return k==Kind::Bad?Kind::Any:k;}
enum class R : std::uint8_t {
    Mov, IntToFloat,
    AddI, SubI, MulI, FloorDivI, ModI, LtI, LeI, GtI, GeI, EqI, NeI,
    AddF, SubF, MulF, DivF, LtF, LeF, GtF, GeF, EqF, NeF,
    EqB, NeB, NotB, NegI, NegF,
    Binary, Unary, // general evaluation of scalar operands with a scalar result
    Jump, JumpFalse, JumpTrue,
    RangeInit, IterNext,
    Call, CallBuiltin, Return, ReturnVoid, ReturnValue,
    // Value instructions (frame VM semantics of the bytecode instruction at origin).
    VMov, VLoad, VLoadGlobal, VBind, VStore, VBinary, VUnary, VJumpFalse, VJumpTrue, VCheck,
    VMember, VIndex, VCheckIndex, VCheckSlice, VPropagate,
    VMakeArray, VAppend, VMakeStruct, VInitField, VMakeMap, VMapInsert, VSliceInit, VIterNext,
    VBox, VLocateIndex, VStoreLocation,
};
// Register operand fields: `regs` marks which of a,b,c,d are registers, `values` which of
// those name Value registers (the rest are scalar registers).
constexpr std::uint8_t fa=1,fb=2,fc=4,fd=8;
struct Ins { R op; Kind x{}; std::uint8_t regs{}, values{}; std::uint16_t a{}, b{}, c{}, d{}; std::uint32_t t{}; std::uint8_t temps{}; };
constexpr std::uint16_t temp_tag=0x8000, const_tag=0x4000, index_mask=0x3fff;
Value box(RegisterValue v,Kind k){
    switch(k){case Kind::Int:return Value(v.i);case Kind::Float:return Value(v.f);case Kind::Bool:return Value(v.i!=0);default:return Value{};}
}
bool unbox(const Value& v,Kind k,RegisterValue& out){
    switch(k){
    case Kind::Int:if(auto p=hua::get_if<std::int64_t>(&v.data)){out.i=*p;return true;}return false;
    case Kind::Float:if(auto p=hua::get_if<double>(&v.data)){out.f=*p;return true;}return false;
    case Kind::Bool:if(auto p=hua::get_if<bool>(&v.data)){out.i=*p;return true;}return false;
    default:return false;
    }
}
Kind kind_of(const Value& v){
    switch(v.data.index()){case 1:return Kind::Bool;case 2:return Kind::Int;case 3:return Kind::Float;default:return Kind::Bad;}
}
[[noreturn]] void invalid(const SourceSpan& s){runtime_error(s,"invalid VM instruction/stack state","E6001");}
// vm.cpp's writable(): whether a value may be lent to a `mut` parameter.
bool writable(const Value& v){if(auto p=cget_if<PlainRecord>(&v.data))return p->writable;if(auto p=hua::get_if<RngValue>(&v.data))return p->writable;if(auto p=hua::get_if<DequeValue>(&v.data))return p->writable;if(auto p=hua::get_if<BufferValue>(&v.data))return p->writable;if(auto p=hua::get_if<ListValue>(&v.data))return p->writable;if(auto p=cget_if<SliceValue>(&v.data))return p->writable;if(auto p=hua::get_if<StructValue>(&v.data))return p->writable;if(auto p=hua::get_if<MapValue>(&v.data))return p->writable;if(auto p=cget_if<ResultValue>(&v.data))return p->writable;return false;}
// `mut` parameters of handle types borrow the handle (no struct write path is involved).
bool handle_type(std::string_view t){return t=="Buffer"||t.starts_with("List<")||t.starts_with("map[")||t.starts_with("Deque<")||t=="Rng";}
// pop()'s check for a struct value leaving the operand stack.
void popped(const Value& v,const SourceSpan& s){if(auto o=hua::get_if<StructValue>(&v.data))require_complete_enum(*o->data,s);}
// Result kind of a general operator, taken from the evaluator itself on sample operands.
Kind general_result(std::string_view text,Kind l,Kind r,bool contextual){
    auto sample=[](Kind k,bool right){return k==Kind::Int?Value(std::int64_t(right?2:7)):k==Kind::Float?Value(right?0.5:2.5):Value(!right);};
    try{auto out=binary_value(text,sample(l,false),sample(r,true),{},contextual);auto k=kind_of(out);return k;}catch(...){return Kind::Bad;}
}
Kind general_unary(std::string_view text,Kind k){
    try{auto out=unary_value(text,k==Kind::Int?Value(std::int64_t(7)):k==Kind::Float?Value(2.5):Value(true),{});return kind_of(out);}catch(...){return Kind::Bad;}
}
}
struct VirtualMachine::RegisterCode {
    std::vector<Ins> code;
    std::vector<const Instruction*> origin;
    std::vector<RegisterValue> constants;
    std::vector<Value> value_constants;
    const Node* function{};
    std::uint16_t registers{}, constant_base{}, values{}, locations{};
    std::vector<Kind> parameter_kinds;
    std::vector<std::string> parameter_types, returns;
    std::vector<bool> parameter_mut;
    Kind result{Kind::Void};
    std::vector<const Node*> callees;
    std::vector<RegisterCode*> callee_code;
    std::vector<std::vector<Kind>> call_arguments; // argument kinds per callee index
    std::vector<bool> scalar_calls; // every argument is a scalar of its parameter's kind (no conversion)
    struct Builtin { std::string name; const BuiltinInfo* info{}; std::vector<Kind> kinds; };
    std::vector<Builtin> builtins;
    std::vector<Binding*> globals;
    std::vector<std::string> types; // declared local types used by VStore
    mutable std::vector<std::unique_ptr<AccessCache>> caches;
    const SourceSpan& span(std::size_t pc) const {return origin[pc]->span;}
    AccessCache& cache(std::size_t pc) const {auto& c=caches[pc];if(!c)c=std::make_unique<AccessCache>();return *c;}
};
struct RegisterTranslator {
    using RegisterCode=VirtualMachine::RegisterCode;
    struct Entry { Kind kind{Kind::Bad}; std::uint16_t reg{}; const Node* function{}; std::string builtin; Kind target_kind{}; bool target_mutable{}; std::uint32_t target_type{}; bool write_root{};
        bool same(const Entry& o)const{return kind==o.kind&&function==o.function&&builtin==o.builtin;} };
    struct Local { std::uint16_t reg; Kind kind; bool mutable_binding; std::uint32_t type; }; // type: index+1 into out.types, 0 unknown
    struct Iterator { bool range; std::uint16_t base, value; };
    const Code& code;RegisterCode& out;
    std::function<VirtualMachine::Binding*(const std::string&)> global;
    std::function<const VirtualMachine::FunctionLayout&(const Node*)> layout_of;
    const std::vector<std::uint8_t>& write_mode;
    std::set<std::pair<std::size_t,std::size_t>>& boxed; // (label pc, depth) slots merged as Value
    std::vector<Entry> stack;
    std::vector<std::unordered_map<std::string,Local>> scopes;
    std::vector<Iterator> iterators;
    std::uint16_t locals{},vlocals{},max_depth{};
    std::unordered_map<std::size_t,std::vector<Entry>> labels; // stack shape at a branch target
    struct Retry {};
    std::unordered_map<std::size_t,std::size_t> placed;       // bytecode pc -> register pc
    std::vector<std::pair<std::size_t,std::size_t>> fixups;    // register pc, bytecode target
    std::vector<bool> is_target;
    std::size_t pc{};
    struct Reject {};
    std::uint16_t locations{};
    std::string why; // first rejection reason (diagnostics only)
    [[noreturn]] void reject(){throw Reject{};}
    static std::uint16_t T(std::size_t depth){return static_cast<std::uint16_t>(temp_tag|depth);}
    static std::uint8_t file(Kind k,std::uint8_t field){return k==Kind::Any?field:0;}
    std::uint16_t constant(RegisterValue v){
        for(std::size_t i=0;i<out.constants.size();++i)if(out.constants[i].i==v.i)return static_cast<std::uint16_t>(const_tag|i);
        if(out.constants.size()>=index_mask)reject();
        out.constants.push_back(v);return static_cast<std::uint16_t>(const_tag|(out.constants.size()-1));
    }
    std::uint16_t value_constant(const Value& v){
        if(out.value_constants.size()>=index_mask)reject();
        out.value_constants.push_back(v);return static_cast<std::uint16_t>(const_tag|(out.value_constants.size()-1));
    }
    std::uint16_t new_local(){if(locals>=index_mask)reject();return locals++;}
    std::uint16_t new_value(){if(vlocals>=index_mask)reject();return vlocals++;}
    std::uint16_t new_register(Kind k){return k==Kind::Any?new_value():new_local();}
    std::uint32_t type_index(const std::string& t){out.types.push_back(t);return static_cast<std::uint32_t>(out.types.size());}
    void emit(Ins i){out.code.push_back(i);out.origin.push_back(&code.instructions[pc]);}
    void push(Entry e){stack.push_back(std::move(e));if(stack.size()>max_depth)max_depth=static_cast<std::uint16_t>(stack.size());if(max_depth>=index_mask)reject();}
    Entry pop(){if(stack.empty())reject();auto e=std::move(stack.back());stack.pop_back();if(e.write_root&&code.instructions[pc].op!=Op::LocateIndex)reject();return e;}
    // Put the entry at depth d into T(d) (a branch or a call needs canonical temporaries).
    void canonical(std::size_t d){
        auto& e=stack[d];
        if(e.kind==Kind::Function||e.kind==Kind::Builtin)return; // a translation-time constant
        if(!data(e.kind)||e.write_root)reject();
        if(e.reg==T(d))return;
        if(e.kind==Kind::Any)emit({R::VMov,Kind::Any,fa|fb,fa|fb,T(d),e.reg});
        else emit({R::Mov,e.kind,fa|fb,0,T(d),e.reg});
        e.reg=T(d);
    }
    void canonical_all(){for(std::size_t d=0;d<stack.size();++d)canonical(d);}
    // Before local `reg` changes, entries still reading it get their own copy.
    void detach(std::uint16_t reg,bool value){for(std::size_t d=0;d<stack.size();++d)if(data(stack[d].kind)&&(stack[d].kind==Kind::Any)==value&&stack[d].reg==reg)canonical(d);}
    // The stack shape that reaches `target`: canonical temporaries, with the slots merged as
    // Value (a scalar on one path, a Value on another) boxed first.
    void arrive(std::size_t target){
        canonical_all();
        for(std::size_t d=0;d<stack.size();++d)if(boxed.contains({target,d})&&scalar(stack[d].kind)){
            emit({R::VBox,stack[d].kind,fa|fb,fa,T(d),stack[d].reg});stack[d].kind=Kind::Any;stack[d].reg=T(d);}
        if(auto found=labels.find(target);found!=labels.end()){
            if(found->second.size()!=stack.size())reject();
            for(std::size_t d=0;d<stack.size();++d)if(!found->second[d].same(stack[d])){
                const auto a=found->second[d].kind,b=stack[d].kind;
                if((scalar(a)&&b==Kind::Any)||(a==Kind::Any&&scalar(b))){boxed.insert({target,d});throw Retry{};}
                reject();}
        }else{std::vector<Entry> shape;for(auto& e:stack){Entry x;x.kind=e.kind;x.reg=e.reg;x.function=e.function;x.builtin=e.builtin;shape.push_back(std::move(x));}labels.emplace(target,std::move(shape));}
    }
    // A jump is emitted after arrive(): the boxing moves must precede it.
    void branch_to(std::size_t target){fixups.emplace_back(out.code.size()-1,target);}
    const Local* find(const std::string& name){for(auto s=scopes.rbegin();s!=scopes.rend();++s)if(auto f=s->find(name);f!=s->end())return &f->second;return nullptr;}
    static std::uint32_t kinds_bits(Kind l,Kind r,bool flag){return (static_cast<std::uint32_t>(l)<<24)|(static_cast<std::uint32_t>(r)<<20)|(flag?1u<<28:0u);}
    // Scalar operands: fast forms for the common operators, else general evaluation.
    void binary(std::string_view text,bool flag,std::uint16_t dst,const Entry& l,const Entry& r,Kind& result){
        R op=R::Binary;Kind k=Kind::Bad;
        if(l.kind==Kind::Int&&r.kind==Kind::Int){
            if(text=="+"){op=R::AddI;k=Kind::Int;}else if(text=="-"){op=R::SubI;k=Kind::Int;}else if(text=="*"){op=R::MulI;k=Kind::Int;}
            else if(text=="//"){op=R::FloorDivI;k=Kind::Int;}else if(text=="%"){op=R::ModI;k=Kind::Int;}
            else if(text=="<"){op=R::LtI;k=Kind::Bool;}else if(text=="<="){op=R::LeI;k=Kind::Bool;}else if(text==">"){op=R::GtI;k=Kind::Bool;}
            else if(text==">="){op=R::GeI;k=Kind::Bool;}else if(text=="=="){op=R::EqI;k=Kind::Bool;}else if(text=="!="){op=R::NeI;k=Kind::Bool;}
        }else if(l.kind==Kind::Float&&r.kind==Kind::Float){
            if(text=="+"){op=R::AddF;k=Kind::Float;}else if(text=="-"){op=R::SubF;k=Kind::Float;}else if(text=="*"){op=R::MulF;k=Kind::Float;}else if(text=="/"){op=R::DivF;k=Kind::Float;}
            else if(text=="<"){op=R::LtF;k=Kind::Bool;}else if(text=="<="){op=R::LeF;k=Kind::Bool;}else if(text==">"){op=R::GtF;k=Kind::Bool;}
            else if(text==">="){op=R::GeF;k=Kind::Bool;}else if(text=="=="){op=R::EqF;k=Kind::Bool;}else if(text=="!="){op=R::NeF;k=Kind::Bool;}
        }else if(l.kind==Kind::Bool&&r.kind==Kind::Bool){
            if(text=="=="){op=R::EqB;k=Kind::Bool;}else if(text=="!="){op=R::NeB;k=Kind::Bool;}
        }
        // The fast form must agree with the evaluator's result kind for these operands.
        const Kind general=general_result(text,l.kind,r.kind,flag);
        if(general==Kind::Bad||(op!=R::Binary&&general!=k))reject();
        if(op==R::Binary)k=general;
        emit({op,k,fa|fb|fc,0,dst,l.reg,r.reg,0,kinds_bits(l.kind,r.kind,flag)});result=k;
    }
    void run(const VirtualMachine::FunctionLayout& layout){
        const auto& ins=code.instructions;is_target.assign(ins.size()+1,false);
        if(write_mode.size()!=ins.size())reject();
        for(const auto& i:ins){if(i.op==Op::Jump||i.op==Op::JumpFalse||i.op==Op::JumpTrue||i.op==Op::IterNext){if(i.target>ins.size())reject();is_target[i.target]=true;}}
        scopes.emplace_back();
        // Scalar parameters take scalar registers 0..; the others Value registers 0..
        for(const auto& p:layout.parameters){
            auto k=declared_kind(p.type);if(k==Kind::Bad||(p.mutable_view&&!handle_type(p.type))){why="param:"+p.type+(p.mutable_view?":mut":"");reject();}
            out.parameter_kinds.push_back(k);out.parameter_types.push_back(p.type);out.parameter_mut.push_back(p.mutable_view);
            scopes.back()[p.name]=Local{new_register(k),k,false,type_index(p.type)};
        }
        out.returns=layout.returns;
        if(layout.returns.size()>1){why="multi-return";reject();}
        out.result=layout.returns.empty()?Kind::Void:declared_kind(layout.returns[0]);if(out.result==Kind::Bad)reject();
        bool reachable=true;
        for(pc=0;pc<ins.size();++pc){
            const auto& op=ins[pc];
            if(is_target[pc]){
                if(reachable)arrive(pc);
                else if(auto f=labels.find(pc);f!=labels.end()){stack.clear();for(const auto& e:f->second)push(e);reachable=true;}
                placed[pc]=out.code.size();
            }
            // Scope and iterator structure is lexical: track it on unreachable paths too.
            if(op.op==Op::EnterScope){scopes.emplace_back();continue;}
            if(op.op==Op::LeaveScope){if(scopes.size()<2)reject();scopes.pop_back();continue;}
            if(op.op==Op::Unwind)continue;
            if(op.op==Op::IterEnd){if(iterators.empty())reject();iterators.pop_back();continue;}
            if(!reachable){
                if(op.op==Op::IterNext)reject();
                if(op.op==Op::RangeInit||op.op==Op::SliceInit)iterators.push_back({true,0,0});
                continue;
            }
            // Mode 3 (pinned until consumed) only matters when something mutates the waiting
            // struct in place; translated code has no field/mutating-call writes, so it reads
            // as a plain load. Modes 1/2 are write paths: only a Load feeding LocateIndex.
            const auto mode=write_mode[pc];
            if((op.op==Op::Member||op.op==Op::Index)&&mode&&mode!=3)reject();
            if(op.op==Op::Load&&mode&&mode!=3&&!find(op.text))reject();
            switch(op.op){
            case Op::Constant:{
                const auto& v=op.constant;
                if(v.data.index()==0){push(Entry{Kind::Void,0});break;}
                auto k=kind_of(v);
                if(k==Kind::Bad){push(Entry{Kind::Any,value_constant(v)});break;}
                RegisterValue r{};unbox(v,k,r);push(Entry{k,constant(r)});break;}
            case Op::Load:{
                if(auto local=find(op.text)){
                    if(local->kind!=Kind::Any){push(Entry{local->kind,local->reg});break;}
                    const auto dst=T(stack.size());emit({R::VLoad,Kind::Any,fa|fb,fa|fb,dst,local->reg});
                    Entry e{Kind::Any,dst};e.write_root=mode&&mode!=3;push(std::move(e));break;}
                auto binding=global(op.text);if(!binding)reject();
                if(auto c=cget_if<Callable>(&binding->value.data)){
                    if(c->extra)reject();
                    if(c->function){push(Entry{Kind::Function,0,c->function});break;}
                    if(c->builtin.empty())reject();
                    Entry e{Kind::Builtin,0};e.builtin=c->builtin.str();push(std::move(e));break;
                }
                out.globals.push_back(binding);const auto dst=T(stack.size());
                emit({R::VLoadGlobal,Kind::Any,fa,fa,dst,0,0,0,static_cast<std::uint32_t>(out.globals.size()-1)});push(Entry{Kind::Any,dst});break;}
            case Op::Bind:{
                auto v=pop();if(!data(v.kind))reject();
                if(scopes.back().contains(op.text))reject();
                const bool contextual=!op.contextual.empty()&&op.contextual[0];
                const Kind declared=op.type.empty()?Kind::Bad:declared_kind(op.type);
                Kind k=op.type.empty()?(scalar(v.kind)?v.kind:Kind::Any):declared;if(k==Kind::Bad)reject();
                auto reg=new_register(k);
                std::uint32_t type=op.type.empty()?(scalar(k)?type_index(std::string(k==Kind::Int?"int":k==Kind::Float?"float":"bool")):0):type_index(op.type);
                if(scalar(k)&&k==v.kind)emit({R::Mov,k,fa|fb,0,reg,v.reg});
                else if(k==Kind::Float&&v.kind==Kind::Int&&contextual)emit({R::IntToFloat,k,fa|fb,0,reg,v.reg});
                else emit({R::VBind,k,fa|fb,static_cast<std::uint8_t>(file(k,fa)|file(v.kind,fb)),reg,v.reg,0,0,(static_cast<std::uint32_t>(v.kind)<<8)|(op.flag?1u:0u)|(contextual?2u:0u)});
                scopes.back()[op.text]=Local{reg,k,op.flag,type};break;}
            case Op::LocateIndex:{
                auto index=pop(),v=pop();if(!data(v.kind)||!data(index.kind))reject();
                const auto slot=locations++;
                emit({R::VLocateIndex,v.kind,fb|fc,static_cast<std::uint8_t>(file(v.kind,fb)|file(index.kind,fc)),slot,v.reg,index.reg,0,kinds_bits(v.kind,index.kind,false)});
                push(Entry{Kind::Location,slot});break;}
            case Op::LocateName:{
                auto local=find(op.text);if(!local)reject();
                Entry e{Kind::Target,local->reg};e.target_kind=local->kind;e.target_mutable=local->mutable_binding;e.target_type=local->type;push(std::move(e));break;}
            case Op::Store:{
                auto v=pop();auto target=pop();
                if(target.kind==Kind::Location){
                    if(!data(v.kind))reject();const auto dst=T(stack.size());
                    emit({R::VStoreLocation,v.kind,fa|fb,static_cast<std::uint8_t>(fa|file(v.kind,fb)),dst,v.reg,target.reg});
                    push(Entry{Kind::Any,dst});break;}
                if(target.kind!=Kind::Target||!target.target_mutable||!data(v.kind))reject();
                const auto reg=target.reg;const Kind k=target.target_kind;
                const bool compound=op.text!="=";
                if(compound&&(op.text.size()<2||op.text.back()!='='))reject();
                detach(reg,k==Kind::Any);
                if(scalar(k)&&scalar(v.kind)&&(!compound?(v.kind==k||(k==Kind::Float&&v.kind==Kind::Int&&op.flag)):true)){
                    if(!compound){emit({v.kind==k?R::Mov:R::IntToFloat,k,fa|fb,0,reg,v.reg});}
                    else{Kind result;binary(std::string_view(op.text).substr(0,op.text.size()-1),op.flag,reg,Entry{k,reg},v,result);if(result!=k)reject();}
                }else{
                    if(!target.target_type)reject();
                    emit({R::VStore,k,fa|fb,static_cast<std::uint8_t>(file(k,fa)|file(v.kind,fb)),reg,v.reg,0,0,(target.target_type<<16)|(static_cast<std::uint32_t>(v.kind)<<8)|(compound?1u:0u)});
                }
                push(Entry{k,reg});break;}
            case Op::Binary:{
                auto r=pop();auto l=pop();if(!data(l.kind)||!data(r.kind))reject();
                const auto dst=T(stack.size());
                if(scalar(l.kind)&&scalar(r.kind)){Kind k;binary(op.text,op.flag,dst,l,r,k);push(Entry{k,dst});break;}
                emit({R::VBinary,Kind::Any,fa|fb|fc,static_cast<std::uint8_t>(fa|file(l.kind,fb)|file(r.kind,fc)),dst,l.reg,r.reg,0,kinds_bits(l.kind,r.kind,op.flag)});
                push(Entry{Kind::Any,dst});break;}
            case Op::Unary:{
                auto v=pop();if(!data(v.kind))reject();const auto dst=T(stack.size());
                if(v.kind==Kind::Any){emit({R::VUnary,Kind::Any,fa|fb,fa|fb,dst,v.reg});push(Entry{Kind::Any,dst});break;}
                const Kind general=general_unary(op.text,v.kind);if(general==Kind::Bad)reject();
                Ins i{R::Unary,general,fa|fb,0,dst,v.reg,0,0,static_cast<std::uint32_t>(v.kind)<<24};
                if(op.text=="!"&&v.kind==Kind::Bool&&general==Kind::Bool)i.op=R::NotB;
                else if(op.text=="-"&&v.kind==Kind::Int&&general==Kind::Int)i.op=R::NegI;
                else if(op.text=="-"&&v.kind==Kind::Float&&general==Kind::Float)i.op=R::NegF;
                emit(i);push(Entry{general,dst});break;}
            case Op::Pop:{
                auto v=pop();if(v.kind==Kind::Function||v.kind==Kind::Builtin||v.kind==Kind::Target)reject();
                if(v.kind==Kind::Any)emit({R::VCheck,Kind::Any,fa,fa,v.reg});break;}
            case Op::Jump:arrive(op.target);emit({R::Jump});branch_to(op.target);reachable=false;break;
            case Op::JumpFalse:case Op::JumpTrue:{
                auto v=pop();if(v.kind!=Kind::Bool&&v.kind!=Kind::Any)reject();
                arrive(op.target);
                if(v.kind==Kind::Bool)emit({op.op==Op::JumpFalse?R::JumpFalse:R::JumpTrue,Kind::Bool,fa,0,v.reg});
                else emit({op.op==Op::JumpFalse?R::VJumpFalse:R::VJumpTrue,Kind::Any,fa,fa,v.reg});
                branch_to(op.target);break;}
            case Op::RangeInit:{
                auto step=pop(),end=pop(),start=pop();
                if(start.kind!=Kind::Int||end.kind!=Kind::Int||step.kind!=Kind::Int)reject();
                std::uint16_t base=new_local();new_local();new_local();new_local(); // current, end, step, done
                emit({R::RangeInit,op.flag?Kind::Bool:Kind::Void,fa|fb|fc|fd,0,base,start.reg,end.reg,step.reg});
                iterators.push_back({true,base,0});break;}
            case Op::SliceInit:{
                auto v=pop();if(!data(v.kind))reject();
                std::uint16_t base=new_local();new_local();new_local(); // index, done, map
                const auto value=new_value();
                emit({R::VSliceInit,v.kind,fa|fb|fd,static_cast<std::uint8_t>(fa|file(v.kind,fb)),value,v.reg,0,base});
                iterators.push_back({false,base,value});break;}
            case Op::IterNext:{
                if(iterators.empty())reject();
                const auto it=iterators.back();
                arrive(op.target);
                if(it.range){
                    if(op.names.size()!=1)reject();
                    const auto item=new_local();
                    Kind inclusive=Kind::Void;for(auto r=out.code.rbegin();r!=out.code.rend();++r)if(r->op==R::RangeInit&&r->a==it.base){inclusive=r->x;break;}
                    emit({R::IterNext,inclusive,fa|fb,0,it.base,item});branch_to(op.target);
                    scopes.emplace_back();scopes.back()[op.names[0]]=Local{item,Kind::Int,false,type_index("int")};break;
                }
                if(op.names.empty()||op.names.size()>2)reject();
                const bool keyed=op.names.size()==2;
                const auto key=keyed?new_value():std::uint16_t(0);const auto item=new_value();
                emit({R::VIterNext,keyed?Kind::Bool:Kind::Void,static_cast<std::uint8_t>(fa|fc|fd|(keyed?fb:0)),static_cast<std::uint8_t>(fa|fc|(keyed?fb:0)),it.value,key,item,it.base});
                branch_to(op.target);
                scopes.emplace_back();
                if(keyed)scopes.back()[op.names[0]]=Local{key,Kind::Any,false,0};
                scopes.back()[op.names.back()]=Local{item,Kind::Any,false,0};break;}
            case Op::Member:{
                auto v=pop();if(!data(v.kind))reject();const auto dst=T(stack.size());
                if(v.kind==Kind::Any&&!out.code.empty()&&out.code.back().op==R::VLoad&&out.code.back().a==v.reg&&(v.reg&temp_tag)&&!is_target[pc]&&write_mode[pc]==0&&pc>0&&write_mode[pc-1]==0){
                    v.reg=out.code.back().b;out.code.pop_back();out.origin.pop_back(); // read the local in place
                }
                emit({R::VMember,v.kind,fa|fb,static_cast<std::uint8_t>(fa|file(v.kind,fb)),dst,v.reg});push(Entry{Kind::Any,dst});break;}
            case Op::Index:{
                auto index=pop(),v=pop();if(!data(v.kind)||!data(index.kind))reject();const auto dst=T(stack.size());
                emit({R::VIndex,Kind::Any,fa|fb|fc,static_cast<std::uint8_t>(fa|file(v.kind,fb)|file(index.kind,fc)),dst,v.reg,index.reg,0,kinds_bits(v.kind,index.kind,false)});
                push(Entry{Kind::Any,dst});break;}
            case Op::CheckIndex:case Op::CheckSlice:{
                if(stack.empty()||!data(stack.back().kind))reject();const auto& v=stack.back();
                emit({op.op==Op::CheckIndex?R::VCheckIndex:R::VCheckSlice,v.kind,fa,file(v.kind,fa),v.reg});break;}
            case Op::Propagate:{
                if(out.result!=Kind::Any)reject();
                auto v=pop();if(!data(v.kind))reject();const auto dst=T(stack.size());
                emit({R::VPropagate,v.kind,fa|fb,static_cast<std::uint8_t>(fa|file(v.kind,fb)),dst,v.reg});push(Entry{Kind::Any,dst});break;}
            case Op::MakeArray:{const auto dst=T(stack.size());emit({R::VMakeArray,Kind::Any,fa,fa,dst});push(Entry{Kind::Any,dst});break;}
            case Op::MakeStruct:{const auto dst=T(stack.size());emit({R::VMakeStruct,Kind::Any,fa,fa,dst});push(Entry{Kind::Any,dst});break;}
            case Op::MakeMap:{const auto dst=T(stack.size());emit({R::VMakeMap,Kind::Any,fa,fa,dst});push(Entry{Kind::Any,dst});break;}
            case Op::ArrayAppend:case Op::InitField:{
                auto item=pop();if(!data(item.kind)||stack.empty()||stack.back().kind!=Kind::Any)reject();
                canonical(stack.size()-1); // the literal under construction is a temporary
                emit({op.op==Op::ArrayAppend?R::VAppend:R::VInitField,item.kind,fa|fb,static_cast<std::uint8_t>(fa|file(item.kind,fb)),stack.back().reg,item.reg});break;}
            case Op::MapInsert:{
                auto item=pop(),key=pop();if(!data(item.kind)||!data(key.kind)||stack.empty()||stack.back().kind!=Kind::Any)reject();
                canonical(stack.size()-1);
                emit({R::VMapInsert,Kind::Any,fa|fb|fc,static_cast<std::uint8_t>(fa|file(key.kind,fb)|file(item.kind,fc)),stack.back().reg,key.reg,item.reg,0,kinds_bits(key.kind,item.kind,false)});break;}
            case Op::Call:{
                const std::size_t n=op.argument;if(stack.size()<n+1)reject();
                const std::size_t base=stack.size()-n-1;
                // A write-mode load lent to a `mut` handle parameter is consumed here.
                for(std::size_t d=base+1;d<stack.size();++d){stack[d].write_root=false;canonical(d);}
                std::vector<Kind> args;for(std::size_t d=base+1;d<stack.size();++d)args.push_back(stack[d].kind);
                auto callee=stack[base];
                Kind result;R call;std::uint32_t index;
                if(callee.kind==Kind::Function){
                    const auto& l=layout_of(callee.function);
                    if(l.parameters.size()!=n||l.returns.size()>1)reject();
                    result=l.returns.empty()?Kind::Void:declared_kind(l.returns[0]);if(result==Kind::Bad)reject();
                    index=static_cast<std::uint32_t>(out.callees.size());out.callees.push_back(callee.function);out.call_arguments.push_back(std::move(args));call=R::Call;
                }else if(callee.kind==Kind::Builtin){
                    const auto& info=builtin_info(callee.builtin);
                    if((info.standard&&info.standard->callback)||info.always_suspends||info.suspends_in_task||info.core==CoreBuiltin::task_start||info.core==CoreBuiltin::parallel_map||info.core==CoreBuiltin::task_await||info.core==CoreBuiltin::task_group_begin||info.core==CoreBuiltin::task_group_end)reject();
                    result=scalar_kind(op.type);if(result==Kind::Bad)result=Kind::Any;
                    out.builtins.push_back({callee.builtin,&info,std::move(args)});index=static_cast<std::uint32_t>(out.builtins.size()-1);call=R::CallBuiltin;
                }else reject();
                emit({call,result,static_cast<std::uint8_t>((result==Kind::Void?0:fa)|fb|fd),static_cast<std::uint8_t>(file(result,fa)|fd),T(base),T(base+1),static_cast<std::uint16_t>(n),T(base+1),index});
                stack.resize(base);push(Entry{result,result==Kind::Void?std::uint16_t(0):T(base)});break;}
            case Op::Return:{
                auto v=pop();
                if(out.result==Kind::Void){if(v.kind!=Kind::Void)reject();emit({R::ReturnVoid});}
                else if(!data(v.kind))reject();
                else if(scalar(out.result)&&v.kind==out.result)emit({R::Return,v.kind,fa,0,v.reg});
                else emit({R::ReturnValue,v.kind,fa,file(v.kind,fa),v.reg});
                reachable=false;break;}
            default:reject();
            }
        }
        if(reachable)reject(); // falling off the end is not verified bytecode
        for(auto [at,target]:fixups){auto f=placed.find(target);if(f==placed.end())reject();out.code[at].t=static_cast<std::uint32_t>(f->second);}
    }
    void finish(){
        const std::uint16_t temps=locals,vtemps=vlocals;
        if(temps+max_depth+out.constants.size()>=0x3fff||vtemps+max_depth>=0x3fff)reject();
        out.constant_base=static_cast<std::uint16_t>(temps+max_depth);
        out.registers=static_cast<std::uint16_t>(out.constant_base+out.constants.size());
        out.values=static_cast<std::uint16_t>(vtemps+max_depth);
        auto map=[&](std::uint16_t r,bool value)->std::uint16_t{
            if(r&temp_tag)return static_cast<std::uint16_t>((value?vtemps:temps)+(r&index_mask));
            if(r&const_tag)return value?r:static_cast<std::uint16_t>(out.constant_base+(r&index_mask)); // Value constants stay pool references
            return r;};
        for(auto& i:out.code){
            const auto temp=[&](std::uint8_t f,std::uint16_t x){if((i.regs&f)&&(i.values&f)&&(x&temp_tag))i.temps|=f;};
            temp(fb,i.b);temp(fc,i.c);
            if(i.regs&fa)i.a=map(i.a,i.values&fa);
            if(i.regs&fb)i.b=map(i.b,i.values&fb);
            if(i.regs&fc)i.c=map(i.c,i.values&fc);
            if(i.regs&fd)i.d=map(i.d,i.values&fd);
        }
        out.caches.resize(out.code.size());out.locations=locations;
    }
};

VirtualMachine::RegisterCode* VirtualMachine::register_code(const Node& root){
    if(auto f=register_codes_.find(&root);f!=register_codes_.end())return f->second.get();
    // Translate root and every callee it reaches; then reject, to a fixpoint, functions
    // whose callees did not translate.
    std::vector<const Node*> work{&root},fresh;
    while(!work.empty()){
        const Node* fn=work.back();work.pop_back();
        if(register_codes_.contains(fn))continue;
        auto body=bytecode_?bytecode_->functions.find(fn):decltype(bytecode_->functions.end()){};
        std::shared_ptr<RegisterCode> code;
        if(bytecode_&&body!=bytecode_->functions.end()&&!model_.nested.contains(fn)){
            const auto& layout=function_layout(*fn);
            if(!layout.external&&!layout.unsafe){
                const auto& slots=code_layout(body->second,fn);
                // A merge of a scalar with a Value retries with that slot boxed on every path.
                std::set<std::pair<std::size_t,std::size_t>> boxed;
                for(int attempt=0;;++attempt){
                    code=std::make_shared<RegisterCode>();code->function=fn;
                    RegisterTranslator t{body->second,*code,[this](const std::string& name)->Binding*{
                        for(auto env=global_.get();env;env=env->parent.get())if(auto b=env->own(name))return b;return nullptr;},
                        [this](const Node* callee)->const FunctionLayout&{return function_layout(*callee);},slots.write_mode,boxed};
                    try{t.run(layout);t.finish();break;}
                    catch(const RegisterTranslator::Retry&){if(attempt<1024)continue;code.reset();break;}
                    catch(const RegisterTranslator::Reject&){code.reset();break;}
                }
            }
        }
        register_codes_[fn]=code;fresh.push_back(fn);
        if(code)for(auto callee:code->callees)if(!register_codes_.contains(callee))work.push_back(callee);
    }
    for(bool changed=true;changed;){
        changed=false;
        for(auto fn:fresh){
            auto& code=register_codes_[fn];if(!code)continue;
            bool ok=true;code->callee_code.assign(code->callees.size(),nullptr);
            for(std::size_t i=0;i<code->callees.size();++i){
                auto f=register_codes_.find(code->callees[i]);
                auto* callee=f==register_codes_.end()?nullptr:f->second.get();
                if(!callee){ok=false;break;}
                code->callee_code[i]=callee;
            }
            if(ok){code->scalar_calls.assign(code->callees.size(),false);
                for(std::size_t i=0;i<code->callees.size();++i){bool plain=true;const auto& kinds=code->call_arguments[i];
                    for(std::size_t k=0;k<kinds.size();++k)if(!scalar(kinds[k])||code->callee_code[i]->parameter_kinds[k]!=kinds[k]||code->callee_code[i]->parameter_mut[k])plain=false;
                    code->scalar_calls[i]=plain;}}
            if(!ok){code.reset();changed=true;}
        }
    }
    for(auto fn:fresh)if(auto& code=register_codes_[fn]){register_max_=std::max<std::size_t>(register_max_,code->registers);value_max_=std::max<std::size_t>(value_max_,code->values);}
    return register_codes_[&root].get();
}

namespace {
// enter_function's treatment of one argument for a non-`mut` parameter: the argument
// list's own copy loses a pin path, then the parameter value is a read-only copy of it.
Value parameter_value(Value arg,const std::string& type,const SourceSpan& span,bool contextual){
    if(auto h=hua::get_if<StructValue>(&arg.data);h&&h->path)h->path.reset();
    auto v=enforce_type(copy_value(arg),type,span,contextual);
    return read_only(std::move(v));
}
}

bool VirtualMachine::register_call(const Callable& callable,const std::vector<Value>& args,const Instruction& site,const FunctionLayout& layout,std::size_t user_calls){
    // HUA_NO_REGISTER_VM=1 keeps every call on the frame path (differential testing).
    static const bool disabled=[]{const char* v=std::getenv("HUA_NO_REGISTER_VM");return v&&*v&&*v!='0';}();
    if(disabled||(context_&&context_->in_task)||callable.extra||!callable.function||!bytecode_||register_user_base_)return false;
    auto* code=register_code(*callable.function);if(!code)return false;
    if(args.size()!=code->parameter_kinds.size())return false;
    for(std::size_t i=0;i<args.size();++i)if(scalar(code->parameter_kinds[i])&&kind_of(args[i])!=code->parameter_kinds[i])return false;
    (void)layout;
    const std::size_t limit=context_?context_->execution_limits.calls:128;
    if(register_file_.size()<(limit+2)*register_max_)register_file_.assign((limit+2)*register_max_,RegisterValue{});
    if(value_file_.size()<(limit+2)*value_max_)value_file_.resize((limit+2)*value_max_);
    RegisterValue* regs=register_file_.data();Value* values=value_file_.data();
    std::size_t s=0,v=0;
    for(std::size_t i=0;i<args.size();++i){
        const Kind k=code->parameter_kinds[i];
        if(code->parameter_mut[i]){if(!writable(args[i]))runtime_error(site.span,"mut parameter requires mutable argument","E4007");values[v++]=enforce_borrow(args[i],code->parameter_types[i],site.span);}
        else if(k==Kind::Any){HUA_PROFILE_CONSTRAINT_SOURCE(argument);values[v++]=parameter_value(args[i],code->parameter_types[i],site.span,i<site.contextual.size()&&site.contextual[i]);}
        else unbox(args[i],k,regs[s++]);
    }
    // A call observer sees the same enter/leave events, with flushed metrics, as frames give.
    if(observer_){flush_metrics();observer_->enter(callable.function,frames_.empty()?nullptr:frames_.back().function,args);}
    register_user_base_=user_calls+1;
    Value result;std::int64_t raw;
    try{raw=run_registers(*code,regs,values,0,site.span,&result);}
    catch(const Untraced& e){register_user_base_=0;flush_metrics();throw e.error;}
    catch(...){register_user_base_=0;flush_metrics();throw;}
    register_user_base_=0;flush_metrics();
    Value value;
    if(code->result==Kind::Any)value=std::move(result);else{RegisterValue r;r.i=raw;value=box(r,code->result);}
    if(!frames_.empty())stack_.emplace_back(std::move(value));else completed_=std::move(value);
    if(observer_){flush_metrics();observer_->leave(callable.function,frames_.empty()?(completed_?&*completed_:nullptr):hua::get_if<Value>(&stack_.back()));}
    return true;
}

std::int64_t VirtualMachine::run_registers(const RegisterCode& c,RegisterValue* r,Value* v,std::size_t depth,const SourceSpan& site,Value* result){
    for(std::size_t i=0;i<c.constants.size();++i)r[c.constant_base+i]=c.constants[i];
    const Ins* code=c.code.data();std::size_t pc=0;
    const std::size_t step_limit=context_?context_->execution_limits.steps:std::numeric_limits<std::size_t>::max();
    const std::size_t call_limit=context_?context_->execution_limits.calls:128;
    unsigned poll=256;
    // Value operands: registers, or (const_tag) the function's constant pool.
    auto V=[&](std::uint16_t x)->const Value&{return (x&const_tag)?c.value_constants[x&index_mask]:v[x];};
    auto operand=[&](Kind k,std::uint16_t x)->Value{return k==Kind::Any?V(x):box(r[x],k);};
    // A consumed operand: a stack temporary is moved out (as pop() moves it), others copied.
    auto take=[&](const Ins& i,std::uint8_t f,Kind k,std::uint16_t x)->Value{if(k==Kind::Any&&(i.temps&f))return std::move(v[x]);return operand(k,x);};
    // Scalar general path: binary_value on boxed scalars, unboxed to the static result kind.
    auto general=[&](const Ins& i,std::size_t at){
        const Kind lk=static_cast<Kind>((i.t>>24)&0xf),rk=static_cast<Kind>((i.t>>20)&0xf);const bool flag=(i.t>>28)&1;
        const auto& origin=*c.origin[at];
        const std::string_view text=origin.op==Op::Store?std::string_view(origin.text).substr(0,origin.text.size()-1):std::string_view(origin.text);
        auto out=binary_value(text,box(r[i.b],lk),box(r[i.c],rk),c.span(at),flag);
        if(!unbox(out,i.x,r[i.a]))runtime_error(c.span(at),"register VM result kind mismatch","E8002");
    };
    std::vector<ValueLocation> places; // LocateIndex results waiting for their Store
    auto clear=[&]{for(std::size_t k=0;k<c.values;++k)if(v[k].data.index())v[k]=Value{};};
    // Store a value into a destination of static kind k (scalar register or Value register).
    auto put=[&](Kind k,std::uint16_t dst,Value value,std::size_t at){
        if(k==Kind::Any){v[dst]=std::move(value);return;}
        if(!unbox(value,k,r[dst]))runtime_error(c.span(at),"register VM result kind mismatch","E8002");
    };
    try{
        for(;;){
            const Ins& i=code[pc++];
            ++pending_metric_steps_;
            if(++steps_>step_limit){flush_metrics();throw Untraced{Diagnostic("E4099",c.span(pc-1),"execution step limit exceeded ("+std::to_string(step_limit)+")")};}
            if(--poll==0){poll=256;flush_metrics();try{cancellation_checkpoint(context_,c.span(pc-1));}catch(const Diagnostic& e){throw Untraced{e};}}
            switch(i.op){
            case R::Mov:r[i.a]=r[i.b];break;
            case R::IntToFloat:r[i.a].f=static_cast<double>(r[i.b].i);break;
            case R::AddI:{std::int64_t x;if(__builtin_add_overflow(r[i.b].i,r[i.c].i,&x)){general(i,pc-1);break;}r[i.a].i=x;break;}
            case R::SubI:{std::int64_t x;if(__builtin_sub_overflow(r[i.b].i,r[i.c].i,&x)){general(i,pc-1);break;}r[i.a].i=x;break;}
            case R::MulI:{std::int64_t x;if(__builtin_mul_overflow(r[i.b].i,r[i.c].i,&x)){general(i,pc-1);break;}r[i.a].i=x;break;}
            case R::FloorDivI:case R::ModI:{
                const auto x=r[i.b].i,y=r[i.c].i;
                if(y==0||(x==std::numeric_limits<std::int64_t>::min()&&y==-1)){general(i,pc-1);break;}
                std::int64_t q=x/y,rem=x%y;if(rem&&((rem<0)!=(y<0))){--q;rem+=y;}r[i.a].i=i.op==R::ModI?rem:q;break;}
            case R::LtI:r[i.a].i=r[i.b].i<r[i.c].i;break;
            case R::LeI:r[i.a].i=r[i.b].i<=r[i.c].i;break;
            case R::GtI:r[i.a].i=r[i.b].i>r[i.c].i;break;
            case R::GeI:r[i.a].i=r[i.b].i>=r[i.c].i;break;
            case R::EqI:r[i.a].i=r[i.b].i==r[i.c].i;break;
            case R::NeI:r[i.a].i=r[i.b].i!=r[i.c].i;break;
            case R::AddF:{double x=r[i.b].f+r[i.c].f;if(!std::isfinite(x)){general(i,pc-1);break;}r[i.a].f=x;break;}
            case R::SubF:{double x=r[i.b].f-r[i.c].f;if(!std::isfinite(x)){general(i,pc-1);break;}r[i.a].f=x;break;}
            case R::MulF:{double x=r[i.b].f*r[i.c].f;if(!std::isfinite(x)){general(i,pc-1);break;}r[i.a].f=x;break;}
            case R::DivF:{double x=r[i.c].f!=0?r[i.b].f/r[i.c].f:NAN;if(!std::isfinite(x)){general(i,pc-1);break;}r[i.a].f=x;break;}
            case R::LtF:r[i.a].i=r[i.b].f<r[i.c].f;break;
            case R::LeF:r[i.a].i=r[i.b].f<=r[i.c].f;break;
            case R::GtF:r[i.a].i=r[i.b].f>r[i.c].f;break;
            case R::GeF:r[i.a].i=r[i.b].f>=r[i.c].f;break;
            case R::EqF:r[i.a].i=r[i.b].f==r[i.c].f;break;
            case R::NeF:r[i.a].i=r[i.b].f!=r[i.c].f;break;
            case R::EqB:r[i.a].i=(r[i.b].i!=0)==(r[i.c].i!=0);break;
            case R::NeB:r[i.a].i=(r[i.b].i!=0)!=(r[i.c].i!=0);break;
            case R::NotB:r[i.a].i=!r[i.b].i;break;
            case R::NegI:if(r[i.b].i==std::numeric_limits<std::int64_t>::min())goto unary_general;r[i.a].i=-r[i.b].i;break;
            case R::NegF:r[i.a].f=-r[i.b].f;break;
            case R::Binary:general(i,pc-1);break;
            case R::Unary:
            unary_general:{
                auto out=unary_value(c.origin[pc-1]->text,box(r[i.b],static_cast<Kind>((i.t>>24)&0xf)),c.span(pc-1));
                if(!unbox(out,i.x,r[i.a]))runtime_error(c.span(pc-1),"register VM result kind mismatch","E8002");break;}
            case R::Jump:pc=i.t;break;
            case R::JumpFalse:if(!r[i.a].i)pc=i.t;break;
            case R::JumpTrue:if(r[i.a].i)pc=i.t;break;
            case R::RangeInit:{
                const auto step=r[i.d].i;
                if(!step)runtime_error(c.span(pc-1),"range step cannot be zero","E4005");
                r[i.a].i=r[i.b].i;r[i.a+1].i=r[i.c].i;r[i.a+2].i=step;r[i.a+3].i=0;break;}
            case R::IterNext:{
                auto& cur=r[i.a].i;const auto end=r[i.a+1].i,step=r[i.a+2].i;auto& done=r[i.a+3].i;const bool inclusive=i.x==Kind::Bool;
                const bool has=!done&&(step>0?(inclusive?cur<=end:cur<end):(inclusive?cur>=end:cur>end));
                if(!has){pc=i.t;break;}
                r[i.b].i=cur;
                if((inclusive&&cur==end)||(step>0&&cur>std::numeric_limits<std::int64_t>::max()-step)||(step<0&&cur<std::numeric_limits<std::int64_t>::min()-step))done=1;
                else cur+=step;
                break;}
            case R::Call:{
                const RegisterCode& callee=*c.callee_code[i.t];const auto& kinds=c.call_arguments[i.t];
                const auto& call=*c.origin[pc-1];
                const std::size_t user_calls=register_user_base_+depth;
                if(user_calls>=call_limit)runtime_error(c.span(pc-1),"function call depth limit exceeded ("+std::to_string(call_limit)+")","E4099");
                if(context_&&context_->execution_metrics)context_->execution_metrics->call(static_cast<unsigned>(user_calls+1));
                RegisterValue* next=r+c.registers;Value* nextv=v+c.values;
                if(next+callee.registers>register_file_.data()+register_file_.size()||nextv+callee.values>value_file_.data()+value_file_.size())
                    runtime_error(c.span(pc-1),"function call depth limit exceeded ("+std::to_string(call_limit)+")","E4099");
                if(c.scalar_calls[i.t]&&!observer_)[[likely]]{
                    for(std::size_t k=0;k<i.c;++k)next[k]=r[i.b+k];
                    const auto raw=run_registers(callee,next,nextv,depth+1,call.span,i.x==Kind::Any?&v[i.a]:nullptr);
                    if(i.x!=Kind::Any&&i.x!=Kind::Void)r[i.a].i=raw;
                    break;}
                // Arguments leave the operand stack (pop's check), then convert as enter_function does.
                for(std::size_t k=0;k<i.c;++k)if(kinds[k]==Kind::Any)popped(v[i.d+k],c.span(pc-1));
                // Each argument is a consumed temporary: taken out of its register (as pop moves it).
                std::vector<Value> observed;
                if(observer_)[[unlikely]]for(std::size_t k=0;k<i.c;++k)observed.push_back(operand(kinds[k],kinds[k]==Kind::Any?i.d+k:i.b+k));
                auto argument=[&](std::size_t k)->Value{return kinds[k]==Kind::Any?std::move(v[i.d+k]):box(r[i.b+k],kinds[k]);};
                std::size_t s=0,w=0;
                for(std::size_t k=0;k<i.c;++k){
                    const Kind pk=callee.parameter_kinds[k],ak=kinds[k];
                    if(scalar(pk)&&pk==ak){next[s++]=r[i.b+k];continue;}
                    if(callee.parameter_mut[k]){Value arg=argument(k);if(!writable(arg))runtime_error(call.span,"mut parameter requires mutable argument","E4007");nextv[w++]=enforce_borrow(std::move(arg),callee.parameter_types[k],call.span);continue;}
                    HUA_PROFILE_CONSTRAINT_SOURCE(argument);
                    const bool contextual=k<call.contextual.size()&&call.contextual[k];
                    if(pk==Kind::Any)nextv[w++]=parameter_value(argument(k),callee.parameter_types[k],call.span,contextual);
                    else{const Value given=argument(k);auto converted=enforce_type(copy_value(given),callee.parameter_types[k],call.span,contextual);if(!unbox(converted,pk,next[s++]))runtime_error(call.span,"register VM result kind mismatch","E8002");}
                }
                Value out;std::int64_t raw;
                if(observer_)[[unlikely]]{
                    flush_metrics();observer_->enter(callee.function,c.function,observed);
                    raw=run_registers(callee,next,nextv,depth+1,call.span,&out);
                    Value shown;if(callee.result==Kind::Any)shown=out;else{RegisterValue x;x.i=raw;shown=box(x,callee.result);}
                    flush_metrics();observer_->leave(callee.function,&shown);
                }else raw=run_registers(callee,next,nextv,depth+1,call.span,&out);
                if(i.x==Kind::Any)v[i.a]=std::move(out);else if(i.x!=Kind::Void)r[i.a].i=raw;
                break;}
            case R::Return:{const auto x=r[i.a].i;clear();return x;}
            case R::ReturnVoid:clear();return 0;
            case R::ReturnValue:{
                Value value=operand(i.x,i.a);popped(value,c.span(pc-1));
                value=enforce_return(copy_value(std::move(value)),c.returns,site,c.origin[pc-1]->flag);
                if(c.result==Kind::Any){*result=std::move(value);clear();return 0;}
                RegisterValue x;if(!unbox(value,c.result,x))runtime_error(c.span(pc-1),"register VM result kind mismatch","E8002");
                clear();return x.i;}
            default:register_value_op(c,pc-1,r,v,places);break;
            case R::VJumpFalse:case R::VJumpTrue:{
                const auto& value=V(i.a);bool condition;
                if(auto b=hua::get_if<bool>(&value.data))condition=*b;else{popped(value,c.span(pc-1));condition=as_bool(value,c.span(pc-1));}
                if(condition==(i.op==R::VJumpTrue))pc=i.t;break;}
            case R::VPropagate:{
                const auto& op=*c.origin[pc-1];const auto& s=op.span;
                Value value=take(i,fb,i.x,i.b);popped(value,s);
                auto rv=cget_if<ResultValue>(&value.data);if(!rv)runtime_error(s,"expected Result","E4003");
                if(rv->ok){v[i.a]=result_payload(value,true,s);break;}
                // Early return of the error, as finish_return/complete_return do it.
                Value out=copy_value(propagated_error(value,s));
                *result=enforce_return(std::move(out),c.returns,site,op.flag);clear();return 0;}
            case R::VIterNext:{
                const auto& op=*c.origin[pc-1];const auto& s=op.span;
                const auto& slice=cget<SliceValue>(v[i.a].data);auto& index=r[i.d].i;auto& done=r[i.d+1].i;const bool map=r[i.d+2].i!=0;
                const bool has=!done&&static_cast<std::uint64_t>(index)<slice.length;
                if(!has){pc=i.t;break;}
                Value item;{auto x=sequence_read(slice,static_cast<std::size_t>(index));item=slice.writable?x:read_only(x);}
                const std::int64_t at=index;
                if(index==std::numeric_limits<std::int64_t>::max())done=1;else ++index;
                Value key(at);
                if(map){auto parts=unpack_multi(item,2,s);key=parts[0];item=op.names.size()==1?key:parts[1];}
                // Loop variables bind read-only, as bind_slot does it.
                auto bound=[&](Value x){const auto kind=value_kind(x);if(kind>=1&&kind<=3)return x;
                    if(hua::holds_alternative<MultiValue>(x.data))runtime_error(s,"multiple values require positional binding","E4003");
                    return read_only(copy_value(std::move(x)));};
                if(i.x==Kind::Bool)v[i.b]=bound(std::move(key));
                v[i.c]=bound(std::move(item));break;}
            }
        }
    }catch(const Diagnostic& error){
        clear();
        auto reported=error.located(c.span(pc-1));
        reported.add_context(site,"called from");
        throw reported;
    }catch(...){clear();throw;}
}

// Non-control Value instructions, out of line so the dispatch loop stays small: each runs
// the frame VM's steps for its bytecode instruction (see the file comment).
void VirtualMachine::register_value_op(const RegisterCode& c,std::size_t at,RegisterValue* r,Value* v,std::vector<ValueLocation>& places){
    const Ins& i=c.code[at];
    auto V=[&](std::uint16_t x)->const Value&{return (x&const_tag)?c.value_constants[x&index_mask]:v[x];};
    auto operand=[&](Kind k,std::uint16_t x)->Value{return k==Kind::Any?V(x):box(r[x],k);};
    auto take=[&](const Ins& i,std::uint8_t f,Kind k,std::uint16_t x)->Value{if(k==Kind::Any&&(i.temps&f))return std::move(v[x]);return operand(k,x);};
    auto put=[&](Kind k,std::uint16_t dst,Value value,std::size_t at){
        if(k==Kind::Any){v[dst]=std::move(value);return;}
        if(!unbox(value,k,r[dst]))runtime_error(c.span(at),"register VM result kind mismatch","E8002");
    };
    switch(i.op){
            case R::CallBuiltin:{
                const auto& b=c.builtins[i.t];const auto& call=*c.origin[at];
                // Arguments are consumed temporaries: moved into a pooled vector, as pop_arguments does.
                ArgumentLease lease(*this);auto& args=lease.values;args.reserve(i.c);
                for(std::size_t k=0;k<i.c;++k){if(b.kinds[k]==Kind::Any){args.push_back(std::move(v[i.d+k]));popped(args.back(),call.span);}else args.push_back(box(r[i.b+k],b.kinds[k]));}
                HUA_PROFILE_COUNT(builtin_calls);
                auto out=invoke_builtin(*b.info,b.name,args,call.span,output_,call.contextual,call.type,context_);
                if(i.x!=Kind::Void)put(i.x,i.a,std::move(out),at);
                break;}
            case R::VMov:v[i.a]=V(i.b);break;
            case R::VLoad:{auto& local=v[i.b];if(auto p=cget_if<PlainRecord>(&local.data);p&&p->writable)materialize_record(local);v[i.a]=local;break;}
            case R::VLoadGlobal:{auto& value=c.globals[i.t]->value;if(auto p=cget_if<PlainRecord>(&value.data);p&&p->writable)materialize_record(value);v[i.a]=value;break;}
            case R::VBind:{
                const auto& op=*c.origin[at];const Kind sk=static_cast<Kind>((i.t>>8)&0xf);
                HUA_PROFILE_CONSTRAINT_SOURCE(binding);
                Value value=take(i,fb,sk,i.b);popped(value,op.span);
                value=enforce_type(std::move(value),op.type,op.span,(i.t&2)!=0);
                const auto kind=value_kind(value);
                if(i.x!=Kind::Any||(kind>=1&&kind<=3)){put(i.x,i.a,std::move(value),at);break;}
                if(hua::holds_alternative<MultiValue>(value.data))runtime_error(op.span,"multiple values require positional binding","E4003");
                value=copy_value(std::move(value));if(!(i.t&1))value=read_only(std::move(value));
                v[i.a]=std::move(value);break;}
            case R::VStore:{
                const auto& op=*c.origin[at];const Kind sk=static_cast<Kind>((i.t>>8)&0xf);const bool compound=i.t&1;
                const auto& type=c.types[(i.t>>16)-1];
                Value value=take(i,fb,sk,i.b);popped(value,op.span);
                if(compound)value=binary_value(std::string_view(op.text).substr(0,op.text.size()-1),operand(i.x,i.a),value,op.span,op.flag);
                value=enforce_type(std::move(value),type,op.span,op.flag||compound);
                put(i.x,i.a,copy_value(value),at);break;}
            case R::VBinary:{
                const auto& op=*c.origin[at];const Kind lk=static_cast<Kind>((i.t>>24)&0xf),rk=static_cast<Kind>((i.t>>20)&0xf);
                Value right=take(i,fc,rk,i.c);popped(right,op.span);Value left=take(i,fb,lk,i.b);popped(left,op.span);
                v[i.a]=binary_value(op.text,left,right,op.span,op.flag);break;}
            case R::VUnary:{const auto& op=*c.origin[at];Value value=take(i,fb,Kind::Any,i.b);popped(value,op.span);v[i.a]=unary_value(op.text,value,op.span);break;}
            case R::VCheck:popped(V(i.a),c.span(at));break;
            case R::VMember:{
                const auto& op=*c.origin[at];const auto& s=op.span;
                // A struct local read in place (the frame VM's Load+Member superinstruction);
                // otherwise a local is loaded as Load does (PlainRecord materialized) first.
                if(i.x==Kind::Any&&!(i.temps&fb)&&!(i.b&const_tag)){
                    auto& local=v[i.b];
                    if(local.data.index()==6&&op.text!="len"){
                        auto& object=hua::get<StructValue>(local.data);auto& cache=c.cache(at);
                        if(auto* item=read_field(cache.field,object,op.text)){
                            if(hua::holds_alternative<StructValue>(item->data)||holds<PlainRecord>(item->data)){
                                auto* alias=field(cache.field,object,op.text);retain_record_alias(*alias);item=alias;}
                            if(object.writable&&object.fields_readonly)[[unlikely]]{v[i.a]=copy_value(read_only(*item));break;}
                            v[i.a]=object.writable?*item:read_only(*item);break;}
                    }
                    if(auto p=cget_if<PlainRecord>(&local.data);p&&p->writable)materialize_record(local);
                }
                Value value=take(i,fb,i.x,i.b);popped(value,s);
                if(op.text=="len"&&(holds<SliceValue>(value.data)||hua::holds_alternative<MapValue>(value.data)||is_string(value.data))){v[i.a]=aggregate_member(value,op.text,s);break;}
                if(auto p=cget_if<PlainRecord>(&value.data)){if(auto item=plain_record_field(*p,op.text)){v[i.a]=std::move(*item);break;}}
                auto object=materialize_record(value);if(!object)runtime_error(s,"member access requires a struct","E4003");
                auto& cache=c.cache(at);
                if(auto* item=read_field(cache.field,*object,op.text)){
                    if(hua::holds_alternative<StructValue>(item->data)||holds<PlainRecord>(item->data)){
                        auto* alias=field(cache.field,*object,op.text);retain_record_alias(*alias);item=alias;
                    }
                    if(object->writable&&object->fields_readonly)[[unlikely]]{v[i.a]=copy_value(read_only(*item));break;}
                    v[i.a]=object->writable?*item:read_only(*item);break;}
                auto& route=cache.method;
                if(!route.method||route.owner!=object->data->name){
                    auto method=model_.functions.find(object->data->name+"."+op.text);
                    if(method==model_.functions.end())runtime_error(s,"unknown struct member","E4003");
                    if(method->second->span.file_id!=s.file_id && method->second->text.find(" pub")==std::string::npos)
                        runtime_error(s,"method is private to its defining module","E5003");
                    auto m=model_.mutating.find(method->first);
                    route.owner=object->data->name;route.method=method->second;route.mutating=m!=model_.mutating.end()&&m->second;
                }
                // A method value that outlives this instruction keeps a live receiver alias.
                if(route.mutating){object->data->no_share=true;if(object->path)object->path->each([](auto& body){body->no_share=true;});}
                v[i.a]=Value(Callable{route.method,"",*object});break;}
            case R::VIndex:{
                const auto& s=c.span(at);const Kind bk=static_cast<Kind>((i.t>>24)&0xf),ik=static_cast<Kind>((i.t>>20)&0xf);
                Value index=take(i,fc,ik,i.c);popped(index,s);Value value=take(i,fb,bk,i.b);popped(value,s);
                v[i.a]=index_value(value,index,s);break;}
            case R::VCheckIndex:{
                Value value=operand(i.x,i.a);
                if(!holds<SliceValue>(value.data)&&!hua::holds_alternative<MapValue>(value.data))runtime_error(c.span(at),"index/slice requires a slice, array or map","E4003");break;}
            case R::VCheckSlice:{
                Value value=operand(i.x,i.a);
                if(!holds<SliceValue>(value.data))runtime_error(c.span(at),"index/slice requires a slice or array","E4003");break;}
            case R::VBox:v[i.a]=box(r[i.b],i.x);break;
            case R::VLocateIndex:{
                const auto& s=c.span(at);const Kind bk=static_cast<Kind>((i.t>>24)&0xf),ik=static_cast<Kind>((i.t>>20)&0xf);
                Value index=take(i,fc,ik,i.c);popped(index,s);Value value=take(i,fb,bk,i.b);popped(value,s);
                if(places.size()<c.locations)places.resize(c.locations);
                auto l=index_location(std::move(value),std::move(index),s);
                if(!l.writable)runtime_error(s,"cannot mutate immutable binding or read-only view","E4007");
                places[i.a]=std::move(l);break;}
            case R::VStoreLocation:{
                const auto& op=*c.origin[at];const auto& s=op.span;
                Value value=take(i,fb,i.x,i.b);popped(value,s);
                auto where=std::move(places[i.c]);places[i.c]=ValueLocation{};
                if(op.text!="=")value=binary_value(std::string_view(op.text).substr(0,op.text.size()-1),location_read(where,s),value,s,op.flag);
                value=enforce_type(std::move(value),where.type,s,op.flag||op.text!="=");location_write(where,value,s);
                v[i.a]=std::move(value);break;}
            case R::VMakeArray:{auto storage=managed<std::vector<Value>>();v[i.a]=Value(SliceValue{storage,0,0,true,""});break;}
            case R::VAppend:{
                const auto& op=*c.origin[at];const auto& s=op.span;
                Value item=take(i,fb,i.x,i.b);popped(item,s);item=copy_value(std::move(item));
                auto slice=mget_if<SliceValue>(&v[i.a].data);if(!slice)invalid(s);
                if(slice->element_type.empty())slice->element_type=value_type(item);else item=enforce_type(item,slice->element_type,s,op.flag);
                slice->storage->push_back(std::move(item));slice->contextual.push_back(op.flag);++slice->length;break;}
            case R::VMakeStruct:{
                const auto& op=*c.origin[at];const auto& s=op.span;
                if(op.text=="std.error.Value")runtime_error(s,"standard Error must be constructed through std.error","E4003");
                if(op.text=="std.file.File"||op.text=="std.file.IOFailure")runtime_error(s,"opaque File/IOFailure must be constructed through std.file","E4003");
                auto object=managed<StructData>();object->name=op.text;
                auto [layout,inserted]=enum_layouts_.try_emplace(op.text);
                if(inserted)layout->second=enum_layout(*model_.structures.at(op.text));
                object->enumeration=layout->second;
                if(!object->enumeration){auto [shape,fresh]=record_layouts_.try_emplace(op.text);if(fresh)shape->second=record_layout(*model_.structures.at(op.text));object->fields=RecordFields(shape->second);}
                v[i.a]=Value(StructValue{object,true});break;}
            case R::VInitField:{
                const auto& op=*c.origin[at];const auto& s=op.span;
                Value item=take(i,fb,i.x,i.b);popped(item,s);
                auto object=hua::get_if<StructValue>(&v[i.a].data);if(!object)invalid(s);
                auto value=enforce_type(std::move(item),field_type(c.cache(at).field,object->data->name,op.text,s),s,op.flag);
                initialize_field(*object->data,op.text,std::move(value),s);break;}
            case R::VMakeMap:{const auto& op=*c.origin[at];v[i.a]=make_map(op.type,op.span);break;}
            case R::VMapInsert:{
                const auto& op=*c.origin[at];const auto& s=op.span;const Kind kk=static_cast<Kind>((i.t>>24)&0xf),ik=static_cast<Kind>((i.t>>20)&0xf);
                Value item=take(i,fc,ik,i.c);popped(item,s);Value key=take(i,fb,kk,i.b);popped(key,s);
                auto m=hua::get_if<MapValue>(&v[i.a].data);if(!m)invalid(s);map_put(*m,key,std::move(item),s,op.flag);break;}
            case R::VSliceInit:{
                const auto& s=c.span(at);
                Value value=take(i,fb,i.x,i.b);popped(value,s);
                r[i.d].i=0;r[i.d+1].i=0;r[i.d+2].i=0;
                if(auto m=hua::get_if<MapValue>(&value.data)){
                    auto entries=managed<std::vector<Value>>();
                    for(const auto& [k,x]:m->data->entries)entries->push_back(make_multi({hua::visit([](const auto& y){return Value(y);},k),copy_value(m->writable?x:read_only(x))}));
                    v[i.a]=Value(SliceValue{entries,0,entries->size(),false,""});r[i.d+2].i=1;break;}
                if(!holds<SliceValue>(value.data))runtime_error(s,"for requires a slice or range","E4003");
                v[i.a]=std::move(value);break;}
    default:invalid(c.span(at));
    }
}
}
