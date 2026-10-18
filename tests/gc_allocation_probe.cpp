#include "hua/gc.hpp"
#include "hua/value.hpp"
#include "support/native_allocations.hpp"
#include <iostream>
#include <unordered_map>
#include <stdexcept>
using hua::native_allocation::snapshot;
namespace fixture {
struct Node {std::vector<std::shared_ptr<Node>> links;void clear(){links.clear();}};
void heap_edges(const Node& n,const hua::HeapVisitor& visit){for(const auto& p:n.links)visit(p.get());}
}
namespace {
std::size_t checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
void calibration(){
    auto before=snapshot();void* a=::operator new(101);void* b=::operator new(257,std::align_val_t(64));void* c=::operator new(0);
    auto during=snapshot();check(during.calls-before.calls==3&&during.requested-before.requested==358&&during.live-before.live==358,"allocation count calibration");
    check(reinterpret_cast<std::uintptr_t>(b)%64==0,"aligned allocation");
    ::operator delete(a);::operator delete(b,std::align_val_t(64));::operator delete(c);check(snapshot().live==before.live,"deallocation restores live bytes");
    hua::native_allocation::fail_after(0);auto failed=::operator new(23,std::nothrow);check(failed==nullptr&&snapshot().live==before.live,"nothrow fault");hua::native_allocation::clear_failure();
}
void run(std::size_t n){
    hua::ManagedHeap heap(65536);std::vector<std::shared_ptr<fixture::Node>> nodes;
    for(std::size_t i=0;i<n;++i)nodes.push_back(std::make_shared<fixture::Node>());
    for(std::size_t i=0;i<n;++i)nodes[i]->links={nodes[(i+1)%n],nodes[(i+1)%n]};
    auto begin=snapshot();for(const auto& p:nodes)heap.track(p);auto tracked=snapshot();
    auto count=heap.collect();auto end=snapshot();check(count==0&&heap.live()==n,"rooted graph");
    std::cout<<"{\"vertices\":"<<n<<",\"track_calls\":"<<tracked.calls-begin.calls<<",\"track_requested\":"<<tracked.requested-begin.requested<<",\"track_live\":"<<tracked.live-begin.live<<",\"collect_calls\":"<<end.calls-tracked.calls<<",\"collect_requested\":"<<end.requested-tracked.requested<<",\"collect_live_delta\":"<<std::int64_t(end.live)-std::int64_t(tracked.live)<<"}\n";
    if(n){auto weak=std::weak_ptr(nodes.front());auto address=nodes.front().get();
        for(unsigned point=0;point<5;++point){hua::native_allocation::fail_after(point);bool failed=false;try{heap.collect();}catch(const std::bad_alloc&){failed=true;}hua::native_allocation::clear_failure();
            check(failed&&heap.live()==n&&weak.lock().get()==address,"collector allocation failure preserves roots");check(heap.collect()==0,"collector recovers after allocation failure");}
        nodes.clear();check(heap.collect()==n&&weak.expired()&&heap.live()==0,"cycle collected after failures");
    }
}
void payload(){
    auto before=snapshot();{
        auto object=std::make_shared<hua::StructData>();auto shell=snapshot();
        for(unsigned i=0;i<128;++i)object->fields.emplace(std::to_string(i),hua::Value(std::string(256,'x')));
        auto full=snapshot();std::cout<<"{\"storage_fixture\":\"StructData128_strings256\",\"object_size\":"<<sizeof(hua::StructData)<<",\"shared_allocation_requested\":"<<shell.requested-before.requested<<",\"backing_requested\":"<<full.requested-shell.requested<<",\"backing_live\":"<<full.live-shell.live<<",\"value_size\":"<<sizeof(hua::Value)<<"}\n";
    }check(snapshot().live==before.live,"owned backing released");
}
}
int main(){try{calibration();std::cout<<"{\"visitor_size\":"<<sizeof(hua::HeapVisitor)<<",\"weak_size\":"<<sizeof(std::weak_ptr<const void>)<<"}\n";
    for(auto n:{0u,1u,257u,8192u})run(n);payload();std::cout<<"{\"status\":\"passed\",\"checks\":"<<checks<<"}\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
