#pragma once

#include "hua/value.hpp"

namespace hua::random_engine {
using I=std::int64_t;
using U=std::uint64_t;

// Fixed algorithm (the std.random contract): xoshiro256** seeded through SplitMix64.
// Changing any of these steps changes every sequence; a new algorithm needs new names.
inline U rotl(U x,int k){return x<<k|x>>(64-k);}
inline U next(RngState& r){
    auto* s=r.s;const U result=rotl(s[1]*5,7)*9,t=s[1]<<17;
    s[2]^=s[0];s[3]^=s[1];s[1]^=s[2];s[0]^=s[3];s[2]^=t;s[3]=rotl(s[3],45);
    return result;
}
inline void seed(RngState& r,I value){
    U x=static_cast<U>(value);
    for(auto& word:r.s){U z=(x+=0x9e3779b97f4a7c15ull);z=(z^z>>30)*0xbf58476d1ce4e5b9ull;z=(z^z>>27)*0x94d049bb133111ebull;word=z^z>>31;}
}
// Lemire's multiply-shift with rejection: unbiased in [0, span), span >= 1.
inline U bounded(RngState& r,U span){
    auto m=static_cast<unsigned __int128>(next(r))*span;auto low=static_cast<U>(m);
    if(low<span){const U threshold=(U(0)-span)%span;while(low<threshold){m=static_cast<unsigned __int128>(next(r))*span;low=static_cast<U>(m);}}
    return static_cast<U>(m>>64);
}
}
