// Test-only function work attribution. No frontend or compiler is substituted.
#include "hua/archive.hpp"
#include "hua/vm.hpp"
#include "function_work_profile.hpp"
#include <fstream>
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif
int run(const std::filesystem::path& trace,const std::filesystem::path& compiler,const std::string& input,const std::string& output){
 if(std::filesystem::exists(trace)||std::filesystem::exists(std::filesystem::path(std::u8string(output.begin(),output.end()))))throw std::runtime_error("evidence already exists");
 auto image=hua::read_huab(compiler);hua::RuntimeContext context;context.input=&std::cin;context.working_directory=std::filesystem::current_path();context.arguments={input,output};context.execution_limits={500000000,128,65536};context.execution_metrics=std::make_shared<hua::ExecutionMetrics>();
 hua::performance::Counters counters;int result=0;
 {
  hua::performance::Scope scope(counters);hua::test_work::Session work(counters);
  try{result=hua::report_entry(hua::VirtualMachine(image.model,std::cout,&context).run(image.bytecode),std::cerr);}
  catch(const hua::Diagnostic& e){std::cerr<<"error["<<e.code()<<"] "<<e.what()<<'\n';result=1;}
  std::ofstream file(trace);work.write(file);if(!file||file.tellp()>16777216)throw std::runtime_error("work report failed or exceeds16MiB");
 }
 hua::performance::write(std::cerr,counters);std::cerr<<'\n';return result;
}
#ifdef _WIN32
std::string utf8(const wchar_t* p){int n=WideCharToMultiByte(CP_UTF8,0,p,-1,nullptr,0,nullptr,nullptr);std::string s(n,'\0');WideCharToMultiByte(CP_UTF8,0,p,-1,s.data(),n,nullptr,nullptr);s.pop_back();return s;}
int wmain(int argc,wchar_t** argv){try{return argc==5?run(argv[1],argv[2],utf8(argv[3]),utf8(argv[4])):2;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#else
int main(int argc,char** argv){return argc==5?run(argv[1],argv[2],argv[3],argv[4]):2;}
#endif
