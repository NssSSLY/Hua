// BO-11 test-only census: read/verify existing archives, parse embedded sources.
// Never invokes a compiler in the Hua VM and never executes the inspected image.
#include "hua/archive.hpp"
#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include <array>
#include <functional>
#include <iostream>
#include <map>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
int inspect(const std::filesystem::path& path) {
    try {
        auto image=hua::read_huab(path);
        std::size_t bytes=0,tokens=0,nodes=0,functions=0,parameters=0,branches=0,depth=0;
        std::map<std::string,std::size_t> kinds;
        std::function<void(const hua::Node&,std::size_t)> visit=[&](const hua::Node& n,std::size_t d){
            ++nodes; ++kinds[std::string(hua::node_name(n.kind))];depth=std::max(depth,d);
            if(n.kind==hua::NodeKind::Function)++functions;
            if(n.kind==hua::NodeKind::Parameter)++parameters;
            for(const auto& child:n.children)visit(*child,d+1);
        };
        for(const auto& source:image.sources){
            bytes+=source->text().size();auto lexed=hua::Lexer(*source).scan();tokens+=lexed.size();
            auto tree=hua::Parser(std::move(lexed)).parse();visit(*tree,0);
        }
        std::size_t instructions=0,max_code=0,name_bytes=0,type_bytes=0,span_visits=0;
        std::array<std::size_t,41> ops{};
        auto code=[&](const hua::Code& c){
            instructions+=c.instructions.size();max_code=std::max(max_code,c.instructions.size());
            for(const auto& op:c.instructions){++ops.at(static_cast<unsigned>(op.op));
                name_bytes+=op.text.size();type_bytes+=op.type.size();++span_visits;
                if(op.op==hua::Op::Jump||op.op==hua::Op::JumpFalse||op.op==hua::Op::JumpTrue||op.op==hua::Op::IterNext)++branches;
            }
        };
        code(image.bytecode.initializer);for(const auto& [_,c]:image.bytecode.functions)code(c);
        std::cout<<"{\"schema\":\"hua.bootstrap.scale-census.v1\",\"modules\":"<<image.sources.size()
          <<",\"source_bytes\":"<<bytes<<",\"tokens\":"<<tokens<<",\"ast_nodes\":"<<nodes
          <<",\"source_functions\":"<<functions<<",\"parameters\":"<<parameters<<",\"ast_depth\":"<<depth
          <<",\"functions\":"<<image.bytecode.functions.size()<<",\"instructions\":"<<instructions
          <<",\"max_code\":"<<max_code<<",\"branches\":"<<branches<<",\"instruction_text_bytes\":"<<name_bytes
          <<",\"instruction_type_bytes\":"<<type_bytes<<",\"instruction_span_visits\":"<<span_visits<<",\"opcodes\":[";
        for(std::size_t i=0;i<ops.size();++i){if(i)std::cout<<',';std::cout<<ops[i];}
        std::cout<<"],\"node_kinds\":{";bool first=true;
        for(const auto& [k,v]:kinds){if(!first)std::cout<<',';first=false;std::cout<<'"'<<k<<"\":"<<v;}
        std::cout<<"}}\n";return 0;
    } catch(const hua::Diagnostic& e){std::cerr<<e.code()<<": "<<e.what()<<'\n';return 1;}
      catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){return argc==2?inspect(argv[1]):2;}
#else
int main(int argc,char** argv){return argc==2?inspect(argv[1]):2;}
#endif
