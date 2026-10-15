#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include "hua/vm.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
int main(){try{
    hua::SemanticModel model;std::ostringstream output;hua::VirtualMachine vm(model,output);unsigned checks=0;
    for(unsigned i=0;i<32;++i){bool text=i%2;std::string type=text?"string":"int",literal=text?"\"tag\"":"7";
        std::string code="struct Box{value "+type+"}\nfn value(x "+type+") "+type+"{return x}\nfn pair(x "+type+")("+type+",Result<"+type+">){return value(x),ok(x)}\nfn touch(b mut Box,x "+type+"){b.value=value(x)}\nfn main(){var b=Box{value:"+literal+"}\nlet a,r=pair("+literal+")\ntouch(b,unwrap(r))\nprint(b.value,a)}\n";
        hua::Source source("reused-vm.hua",code);auto tree=hua::Parser(hua::Lexer(source).scan()).parse();
        model=hua::SemanticAnalyzer{}.analyze(*tree);auto bytecode=hua::BytecodeCompiler{}.compile(*tree,model);output.str("");output.clear();
        if(vm.run(bytecode).exit_code)throw std::runtime_error("reused VM entry failed");++checks;
        if(output.str()!=(text?"tag tag\n":"7 7\n"))throw std::runtime_error("stale VM parameter/return/field metadata after old tree destruction");++checks;
    }
    std::cout<<checks<<" VM metadata reuse checks passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
