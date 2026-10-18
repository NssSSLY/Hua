#include "hua/gc.hpp"
#include "hua/gc_graph.hpp"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
namespace fixture {
struct Log {std::vector<std::pair<std::size_t,int>> edges;std::vector<std::size_t> clears;std::function<void()> on_clear;};
struct Node {
    std::size_t id;Log* log;std::vector<std::shared_ptr<Node>> values;
    Node(std::size_t n,Log& l):id(n),log(&l){}
    void clear(){log->clears.push_back(id);if(log->on_clear)log->on_clear();values.clear();}
};
void heap_edges(const Node& node,const hua::HeapVisitor& visit){
    for(const auto& value:node.values){node.log->edges.emplace_back(node.id,value?int(value->id):-1);visit(value.get());}
}
}
namespace {
std::uint64_t checks{},cases{},scanned{},cleared{},fingerprint=1469598103934665603ULL;
std::string context;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(context+": "+why);}
void hash(std::uint64_t value){fingerprint=(fingerprint^value)*1099511628211ULL;}
struct Spec {std::vector<std::vector<int>> links;std::vector<bool> roots;std::vector<std::size_t> foreign_roots;};
std::vector<bool> prune(const Spec& spec,std::vector<bool> active,bool rooted){
    std::vector<std::size_t> counts(active.size());
    for(std::size_t i=0;i<active.size();++i)if(active[i])for(auto target:spec.links[i])if(target>=0&&std::size_t(target)<active.size())++counts[target];
    if(rooted){for(std::size_t i=0;i<active.size();++i)counts[i]+=spec.roots[i];for(auto target:spec.foreign_roots)++counts[target];}
    // Independent reference-count elimination before cycle collection.
    bool changed=true;while(changed){changed=false;for(std::size_t i=0;i<active.size();++i)if(active[i]&&counts[i]==0){active[i]=false;changed=true;for(auto target:spec.links[i])if(target>=0&&std::size_t(target)<active.size())--counts[target];}}
    return active;
}
std::vector<bool> reachable(const Spec& spec,const std::vector<bool>& active,bool rooted){
    std::vector<bool> marked(active.size());std::vector<std::size_t> pending;
    if(rooted){for(std::size_t i=0;i<active.size();++i)if(active[i]&&spec.roots[i])pending.push_back(i);for(auto target:spec.foreign_roots)if(active[target])pending.push_back(target);}
    while(!pending.empty()){auto i=pending.back();pending.pop_back();if(marked[i])continue;marked[i]=true;for(auto target:spec.links[i])if(target>=0&&std::size_t(target)<active.size()&&active[target]&&!marked[target])pending.push_back(target);}
    return marked;
}
void table_contract(const Spec& spec,const std::vector<bool>& active){
    std::vector<std::size_t> ids;for(std::size_t i=0;i<active.size();++i)if(active[i])ids.push_back(i);
    std::vector<std::vector<std::size_t>> reference(ids.size());
    for(std::size_t i=0;i<ids.size();++i)for(auto target:spec.links[ids[i]])if(target>=0&&std::size_t(target)<active.size()&&active[target])reference[i].push_back(std::find(ids.begin(),ids.end(),std::size_t(target))-ids.begin());
    hua::gc_detail::EdgeTable flat(ids.size());std::vector<std::size_t> incoming(ids.size());
    for(std::size_t i=0;i<reference.size();++i){flat.begin_vertex(i);for(auto target:reference[i]){flat.targets.push_back(target);++incoming[target];}}flat.finish();
    check(flat.offsets.size()==ids.size()+1&&flat.offsets.back()==flat.targets.size(),"offset endpoint");
    std::vector<std::size_t> expected_incoming(ids.size());
    for(std::size_t i=0;i<reference.size();++i){auto view=flat.outgoing(i);check(std::equal(view.begin(),view.end(),reference[i].begin(),reference[i].end()),"exact edge order/multiplicity");for(auto j:reference[i])++expected_incoming[j];}
    check(incoming==expected_incoming,"duplicate edge incoming count");
}
void run(const std::string& name,const Spec& spec){
    ++cases;fixture::Log log;hua::ManagedHeap heap(65536);const auto n=spec.links.size();
    std::vector<std::shared_ptr<fixture::Node>> nodes,roots;std::vector<std::weak_ptr<fixture::Node>> weak;std::vector<const void*> addresses;
    for(std::size_t i=0;i<n;++i){auto p=std::make_shared<fixture::Node>(i,log);heap.track(p);nodes.push_back(p);weak.push_back(p);addresses.push_back(p.get());}
    auto foreign=std::make_shared<fixture::Node>(n,log);
    for(std::size_t i=0;i<n;++i){for(auto target:spec.links[i])nodes[i]->values.push_back(target<0?nullptr:std::size_t(target)==n?foreign:nodes.at(target));if(spec.roots[i])roots.push_back(nodes[i]);}
    for(auto target:spec.foreign_roots)foreign->values.push_back(nodes.at(target));nodes.clear();
    auto phase=[&](const char* label,std::vector<bool> before,bool rooted){
        context=name+"/"+label;auto active=prune(spec,std::move(before),rooted),marked=reachable(spec,active,rooted);table_contract(spec,active);
        std::vector<std::pair<std::size_t,int>> expected_edges;std::vector<std::size_t> expected_clears;
        for(std::size_t i=0;i<n;++i)if(active[i]){for(auto target:spec.links[i])expected_edges.emplace_back(i,target);if(!marked[i])expected_clears.push_back(i);}
        log.edges.clear();log.clears.clear();const auto result=heap.collect();
        check(log.edges==expected_edges,"one visit per live vertex, original callback order");
        check(log.clears==expected_clears&&result==expected_clears.size(),"original clear order and count");
        check(heap.live()==std::count(marked.begin(),marked.end(),true),"live set cardinality");
        for(std::size_t i=0;i<n;++i){check(weak[i].expired()==!marked[i],"exact survivor identity");if(marked[i])check(weak[i].lock().get()==addresses[i],"survivor address moved");}
        scanned+=expected_edges.size();cleared+=result;hash(result);hash(heap.live());for(auto [i,j]:log.edges){hash(i);hash(std::uint64_t(j+1));}for(auto i:log.clears)hash(i);
        return marked;
    };
    auto active=phase("initial",std::vector<bool>(n,true),true);active=phase("repeat",active,true);
    roots.clear();foreign->values.clear();foreign.reset();phase("release",active,false);
    check(heap.live()==0,"final release");
}
Spec ring(std::size_t n,unsigned degree,bool rooted){Spec s;s.links.resize(n);s.roots.resize(n);if(n&&rooted)s.roots[0]=true;for(std::size_t i=0;i<n;++i)for(unsigned j=0;j<degree;++j)s.links[i].push_back((i+1)%n);return s;}
void nested_collection(){
    ++cases;context="nested-heap-collection";fixture::Log outer_log,inner_log;hua::ManagedHeap outer(65536),inner(65536);
    auto make=[](hua::ManagedHeap& heap,fixture::Log& log,std::size_t n){std::vector<std::shared_ptr<fixture::Node>> nodes;for(std::size_t i=0;i<n;++i){auto p=std::make_shared<fixture::Node>(i,log);heap.track(p);nodes.push_back(p);}for(std::size_t i=0;i<n;++i)nodes[i]->values.push_back(nodes[(i+1)%n]);return nodes;};
    auto children=make(inner,inner_log,3);std::weak_ptr<fixture::Node> inner_weak=children[0];children.clear();
    auto parents=make(outer,outer_log,2);std::weak_ptr<fixture::Node> outer_weak=parents[0];parents.clear();unsigned calls=0;
    outer_log.on_clear=[&]{if(++calls==1){check(inner.collect()==3&&inner_weak.expired()&&inner.live()==0,"inner cycle not released during outer clear");}};
    check(outer.collect()==2&&outer_weak.expired()&&outer.live()==0,"outer graph corrupted by nested collection");
    check(calls==2&&outer_log.clears==std::vector<std::size_t>{0,1}&&inner_log.clears==std::vector<std::size_t>{0,1,2},"nested clear order");
    check(inner.collect()==0&&outer.collect()==0,"nested collector left pins or records");hash(calls);hash(inner.live());hash(outer.live());
}
std::uint32_t state=0x81a72d3bu;
std::uint32_t random(){state^=state<<13;state^=state>>17;state^=state<<5;return state;}
}
int main(int argc,char** argv){try{
    if(argc==2&&std::string(argv[1])=="--nested-only"){nested_collection();std::cout<<"{\"nested_cases\":"<<cases<<",\"checks\":"<<checks<<",\"fingerprint\":"<<fingerprint<<"}\n";return 0;}
    run("empty",Spec{});run("self-duplicate",ring(1,1024,false));run("root-self-duplicate",ring(1,1024,true));
    run("edge-count-exceeds-vertices",ring(2,2048,true));run("large-dead-ring",ring(8192,4,false));run("large-live-ring",ring(8192,4,true));
    Spec chain;chain.links.resize(257);chain.roots.resize(257);chain.roots[0]=true;for(int i=0;i<256;++i)chain.links[i].push_back(i+1);run("chain",chain);chain.roots[0]=false;run("expired-chain",chain);
    Spec isolated;isolated.links.resize(257);isolated.roots.resize(257,true);run("isolated-roots",isolated);
    for(unsigned test=0;test<192;++test){const auto n=1+random()%48;Spec s;s.links.resize(n);s.roots.resize(n);for(std::size_t i=0;i<n;++i){s.roots[i]=random()%9==0;for(unsigned j=0,count=random()%10;j<count;++j)s.links[i].push_back(int(random()%(n+2))-1);}for(unsigned i=0,count=random()%5;i<count;++i)s.foreign_roots.push_back(random()%n);run("mixed-"+std::to_string(test),s);}
    nested_collection();
    std::cout<<"{\"cases\":"<<cases<<",\"checks\":"<<checks<<",\"observed_edges\":"<<scanned<<",\"cleared\":"<<cleared<<",\"fingerprint\":"<<fingerprint<<"}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
