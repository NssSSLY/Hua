#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include "hua/vm.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <atomic>
#include <thread>
int main(){try{
    unsigned checks=0;
    const auto& standards=hua::standard_functions();
    // Exercise concurrent first use and retain exact declaration identity.
    std::atomic<bool> invalid{false};std::vector<std::thread> readers;
    for(unsigned i=0;i<8;++i)readers.emplace_back([&]{for(unsigned pass=0;pass<4;++pass)for(const auto& f:standards)
        if(hua::standard_function(hua::standard_name(f))!=&f)invalid.store(true);});
    for(auto& reader:readers)reader.join();
    if(invalid.load())throw std::runtime_error("standard lookup concurrent identity mismatch");++checks;
    for(const auto& f:standards){
        const auto name=hua::standard_name(f);
        if(hua::standard_function(name)!=&f)throw std::runtime_error("standard lookup identity mismatch");++checks;
        const auto wrapped="!"+name+"?";
        if(hua::standard_function(std::string_view(wrapped).substr(1,name.size()))!=&f)
            throw std::runtime_error("nonterminated standard name view mismatch");++checks;
        if(hua::standard_function(name+"?"))throw std::runtime_error("standard suffix accepted");++checks;
        if(hua::standard_function(name+std::string(1,'\0')+"ignored"))throw std::runtime_error("standard name truncated at NUL");++checks;
    }
    for(const auto* name:{"","len","ok","unwrap","std.bytes.from_text","$std$","$std$bytes$","$std$unknown$nope","$std$bytes$from_text$extra"}){
        if(hua::standard_function(name))throw std::runtime_error("unknown/core name accepted as standard");++checks;
    }
    hua::SemanticModel model;std::ostringstream output;hua::VirtualMachine vm(model,output);
    for(unsigned i=0;i<32;++i){bool text=i%2;std::string type=text?"string":"int",literal=text?"\"tag\"":"7";
        std::string code="struct Box{value "+type+"}\nfn value(x "+type+") "+type+"{return x}\nfn pair(x "+type+")("+type+",Result<"+type+">){return value(x),ok(x)}\nfn touch(b mut Box,x "+type+"){b.value=value(x)}\nfn main(){var b=Box{value:"+literal+"}\nlet a,r=pair("+literal+")\ntouch(b,unwrap(r))\nprint(b.value,a)}\n";
        hua::Source source("reused-vm.hua",code);auto tree=hua::Parser(hua::Lexer(source).scan()).parse();
        model=hua::SemanticAnalyzer{}.analyze(*tree);auto bytecode=hua::BytecodeCompiler{}.compile(*tree,model);output.str("");output.clear();
        if(vm.run(bytecode).exit_code)throw std::runtime_error("reused VM entry failed");++checks;
        if(output.str()!=(text?"tag tag\n":"7 7\n"))throw std::runtime_error("stale VM parameter/return/field metadata after old tree destruction");++checks;
    }
    std::cout<<checks<<" standard lookup / VM metadata checks passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
