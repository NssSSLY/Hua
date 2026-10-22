#pragma once
#include "hua/performance.hpp"
#include <functional>
#include <type_traits>
#include <memory>
#include <vector>
#include <cstddef>
#include <cstdint>
namespace hua {
// Non-owning edge callback for collections (avoids std::function type erasure on
// every edge). The referenced callable must outlive the visitor: bind a named
// lambda, never a temporary.
class HeapVisitor {
    const void* target_{};void (*call_)(const void*,const void*){};
public:
    template<class F> requires (!std::is_same_v<std::remove_cvref_t<F>,HeapVisitor>)
    HeapVisitor(const F& f):target_(&f),call_([](const void* t,const void* p){(*static_cast<const F*>(t))(p);}){}
    void operator()(const void* p) const {call_(target_,p);}
};
struct Value;struct StructData;struct MapData;struct ListData;struct MultiData;struct ErrorData;struct IOFailureData;
void heap_edges(const Value&,const HeapVisitor&);
void heap_edges(const StructData&,const HeapVisitor&);
void heap_edges(const MapData&,const HeapVisitor&);
void heap_edges(const ListData&,const HeapVisitor&);
void heap_edges(const MultiData&,const HeapVisitor&);
void heap_edges(const ErrorData&,const HeapVisitor&);
void heap_edges(const IOFailureData&,const HeapVisitor&);
void heap_edges(const std::vector<Value>&,const HeapVisitor&);
class ManagedHeap {
    // track() installs only capture-free callbacks; records keep weak references.
    struct Record {std::weak_ptr<const void> pointer;void (*edges)(const void*,const HeapVisitor&){};void (*clear)(const void*){};};
    std::vector<Record> records_, old_records_, pending_records_;
    bool collecting_{};
    unsigned minor_since_major_{};
    std::size_t major_record_goal_{};
    std::size_t collect_records(std::vector<Record>&);
    // Collection-local work storage, reused by later collections of this heap.
    struct Scratch;std::shared_ptr<Scratch> scratch_;
    void merge_pending();
    struct CollectionGuard { bool& active; explicit CollectionGuard(bool& value):active(value){active=true;} ~CollectionGuard(){active=false;} };
public:
    struct CollectionWork { std::uint64_t minors{}, majors{}, objects{}, edges{}; };
private:
    CollectionWork collection_work_;

    std::size_t allocated_{},collected_{},collection_interval_{256};
public:
    explicit ManagedHeap(std::size_t interval=256):collection_interval_(interval==65536?65536:interval==8192?8192:256){}
    std::size_t collection_interval() const{return collection_interval_;}
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
        (collecting_?pending_records_:records_).push_back(std::move(record));++allocated_;
    }
    // Explicit collection remains a complete-heap operation. Automatic polls
    // collect only the new candidate set, with bounded complete-heap fallback.
    std::size_t collect();
    std::size_t poll();
    std::size_t live() const;
    const CollectionWork& collection_work() const { return collection_work_; }

    std::size_t allocated() const{return allocated_;}
    std::size_t collected() const{return collected_;}
};
ManagedHeap& managed_heap();
struct HeapScope {ManagedHeap* previous;explicit HeapScope(ManagedHeap&);~HeapScope();};
template<class T,class... Args> std::shared_ptr<T> managed(Args&&... args){
    auto& heap=managed_heap();
    if(heap.allocated()%heap.collection_interval()==heap.collection_interval()-1)heap.poll();
    HUA_PROFILE_COUNT(managed_allocations);HUA_PROFILE_ADD(tracked_object_bytes,sizeof(T));
    auto object=std::make_shared<T>(std::forward<Args>(args)...);heap.track(object);return object;
}
}
