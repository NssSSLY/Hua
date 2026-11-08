// R1: typed register execution for the scalar subset of verified bytecode.
//
// A function is translated once, at its first call, from its stack bytecode into code for
// 8-byte registers whose types are all known statically: parameters, locals and int range
// iterators get fixed registers, every operand-stack depth d maps to temporary T(d), and
// constants live in a per-function register block copied in at entry. Block scopes vanish
// (names resolve at translation time) and no instruction checks a type at run time (D5,
// 2026-11-08: the checker's types are trusted for verified code; argument kinds are
// checked once where the frame-based VM calls in).
//
// Supported: int/float/bool constants, locals and parameters; Bind/Load/LocateName+Store
// (plain and compound); Binary/Unary; Jump/JumpFalse/JumpTrue; Enter/LeaveScope/Unwind;
// int ranges (RangeInit/IterNext/IterEnd with one loop name); direct calls to translated
// functions; `print` statements; Return. Everything else rejects the whole function, and
// a function calling a rejected one is rejected too, so such calls keep the old path.
//
// Exactness: fast int/float operations cover only the cases where the result is plainly
// representable; any other case (overflow, division by zero, a non-finite float, other
// operators) evaluates through binary_value/unary_value, which produce the frame VM's
// value or diagnostic. Errors are located at the failing instruction and gain one
// "called from" per register frame, as frame VM traces do. Budget steps count register
// instructions (D4).
#include "hua/vm.hpp"
#include "hua/runtime.hpp"
#include "hua/tasks.hpp"
#include "hua/file.hpp"
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
enum class Kind : std::uint8_t { Void, Int, Float, Bool, Function, Builtin, Target, Bad };
Kind scalar_kind(std::string_view type){
    if(type=="int")return Kind::Int;if(type=="float")return Kind::Float;if(type=="bool")return Kind::Bool;return Kind::Bad;
}
enum class R : std::uint8_t {
    Mov, IntToFloat,
    AddI, SubI, MulI, FloorDivI, ModI, LtI, LeI, GtI, GeI, EqI, NeI,
    AddF, SubF, MulF, DivF, LtF, LeF, GtF, GeF, EqF, NeF,
    EqB, NeB, NotB, NegI, NegF,
    Binary, Unary, // general evaluation (binary_value/unary_value); x = result kind
    Jump, JumpFalse, JumpTrue,
    RangeInit, IterNext,
    Call, Print, Return, ReturnVoid,
};
struct Ins { R op; Kind x{}; std::uint16_t a{}, b{}, c{}; std::uint32_t t{}; };
constexpr std::uint16_t temp_tag=0x8000, const_tag=0x4000, index_mask=0x3fff;
Value box(RegisterValue v,Kind k){
    switch(k){case Kind::Int:return Value(v.i);case Kind::Float:return Value(v.f);case Kind::Bool:return Value(v.i!=0);default:return Value{};}
}
bool unbox(const Value& v,Kind k,RegisterValue& out){
    switch(k){
    case Kind::Int:if(auto p=std::get_if<std::int64_t>(&v.data)){out.i=*p;return true;}return false;
    case Kind::Float:if(auto p=std::get_if<double>(&v.data)){out.f=*p;return true;}return false;
    case Kind::Bool:if(auto p=std::get_if<bool>(&v.data)){out.i=*p;return true;}return false;
    default:return false;
    }
}
Kind kind_of(const Value& v){
    switch(v.data.index()){case 1:return Kind::Bool;case 2:return Kind::Int;case 3:return Kind::Float;default:return Kind::Bad;}
}
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
    const Node* function{};
    std::uint16_t parameters{}, registers{}, constant_base{};
    std::vector<Kind> parameter_kinds;
    Kind result{Kind::Void};
    std::vector<const Node*> callees;
    std::vector<RegisterCode*> callee_code;
    struct Builtin { std::string name; const BuiltinInfo* info{}; std::vector<Kind> kinds; };
    std::vector<Builtin> builtins;
    std::vector<std::string_view> texts; // operator text per general/fast arithmetic Ins (by t)
    const SourceSpan* span(std::size_t pc) const {return &origin[pc]->span;}
    std::vector<Kind> call_kinds; // declared result kind per callee index
    Kind code_result(std::size_t callee) const {return call_kinds[callee];}
};
struct RegisterTranslator {
    using RegisterCode=VirtualMachine::RegisterCode;
    struct Entry { Kind kind{Kind::Bad}; std::uint16_t reg{}; const Node* function{}; std::string builtin; Kind target_kind{}; bool target_mutable{}; };
    struct Local { std::uint16_t reg; Kind kind; bool mutable_binding; };
    const Code& code;RegisterCode& out;
    std::function<const VirtualMachine::Binding*(const std::string&)> global;
    std::function<Kind(const Node*)> result_of;
    std::vector<Entry> stack;
    std::vector<std::unordered_map<std::string,Local>> scopes;
    std::vector<std::uint16_t> iterators;
    std::uint16_t locals{},max_depth{};
    std::unordered_map<std::size_t,std::vector<Kind>> labels; // stack kinds at a branch target
    std::unordered_map<std::size_t,std::size_t> placed;       // bytecode pc -> register pc
    std::vector<std::pair<std::size_t,std::size_t>> fixups;    // register pc, bytecode target
    std::vector<bool> is_target;
    std::size_t pc{};
    bool failed{};
    struct Reject {};
    [[noreturn]] void reject(){throw Reject{};}
    static std::uint16_t T(std::size_t depth){return static_cast<std::uint16_t>(temp_tag|depth);}
    std::uint16_t constant(RegisterValue v){
        for(std::size_t i=0;i<out.constants.size();++i)if(out.constants[i].i==v.i)return static_cast<std::uint16_t>(const_tag|i);
        if(out.constants.size()>=index_mask)reject();
        out.constants.push_back(v);return static_cast<std::uint16_t>(const_tag|(out.constants.size()-1));
    }
    std::uint16_t new_local(){if(locals>=index_mask)reject();return locals++;}
    void emit(Ins i){out.code.push_back(i);out.origin.push_back(&code.instructions[pc]);}
    bool scalar(Kind k){return k==Kind::Int||k==Kind::Float||k==Kind::Bool;}
    void push(Entry e){stack.push_back(std::move(e));if(stack.size()>max_depth)max_depth=static_cast<std::uint16_t>(stack.size());if(max_depth>=index_mask)reject();}
    Entry pop(){if(stack.empty())reject();auto e=std::move(stack.back());stack.pop_back();return e;}
    // Put the entry at depth d into T(d) (a branch or a call needs canonical temporaries).
    void canonical(std::size_t d){auto& e=stack[d];if(!scalar(e.kind))reject();if(e.reg!=T(d)){emit({R::Mov,e.kind,T(d),e.reg});e.reg=T(d);}}
    void canonical_all(){for(std::size_t d=0;d<stack.size();++d)canonical(d);}
    // Before local `reg` changes, entries still reading it get their own copy.
    void detach(std::uint16_t reg){for(std::size_t d=0;d<stack.size();++d)if(stack[d].reg==reg&&scalar(stack[d].kind)&&!(stack[d].reg&temp_tag))canonical(d);}
    std::vector<Kind> kinds(){std::vector<Kind> k;for(auto& e:stack)k.push_back(e.kind);return k;}
    void branch_to(std::size_t target){
        canonical_all();auto k=kinds();
        if(auto found=labels.find(target);found!=labels.end()){if(found->second!=k)reject();}else labels.emplace(target,std::move(k));
        fixups.emplace_back(out.code.size()-1,target);
    }
    const Local* find(const std::string& name){for(auto s=scopes.rbegin();s!=scopes.rend();++s)if(auto f=s->find(name);f!=s->end())return &f->second;return nullptr;}
    // Fast forms for the common scalar operators; anything else evaluates generally.
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
        Ins i{op,k,dst,l.reg,r.reg,static_cast<std::uint32_t>(out.texts.size())};
        i.x=k;out.texts.push_back(text);
        // operand kinds for the general path ride in the high bits of t
        i.t|=(static_cast<std::uint32_t>(l.kind)<<24)|(static_cast<std::uint32_t>(r.kind)<<20)|(flag?1u<<28:0u);
        emit(i);result=k;
    }
    void run(const VirtualMachine::FunctionLayout& layout){
        const auto& ins=code.instructions;is_target.assign(ins.size()+1,false);
        for(const auto& i:ins){if(i.op==Op::Jump||i.op==Op::JumpFalse||i.op==Op::JumpTrue||i.op==Op::IterNext){if(i.target>ins.size())reject();is_target[i.target]=true;}}
        scopes.emplace_back();
        for(const auto& p:layout.parameters){
            auto k=scalar_kind(p.type);if(k==Kind::Bad||p.mutable_view)reject();
            out.parameter_kinds.push_back(k);scopes.back()[p.name]=Local{new_local(),k,false};
        }
        out.parameters=locals;
        if(layout.returns.size()>1)reject();
        out.result=layout.returns.empty()?Kind::Void:scalar_kind(layout.returns[0]);if(out.result==Kind::Bad)reject();
        bool reachable=true;
        for(pc=0;pc<ins.size();++pc){
            const auto& op=ins[pc];
            if(is_target[pc]){
                if(reachable){canonical_all();auto k=kinds();if(auto f=labels.find(pc);f!=labels.end()){if(f->second!=k)reject();}else labels.emplace(pc,k);}
                else if(auto f=labels.find(pc);f!=labels.end()){stack.clear();for(std::size_t d=0;d<f->second.size();++d)push(Entry{f->second[d],T(d)});reachable=true;}
                placed[pc]=out.code.size();
            }
            // Scope and iterator structure is lexical: track it on unreachable paths too.
            if(op.op==Op::EnterScope){scopes.emplace_back();continue;}
            if(op.op==Op::LeaveScope){if(scopes.size()<2)reject();scopes.pop_back();continue;}
            if(op.op==Op::Unwind)continue;
            if(op.op==Op::IterEnd){if(iterators.empty())reject();iterators.pop_back();continue;}
            if(!reachable){
                if(op.op==Op::IterNext)reject();
                if(op.op==Op::RangeInit){iterators.push_back(0);}
                continue;
            }
            switch(op.op){
            case Op::Constant:{
                const auto& v=op.constant;
                if(v.data.index()==0){push(Entry{Kind::Void,0});break;}
                auto k=kind_of(v);if(k==Kind::Bad)reject();
                RegisterValue r{};unbox(v,k,r);push(Entry{k,constant(r)});break;}
            case Op::Load:{
                if(auto local=find(op.text)){push(Entry{local->kind,local->reg});break;}
                auto binding=global(op.text);if(!binding)reject();
                auto c=cget_if<Callable>(&binding->value.data);if(!c||c->extra)reject();
                if(c->function){push(Entry{Kind::Function,0,c->function});break;}
                if(c->builtin.empty())reject();
                Entry e{Kind::Builtin,0};e.builtin=c->builtin.str();push(std::move(e));break;}
            case Op::Bind:{
                auto v=pop();if(!scalar(v.kind))reject();
                Kind k=op.type.empty()?v.kind:scalar_kind(op.type);if(k==Kind::Bad)reject();
                auto reg=new_local();
                if(k==v.kind)emit({R::Mov,k,reg,v.reg});
                else if(k==Kind::Float&&v.kind==Kind::Int&&!op.contextual.empty()&&op.contextual[0])emit({R::IntToFloat,k,reg,v.reg});
                else reject();
                if(scopes.back().contains(op.text))reject();
                scopes.back()[op.text]=Local{reg,k,op.flag};break;}
            case Op::LocateName:{
                auto local=find(op.text);if(!local)reject();
                Entry e{Kind::Target,local->reg};e.target_kind=local->kind;e.target_mutable=local->mutable_binding;push(std::move(e));break;}
            case Op::Store:{
                auto v=pop();auto target=pop();if(target.kind!=Kind::Target||!target.target_mutable||!scalar(v.kind))reject();
                const auto reg=target.reg;const Kind k=target.target_kind;
                detach(reg);
                if(op.text=="="){
                    if(v.kind==k)emit({R::Mov,k,reg,v.reg});
                    else if(k==Kind::Float&&v.kind==Kind::Int&&op.flag)emit({R::IntToFloat,k,reg,v.reg});
                    else reject();
                }else{
                    if(op.text.size()<2||op.text.back()!='=')reject();
                    Kind result;binary(std::string_view(op.text).substr(0,op.text.size()-1),op.flag,reg,Entry{k,reg},v,result);
                    if(result!=k)reject();
                }
                push(Entry{k,reg});break;}
            case Op::Binary:{
                auto r=pop();auto l=pop();if(!scalar(l.kind)||!scalar(r.kind))reject();
                Kind k;binary(op.text,op.flag,T(stack.size()),l,r,k);push(Entry{k,T(stack.size())});break;}
            case Op::Unary:{
                auto v=pop();if(!scalar(v.kind))reject();const auto dst=T(stack.size());
                const Kind general=general_unary(op.text,v.kind);if(general==Kind::Bad)reject();
                Ins i{R::Unary,general,dst,v.reg,0,static_cast<std::uint32_t>(out.texts.size())};out.texts.push_back(op.text);
                if(op.text=="!"&&v.kind==Kind::Bool&&general==Kind::Bool)i.op=R::NotB;
                else if(op.text=="-"&&v.kind==Kind::Int&&general==Kind::Int)i.op=R::NegI;
                else if(op.text=="-"&&v.kind==Kind::Float&&general==Kind::Float)i.op=R::NegF;
                i.t|=static_cast<std::uint32_t>(v.kind)<<24;emit(i);push(Entry{general,dst});break;}
            case Op::Pop:{auto v=pop();if(v.kind==Kind::Function||v.kind==Kind::Builtin||v.kind==Kind::Target)reject();break;}
            case Op::Jump:canonical_all();emit({R::Jump});branch_to(op.target);reachable=false;break;
            case Op::JumpFalse:case Op::JumpTrue:{
                auto v=pop();if(v.kind!=Kind::Bool)reject();
                canonical_all();emit({op.op==Op::JumpFalse?R::JumpFalse:R::JumpTrue,Kind::Bool,v.reg});branch_to(op.target);break;}
            case Op::RangeInit:{
                auto step=pop(),end=pop(),start=pop();
                if(start.kind!=Kind::Int||end.kind!=Kind::Int||step.kind!=Kind::Int)reject();
                std::uint16_t base=new_local();new_local();new_local();new_local(); // current, end, step, done
                Ins i{R::RangeInit,op.flag?Kind::Bool:Kind::Void,base,start.reg,end.reg};i.t=step.reg;emit(i);
                iterators.push_back(base);break;}
            case Op::IterNext:{
                if(iterators.empty()||op.names.size()!=1)reject();
                const auto base=iterators.back();
                canonical_all();
                const auto item=new_local();
                // inclusive flag was recorded on RangeInit; IterNext reads it from x
                Kind inclusive=Kind::Void;for(auto r=out.code.rbegin();r!=out.code.rend();++r)if(r->op==R::RangeInit&&r->a==base){inclusive=r->x;break;}
                emit({R::IterNext,inclusive,base,item});branch_to(op.target);
                scopes.emplace_back();scopes.back()[op.names[0]]=Local{item,Kind::Int,false};break;}
            case Op::Call:{
                const std::size_t n=op.argument;if(stack.size()<n+1)reject();
                const std::size_t base=stack.size()-n-1;auto& callee=stack[base];
                if(callee.kind==Kind::Function){
                    std::size_t index=out.callees.size();out.callees.push_back(callee.function);
                    for(std::size_t d=base+1;d<stack.size();++d)canonical(d);
                    std::vector<Kind> args;for(std::size_t d=base+1;d<stack.size();++d)args.push_back(stack[d].kind);
                    pending_calls.push_back({index,std::move(args)});
                    emit({R::Call,Kind::Void,T(base),T(base+1),static_cast<std::uint16_t>(n),static_cast<std::uint32_t>(index)});
                    const Kind result=result_of(out.callees[index]);if(result==Kind::Bad)reject();out.call_kinds.push_back(result);
                    out.code.back().x=result;
                    stack.resize(base);push(Entry{result,result==Kind::Void?std::uint16_t(0):T(base)});
                    break;
                }
                if(callee.kind!=Kind::Builtin||pc+1>=ins.size()||ins[pc+1].op!=Op::Pop)reject();
                const auto& info=builtin_info(callee.builtin);
                if(info.core!=CoreBuiltin::print||builtin_may_suspend(info,nullptr)||info.suspends_in_task)reject();
                RegisterCode::Builtin b{callee.builtin,&info,{}};
                for(std::size_t d=base+1;d<stack.size();++d){canonical(d);b.kinds.push_back(stack[d].kind);}
                out.builtins.push_back(std::move(b));
                emit({R::Print,Kind::Void,0,T(base+1),static_cast<std::uint16_t>(n),static_cast<std::uint32_t>(out.builtins.size()-1)});
                stack.resize(base);push(Entry{Kind::Void,0});break;}
            case Op::Return:{
                auto v=pop();
                if(out.result==Kind::Void){if(v.kind!=Kind::Void)reject();emit({R::ReturnVoid});}
                else{if(v.kind!=out.result)reject();emit({R::Return,v.kind,v.reg});}
                reachable=false;break;}
            default:reject();
            }
        }
        if(reachable)reject(); // falling off the end is not verified bytecode
        for(auto [at,target]:fixups){auto f=placed.find(target);if(f==placed.end())reject();out.code[at].t=static_cast<std::uint32_t>(f->second);}
    }
    // Call result kinds are known once callees are translated (see register_code).
    struct PendingCall { std::size_t callee; std::vector<Kind> args; };
    std::vector<PendingCall> pending_calls;
    void finish(){
        const std::uint16_t temps=locals;
        if(temps+max_depth+out.constants.size()>=0x3fff)reject();
        out.constant_base=static_cast<std::uint16_t>(temps+max_depth);
        out.registers=static_cast<std::uint16_t>(out.constant_base+out.constants.size());
        auto map=[&](std::uint16_t r)->std::uint16_t{if(r&temp_tag)return static_cast<std::uint16_t>(temps+(r&index_mask));if(r&const_tag)return static_cast<std::uint16_t>(out.constant_base+(r&index_mask));return r;};
        for(auto& i:out.code){
            switch(i.op){
            case R::Jump:break;
            case R::JumpFalse:case R::JumpTrue:case R::Return:i.a=map(i.a);break;
            case R::RangeInit:i.a=map(i.a);i.b=map(i.b);i.c=map(i.c);i.t=map(static_cast<std::uint16_t>(i.t));break;
            case R::IterNext:i.a=map(i.a);i.b=map(i.b);break;
            case R::Call:case R::Print:i.a=map(i.a);i.b=map(i.b);break;
            case R::ReturnVoid:break;
            default:i.a=map(i.a);i.b=map(i.b);i.c=map(i.c);break;
            }
        }
    }
};

