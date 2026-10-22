// Prepare the unchanged native frontend once; exercise Hua analysis using AST/VM.
// No input-program semantic analysis is delegated to C++ by this test driver.
#include "hua/module.hpp"
#include "hua/archive.hpp"
#include "hua/sema.hpp"
#include "hua/interpreter.hpp"
#include "hua/vm.hpp"
#include "hua/entry.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
using namespace hua;
std::string utf8(const std::filesystem::path& p){auto s=p.u8string();return std::string(s.begin(),s.end());}
int run(const std::vector<std::filesystem::path>& p){
 if(p.size()<3||std::filesystem::exists(p[0]))throw std::runtime_error("trace entry cases...");
 std::ofstream trace(p[0]);auto start=std::chrono::steady_clock::now();auto event=[&](const char* s){trace<<s<<' '<<std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<<'\n';trace.flush();};
 std::size_t first=2;bool archive=p.size()>4&&p[2]=="--archive";if(archive)first=4;
 event("prepare-begin");ModuleLoader loader;const auto& tree=loader.load(p[1]);auto model=SemanticAnalyzer{}.analyze(tree,loader.externals());auto code=BytecodeCompiler{}.compile(tree,model);if(archive){if(std::filesystem::exists(p[3]))throw std::runtime_error("artifact already exists");write_huab(p[3],code,model,loader.sources());}event("prepare-end");
 for(bool ast:{true,false}){
  RuntimeContext c;c.input=&std::cin;c.working_directory=std::filesystem::current_path();c.execution_limits={500000000,128,65536};c.execution_metrics=std::make_shared<ExecutionMetrics>();
  for(std::size_t i=first;i<p.size();++i)c.arguments.push_back(utf8(p[i]));
  event(ast?"AST-begin":"VM-begin");std::cout<<(ast?"@@prepared:AST\n":"@@prepared:VM\n")<<std::flush;
  auto result=ast?Interpreter(model,std::cout,&c).run(tree):VirtualMachine(model,std::cout,&c).run(code);
  if(auto status=report_entry(result,std::cerr))return status;event(ast?"AST-end":"VM-end");
 }
 return 0;
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){try{std::vector<std::filesystem::path> p;for(int i=1;i<argc;++i)p.emplace_back(argv[i]);return run(p);}catch(const Diagnostic& e){std::cerr<<e.code()<<' '<<e.what()<<' '<<e.span().file_id<<':'<<e.span().line<<'\n';return 1;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#else
int main(int argc,char** argv){try{std::vector<std::filesystem::path> p;for(int i=1;i<argc;++i)p.emplace_back(argv[i]);return run(p);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#endif
