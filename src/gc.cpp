#include "hua/performance.hpp"
#include "hua/value.hpp"
#include "hua/gc.hpp"
#include "hua/gc_graph.hpp"
#include "hua/gc_index.hpp"
#include <algorithm>
#ifdef HUA_GC_COST_PROFILE
#include "hua/gc_cost.hpp"
#endif
namespace hua {
#ifdef HUA_GC_COST_PROFILE
static_assert(std::variant_size_v<Value::Data> == 21,"Update GC cost Value kind schema");
#endif
namespace {thread_local ManagedHeap fallback;thread_local ManagedHeap* active=nullptr;}
ManagedHeap& managed_heap(){return active?*active:fallback;}
HeapScope::HeapScope(ManagedHeap& heap):previous(active){active=&heap;}
HeapScope::~HeapScope(){active=previous;}
std::size_t ManagedHeap::live() const {return std::count_if(records_.begin(),records_.end(),[](const auto& x){return !x.pointer.expired();});}
std::size_t ManagedHeap::collect(){
#ifdef HUA_GC_COST_PROFILE
    gc_cost::Collection cost;
    if(cost.enabled()){cost.data.allocated=allocated_;cost.data.records_before=records_.size();cost.data.record_capacity_bytes=records_.capacity()*sizeof(Record);cost.data.record_size_bytes=sizeof(Record);}
#endif
#ifdef HUA_INTERNAL_PROFILE
    performance::CollectionTimer collection_timer;
#endif
    HUA_PROFILE_COUNT(gc_collections);HUA_PROFILE_ADD(gc_records,records_.size());
    std::erase_if(records_,[](const auto& x){return x.pointer.expired();});
    HUA_PROFILE_ADD(gc_live_records,records_.size());
#ifdef HUA_GC_COST_PROFILE
    if(cost.enabled()){cost.data.live_before=records_.size();cost.data.expired_before=cost.data.records_before-records_.size();}cost.phase(0);
#endif
    std::vector<std::shared_ptr<const void>> objects;
#ifdef HUA_GC_COST_PROFILE
    gc_detail::AddressIndex<gc_detail::AddressHash,true> index(records_.size());
#else
    gc_detail::AddressIndex<> index(records_.size());
#endif
    for(const auto& r:records_){auto p=r.pointer.lock();
#ifdef HUA_GC_COST_PROFILE
        cost.push(objects,cost.data.object_growths);
#endif
        index.emplace(p.get(),objects.size());objects.push_back(std::move(p));
    }
#ifdef HUA_GC_COST_PROFILE
    cost.phase(1);
#endif
    gc_detail::EdgeTable edges(objects.size());std::vector<std::size_t> incoming(objects.size());
#ifdef HUA_GC_COST_PROFILE
    cost.phase(2);
#endif
    // One synchronous, non-owning visitor per collection. Its single state
    // pointer fits std::function's inline storage in both diagnostic modes.
    struct EdgeState {
        decltype(index)& addresses;gc_detail::EdgeTable& graph;decltype(incoming)& counts;
#ifdef HUA_GC_COST_PROFILE
        gc_cost::Collection& cost;
#endif
    } state{index,edges,incoming
#ifdef HUA_GC_COST_PROFILE
        ,cost
#endif
    };
    const HeapVisitor visitor=[&state](const void* p){
#ifdef HUA_GC_COST_PROFILE
        auto& cost=state.cost;if(cost.enabled()){++cost.data.emitted_edges;if(!p)++cost.data.null_edges;}
#endif
        if(auto target=state.addresses.find(p);target!=gc_detail::AddressIndex<>::missing){
#ifdef HUA_GC_COST_PROFILE
            if(cost.enabled())++cost.data.tracked_edges;cost.push(state.graph.targets,cost.data.flat_edge_growths);
#endif
            state.graph.targets.push_back(target);++state.counts[target];
        }
#ifdef HUA_GC_COST_PROFILE
        else if(cost.enabled())++cost.data.untracked_edges;
#endif
    };
#ifdef HUA_GC_COST_PROFILE
    if(cost.enabled())cost.data.edge_visitor_builds=1;
#endif
    for(std::size_t i=0;i<objects.size();++i){edges.begin_vertex(i);if(records_[i].edges)records_[i].edges(objects[i].get(),visitor);}
    edges.finish();
#ifdef HUA_GC_COST_PROFILE
    cost.phase(3);
#endif
#ifdef HUA_INTERNAL_PROFILE
    for(std::size_t i=0;i<objects.size();++i){HUA_PROFILE_ADD(gc_edges,edges.offsets[i+1]-edges.offsets[i]);}
#endif
    std::vector<bool> marked(objects.size());std::vector<std::size_t> work;
    for(std::size_t i=0;i<objects.size();++i)if(objects[i].use_count()>static_cast<long>(incoming[i]+1)){
#ifdef HUA_GC_COST_PROFILE
        if(cost.enabled()){++cost.data.roots;++cost.data.marked;cost.data.external_excess+=objects[i].use_count()-(incoming[i]+1);}cost.push(work,cost.data.work_growths);
#endif
        marked[i]=true;work.push_back(i);
    }
#ifdef HUA_GC_COST_PROFILE
    cost.phase(4);
#endif
    while(!work.empty()){auto i=work.back();work.pop_back();for(auto j:edges.outgoing(i))if(!marked[j]){
#ifdef HUA_GC_COST_PROFILE
        if(cost.enabled())++cost.data.marked;cost.push(work,cost.data.work_growths);
#endif
        marked[j]=true;work.push_back(j);
    }}
#ifdef HUA_GC_COST_PROFILE
    cost.phase(5);
#endif
    std::size_t count=0;for(std::size_t i=0;i<objects.size();++i)if(!marked[i]&&records_[i].clear){records_[i].clear(objects[i].get());++count;}
#ifdef HUA_GC_COST_PROFILE
    if(cost.enabled()){cost.data.clearable_unreachable=count;cost.data.unclearable_unreachable=objects.size()-cost.data.marked-count;}cost.phase(6);
#endif
    objects.clear();std::erase_if(records_,[](const auto& x){return x.pointer.expired();});collected_+=count;
#ifdef HUA_GC_COST_PROFILE
    if(cost.enabled()){
        auto& d=cost.data;d.records_after=records_.size();d.expired_after=d.live_before-records_.size();
        d.object_capacity_bytes=objects.capacity()*sizeof(decltype(objects)::value_type);
        d.offsets_capacity_bytes=edges.offsets.capacity()*sizeof(std::size_t);d.flat_edge_capacity_bytes=edges.targets.capacity()*sizeof(std::size_t);
        for(std::size_t i=0;i<incoming.size();++i)d.adjacency_nonempty+=edges.offsets[i]!=edges.offsets[i+1];
        d.incoming_bytes=incoming.capacity()*sizeof(std::size_t);d.marked_capacity_bytes=marked.capacity()/8;d.work_capacity_bytes=work.capacity()*sizeof(std::size_t);
        d.index_nodes=index.size();d.index_slots=index.capacity();d.index_slot_bytes=index.capacity_bytes();d.index_buffer_builds=index.capacity()!=0;
        const auto& probes=index.statistics();d.index_build_probes=probes.build;d.index_lookup_probes=probes.lookup;d.index_lookup_calls=probes.lookup_calls;d.index_null_lookups=probes.null_lookups;d.index_max_build_probes=probes.max_build;d.index_max_lookup_probes=probes.max_lookup;
    }cost.phase(7);
#endif
    return count;
}
void heap_edges(const Value& v,const HeapVisitor& visit){
#ifdef HUA_GC_COST_PROFILE
    if(gc_cost::current){++gc_cost::current->value_kinds[v.data.index()];++gc_cost::current->value_sources[gc_cost::current->source][v.data.index()];}
#endif
    std::visit([&](const auto& x){using T=std::decay_t<decltype(x)>;
        if constexpr(requires{x.data.get();})visit(x.data.get());
        else if constexpr(std::is_same_v<T,SliceValue>){visit(x.storage.get());visit(x.packed.get());}
        else if constexpr(std::is_same_v<T,ResultValue>)visit(x.payload.get());
        else if constexpr(std::is_same_v<T,Callable>){visit(x.closure.get());if(x.receiver)visit(x.receiver->data.get());}
    },v.data);
}
void heap_edges(const IOFailureData& x,const HeapVisitor& v){
#ifdef HUA_GC_COST_PROFILE
    if(gc_cost::current){++gc_cost::current->iofailure_scans;}
#endif
v(x.error.data.get());if(x.partial)v(x.partial->data.get());}
void heap_edges(const ErrorData& x,const HeapVisitor& v){
#ifdef HUA_GC_COST_PROFILE
    if(gc_cost::current){++gc_cost::current->error_scans;}
#endif
v(x.cause.get());}
void heap_edges(const StructData& x,const HeapVisitor& v){
#ifdef HUA_GC_COST_PROFILE
    if(gc_cost::current){++gc_cost::current->struct_scans;gc_cost::current->struct_fields+=x.fields.size();}
#endif
#ifdef HUA_GC_COST_PROFILE
    gc_cost::Source source(0);
#endif
x.fields.each_value([&](const Value& item){heap_edges(item,v);});}
void heap_edges(const MapData& x,const HeapVisitor& v){
#ifdef HUA_GC_COST_PROFILE
    if(gc_cost::current){++gc_cost::current->map_scans;gc_cost::current->map_entries+=x.entries.size();}
#endif
#ifdef HUA_GC_COST_PROFILE
    gc_cost::Source source(1);
#endif
for(const auto& [_,item]:x.entries)heap_edges(item,v);}
void heap_edges(const ListData& x,const HeapVisitor& v){
#ifdef HUA_GC_COST_PROFILE
    if(gc_cost::current){++gc_cost::current->list_scans;}
#endif
heap_edges(x.values,v);}
void heap_edges(const MultiData& x,const HeapVisitor& v){
#ifdef HUA_GC_COST_PROFILE
    if(gc_cost::current){++gc_cost::current->multi_scans;}
#endif
heap_edges(x.values,v);}
void heap_edges(const std::vector<Value>& x,const HeapVisitor& v){
#ifdef HUA_GC_COST_PROFILE
    if(gc_cost::current){++gc_cost::current->vector_scans;gc_cost::current->vector_values+=x.size();}
#endif
#ifdef HUA_GC_COST_PROFILE
    gc_cost::Source source(2);
#endif
for(const auto& item:x)heap_edges(item,v);}
}
