#include "hua/stdlib.hpp"
#include "hua/tasks.hpp"
#include "hua/simd.hpp"
#include "hua/json.hpp"
#include <algorithm>
#include <fstream>
#include <istream>
#include <ostream>
namespace hua {
namespace {
using I=std::int64_t;
constexpr std::size_t max_text=16*1024*1024,max_items=1000000;
std::string utf8(const std::filesystem::path& p){auto s=p.u8string();return {s.begin(),s.end()};}
Value strings(const std::vector<std::string>& values){auto data=managed<std::vector<Value>>();for(const auto& x:values)data->emplace_back(x);return Value(SliceValue{data,0,data->size(),false,"string"});}
std::filesystem::path path(const std::string& text,RuntimeContext* context){if(text.find('\0')!=std::string::npos)throw std::runtime_error("path contains NUL");auto p=std::filesystem::path(std::u8string(text.begin(),text.end()));if(p.is_relative())p=(context&&!context->working_directory.empty()?context->working_directory:std::filesystem::current_path())/p;return p;}
void bounded(const std::string& text,const SourceSpan& s){if(text.size()>max_text)runtime_error(s,"standard library text exceeds 16 MiB","E4099");}
}
const std::vector<StandardFunction>& standard_functions() {
    static const std::vector<StandardFunction> functions={
        {"error","make",{"string","string","string"},"Result<std.error.Value,string>",false,false,7},
        {"error","wrap",{"string","string","string","std.error.Value"},"Result<std.error.Value,string>",false,false,7},
        {"error","from_string",{"string","string"},"std.error.Value",false,false,7},
        {"error","with_context",{"std.error.Value","string"},"std.error.Value",false,false,7},
        {"error","with_origin",{"std.error.Value","string"},"std.error.Value",false,false,7},
        {"error","with_native",{"std.error.Value","string","int"},"std.error.Value",false,false,7},
        {"error","domain",{"std.error.Value"},"string",false,false,7},
        {"error","code",{"std.error.Value"},"string",false,false,7},
        {"error","message",{"std.error.Value"},"string",false,false,7},
        {"error","cause",{"std.error.Value"},"std.error.Value?",false,false,7},
        {"error","origin",{"std.error.Value"},"string?",false,false,7},
        {"error","native_domain",{"std.error.Value"},"string?",false,false,7},
        {"error","native_code",{"std.error.Value"},"int?",false,false,7},
        {"error","contexts",{"std.error.Value"},"[]string",false,false,7},
        {"error","truncated",{"std.error.Value"},"bool",false,false,7},
        {"error","format",{"std.error.Value"},"string",false,false,7},
        {"path","join",{"[]string"},"Result<string,string>",false,false,6},
        {"path","normalize",{"string"},"Result<string,string>",false,false,6},
        {"path","basename",{"string"},"Result<string,string>",false,false,6},
        {"path","dirname",{"string"},"Result<string,string>",false,false,6},
        {"path","ext",{"string"},"Result<string,string>",false,false,6},
        {"path","relative",{"string","string"},"Result<string,string>",false,false,6},
        {"fs","stat",{"string"},"Result<Json,string>",false,false,6},
        {"fs","rename",{"string","string"},"Result<bool,string>",false,false,6},
        {"fs","replace",{"string","string"},"Result<bool,string>",false,false,6},
        {"fs","temp_file",{"string","string"},"Result<string,string>",false,false,6},
        {"math","floor",{"float"},"Result<float,string>",false,false,6},
        {"math","ceil",{"float"},"Result<float,string>",false,false,6},
        {"math","trunc",{"float"},"Result<float,string>",false,false,6},
        {"math","round",{"float"},"Result<float,string>",false,false,6},
        {"math","sin",{"float"},"Result<float,string>",false,false,6},
        {"math","cos",{"float"},"Result<float,string>",false,false,6},
        {"math","tan",{"float"},"Result<float,string>",false,false,6},
        {"math","exp",{"float"},"Result<float,string>",false,false,6},
        {"math","log",{"float"},"Result<float,string>",false,false,6},
        {"math","log2",{"float"},"Result<float,string>",false,false,6},
        {"math","log10",{"float"},"Result<float,string>",false,false,6},
        {"math","hypot",{"float","float"},"Result<float,string>",false,false,6},
        {"alg","sorted",{""},"",false,false,6},
        {"alg","reverse",{""},"",false,false,6},
        {"alg","contains",{"",""},"",false,false,6},
        {"alg","index",{"",""},"",false,false,6},
        {"alg","lower_bound",{"",""},"",false,false,6},
        {"alg","upper_bound",{"",""},"",false,false,6},
        {"alg","binary_search",{"",""},"",false,false,6},
        {"alg","sum",{""},"",false,false,6},
        {"alg","min",{""},"",false,false,6},
        {"alg","max",{""},"",false,false,6},
        {"time","monotonic_ns",{},"int",false,false,6},
        {"time","unix_ms",{},"int",false,false,6},
        {"os","getenv",{"string"},"Result<string?,string>",false,false,6},
        {"simd","backend",{},"string",false,false,6},
        {"simd","add",{"",""},"",false,true,6},
        {"simd","sub",{"",""},"",false,true,6},
        {"simd","mul",{"",""},"",false,true,6},
        {"gc","collect",{},"int",false,false,6},
        {"gc","live",{},"int",false,false,6},
        {"gc","allocated",{},"int",false,false,6},
        {"gc","collected",{},"int",false,false,6},
        {"task","sleep",{"int"},"void",false,false,6},
        {"task","yield",{},"void",false,false,6},
        {"task","worker_id",{},"string",false,false,6},
        {"task","cancel",{""},"bool",false,false,6},
        {"task","done",{""},"bool",false,false,6},
        {"task","state",{""},"string",false,false,6},
        {"task","timeout",{"","int"},"",false,true,6},
        {"task","all",{"",""},"",false,true,6},
        {"task","race",{"",""},"",false,true,6},
        {"os","args",{},"[]string"},
        {"os","cwd",{},"Result<string,string>"},
        {"io","read_line",{},"Result<string?,string>"},
        {"io","read_all",{},"Result<string,string>"},
        {"io","write",{"string"},"Result<int,string>"},
        {"fs","read_text",{"string"},"Result<string,string>"},
        {"fs","write_text",{"string","string"},"Result<bool,string>"},
        {"fs","append_text",{"string","string"},"Result<bool,string>"},
        {"fs","exists",{"string"},"Result<bool,string>"},
        {"fs","remove",{"string"},"Result<bool,string>"},
        {"fs","mkdir_all",{"string"},"Result<bool,string>"},
        {"fs","list_dir",{"string"},"Result<[]string,string>"},
        {"strings","contains",{"string","string"},"bool"},
        {"strings","starts_with",{"string","string"},"bool"},
        {"strings","ends_with",{"string","string"},"bool"},
        {"strings","index",{"string","string"},"int"},
        {"strings","len_bytes",{"string"},"int"},
        {"strings","rune_count",{"string"},"Result<int,string>"},
        {"strings","trim",{"string"},"string"},
        {"strings","lower_ascii",{"string"},"string"},
        {"strings","upper_ascii",{"string"},"string"},
        {"strings","split",{"string","string"},"[]string"},
        {"strings","join",{"[]string","string"},"string"},
        {"strings","replace",{"string","string","string"},"string"},
        {"strings","slice",{"string","int","int"},"Result<string,string>"},
        {"json","decode",{"string"},"Result<Json,string>"},
        {"json","encode",{""},"Result<string,string>"},
        {"json","kind",{"Json"},"string"},
        {"json","get",{"Json","string"},"Result<Json,string>"},
        {"json","at",{"Json","int"},"Result<Json,string>"},
        {"json","len",{"Json"},"Result<int,string>"},
        {"json","keys",{"Json"},"Result<[]string,string>"},
        {"json","as_string",{"Json"},"Result<string,string>"},
        {"json","as_int",{"Json"},"Result<int,string>"},
        {"json","as_float",{"Json"},"Result<float,string>"},
        {"json","as_bool",{"Json"},"Result<bool,string>"},
        {"json","is_null",{"Json"},"bool"},
        {"fs","read_bytes",{"string"},"Result<Bytes,string>",false,false,4},
        {"fs","write_bytes",{"string","Bytes"},"Result<bool,string>",false,false,4},
        {"fs","append_bytes",{"string","Bytes"},"Result<bool,string>",false,false,4},
        {"bytes","from_ints",{"[]int"},"Result<Bytes,string>",false,false,4},
        {"bytes","from_text",{"string"},"Bytes",false,false,4},
        {"bytes","to_text",{"Bytes"},"Result<string,string>",false,false,4},
        {"bytes","to_ints",{"Bytes"},"Result<[]int,string>",false,false,4},
        {"bytes","len",{"Bytes"},"int",false,false,4},
        {"bytes","at",{"Bytes","int"},"Result<int,string>",false,false,4},
        {"bytes","slice",{"Bytes","int","int"},"Result<Bytes,string>",false,false,4},
        {"bytes","concat",{"Bytes","Bytes"},"Result<Bytes,string>",false,false,4},
        {"bytes","u32_le",{"int"},"Result<Bytes,string>",false,false,4},
        {"bytes","i64_le",{"int"},"Bytes",false,false,4},
        {"bytes","read_u32_le",{"Bytes","int"},"Result<int,string>",false,false,4},
        {"bytes","read_i64_le",{"Bytes","int"},"Result<int,string>",false,false,4},
        {"bytes","crc32",{"Bytes"},"int",false,false,4},
        {"buffer","new",{},"Buffer",false,true,4},
        {"buffer","len",{"Buffer"},"int",false,false,4},
        {"buffer","append",{"Buffer","Bytes"},"Result<int,string>",true,false,4},
        {"buffer","write_u8",{"Buffer","int"},"Result<int,string>",true,false,4},
        {"buffer","write_u32_le",{"Buffer","int"},"Result<int,string>",true,false,4},
        {"buffer","write_i64_le",{"Buffer","int"},"Result<int,string>",true,false,4},
        {"buffer","write_text",{"Buffer","string"},"Result<int,string>",true,false,4},
        {"buffer","bytes",{"Buffer"},"Bytes",false,false,4},
        {"buffer","text",{"Buffer"},"Result<string,string>",false,false,4},
        {"buffer","clear",{"Buffer"},"void",true,false,4},
        {"list","from_slice",{""},"List",false,true,4},
        {"list","len",{""},"int",false,false,4},
        {"list","get",{"","int"},"",false,false,4},
        {"list","set",{"","int",""},"Result<bool,string>",true,false,4},
        {"list","append",{"",""},"Result<int,string>",true,false,4},
        {"list","extend",{"",""},"Result<int,string>",true,false,4},
        {"list","pop",{""},"",true,false,4},
        {"list","snapshot",{""},"",false,false,4},
        {"list","clear",{""},"void",true,false,4},
        {"list","reserve",{"","int"},"Result<bool,string>",true,false,4},
    };return functions;
}
std::string standard_name(const StandardFunction& f){return "$std$"+f.module+"$"+f.name;}
const StandardFunction* standard_function(std::string_view name){for(const auto& f:standard_functions())if(standard_name(f)==name)return &f;return nullptr;}
bool standard_module(std::string_view name){for(const auto& f:standard_functions())if(name=="std."+f.module)return true;return false;}
Value standard_result(Value value,bool ok,const std::string& type){return Value(ResultValue{ok,managed<Value>(std::move(value)),type,"string",true,false});}
std::string text_utf8_error(std::string_view s) {
    for(std::size_t i=0;i<s.size();) {auto b=static_cast<unsigned char>(s[i++]);if(b<128)continue;unsigned n;std::uint32_t v,min;
        if(b>=194&&b<=223){n=1;v=b&31;min=128;}else if(b>=224&&b<=239){n=2;v=b&15;min=2048;}else if(b>=240&&b<=244){n=3;v=b&7;min=65536;}else return "invalid UTF-8";
        if(n>s.size()-i)return "truncated UTF-8";while(n--){auto c=static_cast<unsigned char>(s[i++]);if((c&192)!=128)return "invalid UTF-8 continuation";v=(v<<6)|(c&63);}
        if(v<min||v>0x10ffff||(v>=0xd800&&v<=0xdfff))return "invalid UTF-8 code point";
    }return {};
}
Value invoke_standard(const StandardFunction& f,const std::vector<Value>& input,const SourceSpan& s,std::ostream& output,RuntimeContext* context) {
    if(input.size()!=f.parameters.size())runtime_error(s,"standard function argument count mismatch","E4003");
    std::vector<std::string> types;for(const auto& v:input)types.push_back(value_type(v));auto signature=standard_signature(f,types);
    if(f.module=="math"){
        for(const auto& value:input)if(value_type(value)!="float")runtime_error(s,"math requires explicit default float arguments","E4003");
        return invoke_algorithm_standard(f,input,s,context);
    }
    if(f.module=="alg"&&input.size()==2&&value_type(input[1])!=signature.parameters[1])
        runtime_error(s,"algorithm target requires identical element type","E4003");
    std::vector<Value> args;for(std::size_t i=0;i<input.size();++i){if(signature.parameters[i]=="Bytes"&&!std::holds_alternative<BytesValue>(input[i].data))runtime_error(s,"expected Bytes","E4003");if(signature.parameters[i]=="Buffer"&&!std::holds_alternative<BufferValue>(input[i].data))runtime_error(s,"expected Buffer","E4003");if(f.parameters[i]=="Json"){auto j=std::get_if<JsonValue>(&input[i].data);if(!j||!j->data)runtime_error(s,"expected a Json value","E4003");}args.push_back(enforce_type(input[i],signature.parameters[i],s));}
    if(f.mutates_first){bool writable=false;if(auto p=std::get_if<BufferValue>(&args[0].data))writable=p->writable;if(auto p=std::get_if<ListValue>(&args[0].data))writable=p->writable;if(!writable)runtime_error(s,"standard mutation requires a writable argument","E4007");}
    if(f.module=="error")return invoke_error_standard(f,args,s);
    if(f.module=="path"||(f.module=="fs"&&(f.name=="stat"||f.name=="rename"||f.name=="replace"||f.name=="temp_file")))return invoke_filesystem_standard(f,args,s,context);
    if(f.module=="alg")return invoke_algorithm_standard(f,args,s,context);
    if(f.module=="time"||(f.module=="os"&&f.name=="getenv"))return invoke_system_standard(f,args,s,context);
    if(f.module=="simd")return simd_standard(f.name,args,s,context);
    if(f.module=="gc"){auto& heap=managed_heap();return Value(static_cast<I>(f.name=="collect"?heap.collect():f.name=="live"?heap.live():f.name=="allocated"?heap.allocated():heap.collected()));}
    if(f.module=="task")return task_standard(f.name,args,s,context);
    if(f.module=="bytes"||f.module=="buffer"||f.module=="list")return invoke_binary_standard(f,args,s);
    if(f.module=="strings")for(const auto& v:args)if(auto t=std::get_if<std::string>(&v.data))bounded(*t,s);
    auto result=type_arguments(f.result,"Result");std::string success=result.empty()?"":result[0];
    auto ok=[&](Value v){return standard_result(std::move(v),true,success);};auto err=[&](std::string text){return standard_result(Value(std::move(text)),false,success);};
    auto text=[&](std::size_t i)->const std::string& {return std::get<std::string>(args[i].data);};
    if(f.module=="os") {
        if(f.name=="args")return strings(context?context->arguments:std::vector<std::string>{});
        try{return ok(Value(utf8(context&&!context->working_directory.empty()?context->working_directory:std::filesystem::current_path())));}catch(const std::filesystem::filesystem_error&){return err("cannot get working directory");}
    }
    if(f.module=="io") {
        if(f.name=="write"){if(text(0).size()>max_text)return err("stdout text exceeds 16 MiB");auto invalid=text_utf8_error(text(0));if(!invalid.empty())return err(invalid);output.write(text(0).data(),static_cast<std::streamsize>(text(0).size()));if(!output)return err("stdout write failed");return ok(Value(static_cast<I>(text(0).size())));}
        if(!context||!context->input)return err("stdin is unavailable");auto& stream=*context->input;std::string value;char c;bool any=false,line_end=false;
        while(stream.get(c)){any=true;if(f.name=="read_line"&&c=='\n'){line_end=true;break;}if(value.size()>=max_text)return err("stdin text exceeds 16 MiB");value+=c;}
        if(stream.bad()||(!stream.eof()&&stream.fail()))return err("stdin read failed");
        if(f.name=="read_line"&&!any&&stream.eof())return ok(Value{});
        if(f.name=="read_line"&&line_end&&!value.empty()&&value.back()=='\r')value.pop_back();
        auto invalid=text_utf8_error(value);if(!invalid.empty())return err(invalid);return ok(Value(std::move(value)));
    }
    if(f.module=="fs") {
        try {
            auto p=path(text(0),context);std::error_code ec;
            if(f.name=="exists"){bool exists=std::filesystem::exists(p,ec);return ec?err("cannot inspect path"):ok(Value(exists));}
            if(f.name=="remove"||f.name=="mkdir_all"){bool changed=f.name=="remove"?std::filesystem::remove(p,ec):std::filesystem::create_directories(p,ec);return ec?err("filesystem operation failed"):ok(Value(changed));}
            if(f.name=="list_dir"){std::vector<std::string> names;std::filesystem::directory_iterator it(p,ec),end;if(ec)return err("cannot list directory");for(;it!=end;it.increment(ec)){if(ec)return err("directory iteration failed");if(names.size()>=max_items)return err("directory entry limit exceeded");names.push_back(utf8(it->path().filename()));}if(ec)return err("directory iteration failed");std::sort(names.begin(),names.end());return ok(strings(names));}
            if(f.name=="read_text"||f.name=="read_bytes") {
                if(!std::filesystem::is_regular_file(p,ec)||ec)return err("cannot read regular text file");std::ifstream file(p,std::ios::binary|std::ios::ate);if(!file)return err("cannot open text file");auto size=file.tellg();if(size<0||size>static_cast<std::streamoff>(f.name=="read_bytes"?128*1024*1024:max_text))return err(f.name=="read_bytes"?"binary file is unreadable or exceeds 128 MiB":"text file is unreadable or exceeds 16 MiB");
                std::string value(static_cast<std::size_t>(size),'\0');file.seekg(0);if(!value.empty()&&!file.read(value.data(),static_cast<std::streamsize>(value.size())))return err("text file read failed");if(f.name=="read_bytes")return ok(Value(BytesValue{managed<const std::string>(std::move(value))}));auto invalid=text_utf8_error(value);if(!invalid.empty())return err(invalid);return ok(Value(std::move(value)));
            }
            bool binary=f.name=="write_bytes"||f.name=="append_bytes";const auto& content=binary?*std::get<BytesValue>(args[1].data).data:text(1);
            if(content.size()>(binary?128*1024*1024:max_text))return err("file content exceeds size limit");if(!binary){auto invalid=text_utf8_error(content);if(!invalid.empty())return err(invalid);}
            // Existing non-regular targets (including directories/devices) are not opened.
            if(std::filesystem::exists(p,ec)&&(!std::filesystem::is_regular_file(p,ec)||ec))return err("write target is not a regular file");if(ec)return err("cannot inspect write target");
            std::ofstream file(p,std::ios::binary|((f.name=="append_text"||f.name=="append_bytes")?std::ios::app:std::ios::trunc));if(!file)return err("cannot open text file for writing");file.write(content.data(),static_cast<std::streamsize>(content.size()));file.flush();if(!file)return err("text file write failed");file.close();if(!file)return err("text file close failed");return ok(Value(true));
        }catch(const std::filesystem::filesystem_error&){return err("filesystem path operation failed");}catch(const std::runtime_error& e){return err(e.what());}
    }
    if(f.module=="json") {if(f.name=="decode")return json_decode(text(0));if(f.name=="encode")return json_encode(args[0]);return json_access(f.name,args);}
    if(f.name=="join"){const auto& slice=std::get<SliceValue>(args[0].data);std::string out;for(std::size_t i=0;i<slice.length;++i){if(i){if(text(1).size()>max_text-out.size())runtime_error(s,"join text exceeds 16 MiB","E4099");out+=text(1);}const auto& item=std::get<std::string>((*slice.storage)[slice.start+i].data);if(item.size()>max_text-out.size())runtime_error(s,"join text exceeds 16 MiB","E4099");out+=item;}return Value(std::move(out));}
    const auto& a=text(0);bounded(a,s);const auto& n=f.name;
    if(n=="len_bytes")return Value(static_cast<I>(a.size()));
    if(n=="contains"||n=="starts_with"||n=="ends_with"||n=="index"){const auto& b=text(1);if(n=="contains")return Value(a.find(b)!=std::string::npos);if(n=="starts_with")return Value(a.starts_with(b));if(n=="ends_with")return Value(a.ends_with(b));auto found=a.find(b);return Value(found==std::string::npos?I{-1}:static_cast<I>(found));}
    if(n=="rune_count"){auto invalid=text_utf8_error(a);if(!invalid.empty())return err(invalid);I count=0;for(unsigned char c:a)if((c&192)!=128)++count;return ok(Value(count));}
    if(n=="slice"){auto start=as_int(args[1],s),end=as_int(args[2],s);if(start<0||end<start||static_cast<std::size_t>(end)>a.size())return err("string slice bounds are invalid");auto value=a.substr(static_cast<std::size_t>(start),static_cast<std::size_t>(end-start));auto invalid=text_utf8_error(value);if(!invalid.empty())return err("string slice cuts a UTF-8 character");return ok(Value(std::move(value)));}
    if(n=="trim"){auto begin=a.find_first_not_of(" \t\r\n\v\f");if(begin==std::string::npos)return Value(std::string{});return Value(a.substr(begin,a.find_last_not_of(" \t\r\n\v\f")-begin+1));}
    if(n=="lower_ascii"||n=="upper_ascii"){auto out=a;for(char& c:out){if(n=="lower_ascii"&&c>='A'&&c<='Z')c+=32;else if(n=="upper_ascii"&&c>='a'&&c<='z')c-=32;}return Value(std::move(out));}
    if(n=="split"){
        std::vector<std::string> out;const auto& delimiter=text(1);
        if(delimiter.empty()){auto invalid=text_utf8_error(a);if(!invalid.empty())runtime_error(s,invalid,"E4003");for(std::size_t at=0;at<a.size();){std::size_t next=at+1;while(next<a.size()&&(static_cast<unsigned char>(a[next])&192)==128)++next;out.push_back(a.substr(at,next-at));at=next;if(out.size()>max_items)runtime_error(s,"split item limit exceeded","E4099");}}
        else {std::size_t at=0;while(true){auto next=a.find(delimiter,at);out.push_back(a.substr(at,next==std::string::npos?next:next-at));if(out.size()>max_items)runtime_error(s,"split item limit exceeded","E4099");if(next==std::string::npos)break;at=next+delimiter.size();}}return strings(out);
    }
    if(n=="replace"){const auto& before=text(1);const auto& after=text(2);if(before.empty())return Value(a);std::string out;std::size_t at=0;while(true){auto next=a.find(before,at);auto length=next==std::string::npos?a.size()-at:next-at;if(length>max_text-out.size())runtime_error(s,"replace text exceeds 16 MiB","E4099");out.append(a,at,length);if(next==std::string::npos)break;if(after.size()>max_text-out.size())runtime_error(s,"replace text exceeds 16 MiB","E4099");out+=after;at=next+before.size();}return Value(std::move(out));}
    runtime_error(s,"unknown standard function","E4003");
}
}
