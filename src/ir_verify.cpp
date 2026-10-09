#include "hua/ir.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <set>
#include <optional>
namespace hua::ir {
namespace {
using H=HirKind;using M=MirOp;using T=TermKind;
constexpr std::size_t object_limit=250000, block_limit=4097;
constexpr std::uint32_t effect_mask=511;
void need(bool yes,const SourceSpan& s,const std::string& why){if(!yes)invalid(s,why);}
bool number(TypeId t){return t==int_type||t==float_type;}
bool scalar(TypeId t){return t.value>=1&&t.value<=4;}
bool converts(TypeId expected,TypeId actual,bool contextual){
    return expected==actual||(expected==float_type&&actual==int_type&&contextual);
}
void span(const Tables& t,const SourceSpan& s){
    need(s.line&&s.column&&s.start_offset<=s.end_offset,s,"source span");
    auto source=std::find_if(t.sources.begin(),t.sources.end(),[&](const auto& x){return x.filename==s.file_id;});
    need(source!=t.sources.end()&&s.end_offset<=source->bytes,s,"source identity/bounds");
}
void effects(std::uint32_t e,const SourceSpan& s){need((e&~effect_mask)==0,s,"effect bits");}
void constant(const ScalarConstant& v,TypeId t,const SourceSpan& s){
    bool okay=t==void_type?std::holds_alternative<std::monostate>(v.data):
        t==int_type?std::holds_alternative<std::int64_t>(v.data):
        t==bool_type?std::holds_alternative<bool>(v.data):
        t==float_type?std::holds_alternative<double>(v.data)&&std::isfinite(std::get<double>(v.data)):false;
    need(okay,s,"typed scalar constant");
}
void unary(const std::string& op,TypeId input,TypeId output,const SourceSpan& s){
    need(op=="!"?input==bool_type&&output==bool_type:
        (op=="+"||op=="-")?number(input)&&output==input:
        op=="~"?input==int_type&&output==int_type:false,s,"unary signature");
}
void binary(const std::string& op,TypeId a,TypeId b,TypeId result,const SourceSpan& s){
    bool okay=false;
    if(op=="&&"||op=="||")okay=a==bool_type&&b==bool_type&&result==bool_type;
    else if(op=="=="||op=="!=")okay=((number(a)&&number(b))||(a==bool_type&&b==bool_type))&&result==bool_type;
    else if(op=="<"||op=="<="||op==">"||op==">=")okay=number(a)&&number(b)&&result==bool_type;
    else if(op=="&"||op=="|"||op=="^"||op=="<<"||op==">>")okay=a==int_type&&b==int_type&&result==int_type;
    else if(op=="+"||op=="-"||op=="*"||op=="/"||op=="//"||op=="%"||op=="**")
        okay=number(a)&&number(b)&&result==(op=="/"||a==float_type||b==float_type?float_type:int_type);
    need(okay,s,"binary signature");
}
void store(const Symbol& y,const std::string& op,TypeId rhs,bool contextual,const SourceSpan& s){
    need(y.mutable_binding&&(y.kind==SymbolKind::Global||y.kind==SymbolKind::Local),s,"store to readonly/non-slot symbol");
    if(op=="=")need(converts(y.type,rhs,contextual),s,"store type");
    else {
        need(op.size()>1&&op.back()=='=',s,"store operator");
        auto base=op.substr(0,op.size()-1);
        auto result=base=="/"||y.type==float_type||rhs==float_type?float_type:int_type;
        binary(base,y.type,rhs,result,s);
        need(converts(y.type,result,true),s,"compound store type");
    }
}
void call(const Tables& tables,SymbolId symbol_id,TypeId callee,
          const std::vector<TypeId>& args,const std::vector<bool>& ctx,TypeId result,const SourceSpan& s){
    const auto& y=symbol(tables,symbol_id);const auto& sig=type(tables,y.type);
    need((y.kind==SymbolKind::Function||y.kind==SymbolKind::Helper)&&sig.kind==TypeKind::Callable&&callee==y.type,s,"direct callee signature");
    need(result==sig.result&&ctx.size()==args.size(),s,"call result/context flags");
    if(sig.helper==Helper::Print){for(auto t:args)need(number(t)||t==bool_type,s,"print scalar argument");}
    else if(sig.helper!=Helper::None)need(args.size()==1&&number(args.front()),s,"numeric helper signature");
    else {
        need(args.size()==sig.parameters.size(),s,"call arity");
        for(std::size_t i=0;i<args.size();++i)need(converts(sig.parameters[i],args[i],ctx[i]),s,"call argument type");
    }
}
void tables(const Tables& t,const std::vector<FunctionInfo>& fs){
    need(t.sources.size()==1&&t.modules.size()==1&&fs.size()>=1&&fs.size()<=4096,{},"single-module/function limits");
    need(t.sources[0].id==SourceId{1}&&!t.sources[0].filename.empty()&&t.sources[0].bytes<=16*1024*1024,{},"source table");
    need(t.modules[0].id==ModuleId{1}&&t.modules[0].source==SourceId{1},{},"module table");
    need(t.types.size()==fs.size()+6&&t.symbols.size()<=object_limit,{},"type/symbol table limits");
    const char* base[]={"void","int","float","bool","$core$print","$core$int","$core$float"};
    for(std::size_t j=0;j<t.types.size();++j){
        const auto& x=t.types[j];need(x.id.value==j+1,{},"dense type ID");
        need(static_cast<unsigned>(x.kind)<=static_cast<unsigned>(TypeKind::Callable),{},"type kind");
        if(j<4)need(x.kind==static_cast<TypeKind>(j)&&x.name==base[j]&&!x.result&&x.parameters.empty()&&x.helper==Helper::None&&!x.variadic,{},"scalar type table");
        else {
            need(x.kind==TypeKind::Callable&&scalar(x.result),{},"callable result");
            for(auto p:x.parameters)need(scalar(p)&&p!=void_type,{},"callable parameter");
            if(j<7)need(x.name==base[j]&&x.helper==static_cast<Helper>(j-3)&&x.parameters.empty()&&x.result==(j==4?void_type:j==5?int_type:float_type)&&x.variadic==(j==4),{},"helper type table");
            else need(x.helper==Helper::None&&!x.variadic,{},"ordinary callable type");
        }
    }
    for(std::size_t j=0;j<fs.size();++j){
        const auto& x=fs[j];need(x.id.value==j+1&&!x.name.empty(),x.span,"dense function ID/name");span(t,x.span);effects(x.effects,x.span);
        need(scalar(x.result),x.span,"function result");
        if(j==0)need(x.initializer&&!x.symbol&&x.parameters.empty()&&x.result==void_type&&x.name=="<initialize>",x.span,"initializer signature");
        else {
            const auto& y=symbol(t,x.symbol);const auto& sig=type(t,y.type);
            need(!x.initializer&&y.kind==SymbolKind::Function&&y.function==x.id&&y.name==x.name&&sig.name==x.name&&sig.result==x.result&&sig.parameters.size()==x.parameters.size(),x.span,"function symbol/signature");
            std::set<SymbolId> ps;
            for(std::size_t k=0;k<x.parameters.size();++k){
                const auto& p=symbol(t,x.parameters[k]);
                need(ps.insert(p.id).second&&p.kind==SymbolKind::Parameter&&p.owner==x.id&&p.type==sig.parameters[k],p.span,"parameter symbol/signature");
            }
        }
    }
    std::set<std::string> globals;
    for(std::size_t j=0;j<t.symbols.size();++j){
        const auto& y=t.symbols[j];need(y.id.value==j+1&&!y.name.empty(),y.span,"dense symbol ID/name");
        span(t,y.span);type(t,y.type);
        need(static_cast<unsigned>(y.kind)<=static_cast<unsigned>(SymbolKind::Helper)&&y.owner&&y.owner.value<=fs.size(),y.span,"symbol kind/owner");
        if(y.kind==SymbolKind::Function)need(y.function&&y.function.value<=fs.size()&&fs[y.function.value-1].symbol==y.id&&!y.mutable_binding,y.span,"function identity");
        else need(!y.function,y.span,"nonfunction identity");
        if(y.kind==SymbolKind::Helper)need(y.type.value>=5&&y.type.value<=7&&y.name==type(t,y.type).name&&!y.mutable_binding&&y.owner.value==1,y.span,"helper identity");
        if(y.kind==SymbolKind::Parameter)need(!y.mutable_binding&&std::find(fs[y.owner.value-1].parameters.begin(),fs[y.owner.value-1].parameters.end(),y.id)!=fs[y.owner.value-1].parameters.end(),y.span,"parameter ownership");
        if(y.kind==SymbolKind::Global)need(y.owner.value==1,y.span,"global ownership");
        if(y.kind==SymbolKind::Global||y.kind==SymbolKind::Local||y.kind==SymbolKind::Parameter)need(scalar(y.type)&&y.type!=void_type,y.span,"slot type");
        if(y.kind==SymbolKind::Global||y.kind==SymbolKind::Helper||y.kind==SymbolKind::Function)need(globals.insert(y.name).second,y.span,"duplicate global name");
    }
}
std::uint32_t required(M op){
    switch(op){case M::Load:case M::Locate:return Read;
    case M::Store:return Read|Write;
    case M::Bind:return Write;
    case M::EnterScope:case M::RangeInit:case M::Call:return Allocate;
    default:return 0;}
}
struct HirCheck {
    const HirModule& m;
    std::vector<unsigned> owners;
    std::vector<std::set<SymbolId>> scopes;
    const FunctionInfo* fn{};
    const HirNode& get(HirNodeId id)const{return m.nodes[id.value-1];}
    void accessible(SymbolId id,const SourceSpan& s){
        const auto& y=symbol(m.tables,id);
        if(y.kind==SymbolKind::Local||y.kind==SymbolKind::Parameter){
            need(y.owner==fn->id,s,"local symbol owner");
            need(std::any_of(scopes.begin(),scopes.end(),[&](const auto& x){return x.contains(id);}),s,"local symbol outside lexical scope");
        }
    }
    void visit(HirNodeId id,std::size_t loops,std::size_t depth,bool expression){
        const auto& n=get(id);need(depth<=384,n.span,"HIR nesting limit");
        need(!owners[id.value-1],n.span,"HIR node reused/shared");owners[id.value-1]=fn->id.value;
        auto arity=[&](std::size_t count){need(n.children.size()==count,n.span,"HIR node arity");};
        auto child=[&](std::size_t j){visit(n.children[j],loops,depth+1,true);};
        auto statement=[&](std::size_t j){visit(n.children[j],loops,depth+1,false);};
        auto ct=[&](std::size_t j){return get(n.children[j]).type;};
        auto cx=[&](std::size_t j){return get(n.children[j]).contextual;};
        bool expr=n.kind==H::Constant||n.kind==H::Load||n.kind==H::Unary||n.kind==H::Binary||n.kind==H::Store||n.kind==H::Call;
        need(expr==expression,n.span,"HIR expression/statement category");
        need(!n.contextual||(n.kind==H::Constant&&number(n.type)),n.span,"nonliteral contextual conversion");
        std::uint32_t req=0;
        switch(n.kind){
        case H::Constant:
            arity(0);need(scalar(n.type),n.span,"constant type");
            if(n.failure_code.empty())constant(n.constant,n.type,n.span);
            else {need(n.failure_code=="E4002"&&!n.failure_message.empty(),n.span,"literal failure");req|=MayFail;}break;
        case H::Load:
            arity(0);accessible(n.symbol,n.span);need(n.type==symbol(m.tables,n.symbol).type,n.span,"load type");req|=Read|MayFail;break;
        case H::Unary:arity(1);child(0);unary(n.text,ct(0),n.type,n.span);req|=MayFail;break;
        case H::Binary:arity(2);child(0);child(1);binary(n.text,ct(0),ct(1),n.type,n.span);req|=MayFail;break;
        case H::Store:
            arity(1);span(m.tables,n.control_span);accessible(n.symbol,n.span);child(0);
            store(symbol(m.tables,n.symbol),n.text,ct(0),n.flag,n.span);
            need(n.type==symbol(m.tables,n.symbol).type,n.span,"store result type");req|=Read|Write|MayFail;break;
        case H::Call:{
            need(!n.children.empty(),n.span,"call callee");std::vector<TypeId> args;std::vector<bool> ctx;
            for(std::size_t j=0;j<n.children.size();++j){child(j);if(j){args.push_back(ct(j));ctx.push_back(cx(j));}}
            const auto& callee=get(n.children[0]);need(callee.kind==H::Load&&callee.symbol==n.symbol,n.span,"direct HIR call");
            call(m.tables,n.symbol,ct(0),args,ctx,n.type,n.span);const auto& y=symbol(m.tables,n.symbol);
            req|=MayFail|Allocate;if(y.function)req|=m.functions[y.function.value-1].info.effects;
            if(type(m.tables,y.type).helper==Helper::Print)req|=Io;break;}
        case H::Block:
            need(n.type==void_type,n.span,"block result");if(n.flag){scopes.emplace_back();req|=Allocate|MayFail;}
            for(std::size_t j=0;j<n.children.size();++j)statement(j);
            if(n.flag)scopes.pop_back();break;
        case H::Bind:{
            arity(1);child(0);const auto& y=symbol(m.tables,n.symbol);
            need((y.kind==SymbolKind::Global||y.kind==SymbolKind::Local)&&y.owner==fn->id&&y.type==n.type&&y.mutable_binding==n.flag&&converts(n.type,ct(0),cx(0)),n.span,"binding signature");
            need(scopes.back().insert(y.id).second,n.span,"duplicate binding identity");req|=Write|MayFail;break;}
        case H::Expression:arity(1);child(0);need(n.type==void_type,n.span,"expression statement type");break;
        case H::If:
            need(n.children.size()==2||n.children.size()==3,n.span,"if arity");child(0);need(ct(0)==bool_type&&n.type==void_type,n.span,"if condition");
            statement(1);if(n.children.size()==3)statement(2);break;
        case H::While:
            arity(2);child(0);need(ct(0)==bool_type&&n.type==void_type,n.span,"while condition");visit(n.children[1],loops+1,depth+1,false);break;
        case H::For:
            arity(4);span(m.tables,n.control_span);need(n.names.size()==1||n.names.size()==2,n.span,"range binding count");
            need(get(n.children[3]).kind==H::Block&&!get(n.children[3]).flag&&n.type==void_type,n.span,"range body block");
            for(std::size_t j=0;j<3;++j){child(j);need(ct(j)==int_type,n.span,"range type");}
            scopes.emplace_back();for(auto id:n.names){const auto& y=symbol(m.tables,id);need(y.kind==SymbolKind::Local&&y.owner==fn->id&&y.type==int_type&&!y.mutable_binding&&scopes.back().insert(id).second,n.span,"range binding");}
            visit(n.children[3],loops+1,depth+1,false);scopes.pop_back();req|=Read|Write|Allocate|MayFail;break;
        case H::Return:
            need(n.children.size()<=1&&!fn->initializer&&n.type==void_type,n.span,"return shape");
            if(n.children.empty())need(fn->result==void_type,n.span,"empty return type");
            else {child(0);need(converts(fn->result,ct(0),cx(0)),n.span,"return type");}break;
        case H::Break:case H::Continue:arity(0);need(loops&&n.type==void_type,n.span,"loop control");break;
        }
        for(auto c:n.children)req|=get(c).effects;
        need((n.effects&req)==req,n.span,"missing HIR effect");
    }
};
struct FlowState {
    std::vector<ValueId> stack;
    std::vector<std::set<SymbolId>> scopes;
    std::vector<std::uint32_t> iterators;
    auto operator<=>(const FlowState&)const=default;
};
void mir_function(const MirModule& m,const MirFunction& f){
    const auto& tb=m.tables;auto loc=f.info.span;
    need(!f.blocks.empty()&&f.blocks.size()<=block_limit&&f.values.size()<=object_limit,loc,"MIR size limits");
    need(f.entry==BlockId{1}&&f.error.value==f.blocks.size()&&f.entry!=f.error,loc,"entry/error identity");
    struct Definition{std::size_t block{},position{};};
    std::vector<Definition> definitions(f.values.size());
    auto value=[&](ValueId id)->const MirValue& {need(id&&id.value<=f.values.size(),loc,"value ID");return f.values[id.value-1];};
    auto define=[&](ValueId id,std::size_t block,std::size_t position){value(id);need(!definitions[id.value-1].block,loc,"multiple SSA definitions");definitions[id.value-1]={block,position};};
    for(std::size_t j=0;j<f.values.size();++j){
        const auto& v=f.values[j];need(v.id.value==j+1,loc,"dense value ID");const auto& t=type(tb,v.type);
        need(static_cast<unsigned>(v.kind)<=static_cast<unsigned>(ValueKind::Location),loc,"value kind");
        need(v.kind==ValueKind::Callable?t.kind==TypeKind::Callable:scalar(v.type),loc,"value representation/type");
        need(v.kind!=ValueKind::Location||v.type!=void_type,loc,"void location");
    }
    auto edge=[&](const Edge& e,const SourceSpan& s){
        need(e.target&&e.target.value<=f.blocks.size()&&e.target!=f.error,s,"normal edge target");
        const auto& ps=f.blocks[e.target.value-1].parameters;need(ps.size()==e.arguments.size(),s,"block parameter arity");
        for(std::size_t k=0;k<ps.size();++k){const auto& a=value(e.arguments[k]);const auto& p=value(ps[k]);need(a.type==p.type&&a.kind==p.kind,s,"block parameter type");}
    };
    std::size_t instruction_count=0;
    for(std::size_t j=0;j<f.blocks.size();++j){
        const auto& b=f.blocks[j];need(b.id.value==j+1,loc,"dense block ID");
        for(auto p:b.parameters){define(p,j+1,0);need(value(p).kind==ValueKind::Scalar,b.terminator.span,"scalar block parameter");}
        std::size_t position=0;
        for(const auto& i:b.instructions){
            ++position;need(++instruction_count<=object_limit,i.span,"instruction limit");span(tb,i.span);type(tb,i.type);effects(i.effects,i.span);
            need(static_cast<unsigned>(i.op)<=static_cast<unsigned>(M::Raise),i.span,"MIR operation");
            need(i.failure==f.error&&i.reference_steps==1&&i.cancellation_checkpoint,i.span,"instruction failure/budget/checkpoint");
            for(auto v:i.operands)value(v);if(i.result){define(i.result,j+1,position);need(value(i.result).type==i.type,i.span,"result type");need(value(i.result).kind==(i.op==M::Locate?ValueKind::Location:type(tb,i.type).kind==TypeKind::Callable?ValueKind::Callable:ValueKind::Scalar),i.span,"result representation");}
            auto shape=[&](std::size_t n,bool result){need(i.operands.size()==n&&static_cast<bool>(i.result)==result,i.span,"instruction shape");};
            auto vt=[&](std::size_t k){return value(i.operands[k]).type;};
            auto normal=[&]{for(auto v:i.operands)need(value(v).kind!=ValueKind::Location,i.span,"location used as ordinary value");};
            std::uint32_t req=required(i.op);
            switch(i.op){
            case M::Constant:shape(0,true);constant(i.constant,i.type,i.span);break;
            case M::Raise:shape(0,true);need(scalar(i.type)&&i.failure_code=="E4002"&&!i.text.empty(),i.span,"literal raise");req|=MayFail;break;
            case M::Load:case M::Locate:{
                shape(0,true);const auto& y=symbol(tb,i.symbol);need(y.type==i.type,i.span,"slot result type");need((y.kind!=SymbolKind::Local&&y.kind!=SymbolKind::Parameter)||y.owner==f.info.id,i.span,"slot function owner");
                if(i.op==M::Locate)need(value(i.result).kind==ValueKind::Location&&y.mutable_binding&&(y.kind==SymbolKind::Local||y.kind==SymbolKind::Global),i.span,"locate mutable slot");
                else need(value(i.result).kind==(type(tb,y.type).kind==TypeKind::Callable?ValueKind::Callable:ValueKind::Scalar),i.span,"load value kind");
                break;}
            case M::Bind:{
                shape(1,false);normal();const auto& y=symbol(tb,i.symbol);
                need((y.kind==SymbolKind::Global||y.kind==SymbolKind::Local)&&y.owner==f.info.id&&y.type==i.type&&y.mutable_binding==i.flag&&i.contextual.size()==1&&converts(y.type,vt(0),i.contextual[0]),i.span,"bind signature");break;}
            case M::Drop:shape(1,false);normal();need(i.type==void_type,i.span,"drop type");break;
            case M::Unary:shape(1,true);normal();unary(i.text,vt(0),i.type,i.span);break;
            case M::Binary:shape(2,true);normal();need(i.text!="&&"&&i.text!="||",i.span,"short circuit must use CFG");binary(i.text,vt(0),vt(1),i.type,i.span);break;
            case M::Store:{
                shape(2,true);const auto& y=symbol(tb,i.symbol);
                need(y.kind!=SymbolKind::Local||y.owner==f.info.id,i.span,"store function owner");
                need(value(i.operands[0]).kind==ValueKind::Location&&vt(0)==y.type&&value(i.operands[1]).kind==ValueKind::Scalar&&i.type==y.type,i.span,"store location/result");
                store(y,i.text,vt(1),i.flag,i.span);break;}
            case M::Call:{
                need(i.result&&!i.operands.empty(),i.span,"call shape");need(i.text==type(tb,i.type).name||(i.type==void_type&&i.text.empty()),i.span,"legacy call annotation");normal();std::vector<TypeId> args;
                for(std::size_t k=1;k<i.operands.size();++k)args.push_back(vt(k));
                call(tb,i.symbol,vt(0),args,i.contextual,i.type,i.span);
                const auto& y=symbol(tb,i.symbol);if(y.function)req|=m.functions[y.function.value-1].info.effects;
                if(type(tb,y.type).helper==Helper::Print)req|=Io;break;}
            case M::EnterScope:case M::LeaveScope:case M::Unwind:case M::IterEnd:shape(0,false);need(i.type==void_type,i.span,"scope/iterator type");need(i.op!=M::Unwind||i.count<=384,i.span,"unwind nesting limit");break;
            case M::RangeInit:shape(3,false);normal();need(i.type==void_type&&vt(0)==int_type&&vt(1)==int_type&&vt(2)==int_type,i.span,"range signature");break;
            }
            need((i.effects&req)==req,i.span,"missing MIR effect");
            need((i.effects&~MayFail&~f.info.effects)==0,i.span,"function effect underclaim");
        }
        const auto& t=b.terminator;span(tb,t.span);need(static_cast<unsigned>(t.kind)<=static_cast<unsigned>(T::PropagateError),t.span,"terminator kind");
        for(auto v:t.operands)value(v);
        if(b.id==f.error){need(b.instructions.empty()&&b.parameters.empty()&&t.kind==T::PropagateError&&!t.failure&&!t.reference_steps&&!t.cancellation_checkpoint&&t.operands.empty()&&!t.taken.target&&!t.next.target,t.span,"propagation block");continue;}
        need(t.kind!=T::PropagateError,t.span,"propagate outside error block");
        if(t.kind==T::Fallthrough)need(!t.failure&&!t.reference_steps&&!t.cancellation_checkpoint,t.span,"structural fallthrough charge");
        else need(t.failure==f.error&&t.reference_steps==1&&t.cancellation_checkpoint,t.span,"terminator failure/budget/checkpoint");
        if(t.kind==T::Jump){edge(t.taken,t.span);need(t.operands.empty()&&!t.next.target,t.span,"jump shape");}
        else if(t.kind==T::Fallthrough){edge(t.next,t.span);need(t.operands.empty()&&!t.taken.target&&t.next.target.value==j+2,t.span,"physical fallthrough");}
        else if(t.kind==T::Branch||t.kind==T::IterNext){
            edge(t.taken,t.span);edge(t.next,t.span);need(t.next.target.value==j+2,t.span,"physical branch fallthrough");
            if(t.kind==T::Branch)need(t.operands.size()==1&&value(t.operands[0]).type==bool_type&&value(t.operands[0]).kind==ValueKind::Scalar&&t.names.empty(),t.span,"branch condition");
            else {need(t.operands.empty()&&(t.names.size()==1||t.names.size()==2),t.span,"iterator terminator");
                std::set<SymbolId> names;for(auto id:t.names){const auto& y=symbol(tb,id);need(y.kind==SymbolKind::Local&&y.type==int_type&&y.owner==f.info.id&&!y.mutable_binding&&names.insert(id).second,t.span,"iteration symbol");}}
        } else if(t.kind==T::Return)need(t.operands.size()==1&&value(t.operands[0]).kind==ValueKind::Scalar&&!t.taken.target&&!t.next.target,t.span,"return shape");
    }
    for(const auto& d:definitions)need(d.block,loc,"undefined SSA value");
    for(std::size_t j=0;j<f.blocks.size();++j){
        std::size_t p=0;for(const auto& i:f.blocks[j].instructions){++p;for(auto v:i.operands){const auto& d=definitions[v.value-1];need(d.block!=j+1||d.position<p,i.span,"SSA local use before definition");}}
        for(auto v:f.blocks[j].terminator.operands){const auto& d=definitions[v.value-1];need(d.block!=j+1||d.position<=p,f.blocks[j].terminator.span,"SSA terminal use before definition");}
    }
    need(f.blocks.front().parameters.empty(),loc,"entry parameters");
    // Propagate an exact abstract VM stack and lexical/iterator state. Each
    // operand must be the value actually on the stack, not just a same-typed ID.
    std::vector<std::optional<FlowState>> entries(f.blocks.size());
    FlowState start;start.scopes.emplace_back();
    start.scopes[0].insert(f.info.parameters.begin(),f.info.parameters.end());
    entries[0]=start;std::deque<std::size_t> queue{0};
    std::vector<bool> reachable(f.blocks.size());
    std::vector<std::vector<std::size_t>> predecessors(f.blocks.size());
    auto propagate=[&](std::size_t from,const Edge& e,FlowState state){
        const auto& dest=f.blocks[e.target.value-1];
        need(e.arguments.size()<=state.stack.size(),dest.terminator.span,"edge stack arguments");
        auto base=state.stack.size()-e.arguments.size();
        for(std::size_t k=0;k<e.arguments.size();++k){need(state.stack[base+k]==e.arguments[k],dest.terminator.span,"edge argument stack order");state.stack[base+k]=dest.parameters[k];}
        auto to=e.target.value-1;predecessors[to].push_back(from);
        if(entries[to])need(*entries[to]==state,dest.terminator.span,"stack/scope/iterator merge mismatch");
        else {entries[to]=std::move(state);queue.push_back(to);}
    };
    std::size_t work=0;
    while(!queue.empty()){
        auto j=queue.front();queue.pop_front();reachable[j]=true;auto state=*entries[j];const auto& b=f.blocks[j];bool raised=false;
        auto consume=[&](const std::vector<ValueId>& args,const SourceSpan& s){
            need(args.size()<=state.stack.size(),s,"stack underflow");auto base=state.stack.size()-args.size();
            for(std::size_t k=0;k<args.size();++k)need(state.stack[base+k]==args[k],s,"SSA operand/stack mismatch");state.stack.resize(base);
        };
        auto access=[&](SymbolId id,const SourceSpan& s){
            const auto& y=symbol(tb,id);
            if(y.kind==SymbolKind::Local||y.kind==SymbolKind::Parameter)need(y.owner==f.info.id&&std::any_of(state.scopes.begin(),state.scopes.end(),[&](const auto& x){return x.contains(id);}),s,"local slot lifetime");
            // Names sharing one VM scope must never resolve to another symbol.
            for(auto sc=state.scopes.rbegin();sc!=state.scopes.rend();++sc)
                for(auto active:*sc)if(symbol(tb,active).name==y.name){need(active==id,s,"shadowed symbol identity");return;}
        };
        for(const auto& i:b.instructions){
            need(++work<=4000000,i.span,"flow validation work limit");consume(i.operands,i.span);
            if(i.op==M::Load||i.op==M::Locate||i.op==M::Store)access(i.symbol,i.span);
            if(i.op==M::Store){
                const auto& def=definitions[i.operands[0].value-1];const auto& locate=f.blocks[def.block-1].instructions[def.position-1];
                need(locate.op==M::Locate&&locate.symbol==i.symbol,i.span,"store location identity");
            }
            if(i.op==M::Call){
                const auto& def=definitions[i.operands[0].value-1];need(def.position,i.span,"callee definition");
                const auto& load=f.blocks[def.block-1].instructions[def.position-1];need(load.op==M::Load&&load.symbol==i.symbol,i.span,"callee identity");
            }
            switch(i.op){
            case M::Bind:
                for(auto active:state.scopes.back())need(symbol(tb,active).name!=symbol(tb,i.symbol).name,i.span,"duplicate VM scope name");
                need(state.scopes.back().insert(i.symbol).second,i.span,"duplicate live binding");break;
            case M::EnterScope:state.scopes.emplace_back();break;
            case M::LeaveScope:need(state.scopes.size()>1,i.span,"scope underflow");state.scopes.pop_back();break;
            case M::Unwind:need(i.count<state.scopes.size(),i.span,"invalid scope unwind");for(std::size_t k=0;k<i.count;++k)state.scopes.pop_back();break;
            case M::RangeInit:state.iterators.push_back(i.operands.front().value);break;
            case M::IterEnd:need(!state.iterators.empty(),i.span,"iterator underflow");state.iterators.pop_back();break;
            case M::Raise:raised=true;break;
            default:break;
            }
            if(raised)break;if(i.result)state.stack.push_back(i.result);
        }
        if(raised)continue;
        const auto& t=b.terminator;consume(t.operands,t.span);
        if(t.kind==T::Return){
            need(state.stack.empty(),t.span,"return stack imbalance");
            need(converts(f.info.result,value(t.operands[0]).type,t.flag),t.span,"reachable return type");continue;
        }
        if(t.kind==T::IterNext){need(!state.iterators.empty(),t.span,"iteration without initialized range");
            propagate(j,t.taken,state);state.scopes.emplace_back(t.names.begin(),t.names.end());propagate(j,t.next,state);
        }else if(t.kind==T::Jump)propagate(j,t.taken,state);
        else if(t.kind==T::Fallthrough)propagate(j,t.next,state);
        else if(t.kind==T::Branch){propagate(j,t.taken,state);propagate(j,t.next,state);}
    }
    // Explicit dominance proof, including edge arguments. Unreachable blocks are
    // structurally checked above but cannot make a runtime value use valid.
    const auto n=f.blocks.size(),words=(n+63)/64;
    std::vector<std::vector<std::uint64_t>> dom(n,std::vector<std::uint64_t>(words));
    for(std::size_t j=0;j<n;++j)if(reachable[j])for(std::size_t k=0;k<n;++k)if(reachable[k])dom[j][k/64]|=std::uint64_t{1}<<(k%64);
    dom[0].assign(words,0);dom[0][0]=1;bool changed=true;work=0;
    while(changed){changed=false;for(std::size_t j=1;j<n;++j)if(reachable[j]){
        auto bits=dom[j];bool first=true;
        for(auto p:predecessors[j])if(reachable[p]){
            need((work+=words)<=16000000,loc,"dominance work limit");
            if(first){bits=dom[p];first=false;}else for(std::size_t k=0;k<words;++k)bits[k]&=dom[p][k];
        }
        bits[j/64]|=std::uint64_t{1}<<(j%64);if(bits!=dom[j]){dom[j]=std::move(bits);changed=true;}
    }}
    auto use=[&](ValueId id,std::size_t block,std::size_t position,const SourceSpan& s){
        const auto& d=definitions[id.value-1];
        need(d.block==block+1?d.position<position:(dom[block][(d.block-1)/64]&(std::uint64_t{1}<<((d.block-1)%64)))!=0,s,"SSA use not dominated by definition");
    };
    for(std::size_t j=0;j<n;++j)if(reachable[j]){
        const auto& b=f.blocks[j];std::size_t p=0;bool raised=false;
        for(const auto& i:b.instructions){++p;for(auto v:i.operands)use(v,j,p,i.span);if(i.op==M::Raise){raised=true;break;}}
        if(raised)continue;++p;for(auto v:b.terminator.operands)use(v,j,p,b.terminator.span);
        for(const auto* e:{&b.terminator.taken,&b.terminator.next})for(auto v:e->arguments)use(v,j,p,b.terminator.span);
    }
}
}
void verify_hir(const HirModule& m){
    std::vector<FunctionInfo> fs;for(const auto& f:m.functions)fs.push_back(f.info);tables(m.tables,fs);
    need(m.nodes.size()<=object_limit,{},"HIR size limit");
    for(std::size_t j=0;j<m.nodes.size();++j){
        const auto& n=m.nodes[j];need(n.id.value==j+1,n.span,"dense HIR ID");span(m.tables,n.span);type(m.tables,n.type);effects(n.effects,n.span);
        need(n.source==SourceId{1}&&static_cast<unsigned>(n.kind)<=static_cast<unsigned>(H::Continue),n.span,"HIR source/kind");
        for(auto c:n.children)need(c&&c.value<n.id.value,n.span,"HIR child ID/order/cycle");
        if(n.symbol)symbol(m.tables,n.symbol);for(auto y:n.names)symbol(m.tables,y);
    }
    HirCheck check{m,std::vector<unsigned>(m.nodes.size()),{},{}};
    for(const auto& f:m.functions){
        need(f.body&&f.body.value<=m.nodes.size()&&m.nodes[f.body.value-1].kind==H::Block&&!m.nodes[f.body.value-1].flag,f.info.span,"function body");
        check.fn=&f.info;check.scopes={std::set<SymbolId>(f.info.parameters.begin(),f.info.parameters.end())};
        check.visit(f.body,0,0,false);
        need((f.info.effects&m.nodes[f.body.value-1].effects)==m.nodes[f.body.value-1].effects,f.info.span,"function HIR effects");
    }
    for(auto owner:check.owners)need(owner,{},"orphan HIR node");
}
void verify_mir(const MirModule& m){
    std::vector<FunctionInfo> fs;for(const auto& f:m.functions)fs.push_back(f.info);tables(m.tables,fs);
    for(const auto& f:m.functions)mir_function(m,f);
}
}
