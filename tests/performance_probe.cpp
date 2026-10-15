#include "hua/archive.hpp"
#include "hua/vm.hpp"
#include "hua/performance.hpp"
#include <chrono>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <streambuf>
#include <ctime>
#ifdef _WIN32
#include <windows.h>
#endif
namespace {
using Clock=std::chrono::steady_clock;
std::filesystem::path path(const std::string& s){return std::filesystem::path(std::u8string(s.begin(),s.end()));}
std::uint64_t cpu_ns(){
#ifdef _WIN32
 FILETIME created{},ended{},kernel{},user{};
 if(GetProcessTimes(GetCurrentProcess(),&created,&ended,&kernel,&user))
  return (((std::uint64_t(kernel.dwHighDateTime)<<32)|kernel.dwLowDateTime)+((std::uint64_t(user.dwHighDateTime)<<32)|user.dwLowDateTime))*100;
#endif
 return std::uint64_t(double(std::clock())/CLOCKS_PER_SEC*1e9);
}
class PhaseBuffer final:public std::streambuf{
 std::streambuf* destination_;std::ofstream& trace_;hua::performance::Counters& counters_;Clock::time_point start_;std::uint64_t cpu_start_;std::string line_;
 void line(){
  constexpr std::string_view prefix="@@hua.phase:";
  if(line_.starts_with(prefix)){
   auto body=line_.substr(prefix.size());auto separator=body.find(':');
   auto event=body.substr(0,separator),phase=separator==std::string::npos?"":body.substr(separator+1);
   if((event!="begin"&&event!="end")||phase.empty()||phase.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-")!=std::string::npos)
    throw std::runtime_error("invalid diagnostic phase marker");
   record(event,phase);
  }else{destination_->sputn(line_.data(),static_cast<std::streamsize>(line_.size()));destination_->sputc('\n');}
  line_.clear();
 }
 int_type overflow(int_type c)override{
  if(traits_type::eq_int_type(c,traits_type::eof()))return traits_type::not_eof(c);
  char ch=traits_type::to_char_type(c);if(ch=='\n')line();else{
   if(line_.size()>=16777216)throw std::runtime_error("diagnostic output line exceeds 16 MiB");
   line_.push_back(ch);
  }return c;
 }
 int sync()override{return destination_->pubsync();}
public:
 PhaseBuffer(std::streambuf* destination,std::ofstream& trace,hua::performance::Counters& counters):
  destination_(destination),trace_(trace),counters_(counters),start_(Clock::now()),cpu_start_(cpu_ns()){}
 void record(const std::string& event,const std::string& phase){
  auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start_).count();
  auto cpu=cpu_ns()-cpu_start_;
  trace_<<"{\"event\":\""<<event<<"\",\"phase\":\""<<phase<<"\",\"ns\":"<<ns<<",\"cpu_ns\":"<<cpu<<",\"counters\":";
  hua::performance::write(trace_,counters_);trace_<<"}\n";trace_.flush();
  if(!trace_)throw std::runtime_error("cannot write diagnostic phase trace");
 }
 void finish(){if(!line_.empty()){destination_->sputn(line_.data(),static_cast<std::streamsize>(line_.size()));line_.clear();}destination_->pubsync();}
};
}
static int run(std::vector<std::string> args){
 std::string trace_path;
 if(!args.empty()&&args[0]=="--phase-events"){
  if(args.size()<4){std::cerr<<"usage: probe [--phase-events trace.jsonl] compiler.huab input output\n";return 2;}
  trace_path=args[1];args.erase(args.begin(),args.begin()+2);
 }
 if(args.empty())return 2;
 auto start=Clock::now();auto image=hua::read_huab(path(args[0]));auto loaded=Clock::now();
 hua::RuntimeContext context;context.input=&std::cin;
 context.working_directory=std::filesystem::current_path();context.arguments.assign(args.begin()+1,args.end());
 context.execution_limits={500000000,128,65536};context.execution_metrics=std::make_shared<hua::ExecutionMetrics>();
 hua::performance::Counters counters;int code=0;
 std::ofstream trace;if(!trace_path.empty()){trace.open(path(trace_path));if(!trace){std::cerr<<"cannot open diagnostic phase trace\n";return 2;}}
 counters.struct_histogram_enabled=trace.is_open();
 PhaseBuffer buffer(std::cout.rdbuf(),trace,counters);std::ostream output(&buffer);
 output.exceptions(std::ios::badbit|std::ios::failbit);
 {
  hua::performance::Scope scope(counters);
  if(trace.is_open())buffer.record("begin","execution");
  try{code=hua::report_entry(hua::VirtualMachine(image.model,trace.is_open()?output:std::cout,&context).run(image.bytecode),std::cerr);}
  catch(const hua::Diagnostic& error){std::cerr<<"error["<<error.code()<<"] "<<error.what()<<'\n';code=1;}
  if(trace.is_open()){buffer.finish();buffer.record("end","execution");}
 }
 auto finished=Clock::now();hua::performance::write(std::cerr,counters);std::cerr<<'\n';
 std::cerr<<"{\"load_seconds\":"<<std::chrono::duration<double>(loaded-start).count()<<",\"run_seconds\":"<<std::chrono::duration<double>(finished-loaded).count()<<",\"steps\":"<<context.execution_metrics->steps.load()<<",\"peak_calls\":"<<context.execution_metrics->peak_calls.load()<<"}\n";
 return code;
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){std::vector<std::string> args;for(int i=1;i<argc;++i){int n=WideCharToMultiByte(CP_UTF8,0,argv[i],-1,nullptr,0,nullptr,nullptr);std::string s(n,'\0');WideCharToMultiByte(CP_UTF8,0,argv[i],-1,s.data(),n,nullptr,nullptr);s.pop_back();args.push_back(std::move(s));}try{return run(std::move(args));}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#else
int main(int argc,char** argv){try{return run({argv+1,argv+argc});}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#endif
