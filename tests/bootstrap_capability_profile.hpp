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
 struct Work {std::uint64_t calls{},inclusive{},exclusive{};};
 struct Frame {const Node* fn{};std::string name,body,type;std::uint64_t begin{},children{};};
 std::map<std::string,Work> work,body_work;std::vector<Frame> work_stack;
 std::unordered_map<const Node*,std::string> helpers;std::map<std::string,std::uint64_t> volume;
 std::map<std::string,std::uint64_t> calls,bodies;const Node* analyze{};bool complete{};
 Session(const SemanticModel& model,ExecutionMetrics& m,std::ofstream& out):output(out),metrics(m){
  if(active)throw std::runtime_error("nested bootstrap phase session");
  std::map<std::pair<std::string,std::string>,const char*> names={{{"types.hua","analyze"},"types"},{{"types.hua","infer"},"return-infer"},{{"modules.hua","analyze_compiler"},"names"},{{"parser.hua","parse_compiler"},"parse"},{{"capabilities.hua","analyze"},"capabilities"},{{"lowering.hua","lower"},"lower"},{{"lowering.hua","verify"},"core-verify"},{{"bytecode.hua","compile"},"codegen"},{{"bytecode.hua","generate_verified"},"emit"},{{"bytecode_verify.hua","verify"},"bytecode-verify"},{{"archive.hua","encode"},"encode"},{{"archive.hua","publish"},"publish"}};
  std::map<std::string,const char*> counters={{"statement","statement"},{"expression","expression"},{"field_defaults","field-defaults"},{"same_fields","same-fields"},{"editable","editable"},{"merge","merge"},{"select","select"}};
  std::size_t phases=0;
  for(const auto& [_,fn]:model.functions){auto file=std::filesystem::path(fn->span.file_id).filename().string(),name=declared_name(*fn);auto split=name.rfind('$');if(split!=std::string::npos)name.erase(0,split+1);
   Route route;if(auto found=names.find({file,name});found!=names.end()){route.phase=found->second;++phases;if(file=="capabilities.hua"&&name=="analyze")analyze=fn;}
   if(file=="capabilities.hua"){helpers.emplace(fn,name);if(auto found=counters.find(name);found!=counters.end())route.count=found->second;}
   if(route.phase||route.count)routes.emplace(fn,route);
  }
  if(phases!=names.size()||!analyze)throw std::runtime_error("compiler phase identity mismatch");active=this;event("begin","execution");
 }
 ~Session(){active=nullptr;}
 void event(const char* kind,const char* phase){
  auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
  output<<"{\"event\":"<<std::quoted(kind)<<",\"phase\":"<<std::quoted(phase)<<",\"ns\":"<<ns<<",\"cpu_ns\":"<<cpu_ns()-cpu_start<<",\"counters\":{\"vm_steps\":"<<metrics.steps.load()<<"}}\n";output.flush();if(!output)throw std::runtime_error("phase trace write failed");
 }
 static const Value& member(const Value& v,const char* key){auto* p=std::get_if<StructValue>(&v.data);if(!p)throw std::runtime_error("expected record in capability diagnostic");return p->data->fields.at(key);}
 static std::uint64_t size(const Value& v){auto* m=std::get_if<MapValue>(&v.data);if(!m)throw std::runtime_error("expected map in capability diagnostic");return m->data->entries.size();}
 static std::string string(const Value& v){return std::get<std::string>(v.data);}
 void enter(const Node* fn,const Node* caller,const std::vector<Value>& args){
  if(auto h=helpers.find(fn);h!=helpers.end()){
   Frame f{fn,h->second,"","",metrics.steps.load(),0};
   if(f.name=="statement"&&caller==analyze)f.body=string(member(args.at(1),"function"));
   if(f.name=="field_defaults"){f.type=string(member(args.at(1),"type_name"));volume["seed-root-origins"]+=size(member(args.at(1),"origins"));}
   if(f.name=="snapshot"){volume["snapshot-bindings"]+=size(args.at(0));for(const auto& [_,v]:std::get<MapValue>(args[0].data).data->entries)volume["snapshot-fields"]+=size(member(v,"fields"));}
   if(f.name=="editable")volume["editable-fields"]+=size(member(args.at(1),"fields"));
   if(f.name=="storage_aliases"){const auto& values=member(args.at(0),"values");volume["alias-bindings"]+=size(values);for(const auto& [_,v]:std::get<MapValue>(values.data).data->entries)volume["alias-fields"]+=size(member(v,"fields"));}
   if(f.name=="copy_view")volume["copy-view-origins"]+=size(member(args.at(0),"origins"));
   work_stack.push_back(std::move(f));
  }
  auto found=routes.find(fn);if(found==routes.end())return;const auto& route=found->second;
  if(route.count){++calls[route.count];if(std::string_view(route.count)=="statement"&&caller==analyze)++bodies[string(member(args.at(1),"function"))];}
  if(route.phase){stack.emplace_back(fn,route.phase);event("begin",route.phase);}
 }
 void leave(const Node* fn,const Value* result){
  if(helpers.contains(fn)){
   if(work_stack.empty()||work_stack.back().fn!=fn)throw std::runtime_error("unbalanced capability helper");
   auto f=std::move(work_stack.back());work_stack.pop_back();auto delta=metrics.steps.load()-f.begin;
   if(delta<f.children)throw std::runtime_error("invalid helper step partition");auto& w=work[f.name];++w.calls;w.inclusive+=delta;w.exclusive+=delta-f.children;
   if(!work_stack.empty())work_stack.back().children+=delta;
   if(!f.body.empty()){auto& b=body_work[f.body];++b.calls;b.inclusive+=delta;b.exclusive+=delta-f.children;}
   if(f.name=="field_defaults"&&result){auto count=size(*result);volume["default-instantiated-fields"]+=count;volume["shape-max:"+f.type]=std::max(volume["shape-max:"+f.type],count);for(const auto& [_,v]:std::get<MapValue>(result->data).data->entries)volume["default-instantiated-origins"]+=size(member(v,"origins"));}
   if(f.name=="analyze"&&result){const auto& model=*std::get<ResultValue>(result->data).payload;volume["actual-passes"]=std::get<std::int64_t>(member(model,"passes").data);volume["logical-work"]=std::get<std::int64_t>(member(model,"work").data);}
  }
  auto found=routes.find(fn);if(found==routes.end()||!found->second.phase)return;
  if(stack.empty()||stack.back().first!=fn)throw std::runtime_error("unbalanced VM phase return");event("end",stack.back().second);stack.pop_back();
 }
 void finish(){if(!work_stack.empty())throw std::runtime_error("unbalanced helper end");if(!stack.empty())throw std::runtime_error("incomplete VM phase execution");event("end","execution");complete=true;}
 void report(std::ostream& out){out<<"{\"schema\":\"hua.bootstrap.phase-summary.v1\",\"steps\":"<<metrics.steps.load()<<",\"peak_calls\":"<<metrics.peak_calls.load()<<",\"complete\":"<<(complete?"true":"false")<<",\"calls\":{";bool first=true;
  for(const auto& [n,v]:calls){if(!first)out<<',';first=false;out<<std::quoted(n)<<':'<<v;}out<<"},\"body_visits\":{";first=true;for(const auto& [n,v]:bodies){if(!first)out<<',';first=false;out<<std::quoted(n)<<':'<<v;}out<<"},\"helper_work\":{";first=true;for(const auto& [n,v]:work){if(!first)out<<',';first=false;out<<std::quoted(n)<<":{\"calls\":"<<v.calls<<",\"inclusive\":"<<v.inclusive<<",\"exclusive\":"<<v.exclusive<<'}';}out<<"},\"body_work\":{";first=true;for(const auto& [n,v]:body_work){if(!first)out<<',';first=false;out<<std::quoted(n)<<":{\"calls\":"<<v.calls<<",\"steps\":"<<v.inclusive<<'}';}out<<"},\"volumes\":{";first=true;for(const auto& [n,v]:volume){if(!first)out<<',';first=false;out<<std::quoted(n)<<':'<<v;}out<<"}}\n";
 }
};
inline void enter(const Node* fn,const Node* caller,const std::vector<Value>& args){if(active)active->enter(fn,caller,args);}
inline void leave(const Node* fn,const Value* result){if(active)active->leave(fn,result);}
}
