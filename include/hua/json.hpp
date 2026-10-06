#pragma once
#include "hua/value.hpp"
namespace hua {
struct JsonData {
    using Pointer = std::shared_ptr<const JsonData>;
    using Array = std::vector<Pointer>;
    using Object = std::map<std::string, Pointer>;
    std::variant<std::monostate,bool,std::int64_t,double,std::string,Array,Object> data;
    template<class T> explicit JsonData(T value): data(std::move(value)) {}
};
Value json_decode(const std::string& text);
Value json_encode(const Value& value);
Value json_access(const std::string& operation,const std::vector<Value>& arguments);
std::string json_display(const JsonValue& value);
}
