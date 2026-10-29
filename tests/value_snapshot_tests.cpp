#include "hua/value.hpp"
#include "hua/enum_value.hpp"
#include "support/native_allocations.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <limits>
#include <utility>
using namespace hua;
namespace {
unsigned checks=0;
void check(bool ok,const char* reason){++checks;if(!ok)throw std::runtime_error(reason);}
Value record(std::string text){
 auto layout=std::make_shared<RecordLayout>(std::vector<std::string>{"name","n"},"Text",std::map<std::string,std::string>{{"name","string"},{"n","int"}});
 auto data=managed<StructData>();data->name="Text";data->fields=RecordFields(layout);
 data->fields.emplace("name",Value(std::move(text)));data->fields.emplace("n",Value(std::int64_t(1)));
 return Value(StructValue{std::move(data),true});
}
RecordFields& fields(Value& v){return std::get<StructValue>(v.data).data->fields;}
const Value& read(const Value& v,const std::string& name){return std::as_const(std::get<StructValue>(v.data).data->fields).at(name);}
void snapshots(){
 auto source=record(std::string(256,'x'));auto a=copy_value(source),b=copy_value(source);
 check(fields(source).backing_identity()==fields(a).backing_identity(),"snapshot rebuilt field table");
 check(fields(a).backing_identity()==fields(b).backing_identity(),"second snapshot did not share");
 check(string_value(read(source,"name").data).data()==string_value(read(a,"name").data).data(),"long string was copied");
 fields(a).at("n")=Value(std::int64_t(9));
 check(as_int(read(source,"n"),{})==1&&as_int(read(b,"n"),{})==1,"write changed other snapshot");
 check(as_int(read(a,"n"),{})==9,"detached write missing");
 auto& escaped=fields(source).at("n");auto late=copy_value(source);escaped=Value(std::int64_t(7));
 check(as_int(read(late,"n"),{})==1,"escaped location changed later snapshot");
 auto body=fields(b).backing_identity();
 for(unsigned fail=0;fail<5;++fail){auto candidate=copy_value(b);native_allocation::fail_after(fail);
  try{fields(candidate).at("n")=Value(std::int64_t(12));}catch(const std::bad_alloc&){}native_allocation::clear_failure();
  check(as_int(read(b,"n"),{})==1&&fields(b).backing_identity()==body,"failed detach damaged source");
 }
 // Nested aliases force a private field body before publication.
 auto child=record("nested");auto layout=std::make_shared<RecordLayout>(std::vector<std::string>{"child"},"Outer",std::map<std::string,std::string>{{"child","Text"}});
 auto data=managed<StructData>();data->name="Outer";data->fields=RecordFields(layout);data->fields.emplace("child",copy_value(child));
 Value parent(StructValue{data,true});auto frozen=copy_value(parent);
 auto& nested=fields(parent).at("child");fields(nested).at("n")=Value(std::int64_t(8));
 check(as_int(read(read(frozen,"child"),"n"),{})==1,"nested alias crossed snapshot boundary");
}
void shared_clear(){
 auto source=record("clear");auto snapshot=copy_value(source);auto layout=fields(source).retain_layout();
 native_allocation::fail_after(0);
 try{fields(source).clear();}catch(...){native_allocation::clear_failure();throw;}
 native_allocation::clear_failure();check(fields(source).empty()&&fields(source).retain_layout()==layout,"shared clear lost layout");
 check(string_value(read(snapshot,"name").data)=="clear","clear changed another snapshot");
 fields(source).emplace("n",Value(std::int64_t(3)));check(as_int(read(source,"n"),{})==3,"cleared shared table cannot reinitialize");
 try{fields(source).emplace("unknown",Value{});throw std::runtime_error("cleared layout accepted unknown field");}catch(const std::out_of_range&){check(true,"layout retained");}
}
void result_and_strings(){
 Value text(std::string(512,'a'));auto begin=native_allocation::snapshot();
 for(unsigned i=0;i<1000;++i){auto copy=copy_value(text);check(string_value(copy.data).data()==string_value(text.data).data(),"immutable text buffer not shared");}
 check(native_allocation::snapshot().calls==begin.calls,"long text copy allocated");
 Value result(ResultValue{true,managed<Value>(text),"string","string",true,false});
 auto payload=std::get<ResultValue>(result.data).payload;auto before=managed_heap().allocated();begin=native_allocation::snapshot();
 for(unsigned i=0;i<1000;++i){auto copy=copy_value(result);copy=enforce_type(std::move(copy),"Result<string,string>",{});check(std::get<ResultValue>(copy.data).payload==payload,"unchanged Result box rebuilt");}
 check(managed_heap().allocated()==before,"unchanged Result allocated managed payload");
 check(native_allocation::snapshot().calls==begin.calls,"unchanged Result allocated native storage");
 auto converted=enforce_type(Value(ResultValue{true,managed<Value>(Value(std::int64_t(255))),"int","string",true,true}),"Result<u8,string>",{});
 check(value_type(result_payload(converted,true,{}))=="u8","Result conversion skipped");
 auto invalid=Value(ResultValue{true,managed<Value>(Value(std::numeric_limits<double>::infinity())),"float","string",true,false});
 try{enforce_type(invalid,"Result<float,string>",{});throw std::runtime_error("nonfinite accepted");}catch(const Diagnostic& d){check(d.code()=="E4002","nonfinite diagnostic changed");}
 std::atomic<bool> ok{true};std::vector<std::jthread> threads;
 for(int i=0;i<4;++i)threads.emplace_back([&]{for(int j=0;j<1000;++j){auto copy=text;if(string_value(copy.data)!=string_value(text.data))ok=false;}});
 threads.clear();check(ok,"concurrent immutable string read changed");
}
void revoked_proof(){
 std::weak_ptr<StructData> weak;
 {auto source=record("before");auto snapshot=copy_value(source);weak=std::get<StructValue>(source.data).data;
  auto list=managed<ListData>();list->element_type="Text";list->values.push_back(source);
  // Deliberately bypass the language field type to exercise the raw write barrier.
  fields(source).at("name")=Value(ListValue{list,true});
  check(string_value(read(snapshot,"name").data)=="before","write barrier changed prior snapshot");
 }
 managed_heap().collect();check(weak.expired(),"field mutation retained stale acyclic proof");
}
void cycles(){
 std::weak_ptr<StructData> wa,wb;
 {auto a=record("cycle");auto& ad=std::get<StructValue>(a.data).data;wa=ad;
  auto list=managed<ListData>();list->element_type="Text";list->values.push_back(a);
  // A shared body whose field points back through a shared container.
  auto layout=std::make_shared<RecordLayout>(std::vector<std::string>{"items"},"Cycle",std::map<std::string,std::string>{{"items","List<Text>"}});
  ad->fields=RecordFields(layout);ad->fields.emplace("items",Value(ListValue{list,true}));
  auto b=copy_value(a);wb=std::get<StructValue>(b.data).data;list->values.push_back(b);
  managed_heap().collect();check(!wa.expired()&&!wb.expired(),"live shared cycle reclaimed");
 }
 managed_heap().collect();check(wa.expired()&&wb.expired(),"shared body cycle leaked");
}
}
int main(){try{ManagedHeap heap(65536);HeapScope scope(heap);snapshots();shared_clear();result_and_strings();cycles();revoked_proof();heap.collect();check(heap.live()==0,"session leaked");std::cout<<checks<<" shared snapshot/Result/string checks passed\n";return 0;}catch(const std::exception& e){native_allocation::clear_failure();std::cerr<<e.what()<<'\n';return 1;}}
