#include "hua/stdlib.hpp"
#include <algorithm>
namespace hua {namespace {
constexpr std::size_t max_depth=16,max_contexts=16,max_message=4096,max_context=512,max_origin=1024;
std::string clipped(std::string_view text,std::size_t maximum,bool& truncated){
    if(text.size()<=maximum)return std::string(text);
    truncated=true;auto end=maximum;
    while(end&&(static_cast<unsigned char>(text[end])&192)==128)--end;
    return std::string(text.substr(0,end));
}
bool identity(const std::string& text){
    if(text.empty()||text.size()>128)return false;
    for(unsigned char c:text)if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return false;
    return true;
}
std::shared_ptr<const ErrorData> shortened(const std::shared_ptr<const ErrorData>& source,std::size_t depth,bool& truncated){
    if(!source)return {};
    if(!depth){truncated=true;return {};}
    auto data=*source;data.cause=shortened(source->cause,depth-1,truncated);
    data.truncated=data.truncated||truncated;
    return managed<const ErrorData>(std::move(data));
}
Value error(ErrorData data){return Value(ErrorValue{managed<const ErrorData>(std::move(data))});}
}
Value invoke_error_standard(const StandardFunction& f,const std::vector<Value>& args,const SourceSpan& span){
    auto text=[&](std::size_t i)->const std::string&{return std::get<std::string>(args[i].data);};
    auto node=[&](std::size_t i)->const ErrorData&{auto p=std::get_if<ErrorValue>(&args[i].data);if(!p||!p->data)runtime_error(span,"expected std.error.Value","E4003");return *p->data;};
    for(const auto& value:args)if(auto text=std::get_if<std::string>(&value.data);text&&!text_utf8_error(*text).empty()){if(f.name=="make"||f.name=="wrap")return standard_result(Value(std::string("ERROR_TEXT: valid UTF-8 required")),false,"std.error.Value");runtime_error(span,"error text requires valid UTF-8","E4003");}
    if(f.name=="make"||f.name=="wrap"){
        if(!identity(text(0))||!identity(text(1)))return standard_result(Value(std::string("ERROR_IDENTITY: domain/code require 1..128 lowercase ASCII identifier bytes")),false,"std.error.Value");
        ErrorData data;data.domain=text(0);data.code=text(1);
        data.message=clipped(text(2),max_message,data.truncated);
        if(f.name=="wrap"){node(3);data.cause=shortened(std::get<ErrorValue>(args[3].data).data,max_depth-1,data.truncated);}
        return standard_result(error(std::move(data)),true,"std.error.Value");
    }
    if(f.name=="from_string"){
        ErrorData data;data.domain="legacy";data.code="failure";
        data.message=clipped(text(0),max_message,data.truncated);data.origin=clipped(text(1),max_origin,data.truncated);return error(std::move(data));
    }
    const auto& data=node(0);
    if(f.name=="domain")return Value(data.domain);
    if(f.name=="code")return Value(data.code);
    if(f.name=="message")return Value(data.message);
    if(f.name=="cause")return data.cause?Value(ErrorValue{data.cause}):Value{};
    if(f.name=="origin")return data.origin.empty()?Value{}:Value(data.origin);
    if(f.name=="native_domain")return data.native_domain.empty()?Value{}:Value(data.native_domain);
    if(f.name=="native_code")return data.native_code?Value(*data.native_code):Value{};
    if(f.name=="truncated")return Value(data.truncated);
    if(f.name=="contexts"){auto values=managed<std::vector<Value>>();for(const auto& value:data.contexts)values->emplace_back(value);return Value(SliceValue{values,0,values->size(),false,"string"});}
    if(f.name=="with_context"||f.name=="with_origin"||f.name=="with_native"){
        auto copy=data;
        if(f.name=="with_context"){if(copy.contexts.size()==max_contexts)copy.truncated=true;else copy.contexts.push_back(clipped(text(1),max_context,copy.truncated));}
        else if(f.name=="with_origin")copy.origin=clipped(text(1),max_origin,copy.truncated);
        else{copy.native_domain=clipped(text(1),128,copy.truncated);copy.native_code=as_int(args[2],span);}
        return error(std::move(copy));
    }
    if(f.name=="format"){
        std::string out;auto current=&data;
        for(std::size_t depth=0;current&&depth<max_depth;++depth){
            if(depth)out+="\ncaused by: ";out+=current->domain+"."+current->code+": "+current->message;
            if(!current->origin.empty())out+=" ["+current->origin+"]";
            if(current->native_code)out+=" ["+current->native_domain+":"+std::to_string(*current->native_code)+"]";
            for(const auto& context:current->contexts)out+="\ncontext: "+context;
            if(current->truncated)out+="\n[truncated]";
            current=current->cause.get();
        }
        return Value(std::move(out));
    }
    runtime_error(span,"unknown error operation","E4003");
}
}
