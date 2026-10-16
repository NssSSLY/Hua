#pragma once
// Internal diagnostics only. Ordinary builds compile every hook away.
#ifdef HUA_INTERNAL_PROFILE
#include <atomic>
#include <array>
#include <cstdint>
#include <ostream>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <iomanip>
namespace hua::performance {
#define HUA_PERFORMANCE_FIELDS(X) \
 X(vm_steps) X(user_calls) X(builtin_calls) X(parameter_scan_nodes) X(function_metadata_builds) X(field_scan_nodes) \
 X(type_name_calls) X(type_argument_parses) X(field_lookups) \
 X(environment_lookups) X(environment_probes) X(copy_const) X(copy_move) \
 X(copy_struct) X(copy_struct_fields) X(deep_clone_roots) X(deep_clone_nodes) X(copy_result) X(copy_fixed_array) X(enforce_type_calls) \
 X(result_constraints) X(result_ok_constraints) X(result_err_constraints) X(result_void_constraints) \
 X(result_payload_unique) X(result_payload_shared) X(result_payload_struct) X(result_payload_nested) X(result_payload_fixed_array) \
 X(result_contextual_constraints) X(result_readonly_constraints) X(result_materializations) X(result_payload_type_changes) \
 X(managed_allocations) X(tracked_object_bytes) X(gc_collections) X(gc_records) X(gc_live_records) X(gc_edges) X(gc_nanoseconds) \
 X(resource_pumps) X(resource_candidates) X(resource_fast_returns) X(file_checkpoints)
enum class ConstraintOrigin { other, binding, argument, returning, borrow, assignment, count };
inline thread_local ConstraintOrigin constraint_origin=ConstraintOrigin::other;
struct ConstraintSource {
 ConstraintOrigin previous;explicit ConstraintSource(ConstraintOrigin source):previous(constraint_origin){constraint_origin=source;}
 ~ConstraintSource(){constraint_origin=previous;}
};
struct Counters {
#define HUA_COUNTER(name) std::atomic<std::uint64_t> name{0};
 HUA_PERFORMANCE_FIELDS(HUA_COUNTER)
#undef HUA_COUNTER
 std::array<std::atomic<std::uint64_t>,static_cast<std::size_t>(ConstraintOrigin::count)> result_origins{};
 struct StructCopies {std::uint64_t objects{},fields{};};
 bool struct_histogram_enabled{};
 mutable std::mutex struct_mutex;
 std::map<std::string,StructCopies> struct_copies;
};
inline std::atomic<Counters*> active{nullptr};
inline void record_result_origin(){if(auto* counters=active.load(std::memory_order_relaxed))counters->result_origins[static_cast<std::size_t>(constraint_origin)].fetch_add(1,std::memory_order_relaxed);}
inline void record_struct_copy(const std::string& name,std::size_t fields){
 if(auto* counters=active.load(std::memory_order_relaxed);counters&&counters->struct_histogram_enabled){
  std::lock_guard lock(counters->struct_mutex);auto& entry=counters->struct_copies[name];++entry.objects;entry.fields+=fields;
 }
}
struct CollectionTimer {
 using Clock=std::chrono::steady_clock;Counters* counters;Clock::time_point start;
 CollectionTimer():counters(active.load(std::memory_order_relaxed)),start(counters?Clock::now():Clock::time_point{}){}
 ~CollectionTimer(){if(counters)counters->gc_nanoseconds.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count(),std::memory_order_relaxed);}
};
struct Scope {
 Counters* previous;
 explicit Scope(Counters& counters):previous(active.exchange(&counters)){}
 ~Scope(){active.store(previous);}
};
inline void write(std::ostream& output,const Counters& counters) {
 output << "{\"schema\":\"hua.internal.performance.v1\"";
#define HUA_COUNTER_JSON(name) output << ",\"" #name "\":" << counters.name.load();
 HUA_PERFORMANCE_FIELDS(HUA_COUNTER_JSON)
#undef HUA_COUNTER_JSON
 output << ",\"result_origins\":{";bool first_origin=true;
 const char* origins[]={"other","binding","argument","returning","borrow","assignment"};
 for(std::size_t i=0;i<counters.result_origins.size();++i){if(!first_origin)output<<',';first_origin=false;output<<std::quoted(origins[i])<<':'<<counters.result_origins[i].load();}output<<'}';
 if(counters.struct_histogram_enabled){
  std::lock_guard lock(counters.struct_mutex);
  output << ",\"struct_copies\":{";bool first=true;
  for(const auto& [name,entry]:counters.struct_copies){
   if(!first)output<<',';first=false;output<<std::quoted(name)<<":{\"objects\":"<<entry.objects<<",\"fields\":"<<entry.fields<<'}';
  }output<<'}';
 }
 output << "}";
}
}
#define HUA_PROFILE_CONSTRAINT_SOURCE(name) ::hua::performance::ConstraintSource hua_constraint_source(::hua::performance::ConstraintOrigin::name)
#define HUA_PROFILE_ADD(name,amount) do { if(auto* hua_profile=::hua::performance::active.load(std::memory_order_relaxed)) hua_profile->name.fetch_add((amount),std::memory_order_relaxed); } while(false)
#define HUA_PROFILE_COUNT(name) HUA_PROFILE_ADD(name,1)
#else
#define HUA_PROFILE_CONSTRAINT_SOURCE(name) ((void)0)
#define HUA_PROFILE_ADD(name,amount) ((void)0)
#define HUA_PROFILE_COUNT(name) ((void)0)
#endif
