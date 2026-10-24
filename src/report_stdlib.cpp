#include "hua/stdlib.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
namespace hua {
namespace {
using I=std::int64_t;
constexpr std::size_t max_text=16*1024*1024,max_shown=200;
// One lock for every stderr writer here: a log line or write_err call is a single
// write, so lines from concurrent Tasks never interleave.
std::mutex& stderr_lock(){static std::mutex lock;return lock;}
bool write_stderr(std::string_view text){std::lock_guard guard(stderr_lock());std::cerr.write(text.data(),static_cast<std::streamsize>(text.size()));std::cerr.flush();return bool(std::cerr);}
const std::string& text(const Value& v,const SourceSpan& s){auto p=std::get_if<std::string>(&v.data);if(!p)runtime_error(s,"expected string","E4003");return *p;}
// UTF-8-safe truncation for assertion messages.
std::string shown(const Value& v){
    auto out=show(v);if(out.size()<=max_shown)return out;
    auto cut=max_shown;while(cut>0&&(static_cast<unsigned char>(out[cut])&0xc0)==0x80)--cut;
    return out.substr(0,cut)+"...";
}
// std.log levels; set_level is process-wide.
std::atomic<int> minimum_level{1};
int level_of(std::string_view name,const SourceSpan& s){
    if(name=="debug")return 0;if(name=="info")return 1;if(name=="warn")return 2;if(name=="error")return 3;
    runtime_error(s,"log level must be debug/info/warn/error","E4003");
}
// logfmt value: bare when it has no space, '=', '"', '\\' or control byte; otherwise
// quoted with \\ \" \n \r \t and \u00XX escapes.
void append_value(std::string& out,std::string_view v){
    bool bare=!v.empty();
    for(unsigned char c:v)if(c==' '||c=='='||c=='"'||c=='\\'||c<0x20||c==0x7f){bare=false;break;}
    if(bare){out+=v;return;}
    out+='"';
    for(unsigned char c:v){
        if(c=='"'||c=='\\'){out+='\\';out+=static_cast<char>(c);}
        else if(c=='\n')out+="\\n";else if(c=='\r')out+="\\r";else if(c=='\t')out+="\\t";
        else if(c<0x20||c==0x7f){static constexpr char hex[]="0123456789abcdef";out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}
        else out+=static_cast<char>(c);
    }
    out+='"';
}
void valid_key(const std::string& key,const SourceSpan& s){
    if(key.empty())runtime_error(s,"log field keys must be non-empty","E4003");
    for(unsigned char c:key)if(c==' '||c=='='||c=='"'||c=='\\'||c<0x20||c==0x7f)runtime_error(s,"log field keys must not contain spaces, '=', quotes or control characters","E4003");
}
void log_line(int level,const std::string& message,const MapValue* fields,const SourceSpan& s){
    static constexpr const char* names[]{"debug","info","warn","error"};
    if(level<minimum_level.load(std::memory_order_relaxed))return;
    auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::string line="ts="+format_rfc3339_utc(static_cast<I>(now))+" level="+names[level]+" msg=";
    append_value(line,message);
    if(fields)for(const auto& [key,value]:fields->data->entries){
        const auto& k=std::get<std::string>(key);valid_key(k,s);
        line+=' ';line+=k;line+='=';append_value(line,text(value,s));
    }
    line+='\n';
    if(line.size()>max_text)runtime_error(s,"log line exceeds 16 MiB","E4099");
    write_stderr(line);
}
[[noreturn]] void assertion(const SourceSpan& s,const std::string& detail){runtime_error(s,"assertion failed: "+detail,"E4900");}
}

void reset_report_state(){minimum_level.store(1);}

// io.write_err, std.test and std.log (since 18): one-call operations dispatched
// before the generic argument checks, so they check argument kinds themselves.
Value invoke_report_standard(const StandardFunction& f,const std::vector<Value>& args,const SourceSpan& s){
    if(args.size()!=f.parameters.size())runtime_error(s,"standard function argument count mismatch","E4003");
    const auto& name=f.name;
    if(f.module=="io"){
        const auto& t=text(args[0],s);
        if(t.size()>max_text)return standard_result(Value(std::string("stderr text exceeds 16 MiB")),false,"int");
        if(!write_stderr(t))return standard_result(Value(std::string("stderr write failed")),false,"int");
        return standard_result(Value(static_cast<I>(t.size())),true,"int");
    }
    if(f.module=="test"){
        if(name=="assert"){auto c=std::get_if<bool>(&args[0].data);if(!c)runtime_error(s,"expected bool","E4003");if(!*c)assertion(s,text(args[1],s));return {};}
        if(name=="fail")assertion(s,text(args[0],s));
        const bool same=as_bool(binary_value("==",args[0],args[1],s),s);
        if(name=="equal"&&!same)assertion(s,"expected "+shown(args[1])+", got "+shown(args[0]));
        if(name=="not_equal"&&same)assertion(s,"values are equal: "+shown(args[0]));
        return {};
    }
    if(name=="set_level"){minimum_level.store(level_of(text(args[0],s),s));return {};}
    if(name=="with_fields"){
        auto m=std::get_if<MapValue>(&args[2].data);if(!m||!m->data||m->data->key_type!="string"||m->data->item_type!="string")runtime_error(s,"log fields must be map[string]string","E4003");
        log_line(level_of(text(args[0],s),s),text(args[1],s),m,s);return {};
    }
    log_line(level_of(name,s),text(args[0],s),nullptr,s);
    return {};
}
}
