// Read-only measurements of call-local projection, merges and dependency edges.
#include "bootstrap_capability_profile.hpp"
#include <iostream>
#include <map>
#include <set>
using namespace hua;
namespace {
Value member(const Value& v,const std::string& key){if(auto p=std::get_if<PlainRecord>(&v.data))return *plain_record_field(*p,key);return std::get<StructValue>(v.data).data->fields.at(key);}
std::string str(const Value& v){return std::get<std::string>(v.data);}
bool flag(const Value& v){return std::get<bool>(v.data);}
const auto& entries(const Value& v){return std::get<MapValue>(v.data).data->entries;}
void append(std::string& out,const std::string& s){out+=std::to_string(s.size())+":"+s;}
std::string view_key(const Value& v,bool readonly){std::string out;append(out,str(member(v,"type_name")));out+=readonly?'0':flag(member(v,"writable"))?'1':'0';out+=readonly?'1':flag(member(v,"readonly_children"))?'1':'0';for(auto n:{"deep_owned","record_copy"})out+=flag(member(v,n))?'1':'0';for(const auto& [k,b]:entries(member(v,"origins"))){append(out,std::get<std::string>(k));out+=flag(b)?'1':'0';}return out;}
std::map<std::string,std::string> fields(const Value& v){std::map<std::string,std::string> out;auto prefix=str(member(v,"prefix"));auto readonly=flag(member(v,"readonly_fields"));auto fs=member(v,"fields");if(prefix.empty()){for(const auto& [key,item]:entries(fs))out[std::get<std::string>(key)]=view_key(item,readonly);}else{auto ps=member(v,"prefixes");auto found=entries(ps).find(prefix);if(found!=entries(ps).end())for(const auto& [key,unused]:entries(found->second)){auto item=entries(fs).find(key);if(item!=entries(fs).end())out[std::get<std::string>(key).substr(prefix.size())]=view_key(item->second,readonly);}}return out;}
struct Observer final:VMCallObserver {
 bootstrap_phase::Session& session;
 std::map<std::string,std::uint64_t> count;
 std::vector<std::set<std::string>> projections;
 struct Body{std::size_t depth;std::set<std::string> targets;};std::vector<Body> bodies;std::size_t statement_depth{};bool grouped{};
 explicit Observer(bootstrap_phase::Session& s):session(s){if(s.pooled)throw std::runtime_error("legacy eager-field probe: use c04_representation_probe for pooled transforms");for(const auto& [fn,n]:s.helpers)if(n=="record_dependencies")grouped=true;}
 void enter(const Node* fn,const Node* caller,const std::vector<Value>& args)override{
  session.enter(fn,caller,args);auto found=session.helpers.find(fn);if(found==session.helpers.end())return;const auto& name=found->second;
  if(name=="statement"){++statement_depth;if(caller==session.analyze)bodies.push_back({statement_depth,{}});}
  if(name=="invoke"){
   ++count["invokes"];projections.emplace_back();auto state=args[0],env=args[1],function=args[3];auto target=str(member(function,"target"));
   if(flag(member(state,"probing"))&&!str(member(env,"function")).empty()){
    ++count["dependency_consults"];if(!grouped)++count["global_dependency_queries"];if(!bodies.empty()&&bodies.back().targets.insert(target).second)++count["unique_body_dependencies"];
   }
   auto summaries=member(state,"summaries");auto summary=entries(summaries).find(target);if(summary!=entries(summaries).end()){
    auto ws=member(summary->second,"writes");std::set<std::string> owners;
    for(const auto& [k,v]:entries(ws)){auto origin=std::get<std::string>(k);owners.insert(origin.substr(0,origin.find('|')));}
    count["candidate_write_entries"]+=entries(ws).size();count["candidate_write_owners"]+=owners.size();auto params=std::get<SliceValue>(member(function,"parameters").data);count["candidate_parameter_scan_iterations"]+=params.length*entries(ws).size();count["candidate_unique_owner_scan_iterations"]+=params.length*owners.size();
   }
  }
  if(name=="projection"&&!projections.empty()&&session.helpers.contains(caller)&&session.helpers.at(caller)=="invoke"){
   ++count["direct_argument_projections"];auto id=member(args[3],"id");auto key=str(args[1])+":"+std::to_string(std::get<std::int64_t>(member(id,"index").data));if(!projections.back().insert(key).second)++count["repeat_argument_projections"];
  }
  if(name=="record_dependencies")count["global_dependency_queries"]+=entries(member(args[1],"callees")).size();
  if(name=="instantiate"||name=="instantiate_at"){
   count["instantiation_argument_values"]+=args.size();++count["instantiate_values"];auto& v=args[name=="instantiate"?3:1];if(!str(member(v,"prefix")).empty()||flag(member(v,"readonly_fields"))){++count["projected_summary_values"];count["projected_summary_fields"]+=fields(v).size();}
  }
  if(name=="instantiate_view"||name=="instantiate_view_at"){
   count["instantiation_argument_values"]+=args.size();++count["instantiated_views"];auto source=args[name=="instantiate_view"?3:1];auto actual=args[name=="instantiate_view"?4:2];auto origins=member(source,"origins");if(entries(origins).empty())++count["origin_free_views"];
   for(const auto& [key,v]:entries(origins)){auto origin=std::get<std::string>(key);auto owner=origin.substr(0,origin.find('|'));if(!entries(actual).contains(owner)&&owner.find('!')!=std::string::npos)++count["renamed_local_origins"];}
  }
  if(name=="merge"){
   ++count["merge_values"];auto a=fields(args[0]),b=fields(args[1]);count["merge_left_fields"]+=a.size();count["merge_right_fields"]+=b.size();if(a.size()>8&&a==b){++count["large_equal_merges"];count["large_equal_field_visits"]+=a.size()+b.size();}bool covered=true;for(const auto& [key,v]:a)if(!b.contains(key)){covered=false;break;}if(covered){++count["left_tables_covered"];count["covered_left_fields"]+=a.size();}
  }
 }
 void leave(const Node* fn,const Value* result)override{
  auto f=session.helpers.find(fn);if(f!=session.helpers.end()){
   if(f->second=="invoke")projections.pop_back();
   if(f->second=="statement"){if(!bodies.empty()&&bodies.back().depth==statement_depth)bodies.pop_back();--statement_depth;}
  }session.leave(fn,result);
 }
 void report(){std::cerr<<"{\"schema\":\"hua.c04.call-work.v1\",\"counts\":{";bool first=true;for(const auto& [n,v]:count){if(!first)std::cerr<<',';first=false;std::cerr<<std::quoted(n)<<':'<<v;}std::cerr<<"}}\n";}
};
int run(const std::vector<std::filesystem::path>& paths){
 if(paths.size()!=4||std::filesystem::exists(paths[0])||std::filesystem::exists(paths[3]))throw std::runtime_error("expected new trace compiler source output");
 auto image=read_huab(paths[1]);RuntimeContext context;context.input=&std::cin;context.working_directory=std::filesystem::current_path();auto utf8=[](const std::filesystem::path& p){auto s=p.u8string();return std::string(s.begin(),s.end());};context.arguments={utf8(paths[2]),utf8(paths[3])};context.execution_limits={500000000,128,65536};context.execution_metrics=std::make_shared<ExecutionMetrics>();
 std::ofstream trace(paths[0]);bootstrap_phase::Session session(image.model,*context.execution_metrics,trace);Observer observer(session);auto result=report_entry(VirtualMachine(image.model,std::cout,&context,&observer).run(image.bytecode),std::cerr);if(result)return result;session.finish();session.report(std::cerr);observer.report();return 0;
}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){try{std::vector<std::filesystem::path> paths;for(int i=1;i<argc;++i)paths.emplace_back(argv[i]);return run(paths);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#else
int main(int argc,char** argv){try{std::vector<std::filesystem::path> paths;for(int i=1;i<argc;++i)paths.emplace_back(argv[i]);return run(paths);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#endif
