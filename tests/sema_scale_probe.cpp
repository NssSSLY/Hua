// Host-only BO-08 diagnostics: no Hua VM invocation or semantic optimization.
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include "hua/module.hpp"
#include "hua/sema.hpp"
#include "hua/archive.hpp"
#include "hua/runtime.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
using Clock=std::chrono::steady_clock;
double cpu(){FILETIME c{},e{},k{},u{};if(!GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u))throw std::runtime_error("GetProcessTimes failed");return double(((std::uint64_t(k.dwHighDateTime)<<32)|k.dwLowDateTime)+((std::uint64_t(u.dwHighDateTime)<<32)|u.dwLowDateTime))/1e7;}
std::string quoted(const std::string& text){std::ostringstream out;constexpr char hex[]="0123456789abcdef";out<<'"';for(unsigned char c:text){if(c=='"'||c=='\\')out<<'\\'<<c;else if(c<32)out<<"\\u00"<<hex[c>>4]<<hex[c&15];else out<<c;}out<<'"';return out.str();}
void write_new(const std::filesystem::path& path,const std::string& text){if(text.size()>16777216)throw std::runtime_error("diagnostic report exceeds 16 MiB");if(std::filesystem::exists(path))throw std::runtime_error("evidence already exists");std::ofstream file(path,std::ios::binary);file<<text;if(!file)throw std::runtime_error("cannot write evidence");}
int wmain(int argc,wchar_t** argv){
 if(argc!=7){std::cerr<<"usage: sema_probe counts.json stages.json facts.json input.hua output.huab enabled|disabled\n";return 2;}
 hua::sema_profile::Counters counters;std::unique_ptr<hua::sema_profile::Scope> scope;
 if(std::wstring(argv[6])==L"enabled")scope=std::make_unique<hua::sema_profile::Scope>(counters);
 else if(std::wstring(argv[6])!=L"disabled")return 2;
 auto start=Clock::now();double initial_cpu=cpu();std::ostringstream stages;
 auto event=[&](const char* name){stages<<std::setprecision(12)<<"{\"phase\":\""<<name<<"\",\"wall_seconds\":"<<std::chrono::duration<double>(Clock::now()-start).count()<<",\"cpu_seconds\":"<<cpu()-initial_cpu<<"}\n";};
 int code=0;
 try{
  if(std::filesystem::exists(argv[5]))throw std::runtime_error("artifact already exists");
  event("begin");hua::ModuleLoader loader;const auto& tree=loader.load(argv[4]);event("loaded");
  hua::SemanticAnalyzer analyzer;auto model=analyzer.analyze(tree,loader.externals());event("analyzed");
  std::ostringstream facts;facts<<"{\"nodes\":[";bool first=true;std::size_t nodes=0;
  std::function<void(const hua::Node&)> visit=[&](const hua::Node& n){if(!first)facts<<',';first=false;++nodes;
   facts<<'['<<int(n.kind)<<','<<quoted(n.text)<<','<<quoted(n.span.file_id)<<','<<n.span.start_offset<<','<<n.span.end_offset<<','<<n.span.line<<','<<n.span.column<<',';
   if(auto found=model.types.find(&n);found!=model.types.end())facts<<quoted(found->second);else facts<<"null";facts<<',';
   if(auto found=model.writable.find(&n);found!=model.writable.end())facts<<(found->second?"true":"false");else facts<<"null";facts<<',';
   if(auto found=model.constants.find(&n);found!=model.constants.end())facts<<quoted(hua::show(found->second));else facts<<"null";facts<<']';
   for(const auto& child:n.children)visit(*child);
  };visit(tree);facts<<"],\"node_count\":"<<nodes<<",\"builtin_symbols\":"<<2*hua::builtin_names().size()<<'}';write_new(argv[3],facts.str());
  auto bytecode=hua::BytecodeCompiler{}.compile(tree,model);event("compiled");hua::write_huab(argv[5],bytecode,model,loader.sources());event("written");
  std::cout<<"Built\n";
 }catch(const hua::Diagnostic& error){std::cerr<<"error["<<error.code()<<"] "<<error.what()<<'\n';event("diagnostic");code=1;}
 catch(const std::exception& error){std::cerr<<error.what()<<'\n';event("failed");code=2;}
 try{std::ostringstream text;hua::sema_profile::write(text,counters);write_new(argv[1],text.str());write_new(argv[2],stages.str());}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 2;}
 return code;
}
