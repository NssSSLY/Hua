// C02 reference only: independently use the existing C++ lexer/parser.
#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include "hua/module.hpp"
#include "hua/sema.hpp"
#include "hua/runtime.hpp"
#include "hua/stdlib.hpp"
#include <algorithm>
#include <set>
#include <map>
#include <functional>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
static void quote(const std::string& s) {
    constexpr char hex[]="0123456789abcdef";
    std::cout << '"';
    for(unsigned char c:s) {
        if(c=='"'||c=='\\') std::cout << '\\' << c;
        else if(c<32) std::cout << "\\u00" << hex[c>>4] << hex[c&15];
        else std::cout << c;
    }
    std::cout << '"';
}
static void nodes(const hua::Node& n,unsigned depth,bool& first) {
    if(!first)std::cout<<','; first=false;
    std::cout<<"{\"kind\":";quote(std::string(hua::node_name(n.kind)));
    std::cout<<",\"text\":";quote(n.text);
    std::cout<<",\"start\":"<<n.span.start_offset<<",\"end\":"<<n.span.end_offset
             <<",\"line\":"<<n.span.line<<",\"column\":"<<n.span.column
             <<",\"depth\":"<<depth<<",\"arity\":"<<n.children.size()<<'}';
    for(const auto& child:n.children)nodes(*child,depth+1,first);
}
static int parse(const std::string& name) {
    std::ifstream input(std::filesystem::path(std::u8string(name.begin(),name.end())),std::ios::binary);
    if(!input)return 2;
    hua::Source source("input.hua",std::string(std::istreambuf_iterator<char>(input),{}));
    try {
        const auto tree=hua::Parser(hua::Lexer(source).scan()).parse();bool first=true;
        std::cout<<"{\"status\":\"ok\",\"nodes\":[";nodes(*tree,0,first);std::cout<<"]}\n";
    } catch(const hua::Diagnostic& error) {
        std::cout<<"{\"status\":\"error\",\"code\":";quote(error.code());
        std::cout<<",\"start\":"<<error.span().start_offset<<",\"end\":"<<error.span().end_offset
                 <<",\"line\":"<<error.span().line<<",\"column\":"<<error.span().column<<"}\n";
    }
    return 0;
}

static std::filesystem::path path(const std::string& s) {return std::filesystem::path(std::u8string(s.begin(),s.end()));}
static std::string text(const std::filesystem::path& p) {const auto s=p.generic_u8string();return std::string(s.begin(),s.end());}
static int catalog() {
    const std::set<std::string> modules={"alg","buffer","bytes","fs","io","json","list","math","os","path","strings","time"};
    std::set<std::string> core;
    for(const auto& name:hua::builtin_names())if(!hua::standard_function(name)&&name!="task_start"&&name!="task_await"&&name!="task_group_begin"&&name!="task_group_end"&&name!="parallel_map"&&name!="simd_check")core.insert(name);
    std::cout<<"{\"core\":[";bool first=true;
    for(const auto& n:core){if(!first)std::cout<<',';first=false;quote(n);}
    std::cout<<"],\"modules\":{";first=true;
    for(const auto& m:modules){if(!first)std::cout<<',';first=false;quote("std."+m);std::cout<<":[";bool next=true;
        for(const auto& f:hua::standard_functions())if(f.module==m){if(!next)std::cout<<',';next=false;quote(f.name);}
        if(m=="json"){if(!next)std::cout<<',';quote("Value");}std::cout<<']';}
    std::cout<<"}}\n";return 0;
}
static int modules(const std::string& name,bool check) {
    const auto entry=std::filesystem::weakly_canonical(path(name));const auto root=entry.parent_path();hua::ModuleLoader loader;
    try {
        const auto& linked=loader.load(entry);
        if(check)(void)hua::SemanticAnalyzer().analyze(linked,loader.externals());
        std::cout<<"{\"status\":\"ok\",\"modules\":[";bool first=true;
        for(const auto* source:loader.sources()){
            if(source->filename().starts_with('<'))continue;
            if(!first)std::cout<<',';first=false;
            std::cout<<"{\"id\":";quote(text(path(source->filename()).lexically_relative(root)));std::cout<<",\"declarations\":[";
            const auto tree=hua::Parser(hua::Lexer(*source).scan()).parse();bool next=true;
            auto declaration=[&](const hua::Node& node,const std::string& nm,const std::string& kind,bool pub){
                if(nm=="_")return;if(!next)std::cout<<',';next=false;
                std::cout<<"{\"name\":";quote(nm);std::cout<<",\"kind\":";quote(kind);std::cout<<",\"published\":"<<(pub?"true":"false")<<'}';};
            for(const auto& n:tree->children){const auto nm=n->text.substr(0,n->text.find(' '));const bool pub=n->text.ends_with(" pub");using N=hua::NodeKind;
                if(n->kind==N::Function||n->kind==N::Struct||n->kind==N::Enum||n->kind==N::Let||n->kind==N::Var||n->kind==N::Const)declaration(*n,nm,std::string(hua::node_name(n->kind)),pub);
                if(n->kind==N::Enum)for(const auto& v:n->children)declaration(*v,nm+"."+v->text,"Variant",pub);
                if(n->kind==N::MultiBinding)for(const auto& v:n->children[0]->children)declaration(*v,v->text,n->text,false);
            }
            std::cout<<"]}";
        }
        std::cout<<"]}\n";
    }catch(const hua::Diagnostic& error){
        std::cout<<"{\"status\":\"error\",\"code\":";quote(error.code());std::cout<<",\"source\":";
        const auto& id=error.span().file_id;quote(id.starts_with('<')?id:text(path(id).lexically_relative(root)));
        std::cout<<",\"line\":"<<error.span().line<<",\"column\":"<<error.span().column<<"}\n";
    }
    return 0;
}

