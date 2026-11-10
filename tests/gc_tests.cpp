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
 for(auto interval:{8192u,65536u}){
   ManagedHeap compiler(interval);HeapScope compiler_scope(compiler);
   if(compiler.collection_interval()!=interval)throw std::runtime_error("compiler collection policy");
   auto left=managed<ListData>(),right=managed<ListData>();
   left->values.emplace_back(ListValue{right,true});right->values.emplace_back(ListValue{left,true});
   std::weak_ptr<ListData> cycle=left;left.reset();right.reset();
   for(unsigned i=0;i<interval;++i)(void)managed<std::vector<Value>>();
   if(!cycle.expired())throw std::runtime_error("bounded automatic compiler cycle collection");
   compiler.collect();if(compiler.live())throw std::runtime_error("compiler heap finish");
 }
 {
   // A slice body shared by a live Value and a Value inside an unreachable cycle: the
   // collection must not treat the slice storage as unreachable and clear it.
   ManagedHeap shared_heap;HeapScope shared_scope(shared_heap);
   auto storage=managed<std::vector<Value>>();storage->emplace_back(std::int64_t{1});storage->emplace_back(std::int64_t{2});
   Value live(SliceValue{storage,0,2,true,"int"});storage.reset();
   auto cycle=managed<ListData>();cycle->values.push_back(live);cycle->values.emplace_back(ListValue{cycle,true});cycle.reset();
   shared_heap.collect();
   const auto& slice=hua::cget<SliceValue>(live.data);
   if(!slice.storage||slice.storage->size()!=2||!hua::holds_alternative<std::int64_t>((*slice.storage)[1].data))throw std::runtime_error("shared slice body cleared by collection");
 }
 std::cout<<"GC roots, stable addresses, immediate release and mixed cycles OK\n";
}
