#pragma once
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>
namespace hua::gc_detail {
struct AddressHash {
    std::size_t operator()(const void* pointer) const noexcept {
        auto bits=std::uint64_t(reinterpret_cast<std::uintptr_t>(pointer));
        bits=(bits^(bits>>30))*UINT64_C(0xbf58476d1ce4e5b9);
        bits=(bits^(bits>>27))*UINT64_C(0x94d049bb133111eb);
        return static_cast<std::size_t>(bits^(bits>>31));
    }
};
struct IndexProbes {
    std::size_t build{},lookup{},lookup_calls{},null_lookups{},max_build{},max_lookup{};
};
struct NoIndexProbes {};
// Collection-local, non-owning keys. Fixed capacity leaves at least half empty;
// no erase, rehash, or cross-collection address retention. First insertion wins.
template<class Hash=AddressHash,bool Profile=false> class AddressIndex {
    struct Slot {const void* key{};std::size_t value{};};
    std::vector<Slot> slots_;
    std::size_t size_{};
    [[no_unique_address]] Hash hash_;
    [[no_unique_address]] mutable std::conditional_t<Profile,IndexProbes,NoIndexProbes> probes_;
public:
    static constexpr auto missing=std::numeric_limits<std::size_t>::max();
    static std::size_t capacity_for(std::size_t expected,std::size_t max_slots){
        if(!expected)return 0;
        const auto largest=std::bit_floor(max_slots);
        if(largest<8||expected>largest/2)throw std::length_error("GC address index capacity overflow");
        return std::bit_ceil(std::max(std::size_t(8),expected*2));
    }
    explicit AddressIndex(std::size_t expected,Hash hash={}):hash_(hash){slots_.resize(capacity_for(expected,slots_.max_size()));}
    bool emplace(const void* key,std::size_t value){
        if(!key)throw std::invalid_argument("GC address index requires a pinned nonnull object");
        if(slots_.empty())throw std::length_error("GC address index has no capacity");
        auto position=hash_(key)&(slots_.size()-1);std::size_t steps=0;
        while(slots_[position].key){
            if constexpr(Profile)++steps;
            if(slots_[position].key==key){
                if constexpr(Profile){probes_.build+=steps;probes_.max_build=std::max(probes_.max_build,steps);}return false;
            }
            position=(position+1)&(slots_.size()-1);
        }
        if(size_>=slots_.size()/2)throw std::length_error("GC address index load bound exceeded");
        if constexpr(Profile){++steps;probes_.build+=steps;probes_.max_build=std::max(probes_.max_build,steps);}
        slots_[position]={key,value};++size_;return true;
    }
    std::size_t find(const void* key)const noexcept {
        if constexpr(Profile){++probes_.lookup_calls;if(!key)++probes_.null_lookups;}
        if(!key||slots_.empty())return missing;
        auto position=hash_(key)&(slots_.size()-1);std::size_t steps=0;
        for(;;){
            if constexpr(Profile)++steps;
            const auto& slot=slots_[position];
            if(!slot.key||slot.key==key){
                if constexpr(Profile){probes_.lookup+=steps;probes_.max_lookup=std::max(probes_.max_lookup,steps);}
                return slot.key?slot.value:missing;
            }
            position=(position+1)&(slots_.size()-1);
        }
    }
    std::size_t size()const{return size_;}
    std::size_t capacity()const{return slots_.size();}
    std::size_t capacity_bytes()const{return slots_.capacity()*sizeof(Slot);}
    const IndexProbes& statistics()const requires(Profile){return probes_;}
};
}