VirtualMachine::RegisterCode* VirtualMachine::register_code(const Node& root){
    if(auto f=register_codes_.find(&root);f!=register_codes_.end())return f->second.get();
    // Translate root and every callee it reaches; then reject, to a fixpoint, functions
    // whose callees did not translate or whose call argument kinds do not match.
    std::vector<const Node*> work{&root},fresh;
    std::unordered_map<const Node*,std::vector<RegisterTranslator::PendingCall>> calls;
    while(!work.empty()){
        const Node* fn=work.back();work.pop_back();
        if(register_codes_.contains(fn))continue;
        auto body=bytecode_?bytecode_->functions.find(fn):decltype(bytecode_->functions.end()){};
        std::shared_ptr<RegisterCode> code;
        if(bytecode_&&body!=bytecode_->functions.end()&&!model_.nested.contains(fn)){
            const auto& layout=function_layout(*fn);
            if(!layout.external&&!layout.unsafe){
                code=std::make_shared<RegisterCode>();code->function=fn;
                RegisterTranslator t{body->second,*code,[this](const std::string& name)->const Binding*{
                    for(auto env=global_.get();env;env=env->parent.get())if(auto b=env->own(name))return b;return nullptr;},
                    [this](const Node* callee)->Kind{
                        const auto& l=function_layout(*callee);
                        if(l.returns.empty())return Kind::Void;if(l.returns.size()>1)return Kind::Bad;return scalar_kind(l.returns[0]);}};
                try{t.run(layout);t.finish();calls[fn]=std::move(t.pending_calls);}
                catch(const RegisterTranslator::Reject&){code.reset();}
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
            for(const auto& call:calls[fn]){
                auto f=register_codes_.find(code->callees[call.callee]);
                auto* callee=f==register_codes_.end()?nullptr:f->second.get();
                if(!callee||callee->parameter_kinds!=call.args||callee->result!=code->code_result(call.callee)){ok=false;break;}
                code->callee_code[call.callee]=callee;
            }
            if(!ok){code.reset();changed=true;}
        }
    }
    for(auto fn:fresh)if(auto& code=register_codes_[fn])register_max_=std::max<std::size_t>(register_max_,code->registers);
    return register_codes_[&root].get();
}

bool VirtualMachine::register_call(const Callable& callable,const std::vector<Value>& args,const Instruction& site,const FunctionLayout& layout,std::size_t user_calls){
    // HUA_NO_REGISTER_VM=1 keeps every call on the frame path (differential testing).
    static const bool disabled=[]{const char* v=std::getenv("HUA_NO_REGISTER_VM");return v&&*v&&*v!='0';}();
    if(disabled||(context_&&context_->in_task)||callable.extra||!callable.function||!bytecode_)return false;
    auto* code=register_code(*callable.function);if(!code)return false;
    if(args.size()!=code->parameters)return false;
    for(std::size_t i=0;i<args.size();++i){Kind k=kind_of(args[i]);if(k!=code->parameter_kinds[i])return false;}
    (void)layout;
    const std::size_t limit=context_?context_->execution_limits.calls:128;
    const std::size_t needed=(limit+2)*register_max_;
    const bool outermost=register_user_base_==0;
    if(outermost&&register_file_.size()<needed)register_file_.assign(needed,RegisterValue{});
    if(register_file_.empty())return false;
    std::size_t saved=register_user_base_;
    // The frame base for nested entry (a callback re-entering) starts after the live ones.
    RegisterValue* regs=register_file_.data();
    if(!outermost)return false; // nested re-entry keeps the frame path in this prototype
    register_user_base_=user_calls+1;
    for(std::size_t i=0;i<args.size();++i){if(code->parameter_kinds[i]==Kind::Float)regs[i].f=std::get<double>(args[i].data);else if(code->parameter_kinds[i]==Kind::Bool)regs[i].i=std::get<bool>(args[i].data);else regs[i].i=std::get<std::int64_t>(args[i].data);}
    // A call observer sees the same enter/leave events, with flushed metrics, as frames give.
    if(observer_){flush_metrics();observer_->enter(callable.function,frames_.empty()?nullptr:frames_.back().function,args);}
    std::int64_t raw;
    try{raw=run_registers(*code,regs,0,site.span);}
    catch(const Untraced& e){register_user_base_=saved;flush_metrics();throw e.error;}
    catch(...){register_user_base_=saved;flush_metrics();throw;}
    register_user_base_=saved;flush_metrics();
    RegisterValue r;r.i=raw;Value value=box(r,code->result);
    if(!frames_.empty())stack_.emplace_back(std::move(value));else completed_=std::move(value);
    if(observer_){flush_metrics();observer_->leave(callable.function,frames_.empty()?(completed_?&*completed_:nullptr):std::get_if<Value>(&stack_.back()));}
    return true;
}

std::int64_t VirtualMachine::run_registers(const RegisterCode& c,RegisterValue* r,std::size_t depth,const SourceSpan& site){
    for(std::size_t i=0;i<c.constants.size();++i)r[c.constant_base+i]=c.constants[i];
    const Ins* code=c.code.data();std::size_t pc=0;
    const std::size_t step_limit=context_?context_->execution_limits.steps:std::numeric_limits<std::size_t>::max();
    const std::size_t call_limit=context_?context_->execution_limits.calls:128;
    unsigned poll=256;
    auto general=[&](const Ins& i,std::size_t at){
        const Kind lk=static_cast<Kind>((i.t>>24)&0xf),rk=static_cast<Kind>((i.t>>20)&0xf);const bool flag=(i.t>>28)&1;
        auto out=binary_value(c.texts[i.t&0xfffff],box(r[i.b],lk),box(r[i.c],rk),*c.span(at),flag);
        if(!unbox(out,i.x,r[i.a]))runtime_error(*c.span(at),"register VM result kind mismatch","E8002");
    };
    try{
        for(;;){
            const Ins& i=code[pc++];
            ++pending_metric_steps_;
            if(++steps_>step_limit){flush_metrics();throw Untraced{Diagnostic("E4099",*c.span(pc-1),"execution step limit exceeded ("+std::to_string(step_limit)+")")};}
            if(--poll==0){poll=256;flush_metrics();try{cancellation_checkpoint(context_,*c.span(pc-1));}catch(const Diagnostic& e){throw Untraced{e};}}
            switch(i.op){
            case R::Mov:r[i.a]=r[i.b];break;
            case R::IntToFloat:r[i.a].f=static_cast<double>(r[i.b].i);break;
            case R::AddI:{std::int64_t v;if(__builtin_add_overflow(r[i.b].i,r[i.c].i,&v)){general(i,pc-1);break;}r[i.a].i=v;break;}
            case R::SubI:{std::int64_t v;if(__builtin_sub_overflow(r[i.b].i,r[i.c].i,&v)){general(i,pc-1);break;}r[i.a].i=v;break;}
            case R::MulI:{std::int64_t v;if(__builtin_mul_overflow(r[i.b].i,r[i.c].i,&v)){general(i,pc-1);break;}r[i.a].i=v;break;}
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
            case R::AddF:{double v=r[i.b].f+r[i.c].f;if(!std::isfinite(v)){general(i,pc-1);break;}r[i.a].f=v;break;}
            case R::SubF:{double v=r[i.b].f-r[i.c].f;if(!std::isfinite(v)){general(i,pc-1);break;}r[i.a].f=v;break;}
            case R::MulF:{double v=r[i.b].f*r[i.c].f;if(!std::isfinite(v)){general(i,pc-1);break;}r[i.a].f=v;break;}
            case R::DivF:{double v=r[i.c].f!=0?r[i.b].f/r[i.c].f:NAN;if(!std::isfinite(v)){general(i,pc-1);break;}r[i.a].f=v;break;}
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
                auto out=unary_value(c.texts[i.t&0xfffff],box(r[i.b],static_cast<Kind>((i.t>>24)&0xf)),*c.span(pc-1));
                if(!unbox(out,i.x,r[i.a]))runtime_error(*c.span(pc-1),"register VM result kind mismatch","E8002");break;}
            case R::Jump:pc=i.t;break;
            case R::JumpFalse:if(!r[i.a].i)pc=i.t;break;
            case R::JumpTrue:if(r[i.a].i)pc=i.t;break;
            case R::RangeInit:{
                const auto step=r[i.t].i;
                if(!step)runtime_error(*c.span(pc-1),"range step cannot be zero","E4005");
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
                const RegisterCode& callee=*c.callee_code[i.t];
                const std::size_t user_calls=register_user_base_+depth;
                if(user_calls>=call_limit)runtime_error(*c.span(pc-1),"function call depth limit exceeded ("+std::to_string(call_limit)+")","E4099");
                if(context_&&context_->execution_metrics)context_->execution_metrics->call(static_cast<unsigned>(user_calls+1));
                RegisterValue* next=r+c.registers;
                if(next+callee.registers>register_file_.data()+register_file_.size())runtime_error(*c.span(pc-1),"function call depth limit exceeded ("+std::to_string(call_limit)+")","E4099");
                for(std::size_t k=0;k<i.c;++k)next[k]=r[i.b+k];
                if(observer_)[[unlikely]]{
                    std::vector<Value> args;for(std::size_t k=0;k<i.c;++k)args.push_back(box(r[i.b+k],callee.parameter_kinds[k]));
                    flush_metrics();observer_->enter(callee.function,c.function,args);
                    r[i.a].i=run_registers(callee,next,depth+1,*c.span(pc-1));
                    Value result=box(r[i.a],callee.result);flush_metrics();observer_->leave(callee.function,&result);break;}
                r[i.a].i=run_registers(callee,next,depth+1,*c.span(pc-1));break;}
            case R::Print:{
                const auto& b=c.builtins[i.t];std::vector<Value> args;args.reserve(i.c);
                for(std::size_t k=0;k<i.c;++k)args.push_back(box(r[i.b+k],b.kinds[k]));
                const Instruction& call=*c.origin[pc-1];
                invoke_builtin(*b.info,b.name,args,call.span,output_,call.contextual,call.type,context_);break;}
            case R::Return:return r[i.a].i;
            case R::ReturnVoid:return 0;
            }
        }
    }catch(const Diagnostic& error){
        auto reported=error.located(*c.span(pc-1));
        reported.add_context(site,"called from");
        throw reported;
    }
}
}
