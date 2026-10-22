// One own-source compiler run. Read-only call observer; no input analysis in C++.
#include "hua/archive.hpp"
#include "hua/vm.hpp"
#include "hua/entry.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <map>
#include <unordered_map>
struct Trace final:hua::VMCallObserver{
 using Clock=std::chrono::steady_clock;
 struct Name{std::string file,name,phase;};
 struct Work{std::uint64_t entered{},completed{},inclusive{},exclusive{};};
 struct Frame{const hua::Node* fn;std::uint64_t begin,children{};std::size_t visit{std::size_t(-1)};};
 struct Visit{std::string target;bool probing{};std::uint64_t begin{},end{},logical_work{},values{},ordinal{};};
 hua::ExecutionMetrics& metrics;std::ofstream events;Clock::time_point start=Clock::now();
 std::unordered_map<const hua::Node*,Name> names;std::unordered_map<const hua::Node*,Work> work;
 std::vector<Frame> stack;std::vector<Visit> visits;std::vector<std::pair<const hua::Node*,std::string>> phases;
 std::map<std::string,std::uint64_t> volumes,body_ordinals;
 const hua::Node* capability{};std::uint64_t last_progress{},last_origin_size{},max_origin_size{};
 explicit Trace(const hua::SemanticModel& model,hua::ExecutionMetrics& m,const std::filesystem::path& path):metrics(m),events(path){
  if(!events)throw std::runtime_error("trace unavailable");
  std::map<std::pair<std::string,std::string>,std::string> routes={
   {{"types.hua","analyze"},"types"},{{"types.hua","infer"},"return-infer"},
   {{"modules.hua","analyze_compiler"},"names"},{{"parser.hua","parse_compiler"},"parse"},
   {{"capabilities.hua","analyze"},"capabilities"},{{"lowering.hua","lower"},"lower"},
   {{"lowering.hua","verify"},"core-verify"},{{"bytecode.hua","compile"},"codegen"},
   {{"bytecode.hua","generate_verified"},"emit"},{{"bytecode_verify.hua","verify"},"bytecode-verify"},
   {{"archive.hua","encode"},"encode"},{{"archive.hua","publish"},"publish"}};
  for(const auto& [_,fn]:model.functions){Name n{std::filesystem::path(fn->span.file_id).filename().string(),hua::declared_name(*fn),""};
   auto k=n.name.rfind('$');if(k!=std::string::npos)n.name.erase(0,k+1);
   if(auto it=routes.find({n.file,n.name});it!=routes.end())n.phase=it->second;
   if(n.file=="capabilities.hua"&&n.name=="analyze")capability=fn;
   names.emplace(fn,std::move(n));work.emplace(fn,Work{});
  }
  if(!capability)throw std::runtime_error("not a complete compiler");
  event("begin","execution");
 }
 static const hua::Value& member(const hua::Value& v,const char* k){return std::get<hua::StructValue>(v.data).data->fields.at(k);}
 static std::uint64_t size(const hua::Value& v){return std::get<hua::MapValue>(v.data).data->entries.size();}
 std::string identity(const hua::Node* fn)const{const auto& n=names.at(fn);return n.file+"#"+n.name;}
 double seconds()const{return std::chrono::duration<double>(Clock::now()-start).count();}
 std::string active_target()const{for(auto i=stack.rbegin();i!=stack.rend();++i)if(i->visit!=std::size_t(-1))return visits[i->visit].target;return "";}
 void event(const std::string& kind,const std::string& phase){
  events<<"{\"event\":"<<std::quoted(kind)<<",\"phase\":"<<std::quoted(phase)<<",\"seconds\":"<<seconds()<<",\"steps\":"<<metrics.steps.load()<<",\"target\":"<<std::quoted(active_target())<<",\"leaf\":"<<std::quoted(stack.empty()?"":identity(stack.back().fn))<<"}\n";events.flush();
 }
 void enter(const hua::Node* fn,const hua::Node* caller,const std::vector<hua::Value>& args)override{
  if(!names.contains(fn))throw std::runtime_error("unrecognized function identity");
  auto step=metrics.steps.load();auto& w=work.at(fn);++w.entered;
  Frame f{fn,step};const auto& n=names.at(fn);
  if(n.file=="capabilities.hua"){
   if(n.name=="statement"&&caller==capability){auto target=std::get<std::string>(member(args.at(1),"function").data);
    f.visit=visits.size();visits.push_back({target,std::get<bool>(member(args.at(0),"probing").data),step,0,
     std::uint64_t(std::get<std::int64_t>(member(args.at(0),"work").data)),size(member(args.at(1),"values")),++body_ordinals[target]});}
   if(n.name=="instantiate_view"||n.name=="instantiate_view_at"){auto i=n.name=="instantiate_view"?3u:1u;auto v=size(member(args.at(i),"origins"));volumes["instantiate-input-origin-entries"]+=v;last_origin_size=v;max_origin_size=std::max(max_origin_size,v);}
   if(n.name=="instantiate"||n.name=="instantiate_at"){auto i=n.name=="instantiate"?3u:1u;volumes["instantiate-input-field-entries"]+=size(member(args.at(i),"fields"));}
  }
  stack.push_back(f);
  if(!n.phase.empty()){phases.emplace_back(fn,n.phase);event("begin",n.phase);}
  if(step-last_progress>=5000000){last_progress=step;event("progress",phases.empty()?"execution":phases.back().second);}
 }
 void leave(const hua::Node* fn,const hua::Value* result)override{
  if(!fn)return; // VM initializer has no corresponding function enter.
  if(stack.empty()||stack.back().fn!=fn)throw std::runtime_error("unbalanced observer");
  auto f=stack.back();auto delta=metrics.steps.load()-f.begin;auto& w=work.at(fn);++w.completed;w.inclusive+=delta;w.exclusive+=delta-f.children;
  if(f.visit!=std::size_t(-1))visits[f.visit].end=metrics.steps.load();
  const auto& n=names.at(fn);
  if(n.file=="capabilities.hua"&&n.name=="field_defaults"&&result)volumes["field-defaults-output-entries"]+=size(*result);
  if(!n.phase.empty()){event("end",n.phase);phases.pop_back();}
  stack.pop_back();if(!stack.empty())stack.back().children+=delta;
 }
 void report(std::ostream& out,bool success){
  auto total=metrics.steps.load();auto all=work;
  // Include unfinished frames at the exact failure boundary; remove their
  // active child contribution so exclusive VM work is not double counted.
  for(std::size_t i=0;i<stack.size();++i){const auto& f=stack[i];auto delta=total-f.begin;
   auto child=i+1<stack.size()?total-stack[i+1].begin:0;all[f.fn].inclusive+=delta;all[f.fn].exclusive+=delta-f.children-child;}
  std::uint64_t partition=0;
  out<<"{\"schema\":\"hua.bootstrap.failure-trace.v1\",\"success\":"<<(success?"true":"false")<<",\"seconds\":"<<seconds()<<",\"steps\":"<<total<<",\"peak_calls\":"<<metrics.peak_calls.load()<<",\"active_phase\":"<<std::quoted(phases.empty()?"":phases.back().second)<<",\"active_target\":"<<std::quoted(active_target())<<",\"helpers\":{";
  bool first=true;std::map<std::string,Work> ordered;for(auto& [fn,w]:all)if(w.entered)ordered.emplace(identity(fn),w);
  for(auto& [name,w]:ordered){if(!first)out<<',';first=false;partition+=w.exclusive;out<<std::quoted(name)<<":{\"entered\":"<<w.entered<<",\"completed\":"<<w.completed<<",\"inclusive_steps\":"<<w.inclusive<<",\"exclusive_steps\":"<<w.exclusive<<"}";}
  out<<"},\"partitioned_steps\":"<<partition<<",\"outside_function_steps\":"<<total-partition<<",\"active_stack\":[";first=true;
  for(const auto& f:stack){if(!first)out<<',';first=false;out<<std::quoted(identity(f.fn));}
  out<<"],\"visits\":[";first=true;
  for(const auto& v:visits){if(!first)out<<',';first=false;out<<"{\"target\":"<<std::quoted(v.target)<<",\"probing\":"<<(v.probing?"true":"false")<<",\"ordinal\":"<<v.ordinal<<",\"begin_step\":"<<v.begin<<",\"end_step\":"<<(v.end?v.end:total)<<",\"complete\":"<<(v.end?"true":"false")<<",\"values\":"<<v.values<<",\"logical_work_at_entry\":"<<v.logical_work<<"}";}
  out<<"],\"volumes\":{";first=true;for(auto& [k,v]:volumes){if(!first)out<<',';first=false;out<<std::quoted(k)<<':'<<v;}
  out<<"},\"last_instantiate_origin_count\":"<<last_origin_size<<",\"max_instantiate_origin_count\":"<<max_origin_size<<"}\n";
 }
};
std::string utf8(const std::filesystem::path& p){auto s=p.u8string();return std::string(s.begin(),s.end());}
int execute(const std::vector<std::filesystem::path>& paths){
 if(paths.size()!=5)throw std::runtime_error("compiler input output trace summary");
 if(std::filesystem::exists(paths[2])||std::filesystem::exists(paths[3])||std::filesystem::exists(paths[4]))throw std::runtime_error("evidence exists");
 auto image=hua::read_huab(paths[0]);hua::RuntimeContext c;c.input=&std::cin;c.working_directory=std::filesystem::current_path();c.arguments={utf8(paths[1]),utf8(paths[2])};c.execution_limits={500000000,128,65536};c.execution_metrics=std::make_shared<hua::ExecutionMetrics>();
 Trace trace(image.model,*c.execution_metrics,paths[3]);std::ofstream summary(paths[4]);
 try{auto entry=hua::VirtualMachine(image.model,std::cout,&c,&trace).run(image.bytecode);auto code=hua::report_entry(entry,std::cerr);trace.event(code?"failed":"end","execution");trace.report(summary,code==0);return code;}
 catch(const hua::Diagnostic& e){std::cerr<<e.code()<<": "<<e.what()<<"\n"<<e.span().file_id<<':'<<e.span().line<<':'<<e.span().column<<'\n';trace.event("failed",trace.phases.empty()?"execution":trace.phases.back().second);trace.report(summary,false);return 1;}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){try{std::vector<std::filesystem::path> p;for(int i=1;i<argc;++i)p.emplace_back(argv[i]);return execute(p);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#else
int main(int argc,char** argv){try{std::vector<std::filesystem::path> p;for(int i=1;i<argc;++i)p.emplace_back(argv[i]);return execute(p);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#endif
