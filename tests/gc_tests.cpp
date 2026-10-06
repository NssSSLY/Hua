#include "hua/value.hpp"
#include "hua/gc.hpp"
#include <iostream>
#include <stdexcept>
int main(){using namespace hua;ManagedHeap heap;HeapScope scope(heap);
 auto a=managed<ListData>();auto b=managed<ListData>();
 a->values.emplace_back(ListValue{b,true});b->values.emplace_back(ListValue{a,true});
 auto address=a.get();if(heap.collect()!=0||a.get()!=address||heap.live()!=2)throw std::runtime_error("root/address stability");
 std::weak_ptr<ListData> weak=a;a.reset();b.reset();if(heap.collect()!=2||!weak.expired()||heap.live())throw std::runtime_error("cycle reclamation");
 auto map=managed<MapData>();auto storage=managed<std::vector<Value>>();map->entries.emplace(std::string("x"),Value(SliceValue{storage,0,1,true,""}));storage->emplace_back(MapValue{map,true});
 map.reset();storage.reset();if(heap.collect()!=2||heap.live())throw std::runtime_error("mixed cycle");
 auto result=managed<Value>();auto list=managed<ListData>();*result=Value(ListValue{list,true});list->values.emplace_back(ResultValue{true,result,"", "",true,false});
 result.reset();list.reset();if(heap.collect()!=2||heap.live())throw std::runtime_error("Result payload cycle");
 auto backing=managed<std::vector<Value>>();backing->emplace_back(std::int64_t{7});Value view(SliceValue{backing,0,1,false,"int"});backing.reset();if(heap.collect()!=0||heap.live()!=1)throw std::runtime_error("slice root");view={};heap.collect();if(heap.live())throw std::runtime_error("slice release");
 struct Capture {std::vector<Value> values;};
 auto retained=managed<ListData>();std::weak_ptr<ListData> ordinary=retained;retained.reset();if(!ordinary.expired())throw std::runtime_error("acyclic release must be immediate");
 std::cout<<"GC roots, stable addresses, immediate release and mixed cycles OK\n";
}
