#include "bootstrap_capability_profile.hpp"
#include <iostream>
struct Observer final:hua::VMCallObserver {
 hua::bootstrap_phase::Session& s;explicit Observer(hua::bootstrap_phase::Session& p):s(p){}
 static std::uint64_t fields(const hua::Value& v){
  const auto& record=std::get<hua::StructValue>(v.data).data->fields;const auto& raw=std::get<hua::MapValue>(record.at("fields").data).data->entries;
  auto cursor=record.find("prefix");if(cursor==record.end())return raw.size();const auto& prefix=std::get<std::string>(cursor->second.data);if(prefix.empty())return raw.size();
  const auto& index=std::get<hua::MapValue>(record.at("prefixes").data).data->entries;auto i=index.find(prefix);if(i==index.end())return 0;
  std::uint64_t count=0;for(const auto& [path,unused]:std::get<hua::MapValue>(i->second.data).data->entries)if(raw.contains(path))++count;return count;
 }
 void enter(const hua::Node* n,const hua::Node* caller,const std::vector<hua::Value>& args)override{
  s.enter(n,caller,args);auto h=s.helpers.find(n);if(h==s.helpers.end())return;
  if(h->second=="select"){auto fields=Observer::fields(args.at(0));s.volume["select-input-fields"]+=fields;s.volume["select-max-input-fields"]=std::max(s.volume["select-max-input-fields"],fields);}
  if(h->second=="storage_aliases"){
   if(s.size(s.member(args.at(1),"origins"))==0)++s.volume["empty-origin-alias-calls"];
   for(const auto& [_,v]:std::get<hua::MapValue>(s.member(args.at(0),"values").data).data->entries)s.volume["alias-fields"]-=s.size(s.member(v,"fields"))-Observer::fields(v);
  }
  if(h->second=="snapshot")for(const auto& [_,v]:std::get<hua::MapValue>(args.at(0).data).data->entries)s.volume["snapshot-fields"]-=s.size(s.member(v,"fields"))-Observer::fields(v);
  if(h->second=="editable")s.volume["editable-fields"]-=s.size(s.member(args.at(1),"fields"))-Observer::fields(args.at(1));
  if(h->second=="statement"&&caller==s.analyze){s.volume["work-at-last-body-entry"]=std::get<std::int64_t>(s.member(args.at(0),"work").data);s.volume["summaries-at-last-body-entry"]=s.size(s.member(args.at(0),"summaries"));}
  if(h->second=="instantiate_view"||h->second=="merge_view"){auto fields=s.size(s.member(args.at(h->second=="instantiate_view"?3:0),"origins"));s.volume["view-origin-entries"]+=fields;s.volume["view-max-origins"]=std::max(s.volume["view-max-origins"],fields);}
 }
 void leave(const hua::Node* n,const hua::Value* v)override{
  auto h=s.helpers.find(n);if(h!=s.helpers.end()&&h->second=="select"&&v){auto fields=Observer::fields(*v);s.volume["select-output-fields"]+=fields;s.volume["select-max-output-fields"]=std::max(s.volume["select-max-output-fields"],fields);}
  s.leave(n,v);
 }
 void partial(){std::uint64_t active_child=0;auto now=s.metrics.steps.load();
  for(auto i=s.work_stack.rbegin();i!=s.work_stack.rend();++i){auto total=now-i->begin;auto& w=s.work[i->name];++w.calls;w.inclusive+=total;w.exclusive+=total-i->children-active_child;
   if(!i->body.empty()){auto& b=s.body_work[i->body];++b.calls;b.inclusive+=total;s.volume["partial-body:"+i->body]=total;}
   active_child=total;
  }
 }
};
int run(const std::filesystem::path& trace,const std::filesystem::path& compiler,const std::string& input,const std::string& output,std::uint64_t limit){
 if(limit==0||limit>=500000000||std::filesystem::exists(trace)||std::filesystem::exists(std::filesystem::u8path(output)))throw std::runtime_error("invalid sample limit or existing evidence");
 auto image=hua::read_huab(compiler);hua::RuntimeContext context;context.input=&std::cin;context.working_directory=std::filesystem::current_path();context.arguments={input,output};context.execution_limits={limit,128,65536};context.execution_metrics=std::make_shared<hua::ExecutionMetrics>();std::ofstream file(trace);hua::bootstrap_phase::Session session(image.model,*context.execution_metrics,file);Observer observer(session);
 try{auto result=hua::VirtualMachine(image.model,std::cout,&context,&observer).run(image.bytecode);if(result.exit_code)throw std::runtime_error("unexpected compiler result failure");session.finish();session.report(std::cerr);return 0;}
 catch(const hua::Diagnostic& e){if(e.code()!="E4099"||context.execution_metrics->steps!=limit+1)throw;observer.partial();session.volume["intentional-sample-step-limit"]=limit;session.report(std::cerr);std::cout<<"bounded diagnostic sample stopped at "<<limit<<" steps; no successful compilation is claimed\n";return 0;}
}
#ifdef _WIN32
std::string utf8(const wchar_t* p){int n=WideCharToMultiByte(CP_UTF8,0,p,-1,nullptr,0,nullptr,nullptr);std::string s(n,'\0');WideCharToMultiByte(CP_UTF8,0,p,-1,s.data(),n,nullptr,nullptr);s.pop_back();return s;}
int wmain(int argc,wchar_t** argv){try{return argc==6?run(argv[1],argv[2],utf8(argv[3]),utf8(argv[4]),std::stoull(argv[5])):2;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#else
int main(int argc,char** argv){try{return argc==6?run(argv[1],argv[2],argv[3],argv[4],std::stoull(argv[5])):2;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#endif
