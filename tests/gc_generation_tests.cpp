#include "hua/gc.hpp"
#include "hua/value.hpp"
#include "support/native_allocations.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <functional>
namespace fixture {
struct Node {std::vector<std::shared_ptr<Node>> links;unsigned* visits{};std::function<void()> clearing;void clear(){links.clear();if(clearing)clearing();}};
void heap_edges(const Node& node,const hua::HeapVisitor& visit){if(node.visits)++*node.visits;for(const auto& p:node.links)visit(p.get());}
}
namespace {
unsigned checks=0;
void check(bool value,const char* what){++checks;if(!value)throw std::runtime_error(what);}
void scope_and_bound(){
 hua::ManagedHeap heap(65536);unsigned old_visits=0,young_visits=0;
 std::vector<std::shared_ptr<fixture::Node>> old;
 for(unsigned i=0;i<1024;++i){auto p=std::make_shared<fixture::Node>();p->visits=&old_visits;heap.track(p);old.push_back(std::move(p));}
 heap.poll();check(old_visits==1024,"initial candidate scan");old_visits=0;
 auto a=std::make_shared<fixture::Node>(),b=std::make_shared<fixture::Node>();a->visits=&young_visits;b->visits=&young_visits;a->links={b,b};b->links={a};heap.track(a);heap.track(b);
 std::weak_ptr<fixture::Node> wa=a;old.front()->links={a};a.reset();b.reset();
 heap.poll();check(old_visits==0,"minor scanned old objects");check(young_visits==2,"minor omitted young objects");check(!wa.expired(),"old to young strong reference lost");
 old.front()->links.clear();
 for(unsigned i=0;i<5;++i)heap.poll();
 check(old_visits==0,"old partition scanned before major boundary");check(!wa.expired(),"promoted cycle must wait for major");
 heap.poll();check(wa.expired(),"automatic major did not reclaim promoted garbage");check(old_visits==1024,"bounded major omitted old graph");
 const auto& work=heap.collection_work();check(work.minors==7&&work.majors==1,"automatic collection schedule");
 check(work.objects==1024+2+1024+2,"minor scan work accounting");
 old.clear();heap.collect();check(heap.live()==0,"old roots leaked");
}
void exhaustive(){
 // Independent graph reachability is the oracle; actual shared_ptr counts are
 // never provided to the oracle and no collector algorithm is copied here.
 for(unsigned n=1;n<=3;++n)for(unsigned mask=0;mask<(1u<<(n*n));++mask)for(unsigned roots=0;roots<(1u<<n);++roots)for(unsigned young=0;young<(1u<<n);++young){
  hua::ManagedHeap heap(65536);std::vector<std::shared_ptr<fixture::Node>> nodes(n);std::vector<std::weak_ptr<fixture::Node>> weak(n);
  for(unsigned i=0;i<n;++i){nodes[i]=std::make_shared<fixture::Node>();weak[i]=nodes[i];if(!(young&(1u<<i)))heap.track(nodes[i]);}
  heap.poll();for(unsigned i=0;i<n;++i)if(young&(1u<<i))heap.track(nodes[i]);
  for(unsigned u=0;u<n;++u)for(unsigned v=0;v<n;++v)if(mask&(1u<<(u*n+v)))nodes[u]->links.push_back(nodes[v]);
  unsigned live=roots;
  for(unsigned pass=0;pass<n;++pass)for(unsigned u=0;u<n;++u)if(live&(1u<<u))for(unsigned v=0;v<n;++v)if(mask&(1u<<(u*n+v)))live|=1u<<v;
  for(unsigned i=0;i<n;++i)if(!(roots&(1u<<i)))nodes[i].reset();
  heap.poll();for(unsigned i=0;i<n;++i)if(live&(1u<<i))check(!weak[i].expired(),"minor reclaimed globally reachable object");
  heap.collect();for(unsigned i=0;i<n;++i)check(weak[i].expired()==!(live&(1u<<i)),"full collector disagrees with independent reachability");
  nodes.clear();heap.collect();check(heap.live()==0,"exhaustive graph leaked after root release");
 }
}
void failures(){
 using namespace hua::native_allocation;
 for(unsigned point=0;point<12;++point){
  hua::ManagedHeap heap(65536);auto old=std::make_shared<fixture::Node>();heap.track(old);heap.poll();
  auto a=std::make_shared<fixture::Node>(),b=std::make_shared<fixture::Node>();a->links={b};b->links={a};heap.track(a);heap.track(b);old->links={a};
  std::weak_ptr<fixture::Node> weak=a;a.reset();b.reset();fail_after(point);
  try{heap.poll();}catch(const std::bad_alloc&){}clear_failure();check(!weak.expired()&&heap.live()==3,"minor allocation failure lost records/roots");
  check(heap.collect()==0,"collector did not recover after failed poll");old->links.clear();check(heap.collect()==2&&weak.expired(),"cycle not reclaimable after allocation failure");
 }
}
void pending_and_reentry(){
 hua::ManagedHeap heap(65536);hua::HeapScope scope(heap);std::shared_ptr<fixture::Node> created;
 auto a=hua::managed<fixture::Node>();a->links={a};a->clearing=[&]{check(heap.collect()==0,"same heap reentry");created=hua::managed<fixture::Node>();};a.reset();
 check(heap.collect()==1,"clear callback cycle");check(heap.live()==1&&created,"deferred tracking lost during collection");created.reset();heap.collect();check(heap.live()==0,"deferred track cleanup");
}
}
int main(){try{scope_and_bound();exhaustive();failures();pending_and_reentry();std::cout<<"{\"status\":\"passed\",\"checks\":"<<checks<<",\"old_objects_in_minor\":0,\"major_poll_bound\":8}\n";return 0;}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
