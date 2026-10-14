#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include "hua/sema.hpp"
#include <iostream>
#include <stdexcept>
int main(){try{
    hua::SemanticAnalyzer analyzer;unsigned checks=0;
    for(unsigned i=0;i<32;++i){
        bool fresh=i%2;std::string text="struct Box{xs []int}\nfn make() Box{";
        text+=fresh?"return Box{xs:[1]}":"let ro=[1]\nreturn Box{xs:ro}";
        text+="}\nfn main(){var b=make()\nb.xs[0]=9}\n";
        hua::Source source("reused.hua",text);auto tree=hua::Parser(hua::Lexer(source).scan()).parse();bool accepted=false;
        try{analyzer.analyze(*tree);accepted=true;}catch(const hua::Diagnostic& d){if(d.code()!="E3003")throw;}
        ++checks;if(accepted!=fresh)throw std::runtime_error("stale return/field facts across analyzed trees");
    }
    std::cout<<checks<<" reused analyzer lifetime/capability checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