static int signatures() {
    const std::set<std::string> modules={"alg","buffer","bytes","fs","io","json","list","math","os","path","strings","time"};
    std::cout<<"[";bool first=true;
    for(const auto& f:hua::standard_functions())if(modules.contains(f.module)){
        if(!first)std::cout<<',';first=false;std::cout<<"{\"target\":";quote("builtin:std."+f.module+"#"+f.name);
        std::cout<<",\"parameters\":[";bool next=true;for(const auto& p:f.parameters){if(!next)std::cout<<',';next=false;quote(p);}
        std::cout<<"],\"result\":";quote(f.result);std::cout<<",\"mutates\":"<<(f.mutates_first?"true":"false")<<",\"writable\":"<<(f.writable_result?"true":"false")<<"}";
    }
    std::cout<<"]\n";return 0;
}
static int types(const std::string& name) {
    const auto entry=std::filesystem::weakly_canonical(path(name));const auto root=entry.parent_path();hua::ModuleLoader loader;
    try {
        const auto& tree=loader.load(entry);auto model=hua::SemanticAnalyzer().analyze(tree,loader.externals());
        auto module=[&](const hua::Node& n){return text(path(n.span.file_id).lexically_relative(root));};
        std::map<std::string,std::string> names;
        for(const auto& [key,n]:model.structures)if(!n->span.file_id.starts_with('<'))names[key]=module(*n)+"#"+hua::display_type(key);
        for(const auto& [key,n]:model.functions)if(!n->span.file_id.starts_with('<'))names[key]=module(*n)+"#"+hua::display_type(key);
        auto normalize=[&](const std::string& t){
            std::string out;
            for(std::size_t i=0;i<t.size();){
                if(std::isalnum(static_cast<unsigned char>(t[i]))||t[i]=='_'||t[i]=='$'||t[i]=='.'){
                    auto start=i;while(i<t.size()&&(std::isalnum(static_cast<unsigned char>(t[i]))||t[i]=='_'||t[i]=='$'||t[i]=='.'))++i;
                    const auto word=t.substr(start,i-start);auto found=names.find(word);out+=found==names.end()?word:found->second;
                }else out+=t[i++];
            }
            if(out.starts_with("builtin:$std$")){auto pos=out.find('$',13);if(pos!=std::string::npos)out="builtin:std."+out.substr(13,pos-13)+"#"+out.substr(pos+1);}
            else if(out.starts_with("builtin:"))out="builtin:core:"+out.substr(8);
            if(out.starts_with("fn:")){auto actual=t.substr(3);auto dot=actual.rfind('.');if(dot!=std::string::npos){auto found=model.structures.find(actual.substr(0,dot));if(found!=model.structures.end()&&found->second->text.find(" enum")!=std::string::npos)out="variant:"+out.substr(3);}}
            return out;
        };
        std::cout<<"{\"status\":\"ok\",\"facts\":[";bool first=true;
        std::function<void(const hua::Node&)> visit=[&](const hua::Node& n){
            if(auto found=model.types.find(&n);found!=model.types.end()&&!found->second.empty()&&!n.span.file_id.starts_with('<')){
                if(!first)std::cout<<',';first=false;std::cout<<"{\"module\":";quote(module(n));std::cout<<",\"kind\":";quote(std::string(hua::node_name(n.kind)));
                std::cout<<",\"start\":"<<n.span.start_offset<<",\"end\":"<<n.span.end_offset<<",\"type_name\":";quote(normalize(found->second));
                std::cout<<",\"writable\":"<<(model.writable.contains(&n)?(model.writable.at(&n)?"true":"false"):"null")<<"}";
            }
            for(const auto& c:n.children)visit(*c);
        };visit(tree);
        std::cout<<"],\"functions\":[";first=true;
        for(const auto& [key,n]:model.functions)if(!n->span.file_id.starts_with('<')&&!model.nested.contains(n)){
            if(!first)std::cout<<',';first=false;std::cout<<"{\"target\":";quote(names.at(key));std::cout<<",\"writable\":"<<(model.return_writable.at(key)?"true":"false")<<"}";
        }
        std::cout<<"]}\n";
    }catch(const hua::Diagnostic& error){
        std::cout<<"{\"status\":\"error\",\"code\":";quote(error.code());std::cout<<",\"source\":";quote(text(path(error.span().file_id).lexically_relative(root)));
        std::cout<<",\"line\":"<<error.span().line<<",\"column\":"<<error.span().column<<"}\n";
    }
    return 0;
}

static int run(const std::vector<std::string>& a){
    if(a.size()==1&&a[0]=="--signatures")return signatures();
    if(a.size()==2&&a[0]=="--types")return types(a[1]);
    if(a.size()==1&&a[0]=="--catalog")return catalog();
    if(a.size()==2&&(a[0]=="--modules"||a[0]=="--load"))return modules(a[1],a[0]=="--modules");
    return a.size()==1?parse(a[0]):2;
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    std::vector<std::string> args;
    for(int i=1;i<argc;++i){const int size=WideCharToMultiByte(CP_UTF8,0,argv[i],-1,nullptr,0,nullptr,nullptr);if(size<=0)return 2;
        std::string s(size,'\0');WideCharToMultiByte(CP_UTF8,0,argv[i],-1,s.data(),size,nullptr,nullptr);s.pop_back();args.push_back(std::move(s));}
    return run(args);
}
#else
int main(int argc,char** argv){return run(std::vector<std::string>(argv+1,argv+argc));}
#endif
