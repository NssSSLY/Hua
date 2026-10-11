// A01 audit only. Reuse the host loader, semantic checker and executors.
#include "hua/module.hpp"
#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include "hua/sema.hpp"
#include "hua/interpreter.hpp"
#include "hua/vm.hpp"
#include "hua/archive.hpp"
#include <fstream>
#include <iostream>
#include <unordered_set>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace {
using N=hua::NodeKind;
[[noreturn]] void excluded(const hua::Node& n,const std::string& detail) {
    throw hua::Diagnostic("E8101",n.span,"SB1 source subset excludes: "+detail);
}
void validate(const hua::Node& n,N parent=N::Program) {
    // Closed allowlist: adding a syntax node cannot silently enlarge SB1.
    switch(n.kind) {
    case N::Program:case N::Block:case N::Let:case N::Var:case N::Const:
    case N::Parameter:case N::ReturnTypes:case N::Field:case N::Import:
    case N::If:case N::While:case N::For:case N::Return:case N::Break:case N::Continue:
    case N::ExpressionStatement:case N::Integer:case N::Float:case N::String:
    case N::Boolean:case N::Nil:case N::Name:case N::Unary:case N::Binary:
    case N::Assignment:case N::Update:case N::Call:case N::Index:case N::Slice:
    case N::Member:case N::Array:case N::StructLiteral:case N::FieldInit:case N::Range:
    case N::Omitted:case N::TypeName:case N::SliceType:case N::ArrayType:
    case N::GenericType:case N::OptionalType:case N::MutableType:case N::MapLiteral:
    case N::MapEntry:case N::Pack:case N::BindingList:case N::MultiBinding:
    case N::MultiAssignment:case N::Propagate:case N::Defer:case N::Enum:
    case N::Variant:case N::Match:case N::MatchArm:case N::Function:case N::Struct:break;
    default:excluded(n,std::string(hua::node_name(n.kind)));
    }
    if(n.kind==N::Function||n.kind==N::Struct) {
        if(parent!=N::Program)excluded(n,"nested function/struct");
        if(n.text.find(" async")!=std::string::npos||n.text.find(" unsafe")!=std::string::npos||n.text.find(" @")!=std::string::npos)
            excluded(n,"async/unsafe/attribute declaration");
        auto name=n.text.substr(0,n.text.find(' '));
        if(name.starts_with("__"))excluded(n,"reserved internal declaration name");
        if(n.kind==N::Function&&name.find('.')!=std::string::npos)excluded(n,"method declaration");
    }
    if(n.kind==N::Block&&n.text=="taskgroup")excluded(n,"taskgroup");
    if(n.kind==N::For&&!n.text.empty())excluded(n,"parallel/SIMD loop");
    if(n.kind==N::Unary&&(n.text=="await"||n.text=="spawn"))excluded(n,n.text);
    if(n.kind==N::Name&&n.text.starts_with("__"))excluded(n,"internal runtime name");
    if(n.kind==N::TypeName&&(n.text=="Task"||n.text=="std.file.File"||n.text=="std.file.IOFailure"||n.text=="std.error.Value"))
        excluded(n,"Task/opaque resource/structured Error type");
    if(n.kind==N::GenericType) {
        if(n.text!="Result"&&n.text!="List"&&n.text!="map")excluded(n,"generic type "+n.text);
        if(n.text=="Result"&&n.children.size()==2&&(n.children[1]->kind!=N::TypeName||n.children[1]->text!="string"))
            excluded(n,"Result error type other than string");
    }
    if(n.kind==N::Import&&(n.text=="std"||n.text.starts_with("std."))) {
        static const std::unordered_set<std::string> modules={"std.io","std.os","std.fs","std.path","std.strings","std.bytes","std.list","std.buffer","std.json","std.math","std.alg","std.time"};
        if(!modules.contains(n.text))excluded(n,"standard module "+n.text);
    }
    for(const auto& c:n.children)validate(*c,n.kind);
}
int cli(const std::vector<std::string>& args) {
    if(args.size()<3){std::cerr<<"Internal A01 audit: hua_subset_probe <check|run|interpret|build> file.hua [-- args | -o file.huab]\n";return 2;}
    const auto action=args[1];bool execute=action=="run"||action=="interpret";
    if(action!="check"&&!execute&&action!="build")return 2;
    hua::ExecutionOptions execution;
    if(execute?!hua::execution_options(args,execution):(args.size()!=3&&!(action=="build"&&args.size()==5&&args[3]=="-o")))return 2;
    auto metrics=execution.report?std::make_shared<hua::ExecutionMetrics>():nullptr;
    hua::ExecutionReport budget_report{execution,metrics,std::cerr};
    hua::ModuleLoader loader;std::unique_ptr<hua::Source> entry_source;
    try {
        auto path=std::filesystem::path(std::u8string(args[2].begin(),args[2].end()));
        if(path.extension()!=".hua")throw hua::Diagnostic("E8101",{},"SB1 audit requires .hua source, not an external module or HUAB");
        std::ifstream input(path,std::ios::binary|std::ios::ate);
        if(!input)throw hua::Diagnostic("E0001",{},"cannot read SB1 entry source");
        auto size=input.tellg();if(size<0||size>16*1024*1024)throw hua::Diagnostic("E0001",{},"SB1 entry exceeds source limit");
        std::string text(static_cast<std::size_t>(size),'\0');input.seekg(0);
        if(size&&!input.read(text.data(),size))throw hua::Diagnostic("E0001",{},"cannot read complete SB1 entry source");
        entry_source=std::make_unique<hua::Source>(args[2],std::move(text));
        validate(*hua::Parser(hua::Lexer(*entry_source).scan()).parse());
        const auto& tree=loader.load(path);
        // Loader only reads/links/lowers and registers metadata. It never calls
        // ExternalRegistry::initialize. Validate the entire local closure before
        // semantic compilation, executing statements or writing an artifact.
        for(const auto* source:loader.sources()) {
            if((source->filename().starts_with("<std.")&&source->filename().ends_with(">")))continue;
            auto origin=std::filesystem::path(std::u8string(source->filename().begin(),source->filename().end()));
            if(origin.extension()!=".hua")throw hua::Diagnostic("E8101",source->span(0,0),"SB1 source subset excludes: Native/WASM module");
            validate(*hua::Parser(hua::Lexer(*source).scan()).parse());
        }
        auto model=hua::SemanticAnalyzer{}.analyze(tree,loader.externals());
        auto bytecode=hua::BytecodeCompiler{}.compile(tree,model);
        if(action=="check"){std::cout<<"SB1 OK\n";return 0;}
        if(action=="build") {
            auto target=path;target.replace_extension(".huab");
            if(args.size()==5)target=std::filesystem::path(std::u8string(args[4].begin(),args[4].end()));
            if(target.extension()!=".huab")return 2;
            hua::write_huab(target,bytecode,model,loader.sources());std::cout<<"Built SB1 audit HUAB\n";return 0;
        }
        hua::RuntimeContext context;context.input=&std::cin;context.working_directory=std::filesystem::current_path();
        context.execution_limits=execution.limits;context.execution_metrics=metrics;
        if(execute)context.arguments.assign(args.begin()+execution.arguments,args.end());
        if(action=="interpret")hua::Interpreter(model,std::cout,&context).run(tree);
        else hua::VirtualMachine(model,std::cout,&context).run(bytecode);
        return 0;
    }catch(const hua::Diagnostic& d) {
        auto source=loader.source(d.span().file_id);
        if(!source&&entry_source&&entry_source->filename()==d.span().file_id)source=entry_source.get();
        if(source)std::cerr<<d.render(*source);else std::cerr<<"error["<<d.code()<<"]: "<<d.what()<<'\n';
        return 1;
    }catch(const std::exception& e){std::cerr<<"error[E0001]: "<<e.what()<<'\n';return 1;}
}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){
    SetConsoleCP(CP_UTF8);SetConsoleOutputCP(CP_UTF8);std::vector<std::string> args;
    for(int i=0;i<argc;++i){auto n=WideCharToMultiByte(CP_UTF8,0,argv[i],-1,nullptr,0,nullptr,nullptr);if(n<=0)return 2;
        std::string s(static_cast<std::size_t>(n),'\0');WideCharToMultiByte(CP_UTF8,0,argv[i],-1,s.data(),n,nullptr,nullptr);s.pop_back();args.push_back(std::move(s));}
    return cli(args);
}
#else
int main(int argc,char** argv){return cli(std::vector<std::string>(argv,argv+argc));}
#endif
