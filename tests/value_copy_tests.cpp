#include "hua/value.hpp"
#include <iostream>
#include <stdexcept>
using namespace hua;
int main(){
 ManagedHeap heap(65536);HeapScope scope(heap);int checks=0;
 auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("consumed value snapshot/capability regression");};
 auto record=[](int n){auto d=managed<StructData>();d->name="Box";d->fields.emplace("n",Value(std::int64_t(n)));return Value(StructValue{std::move(d),true});};
 auto number=[](const Value& v){return std::get<std::int64_t>(std::get<StructValue>(v.data).data->fields.at("n").data);};
 // An owned temporary can retain storage; a shared record must remain an independent value.
 Value owned=record(1);auto address=std::get<StructValue>(owned.data).data.get();auto moved=copy_value(std::move(owned));
 check(std::get<StructValue>(moved.data).data.get()==address);check(number(moved)==1);
 Value original=record(2),alias=original;auto copied=copy_value(std::move(alias));
 std::get<StructValue>(copied.data).data->fields.at("n")=Value(std::int64_t(9));check(number(original)==2);check(number(copied)==9);
 // Owning the outer struct does not grant ownership of nested shared records.
 Value child=record(3);auto outer=managed<StructData>();outer->name="Outer";outer->fields.emplace("child",child);
 Value wrapper(StructValue{std::move(outer),true});auto separate=copy_value(std::move(wrapper));
 auto& nested=std::get<StructValue>(separate.data).data->fields.at("child");
 std::get<StructValue>(nested.data).data->fields.at("n")=Value(std::int64_t(7));check(number(child)==3);
 // A readonly record copy opens its scalar surface and preserves readonly shared descendants.
 auto list=managed<ListData>();list->element_type="int";list->values.emplace_back(std::int64_t(1));
 auto data=managed<StructData>();data->name="WithList";data->fields.emplace("items",Value(ListValue{list,true}));
 Value ro(StructValue{std::move(data),false});auto surface=copy_value(std::move(ro));auto s=std::get<StructValue>(surface.data);
 check(s.writable);auto view=std::get<ListValue>(s.data->fields.at("items").data);check(!view.writable);check(view.data==list);
 // Result payload reuse never exposes an alias's record or upgrades a borrowed List.
 Value result(ResultValue{true,managed<Value>(record(4)),"Box","string",true,false});Value result_alias=result;
 auto box=copy_value(std::move(result_alias));auto payload=result_payload(box,true,{});
 std::get<StructValue>(payload.data).data->fields.at("n")=Value(std::int64_t(8));check(number(result_payload(result,true,{}))==4);
 Value readonly_result(ResultValue{true,managed<Value>(Value(ListValue{list,true})),"List<int>","string",false,false});
 auto propagated=copy_value(std::move(readonly_result));check(!std::get<ResultValue>(propagated.data).writable);
 check(!std::get<ListValue>(result_payload(propagated,true,{}).data).writable);
 // Multi-value positions keep value copies, contextual metadata and fixed arrays keep separate layout.
 Value multi=make_multi({child,Value(std::int64_t(5))},{false,true});auto positions=copy_value(std::move(multi));
 auto values=unpack_multi(positions,2,{});check(values.size()==2);check(std::get<MultiValue>(positions.data).data->contextual.at(1));
 std::get<StructValue>(values[0].data).data->fields.at("n")=Value(std::int64_t(6));check(number(child)==3);
 auto storage=managed<std::vector<Value>>();storage->emplace_back(std::int64_t(11));
 Value array(fixed_array(SliceValue{storage,0,1,true,"int"},"int",{},false));auto before=std::get<SliceValue>(array.data).packed.get();
 auto fixed=copy_value(std::move(array));check(std::get<SliceValue>(fixed.data).packed.get()!=before);
 // Deep clone, void errors and cycle/depth rejection retain the original contracts.
 auto clone=copy_value(read_only(surface),true);check(std::get<ListValue>(std::get<StructValue>(clone.data).data->fields.at("items").data).writable);
 Value success(ResultValue{true,nullptr,"void","string",false,false});check(!std::get<ResultValue>(copy_value(std::move(success)).data).payload);
 try{copy_value(Value(VoidValue{}));throw std::runtime_error("void accepted");}catch(const Diagnostic& d){check(d.code()=="E4003");}
 auto cycle=managed<StructData>();cycle->name="Cycle";cycle->fields.emplace("self",Value(StructValue{cycle,true}));Value cyclic(StructValue{cycle,true});
 try{copy_value(std::move(cyclic));throw std::runtime_error("cycle accepted");}catch(const Diagnostic& d){check(d.code()=="E4099");}
 cycle->fields.clear();heap.collect();check(number(original)==2);check(number(child)==3);
 // Result constraints consume only owned boxes and preserve shared payload snapshots.
 auto wrap=[](Value value,std::string success="Box",bool writable=true,bool contextual=false){return Value(ResultValue{true,managed<Value>(std::move(value)),std::move(success),"string",writable,contextual});};
 auto constrained_source=wrap(record(21));auto constrained=enforce_type(constrained_source,"Result<Box,string>",{});
 auto& normalized=std::get<ResultValue>(constrained.data);auto& preserved=std::get<ResultValue>(constrained_source.data);
 check(normalized.payload!=preserved.payload);check(normalized.success_type=="Box"&&normalized.error_type=="string");
 std::get<StructValue>(normalized.payload->data).data->fields.at("n")=Value(std::int64_t(22));check(number(*preserved.payload)==21);
 auto owned_result=enforce_type(wrap(record(23)),"Result<Box,string>",{});check(number(*std::get<ResultValue>(owned_result.data).payload)==23);
 // A unique box can still contain a shared root or shared descendants.
 auto shared_record=record(24);auto own_box=enforce_type(wrap(shared_record),"Result<Box,string>",{});
 std::get<StructValue>(std::get<ResultValue>(own_box.data).payload->data).data->fields.at("n")=Value(std::int64_t(25));check(number(shared_record)==24);
 auto shared_child=record(26);auto parent=managed<StructData>();parent->name="Outer";parent->fields.emplace("child",shared_child);
 auto constrained_parent=enforce_type(wrap(Value(StructValue{std::move(parent),true}),"Outer"),"Result<Outer,string>",{});
 auto& parent_value=std::get<StructValue>(std::get<ResultValue>(constrained_parent.data).payload->data);
 std::get<StructValue>(parent_value.data->fields.at("child").data).data->fields.at("n")=Value(std::int64_t(27));check(number(shared_child)==26);
 for(int layers:{1,2,4}){
  auto nested_source=wrap(record(28));std::string nested_type="Result<Box,string>";
  for(int level=1;level<layers;++level){nested_source=wrap(std::move(nested_source),nested_type);nested_type="Result<"+nested_type+",string>";}
  auto nested_copy=enforce_type(nested_source,nested_type,{});auto input_value=&nested_source;auto output_value=&nested_copy;
  for(int level=0;level<layers;++level){auto& a=std::get<ResultValue>(input_value->data);auto& b=std::get<ResultValue>(output_value->data);check(a.payload!=b.payload);input_value=a.payload.get();output_value=b.payload.get();}
  std::get<StructValue>(output_value->data).data->fields.at("n")=Value(std::int64_t(29));check(number(*input_value)==28);
 }
 auto ro_list=wrap(Value(ListValue{list,true}),"List<int>",false);
 auto constrained_ro=enforce_type(std::move(ro_list),"Result<List<int>,string>",{});
 auto& ro_payload=std::get<ResultValue>(constrained_ro.data);check(!ro_payload.writable);
 auto list_view=std::get<ListValue>(ro_payload.payload->data);check(!list_view.writable&&list_view.data==list);
 auto mutable_list=enforce_type(wrap(Value(ListValue{list,true}),"List<int>"),"Result<List<int>,string>",{});
 check(std::get<ListValue>(std::get<ResultValue>(mutable_list.data).payload->data).writable);
 // Contextual and widening conversions still execute; equality of type names is no proof.
 auto converted=enforce_type(wrap(Value(std::int64_t(255)),"int",true,true),"Result<u8,string>",{});
 auto& converted_result=std::get<ResultValue>(converted.data);check(value_type(*converted_result.payload)=="u8"&&!converted_result.contextual);
 check(as_int(*converted_result.payload,{})==255);
 auto exact_float=enforce_type(wrap(Value(std::int64_t(12)),"int",true,true),"Result<float,string>",{});
 check(std::get<double>(std::get<ResultValue>(exact_float.data).payload->data)==12.0);
 auto rejected=[&](Value value,const std::string& type,const std::string& code){try{enforce_type(std::move(value),type,{});throw std::runtime_error("Result constraint accepted invalid input");}catch(const Diagnostic& d){check(d.code()==code);}};
 rejected(wrap(Value(std::int64_t(256)),"int",true,true),"Result<u8,string>","E4002");
 rejected(wrap(Value(std::int64_t(9007199254740993LL)),"int",true,true),"Result<float,string>","E4002");
 rejected(wrap(record(30)),"Result<Box,bool>","E4003");
 rejected(Value(ResultValue{true,nullptr,"Box","string",true,false}),"Result<Box,string>","E4003");
 rejected(wrap(Value{},"nil"),"Result<void,string>","E4003");
 auto void_result=enforce_type(Value(ResultValue{true,nullptr,"void","string",false,false}),"Result<void,string>",{});
 check(!std::get<ResultValue>(void_result.data).payload&&!std::get<ResultValue>(void_result.data).writable);
 auto nullable=enforce_type(wrap(Value{},"nil"),"Result<Box?,string>",{});check(std::get<ResultValue>(nullable.data).payload->data.index()==0);
 auto failure=Value(ResultValue{false,managed<Value>(Value(std::string("failure"))),"Box","string",false,false});
 auto failed=enforce_type(failure,"Result<Box,string>",{});auto& failed_result=std::get<ResultValue>(failed.data);
 check(!failed_result.ok&&!failed_result.writable&&show(*failed_result.payload)=="failure");
 // Fixed-array value snapshots and errors leave a shared source usable.
 auto array_values=managed<std::vector<Value>>();array_values->emplace_back(std::int64_t(31));
 auto array_source=wrap(Value(fixed_array(SliceValue{array_values,0,1,true,"int"},"int",{},false)),"[1]int");
 auto array_result=enforce_type(array_source,"Result<[1]int,string>",{});
 check(std::get<SliceValue>(std::get<ResultValue>(array_source.data).payload->data).packed!=std::get<SliceValue>(std::get<ResultValue>(array_result.data).payload->data).packed);
 auto shared_bad=wrap(Value(std::int64_t(256)),"int",true,true);rejected(shared_bad,"Result<u8,string>","E4002");
 check(as_int(*std::get<ResultValue>(shared_bad.data).payload,{})==256);
 heap.collect();check(number(*preserved.payload)==21);check(number(shared_child)==26);
 std::cout<<checks<<" consumed value ownership/capability/layout/GC checks passed\n";
}
