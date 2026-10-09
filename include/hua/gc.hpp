#pragma once
#include <functional>
#include <memory>
#include <vector>
#include <cstddef>
namespace hua {
using HeapVisitor=std::function<void(const void*)>;
struct Value;struct StructData;struct MapData;struct ListData;struct MultiData;struct ErrorData;
void heap_edges(const Value&,const HeapVisitor&);
void heap_edges(const StructData&,const HeapVisitor&);
void heap_edges(const MapData&,const HeapVisitor&);
void heap_edges(const ListData&,const HeapVisitor&);
void heap_edges(const MultiData&,const HeapVisitor&);
void heap_edges(const ErrorData&,const HeapVisitor&);
void heap_edges(const std::vector<Value>&,const HeapVisitor&);
class ManagedHeap {
    struct Record {std::weak_ptr<const void> pointer;std::function<void(const void*,const HeapVisitor&)> edges;std::function<void(const void*)> clear;};
    std::vector<Record> records_;
    std::size_t allocated_{},collected_{};
public:
    template<class T> void track(const std::shared_ptr<T>& object){
        Record record;record.pointer=object;
        if constexpr(requires(const T& x,const HeapVisitor& v){heap_edges(x,v);})
            record.edges=[](const void* p,const HeapVisitor& v){heap_edges(*static_cast<const T*>(p),v);};
        if constexpr(!std::is_const_v<T> && requires(T& x){x.clear();})
            record.clear=[](const void* p){const_cast<T*>(static_cast<const T*>(p))->clear();};
        else if constexpr(std::is_same_v<T,Value>) record.clear=[](const void* p){*const_cast<T*>(static_cast<const T*>(p))=T{};};
        else if constexpr(requires(T& x){x.fields.clear();}) record.clear=[](const void* p){const_cast<T*>(static_cast<const T*>(p))->fields.clear();};
        else if constexpr(requires(T& x){x.entries.clear();}) record.clear=[](const void* p){const_cast<T*>(static_cast<const T*>(p))->entries.clear();};
        else if constexpr(requires(T& x){x.values.clear();}) record.clear=[](const void* p){const_cast<T*>(static_cast<const T*>(p))->values.clear();};
        else if constexpr(requires(T& x){x.bindings.clear();x.deferred.clear();}) record.clear=[](const void* p){auto* x=const_cast<T*>(static_cast<const T*>(p));x->bindings.clear();x->deferred.clear();};
        records_.push_back(std::move(record));++allocated_;
    }
    std::size_t collect();std::size_t live() const;
    std::size_t allocated() const{return allocated_;}
    std::size_t collected() const{return collected_;}
};
ManagedHeap& managed_heap();
struct HeapScope {ManagedHeap* previous;explicit HeapScope(ManagedHeap&);~HeapScope();};
template<class T,class... Args> std::shared_ptr<T> managed(Args&&... args){
    auto& heap=managed_heap();
    if(heap.allocated()%256==255)heap.collect();
    auto object=std::make_shared<T>(std::forward<Args>(args)...);heap.track(object);return object;
}
}
