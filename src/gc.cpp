#include "hua/value.hpp"
#include "hua/gc.hpp"
#include <unordered_map>
#include <algorithm>
namespace hua {
namespace {thread_local ManagedHeap fallback;thread_local ManagedHeap* active=nullptr;}
ManagedHeap& managed_heap(){return active?*active:fallback;}
HeapScope::HeapScope(ManagedHeap& heap):previous(active){active=&heap;}
HeapScope::~HeapScope(){active=previous;}
std::size_t ManagedHeap::live() const {return std::count_if(records_.begin(),records_.end(),[](const auto& x){return !x.pointer.expired();});}
std::size_t ManagedHeap::collect(){
    std::erase_if(records_,[](const auto& x){return x.pointer.expired();});
    std::vector<std::shared_ptr<const void>> objects;std::unordered_map<const void*,std::size_t> index;
    for(const auto& r:records_){auto p=r.pointer.lock();index.emplace(p.get(),objects.size());objects.push_back(std::move(p));}
    std::vector<std::vector<std::size_t>> edges(objects.size());std::vector<std::size_t> incoming(objects.size());
    for(std::size_t i=0;i<objects.size();++i)if(records_[i].edges)records_[i].edges(objects[i].get(),[&](const void* p){if(auto f=index.find(p);f!=index.end()){edges[i].push_back(f->second);++incoming[f->second];}});
    std::vector<bool> marked(objects.size());std::vector<std::size_t> work;
    for(std::size_t i=0;i<objects.size();++i)if(objects[i].use_count()>static_cast<long>(incoming[i]+1)){marked[i]=true;work.push_back(i);}
    while(!work.empty()){auto i=work.back();work.pop_back();for(auto j:edges[i])if(!marked[j]){marked[j]=true;work.push_back(j);}}
    std::size_t count=0;for(std::size_t i=0;i<objects.size();++i)if(!marked[i]&&records_[i].clear){records_[i].clear(objects[i].get());++count;}
    objects.clear();std::erase_if(records_,[](const auto& x){return x.pointer.expired();});collected_+=count;return count;
}
void heap_edges(const Value& v,const HeapVisitor& visit){
    std::visit([&](const auto& x){using T=std::decay_t<decltype(x)>;
        if constexpr(requires{x.data.get();})visit(x.data.get());
        else if constexpr(std::is_same_v<T,SliceValue>){visit(x.storage.get());visit(x.packed.get());}
        else if constexpr(std::is_same_v<T,ResultValue>)visit(x.payload.get());
        else if constexpr(std::is_same_v<T,Callable>){visit(x.closure.get());if(x.receiver)visit(x.receiver->data.get());}
    },v.data);
}
void heap_edges(const ErrorData& x,const HeapVisitor& v){v(x.cause.get());}
void heap_edges(const StructData& x,const HeapVisitor& v){for(const auto& [_,item]:x.fields)heap_edges(item,v);}
void heap_edges(const MapData& x,const HeapVisitor& v){for(const auto& [_,item]:x.entries)heap_edges(item,v);}
void heap_edges(const ListData& x,const HeapVisitor& v){heap_edges(x.values,v);}
void heap_edges(const MultiData& x,const HeapVisitor& v){heap_edges(x.values,v);}
void heap_edges(const std::vector<Value>& x,const HeapVisitor& v){for(const auto& item:x)heap_edges(item,v);}
}
