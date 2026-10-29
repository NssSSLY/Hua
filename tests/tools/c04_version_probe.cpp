// Read-only C04-only call observer. Does not prepare or analyze the input in C++.
#include "hua/archive.hpp"
#include "hua/vm.hpp"
#include "hua/entry.hpp"
#include <iostream>
#include <iomanip>
#include <unordered_map>
#include <map>
#include <filesystem>
struct Observer final:hua::VMCallObserver {
    struct Work{std::uint64_t calls{},steps{};};
    struct Frame{const hua::Node* fn;std::uint64_t begin,children;};
    hua::ExecutionMetrics& metrics;
    std::unordered_map<const hua::Node*,std::string> names;
    std::vector<Frame> stack;
    std::map<std::string,Work> helpers;
    std::map<std::string,std::uint64_t> bodies;
    const hua::Node* analyze{};
    std::uint64_t c04_steps{},work{},passes{},begin{};
    std::uint64_t numeric_queries{},packed_numeric_queries{},numeric_layout_failures{};
    explicit Observer(const hua::SemanticModel& m,hua::ExecutionMetrics& metric):metrics(metric){
        for(const auto& [_,fn]:m.functions){
            if(std::filesystem::path(fn->span.file_id).filename()!="capabilities.hua")continue;
            auto name=hua::declared_name(*fn);auto pos=name.rfind('$');if(pos!=std::string::npos)name.erase(0,pos+1);
            names[fn]=name;if(name=="analyze")analyze=fn;
        }
        if(!analyze)throw std::runtime_error("missing C04 identity");
    }
    static const hua::Value& member(const hua::Value& v,const char* k){return std::get<hua::StructValue>(v.data).data->fields.at(k);}
    void enter(const hua::Node* fn,const hua::Node* caller,const std::vector<hua::Value>& args)override{
        auto it=names.find(fn);if(it==names.end())return;
        auto step=metrics.steps.load();stack.push_back({fn,step,0});
        if(fn==analyze)begin=step;
        if(it->second=="numeric_view"){
            ++numeric_queries;
            const auto& facts=std::get<hua::SliceValue>(member(args.at(0),"facts").data);
            auto id=std::get<std::int64_t>(args.at(1).data);
            if(id<0||std::size_t(id)>=facts.length||!facts.storage){++numeric_layout_failures;}
            else{
                const auto& v=facts.storage->at(facts.start+std::size_t(id));
                auto cell=std::get_if<hua::SliceValue>(&v.data);
                if(cell&&cell->fixed&&cell->length==2&&cell->element_type=="int"&&cell->packed&&cell->packed->size()==16)++packed_numeric_queries;
                else ++numeric_layout_failures;
            }
        }

        if(it->second=="statement"&&caller==analyze)++bodies[hua::string_value(member(args.at(1),"function").data)];
    }
    void leave(const hua::Node* fn,const hua::Value* result)override{
        auto it=names.find(fn);if(it==names.end())return;
        if(stack.empty()||stack.back().fn!=fn)throw std::runtime_error("unbalanced C04 observer");
        auto frame=stack.back();stack.pop_back();auto total=metrics.steps.load()-frame.begin;
        auto& h=helpers[it->second];++h.calls;h.steps+=total-frame.children;
        if(!stack.empty())stack.back().children+=total;
        if(fn==analyze){
            c04_steps=metrics.steps.load()-begin;
            const auto& model=*std::get<hua::ResultValue>(result->data).payload;
            work=std::get<std::int64_t>(member(model,"work").data);passes=std::get<std::int64_t>(member(model,"passes").data);
        }
    }
    void report(){
        std::cerr<<"{\"schema\":\"hua.c04.version-observer.v2\",\"steps\":"<<metrics.steps.load()<<",\"c04_steps\":"<<c04_steps<<",\"logical_work\":"<<work<<",\"passes\":"<<passes<<",\"body_visits\":{";
        bool first=true;for(auto& [k,n]:bodies){if(!first)std::cerr<<',';first=false;std::cerr<<std::quoted(k)<<':'<<n;}
        std::cerr<<"},\"helpers\":{";first=true;for(auto& [k,h]:helpers){if(!first)std::cerr<<',';first=false;std::cerr<<std::quoted(k)<<":{\"calls\":"<<h.calls<<",\"exclusive_steps\":"<<h.steps<<'}';}
        std::cerr<<"},\"numeric_queries\":"<<numeric_queries<<",\"packed_numeric_queries\":"<<packed_numeric_queries<<",\"numeric_cell_bytes\":16,\"numeric_layout_failures\":"<<numeric_layout_failures<<"}\n";
    }
};
int execute(std::vector<std::filesystem::path> paths){
    if(paths.size()!=2)throw std::runtime_error("component input");
    auto image=hua::read_huab(paths[0]);hua::RuntimeContext c;c.input=&std::cin;c.working_directory=std::filesystem::current_path();
    auto arg=paths[1].u8string();c.arguments={std::string(arg.begin(),arg.end())};c.execution_limits={500000000,128,65536};c.execution_metrics=std::make_shared<hua::ExecutionMetrics>();
    Observer observer(image.model,*c.execution_metrics);auto result=hua::VirtualMachine(image.model,std::cout,&c,&observer).run(image.bytecode);
    auto code=hua::report_entry(result,std::cerr);if(code)return code;observer.report();return 0;
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){try{std::vector<std::filesystem::path> p;for(int i=1;i<argc;++i)p.emplace_back(argv[i]);return execute(p);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#else
int main(int argc,char** argv){try{std::vector<std::filesystem::path> p;for(int i=1;i<argc;++i)p.emplace_back(argv[i]);return execute(p);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#endif
