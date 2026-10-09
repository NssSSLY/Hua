#include "hua/ir.hpp"
#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include "hua/vm.hpp"
#include <functional>
#include <iostream>
#include <sstream>
namespace {
int checks=0,failures=0;
void check(bool ok,const std::string& name){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<name<<'\n';}}
template<class Module,class Verify,class Change>
void reject(const Module& original,Verify verify,Change change,const std::string& label){
    auto copy=original;change(copy);
    try{verify(copy);check(false,label+" accepted");}
    catch(const hua::Diagnostic& d){check(d.code()=="E8002",label+" diagnostic");}
    catch(const std::exception& e){check(false,label+" escaped: "+e.what());}
}
const std::string source=R"(var state=0
var spare=0
const BASE=2**3
fn tick(v int) int {
state+=v
return state
}
fn choose(n int) int {
var total=0
for index,item in 0..n {
if item==2 {continue}
total+=item
if total>10 {break}
}
while total<8 {
total++
}
if n>0 {return total} else {return BASE}
}
fn main() {
let condition=false || (true && false)
print(choose(5),tick(1)+tick(2),condition)
}
)";
hua::NodePtr parse(const hua::Source& s){return hua::Parser(hua::Lexer(s).scan()).parse();}
hua::ir::HirModule independent(){
    hua::Source s("unit.hua",source);auto ast=parse(s);auto model=hua::SemanticAnalyzer{}.analyze(*ast);
    return hua::ir::build_hir(*ast,model,{&s});
}
template<class F> hua::ir::MirInstruction& find(hua::ir::MirModule& m,F predicate){
    for(auto& f:m.functions)for(auto& b:f.blocks)for(auto& i:b.instructions)if(predicate(i))return i;
    throw std::runtime_error("missing test instruction");
}
hua::ir::MirInstruction& find(hua::ir::MirModule& m,hua::ir::MirOp op){return find(m,[&](const auto& i){return i.op==op;});}
std::string normalized(std::string text){
    for(std::size_t pos=0;(pos=text.find("$core$",pos))!=std::string::npos;)text.erase(pos,6);
    return text;
}
bool same(const hua::Code& a,const hua::Code& b){
    if(a.name!=b.name||a.instructions.size()!=b.instructions.size())return false;
    for(std::size_t j=0;j<a.instructions.size();++j){
        const auto& x=a.instructions[j];const auto& y=b.instructions[j];
        if(x.op!=y.op||normalized(x.text)!=normalized(y.text)||x.type!=y.type||x.argument!=y.argument||
           x.target!=y.target||x.flag!=y.flag||x.contextual!=y.contextual||x.names!=y.names||
           hua::show(x.constant)!=hua::show(y.constant)||x.span.file_id!=y.span.file_id||
           x.span.start_offset!=y.span.start_offset||x.span.end_offset!=y.span.end_offset||
           x.span.line!=y.span.line||x.span.column!=y.span.column)return false;
    }
    return true;
}
}
static_assert(!std::is_constructible_v<hua::ir::ScalarConstant,hua::Callable>);
static_assert(!std::is_constructible_v<hua::ir::ScalarConstant,std::string>);
int main(){
    using namespace hua::ir;
    try{
        auto hir=independent();auto again=independent();
        check(dump_hir(hir)==dump_hir(again),"stable HIR IDs across distinct AST allocations");
        auto mir=lower_mir(hir);check(dump_mir(mir)==dump_mir(lower_mir(again)),"stable MIR IDs");
        verify_hir(hir);verify_mir(mir);
        check(explain_mir(mir).find("measured-performance: none")!=std::string::npos,"Explain separates static facts from measurement");check(true,"IR validates after original AST/source/model destruction");
        hua::Source s("unit.hua",source);auto ast=parse(s);auto model=hua::SemanticAnalyzer{}.analyze(*ast);
        auto direct=lower_bytecode(mir,model);auto old=hua::BytecodeCompiler{}.compile(*ast,model);
        check(same(direct.initializer,old.initializer),"initializer direct bytecode exact fields");
        for(const auto& [node,code]:old.functions)check(same(direct.functions.at(node),code),"function bytecode exact fields "+code.name);
        std::ostringstream out;hua::VirtualMachine(model,out).run(direct);check(out.str()=="8 4 false\n","direct bytecode execution");
        auto hv=[](const HirModule& x){verify_hir(x);};auto mv=[](const MirModule& x){verify_mir(x);};
        auto hkind=[](HirModule& x,HirKind kind)->HirNode&{for(auto& n:x.nodes)if(n.kind==kind)return n;throw std::runtime_error("missing HIR");};
        reject(hir,hv,[](auto& x){x.nodes[0].id={0};},"HIR zero ID");
        reject(hir,hv,[](auto& x){x.nodes.back().children={x.nodes.back().id};},"HIR cycle");
        reject(hir,hv,[](auto& x){x.nodes.back().children={{999999}};},"HIR bad child");
        reject(hir,hv,[](auto& x){x.nodes[0].type={999};},"HIR invalid type");
        reject(hir,hv,[](auto& x){x.nodes[0].source={2};},"HIR wrong source ID");
        reject(hir,hv,[](auto& x){x.nodes[0].span.end_offset=100000;},"HIR source bounds");
        reject(hir,hv,[](auto& x){x.nodes[0].kind=static_cast<HirKind>(255);},"HIR invalid kind");
        reject(hir,hv,[&](auto& x){hkind(x,HirKind::Binary).effects=0;},"HIR effects underclaim");
        reject(hir,hv,[&](auto& x){hkind(x,HirKind::Call).symbol={999};},"HIR unknown callee");
        reject(hir,hv,[&](auto& x){hkind(x,HirKind::Constant).constant=ScalarConstant(3.5);},"HIR mismatched scalar constant");
        reject(hir,hv,[&](auto& x){hkind(x,HirKind::While).children.pop_back();},"HIR arity");
        reject(hir,hv,[&](auto& x){auto& n=hkind(x,HirKind::Store);x.tables.symbols[n.symbol.value-1].mutable_binding=false;},"HIR readonly store");
        reject(hir,hv,[](auto& x){x.functions[1].info.parameters.clear();},"HIR function signature");
        reject(hir,hv,[](auto& x){x.tables.symbols[0].id={2};},"HIR stable symbol table");
        reject(mir,mv,[](auto& x){x.functions[0].entry={0};},"MIR invalid entry");
        reject(mir,mv,[](auto& x){x.functions[0].values[0].id={0};},"MIR dense value IDs");
        reject(mir,mv,[](auto& x){find(x,MirOp::Load).symbol={999};},"MIR invalid symbol");
        reject(mir,mv,[](auto& x){find(x,MirOp::Constant).reference_steps=2;},"MIR budget charge");
        reject(mir,mv,[](auto& x){find(x,MirOp::Constant).cancellation_checkpoint=false;},"MIR cancellation checkpoint");
        reject(mir,mv,[](auto& x){find(x,MirOp::Constant).failure={0};},"MIR missing failure edge");
        reject(mir,mv,[](auto& x){find(x,MirOp::Constant).op=static_cast<MirOp>(255);},"MIR invalid operation");
        reject(mir,mv,[](auto& x){find(x,MirOp::Store).effects=0;},"MIR missing write effect");
        reject(mir,mv,[](auto& x){auto& i=find(x,MirOp::Binary);std::swap(i.operands[0],i.operands[1]);},"MIR stack operand order");
        reject(mir,mv,[](auto& x){find(x,MirOp::Call).contextual.clear();},"MIR call argument metadata");
        reject(mir,mv,[](auto& x){find(x,MirOp::Call).text="string";},"MIR legacy call annotation");
        reject(mir,mv,[](auto& x){auto& f=x.functions[0];f.blocks[0].instructions[1].result=f.blocks[0].instructions[0].result;},"MIR multiple SSA definitions");
        reject(mir,mv,[](auto& x){for(auto& f:x.functions)for(auto& b:f.blocks)if(b.terminator.kind==TermKind::Branch){b.terminator.taken.target={999};return;}},"MIR CFG target");
        reject(mir,mv,[](auto& x){for(auto& f:x.functions)for(auto& b:f.blocks)if(!b.parameters.empty()){b.parameters.clear();return;}},"MIR block parameter arity");
        reject(mir,mv,[](auto& x){for(auto& f:x.functions)for(auto& b:f.blocks)if(b.terminator.kind==TermKind::Branch){b.terminator.next=b.terminator.taken;return;}},"MIR physical fallthrough");
        reject(mir,mv,[](auto& x){find(x,MirOp::Unwind).count=999;},"MIR scope unwind");
        reject(mir,mv,[](auto& x){find(x,MirOp::RangeInit).op=MirOp::Drop;find(x,MirOp::Drop).operands.resize(1);},"MIR iterator lifetime");
        reject(mir,mv,[](auto& x){for(const auto& y:x.tables.symbols)if(y.name=="spare"){find(x,MirOp::Store).symbol=y.id;return;}},"MIR location identity");
        reject(mir,mv,[](auto& x){x.functions[0].blocks.back().terminator.kind=TermKind::Return;},"MIR propagation terminal");
        auto bad_model=model;bad_model.functions.erase("choose");
        try{lower_bytecode(mir,bad_model);check(false,"adapter missing declarations");}
        catch(const hua::Diagnostic& d){check(d.code()=="E8002","adapter rejects missing declarations");}
        auto changed=mir;changed.functions[1].info.span.start_offset++;
        try{lower_bytecode(changed,model);check(false,"adapter mismatched declaration");}
        catch(const hua::Diagnostic& d){check(d.code()=="E8002","adapter rejects declaration identity mismatch");}
    }catch(const std::exception& e){check(false,std::string("unexpected: ")+e.what());}
    std::cout<<checks<<" E1 validation checks, "<<failures<<" failures\n";return failures?1:0;
}
