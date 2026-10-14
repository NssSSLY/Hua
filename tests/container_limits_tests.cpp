#include "hua/location.hpp"
#include <iostream>
#include <stdexcept>
namespace {
int checks=0,failures=0;
void check(bool result,const char* message){++checks;if(!result){++failures;std::cerr<<"FAIL: "<<message<<'\n';}}
template<class F> void limited(F&& action,const char* message){
    try{action();check(false,message);}catch(const hua::Diagnostic& d){check(d.code()=="E4099",message);}
}
}
int main(){
    using namespace hua;using I=std::int64_t;
    SourceSpan site{"limits.hua",1,1,0,1};
    auto value=make_map("map<int,int>",site);auto& map=std::get<MapValue>(value.data);
    constexpr I limit=1000000;
    for(I i=0;i<limit;++i)map.data->entries.emplace_hint(map.data->entries.end(),MapKey{i},Value(i));
    check(map.data->entries.size()==limit,"filled exact public map limit");
    map_put(map,Value(I{0}),Value(I{7}),site);
    check(as_int(map.data->entries.at(MapKey{I{0}}),site)==7,"literal insertion helper permits existing key at limit");
    limited([&]{map_put(map,Value(limit),Value(I{9}),site);},"literal insertion helper rejects new key at limit");
    auto old=index_location(value,Value(I{0}),site);location_write(old,Value(I{8}));
    check(as_int(map.data->entries.at(MapKey{I{0}}),site)==8,"index assignment permits existing key at limit");
    auto fresh=index_location(value,Value(limit),site);
    limited([&]{location_write(fresh,Value(I{9}),site);},"index assignment rejects new key at limit");
    check(map.data->entries.size()==limit,"failed index assignment preserves size");
    // Capacity rejection is located and does not partially commit earlier targets.
    try{location_write(fresh,Value(I{9}),site);check(false,"located capacity diagnostic");}
    catch(const Diagnostic& d){check(d.code()=="E4099" && d.span().file_id==site.file_id,"located capacity diagnostic");}
    std::vector<ValueLocation> writes{old,fresh};
    limited([&]{prepare_map_writes(writes,site);},"full map positional admission rejects new key");
    check(as_int(map.data->entries.at(MapKey{I{0}}),site)==8,"failed positional admission preserves earlier target");
    map.data->entries.erase(MapKey{limit-1});
    auto other=index_location(value,Value(limit+1),site);
    writes={fresh,other};
    limited([&]{prepare_map_writes(writes,site);},"two distinct new keys reject with one remaining slot");
    check(map.data->entries.size()==limit-1 && !map.data->entries.contains(MapKey{limit}),"batch capacity failure inserts no prefix");
    writes={old,fresh,fresh};prepare_map_writes(writes,site);
    location_write(old,Value(I{10}),site);location_write(fresh,Value(I{11}),site);location_write(fresh,Value(I{12}),site);
    check(map.data->entries.size()==limit && as_int(map.data->entries.at(MapKey{limit}),site)==12,"duplicate target admits one new key and last write wins");
    writes={old,fresh};prepare_map_writes(writes,site);
    check(map.data->entries.size()==limit,"all existing keys allowed at exact limit");
    std::cout<<checks<<" map capacity checks, "<<failures<<" failures\n";
    return failures?1:0;
}
