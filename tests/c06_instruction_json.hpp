// Test reference projection of already compiled host instructions; no Hua generation.
#pragma once
#include "hua/json.hpp"
#include "hua/bytecode.hpp"
#include <charconv>
#include <cmath>
#include <ostream>
#include <map>
static std::string quoted(const std::string& s) {
    auto v=hua::json_encode(hua::Value(s));auto r=std::get<hua::ResultValue>(v.data);
    if(!r.ok)throw std::runtime_error("cannot encode instruction text");
    return hua::string_value(r.payload->data);
}
static std::string scalar_text(const hua::Value& v) {
    switch(hua::value_kind(v)){
    case 0:return "";case 1:return std::get<bool>(v.data)?"true":"false";
    case 2:return std::to_string(std::get<std::int64_t>(v.data));
    case 3:{char b[64];auto [end,e]=std::to_chars(b,b+64,std::get<double>(v.data),std::chars_format::general,17);if(e!=std::errc{})throw std::runtime_error("float projection");return {b,end};}
    case 4:return hua::string_value(v.data);
    default:throw std::runtime_error("unexpected reference constant");
    }
}
static void instruction_code_json(std::ostream& out,const hua::Code& code) {
    out<<"{\"name\":"<<quoted(code.name)<<",\"instructions\":[";
    bool first=true;for(const auto& op:code.instructions){
        if(!first)out<<',';first=false;
        out<<"{\"op\":"<<static_cast<unsigned>(op.op)<<",\"module\":"<<quoted(op.span.file_id)
           <<",\"start\":"<<op.span.start_offset<<",\"end\":"<<op.span.end_offset<<",\"line\":"<<op.span.line<<",\"column\":"<<op.span.column
           <<",\"text\":"<<quoted(op.text)<<",\"type_name\":"<<quoted(op.type)<<",\"argument\":"<<op.argument<<",\"target\":"<<op.target
           <<",\"flag\":"<<(op.flag?"true":"false")<<",\"constant\":{\"kind\":"<<hua::value_kind(op.constant)<<",\"text\":"<<quoted(scalar_text(op.constant))<<"},\"names\":[";
        bool sep=false;for(const auto& n:op.names){if(sep)out<<',';sep=true;out<<quoted(n);}
        out<<"],\"contextual\":[";sep=false;for(bool b:op.contextual){if(sep)out<<',';sep=true;out<<(b?"true":"false");}out<<"]}";
    }out<<"]}";
}
static void instruction_image_json(std::ostream& out,const hua::Bytecode& image){
    out<<"{\"initializer\":";instruction_code_json(out,image.initializer);out<<",\"functions\":[";
    std::map<std::string,const hua::Code*> functions;for(const auto& [_,c]:image.functions)functions[c.name]=&c;
    bool sep=false;for(const auto& [_,c]:functions){if(sep)out<<',';sep=true;instruction_code_json(out,*c);}out<<"]}\n";
}
