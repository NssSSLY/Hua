// Test-only measurements of immutable transforms and sparse local state.
#include "bootstrap_capability_profile.hpp"
#include <iostream>
#include <set>
using namespace hua;
namespace {
Value field(const Value& v,const char* key){if(auto p=std::get_if<PlainRecord>(&v.data))return *plain_record_field(*p,key);return std::get<StructValue>(v.data).data->fields.at(key);}
const auto& map(const Value& v){return std::get<MapValue>(v.data).data->entries;}
std::size_t slots(const Value& v){return map(field(v,"fields")).size();}
std::size_t slice(const Value& v){if(auto n=std::get_if<std::int64_t>(&v.data))return *n!=0;return std::get<SliceValue>(v.data).length;}
struct Observer final:VMCallObserver{
 bootstrap_phase::Session& session;bool lazy{};std::map<std::string,std::uint64_t> counts;
 explicit Observer(bootstrap_phase::Session& s):session(s){for(auto& [fn,n]:s.helpers)if(n=="instantiate_flat")lazy=true;}
 void enter(const Node* fn,const Node* caller,const std::vector<Value>& args)override{
  session.enter(fn,caller,args);auto i=session.helpers.find(fn);if(i==session.helpers.end())return;const auto& n=i->second;auto offset=std::size_t(session.pooled?1:0);
  if(n==(lazy?"instantiate_flat":"instantiate_at")){++counts["returned_values"];counts["return_backing_slots"]+=slots(args[1+offset]);if(!lazy)counts["eager_return_field_substitutions"]+=slots(args[1+offset]);}
  if(n==(lazy?"flat_instantiate_view":"instantiate_view_at")){++counts["view_substitutions"];counts["substitution_origin_visits"]+=map(field(args[1],"origins")).size();}
  if(n=="raw_field"){++counts["queried_fields"];if(slice(field(args[offset],"plan")))++counts["queried_deferred_fields"];}
  if(n=="materialized"){
   auto v=args[offset];bool flatten=!std::get<std::string>(field(v,"prefix").data).empty()||std::get<bool>(field(v,"readonly_fields").data)||(lazy&&slice(field(v,"plan")));
   if(flatten){++counts["materializations"];counts["materialization_backing_slots"]+=slots(v);}
  }
  if(n=="join")counts["join_candidate_bindings"]+=map(args[1+offset]).size();
  if(n=="join_branches"){
   std::set<std::string> keys;for(int p:{1,2})for(auto& [k,v]:map(field(args[p+offset],"touched")))keys.insert(std::get<std::string>(k));counts["join_candidate_bindings"]+=keys.size();
  }
  if(n=="commit"&&lazy){auto journals=field(args[0],"journals");auto key=std::get<std::string>(args[1].data);for(auto& [id,j]:map(journals))if(!map(field(j,"touched")).contains(key))++counts["journal_first_writes"];}
 }
 void leave(const Node* fn,const Value* result)override{
  auto i=session.helpers.find(fn);if(result&&i!=session.helpers.end()&&i->second=="saved")counts["journal_saved_bindings"]+=map(field(*result,"after")).size();session.leave(fn,result);
 }
 void report(){std::cerr<<"{\"schema\":\"hua.c04.representation-work.v1\",\"lazy\":"<<(lazy?"true":"false")<<",\"counts\":{";bool first=true;for(auto& [key,value]:counts){if(!first)std::cerr<<',';first=false;std::cerr<<std::quoted(key)<<':'<<value;}std::cerr<<"}}\n";}
};
int run(const std::vector<std::filesystem::path>& p){
 if(p.size()!=4||std::filesystem::exists(p[0])||std::filesystem::exists(p[3]))throw std::runtime_error("expected new trace compiler source output");
 auto image=read_huab(p[1]);RuntimeContext context;context.input=&std::cin;context.working_directory=std::filesystem::current_path();
 auto utf8=[](const std::filesystem::path& q){auto s=q.u8string();return std::string(s.begin(),s.end());};context.arguments={utf8(p[2]),utf8(p[3])};context.execution_limits={500000000,128,65536};context.execution_metrics=std::make_shared<ExecutionMetrics>();
 std::ofstream trace(p[0]);bootstrap_phase::Session session(image.model,*context.execution_metrics,trace);Observer observer(session);
 auto result=report_entry(VirtualMachine(image.model,std::cout,&context,&observer).run(image.bytecode),std::cerr);if(result)return result;session.finish();session.report(std::cerr);observer.report();return 0;
}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){try{std::vector<std::filesystem::path> p;for(int i=1;i<argc;++i)p.emplace_back(argv[i]);return run(p);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#else
int main(int argc,char** argv){try{std::vector<std::filesystem::path> p;for(int i=1;i<argc;++i)p.emplace_back(argv[i]);return run(p);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#endif
