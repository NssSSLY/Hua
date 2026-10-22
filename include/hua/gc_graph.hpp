#pragma once
#include <cstddef>
#include <span>
#include <vector>
namespace hua::gc_detail {
// One collection, one pass. Offsets preserve vertex order and targets preserve
// callback order, including duplicate owning references. No managed ownership.
struct EdgeTable {
    std::vector<std::size_t> offsets,targets;
    EdgeTable()=default;
    explicit EdgeTable(std::size_t vertices):offsets(vertices+1){}
    void reset(std::size_t vertices){offsets.assign(vertices+1,0);targets.clear();}
    void begin_vertex(std::size_t vertex){offsets[vertex]=targets.size();}
    void finish(){offsets.back()=targets.size();}
    std::span<const std::size_t> outgoing(std::size_t vertex) const {
        const auto begin=offsets[vertex],end=offsets[vertex+1];
        if(begin==end)return {};
        return {targets.data()+begin,end-begin};
    }
};
}
