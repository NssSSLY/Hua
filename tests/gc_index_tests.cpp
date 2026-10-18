#include "hua/gc_index.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <unordered_map>
// Counts requested allocation bytes in this executable only; no RSS or live-byte claim.
namespace allocation {
struct Counts {std::size_t calls{},bytes{};};
thread_local Counts* active=nullptr;
struct Scope {Counts* previous;explicit Scope(Counts& c):previous(active){active=&c;}~Scope(){active=previous;}};
}
void* operator new(std::size_t bytes){
    if(auto p=std::malloc(bytes?bytes:1)){if(allocation::active){++allocation::active->calls;allocation::active->bytes+=bytes;}return p;}
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
namespace {
std::size_t checks{};
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
template<class Error,class Function> void rejects(Function f){bool threw=false;try{f();}catch(const Error&){threw=true;}check(threw,"expected exception");}
struct CollisionHash {std::size_t operator()(const void*)const noexcept{return 0;}};
struct WrapHash {std::size_t operator()(const void*)const noexcept{return std::numeric_limits<std::size_t>::max();}};
template<class Hash> void compare(std::size_t n){
    std::vector<std::uint64_t> keys(n+17);
    hua::gc_detail::AddressIndex<Hash,true> index(n);
    std::unordered_map<const void*,std::size_t> old;
    for(std::size_t i=0;i<n;++i){check(index.emplace(&keys[i],i*7+3)==old.emplace(&keys[i],i*7+3).second,"insert oracle");}
    for(std::size_t i=n;i>0;--i){check(!index.emplace(&keys[i-1],999),"first insertion wins");}
    for(std::size_t i=0;i<keys.size();++i){auto found=old.find(&keys[i]);check(index.find(&keys[i])==(found==old.end()?index.missing:found->second),"lookup oracle");}
    check(index.find(nullptr)==index.missing,"null misses");check(index.size()==old.size(),"same size");
    check(n==0||index.capacity()>=2*n,"half load bound");
    const auto& facts=index.statistics();check(facts.lookup_calls==keys.size()+1&&facts.null_lookups==1,"lookup counts");
    check(facts.max_lookup<=n+1&&facts.max_build<=n,"bounded collision probes");
    if(n){check(facts.build>=n*2,"build probe lower bound");check(facts.lookup>=keys.size(),"lookup probe lower bound");}
}
void boundaries(){
    using Index=hua::gc_detail::AddressIndex<>;auto cap=Index::capacity_for;
    check(cap(0,0)==0,"zero capacity");check(cap(1,8)==8,"minimum capacity");check(cap(4,8)==8,"half load");check(cap(8,16)==16,"exact power");check(cap(5,17)==16,"round up");
    rejects<std::length_error>([&]{cap(1,7);});rejects<std::length_error>([&]{cap(5,15);});rejects<std::length_error>([&]{cap(9,16);});
    const auto max=std::numeric_limits<std::size_t>::max();rejects<std::length_error>([&]{cap(max,max);});check(cap(std::bit_floor(max)/2,max)==std::bit_floor(max),"largest checked capacity");
    int keys[6]{};Index empty(0);check(empty.find(&keys[0])==Index::missing,"empty miss");rejects<std::length_error>([&]{empty.emplace(keys,0);});
    Index bounded(1);rejects<std::invalid_argument>([&]{bounded.emplace(nullptr,0);});
    for(std::size_t i=0;i<4;++i)check(bounded.emplace(&keys[i],i),"fill half capacity");
    check(!bounded.emplace(&keys[2],999),"duplicate at load bound");rejects<std::length_error>([&]{bounded.emplace(&keys[4],4);});
    for(std::size_t i=0;i<4;++i)check(bounded.find(&keys[i])==i,"unchanged after rejection");
    {Index prior(1);prior.emplace(keys,41);check(prior.find(keys)==41,"old collection value");}
    Index fresh(1);check(fresh.find(keys)==Index::missing,"fresh collection has no stale address");fresh.emplace(keys,73);check(fresh.find(keys)==73,"reused address new value");
}
void allocations(std::size_t n){
    std::vector<std::uint64_t> keys(n);allocation::Counts old_count,new_count;
    {allocation::Scope scope(old_count);std::unordered_map<const void*,std::size_t> old;
     for(std::size_t i=0;i<n;++i)old.emplace(&keys[i],i);
     for(std::size_t i=0;i<n;++i)check(old.find(&keys[i])->second==i,"old allocation lookup");}
    std::size_t capacity_bytes{};
    {allocation::Scope scope(new_count);hua::gc_detail::AddressIndex<> index(n);capacity_bytes=index.capacity_bytes();
     for(std::size_t i=0;i<n;++i)index.emplace(&keys[i],i);
     for(std::size_t i=0;i<n;++i)check(index.find(&keys[i])==i,"new allocation lookup");}
    check(new_count.calls==(n?1:0),"one contiguous allocation per nonempty index");check(new_count.bytes==capacity_bytes,"allocation equals capacity");
    if(n)check(old_count.calls>=n+1,"old node and bucket allocations");
    std::cout<<"{\"entries\":"<<n<<",\"old_allocations\":"<<old_count.calls<<",\"old_requested_bytes\":"<<old_count.bytes<<",\"new_allocations\":"<<new_count.calls<<",\"new_requested_bytes\":"<<new_count.bytes<<"}\n";
}
}
int main(){try{
    boundaries();for(auto n:{0u,1u,2u,4u,17u,127u,257u}){compare<hua::gc_detail::AddressHash>(n);compare<CollisionHash>(n);compare<WrapHash>(n);}
    compare<hua::gc_detail::AddressHash>(65536);
    for(auto n:{0u,2u,16u,257u,90174u,261626u})allocations(n);
    std::cout<<"{\"status\":\"passed\",\"checks\":"<<checks<<",\"oracle\":\"std::unordered_map\",\"forced_collisions_and_wrap\":true,\"allocation_unit\":\"cumulative requested bytes; excludes allocator overhead\"}\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
