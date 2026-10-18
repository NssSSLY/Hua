#pragma once
// Diagnostic-only current-thread GC accounting. Never owns a managed object.
#ifdef HUA_GC_COST_PROFILE
#include <array>
#include <chrono>
#include <cstdint>
#include <ostream>
namespace hua::gc_cost {
using Clock=std::chrono::steady_clock;
inline constexpr const char* phases[]={"prune_expired","pin_and_index","graph_storage","enumerate_edges","discover_roots","mark_reachable","clear_unreachable","release_and_prune","scratch_teardown"};
#define HUA_GC_COST_FIELDS(X) \
 X(record_size_bytes) X(edge_visitor_builds) X(allocated) X(records_before) X(record_capacity_bytes) X(live_before) X(expired_before) \
 X(emitted_edges) X(null_edges) X(untracked_edges) X(tracked_edges) X(roots) X(external_excess) X(marked) \
 X(clearable_unreachable) X(unclearable_unreachable) X(records_after) X(expired_after) \
 X(object_growths) X(index_rehashes) X(adjacency_growths) X(adjacency_nonempty) X(work_growths) \
 X(flat_edge_growths) X(offsets_capacity_bytes) X(flat_edge_capacity_bytes) \
 X(object_capacity_bytes) X(adjacency_outer_bytes) X(adjacency_capacity_bytes) X(incoming_bytes) \
 X(marked_capacity_bytes) X(work_capacity_bytes) X(index_nodes) X(index_buckets) X(index_bucket_lower_bytes) \
 X(index_slots) X(index_slot_bytes) X(index_buffer_builds) X(index_build_probes) X(index_lookup_probes) X(index_lookup_calls) X(index_null_lookups) X(index_max_build_probes) X(index_max_lookup_probes) \
 X(struct_scans) X(struct_fields) X(map_scans) X(map_entries) X(list_scans) X(multi_scans) X(vector_scans) X(vector_values) X(error_scans) X(iofailure_scans)
struct Row {
#define HUA_GC_COST_MEMBER(n) std::uint64_t n{};
 HUA_GC_COST_FIELDS(HUA_GC_COST_MEMBER)
#undef HUA_GC_COST_MEMBER
 std::array<std::uint64_t,9> ns{};
 std::array<std::uint64_t,21> value_kinds{};
 std::array<std::array<std::uint64_t,21>,4> value_sources{};std::size_t source=3;
 std::uint64_t start_ns{},total_ns{};
};
struct Sink {
 std::ostream& output;Clock::time_point start=Clock::now();std::size_t rows{},dropped{};bool failed{};
 explicit Sink(std::ostream& stream):output(stream){}
 void emit(const Row& row) noexcept {
  if(rows>=4096){++dropped;return;}
  try {
   output<<"{\"schema\":\"hua.gc.cost.v4\",\"ordinal\":"<<++rows<<",\"start_ns\":"<<row.start_ns<<",\"total_ns\":"<<row.total_ns;
#define HUA_GC_COST_JSON(n) output<<",\"" #n "\":"<<row.n;
   HUA_GC_COST_FIELDS(HUA_GC_COST_JSON)
#undef HUA_GC_COST_JSON
   output<<",\"phases_ns\":{";for(std::size_t i=0;i<row.ns.size();++i){if(i)output<<',';output<<'"'<<phases[i]<<"\":"<<row.ns[i];}
   output<<"},\"value_kinds\":[";for(std::size_t i=0;i<row.value_kinds.size();++i){if(i)output<<',';output<<row.value_kinds[i];}output<<"],\"value_sources\":[";for(std::size_t i=0;i<row.value_sources.size();++i){if(i)output<<',';output<<'[';for(std::size_t j=0;j<21;++j){if(j)output<<',';output<<row.value_sources[i][j];}output<<']';}output<<"]}\n";output.flush();failed=failed||!output;
  }catch(...){failed=true;}
 }
};
inline thread_local Sink* active=nullptr;
inline thread_local Row* current=nullptr;
struct Source {Row* row=current;std::size_t previous=row?row->source:3;explicit Source(std::size_t n){if(row)row->source=n;}~Source(){if(row)row->source=previous;}};
struct Scope {Sink* previous;explicit Scope(Sink& sink):previous(active){active=&sink;}~Scope(){active=previous;}};
struct Collection {
 Sink* sink=active;Row data;Row* previous=current;Clock::time_point begin{},last{};
 Collection(){if(sink){begin=last=Clock::now();data.start_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(begin-sink->start).count();current=&data;}}
 bool enabled()const{return sink!=nullptr;}
 void phase(std::size_t i){if(sink){auto now=Clock::now();data.ns[i]+=std::chrono::duration_cast<std::chrono::nanoseconds>(now-last).count();last=now;}}
 template<class T> void push(const T& values,std::uint64_t& growths){if(sink&&values.size()==values.capacity())++growths;}
 ~Collection(){if(sink){phase(8);data.total_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(last-begin).count();current=previous;sink->emit(data);}}
};
}
#endif
