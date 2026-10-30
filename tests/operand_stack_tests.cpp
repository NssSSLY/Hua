#include "experimental/s2-1/operand_stack.hpp"
#include "hua/location.hpp"
#include "support/native_allocations.hpp"
#include <iostream>
#include <stdexcept>
using namespace hua;
namespace {
struct Local {void* binding;};
using Stack=OperandStack<Local>;
unsigned checks=0;
void check(bool ok,const char* text){++checks;if(!ok)throw std::runtime_error(text);}
void scalars(){
 Stack s;Value zero(std::int64_t(0));s.emplace_back(zero);s.clear();
 auto before=native_allocation::snapshot();
 for(std::int64_t i=0;i<10000;++i){s.emplace_back(std::in_place_type<Value>,i);auto v=s.take_value();if(std::get<std::int64_t>(v.data)!=i)throw std::runtime_error("scalar changed");}
 auto after=native_allocation::snapshot();
 check(before.calls==after.calls,"scalar push/pop allocated after warmup");
 check(s.location_capacity()==0,"scalar path created complex storage");
 s.emplace_back(Value(true));check(std::get<bool>(s.take_value().data),"bool changed");
 s.emplace_back(Value(1.25));check(std::get<double>(s.take_value().data)==1.25,"float changed");
 s.emplace_back(Value{});check(s.take_value().data.index()==0,"nil changed");
}
void stable(){
 Stack s;auto object=std::make_shared<std::string>("payload");
 std::weak_ptr<std::string> weak=object;
 Value target(std::int64_t(3));
 s.emplace_back(ValueLocation{&target,"int",true,object});auto* location=s.location_if(s.back());object.reset();
 for(int i=0;i<2000;++i){s.emplace_back(Value(std::string(256,'x')));}
 check(as_int(location_read(*location,{}),{})==3&&!weak.expired(),"position failed across stack/pool growth");
 location_write(*location,Value(std::int64_t(9)));check(as_int(target,{})==9,"position write lost");
 auto expected=string_value(s.value_if(s[1])->data);
 for(int i=0;i<2000;++i)s.emplace_back(ValueLocation{&target,"int",true});
 check(string_value(s.value_if(s[1])->data)==expected,"complex value changed during growth");
 s.resize(1);check(s.value_live()==0&&s.location_live()==1,"resize did not release operands");
 s.clear();check(weak.expired()&&s.location_live()==0,"position pool retained owner after pop");
 Value text(std::string(256,'y'));s.emplace_back(text);s.emplace_back(ValueLocation{&target,"int",true});s.clear();
 auto before=native_allocation::snapshot();
 for(int i=0;i<10000;++i){s.emplace_back(text);s.emplace_back(ValueLocation{&target,"int",true});s.clear();}
 auto after=native_allocation::snapshot();check(before.calls==after.calls,"complex operands allocate after warmup");
 s.emplace_back(text);s.replace_back(Value(false));check(s.value_live()==0&&!std::get<bool>(s.take_value().data),"replace retained complex value");
}
void roots(){
 ManagedHeap heap(256);HeapScope scope(heap);Stack s;
 auto cycle=managed<ListData>();cycle->values.emplace_back(ListValue{cycle,true});std::weak_ptr<ListData> weak=cycle;
 s.emplace_back(Value(ListValue{cycle,true}));cycle.reset();heap.collect();check(!weak.expired(),"live stack root was collected");
 s.clear();heap.collect();check(weak.expired()&&heap.live()==0,"pool retained unreachable cycle");
 auto owner=managed<ListData>();owner->values.emplace_back(ListValue{owner,true});weak=owner;
 s.emplace_back(ValueLocation{nullptr,"",true,owner});owner.reset();heap.collect();check(!weak.expired(),"live location root was collected");
 s.clear();heap.collect();check(weak.expired()&&heap.live()==0,"vacant location retained GC root");
}
void failures(){
 Value value(std::string(256,'x'));
 for(unsigned fail=0;fail<6;++fail){
  Stack s;s.emplace_back(Value(std::int64_t(42)));native_allocation::fail_after(fail);
  try{s.emplace_back(value);}catch(const std::bad_alloc&){}native_allocation::clear_failure();
  check(s.size()==1||s.size()==2,"failed push changed stack size");
  if(s.size()==2)s.pop_back();check(s.value_live()==0&&as_int(s.take_value(),{})==42,"failed value push corrupted old operand");
  s.emplace_back(value);s.clear();check(s.value_live()==0,"pool could not recover after failure");
 }
 for(unsigned fail=0;fail<6;++fail){
  Stack s;Value target(std::int64_t(1));s.emplace_back(Value(std::int64_t(7)));native_allocation::fail_after(fail);
  try{s.emplace_back(ValueLocation{&target,"int",true});}catch(const std::bad_alloc&){}native_allocation::clear_failure();
  if(s.size()==2)s.pop_back();check(s.location_live()==0&&as_int(s.take_value(),{})==7,"failed location push leaked or corrupted");
  s.emplace_back(ValueLocation{&target,"int",true});s.clear();check(s.location_live()==0,"location pool failed recovery");
 }
 // Entry growth failure must release a successfully acquired complex slot.
 Stack s;s.emplace_back(Value(std::int64_t(1)));s.emplace_back(value);s.clear();
 for(unsigned n=0;n<8;++n){native_allocation::fail_after(n);try{for(int i=0;i<200;++i)s.emplace_back(value);}catch(const std::bad_alloc&){}native_allocation::clear_failure();s.clear();check(s.value_live()==0,"entry allocation failure leaked slot");}
}
}
int main(){try{
 static_assert(sizeof(Stack::Entry)<sizeof(std::variant<Value,ValueLocation,Local>)/2);
 scalars();stable();roots();failures();
 std::cout<<checks<<" operand checks passed; Entry="<<sizeof(Stack::Entry)<<", old Entry="<<sizeof(std::variant<Value,ValueLocation,Local>)<<", Value="<<sizeof(Value)<<"\n";return 0;
}catch(const std::exception& e){native_allocation::clear_failure();std::cerr<<e.what()<<"\n";return 1;}}
