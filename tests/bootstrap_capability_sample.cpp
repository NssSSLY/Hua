#include "bootstrap_capability_profile.hpp"
#include <iostream>
// Origin flow per call site and pass. Origins are "owner|path"; a callee allocation
// owner ("fn!module:node") is relabelled at every call-through, so '!' counts nesting.
struct Flow {std::uint64_t calls{},in{},out{},max_out{},max_length{},max_nesting{};};
struct BodyPass {std::uint64_t visits{},changed{},compared{},returned{},fields{},writes{},write_origins{},required{},max_length{},max_nesting{};};
// Canonical text of a VM value: record fields sorted by name, map keys in map order.
inline void canonical(std::ostream& out,const hua::Value& v){
 if(auto* b=std::get_if<bool>(&v.data)){out<<(*b?"true":"false");return;}
 if(auto* i=std::get_if<std::int64_t>(&v.data)){out<<*i;return;}
 if(auto* t=std::get_if<std::string>(&v.data)){out<<std::quoted(*t);return;}
 if(auto* r=std::get_if<hua::StructValue>(&v.data)){std::vector<std::pair<std::string,const hua::Value*>> f;for(const auto& [k,x]:r->data->fields)f.emplace_back(std::string(k),&x);std::sort(f.begin(),f.end(),[](auto& a,auto& b){return a.first<b.first;});
  out<<'{';bool first=true;for(const auto& [k,x]:f){if(!first)out<<',';first=false;out<<std::quoted(k)<<':';canonical(out,*x);}out<<'}';return;}
 if(auto* m=std::get_if<hua::MapValue>(&v.data)){out<<'{';bool first=true;for(const auto& [k,x]:m->data->entries){if(!first)out<<',';first=false;std::visit([&](const auto& key){out<<'"'<<key<<'"';},k);out<<':';canonical(out,x);}out<<'}';return;}
 if(auto* s=std::get_if<hua::SliceValue>(&v.data)){out<<'[';for(std::size_t i=0;i<s->length;++i){if(i)out<<',';canonical(out,(*s->storage)[s->start+i]);}out<<']';return;}
 if(auto* l=std::get_if<hua::ListValue>(&v.data)){out<<'[';for(std::size_t i=0;i<l->data->values.size();++i){if(i)out<<',';canonical(out,l->data->values[i]);}out<<']';return;}
 if(auto* r=std::get_if<hua::ResultValue>(&v.data)){out<<"{\"ok\":"<<(r->ok?"true":"false")<<",\"payload\":";if(r->payload)canonical(out,*r->payload);else out<<"null";out<<'}';return;}
 out<<"\"<variant:"<<v.data.index()<<">\"";
}
struct Observer final:hua::VMCallObserver {
 hua::bootstrap_phase::Session& s;explicit Observer(hua::bootstrap_phase::Session& p):s(p){}
 std::map<std::string,std::map<std::string,std::vector<std::string>>> last_writes;std::vector<std::string> flow_keys;std::map<std::string,std::size_t> order;std::size_t last{};std::uint64_t pass{};bool final_validation{};std::string body;
 std::vector<std::string> callees;std::map<std::string,Flow> flows;std::map<std::string,BodyPass> passes;std::filesystem::path facts_path;
 std::string pass_name()const{return final_validation?"final":std::to_string(pass);}
 static void shape(const hua::Value& origins,std::uint64_t& length,std::uint64_t& nesting){
  for(const auto& [k,unused]:std::get<hua::MapValue>(origins.data).data->entries){const auto& o=std::get<std::string>(k);length=std::max<std::uint64_t>(length,o.size());nesting=std::max<std::uint64_t>(nesting,std::count(o.begin(),o.end(),'!'));}
 }
 std::uint64_t view_origins(const hua::Value& v,std::uint64_t& length,std::uint64_t& nesting){const auto& o=s.member(v,"origins");shape(o,length,nesting);return s.size(o);}
 std::uint64_t value_origins(const hua::Value& v,std::uint64_t& fields,std::uint64_t& length,std::uint64_t& nesting){
  auto total=view_origins(s.member(v,"root"),length,nesting);
  for(const auto& [_,f]:std::get<hua::MapValue>(s.member(v,"fields").data).data->entries){++fields;total+=view_origins(f,length,nesting);}
  return total;
 }
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
  if(h->second=="statement"&&caller==s.analyze){
   body=s.string(s.member(args.at(1),"function"));final_validation=!std::get<bool>(s.member(args.at(0),"probing").data);
   if(body.empty())pass=0;else{auto [i,fresh]=order.emplace(body,order.size());if(pass==0)pass=1;else if(!final_validation&&i->second<=last)++pass;last=i->second;}
   if(!body.empty())++passes[pass_name()+"|"+body].visits;
  }
  if(h->second=="invoke")callees.push_back(s.string(s.member(args.at(3),"target")));
  if(h->second=="instantiate_view"||h->second=="merge_view"){
   auto parent=s.work_stack.size()>=2?s.work_stack[s.work_stack.size()-2].name:std::string("-");
   auto site=h->second=="instantiate_view"?s.string(args.at(0))+"->"+(callees.empty()?"-":callees.back())+" via "+parent:body+" via "+parent;
   auto& f=flows[pass_name()+"|"+h->second+"|"+site];++f.calls;f.in+=s.size(s.member(args.at(h->second=="instantiate_view"?3:1),"origins"));flow_keys.push_back(pass_name()+"|"+h->second+"|"+site);
  }
  if(h->second=="statement"&&caller==s.analyze){s.entry_work=s.volume["work-at-last-body-entry"]=std::get<std::int64_t>(s.member(args.at(0),"work").data);s.entry_summaries=s.volume["summaries-at-last-body-entry"]=s.size(s.member(args.at(0),"summaries"));}
  if(h->second=="instantiate_view"||h->second=="merge_view"){auto fields=s.size(s.member(args.at(h->second=="instantiate_view"?3:0),"origins"));s.volume["view-origin-entries"]+=fields;s.volume["view-max-origins"]=std::max(s.volume["view-max-origins"],fields);}
 }
 void leave(const hua::Node* n,const hua::Value* v)override{
  auto h=s.helpers.find(n);
  if(h!=s.helpers.end()){
   if(h->second=="invoke"&&!callees.empty())callees.pop_back();
   if((h->second=="instantiate_view"||h->second=="merge_view")&&v){auto& f=flows.at(flow_keys.back());flow_keys.pop_back();std::uint64_t length=0,nesting=0;auto out=view_origins(*v,length,nesting);f.out+=out;f.max_out=std::max(f.max_out,out);f.max_length=std::max(f.max_length,length);f.max_nesting=std::max(f.max_nesting,nesting);}
   if(h->second=="summary"&&v&&!body.empty()){auto& b=passes[pass_name()+"|"+body];std::uint64_t fields=0,length=0,nesting=0;
    b.returned=value_origins(s.member(*v,"returned"),fields,length,nesting);b.fields=fields;
    b.writes=s.size(s.member(*v,"writes"));b.write_origins=0;for(const auto& [_,w]:std::get<hua::MapValue>(s.member(*v,"writes").data).data->entries)b.write_origins+=view_origins(w,length,nesting);
    if(body.find("#statement")!=std::string::npos||body.find("#expression")!=std::string::npos||body.find("#analyze")!=std::string::npos){auto& keep=last_writes[pass_name()+"|"+body];keep.clear();
     for(const auto& [w,view]:std::get<hua::MapValue>(s.member(*v,"writes").data).data->entries){auto& list=keep[std::get<std::string>(w)];for(const auto& [o,unused]:std::get<hua::MapValue>(s.member(view,"origins").data).data->entries)list.push_back(std::get<std::string>(o));}}
    b.required=s.size(s.member(*v,"required"));b.max_length=length;b.max_nesting=nesting;}
   if(h->second=="analyze"&&v&&!facts_path.empty()){const auto& r=std::get<hua::ResultValue>(v->data);std::ofstream out(facts_path,std::ios::binary);
    if(r.ok&&r.payload){out<<"{\"error\":";canonical(out,s.member(*r.payload,"error"));out<<",\"facts\":";canonical(out,s.member(*r.payload,"facts"));out<<"}\n";}else out<<"{\"analyze_failed\":true}\n";
    if(!out)throw std::runtime_error("facts write failed");}
   if(h->second=="same_summary"&&v&&!body.empty()){auto& b=passes[pass_name()+"|"+body];++b.compared;if(!std::get<bool>(v->data))++b.changed;}
  }
  if(h!=s.helpers.end()&&h->second=="select"&&v){auto fields=Observer::fields(*v);s.volume["select-output-fields"]+=fields;s.volume["select-max-output-fields"]=std::max(s.volume["select-max-output-fields"],fields);}
  s.leave(n,v);
 }
 void dump(const std::filesystem::path& path){std::ofstream out(path);out<<"{\"schema\":\"hua.bootstrap.origin-flow.v1\",\"flows\":{";bool first=true;
  for(const auto& [k,f]:flows){if(!first)out<<',';first=false;out<<std::quoted(k)<<":{\"calls\":"<<f.calls<<",\"in\":"<<f.in<<",\"out\":"<<f.out<<",\"max_out\":"<<f.max_out<<",\"max_length\":"<<f.max_length<<",\"max_nesting\":"<<f.max_nesting<<'}';}
  out<<"},\"summaries\":{";first=true;for(const auto& [k,b]:passes){if(!first)out<<',';first=false;out<<std::quoted(k)<<":{\"visits\":"<<b.visits<<",\"compared\":"<<b.compared<<",\"changed\":"<<b.changed<<",\"returned_origins\":"<<b.returned<<",\"returned_fields\":"<<b.fields<<",\"writes\":"<<b.writes<<",\"write_origins\":"<<b.write_origins<<",\"required\":"<<b.required<<",\"max_length\":"<<b.max_length<<",\"max_nesting\":"<<b.max_nesting<<'}';}
  out<<"},\"writes\":{";first=true;for(const auto& [k,m]:last_writes){if(!first)out<<',';first=false;out<<std::quoted(k)<<":{";bool a=true;for(const auto& [w,list]:m){if(!a)out<<',';a=false;out<<std::quoted(w)<<":[";bool b=true;for(const auto& o:list){if(!b)out<<',';b=false;out<<std::quoted(o);}out<<']';}out<<'}';}
  out<<"}}\n";if(!out)throw std::runtime_error("origin flow write failed");}
 void partial(){std::uint64_t active_child=0;auto now=s.metrics.steps.load();
  for(auto i=s.work_stack.rbegin();i!=s.work_stack.rend();++i){auto total=now-i->begin;auto& w=s.work[i->name];++w.calls;w.inclusive+=total;w.exclusive+=total-i->children-active_child;
   if(!i->body.empty()){auto& b=s.body_work[i->body];++b.calls;b.inclusive+=total;s.volume["partial-body:"+i->body]=total;}
   active_child=total;
  }
 }
};
// Limits >=500M need the explicit beyond-production flag: diagnosis only, never an admission or budget change.
int run(const std::filesystem::path& trace,const std::filesystem::path& compiler,const std::string& input,const std::string& output,std::uint64_t limit,bool beyond=false){
 if(limit==0||(limit>=500000000&&!beyond)||std::filesystem::exists(trace)||std::filesystem::exists(std::filesystem::u8path(output)))throw std::runtime_error("invalid sample limit or existing evidence");
 auto image=hua::read_huab(compiler);hua::RuntimeContext context;context.input=&std::cin;context.working_directory=std::filesystem::current_path();context.arguments={input,output};context.execution_limits={limit,128,65536};context.execution_metrics=std::make_shared<hua::ExecutionMetrics>();std::ofstream file(trace);hua::bootstrap_phase::Session session(image.model,*context.execution_metrics,file);Observer observer(session);observer.facts_path=trace.string()+".facts.json";
 try{auto result=hua::VirtualMachine(image.model,std::cout,&context,&observer).run(image.bytecode);if(result.exit_code)throw std::runtime_error("unexpected compiler result failure");session.finish();session.report(std::cerr);observer.dump(trace.string()+".origins.json");return 0;}
 catch(const hua::Diagnostic& e){if(e.code()!="E4099"||context.execution_metrics->steps!=limit+1)throw;observer.partial();session.volume["intentional-sample-step-limit"]=limit;session.report(std::cerr);observer.dump(trace.string()+".origins.json");std::cout<<"bounded diagnostic sample stopped at "<<limit<<" steps; no successful compilation is claimed\n";return 0;}
}
#ifdef _WIN32
std::string utf8(const wchar_t* p){int n=WideCharToMultiByte(CP_UTF8,0,p,-1,nullptr,0,nullptr,nullptr);std::string s(n,'\0');WideCharToMultiByte(CP_UTF8,0,p,-1,s.data(),n,nullptr,nullptr);s.pop_back();return s;}
int wmain(int argc,wchar_t** argv){try{if(argc==7&&std::wstring(argv[6])!=L"--beyond-production-diagnostic")return 2;return argc==6||argc==7?run(argv[1],argv[2],utf8(argv[3]),utf8(argv[4]),std::stoull(argv[5]),argc==7):2;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#else
int main(int argc,char** argv){try{if(argc==7&&std::string(argv[6])!="--beyond-production-diagnostic")return 2;return argc==6||argc==7?run(argv[1],argv[2],argv[3],argv[4],std::stoull(argv[5]),argc==7):2;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#endif
