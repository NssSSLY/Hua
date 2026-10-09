#include "hua/stdlib.hpp"
#include "hua/json.hpp"
#include "hua/tasks.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <limits>
#include <random>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#endif

namespace hua {
namespace {
namespace fs = std::filesystem;
using I = std::int64_t;
constexpr std::size_t max_path = 32767, max_parts = 1024, max_prefix = 64;
struct Failure { std::string message; };
[[noreturn]] void fail(std::string code, std::string message) {
    throw Failure{std::move(code) + ": " + std::move(message)};
}
void valid(const std::string& s, bool empty = true) {
    if ((!empty && s.empty()) || s.find('\0') != std::string::npos ||
        !text_utf8_error(s).empty()) fail("PATH_INVALID", "non-NUL UTF-8 path required");
    if (s.size() > max_path) fail("PATH_LIMIT", "path exceeds 32767 UTF-8 bytes");
}
bool same(const std::string& a, const std::string& b) {
#ifdef _WIN32
    auto wide=[](const std::string& x) {
        auto n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,x.data(),static_cast<int>(x.size()),nullptr,0);
        std::wstring w(n,L'\0');
        if(n)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,x.data(),static_cast<int>(x.size()),w.data(),n);
        return w;
    };
    if(a.empty() || b.empty())return a==b;
    auto x=wide(a),y=wide(b);
    return CompareStringOrdinal(x.data(),static_cast<int>(x.size()),y.data(),static_cast<int>(y.size()),TRUE)==CSTR_EQUAL;
#else
    return a==b;
#endif
}
struct Lex {
    std::string root;
    std::vector<std::string> parts;
};
void component(Lex& p, const std::string& item) {
    if(item.empty() || item==".")return;
    if(item==".." && !p.parts.empty() && p.parts.back()!="..")p.parts.pop_back();
    else if(item!=".." || p.root.empty())p.parts.push_back(item);
}
Lex parse(std::string text) {
    valid(text);
    Lex out;std::size_t start=0;
#ifdef _WIN32
    for(auto& c:text)if(c=='\\')c='/';
    if(text.starts_with("//")) {
        if(text.starts_with("//?/") || text.starts_with("//./"))fail("PATH_INVALID","device paths are not supported");
        auto server=text.find('/',2);
        if(server==std::string::npos || server==2)fail("PATH_INVALID","UNC requires server and share");
        auto share=text.find('/',server+1);if(share==std::string::npos)share=text.size();
        auto name=text.substr(server+1,share-server-1);
        if(name.empty() || name=="." || name=="..")fail("PATH_INVALID","UNC requires server and share");
        out.root=text.substr(0,share)+"/";start=share;
    } else if(text.size()>=2 && text[1]==':') {
        auto letter=text[0];
        if(!((letter>='A'&&letter<='Z')||(letter>='a'&&letter<='z')) || text.size()<3 || text[2]!='/')
            fail("PATH_INVALID","drive-relative paths are not supported");
        out.root=text.substr(0,3);start=3;
    } else if(text.starts_with('/'))fail("PATH_INVALID","root-relative Windows paths require a drive or UNC share");
#else
    if(text.starts_with('/')){out.root="/";start=1;}
#endif
    while(start<text.size()){
        auto end=text.find('/',start);if(end==std::string::npos)end=text.size();
        component(out,text.substr(start,end-start));start=end+1;
    }
    return out;
}
std::string render(const Lex& p) {
    std::string out=p.root;
    for(const auto& part:p.parts){if(!out.empty()&&out.back()!='/')out+='/';out+=part;}
    if(out.empty())out=".";
    valid(out);return out;
}
Value lexical(const StandardFunction& f,const std::vector<Value>& args,const SourceSpan& span,RuntimeContext* context) {
    auto text=[&](std::size_t i)->const std::string&{return std::get<std::string>(args[i].data);};
    Lex p;
    if(f.name=="join"){
        auto slice=std::get<SliceValue>(args[0].data);
        if(slice.length>max_parts)fail("PATH_LIMIT","join accepts at most 1024 parts");
        std::size_t total=0;
        for(std::size_t i=0;i<slice.length;++i){
            task_checkpoint(context,span);
            auto item=std::get<std::string>(sequence_read(slice,i).data);valid(item);
            if(item.size()>max_path-total)fail("PATH_LIMIT","join input exceeds 32767 UTF-8 bytes");
            total+=item.size();auto next=parse(item);
            if(!next.root.empty())p=std::move(next);
            else for(const auto& part:next.parts)component(p,part);
        }
    } else p=parse(text(0));
    std::string result;
    if(f.name=="join" || f.name=="normalize")result=render(p);
    else if(f.name=="basename")result=p.parts.empty()?render(p):p.parts.back();
    else if(f.name=="dirname"){if(!p.parts.empty())p.parts.pop_back();result=render(p);}
    else if(f.name=="ext"){
        if(!p.parts.empty()){auto name=p.parts.back();auto dot=name.rfind('.');
            if(dot!=std::string::npos && dot!=0 && name!="..")result=name.substr(dot);}
    } else if(f.name=="relative"){
        auto base=parse(text(1));if(!same(p.root,base.root))fail("PATH_ROOT","paths require the same root and absolute/relative form");
        std::size_t common=0;while(common<p.parts.size()&&common<base.parts.size()&&same(p.parts[common],base.parts[common]))++common;
        Lex rel;
        for(std::size_t i=common;i<base.parts.size();++i){
            if(base.parts[i]=="..")fail("PATH_ROOT","base contains an unresolved parent");
            rel.parts.emplace_back("..");
        }
        rel.parts.insert(rel.parts.end(),p.parts.begin()+common,p.parts.end());result=render(rel);
    } else runtime_error(span,"unknown path operation","E4003");
    return standard_result(Value(std::move(result)),true,"string");
}
std::string utf8(const fs::path& p) {auto s=p.generic_u8string();return {s.begin(),s.end()};}
fs::path native(const std::string& text,RuntimeContext* context) {
    valid(text,false);
#ifdef _WIN32
    // Check the original components, before collapsing ".."; do not change filesystem traversal.
    std::string s=text;for(auto& c:s)if(c=='\\')c='/';parse(s);
    std::size_t start=0;
    while(start<s.size()){
        auto end=s.find('/',start);if(end==std::string::npos)end=s.size();
        auto part=s.substr(start,end-start);start=end+1;
        if(part.empty()||part=="."||part=="..")continue;
        if(part.size()==2&&part[1]==':')continue;
        if(part.back()=='.'||part.back()==' ')fail("PATH_INVALID","trailing Windows dots/spaces are not supported");
        for(unsigned char c:part)if(c<32||std::string("<>:\"|?*").find(c)!=std::string::npos)fail("PATH_INVALID","invalid Windows filename");
        auto stem=part.substr(0,part.find('.'));for(auto& c:stem)if(c>='a'&&c<='z')c-=32;
        if(stem=="CON"||stem=="PRN"||stem=="AUX"||stem=="NUL"||
           (stem.size()==4&&(stem.starts_with("COM")||stem.starts_with("LPT"))&&stem[3]>='1'&&stem[3]<='9'))
            fail("PATH_INVALID","Windows device names are not supported");
    }
#endif
    fs::path p(std::u8string(text.begin(),text.end()));
    if(p.is_relative())p=(context&&!context->working_directory.empty()?context->working_directory:fs::current_path())/p;
    valid(utf8(p),false);return p;
}
std::string system_message(const char* op,std::error_code ec) {
    auto code=ec==std::errc::no_such_file_or_directory?"FS_NOT_FOUND":
              ec==std::errc::file_exists?"FS_EXISTS":
              ec==std::errc::cross_device_link?"FS_CROSS_DEVICE":"FS_ERROR";
    std::string message;
#ifdef _WIN32
    if(ec.category()==std::system_category()){
        wchar_t* buffer=nullptr;
        auto count=FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,nullptr,ec.value(),0,reinterpret_cast<wchar_t*>(&buffer),0,nullptr);
        if(count){
            auto bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,buffer,count,nullptr,0,nullptr,nullptr);
            if(bytes){message.resize(bytes);WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,buffer,count,message.data(),bytes,nullptr,nullptr);}
        }
        if(buffer)LocalFree(buffer);
    }else message=ec.message();
