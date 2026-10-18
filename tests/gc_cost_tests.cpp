#include "hua/gc.hpp"
#include "hua/gc_cost.hpp"
#include "hua/value.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
namespace fixture {
struct Node {std::vector<std::shared_ptr<Node>> values;};
void heap_edges(const Node& node,const hua::HeapVisitor& visit){for(const auto& child:node.values)visit(child.get());}
}
unsigned checks{};
void check(bool condition,const char* reason){++checks;if(!condition)throw std::runtime_error(reason);}
std::size_t collect(hua::ManagedHeap& heap,const std::string& name){std::cout<<"{\"fixture\":\""<<name<<"\"}\n";return heap.collect();}
std::shared_ptr<fixture::Node> node(hua::ManagedHeap& heap){auto p=std::make_shared<fixture::Node>();heap.track(p);return p;}
void ring(std::size_t n,unsigned repeats){
 hua::ManagedHeap heap(65536);std::vector<std::shared_ptr<fixture::Node>> nodes;
 for(std::size_t i=0;i<n;++i)nodes.push_back(node(heap));
 for(std::size_t i=0;i<n;++i)for(unsigned j=0;j<repeats;++j)nodes[i]->values.push_back(nodes[(i+1)%n]);
 auto root=nodes[0];std::weak_ptr<fixture::Node> weak=root;const auto address=root.get();nodes.clear();
 auto label=std::to_string(n)+"-"+std::to_string(repeats);
 check(collect(heap,"live-ring-"+label)==0&&heap.live()==n&&root.get()==address,"rooted duplicate ring changed");
 root.reset();check(collect(heap,"dead-ring-"+label)==n&&heap.live()==0&&weak.expired(),"unreachable duplicate ring retained");
}
int main(){try{
 hua::gc_cost::Sink sink(std::cout);hua::gc_cost::Scope scope(sink);
 {hua::ManagedHeap heap(65536);check(collect(heap,"empty")==0,"empty collector");}
 {hua::ManagedHeap heap(65536);for(unsigned i=0;i<64;++i)(void)node(heap);check(collect(heap,"expired64")==0&&heap.live()==0,"expired records");}
 {hua::ManagedHeap heap(65536);std::vector<std::shared_ptr<fixture::Node>> nodes;for(unsigned i=0;i<16;++i)nodes.push_back(node(heap));
  for(unsigned i=0;i+1<16;++i)nodes[i]->values.push_back(nodes[i+1]);auto root=nodes[0];nodes.clear();
  check(collect(heap,"live-chain16")==0&&heap.live()==16,"live chain");root.reset();check(collect(heap,"released-chain16")==0&&heap.live()==0,"chain immediate release");}
 ring(4,2);ring(256,4);ring(4096,4);
 {hua::ManagedHeap heap(65536);auto a=node(heap),foreign=std::make_shared<fixture::Node>();a->values={nullptr,foreign};foreign->values={a};std::weak_ptr<fixture::Node> weak=a;a.reset();
  check(collect(heap,"foreign-holder")==0&&heap.live()==1,"foreign reference root");foreign->values.clear();check(weak.expired(),"foreign holder release");
  check(collect(heap,"foreign-released")==0&&heap.live()==0,"foreign expired record");}
 {hua::ManagedHeap heap(65536);hua::HeapScope hs(heap);auto box=hua::managed<hua::StructData>();for(unsigned i=0;i<4;++i)box->fields.emplace(std::to_string(i),hua::Value(std::int64_t(i)));box->fields.emplace("text",hua::Value(std::string("scalar")));
  auto list=hua::managed<hua::ListData>();for(unsigned i=0;i<3;++i)list->values.emplace_back(std::int64_t(i));list->values.emplace_back(hua::StructValue{box,true});
  check(collect(heap,"value-kinds")==0&&heap.live()==2,"scalar Values root graph");}
 {hua::ManagedHeap heap(65536);std::vector<std::shared_ptr<fixture::Node>> nodes;for(unsigned i=0;i<4096;++i)nodes.push_back(node(heap));
  check(collect(heap,"sparse-roots4096")==0&&heap.live()==4096,"independent external roots");nodes.clear();check(collect(heap,"sparse-released4096")==0&&heap.live()==0,"sparse RC release");}
 {hua::ManagedHeap heap(65536);for(unsigned i=0;i<65536;++i)(void)node(heap);check(collect(heap,"expired65536")==0&&heap.live()==0,"production-sized expired batch");}
 {std::ostringstream nested;hua::gc_cost::Sink second(nested);{hua::gc_cost::Scope scope2(second);hua::ManagedHeap heap(65536);heap.collect();}check(second.rows==1&&!second.failed,"nested diagnostic scope");}
 {hua::ManagedHeap heap(65536);collect(heap,"restored-scope");check(hua::gc_cost::active==&sink&&hua::gc_cost::current==nullptr,"diagnostic scope/row restored");}
 check(!sink.failed&&sink.dropped==0&&sink.rows==17,"diagnostic rows complete");
 std::cerr<<checks<<" GC diagnostic fixture checks passed\n";return 0;
 }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
