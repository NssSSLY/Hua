#include "hua/runtime.hpp"
#include "hua/tasks.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace hua;
using I=std::int64_t;
int main(){
    int checks=0;std::ostringstream output;
    auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("Set/bit native contract regression");};
    auto fails=[&](const char* code,auto body){try{body();}catch(const Diagnostic& d){check(d.code()==code);return;}throw std::runtime_error("expected diagnostic absent");};
    auto call=[&](const char* name,std::vector<Value> args,RuntimeContext* c=nullptr){return invoke_standard(*standard_function(name),args,{},output,c);};
    for(const char* name:{"$std$math$popcount","$std$math$leading_zeros","$std$math$trailing_zeros"}){
        fails("E4003",[&]{call(name,{Value(1.0)});});
        fails("E4003",[&]{call(name,{convert_numeric(Value(I{1}),"i32",{},true)});});
        fails("E4003",[&]{call(name,{});});
    }
    for(const char* name:{"$std$math$rotate_left","$std$math$rotate_right"}){
        fails("E4003",[&]{call(name,{Value(I{1}),Value(-1.0)});});
        fails("E4003",[&]{call(name,{Value(I{1}),Value(I{64})});});
        fails("E4003",[&]{call(name,{Value(I{1}),Value(I{-1})});});
    }
    auto set=[](I count,I start,I step){auto data=managed<MapData>();data->key_type="int";data->item_type="bool";
        for(I i=0;i<count;++i)data->entries.emplace_hint(data->entries.end(),MapKey(start+i*step),Value(false));
        return Value(MapValue{std::move(data),false});};
    auto a=set(1100,0,1),empty=set(0,0,1);
    RuntimeContext context;context.cancellation=std::make_shared<Cancellation>();context.cancellation->requested=true;
    // Call the collection implementation directly, so cancellation must be checked
    // inside the traversal rather than merely at the invoke_builtin boundary.
    fails("E4101",[&]{invoke_collection_standard(*standard_function("$std$sets$symmetric_difference"),{a,empty},{},&context);});
    check(std::get<MapValue>(a.data).data->entries.size()==1100);
    fails("E4003",[&]{call("$std$sets$symmetric_difference",{Value(I{1}),empty});});
    auto strings=managed<MapData>();strings->key_type="string";strings->item_type="bool";
    fails("E4003",[&]{call("$std$sets$symmetric_difference",{a,Value(MapValue{strings,false})});});
    // Two valid maps whose disjoint result exceeds the existing one-million limit.
    // Native setup avoids a million interpreted inserts on every execution path.
    auto even=set(500001,0,2),odd=set(500000,1,2);
    fails("E4099",[&]{call("$std$sets$symmetric_difference",{even,odd});});
    check(std::get<MapValue>(even.data).data->entries.size()==500001);
    check(std::get<MapValue>(odd.data).data->entries.size()==500000);
    // The exact limit is valid; only one more key must fail.
    auto& odd_entries=std::get<MapValue>(odd.data).data->entries;
    odd_entries.erase(std::prev(odd_entries.end()));
    auto exact=call("$std$sets$symmetric_difference",{even,odd});
    check(std::get<MapValue>(exact.data).data->entries.size()==1000000);
    check(std::get<MapValue>(exact.data).writable);
    std::cout<<checks<<" Set/bit native type/capacity/cancellation checks passed\n";
}