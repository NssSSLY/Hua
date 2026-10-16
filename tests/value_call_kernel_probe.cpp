// BO-04 diagnostics only: production Value helpers and scalar-only layout controls.
#include "hua/value.hpp"
#include "hua/gc.hpp"
#include "hua/performance.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
using I=std::int64_t;using Clock=std::chrono::steady_clock;
template<class T>void opaque(T& value){asm volatile("" : : "g"(&value) : "memory");}
std::string key(int n){return "f"+std::to_string(1000+n);}
hua::Value record(int width,int depth){
    auto data=hua::managed<hua::StructData>();data->name="Record"+std::to_string(depth);
    for(int i=0;i<width;++i)data->fields.emplace(key(i),i==0&&depth>1?record(width,depth-1):hua::Value(I(17+i)));
    return hua::Value(hua::StructValue{data,true});
}
hua::Value fixture(const std::string& kind,int width,int depth,int results){
    if(kind=="list"){
        auto data=hua::managed<hua::ListData>();data->element_type="int";
        for(int i=0;i<width;++i)data->values.emplace_back(I(17+i));return hua::Value(hua::ListValue{data,true});
    }
    auto value=record(width,depth);std::string type="Record"+std::to_string(depth);
    for(int i=0;i<results;++i){value=hua::Value(hua::ResultValue{true,hua::managed<hua::Value>(std::move(value)),type,"string",true,false});type="Result<"+type+",string>";}
    return value;
}
I leaf(const hua::Value& value){
    if(auto result=std::get_if<hua::ResultValue>(&value.data))return leaf(*result->payload);
    if(auto structure=std::get_if<hua::StructValue>(&value.data))return leaf(structure->data->fields.at(key(0)));
    if(auto list=std::get_if<hua::ListValue>(&value.data))return std::get<I>(list->data->values.front().data);
    return std::get<I>(value.data);
}
struct Sample{std::uint64_t ns{},allocated{},collections{};I checksum{};};
Sample batch(const std::string& kind,int width,int depth,int results,int rounds,const std::string& op,bool emit){
    hua::ManagedHeap heap(65536);hua::HeapScope heap_scope(heap);auto source=fixture(kind,width,depth,results);
    auto type=hua::value_type(source);std::vector<std::string> returns{type};
    std::vector<hua::Value> unique;
    if(op=="move-unique")for(int i=0;i<rounds;++i)unique.push_back(hua::copy_value(source));
    std::vector<hua::Value> slots;std::vector<I> integers;
    if(op=="slots"||op=="cpp-vector")for(int i=0;i<width;++i){slots.emplace_back(I(17+i));integers.emplace_back(17+i);}
    I checksum=0;auto before=heap.allocated(),collected=heap.collected();
#ifdef HUA_INTERNAL_PROFILE
    hua::performance::Counters counters;hua::performance::Scope profile(counters);
#endif
    auto start=Clock::now();
    for(int i=0;i<rounds;++i){
        if(op=="read"){opaque(source);checksum+=leaf(source);continue;}
        if(op=="slots"){auto value=slots;opaque(value);checksum+=std::get<I>(value[0].data);continue;}
        if(op=="cpp-vector"){auto value=integers;opaque(value);checksum+=value[0];continue;}
        hua::Value value;
        if(op=="copy")value=hua::copy_value(static_cast<const hua::Value&>(source));
        else if(op=="move-unique")value=hua::copy_value(std::move(unique[i]));
        else if(op=="move-shared"){auto alias=source;value=hua::copy_value(std::move(alias));}
        else if(op=="enforce")value=hua::enforce_type(source,type,{});
        else if(op=="borrow")value=hua::enforce_borrow(source,type,{});
        else if(op=="argument")value=hua::read_only(hua::enforce_type(hua::copy_value(source),type,{}));
        else if(op=="return"){
            auto loaded=source;value=hua::copy_value(std::move(loaded));value=hua::enforce_return(std::move(value),returns,{});
        }else if(op=="roundtrip"){
            auto bound=hua::read_only(hua::enforce_type(hua::copy_value(source),type,{}));
            auto loaded=bound;value=hua::copy_value(std::move(loaded));value=hua::enforce_return(std::move(value),returns,{});
        }else throw std::runtime_error("unknown operation");
        opaque(value);checksum+=leaf(value);
    }
    auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
    Sample sample{std::uint64_t(ns),heap.allocated()-before,heap.collected()-collected,checksum};
    if(emit){
        std::cout<<"{\"kind\":\""<<kind<<"\",\"width\":"<<width<<",\"depth\":"<<depth<<",\"results\":"<<results
            <<",\"operation\":\""<<op<<"\",\"operations\":"<<rounds<<",\"ns\":"<<sample.ns<<",\"ns_per_operation\":"<<double(ns)/rounds
            <<",\"managed_allocations\":"<<sample.allocated<<",\"collected_objects\":"<<sample.collections<<",\"checksum\":"<<checksum<<",\"counts\":";
#ifdef HUA_INTERNAL_PROFILE
        hua::performance::write(std::cout,counters);
#else
        std::cout<<"null";
#endif
        std::cout<<"}\n";
    }
    return sample;
}
int preflight(){
    hua::ManagedHeap heap(65536);hua::HeapScope scope(heap);unsigned checks=0;
    auto require=[&](bool value){++checks;if(!value)throw std::runtime_error("value preflight "+std::to_string(checks));};
    auto source=record(8,2),copy=hua::copy_value(source);
    auto& root=std::get<hua::StructValue>(source.data);auto& duplicate=std::get<hua::StructValue>(copy.data);
    require(root.data!=duplicate.data);auto child=std::get<hua::StructValue>(duplicate.data->fields.at(key(0)).data);
    child.data->fields.at(key(0))=hua::Value(I(99));require(leaf(source)==17&&leaf(copy)==99);
    auto unique=hua::copy_value(source);const auto* address=std::get<hua::StructValue>(unique.data).data.get();
    auto moved=hua::copy_value(std::move(unique));require(std::get<hua::StructValue>(moved.data).data.get()==address);
    auto shared=source;auto copied=hua::copy_value(std::move(shared));require(std::get<hua::StructValue>(copied.data).data!=root.data);
    auto list=fixture("list",64,1,0);auto view=hua::copy_value(hua::read_only(list));
    require(std::get<hua::ListValue>(view.data).data==std::get<hua::ListValue>(list.data).data);
    require(!std::get<hua::ListValue>(view.data).writable);
    for(int depth:{1,2,4}){
        auto result=fixture("struct",8,1,depth);auto isolated=hua::copy_value(hua::read_only(result));
        require(leaf(isolated)==17);require(std::get<hua::ResultValue>(isolated.data).payload!=std::get<hua::ResultValue>(result.data).payload);
        auto enforced=hua::enforce_type(result,hua::value_type(result),{});require(leaf(enforced)==17);
    }
    std::vector<hua::Value> slots{hua::Value(I(17)),hua::Value(I(18))};auto scalar_copy=slots;scalar_copy[0]=hua::Value(I(99));require(std::get<I>(slots[0].data)==17);
    std::cout<<"{\"status\":\"passed\",\"checks\":"<<checks<<"}\n";return 0;
}
int main(int argc,char** argv){try{
    if(argc==2&&std::string(argv[1])=="--preflight")return preflight();
    if(argc!=6)return 2;std::string kind=argv[1];int width=std::stoi(argv[2]),depth=std::stoi(argv[3]),results=std::stoi(argv[4]),rounds=std::stoi(argv[5]);
    if((kind!="struct"&&kind!="list")||width<1||width>256||depth<1||depth>4||results<0||results>4||rounds<1||rounds>2048)return 2;
    std::vector<std::string> operations={"read","copy","enforce","argument","return","roundtrip","borrow"};
    if(kind=="struct"){operations.push_back("move-unique");operations.push_back("move-shared");if(depth==1&&results==0){operations.push_back("slots");operations.push_back("cpp-vector");}}
    for(const auto& op:operations){
#ifndef HUA_INTERNAL_PROFILE
        // Timing batches are independent heaps: setup/unique fixtures outside the timer,
        // bounded aggregate duration, no GC interval change or discarded failing retry.
        std::uint64_t total_ns=0,ops=0,allocated=0;I checksum=0;unsigned batches=0;
        do {auto row=batch(kind,width,depth,results,rounds,op,false);total_ns+=row.ns;allocated+=row.allocated;ops+=rounds;checksum+=row.checksum;++batches;}
        while(total_ns<20000000&&batches<128);
        std::cout<<"{\"kind\":\""<<kind<<"\",\"width\":"<<width<<",\"depth\":"<<depth<<",\"results\":"<<results<<",\"operation\":\""<<op
            <<"\",\"operations\":"<<ops<<",\"batches\":"<<batches<<",\"ns\":"<<total_ns<<",\"ns_per_operation\":"<<double(total_ns)/ops
            <<",\"managed_allocations\":"<<allocated<<",\"checksum\":"<<checksum<<",\"counts\":null}\n";
#else
        batch(kind,width,depth,results,rounds,op,true);
#endif
    }return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
