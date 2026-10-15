// BO-02 experimental kernels only. Links the unchanged production Value library.
#include "hua/value.hpp"
#include "hua/gc.hpp"
#include "hua/json.hpp"
#include <chrono>
#include <iostream>
#include <string>
#include <variant>
#include <vector>
#include <utility>
#include <stdexcept>
#include <type_traits>
#include <cstdint>
using I=std::int64_t;
using Clock=std::chrono::steady_clock;
template<class T> inline void opaque(T& value){asm volatile("" : : "g"(&value) : "memory");}
struct Layout{
 int count;std::vector<std::string> names,fields;
 explicit Layout(int n):count(n){for(int i=0;i<n;++i){names.push_back("V"+std::to_string(i));fields.push_back("$V"+std::to_string(i)+"#0#int");}}
};
hua::Value make_value(const Layout& layout,int active,hua::Value payload,bool sparse,const std::string& suffix="int"){
 auto data=hua::managed<hua::StructData>();data->name="Item";
 data->fields.emplace("$case",hua::Value(layout.names.at(active)));
 for(int i=0;i<layout.count;++i)if(!sparse||i==active){
  auto key=i==active?"$"+layout.names[i]+"#0#"+suffix:layout.fields[i];
  data->fields.emplace(key,i==active?payload:hua::Value{});
 }
 return hua::Value(hua::StructValue{std::move(data),true});
}
I read_value(const Layout& layout,const hua::Value& value){
 const auto& fields=std::get<hua::StructValue>(value.data).data->fields;
 const auto& tag=std::get<std::string>(fields.at("$case").data);
 for(int i=0;i<layout.count;++i)if(tag==layout.names[i])return std::get<I>(fields.at(layout.fields[i]).data)+i;
 return -1;
}
I next(I state){return state*48271%2147483647;}
struct Sample{std::uint64_t ns{},allocations{},checks{};I checksum{},state{17};};
Sample value_batch(const Layout& layout,bool sparse,const std::string& op,int rounds){
 auto source=hua::read_only(make_value(layout,op=="match-first"?0:layout.count-1,hua::Value(I(17)),sparse));
 auto held=hua::copy_value(source);I state=17,checksum=0;
 auto& heap=hua::managed_heap();auto allocated=heap.allocated();auto start=Clock::now();
 for(int i=0;i<rounds;++i){
  if(op=="construct"){state=next(state);auto made=make_value(layout,layout.count-1,hua::Value(state),sparse);held=hua::copy_value(made);opaque(held);}
  else if(op=="copy"){auto snapshot=hua::copy_value(source);opaque(snapshot);held=hua::copy_value(snapshot);opaque(held);}
  else{auto argument=hua::copy_value(source);auto borrowed=hua::read_only(argument);auto subject=hua::copy_value(static_cast<const hua::Value&>(borrowed));opaque(subject);checksum+=read_value(layout,subject);}
 }
 auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
 auto delta=heap.allocated()-allocated;
 if(op=="construct"||op=="copy")checksum=read_value(layout,held);
 return {static_cast<std::uint64_t>(ns),delta,static_cast<std::uint64_t>(rounds),checksum,state};
}
template<std::size_t K>struct Case{I value;static constexpr I index=K;};
template<class>struct VariantType;
template<std::size_t... K>struct VariantType<std::index_sequence<K...>>{using type=std::variant<Case<K>...>;};
template<std::size_t N>using Variant=typename VariantType<std::make_index_sequence<N>>::type;
template<std::size_t N> Sample variant_batch(const std::string& op,int rounds){
 using V=Variant<N>;V source=op=="match-first"?V(std::in_place_index<0>,I(17)):V(std::in_place_index<N-1>,I(17));V held=source;
 I state=17,checksum=0;auto start=Clock::now();
 for(int i=0;i<rounds;++i){
  if(op=="construct"){state=next(state);V made(std::in_place_index<N-1>,state);opaque(made);held=made;opaque(held);}
  else if(op=="copy"){V snapshot=source;opaque(snapshot);held=snapshot;opaque(held);}
  else{V argument=source;opaque(argument);V subject=argument;opaque(subject);checksum+=std::visit([](const auto& x){return x.value+std::decay_t<decltype(x)>::index;},subject);}
 }
 auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
 if(op=="construct"||op=="copy")checksum=std::visit([](const auto& x){return x.value+std::decay_t<decltype(x)>::index;},held);
 return {static_cast<std::uint64_t>(ns),0,static_cast<std::uint64_t>(rounds),checksum,state};
}
template<class F>void measure(const std::string& representation,int variants,const std::string& op,int rounds,std::size_t object_bytes,F batch){
 std::uint64_t batches=1;Sample total;
 for(;;){
  total={};total.state=17;hua::ManagedHeap heap(65536);hua::HeapScope scope(heap);
  for(std::uint64_t i=0;i<batches;++i){auto part=batch();total.ns+=part.ns;total.allocations+=part.allocations;total.checks+=part.checks;total.checksum+=part.checksum;total.state=part.state;}
  if(total.ns>=20000000||batches>=131072)break;
  batches*=2;
 }
 std::cout<<"{\"representation\":\""<<representation<<"\",\"variants\":"<<variants<<",\"operation\":\""<<op<<"\",\"rounds\":"<<rounds<<",\"batches\":"<<batches<<",\"operations\":"<<total.checks<<",\"nanoseconds\":"<<total.ns<<",\"ns_per_operation\":"<<double(total.ns)/total.checks<<",\"managed_allocations\":"<<total.allocations<<",\"checksum\":"<<total.checksum<<",\"state\":"<<total.state<<",\"object_bytes\":"<<object_bytes<<"}\n";
}
int preflight(){
 hua::ManagedHeap heap(65536);hua::HeapScope scope(heap);Layout layout(59);int checks=0;
 auto require=[&](bool ok){++checks;if(!ok)throw std::runtime_error("native semantic preflight "+std::to_string(checks));};
 std::vector<std::pair<hua::Value,std::string>> values={{hua::Value(I(42)),"int"},{hua::Value(1.25),"float"},{hua::Value(true),"bool"},{hua::Value(std::string("text\r\nUTF-8:\xc3\xa9")),"string"},{hua::Value{},"int?"},{hua::Value{},"void"}};
 for(const auto& [payload,type]:values){
  auto full=make_value(layout,58,payload,false,type),sparse=make_value(layout,58,payload,true,type);
  require(std::get<hua::StructValue>(full.data).data->fields.size()==60);
  require(std::get<hua::StructValue>(sparse.data).data->fields.size()==2);
  require(hua::show(full)==hua::show(sparse));
  require(!std::get<hua::ResultValue>(hua::json_encode(full).data).ok&&!std::get<hua::ResultValue>(hua::json_encode(sparse).data).ok);
  auto copied=hua::copy_value(hua::read_only(sparse));require(hua::show(copied)==hua::show(sparse));
  const auto key="$V58#0#"+type;
  if(type=="void")require(hua::show(copied)=="Item.V58()");
  else{std::get<hua::StructValue>(copied.data).data->fields.at(key)=hua::Value(I(99));require(hua::show(copied)!=hua::show(sparse));}
 }
 auto data=hua::managed<hua::ListData>();data->element_type="int";data->values.emplace_back(I(1));
 for(bool sparse:{false,true}){
  auto source=make_value(layout,0,hua::Value(hua::ListValue{data,true}),sparse,"List<int>");
  auto copy=hua::copy_value(hua::read_only(source));const auto& item=std::get<hua::StructValue>(copy.data).data->fields.at("$V0#0#List<int>");
  require(!std::get<hua::ListValue>(item.data).writable);require(std::get<hua::ListValue>(item.data).data==data);
 }
 std::cout<<"{\"status\":\"passed\",\"checks\":"<<checks<<"}\n";return 0;
}
template<std::size_t N>int run(int rounds){
 Layout layout(N);
 for(const std::string op:{"construct","copy","match-first","match-last"}){
  for(bool sparse:{false,true})measure(sparse?"value-sparse":"value-full",N,op,rounds,sizeof(hua::Value),[&]{return value_batch(layout,sparse,op,rounds);});
  measure("std-variant",N,op,rounds,sizeof(Variant<N>),[&]{return variant_batch<N>(op,rounds);});
 }
 return 0;
}
int main(int argc,char** argv){try{
 if(argc==2&&std::string(argv[1])=="--preflight")return preflight();
 if(argc!=3)return 2;int n=std::stoi(argv[1]),rounds=std::stoi(argv[2]);if(rounds<1||rounds>8192)return 2;
 switch(n){case 1:return run<1>(rounds);case 8:return run<8>(rounds);case 32:return run<32>(rounds);case 59:return run<59>(rounds);default:return 2;}
 }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
