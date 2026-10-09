#include "hua/ir.hpp"
#include "hua/archive.hpp"
namespace hua::ir {
namespace {
std::string result_type(const Node& n){
    for(const auto& c:n.children)if(c->kind==NodeKind::ReturnTypes){
        if(c->children.size()!=1)return "<multiple>";
        return type_name(*c->children[0]);
    }
    return "void";
}
const Node* declaration(const MirModule& m,const MirFunction& f,const SemanticModel& model){
    auto found=model.functions.find(f.info.name);
    if(found==model.functions.end()||!found->second)invalid(f.info.span,"missing legacy function metadata");
    const auto* n=found->second;
    if(n->kind!=NodeKind::Function||declared_name(*n)!=f.info.name||
       n->span.file_id!=f.info.span.file_id||n->span.start_offset!=f.info.span.start_offset||
       n->span.end_offset!=f.info.span.end_offset||result_type(*n)!=type(m.tables,f.info.result).name||
       n->text.find(" unsafe")!=std::string::npos||n->text.find(" external")!=std::string::npos||
       n->text.find(" abstract")!=std::string::npos||
       (model.nested.contains(n)&&model.nested.at(n)))
        invalid(f.info.span,"legacy declaration identity/signature mismatch");
    std::size_t j=0;
    for(const auto& c:n->children)if(c->kind==NodeKind::Parameter){
        if(j>=f.info.parameters.size())invalid(f.info.span,"legacy parameter arity");
        const auto& y=symbol(m.tables,f.info.parameters[j++]);
        if(c->children.size()!=1||declared_name(*c)!=y.name||
           type_name(*c->children[0])!=type(m.tables,y.type).name||
           c->text.find(" mut")!=std::string::npos)
            invalid(c->span,"legacy parameter signature mismatch");
    }
    if(j!=f.info.parameters.size())invalid(f.info.span,"legacy parameter arity");
    return n;
}
Code emit(const Tables& tables,const MirFunction& f){
    Code out;out.name=f.info.name;
    std::vector<std::size_t> pcs(f.blocks.size());
    std::size_t pc=0;
    for(const auto& b:f.blocks){
        pcs[b.id.value-1]=pc;
        pc+=b.instructions.size();
        if(b.terminator.kind!=TermKind::Fallthrough&&b.terminator.kind!=TermKind::PropagateError)++pc;
    }
    out.instructions.reserve(pc);
    // The verifier has proved operand stack order and physical fallthrough.
    // Block arguments are SSA renamings of the existing stack value: no spills,
    // phi instructions, extra jumps, or extra legacy budget charges.
    for(const auto& b:f.blocks){
        for(const auto& x:b.instructions){
            Instruction i{};i.span=x.span;
            switch(x.op){
            case MirOp::Constant:i.op=Op::Constant;i.constant=box_constant(x.constant);break;
            case MirOp::Load:i.op=Op::Load;i.text=symbol(tables,x.symbol).name;break;
            case MirOp::Locate:i.op=Op::LocateName;i.text=symbol(tables,x.symbol).name;break;
            case MirOp::Bind:i.op=Op::Bind;i.text=symbol(tables,x.symbol).name;i.type=type(tables,x.type).name;i.flag=x.flag;i.contextual=x.contextual;break;
            case MirOp::Drop:i.op=Op::Pop;break;
            case MirOp::Unary:i.op=Op::Unary;i.text=x.text;break;
            case MirOp::Binary:i.op=Op::Binary;i.text=x.text;i.flag=x.flag;break;
            case MirOp::Store:i.op=Op::Store;i.text=x.text;i.flag=x.flag;break;
            case MirOp::Call:i.op=Op::Call;i.argument=x.operands.size()-1;i.type=x.text;i.contextual=x.contextual;break;
            case MirOp::EnterScope:i.op=Op::EnterScope;break;
            case MirOp::LeaveScope:i.op=Op::LeaveScope;break;
            case MirOp::Unwind:i.op=Op::Unwind;i.argument=x.count;break;
            case MirOp::RangeInit:i.op=Op::RangeInit;i.flag=x.flag;break;
            case MirOp::IterEnd:i.op=Op::IterEnd;break;
            case MirOp::Raise:i.op=Op::Fail;i.type=x.failure_code;i.text=x.text;break;
            }
            out.instructions.push_back(std::move(i));
        }
        const auto& t=b.terminator;Instruction i{};i.span=t.span;
        switch(t.kind){
        case TermKind::Fallthrough:case TermKind::PropagateError:continue;
        case TermKind::Jump:i.op=Op::Jump;i.target=pcs[t.taken.target.value-1];break;
        case TermKind::Branch:i.op=t.flag?Op::JumpTrue:Op::JumpFalse;i.target=pcs[t.taken.target.value-1];break;
        case TermKind::IterNext:i.op=Op::IterNext;i.target=pcs[t.taken.target.value-1];
            for(auto id:t.names)i.names.push_back(symbol(tables,id).name);break;
        case TermKind::Return:i.op=Op::Return;i.flag=t.flag;break;
        }
        out.instructions.push_back(std::move(i));
    }
    return out;
}
}
Bytecode lower_bytecode(const MirModule& m,const SemanticModel& model){
    verify_mir(m);
    if(model.functions.size()+1!=m.functions.size()||!model.structures.empty())
        invalid(m.functions[0].info.span,"legacy model contains functions outside E1 module");
    Bytecode code;
    for(const auto& f:m.functions){
        if(f.info.initializer)code.initializer=emit(m.tables,f);
        else code.functions.emplace(declaration(m,f,model),emit(m.tables,f));
    }
    verify_bytecode(code,model);return code;
}
}
