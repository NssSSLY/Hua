#include "hua/value.hpp"
#include "support/native_allocations.hpp"
#include <bit>
#include <cmath>
#include <iostream>
#include <thread>
#include <stdexcept>
using namespace hua;
namespace {
unsigned checks=0;
void check(bool yes,const char* why){++checks;if(!yes)throw std::runtime_error(why);}
Value record(const std::string& name,std::initializer_list<std::pair<std::string,Value>> fields){
 std::vector<std::string> names;std::map<std::string,std::string> types;
 for(const auto& [key,v]:fields){names.push_back(key);types[key]=value_type(v);}
 auto d=managed<StructData>();d->name=name;d->fields=RecordFields(std::make_shared<const RecordLayout>(names,name,types));
 for(const auto& [key,v]:fields)d->fields.emplace(key,v);return Value(StructValue{d,true});
}
std::int64_t field(const Value& v,const std::string& key){if(auto p=std::get_if<PlainRecord>(&v.data))return as_int(*plain_record_field(*p,key),{});return as_int(std::get<StructValue>(v.data).data->fields.at(key),{});}
void values(){
 ManagedHeap heap(65536);HeapScope scope(heap);
 auto id=record("Id",{{"pool",Value(std::int64_t(3))},{"index",Value(std::int64_t(7))}});
 auto copied=copy_value(id);check(std::holds_alternative<PlainRecord>(copied.data),"typed identifier was not inline");check(show(copied)==show(id),"inline display differs");
 auto before=heap.allocated();auto alloc=native_allocation::snapshot();
 for(unsigned i=0;i<1000;++i){auto next=copy_value(id);if(field(next,"index")!=7)throw std::runtime_error("inline copy content");}
 check(heap.allocated()==before,"pure copy allocated managed objects");check(native_allocation::snapshot().calls==alloc.calls,"warmed pure copy allocated native storage");
 auto parent=record("Span",{{"source",id},{"start",Value(std::int64_t(1))},{"end",Value(std::int64_t(8))},{"line",Value(std::int64_t(2))},{"column",Value(std::int64_t(4))}});
 auto span=copy_value(parent);check(std::holds_alternative<PlainRecord>(span.data),"nested pure span was not inline");check(std::get<PlainRecord>(span.data).layout->slots==6,"nested span width");
 std::get<StructValue>(id.data).data->fields.at("index")=Value(std::int64_t(99));
 check(field(*plain_record_field(std::get<PlainRecord>(span.data),"source"),"index")==7,"nested snapshot aliased original");
 auto ro=read_only(span);check(!std::get<PlainRecord>(ro.data).writable,"readonly root upgraded");auto child=*plain_record_field(std::get<PlainRecord>(ro.data),"source");check(!std::get<PlainRecord>(child.data).writable,"readonly child upgraded");
 auto cloned=copy_value(ro,true);check(std::get<PlainRecord>(cloned.data).writable,"clone pure surface not writable");
 auto boxed=cloned;materialize_record(boxed);check(show(boxed)==show(cloned),"materialization differs");
 auto numeric=record("Numeric",{{"a",Value(NumericValue{"u8",std::uint64_t(255)})},{"b",Value(NumericValue{"f32",-0.0})}});
 auto packed=std::get<PlainRecord>(copy_value(numeric).data);auto a=*plain_record_field(packed,"a"),b=*plain_record_field(packed,"b");
 check(value_type(a)=="u8"&&as_int(a,{})==255,"exact numeric identity");check(value_type(b)=="f32"&&std::signbit(std::get<double>(std::get<NumericValue>(b.data).number)),"negative zero changed");
 auto empty=record("Empty",{});check(show(copy_value(empty))=="Empty{}","empty pure record");
 for(unsigned point=0;point<8;++point){auto value=span;auto prior=heap.live();bool failed=false;native_allocation::fail_after(point);try{materialize_record(value);}catch(const std::bad_alloc&){failed=true;}native_allocation::clear_failure();check(show(value)==show(span),"failed materialization changed source");if(failed)check(heap.live()==prior,"failed materialization leaked managed value");}
 auto wide=managed<StructData>();wide->name="Wide";std::vector<std::string> names;std::map<std::string,std::string> types;
 for(unsigned i=0;i<9;++i){names.push_back("v"+std::to_string(i));types[names.back()]="int";}wide->fields=RecordFields(std::make_shared<const RecordLayout>(names,"Wide",types));for(const auto& name:names)wide->fields.emplace(name,Value(std::int64_t(1)));
 check(std::holds_alternative<StructValue>(copy_value(Value(StructValue{wide,true})).data),"wide fallback not preserved");
 auto complex=record("Text",{{"name",Value(std::string("value"))}});check(std::holds_alternative<StructValue>(copy_value(complex).data),"reference payload compacted unsafely");
}
void aliases(){
 ManagedHeap heap(65536);HeapScope scope(heap);
 auto value=record("Point",{{"x",Value(std::int64_t(1))}});auto storage=managed<std::vector<Value>>();storage->push_back(copy_value(value));
 SliceValue ro{storage,0,1,false,"Point"},rw{storage,0,1,true,"Point"};auto first=sequence_read(ro,0);auto second=sequence_read(rw,0);
 std::get<StructValue>(second.data).data->fields.at("x")=Value(std::int64_t(9));check(field(first,"x")==9,"readonly element view changed to snapshot");
 auto map=managed<MapData>();map->key_type="string";map->item_type="Point";map->entries.emplace(std::string("p"),copy_value(value));
 auto readonly=index_value(Value(MapValue{map,false}),Value(std::string("p")),{});auto writable=index_value(Value(MapValue{map,true}),Value(std::string("p")),{});
 std::get<StructValue>(writable.data).data->fields.at("x")=Value(std::int64_t(8));check(field(readonly,"x")==8,"readonly map value lost original identity");
}
void metadata_lifetime(){
 std::weak_ptr<const RecordLayout> weak;
 {auto value=record("Point",{{"x",Value(std::int64_t(1))}});weak=std::get<StructValue>(value.data).data->fields.retain_layout();auto plain=copy_value(value);value={};check(!weak.expired(),"inline record lost layout");materialize_record(plain);check(field(plain,"x")==1,"layout retained after source release");}
 check(weak.expired(),"plain cache created strong metadata cycle");
 auto source=record("Concurrent",{{"x",Value(std::int64_t(3))}});std::vector<std::jthread> threads;std::atomic<bool> okay{true};
 for(unsigned n=0;n<4;++n)threads.emplace_back([&]{try{for(unsigned i=0;i<500;++i)if(field(copy_value(source),"x")!=3)okay=false;}catch(...){okay=false;}});
 threads.clear();check(okay,"concurrent immutable layout cache");
 auto storage=std::make_shared<std::vector<Value>>();storage->push_back(copy_value(source));
 Value shared(SliceValue{storage,0,1,false,"Concurrent"});
 for(unsigned n=0;n<4;++n)threads.emplace_back([&]{try{for(unsigned i=0;i<500;++i){auto copied=copy_value(shared,true);auto slice=std::get<SliceValue>(copied.data);auto item=sequence_read(slice,0);if(field(item,"x")!=3)okay=false;}}catch(...){okay=false;}});
 threads.clear();check(okay,"concurrent terminal result copies differ");
 check(std::holds_alternative<PlainRecord>(storage->at(0).data),"snapshot scan mutated shared terminal result storage");

}
}
int main(){try{values();aliases();metadata_lifetime();std::cout<<"{\"status\":\"passed\",\"checks\":"<<checks<<",\"value_size\":"<<sizeof(Value)<<",\"pure_record_size\":"<<sizeof(PlainRecord)<<",\"pure_copy_managed_allocations\":0,\"warmed_pure_copy_native_allocations\":0}\n";return 0;}catch(const std::exception& e){native_allocation::clear_failure();std::cerr<<e.what()<<'\n';return 1;}}