#else
    message=ec.message();
#endif
    if(message.empty() || !text_utf8_error(message).empty())message="host filesystem operation failed";
    while(!message.empty()&&(message.back()=='\r'||message.back()=='\n'))message.pop_back();
    return std::string(code)+": "+op+" ["+ec.category().name()+":"+std::to_string(ec.value())+"] "+message;
}
[[noreturn]] void system_failure(const char* op,std::error_code ec) {throw Failure{system_message(op,ec)};}
fs::file_status inspect_node(const fs::path& p){
    std::error_code ec;auto info=fs::symlink_status(p,ec);
    if(ec && ec!=std::errc::no_such_file_or_directory)system_failure("stat",ec);
    return info;
}
Value metadata(const fs::path& p){
    auto info=inspect_node(p);if(!fs::exists(info))fail("FS_NOT_FOUND","path does not exist");
    std::string kind=fs::is_symlink(info)?"symlink":fs::is_regular_file(info)?"file":fs::is_directory(info)?"directory":"other";
    JsonData::Object fields;
    auto field=[&](const std::string& name,auto value){fields.emplace(name,managed<const JsonData>(std::move(value)));};
    field("path",utf8(p));field("kind",kind);
    if(kind=="file"){
        std::error_code ec;auto n=fs::file_size(p,ec);if(ec)system_failure("file_size",ec);
        if(n>static_cast<std::uintmax_t>(std::numeric_limits<I>::max()))fail("FS_RANGE","file size exceeds int64");
        field("size_bytes",static_cast<I>(n));
    }else field("size_bytes",std::monostate{});
    if(kind=="file" || kind=="directory"){
        std::error_code ec;auto time=fs::last_write_time(p,ec);if(ec)system_failure("last_write_time",ec);
        auto count=std::chrono::duration_cast<std::chrono::milliseconds>(fs::file_time_type::clock::to_sys(time).time_since_epoch()).count();
        field("modified_ms",static_cast<I>(count));
    }else field("modified_ms",std::monostate{});
    return Value(JsonValue{managed<const JsonData>(std::move(fields))});
}
void move(const fs::path& from,const fs::path& to,bool replace) {
    if(same(utf8(from),utf8(to)))fail("FS_SAME_PATH","source and destination are identical");
    auto src=inspect_node(from);if(!fs::exists(src))fail("FS_NOT_FOUND","source does not exist");
    auto dst=inspect_node(to);
    if(!replace && fs::exists(dst))fail("FS_EXISTS","destination already exists");
    if(replace){
        if(!fs::is_regular_file(src) || (fs::exists(dst)&&!fs::is_regular_file(dst)))fail("FS_TYPE","replace requires regular files, not links or directories");
        if(fs::exists(dst)){std::error_code ec;auto eq=fs::equivalent(from,to,ec);if(ec)system_failure("equivalent",ec);if(eq)fail("FS_SAME_PATH","source and destination refer to the same file");}
    }
#ifdef _WIN32
    if(!MoveFileExW(from.c_str(),to.c_str(),replace?MOVEFILE_REPLACE_EXISTING:0))
        system_failure(replace?"replace":"rename",std::error_code(GetLastError(),std::system_category()));
#else
    int result;
    if(replace)result=::rename(from.c_str(),to.c_str());
#ifdef __linux__
    else result=static_cast<int>(::syscall(SYS_renameat2,AT_FDCWD,from.c_str(),AT_FDCWD,to.c_str(),1 /* RENAME_NOREPLACE */));
#elif __APPLE__
    else result=::renamex_np(from.c_str(),to.c_str(),RENAME_EXCL);
#else
    else fail("FS_UNSUPPORTED","no atomic non-overwriting rename on this platform");
#endif
    if(result!=0)system_failure(replace?"replace":"rename",std::error_code(errno,std::generic_category()));
#endif
}
std::string temporary(const std::string& directory,std::string prefix,RuntimeContext* context,const SourceSpan& span){
    valid(prefix);
    if(prefix.size()>max_prefix)fail("PATH_LIMIT","temporary prefix exceeds 64 UTF-8 bytes");
    for(unsigned char c:prefix)if(c<32 || std::string("/\\<>:\"|?*").find(c)!=std::string::npos)fail("PATH_INVALID","temporary prefix must be a filename fragment");
    if(prefix.empty())prefix="hua-";
    std::error_code ec;auto dir=directory.empty()?fs::temp_directory_path(ec):native(directory,context);
    if(ec)system_failure("temp_directory",ec);
    if(dir.is_relative())dir=(context&&!context->working_directory.empty()?context->working_directory:fs::current_path())/dir;
    if(!fs::is_directory(dir,ec)||ec){if(ec)system_failure("temp_directory",ec);fail("FS_TYPE","temporary directory required");}
    static std::atomic<std::uint64_t> sequence{0};
    std::random_device random;
    for(unsigned attempt=0;attempt<128;++attempt){
        task_checkpoint(context,span);
        auto token=(static_cast<std::uint64_t>(random())<<32)^random()^sequence.fetch_add(1);
        auto name=prefix+std::to_string(token)+".tmp";
        auto candidate=dir/fs::path(std::u8string(name.begin(),name.end()));valid(utf8(candidate),false);
#ifdef _WIN32
        auto handle=CreateFileW(candidate.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE){auto error=GetLastError();if(error==ERROR_FILE_EXISTS||error==ERROR_ALREADY_EXISTS)continue;system_failure("temp_file",std::error_code(error,std::system_category()));}
        if(!CloseHandle(handle)){auto error=GetLastError();DeleteFileW(candidate.c_str());system_failure("temp_close",std::error_code(error,std::system_category()));}
#else
        auto fd=::open(candidate.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
        if(fd<0){if(errno==EEXIST)continue;system_failure("temp_file",std::error_code(errno,std::generic_category()));}
        if(::close(fd)!=0){auto error=errno;::unlink(candidate.c_str());system_failure("temp_close",std::error_code(error,std::generic_category()));}
#endif
        return utf8(candidate);
    }
    fail("FS_EXISTS","temporary name collision limit reached");
}
}
Value invoke_filesystem_standard(const StandardFunction& f,const std::vector<Value>& args,const SourceSpan& span,RuntimeContext* context){
    task_checkpoint(context,span);
    auto success=f.name=="stat"?"Json":(f.module=="path"||f.name=="temp_file")?"string":"bool";
    try{
        if(f.module=="path")return lexical(f,args,span,context);
        if(f.name=="temp_file")return standard_result(Value(temporary(std::get<std::string>(args[0].data),std::get<std::string>(args[1].data),context,span)),true,success);
        auto from=native(std::get<std::string>(args[0].data),context);
        if(f.name=="stat")return standard_result(metadata(from),true,success);
        auto to=native(std::get<std::string>(args[1].data),context);
        task_checkpoint(context,span);move(from,to,f.name=="replace");
        return standard_result(Value(true),true,success);
    }catch(const Failure& e){return standard_result(Value(e.message),false,success);}
    catch(const fs::filesystem_error& e){return standard_result(Value(system_message("filesystem",e.code())),false,success);}
    catch(const Diagnostic&){throw;}
    catch(const std::runtime_error& e){auto message=std::string(e.what());if(!text_utf8_error(message).empty())message="host operation failed";return standard_result(Value("FS_ERROR: "+message),false,success);}
}
}
