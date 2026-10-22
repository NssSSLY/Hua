// Preparation-only diagnostic: no input program is analyzed or executed.
#include "hua/module.hpp"
#include "hua/archive.hpp"
#include "hua/sema.hpp"
#include "hua/bytecode.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
int run(const std::vector<std::filesystem::path>& p){
 if(p.size()!=3||std::filesystem::exists(p[1])||std::filesystem::exists(p[2]))throw std::runtime_error("entry new-artifact new-trace");
 std::ofstream trace(p[2]);auto start=std::chrono::steady_clock::now();
 auto event=[&](const char* name){trace<<name<<' '<<std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<<'\n';trace.flush();};
 event("load-begin");hua::ModuleLoader loader;const auto& tree=loader.load(p[0]);event("load-end");
 event("sema-begin");auto model=hua::SemanticAnalyzer{}.analyze(tree,loader.externals());event("sema-end");
 event("compile-begin");auto code=hua::BytecodeCompiler{}.compile(tree,model);event("compile-end");
 event("write-begin");hua::write_huab(p[1],code,model,loader.sources());event("write-end");return 0;
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){try{std::vector<std::filesystem::path> p;for(int i=1;i<argc;++i)p.emplace_back(argv[i]);return run(p);}catch(const hua::Diagnostic& e){std::cerr<<e.code()<<' '<<e.what()<<' '<<e.span().file_id<<':'<<e.span().line<<'\n';return 1;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#else
int main(int argc,char** argv){try{std::vector<std::filesystem::path> p;for(int i=1;i<argc;++i)p.emplace_back(argv[i]);return run(p);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#endif
