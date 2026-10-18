#include "hua/value.hpp"
#include "hua/vm.hpp"
#include <sstream>
#include "support/native_allocations.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace hua;
namespace {
unsigned checks{};
void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
std::shared_ptr<StructData> record(std::shared_ptr<const RecordLayout> shape={}){
    auto data=managed<StructData>();data->name="Record";if(shape)data->fields=RecordFields(std::move(shape));return data;
}
std::int64_t integer(const Value& v){return std::get<std::int64_t>(v.data);}
void positions_and_order(){
    // Independent ordered-tree oracle, all 120 insertion orders, and initialized nil.
    auto shape=std::make_shared<const RecordLayout>(std::vector<std::string>{"z","b","n","a","m"});
    std::vector<std::string> order=shape->names;
    do {
        auto flat=record(shape),tree=record();Value* first=nullptr;std::string first_name;
        for(std::size_t i=0;i<order.size();++i){
            const auto& name=order[i];auto value=name=="n"?Value{}:Value(std::int64_t(name[0]));
            flat->fields.emplace(name,value);tree->fields.emplace(name,value);
            if(!first){first=&flat->fields.at(name);first_name=name;}
            check(first==&flat->fields.at(first_name),"slot moved during later initializers");
            check(show(Value(StructValue{flat,true}))==show(Value(StructValue{tree,true})),"partial field order differs");
            check(flat->fields.size()==i+1,"absent fields were exposed as nil");
            const RecordFields& constant=flat->fields;
            check(constant.find(name)!=constant.end(),"const lookup failed");
        }
        auto result=flat->fields.emplace("a",Value(std::int64_t(999)));
        check(!result.second&&integer(result.first->second)==97,"duplicate ordinary initializer changed first value");
        check(flat->fields.contains("n")&&!flat->fields.contains("unknown"),"nil/absent distinction lost");
        auto copy=std::get<StructValue>(copy_value(Value(StructValue{flat,true})).data);
        check(copy.data->fields.layout()==shape.get(),"copy lost shared layout");
        copy.data->fields.at("a")=Value(std::int64_t(17));check(integer(flat->fields.at("a"))==97,"scalar alias leaked");
        flat->fields.clear();check(flat->fields.empty(),"clear retained fields");
        flat->fields.emplace("z",Value{});check(flat->fields.size()==1&&flat->fields.contains("z"),"cleared field table cannot reinitialize");
    }while(std::next_permutation(order.begin(),order.end()));
    auto empty=record(std::make_shared<const RecordLayout>(std::vector<std::string>{}));
    check(empty->fields.begin()==empty->fields.end()&&empty->fields.empty(),"empty layout iteration");
    check(show(Value(StructValue{empty,true}))=="Record{}","empty record display");
}
void copies_and_lifetime(){
    auto shape=std::make_shared<const RecordLayout>(std::vector<std::string>{"child","items"});
    std::weak_ptr<const RecordLayout> weak=shape;auto parent=record(shape);shape.reset();
    auto child=record(std::make_shared<const RecordLayout>(std::vector<std::string>{"n"}));child->fields.emplace("n",Value(std::int64_t(3)));
    auto list=managed<ListData>();list->element_type="int";list->values.emplace_back(std::int64_t(5));
    parent->fields.emplace("child",Value(StructValue{child,true}));parent->fields.emplace("items",Value(ListValue{list,true}));
    auto copied=std::get<StructValue>(copy_value(Value(StructValue{parent,false})).data);
    auto nested=std::get<StructValue>(copied.data->fields.at("child").data);nested.data->fields.at("n")=Value(std::int64_t(8));
    check(integer(child->fields.at("n"))==3,"nested record alias leaked");
    auto view=std::get<ListValue>(copied.data->fields.at("items").data);check(view.data==list&&!view.writable,"readonly container was upgraded");
    auto cloned=std::get<StructValue>(copy_value(Value(StructValue{parent,false}),true).data);
    auto owned=std::get<ListValue>(cloned.data->fields.at("items").data);check(owned.data!=list&&owned.writable,"deep clone container contract");
    parent.reset();check(!weak.expired(),"layout died with source owner");copied.data.reset();cloned.data.reset();check(weak.expired(),"layout outlived final record");
}
void edges_and_cycles(){
    auto shape=std::make_shared<const RecordLayout>(std::vector<std::string>{"nil","list","self","slice"});
    auto flat=record(shape),tree=record();auto list=managed<ListData>();auto sequence=managed<std::vector<Value>>();
    for(auto* object:{flat.get(),tree.get()}){
        object->fields.emplace("nil",Value{});object->fields.emplace("list",Value(ListValue{list,true}));
        object->fields.emplace("slice",Value(SliceValue{sequence,0,0,true,"int"}));object->fields.emplace("self",Value(StructValue{flat,true}));
    }
    std::vector<const void*> a,b;heap_edges(*flat,[&](const void* p){a.push_back(p);});heap_edges(*tree,[&](const void* p){b.push_back(p);});
    check(a==b,"dynamic GC edge sequence changed");
    std::weak_ptr<StructData> weak=flat;tree.reset();flat.reset();list.reset();sequence.reset();managed_heap().collect();
    check(weak.expired(),"fixed-slot cycle survived collection");
}
void allocation_mechanism(){
    for(unsigned width:{2,8,32,128}){
        std::vector<std::string> names;for(unsigned i=0;i<width;++i)names.push_back("long_field_name_for_record_"+std::to_string(i));
        auto flat=record(std::make_shared<const RecordLayout>(names)),tree=record();
        for(const auto& name:names){flat->fields.emplace(name,Value(std::int64_t(1)));tree->fields.emplace(name,Value(std::int64_t(1)));}
        // Warm heap bookkeeping before measuring requested C++ allocations, not managed-object counts.
        {auto x=copy_value(Value(StructValue{flat,true}));auto y=copy_value(Value(StructValue{tree,true}));}
        auto measure=[](const std::shared_ptr<StructData>& source){
            auto before=native_allocation::snapshot();auto copy=copy_value(Value(StructValue{source,true}));auto after=native_allocation::snapshot();
            return std::pair{after.calls-before.calls,after.requested-before.requested};
        };
        auto a=measure(flat),b=measure(tree);check(a.first<b.first&&a.second<b.second,"fixed slots did not remove per-field allocations");
        std::cout<<"allocation width="<<width<<" flat_calls="<<a.first<<" tree_calls="<<b.first<<" flat_bytes="<<a.second<<" tree_bytes="<<b.second<<'\n';
    }
}
}
namespace hua {
struct VmSlotTestAccess {
    static unsigned record_routes(){
        SemanticModel model;std::ostringstream output;VirtualMachine vm(model,output);
        VirtualMachine::FieldAccess cache,missing,partial;
        unsigned checks=0;auto verify=[&](bool ok){++checks;if(!ok)throw std::runtime_error("record field route/lifetime mismatch");};
        auto shape=std::make_shared<const RecordLayout>(std::vector<std::string>{"x","z"});
        auto a=record(shape),b=record(shape);a->fields.emplace("x",Value(std::int64_t(1)));b->fields.emplace("x",Value(std::int64_t(2)));
        verify(vm.field(cache,{a,true},"x")==&a->fields.at("x"));
        verify(vm.field(cache,{b,true},"x")==&b->fields.at("x"));
        verify(vm.field(partial,{a,true},"z")==nullptr);a->fields.emplace("z",Value{});
        verify(vm.field(partial,{a,true},"z")==&a->fields.at("z"));
        verify(vm.field(missing,{a,true},"unknown")==nullptr&&vm.field(missing,{b,true},"unknown")==nullptr);
        auto other=record(std::make_shared<const RecordLayout>(std::vector<std::string>{"a","x"}));other->fields.emplace("x",Value(std::int64_t(3)));
        verify(vm.field(cache,{other,true},"x")==&other->fields.at("x"));
        auto dynamic=record();dynamic->fields.emplace("x",Value(std::int64_t(4)));
        verify(vm.field(cache,{dynamic,true},"x")==&dynamic->fields.at("x"));
        verify(vm.field(cache,{b,true},"x")==&b->fields.at("x"));
        // A child VM/Task layout need not be in the caller's layout registry.
        // Retaining it in the access cache prevents address reuse from validating a stale index.
        std::weak_ptr<const RecordLayout> weak=shape;a.reset();b.reset();shape.reset();
        verify(!weak.expired());cache={};missing={};partial={};verify(weak.expired());return checks;
    }
};
}
int main(){
    ManagedHeap heap(65536);HeapScope scope(heap);
    positions_and_order();copies_and_lifetime();edges_and_cycles();allocation_mechanism();checks+=VmSlotTestAccess::record_routes();heap.collect();
    check(heap.live()==0,"record fixture leaked managed values");std::cout<<checks<<" record layout checks passed\n";
}
