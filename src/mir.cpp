#include "hua/ir.hpp"
#include <algorithm>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
namespace hua::ir {
namespace {
using H=HirKind; using M=MirOp; using T=TermKind;
struct Item {
    MirInstruction instruction;
    Terminator term;
    bool terminal{}, merge{};
    std::size_t destination{};
    ValueId parameter;
    std::vector<std::pair<std::size_t,ValueId>> incoming;
};
class Lower {
    const HirModule& hir;
    MirFunction out;
    std::vector<Item> items;
    std::size_t depth{};
    struct Loop {std::size_t depth,next;std::vector<std::size_t> exits;};
    std::vector<Loop> loops;
    const HirNode& node(HirNodeId id)const{return hir.nodes.at(id.value-1);}
    ValueId value(TypeId t,ValueKind k=ValueKind::Scalar) {
        ValueId id{static_cast<std::uint32_t>(out.values.size()+1)};
        out.values.push_back({id,t,k});return id;
    }
    MirInstruction& instruction(M op,const SourceSpan& s,TypeId t=void_type,
                                std::vector<ValueId> args={},bool result=false) {
        Item item{};auto& i=item.instruction;i.op=op;i.span=s;i.type=t;i.operands=std::move(args);
        i.effects=MayFail;if(result)i.result=value(t,type(hir.tables,t).kind==TypeKind::Callable?ValueKind::Callable:ValueKind::Scalar);
        items.push_back(std::move(item));return items.back().instruction;
    }
    std::size_t terminal(T kind,const SourceSpan& s,std::vector<ValueId> args={}) {
        Item item{};item.terminal=true;item.term.kind=kind;item.term.span=s;item.term.operands=std::move(args);
        item.term.reference_steps=1;item.term.cancellation_checkpoint=true;
        items.push_back(std::move(item));return items.size()-1;
    }
    ValueId constant(ScalarConstant v,TypeId t,const SourceSpan& s) {
        auto& i=instruction(M::Constant,s,t,{},true);i.constant=std::move(v);return i.result;
    }
    ValueId expression(HirNodeId id) {
        const auto& n=node(id);
        switch(n.kind) {
        case H::Constant: {
            auto& i=instruction(n.failure_code.empty()?M::Constant:M::Raise,n.span,n.type,{},true);
            i.constant=n.constant;i.text=n.failure_message;i.failure_code=n.failure_code;return i.result;
        }
        case H::Load: {
            auto& i=instruction(M::Load,n.span,n.type,{},true);i.symbol=n.symbol;i.effects|=Read;return i.result;
        }
        case H::Unary: {
            auto v=expression(n.children[0]);auto& i=instruction(M::Unary,n.span,n.type,{v},true);
            i.text=n.text;return i.result;
        }
        case H::Binary: {
            auto left=expression(n.children[0]);
            if(n.text=="&&"||n.text=="||") {
                auto branch=terminal(T::Branch,n.span,{left});items[branch].term.flag=n.text=="||";
                auto right=expression(n.children[1]);
                auto& first=instruction(M::Unary,n.span,bool_type,{right},true);first.text="!";auto x=first.result;
                auto& second=instruction(M::Unary,n.span,bool_type,{x},true);second.text="!";auto y=second.result;
                auto jump=terminal(T::Jump,n.span);
                items[branch].destination=items.size();auto alternate=constant(ScalarConstant(n.text=="||"),bool_type,n.span);
                auto last=items.size()-1;items[jump].destination=items.size();
                Item phi{};phi.merge=true;phi.parameter=value(bool_type);phi.incoming={{jump,y},{last,alternate}};
                auto result=phi.parameter;items.push_back(std::move(phi));return result;
            }
            auto right=expression(n.children[1]);
            auto& i=instruction(M::Binary,n.span,n.type,{left,right},true);i.text=n.text;
            i.flag=node(n.children[1]).contextual;return i.result;
        }
        case H::Store: {
            auto& place=instruction(M::Locate,n.control_span,n.type,{},true);place.symbol=n.symbol;
            place.effects|=Read;out.values[place.result.value-1].kind=ValueKind::Location;auto location=place.result;
            // Validate the target before evaluating the RHS.
            auto rhs=expression(n.children[0]);auto& i=instruction(M::Store,n.span,n.type,{location,rhs},true);
            i.symbol=n.symbol;i.text=n.text;i.flag=n.flag;i.effects|=Read|Write;return i.result;
        }
        case H::Call: {
            std::vector<ValueId> args;for(auto c:n.children)args.push_back(expression(c));
            auto& i=instruction(M::Call,n.span,n.type,std::move(args),true);
            i.symbol=n.symbol;i.text=n.text;i.effects|=n.effects;
            for(std::size_t j=1;j<n.children.size();++j)i.contextual.push_back(node(n.children[j]).contextual);
            return i.result;
        }
        default:invalid(n.span,"HIR expression kind in MIR lowering");
        }
    }
    void block(const HirNode& n,bool scope) {
        if(scope){instruction(M::EnterScope,n.span).effects|=Allocate;++depth;}
        for(auto c:n.children)statement(c);
        if(scope){instruction(M::LeaveScope,n.span);--depth;}
    }
    void statement(HirNodeId id) {
        const auto& n=node(id);
        switch(n.kind) {
        case H::Block:block(n,n.flag);break;
        case H::Bind: {
            auto v=expression(n.children[0]);auto& i=instruction(M::Bind,n.span,n.type,{v});
            i.symbol=n.symbol;i.flag=n.flag;i.effects|=Write;i.contextual={node(n.children[0]).contextual};break;
        }
        case H::Expression: {auto v=expression(n.children[0]);instruction(M::Drop,n.span,void_type,{v});break;}
        case H::If: {
            auto v=expression(n.children[0]);auto branch=terminal(T::Branch,node(n.children[0]).span,{v});
            statement(n.children[1]);auto jump=terminal(T::Jump,n.span);items[branch].destination=items.size();
            if(n.children.size()==3)statement(n.children[2]);items[jump].destination=items.size();break;
        }
        case H::While: {
            auto begin=items.size();auto v=expression(n.children[0]);auto exit=terminal(T::Branch,node(n.children[0]).span,{v});
            loops.push_back({depth,begin,{}});statement(n.children[1]);auto back=terminal(T::Jump,n.span);items[back].destination=begin;
            auto end=items.size();items[exit].destination=end;
            for(auto p:loops.back().exits)items[p].destination=end;loops.pop_back();break;
        }
        case H::For: {
            std::vector<ValueId> args;for(std::size_t j=0;j<3;++j)args.push_back(expression(n.children[j]));
            auto& init=instruction(M::RangeInit,n.control_span,void_type,std::move(args));init.flag=n.flag;init.effects|=Allocate;
            auto next=terminal(T::IterNext,n.span);items[next].term.names=n.names;
            loops.push_back({depth,next,{}});++depth;block(node(n.children.back()),false);--depth;
            instruction(M::LeaveScope,n.span);auto back=terminal(T::Jump,n.span);items[back].destination=next;
            auto end=items.size();instruction(M::IterEnd,n.span);items[next].destination=end;
            for(auto p:loops.back().exits)items[p].destination=end;loops.pop_back();break;
        }
        case H::Break:case H::Continue: {
            if(loops.empty())invalid(n.span,"loop control without loop");
            instruction(M::Unwind,n.span).count=depth-loops.back().depth;
            auto jump=terminal(T::Jump,n.span);
            if(n.kind==H::Continue)items[jump].destination=loops.back().next;
            else loops.back().exits.push_back(jump);break;
        }
        case H::Return: {
            auto v=n.children.empty()?constant({},void_type,n.span):expression(n.children[0]);
            auto p=terminal(T::Return,n.span,{v});items[p].term.flag=!n.children.empty()&&node(n.children[0]).contextual;break;
        }
        default:invalid(n.span,"HIR statement kind in MIR lowering");
        }
    }
    void split() {
        if(items.empty()||items.size()>250000)invalid(out.info.span,"MIR instruction limit");
        std::set<std::size_t> leaders{0};
        for(std::size_t p=0;p<items.size();++p) {
            const auto& x=items[p];if(x.merge)leaders.insert(p);
            if(x.terminal){if(p+1<items.size())leaders.insert(p+1);
                if(x.term.kind==T::Jump||x.term.kind==T::Branch||x.term.kind==T::IterNext){
                    if(x.destination>=items.size())invalid(x.term.span,"unpatched MIR target");leaders.insert(x.destination);}}
        }
        if(leaders.size()>4096)invalid(out.info.span,"MIR block limit");
        std::vector<std::size_t> starts(leaders.begin(),leaders.end());std::vector<BlockId> position(items.size());
        for(std::size_t j=0;j<starts.size();++j){MirBlock b{};b.id={static_cast<std::uint32_t>(j+1)};
            auto end=j+1<starts.size()?starts[j+1]:items.size();
            for(auto p=starts[j];p<end;++p)position[p]=b.id;out.blocks.push_back(std::move(b));}
        out.entry={1};out.error={static_cast<std::uint32_t>(out.blocks.size()+1)};
        for(std::size_t j=0;j<starts.size();++j) {
            auto& b=out.blocks[j];auto end=j+1<starts.size()?starts[j+1]:items.size();bool ended=false;
            for(auto p=starts[j];p<end;++p) {
                auto& x=items[p];
                if(x.merge){b.parameters.push_back(x.parameter);continue;}
                if(x.terminal){b.terminator=x.term;b.terminator.failure=out.error;
                    if(x.term.kind==T::Jump||x.term.kind==T::Branch||x.term.kind==T::IterNext)b.terminator.taken.target=position[x.destination];
                    if(x.term.kind==T::Branch||x.term.kind==T::IterNext){
                        if(p+1>=items.size())invalid(x.term.span,"missing MIR fallthrough");b.terminator.next.target=position[p+1];}
                    ended=true;
                }else {x.instruction.failure=out.error;b.instructions.push_back(x.instruction);}
            }
            if(!ended){if(end>=items.size())invalid(out.info.span,"unterminated MIR block");
                b.terminator.kind=T::Fallthrough;b.terminator.span=out.info.span;b.terminator.next.target=position[end];}
        }
        for(std::size_t p=0;p<items.size();++p)if(items[p].merge)for(auto [from,v]:items[p].incoming) {
            auto& term=out.blocks[position[from].value-1].terminator;auto dest=position[p];bool found=false;
            for(auto* e:{&term.taken,&term.next})if(e->target==dest){e->arguments.push_back(v);found=true;}
            if(!found)invalid(out.info.span,"block parameter without incoming edge");
        }
        MirBlock error{};error.id=out.error;error.terminator.kind=T::PropagateError;error.terminator.span=out.info.span;
        out.blocks.push_back(std::move(error));
    }
public:
    Lower(const HirModule& h,const HirFunction& f):hir(h){out.info=f.info;}
    MirFunction run(const HirFunction& f) {
        block(node(f.body),false);
        auto nil=constant({},void_type,f.info.span);auto p=terminal(T::Return,f.info.span,{nil});items[p].term.implicit_return=true;
        split();return std::move(out);
    }
};
}
MirModule lower_mir(const HirModule& h) {
    verify_hir(h);MirModule m;m.tables=h.tables;for(const auto& f:h.functions)m.functions.push_back(Lower(h,f).run(f));
    verify_mir(m);return m;
}
std::string dump_mir(const MirModule& m) {
    verify_mir(m);std::ostringstream o;o<<"hua.mir 1\n";
    const char* names[]={"constant","load","locate","bind","drop","unary.checked","binary.checked","store.checked",
        "call.checked","enter_scope","leave_scope","unwind","range_init","iter_end","raise"};
    const char* terms[]={"fallthrough","jump","branch","iter_next","return","propagate_error"};
    for(const auto& f:m.functions){o<<"function f"<<f.info.id.value<<' '<<std::quoted(f.info.name)<<" entry b"<<f.entry.value<<" error b"<<f.error.value<<" effects="<<f.info.effects<<'\n';
        for(const auto& v:f.values)o<<"  value v"<<v.id.value<<" t"<<v.type.value<<" kind="<<static_cast<unsigned>(v.kind)<<'\n';
        for(const auto& b:f.blocks){o<<"b"<<b.id.value;for(auto p:b.parameters)o<<" (v"<<p.value<<')';o<<":\n";
            for(const auto& i:b.instructions){o<<"  ";if(i.result)o<<"v"<<i.result.value<<" = ";o<<names[static_cast<unsigned>(i.op)];
                for(auto v:i.operands)o<<" v"<<v.value;if(i.symbol)o<<" y"<<i.symbol.value;
                if(!i.text.empty())o<<' '<<std::quoted(i.text);
                if(i.op==MirOp::Constant)o<<' '<<std::quoted(show(box_constant(i.constant)));
                o<<" t"<<i.type.value<<" effects="<<i.effects<<" fail b"<<i.failure.value<<" charge="<<i.reference_steps<<" checkpoint="<<i.cancellation_checkpoint<<" @"<<i.span.line<<':'<<i.span.column<<'\n';
            }
            const auto& t=b.terminator;o<<"  "<<terms[static_cast<unsigned>(t.kind)];
            for(auto v:t.operands)o<<" v"<<v.value;
            for(const auto* e:{&t.taken,&t.next})if(e->target){o<<" -> b"<<e->target.value;for(auto v:e->arguments)o<<"(v"<<v.value<<')';}
            o<<" charge="<<t.reference_steps<<" checkpoint="<<t.cancellation_checkpoint<<" fail b"<<t.failure.value<<'\n';
        }}
    return o.str();
}

std::string explain_mir(const MirModule& m){
    verify_mir(m);std::ostringstream out;
    out<<"hua.e1.explain 1\n"
       <<"selected: MIR -> legacy stack bytecode -> VM\n"
       <<"proved: scalar signatures, source/ID bounds, CFG, SSA definitions/dominance, block arguments, stack/slot/iterator lifetimes\n"
       <<"runtime-checked: numeric overflow/finite/division, conversions, global initialization, call depth, execution budget, cancellation\n"
       <<"budget: one charge/checkpoint per emitted legacy instruction; structural edges/block parameters add zero\n"
       <<"artifact: HUAB writer 6; public Native ABI 1 unchanged; no serialized HIR/MIR\n"
       <<"unsupported: whole program rejected before initialization; no retry after effects\n";
    for(const auto& f:m.functions)out<<"function f"<<f.info.id.value<<' '<<std::quoted(f.info.name)<<" effects="<<f.info.effects<<" blocks="<<f.blocks.size()<<" values="<<f.values.size()<<'\n';
    out<<"measured-performance: none; typed runtime/native/GPU not implemented by E1\n";
    return out.str();
}
}
