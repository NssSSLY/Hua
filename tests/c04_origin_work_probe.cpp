// Read-only call-boundary evidence for summary origin reuse; no product hooks.
#include "bootstrap_capability_profile.hpp"
#include "hua/enum_value.hpp"
#include <iostream>
#include <set>
#include <unordered_map>
using namespace hua;
namespace {
Value member(const Value& value,const std::string& key){if(auto p=std::get_if<PlainRecord>(&value.data))return *plain_record_field(*p,key);return std::get<StructValue>(value.data).data->fields.at(key);}
std::string string(const Value& value){return std::get<std::string>(value.data);}
std::int64_t integer(const Value& value){return std::get<std::int64_t>(value.data);}
const auto& entries(const Value& value){return std::get<MapValue>(value.data).data->entries;}
std::string owner(const Value& state,const Value& environment,const Value& id){
 auto pool=member(member(member(environment,"unit"),"tree"),"pool");auto nodes=std::get<ListValue>(member(pool,"nodes").data).data;auto index=integer(member(id,"index"));const auto& node=nodes->values.at(index);
 auto payload=std::get<StructValue>(member(node,"payload").data);auto kind=payload.data->variant->name;
 auto refs=member(state,"refs");auto ref=entries(refs).find(string(member(member(environment,"unit"),"id"))+":"+std::to_string(index));
 if(kind=="Name"||(kind=="Member"&&ref!=entries(refs).end()))return ref==entries(refs).end()?"":string(ref->second);
 if(kind!="Member")return "";
 return owner(state,environment,sequence_copy_source(std::get<SliceValue>(member(node,"children").data),0));
}
void append(std::string& out,const std::string& value){out+=std::to_string(value.size())+":"+value;}
struct Observer:VMCallObserver {
 bootstrap_phase::Session& s;std::uint64_t invokes{},eligible{},hits{},instantiated{},saved_calls{},saved_steps{},view_terms{},unique_terms{},views{},unique_views{},seed_calls{},seed_hits{},retained_tables{},retained_fields{};
 struct Call {bool eligible{},hit{};std::set<std::string> terms,views;};std::vector<Call> calls;
 std::unordered_map<std::string,std::string> last;
 std::vector<std::pair<const Node*,std::uint64_t>> instantiate_stack;
 Observer(bootstrap_phase::Session& session):s(session){if(s.pooled)throw std::runtime_error("legacy eager-origin probe: use c04_representation_probe for pooled transforms");}
 void enter(const Node* fn,const Node* caller,const std::vector<Value>& args)override{
  s.enter(fn,caller,args);auto item=s.helpers.find(fn);if(item==s.helpers.end())return;const auto& name=item->second;
  if(name=="statement"&&caller==s.analyze)last.clear();
  if(name=="parameter_seeded"){
   ++seed_calls;const auto& state=args[0];auto type=string(args[1]),owner=string(args[3]);
   auto identity=std::to_string(owner.size())+":"+owner+std::to_string(type.size())+":"+type+":"+(std::get<bool>(args[2].data)?"true":"false");
   const auto& tables=entries(member(state,"parameter_fields"));if(tables.contains(identity))++seed_hits;
   retained_tables=std::max(retained_tables,std::uint64_t(tables.size()));retained_fields=std::max(retained_fields,std::uint64_t(integer(member(state,"parameter_field_count"))));
  }
  if(name=="invoke"){
   ++invokes;Call call;auto state=args[0],env=args[1],node=args[2],function=args[3];std::string key=string(member(function,"target"));
   auto versions=member(env,"versions"),values=member(env,"values");auto children=std::get<SliceValue>(member(node,"children").data);bool valid=true;
   for(std::size_t i=1;i<children.length;++i){auto source=owner(state,env,sequence_copy_source(children,i));if(source.empty()||!entries(values).contains(source)){valid=false;break;}append(key,source);auto version=entries(versions).find(source);key+=":"+std::to_string(version==entries(versions).end()?0:integer(version->second));}
   if(valid){for(const auto& [global,unused]:entries(member(state,"global_symbols"))){append(key,std::get<std::string>(global));auto version=entries(versions).find(global);key+=":"+std::to_string(version==entries(versions).end()?0:integer(version->second));}
    auto site=string(member(member(env,"unit"),"id"))+":"+std::to_string(integer(member(member(node,"id"),"index")));
    call.eligible=true;++eligible;auto previous=last.find(site);call.hit=previous!=last.end()&&previous->second==key;if(call.hit)++hits;last[site]=std::move(key);
   }calls.push_back(std::move(call));
  }
  if(name=="instantiate")instantiate_stack.emplace_back(fn,s.metrics.steps.load());
  if(name=="instantiate_view"&&!calls.empty()){
   ++views;const auto& source=args[3];std::string view;append(view,string(member(source,"type_name")));
   for(auto flag:{"writable","readonly_children","deep_owned","record_copy"})view+=std::get<bool>(member(source,flag).data)?'1':'0';
   for(const auto& [origin,flag]:entries(member(source,"origins"))){++view_terms;auto text=std::get<std::string>(origin);calls.back().terms.insert(text);append(view,text);view+=std::get<bool>(flag.data)?'1':'0';}
   calls.back().views.insert(std::move(view));
  }
 }
 void leave(const Node* fn,const Value* result)override{
  auto item=s.helpers.find(fn);if(item!=s.helpers.end()){
   if(item->second=="instantiate"){auto frame=instantiate_stack.back();instantiate_stack.pop_back();++instantiated;if(instantiate_stack.empty()&&!calls.empty()&&calls.back().hit){++saved_calls;saved_steps+=s.metrics.steps.load()-frame.second;}}
   if(item->second=="invoke"){auto call=std::move(calls.back());calls.pop_back();unique_terms+=call.terms.size();unique_views+=call.views.size();}
  }s.leave(fn,result);
 }
 void report(std::ostream& out){out<<"{\"schema\":\"hua.c04.origin.work.v1\",\"invokes\":"<<invokes<<",\"simple_binding_calls\":"<<eligible<<",\"same_body_binding_version_hits\":"<<hits<<",\"instantiate_calls\":"<<instantiated<<",\"candidate_saved_instantiate_calls\":"<<saved_calls<<",\"candidate_saved_instantiate_inclusive_steps\":"<<saved_steps<<",\"view_calls\":"<<views<<",\"origin_terms\":"<<view_terms<<",\"origin_terms_unique_per_invoke\":"<<unique_terms<<",\"view_values_unique_per_invoke\":"<<unique_views<<",\"parameter_seed_calls\":"<<seed_calls<<",\"parameter_seed_hits\":"<<seed_hits<<",\"max_retained_tables\":"<<retained_tables<<",\"max_retained_fields\":"<<retained_fields<<"}\n";}
};
int run(const std::filesystem::path& compiler,const std::string& input,const std::string& output,const std::filesystem::path& trace){
 if(std::filesystem::exists(std::filesystem::u8path(output))||std::filesystem::exists(trace))throw std::runtime_error("evidence exists");auto image=read_huab(compiler);RuntimeContext context;context.input=&std::cin;context.working_directory=std::filesystem::current_path();context.arguments={input,output};context.execution_limits={500000000,128,65536};context.execution_metrics=std::make_shared<ExecutionMetrics>();std::ofstream file(trace);bootstrap_phase::Session session(image.model,*context.execution_metrics,file);Observer observer(session);auto result=VirtualMachine(image.model,std::cout,&context,&observer).run(image.bytecode);session.finish();session.report(std::cerr);observer.report(std::cerr);return result.exit_code;
}
}
#ifdef _WIN32
std::string utf8(const wchar_t* p){int n=WideCharToMultiByte(CP_UTF8,0,p,-1,nullptr,0,nullptr,nullptr);std::string s(n,'\0');WideCharToMultiByte(CP_UTF8,0,p,-1,s.data(),n,0,0);s.pop_back();return s;}
int wmain(int argc,wchar_t** argv){try{return argc==5?run(argv[1],utf8(argv[2]),utf8(argv[3]),argv[4]):2;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#else
int main(int argc,char** argv){try{return argc==5?run(argv[1],argv[2],argv[3],argv[4]):2;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#endif
