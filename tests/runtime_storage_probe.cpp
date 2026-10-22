// Test-only storage counters; no call observer, bytecode change or instruction hook.
#include "hua/archive.hpp"
#include "hua/vm.hpp"
#include "support/native_allocations.hpp"
#include <iostream>
namespace hua {
struct VmSlotTestAccess {
 static void report(const VirtualMachine& vm,std::ostream& out){
  const auto& pool=*vm.frame_arena_;const auto& w=pool.work;std::size_t retained=0;
  for(const auto& frame:pool.free)retained+=frame->slots.capacity()*sizeof(std::optional<VirtualMachine::Binding>);
  out<<",\"activations\":"<<w.activations<<",\"created\":"<<w.created<<",\"reused\":"<<w.reused<<",\"retired\":"<<w.retired<<",\"active\":"<<w.active<<",\"peak_active\":"<<w.peak<<",\"cached_empty_environments\":"<<pool.free.size()<<",\"cached_slot_capacity_bytes\":"<<retained;
 }
};
}
int run(const std::filesystem::path& compiler,const std::filesystem::path& input,const std::filesystem::path& output){
 if(std::filesystem::exists(output))throw std::runtime_error("evidence already exists");
 auto image=hua::read_huab(compiler);hua::RuntimeContext context;context.input=&std::cin;context.working_directory=std::filesystem::current_path();
 auto utf8=[](const auto& p){auto text=p.u8string();return std::string(text.begin(),text.end());};
 context.arguments={utf8(input),utf8(output)};context.execution_limits={500000000,128,65536};context.execution_metrics=std::make_shared<hua::ExecutionMetrics>();context.heap=std::make_shared<hua::ManagedHeap>(65536);hua::HeapScope heap_scope(*context.heap);
 auto begin=hua::native_allocation::snapshot();int result=0;
 {
  hua::VirtualMachine vm(image.model,std::cout,&context);result=hua::report_entry(vm.run(image.bytecode),std::cerr);auto work=context.heap->collection_work();auto end=hua::native_allocation::snapshot();
  std::cerr<<"{\"schema\":\"hua.runtime.storage.v1\",\"managed_allocated\":"<<context.heap->allocated()<<",\"minor_collections\":"<<work.minors<<",\"major_collections\":"<<work.majors<<",\"scanned_objects\":"<<work.objects<<",\"emitted_edges\":"<<work.edges<<",\"native_allocations\":"<<end.calls-begin.calls<<",\"native_requested_bytes\":"<<end.requested-begin.requested<<",\"steps\":"<<context.execution_metrics->steps.load()<<",\"peak_calls\":"<<context.execution_metrics->peak_calls.load();hua::VmSlotTestAccess::report(vm,std::cerr);std::cerr<<"}\n";
 }
 context.heap->collect();auto live=context.heap->live();std::cerr<<"{\"schema\":\"hua.runtime.release.v1\",\"managed_live_after_vm\":"<<live<<"}\n";
 if(live)throw std::runtime_error("session retained managed objects");return result;
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){try{return argc==4?run(argv[1],argv[2],argv[3]):2;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#else
int main(int argc,char** argv){try{return argc==4?run(argv[1],argv[2],argv[3]):2;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#endif
