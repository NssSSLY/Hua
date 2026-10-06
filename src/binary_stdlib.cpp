#include "hua/stdlib.hpp"
#include <bit>
#include <limits>
namespace hua {
namespace {
using I=std::int64_t;
constexpr std::size_t max_bytes=128*1024*1024,max_text=16*1024*1024,max_items=1000000;
Value bytes(std::string text){return Value(BytesValue{managed<const std::string>(std::move(text))});}
std::string integer_bytes(std::uint64_t bits,unsigned count){std::string out;for(unsigned i=0;i<count;++i)out+=static_cast<char>((bits>>(i*8))&255);return out;}
std::string item_type(const std::string& type){auto items=type_arguments(type,"List");return items.size()==1?items[0]:"";}
}
StandardSignature standard_signature(const StandardFunction& f,const std::vector<std::string>& types){
    StandardSignature out{f.parameters,f.result};
    if(f.module=="simd"&&f.name!="backend"&&types.size()==2){auto close=types[0].find(']');auto element=types[0].starts_with('[')&&close!=std::string::npos?types[0].substr(close+1):"";if(!element.empty()){out.parameters={"[]"+element,"[]"+element};out.result="Result<[]"+element+",string>";}return out;}
    if(f.module=="task"&&!types.empty()){
        auto task=type_arguments(types[0],"Task");if(task.size()==1){out.parameters[0]=types[0];if(f.name=="all"||f.name=="race")out.parameters[1]=types[0];
            if(f.name=="timeout")out.result="Result<"+task[0]+",string>";
            if(f.name=="all")out.result="Task<[]"+task[0]+">";
            if(f.name=="race")out.result=types[0];}return out;
    }
    if(f.module!="list"||types.empty())return out;
    if(f.name=="from_slice"){auto end=types[0].find(']');auto element=types[0].starts_with('[')&&end!=std::string::npos?types[0].substr(end+1):"";out.result=element.empty()?"":"List<"+element+">";return out;}
    auto element=item_type(types[0]);out.parameters[0]=element.empty()?"":"List<"+element+">";
    if(f.name=="append")out.parameters[1]=element;
    if(f.name=="set")out.parameters[2]=element;
    if(f.name=="extend")out.parameters[1]="[]"+element;
    if(f.name=="get"||f.name=="pop")out.result="Result<"+element+",string>";
    if(f.name=="snapshot")out.result="[]"+element;
    return out;
}
Value invoke_binary_standard(const StandardFunction& f,const std::vector<Value>& args,const SourceSpan& s){
    std::vector<std::string> types;for(const auto& v:args)types.push_back(value_type(v));auto signature=standard_signature(f,types);
    auto result=type_arguments(signature.result,"Result");auto success=result.empty()?"":result[0];
    auto ok=[&](Value v){return standard_result(std::move(v),true,success);};
    auto err=[&](const std::string& e){return standard_result(Value(e),false,success);};
    auto integer=[&](std::size_t i){return as_int(args[i],s);};
    if(f.module=="list"){
        if(f.name=="from_slice"){
            auto source=std::get_if<SliceValue>(&args[0].data);if(!source||source->element_type.empty())runtime_error(s,"from_slice requires a typed slice/array","E4003");
            if(source->length>max_items)runtime_error(s,"list item limit exceeded","E4099");
            auto data=managed<ListData>();data->element_type=source->element_type;data->values.reserve(source->length);
            for(std::size_t i=0;i<source->length;++i)data->values.push_back(copy_value(sequence_read(*source,i),true));
            return Value(ListValue{data,true});
        }
        auto handle=std::get_if<ListValue>(&args[0].data);if(!handle||!handle->data)runtime_error(s,"expected List<T>","E4003");auto& data=*handle->data;
        if(f.name=="len")return Value(static_cast<I>(data.values.size()));
        if(f.name=="snapshot"){auto values=managed<std::vector<Value>>();values->reserve(data.values.size());for(const auto& v:data.values)values->push_back(copy_value(read_only(v)));return Value(SliceValue{values,0,values->size(),false,data.element_type});}
        if(f.name=="get"||f.name=="set"){auto at=integer(1);if(at<0||static_cast<std::size_t>(at)>=data.values.size())return err("list index out of bounds");if(f.name=="get")return ok(copy_value(read_only(data.values[static_cast<std::size_t>(at)])));auto value=copy_value(enforce_type(args[2],data.element_type,s));data.values[static_cast<std::size_t>(at)]=std::move(value);return ok(Value(true));}
        if(f.name=="clear"){data.values.clear();return {};}
        if(f.name=="reserve"){auto n=integer(1);if(n<0||n>static_cast<I>(max_items))return err("list capacity outside 0..1000000");auto capacity=static_cast<std::size_t>(n);bool changed=capacity>data.values.capacity();if(changed)data.values.reserve(capacity);return ok(Value(changed));}
        if(f.name=="pop"){if(data.values.empty())return err("cannot pop an empty list");auto value=copy_value(read_only(data.values.back()));data.values.pop_back();return ok(std::move(value));}
        if(f.name=="append"){if(data.values.size()>=max_items)return err("list item limit exceeded");auto value=copy_value(enforce_type(args[1],data.element_type,s));data.values.push_back(std::move(value));return ok(Value(static_cast<I>(data.values.size())));}
        if(f.name=="extend"){auto source=std::get_if<SliceValue>(&args[1].data);if(!source)runtime_error(s,"extend requires a slice/array","E4003");if(source->length>max_items-data.values.size())return err("list item limit exceeded");std::vector<Value> extra;extra.reserve(source->length);for(std::size_t i=0;i<source->length;++i)extra.push_back(copy_value(enforce_type(sequence_read(*source,i),data.element_type,s)));data.values.insert(data.values.end(),extra.begin(),extra.end());return ok(Value(static_cast<I>(data.values.size())));}
    }
    if(f.module=="buffer"){
        if(f.name=="new")return Value(BufferValue{managed<std::string>(),true});
        auto handle=std::get_if<BufferValue>(&args[0].data);if(!handle||!handle->data)runtime_error(s,"expected Buffer","E4003");auto& out=*handle->data;
        if(f.name=="len")return Value(static_cast<I>(out.size()));
        if(f.name=="bytes")return bytes(out);
        if(f.name=="text"){if(out.size()>max_text)return err("text exceeds 16 MiB");auto invalid=text_utf8_error(out);return invalid.empty()?ok(Value(out)):err(invalid);}
        if(f.name=="clear"){out.clear();return {};}
        std::string addition;
        if(f.name=="append")addition=*std::get<BytesValue>(args[1].data).data;
        else if(f.name=="write_text"){addition=std::get<std::string>(args[1].data);if(addition.size()>max_text)return err("text exceeds 16 MiB");auto invalid=text_utf8_error(addition);if(!invalid.empty())return err(invalid);}
        else {auto value=integer(1);if(f.name=="write_u8"){if(value<0||value>255)return err("byte outside 0..255");addition=integer_bytes(static_cast<std::uint64_t>(value),1);}else if(f.name=="write_u32_le"){if(value<0||value>4294967295LL)return err("u32 outside 0..4294967295");addition=integer_bytes(static_cast<std::uint64_t>(value),4);}else addition=integer_bytes(std::bit_cast<std::uint64_t>(value),8);}
        if(addition.size()>max_bytes-out.size())return err("buffer exceeds 128 MiB");out+=addition;return ok(Value(static_cast<I>(out.size())));
    }
    if(f.module=="bytes"){
        if(f.name=="from_text"){const auto& text=std::get<std::string>(args[0].data);if(text.size()>max_text)runtime_error(s,"text exceeds 16 MiB","E4099");auto invalid=text_utf8_error(text);if(!invalid.empty())runtime_error(s,invalid,"E4003");return bytes(text);}
        if(f.name=="from_ints"){const auto& source=std::get<SliceValue>(args[0].data);if(source.length>max_items)return err("byte input exceeds 1000000 items");std::string out;out.reserve(source.length);for(std::size_t i=0;i<source.length;++i){auto value=as_int(sequence_read(source,i),s);if(value<0||value>255)return err("byte outside 0..255");out+=static_cast<char>(value);}return ok(bytes(std::move(out)));}
        if(f.name=="u32_le"){auto value=integer(0);if(value<0||value>4294967295LL)return err("u32 outside 0..4294967295");return ok(bytes(integer_bytes(static_cast<std::uint64_t>(value),4)));}
        if(f.name=="i64_le")return bytes(integer_bytes(std::bit_cast<std::uint64_t>(integer(0)),8));
        auto handle=std::get_if<BytesValue>(&args[0].data);if(!handle||!handle->data)runtime_error(s,"expected Bytes","E4003");const auto& data=*handle->data;
        if(f.name=="len")return Value(static_cast<I>(data.size()));
        if(f.name=="to_text"){if(data.size()>max_text)return err("text exceeds 16 MiB");auto invalid=text_utf8_error(data);return invalid.empty()?ok(Value(data)):err(invalid);}
        if(f.name=="to_ints"){if(data.size()>max_items)return err("byte output exceeds 1000000 items");auto values=managed<std::vector<Value>>();values->reserve(data.size());for(unsigned char c:data)values->emplace_back(static_cast<I>(c));return ok(Value(SliceValue{values,0,values->size(),false,"int"}));}
        if(f.name=="concat"){const auto& other=*std::get<BytesValue>(args[1].data).data;if(other.size()>max_bytes-data.size())return err("bytes exceed 128 MiB");return ok(bytes(data+other));}
        if(f.name=="crc32"){std::uint32_t crc=0xffffffffu;for(unsigned char c:data){crc^=c;for(unsigned k=0;k<8;++k)crc=(crc>>1)^(0xedb88320u&static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc&1)));}return Value(static_cast<I>(~crc));}
        auto at=integer(1);auto count=f.name=="read_u32_le"?4u:8u;
        if(f.name=="slice"){auto end=integer(2);if(at<0||end<at||static_cast<std::size_t>(end)>data.size())return err("byte slice bounds invalid");return ok(bytes(data.substr(static_cast<std::size_t>(at),static_cast<std::size_t>(end-at))));}
        if(at<0||static_cast<std::size_t>(at)>=data.size())return err("byte offset out of bounds");
        if(f.name=="at")return ok(Value(static_cast<I>(static_cast<unsigned char>(data[static_cast<std::size_t>(at)]))));
        if(count>data.size()-static_cast<std::size_t>(at))return err("integer read exceeds byte bounds");std::uint64_t bits=0;for(unsigned i=0;i<count;++i)bits|=std::uint64_t(static_cast<unsigned char>(data[static_cast<std::size_t>(at)+i]))<<(8*i);
        return ok(Value(f.name=="read_u32_le"?static_cast<I>(bits):std::bit_cast<I>(bits)));
    }
    runtime_error(s,"unknown binary/list standard function","E4003");
}
}
