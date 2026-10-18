#include "native_allocations.hpp"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <new>
namespace {
struct alignas(std::max_align_t) Header {void* base;std::size_t bytes;};
constinit std::atomic<std::uint64_t> calls{},requested{},live{},peak{};
thread_local std::size_t failure=std::numeric_limits<std::size_t>::max();
void* allocate(std::size_t bytes,std::size_t alignment){
    if(failure!=std::numeric_limits<std::size_t>::max()){
        if(failure==0){failure=std::numeric_limits<std::size_t>::max();throw std::bad_alloc();}--failure;
    }
    alignment=std::max(alignment,alignof(Header));const auto payload=std::max(bytes,std::size_t(1));
    const auto max=std::numeric_limits<std::size_t>::max();
    if(alignment>max-sizeof(Header)||payload>max-sizeof(Header)-(alignment-1))throw std::bad_alloc();
    auto base=std::malloc(payload+sizeof(Header)+alignment-1);if(!base)throw std::bad_alloc();
    const auto start=reinterpret_cast<std::uintptr_t>(base)+sizeof(Header);
    const auto address=(start+alignment-1)&~(std::uintptr_t(alignment)-1);
    auto* h=reinterpret_cast<Header*>(address)-1;h->base=base;h->bytes=bytes;
    calls.fetch_add(1,std::memory_order_relaxed);requested.fetch_add(bytes,std::memory_order_relaxed);
    auto current=live.fetch_add(bytes,std::memory_order_relaxed)+bytes;auto old=peak.load(std::memory_order_relaxed);
    while(old<current&&!peak.compare_exchange_weak(old,current,std::memory_order_relaxed)){}
    return reinterpret_cast<void*>(address);
}
void release(void* p)noexcept {if(p){auto* h=static_cast<Header*>(p)-1;live.fetch_sub(h->bytes,std::memory_order_relaxed);std::free(h->base);}}
}
namespace hua::native_allocation {
Snapshot snapshot()noexcept{return {calls.load(),requested.load(),live.load(),peak.load()};}
void fail_after(std::size_t count)noexcept{failure=count;}
void clear_failure()noexcept{failure=std::numeric_limits<std::size_t>::max();}
}
void* operator new(std::size_t n){return allocate(n,alignof(std::max_align_t));}
void* operator new[](std::size_t n){return allocate(n,alignof(std::max_align_t));}
void* operator new(std::size_t n,std::align_val_t a){return allocate(n,static_cast<std::size_t>(a));}
void* operator new[](std::size_t n,std::align_val_t a){return allocate(n,static_cast<std::size_t>(a));}
void operator delete(void* p)noexcept{release(p);}void operator delete[](void* p)noexcept{release(p);}
void operator delete(void* p,std::size_t)noexcept{release(p);}void operator delete[](void* p,std::size_t)noexcept{release(p);}
void operator delete(void* p,std::align_val_t)noexcept{release(p);}void operator delete[](void* p,std::align_val_t)noexcept{release(p);}
void operator delete(void* p,std::size_t,std::align_val_t)noexcept{release(p);}void operator delete[](void* p,std::size_t,std::align_val_t)noexcept{release(p);}
void* operator new(std::size_t n,const std::nothrow_t&)noexcept{try{return ::operator new(n);}catch(...){return nullptr;}}
void* operator new[](std::size_t n,const std::nothrow_t&)noexcept{try{return ::operator new[](n);}catch(...){return nullptr;}}
void* operator new(std::size_t n,std::align_val_t a,const std::nothrow_t&)noexcept{try{return ::operator new(n,a);}catch(...){return nullptr;}}
void* operator new[](std::size_t n,std::align_val_t a,const std::nothrow_t&)noexcept{try{return ::operator new[](n,a);}catch(...){return nullptr;}}
void operator delete(void* p,const std::nothrow_t&)noexcept{release(p);}void operator delete[](void* p,const std::nothrow_t&)noexcept{release(p);}
void operator delete(void* p,std::align_val_t,const std::nothrow_t&)noexcept{release(p);}void operator delete[](void* p,std::align_val_t,const std::nothrow_t&)noexcept{release(p);}
