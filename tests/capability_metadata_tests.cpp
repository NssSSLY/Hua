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
    for(unsigned i=0;i<32;++i){
        bool fresh=i%2;auto field=i%4<2?"xs":"ys";
        std::string text="struct Box{"+std::string(field)+" []int}\nfn identity(b mut Box) Box{return b}\nfn main(){";
        text+=fresh?"var ro=[1]":"let ro=[1]";
        text+="\nvar b=Box{"+std::string(field)+":ro}\nvar c=identity(b)\nc."+field+"[0]=9}\n";
        hua::Source source("changed-fields.hua",text);auto tree=hua::Parser(hua::Lexer(source).scan()).parse();bool accepted=false;
        try{analyzer.analyze(*tree);accepted=true;}catch(const hua::Diagnostic& d){if(d.code()!="E3003")throw;}
        ++checks;if(accepted!=fresh)throw std::runtime_error("stale type shape/source owners across changed struct declarations");
    }
    {
        std::string text="struct Box{xs []int}\nvar global=Box{xs:[1]}\nfn f0(){global.xs=[3]\nf1()}\n";
        for(unsigned i=1;i<1000;++i)text+="fn f"+std::to_string(i)+"(){f"+std::to_string(i+1)+"()}\n";
        text+="fn f1000(){let ro=[2]\nglobal.xs=ro}\nfn main(){f0()\nglobal.xs[0]=9}\n";
        hua::Source source("fixed-point-fallback.hua",text);auto tree=hua::Parser(hua::Lexer(source).scan()).parse();bool rejected=false;
        try{analyzer.analyze(*tree);}catch(const hua::Diagnostic& d){if(d.code()!="E3003")throw;rejected=true;}
        ++checks;if(!rejected)throw std::runtime_error("unknown summary fallback reapplied an unproven prefix write");
    }
    std::cout<<checks<<" reused analyzer lifetime/capability checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
