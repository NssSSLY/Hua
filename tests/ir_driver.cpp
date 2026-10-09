#include "hua/ir.hpp"
#include "hua/module.hpp"
#include "hua/archive.hpp"
#include "hua/vm.hpp"
#include <filesystem>
#include <iostream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace {
int cli(const std::vector<std::string>& args){
    if(args.size()!=3&&!(args.size()==5&&args[1]=="build"&&args[3]=="-o")){
        std::cerr<<"Internal E1 probe: hua_ir_probe <hir|mir|explain|check|bytecode|run|build> file.hua [build: -o file.huab]\n";return 2;
    }
    const auto& action=args[1];
    if(action!="hir"&&action!="mir"&&action!="explain"&&action!="check"&&action!="bytecode"&&action!="run"&&action!="build")return 2;
    hua::ModuleLoader loader;
    try{
        auto path=std::filesystem::path(std::u8string(args[2].begin(),args[2].end()));
        const auto& ast=loader.load(path);
        auto model=hua::SemanticAnalyzer{}.analyze(ast,loader.externals());
        if(loader.module_count()!=1)throw hua::Diagnostic("E8001",ast.span,"E1 IR unsupported: multiple modules");
        auto hir=hua::ir::build_hir(ast,model,loader.sources());
        if(action=="hir"){std::cout<<hua::ir::dump_hir(hir);return 0;}
        auto mir=hua::ir::lower_mir(hir);
        if(action=="mir"){std::cout<<hua::ir::dump_mir(mir);return 0;}
        if(action=="explain"){std::cout<<hua::ir::explain_mir(mir);return 0;}
        auto bytecode=hua::ir::lower_bytecode(mir,model);
        if(action=="check"){std::cout<<"E1 OK\n";return 0;}
        if(action=="bytecode"){std::cout<<hua::disassemble(bytecode);return 0;}
        if(action=="build"){
            auto target=path;target.replace_extension(".huab");
            if(args.size()==5)target=std::filesystem::path(std::u8string(args[4].begin(),args[4].end()));
            if(target.extension()!=".huab")return 2;
            hua::write_huab(target,bytecode,model,loader.sources());std::cout<<"Built E1 HUAB v6\n";return 0;
        }
        hua::RuntimeContext context;context.input=&std::cin;context.working_directory=std::filesystem::current_path();
        hua::VirtualMachine(model,std::cout,&context).run(bytecode);return 0;
    }catch(const hua::Diagnostic& d){
        if(auto source=loader.source(d.span().file_id))std::cerr<<d.render(*source);
        else std::cerr<<"error["<<d.code()<<"]: "<<d.what()<<'\n';
        return 1;
    }catch(const std::exception& e){std::cerr<<"error[E0001]: "<<e.what()<<'\n';return 1;}
}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){
    SetConsoleCP(CP_UTF8);SetConsoleOutputCP(CP_UTF8);std::vector<std::string> args;
    for(int j=0;j<argc;++j){
        auto size=WideCharToMultiByte(CP_UTF8,0,argv[j],-1,nullptr,0,nullptr,nullptr);if(size<=0)return 2;
        std::string text(static_cast<std::size_t>(size),'\0');
        WideCharToMultiByte(CP_UTF8,0,argv[j],-1,text.data(),size,nullptr,nullptr);text.pop_back();args.push_back(std::move(text));
    }
    return cli(args);
}
#else
int main(int argc,char** argv){return cli(std::vector<std::string>(argv,argv+argc));}
#endif
