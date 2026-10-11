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
 std::cout<<checks<<" consumed value ownership/capability/layout/GC checks passed\n";
}
