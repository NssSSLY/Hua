#pragma once
#include "hua/value.hpp"
namespace hua {
struct JsonData {
    using Pointer = std::shared_ptr<const JsonData>;
    using Array = std::vector<Pointer>;
    using Object = std::map<std::string, Pointer>;
    std::variant<std::monostate,bool,std::int64_t,double,std::string,Array,Object> data;
    // Levels below this node. Built values stop at the nesting limit, which also bounds
    // the recursion of releasing a tree that was constructed one level at a time.
    std::size_t height{};
    template<class T> explicit JsonData(T value): data(std::move(value)) {
        if(auto a=std::get_if<Array>(&data)){for(const auto& p:*a)height=std::max(height,p->height+1);}
        else if(auto o=std::get_if<Object>(&data)){for(const auto& [_,p]:*o)height=std::max(height,p->height+1);}
    }
};
Value json_decode(const std::string& text);
Value json_encode(const Value& value);
Value json_access(const std::string& operation,const std::vector<Value>& arguments);
std::string json_display(const JsonValue& value);
Value json_construct(const std::string& operation,const std::vector<Value>& arguments,const SourceSpan& span);
}
