// Decode Hua-produced C06 instructions/metadata. No Parser, sema, lowering, or compiler.
#include "hua/json.hpp"
#include "hua/archive.hpp"
#include "hua/vm.hpp"
#include "c06_instruction_json.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
using J=hua::JsonData;
static const J& field(const J& j,const std::string& k){return *std::get<J::Object>(j.data).at(k);}
static std::string str(const J& j){return std::get<std::string>(j.data);}
static std::size_t number(const J& j){auto n=std::get<std::int64_t>(j.data);if(n<0)throw std::runtime_error("negative image index");return n;}
static bool boolean(const J& j){return std::get<bool>(j.data);}
static const J::Array& array(const J& j){return std::get<J::Array>(j.data);}
static std::filesystem::path path(const std::string& s){return std::filesystem::path(std::u8string(s.begin(),s.end()));}
static hua::Value scalar(std::size_t kind,const std::string& text){
    if(kind==0&&text.empty())return {};
    if(kind==1&&(text=="true"||text=="false"))return hua::Value(text=="true");
    if(kind==4)return hua::Value(text);
    if(kind==2){std::int64_t n{};auto [p,e]=std::from_chars(text.data(),text.data()+text.size(),n);if(e==std::errc{}&&p==text.data()+text.size())return hua::Value(n);}
    if(kind==3){double n{};auto [p,e]=std::from_chars(text.data(),text.data()+text.size(),n);if(e==std::errc{}&&p==text.data()+text.size()&&std::isfinite(n))return hua::Value(n);}
    throw std::runtime_error("invalid tagged scalar");
}
static hua::Value scalar(const J& j){return scalar(number(field(j,"kind")),str(field(j,"text")));}
static void row(const J& j,std::initializer_list<std::string> keys,std::size_t numbers,std::size_t strings){
    const auto& o=std::get<J::Object>(j.data);
    if(o.size()!=keys.size())throw std::runtime_error("invalid packed row fields");
    for(const auto& k:keys)if(!o.contains(k))throw std::runtime_error("missing packed row field");
    if(array(field(j,"n")).size()!=numbers||array(field(j,"s")).size()!=strings)throw std::runtime_error("invalid packed row size");
    for(const auto& n:array(field(j,"n")))(void)std::get<std::int64_t>(n->data);
    for(const auto& t:array(field(j,"s")))str(*t);
}
template<class F>static std::size_t blocks(const J& j,std::size_t maximum,F consume){
    const auto& bs=array(j);if(bs.empty()||bs.size()>maximum)throw std::runtime_error("invalid image blocks");
    std::size_t total=0;
    for(const auto& b:bs){
        auto decoded=hua::json_decode(str(*b));const auto& r=std::get<hua::ResultValue>(decoded.data);
        if(!r.ok)throw std::runtime_error("invalid block JSON");
        const auto& value=*std::get<hua::JsonValue>(r.payload->data).data;const auto& rows=array(value);
        if(rows.empty()||rows.size()>1024||rows.size()>maximum-total)throw std::runtime_error("block record budget");
        std::vector<const J*> pending{&value};std::size_t nodes=0;
        while(!pending.empty()){
            const auto* v=pending.back();pending.pop_back();if(++nodes>65536)throw std::runtime_error("block node budget");
            if(auto a=std::get_if<J::Array>(&v->data))for(const auto& x:*a)pending.push_back(x.get());
            if(auto o=std::get_if<J::Object>(&v->data))for(const auto& [_,x]:*o)pending.push_back(x.get());
        }
        total+=rows.size();for(const auto& item:rows)consume(*item);
    }return total;
}
static int run(const std::vector<std::string>& args){
    if(args.size()<2)return 2;
    const bool execute=args[0]=="run";
    hua::ExecutionOptions execution;
    if(execute){std::vector<std::string> full{"hua_instruction_driver"};full.insert(full.end(),args.begin(),args.end());if(!hua::execution_options(full,execution))return 2;}
    else if(!(args.size()==2&&(args[0]=="verify"||args[0]=="image"||args[0]=="bytecode"))&&!(args.size()==4&&args[0]=="build"&&args[2]=="-o"))return 2;
    auto metrics=execution.report?std::make_shared<hua::ExecutionMetrics>():nullptr;
    hua::ExecutionReport report{execution,metrics,std::cerr};
    try{
        std::ifstream stream(path(args[1]),std::ios::binary|std::ios::ate);
        if(!stream||stream.tellg()<0||stream.tellg()>16*1024*1024)throw std::runtime_error("cannot read bounded image");
        stream.seekg(0);hua::ManagedHeap heap{65536};hua::HeapScope scope{heap};
        auto decoded=hua::json_decode(std::string(std::istreambuf_iterator<char>(stream),{}));
        const auto& r=std::get<hua::ResultValue>(decoded.data);if(!r.ok)throw std::runtime_error("invalid image JSON");
        const auto& j=*std::get<hua::JsonValue>(r.payload->data).data;
        const bool packed=str(field(j,"schema"))=="hua.bytecode.image.v2";
        if((!packed&&str(field(j,"schema"))!="hua.bytecode.image.v1")||str(field(j,"profile"))!="C06-BC1"||str(field(j,"status"))!="ok")throw std::runtime_error("invalid image envelope");
        std::map<std::string,std::unique_ptr<hua::Source>> sources;
        for(const auto& s:array(field(j,"sources"))){const auto module=str(field(*s,"module"));if(!sources.emplace(module,std::make_unique<hua::Source>(module,str(field(*s,"text")))).second)throw std::runtime_error("duplicate source");}
        if(!sources.contains(str(field(j,"entry"))))throw std::runtime_error("missing entry");
        std::vector<hua::SourceSpan> sites;
        if(packed){
            if(std::get<J::Object>(j.data).contains("sites"))throw std::runtime_error("mixed sites transport");
            std::set<std::int64_t> ids;
            blocks(field(j,"site_blocks"),262145,[&](const J& r){
                row(r,{"n","s"},6,2);const auto& n=array(field(r,"n"));const auto& s=array(field(r,"s"));
                const auto id=std::get<std::int64_t>(n[0]->data),origin=std::get<std::int64_t>(n[1]->data);const auto module=str(*s[0]),role=str(*s[1]);
                if(!ids.insert(id).second)throw std::runtime_error("duplicate site identity");
                if(id==-1){if(origin!=-1||role!="initializer"||module!=str(field(j,"entry"))||number(*n[2])||number(*n[3]))throw std::runtime_error("invalid initializer site");}
                else if(id<0||id>=262144||origin<0||origin>=32768||(role!="source"&&role!="enum-layout"&&role!="enum-constructor"))throw std::runtime_error("invalid site origin");
                auto span=sources.at(module)->span(number(*n[2]),number(*n[3]));
                if(span.line!=number(*n[4])||span.column!=number(*n[5]))throw std::runtime_error("invalid packed coordinates");sites.push_back(span);
            });
        }else for(const auto& s:array(field(j,"sites"))){auto span=sources.at(str(field(*s,"module")))->span(number(field(*s,"start")),number(field(*s,"end")));if(span.line!=number(field(*s,"line"))||span.column!=number(field(*s,"column")))throw std::runtime_error("invalid source coordinates");sites.push_back(span);}
        auto site=[&](const J& record){return sites.at(number(field(record,"site")));};
        std::vector<hua::NodePtr> declarations;hua::SemanticModel model;
        for(const auto& r:array(field(j,"structures"))){
            auto name=str(field(*r,"name"));auto n=std::make_unique<hua::Node>(hua::NodeKind::Struct,site(*r),name+(boolean(field(*r,"enumeration"))?" enum":"")+(boolean(field(*r,"published"))?" pub":""));
            for(const auto& f:array(field(*r,"fields"))){auto child=std::make_unique<hua::Node>(hua::NodeKind::Field,site(*f),str(field(*f,"name")));child->add(std::make_unique<hua::Node>(hua::NodeKind::TypeName,site(*f),str(field(*f,"type_name"))));n->add(std::move(child));}
            if(!model.structures.emplace(name,n.get()).second)throw std::runtime_error("duplicate structure");declarations.push_back(std::move(n));
        }
        for(const auto& r:array(field(j,"functions"))){
            auto name=str(field(*r,"name"));auto n=std::make_unique<hua::Node>(hua::NodeKind::Function,site(*r),name+(boolean(field(*r,"published"))?" pub":""));
            for(const auto& p:array(field(*r,"parameters"))){auto child=std::make_unique<hua::Node>(hua::NodeKind::Parameter,site(*p),str(field(*p,"name"))+(boolean(field(*p,"writable"))?" mut":""));const auto t=str(field(*p,"type_name"));if(!t.empty())child->add(std::make_unique<hua::Node>(hua::NodeKind::TypeName,site(*p),t));n->add(std::move(child));}
            if(!array(field(*r,"returns")).empty()){auto rs=std::make_unique<hua::Node>(hua::NodeKind::ReturnTypes,site(*r));for(const auto& t:array(field(*r,"returns")))rs->add(std::make_unique<hua::Node>(hua::NodeKind::TypeName,site(*r),str(*t)));n->add(std::move(rs));}
            if(!model.functions.emplace(name,n.get()).second)throw std::runtime_error("duplicate function");model.return_writable[name]=boolean(field(*r,"writable"));model.mutating[name]=false;declarations.push_back(std::move(n));
        }
        std::size_t count=0;
        auto read_code=[&](const J& j){
            hua::Code code;code.name=str(field(j,"name"));
            if(packed){
                const auto& object=std::get<J::Object>(j.data);if(object.size()!=3||!object.contains("blocks")||!object.contains("count"))throw std::runtime_error("invalid packed code");
                const auto expected=number(field(j,"count"));if(!expected||expected>250000||expected>1000000-count)throw std::runtime_error("packed instruction budget");
                const auto actual=blocks(field(j,"blocks"),expected,[&](const J& r){
                    row(r,{"n","s","a","c"},6,3);const auto& n=array(field(r,"n"));const auto& s=array(field(r,"s"));
                    hua::Instruction op{};const auto tag=number(*n[0]);if(tag>40||tag==31||tag==39)throw std::runtime_error("unknown/excluded opcode");
                    op.op=static_cast<hua::Op>(tag);op.span=sites.at(number(*n[1]));op.argument=number(*n[2]);op.target=number(*n[3]);const auto flag=number(*n[4]);if(flag>1||op.argument>4096)throw std::runtime_error("invalid packed operand");op.flag=flag;
                    op.constant=scalar(number(*n[5]),str(*s[2]));op.text=str(*s[0]);op.type=str(*s[1]);
                    if(array(field(r,"a")).size()>4096||array(field(r,"c")).size()>4096)throw std::runtime_error("packed vector budget");
                    for(const auto& name:array(field(r,"a")))op.names.push_back(str(*name));for(const auto& flag:array(field(r,"c")))op.contextual.push_back(boolean(*flag));code.instructions.push_back(std::move(op));
                });
                if(actual!=expected)throw std::runtime_error("packed code count mismatch");count+=actual;return code;
            }
            const auto& ops=array(field(j,"instructions"));if(ops.empty()||ops.size()>250000)throw std::runtime_error("invalid code size");count+=ops.size();if(count>1000000)throw std::runtime_error("instruction budget");
            for(const auto& r:ops){
                hua::Instruction op{};auto tag=number(field(*r,"op"));if(tag>40)throw std::runtime_error("unknown opcode");op.op=static_cast<hua::Op>(tag);op.span=site(*r);op.text=str(field(*r,"text"));op.type=str(field(*r,"type_name"));op.argument=number(field(*r,"argument"));op.target=number(field(*r,"target"));op.flag=boolean(field(*r,"flag"));op.constant=scalar(field(*r,"constant"));for(const auto& n:array(field(*r,"names")))op.names.push_back(str(*n));for(const auto& c:array(field(*r,"contextual")))op.contextual.push_back(boolean(*c));code.instructions.push_back(std::move(op));
            }return code;
        };
        hua::Bytecode code;code.initializer=read_code(field(j,"initializer"));if(code.initializer.name!="<initialize>")throw std::runtime_error("invalid initializer");
        for(const auto& f:array(field(j,"functions"))){auto c=read_code(field(*f,"code"));if(c.name!=str(field(*f,"name")))throw std::runtime_error("function code identity mismatch");if(!code.functions.emplace(model.functions.at(c.name),std::move(c)).second)throw std::runtime_error("duplicate code");}
        if(count!=number(field(j,"instructions")))throw std::runtime_error("instruction count mismatch");
        hua::verify_bytecode(code,model);
        if(args[0]=="verify"){std::cout<<"verified "<<count<<"\n";return 0;}
        if(args[0]=="image"){instruction_image_json(std::cout,code);return 0;}
        if(args[0]=="bytecode"){std::cout<<hua::disassemble(code);return 0;}
        if(args[0]=="build"){std::vector<const hua::Source*> table;for(const auto& [_,s]:sources)table.push_back(s.get());hua::write_huab(path(args[3]),code,model,table);return 0;}
        hua::RuntimeContext context;context.input=&std::cin;context.working_directory=std::filesystem::current_path();context.execution_limits=execution.limits;context.execution_metrics=metrics;context.arguments.assign(args.begin()+execution.arguments-1,args.end());
        hua::VirtualMachine(model,std::cout,&context).run(code);return 0;
    }catch(const hua::Diagnostic& e){std::cerr<<"error["<<e.code()<<"]: "<<e.what()<<"\n --> "<<e.span().file_id<<':'<<e.span().line<<':'<<e.span().column<<'\n';return 1;}
    catch(const std::exception& e){std::cerr<<"adapter error: "<<e.what()<<'\n';return 3;}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){SetConsoleCP(CP_UTF8);SetConsoleOutputCP(CP_UTF8);std::vector<std::string> args;for(int i=1;i<argc;++i){int n=WideCharToMultiByte(CP_UTF8,0,argv[i],-1,nullptr,0,nullptr,nullptr);std::string s(n,'\0');WideCharToMultiByte(CP_UTF8,0,argv[i],-1,s.data(),n,nullptr,nullptr);s.pop_back();args.push_back(std::move(s));}return run(args);}
#else
int main(int argc,char** argv){return run(std::vector<std::string>(argv+1,argv+argc));}
#endif
