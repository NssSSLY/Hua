#include "hua/runtime.hpp"
#include "hua/tasks.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
using namespace hua;
namespace {
int checks=0;
void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
Value call(const std::string& name,std::vector<Value> args,RuntimeContext* context=nullptr){
    return invoke_builtin("$std$"+name,args,{},std::cout,{},{},context);
}
Value text(const std::string& s){return Value(s);}
bool success(const Value& v){return std::get<ResultValue>(v.data).ok;}
void failure(const char* code,const std::string& name,std::vector<Value> args){
    auto v=call(name,std::move(args));check(!success(v),"expected Err");
    check(show(result_payload(v,false,{})).starts_with(code),"wrong failure prefix");
}
std::string read(const std::filesystem::path& path) {
#ifdef _WIN32
    auto handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE)return "OPEN_FAILED";
    char data[16]{};DWORD n=0;auto ok=ReadFile(handle,data,sizeof(data),&n,nullptr);CloseHandle(handle);
    return ok?std::string(data,n):"READ_FAILED";
#else
    std::ifstream in(path,std::ios::binary);return in?std::string(std::istreambuf_iterator<char>(in),{}):"OPEN_FAILED";
#endif
}
}
int main(int argc,char** argv){
    try {
        auto root=std::filesystem::absolute(argc>1?argv[1]:".")/"s1a-native-work";
        // Only this newly created fixture directory is cleaned; preexisting content is never removed.
        if(!std::filesystem::create_directory(root))throw std::runtime_error("fixture directory already exists");
        struct Cleanup{std::filesystem::path p;~Cleanup(){std::error_code ec;std::filesystem::remove_all(p,ec);}} cleanup{root};
        RuntimeContext context;context.working_directory=root;
        failure("PATH_INVALID","path$normalize",{text(std::string("\xc0\x80",2))});
        failure("PATH_INVALID","path$normalize",{text(std::string("a\0b",3))});
        failure("PATH_LIMIT","path$normalize",{text(std::string(32768,'x'))});
        auto data=managed<std::vector<Value>>();for(int i=0;i<1025;++i)data->emplace_back("a");
        failure("PATH_LIMIT","path$join",{Value(SliceValue{data,0,data->size(),false,"string"})});
        data->clear();data->emplace_back(std::string(20000,'a'));data->emplace_back(std::string(20000,'b'));
        failure("PATH_LIMIT","path$join",{Value(SliceValue{data,0,2,false,"string"})});
        failure("PATH_LIMIT","fs$temp_file",{text("."),text(std::string(65,'x'))});
        context.cancellation=std::make_shared<Cancellation>();context.cancellation->requested=true;
        for(auto name:{"path$normalize","fs$stat"}){
            try{call(name,{text("unused")},&context);throw std::runtime_error("missing cancellation");}
            catch(const Diagnostic& d){check(d.code()=="E4101","cancellation must stay Diagnostic");}
        }
        try{call("fs$temp_file",{text("."),text("canceled-")},&context);throw std::runtime_error("missing cancellation");}
        catch(const Diagnostic& d){check(d.code()=="E4101","cancellation must stay Diagnostic");}
        check(std::filesystem::is_empty(root),"canceled operation created a file");
        context.cancellation->requested=false;
        std::set<std::string> names;
        for(int i=0;i<32;++i){
            auto v=call("fs$temp_file",{text("."),text("file-")},&context);check(success(v),"temp_file");
            auto name=std::get<std::string>(result_payload(v,true,{}).data);
            check(names.insert(name).second,"temporary collision");
            check(std::filesystem::file_size(std::filesystem::path(std::u8string(name.begin(),name.end())))==0,"temporary not empty");
        }
        std::mutex names_mutex;std::atomic<bool> temp_bad{false};
        std::vector<std::thread> creators;
        for(int worker=0;worker<8;++worker)creators.emplace_back([&]{
            for(int i=0;i<16;++i){
                try{
                    auto v=call("fs$temp_file",{text("."),text("parallel-")},&context);
                    if(!success(v)){temp_bad=true;continue;}
                    auto name=std::get<std::string>(result_payload(v,true,{}).data);
                    std::lock_guard lock(names_mutex);if(!names.insert(name).second)temp_bad=true;
                }catch(...){temp_bad=true;}
            }
        });
        for(auto& thread:creators)thread.join();
        check(!temp_bad && names.size()==160,"concurrent exclusive temporary creation");
        // Independent OS reader probes replacement: it must never observe a missing or partial target.
        {std::ofstream(root/"target")<<"old";}
        std::atomic<bool> done{false},bad{false};
        std::thread reader([&]{while(!done){auto x=read(root/"target");if(x!="old"&&x!="new")bad=true;}});
        bool moved=true;
        for(int i=0;i<64;++i){
            {std::ofstream(root/"source",std::ios::binary|std::ios::trunc)<<(i%2?"old":"new");}
            auto expected=i%2?"old":"new";auto prior=read(root/"target");bool completed=false;
            for(int attempt=0;attempt<200;++attempt){
                auto result=call("fs$replace",{text("source"),text("target")},&context);
                if(success(result)){completed=true;break;}
                // Windows can reject replacement while a reader holds a delete-pending object.
                // Every failed attempt must keep both names/content; retry is only in this test.
                if(read(root/"source")!=expected || read(root/"target")!=prior){moved=false;break;}
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if(!completed){moved=false;break;}
        }
        done=true;reader.join();
        check(moved,"replace failed");check(!bad,"replacement visibility");
        {std::ofstream(root/"keep")<<"keep";}
        auto exists=call("fs$rename",{text("target"),text("keep")},&context);
        check(!success(exists),"rename overwrote");check(read(root/"keep")=="keep","rename changed existing target");
        check(read(root/"target")!="OPEN_FAILED","rename lost source");
        // Equal physical files and missing parents must preserve the existing target.
        auto same_file=call("fs$replace",{text("target"),text("target")},&context);
        check(!success(same_file),"same-path replace");
        auto missing=call("fs$replace",{text("target"),text("missing/target")},&context);
        check(!success(missing),"missing parent replace");check(std::filesystem::exists(root/"target"),"missing parent lost source");
        std::cout<<checks<<" path/resource/cancel/temp/replace checks passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
