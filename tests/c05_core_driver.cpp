// Test adapter only: consume Hua's already lowered core. Never parse/link/lower/sema input.
// C++ emits/validates instructions and HUAB; C06/C07 remain pending.
#include "hua/json.hpp"
#include "hua/bytecode.hpp"
#include "hua/interpreter.hpp"
#include "hua/vm.hpp"
#include "hua/archive.hpp"
#include "c06_instruction_json.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
using J=hua::JsonData;
static const J& field(const J& j,const std::string& k){return *std::get<J::Object>(j.data).at(k);}
static std::string str(const J& j){return std::get<std::string>(j.data);}
static std::size_t number(const J& j){auto n=std::get<std::int64_t>(j.data);if(n<0)throw std::runtime_error("negative core index");return n;}
static const J::Array& array(const J& j){return std::get<J::Array>(j.data);}
static std::filesystem::path path(const std::string& s){return std::filesystem::path(std::u8string(s.begin(),s.end()));}
static int run(const std::vector<std::string>& args){
    if(args.size()<2)return 2;
    const bool execute=args[0]=="run"||args[0]=="interpret";
    hua::ExecutionOptions execution;
    if(execute){std::vector<std::string> full{"hua_core_driver"};full.insert(full.end(),args.begin(),args.end());if(!hua::execution_options(full,execution))return 2;}
    else if(!(args.size()==2&&(args[0]=="bytecode"||args[0]=="image"))&&!(args.size()==4&&args[0]=="build"&&args[2]=="-o"))return 2;
    auto metrics=execution.report?std::make_shared<hua::ExecutionMetrics>():nullptr;
    hua::ExecutionReport budget_report{execution,metrics,std::cerr};
    try {
        std::ifstream stream(path(args[1]),std::ios::binary|std::ios::ate);
        if(!stream)throw std::runtime_error("cannot read core");
        if(stream.tellg()<0||stream.tellg()>16*1024*1024)throw std::runtime_error("core report exceeds 16 MiB");
        stream.seekg(0);
        // A11 compiler allocation policy for compiler-report decoding only.
        // Interpreter/VM execution continues to select its own default/compiler policy.
        hua::ManagedHeap core_heap{65536};hua::HeapScope core_heap_scope{core_heap};
        const auto decoded=hua::json_decode(std::string(std::istreambuf_iterator<char>(stream),{}));
        const auto& result=std::get<hua::ResultValue>(decoded.data);
        if(!result.ok)throw std::runtime_error("invalid core JSON");
        const auto& report=*std::get<hua::JsonValue>(result.payload->data).data;
        if(str(field(report,"schema"))!="hua.core.lowered.v1"||str(field(report,"profile"))!="C05-L1"||str(field(report,"status"))!="ok")throw std::runtime_error("invalid core envelope");
        const auto& records=array(field(report,"nodes"));
        if(records.size()>262144)throw std::runtime_error("core too large");
        std::map<std::string,std::unique_ptr<hua::Source>> sources;
        for(const auto& s:array(field(report,"sources"))){auto id=str(field(*s,"module"));sources.emplace(id,std::make_unique<hua::Source>(id,str(field(*s,"text"))));}
        const auto entry=str(field(report,"entry"));
        auto program=std::make_unique<hua::Node>(hua::NodeKind::Program,sources.at(entry)->span(0,0));
        std::vector<hua::NodePtr> nodes;std::vector<const hua::Node*> raw;
        hua::SemanticModel model;
        for(std::size_t i=0;i<records.size();++i){
            const auto& r=*records[i];
            if(number(field(r,"id"))!=i)throw std::runtime_error("invalid core node id");
            const auto k=str(field(r,"kind"));
            bool found=false;hua::NodeKind kind{};
            for(int j=0;j<=static_cast<int>(hua::NodeKind::MatchArm);++j)if(hua::node_name(static_cast<hua::NodeKind>(j))==k){found=true;kind=static_cast<hua::NodeKind>(j);break;}
            if(!found||kind==hua::NodeKind::Import||kind==hua::NodeKind::Enum||kind==hua::NodeKind::Variant)throw std::runtime_error("not a core variant");
            const auto module=str(field(r,"module"));const auto& source=*sources.at(module);
            auto span=source.span(number(field(r,"start")),number(field(r,"end")));
            if(span.line!=number(field(r,"line"))||span.column!=number(field(r,"column")))throw std::runtime_error("invalid core source position");
            auto n=std::make_unique<hua::Node>(kind,span,str(field(r,"text")));
            for(const auto& child:array(field(r,"children"))){auto id=number(*child);if(id>=i||!nodes.at(id))throw std::runtime_error("invalid core tree edge");n->add(std::move(nodes[id]));}
            auto* node=n.get();const auto type=str(field(r,"type_name"));
            if(!type.empty())model.types[node]=type;
            model.writable[node]=std::get<bool>(field(r,"writable").data);
            if(kind==hua::NodeKind::Function){auto name=hua::declared_name(*node);if(!model.functions.emplace(name,node).second)throw std::runtime_error("duplicate core function");model.return_writable[name]=model.writable[node];model.mutating[name]=false;}
            if(kind==hua::NodeKind::Struct)if(!model.structures.emplace(hua::declared_name(*node),node).second)throw std::runtime_error("duplicate core structure");
            nodes.push_back(std::move(n));raw.push_back(node);
        }
        for(const auto& init:array(field(report,"initialization")))for(const auto& root:array(field(*init,"roots"))){auto i=number(*root);if(!nodes.at(i))throw std::runtime_error("invalid core root");program->add(std::move(nodes[i]));}
        for(const auto& n:nodes)if(n)throw std::runtime_error("disconnected core node");
        for(const auto* n:raw)if(n->kind==hua::NodeKind::Match){
            const auto type=model.types.at(n);auto structure=model.structures.find(type);
            const bool enumeration=structure!=model.structures.end()&&structure->second->text.find(" enum")!=std::string::npos;
            for(std::size_t j=1;j<n->children.size();++j){const auto& p=*n->children[j]->children[0];if(p.kind==hua::NodeKind::Name&&p.text=="_")continue;
                if(enumeration){auto dot=p.text.rfind('.');model.constants[&p]=hua::Value(dot==std::string::npos?p.text:p.text.substr(dot+1));}
                else model.constants[&p]=hua::literal(p);
            }
        }
        hua::RuntimeContext context;context.input=&std::cin;context.working_directory=std::filesystem::current_path();context.execution_limits=execution.limits;context.execution_metrics=metrics;
        if(execute)context.arguments.assign(args.begin()+execution.arguments-1,args.end());
        if(args[0]=="interpret"){hua::Interpreter(model,std::cout,&context).run(*program);return 0;}
        auto code=hua::BytecodeCompiler{}.compile(*program,model);
        hua::verify_bytecode(code,model);
        if(args[0]=="image")instruction_image_json(std::cout,code);
        else if(args[0]=="bytecode")std::cout<<hua::disassemble(code);
        else if(args[0]=="build"){std::vector<const hua::Source*> table;for(const auto& [_,s]:sources)table.push_back(s.get());hua::write_huab(path(args[3]),code,model,table);}
        else hua::VirtualMachine(model,std::cout,&context).run(code);
        return 0;
    }catch(const hua::Diagnostic& e){std::cerr<<"error["<<e.code()<<"]: "<<e.what()<<"\n --> "<<e.span().file_id<<':'<<e.span().line<<':'<<e.span().column<<'\n';return 1;}
    catch(const std::exception& e){std::cerr<<"adapter error: "<<e.what()<<'\n';return 3;}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){
    SetConsoleCP(CP_UTF8);SetConsoleOutputCP(CP_UTF8);std::vector<std::string> args;
    for(int i=1;i<argc;++i){int n=WideCharToMultiByte(CP_UTF8,0,argv[i],-1,nullptr,0,nullptr,nullptr);std::string s(n,'\0');WideCharToMultiByte(CP_UTF8,0,argv[i],-1,s.data(),n,nullptr,nullptr);s.pop_back();args.push_back(std::move(s));}return run(args);
}
#else
int main(int argc,char** argv){return run(std::vector<std::string>(argv+1,argv+argc));}
#endif
