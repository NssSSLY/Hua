#pragma once
// Test-only call boundaries. No source markers, per-instruction hooks or Value roots.
#include "hua/archive.hpp"
#include "hua/vm.hpp"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <map>
#include <thread>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
#endif
namespace hua::bootstrap_phase {
struct Route {const char* phase{};const char* count{};};
struct Session;
inline thread_local Session* active{};
inline std::uint64_t cpu_ns(){
#ifdef _WIN32
 FILETIME a{},b{},k{},u{};if(GetProcessTimes(GetCurrentProcess(),&a,&b,&k,&u))return (((std::uint64_t(k.dwHighDateTime)<<32)|k.dwLowDateTime)+((std::uint64_t(u.dwHighDateTime)<<32)|u.dwLowDateTime))*100;
#endif
 return std::uint64_t(double(std::clock())/CLOCKS_PER_SEC*1e9);
}
struct Session {
 using Clock=std::chrono::steady_clock;
 std::ofstream& output;ExecutionMetrics& metrics;Clock::time_point start{Clock::now()};std::uint64_t cpu_start{cpu_ns()};
 std::unordered_map<const Node*,Route> routes;std::vector<std::pair<const Node*,const char*>> stack;
 std::map<std::string,std::uint64_t> calls,bodies;const Node* analyze{};bool complete{};
 Session(const SemanticModel& model,ExecutionMetrics& m,std::ofstream& out):output(out),metrics(m){
  if(active)throw std::runtime_error("nested bootstrap phase session");
  std::map<std::pair<std::string,std::string>,const char*> names={{{"types.hua","analyze"},"types"},{{"types.hua","infer"},"return-infer"},{{"modules.hua","analyze_compiler"},"names"},{{"parser.hua","parse_compiler"},"parse"},{{"capabilities.hua","analyze"},"capabilities"},{{"lowering.hua","lower"},"lower"},{{"lowering.hua","verify"},"core-verify"},{{"bytecode.hua","compile"},"codegen"},{{"bytecode.hua","generate_verified"},"emit"},{{"bytecode_verify.hua","verify"},"bytecode-verify"},{{"archive.hua","encode"},"encode"},{{"archive.hua","publish"},"publish"}};
  std::map<std::string,const char*> counters={{"statement","statement"},{"expression","expression"},{"field_defaults","field-defaults"},{"same_fields","same-fields"},{"editable","editable"},{"merge","merge"},{"select","select"}};
  std::size_t phases=0;
  for(const auto& [_,fn]:model.functions){auto file=std::filesystem::path(fn->span.file_id).filename().string(),name=declared_name(*fn);auto split=name.rfind('$');if(split!=std::string::npos)name.erase(0,split+1);
   Route route;if(auto found=names.find({file,name});found!=names.end()){route.phase=found->second;++phases;if(file=="capabilities.hua"&&name=="analyze")analyze=fn;}
   if(file=="capabilities.hua")if(auto found=counters.find(name);found!=counters.end())route.count=found->second;
   if(route.phase||route.count)routes.emplace(fn,route);
  }
  if(phases!=names.size()||!analyze)throw std::runtime_error("compiler phase identity mismatch");active=this;event("begin","execution");
 }
 ~Session(){active=nullptr;}
 void event(const char* kind,const char* phase){
  auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
  output<<"{\"event\":"<<std::quoted(kind)<<",\"phase\":"<<std::quoted(phase)<<",\"ns\":"<<ns<<",\"cpu_ns\":"<<cpu_ns()-cpu_start<<",\"counters\":{\"vm_steps\":"<<metrics.steps.load()<<"}}\n";output.flush();if(!output)throw std::runtime_error("phase trace write failed");
 }
 void enter(const Node* fn,const Node* caller,const std::vector<Value>& args){
  auto found=routes.find(fn);if(found==routes.end())return;const auto& route=found->second;
  if(route.count){++calls[route.count];if(std::string_view(route.count)=="statement"&&caller==analyze){
    if(args.size()<2)throw std::runtime_error("statement argument metadata mismatch");auto* e=std::get_if<StructValue>(&args[1].data);if(!e)throw std::runtime_error("statement environment missing");
    auto field=e->data->fields.find("function");if(field==e->data->fields.end())throw std::runtime_error("statement function metadata missing");auto* name=std::get_if<std::string>(&field->second.data);if(!name)throw std::runtime_error("statement function type mismatch");++bodies[*name];
  }}
  if(route.phase){stack.emplace_back(fn,route.phase);event("begin",route.phase);}
 }
 void leave(const Node* fn){auto found=routes.find(fn);if(found==routes.end()||!found->second.phase)return;
  if(stack.empty()||stack.back().first!=fn)throw std::runtime_error("unbalanced VM phase return");event("end",stack.back().second);stack.pop_back();
 }
 void finish(){if(!stack.empty())throw std::runtime_error("incomplete VM phase execution");event("end","execution");complete=true;}
 void report(std::ostream& out){out<<"{\"schema\":\"hua.bootstrap.phase-summary.v1\",\"steps\":"<<metrics.steps.load()<<",\"peak_calls\":"<<metrics.peak_calls.load()<<",\"complete\":"<<(complete?"true":"false")<<",\"calls\":{";bool first=true;
  for(const auto& [n,v]:calls){if(!first)out<<',';first=false;out<<std::quoted(n)<<':'<<v;}out<<"},\"body_visits\":{";first=true;for(const auto& [n,v]:bodies){if(!first)out<<',';first=false;out<<std::quoted(n)<<':'<<v;}out<<"}}\n";
 }
};
inline void enter(const Node* fn,const Node* caller,const std::vector<Value>& args){if(active)active->enter(fn,caller,args);}
inline void leave(const Node* fn){if(active)active->leave(fn);}
}
