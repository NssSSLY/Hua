#include "hua/native_backend.hpp"
#include "hua/ast.hpp"
#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include "hua/sema.hpp"
#include "hua/interpreter.hpp"
#include "hua/module.hpp"
#include "hua/vm.hpp"
#include "hua/archive.hpp"
#include "hua/lsp.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "release_inputs.hpp"
#include "native_run.hpp"
#endif
namespace {
void usage(std::ostream& out) {
    out << "Hua language\n"
        << "Usage: hua version\n"
        << "       hua check <file.hua|file.huab>\n"
        << "       hua ast <file.hua>\n"
        << "       hua run <file.hua|file.huab> [--execution-profile default|compiler] [--report-budget] [-- args...]\n"
        << "       hua run --release <file.hua> [-- args...]\n"
        << "       hua interpret <file.hua> [--execution-profile default|compiler] [--report-budget] [-- args...]\n"
        << "       hua bytecode <file.hua|file.huab>\n"
        << "       hua build <file.hua> [-o file.huab]\n"
        << "       hua build --release <file.hua> [-o file.exe]\n"
        << "       hua test <file.hua> [--filter text]\n"
        << "       hua lsp\n";
}
int usage_error() { std::cerr<<"error[E0004]: invalid command arguments\n";usage(std::cerr);return 2; }
// hua test (2026-10-24): runs each top-level @test function of a source file, in
// source order, as the entry of a fresh VM and execution context (module state is
// initialized again for every test). Report on stdout; exit 0 only if all pass.
int run_tests(const std::vector<std::string>& args) {
    std::string filter;
    if(args.size()==5&&args[3]=="--filter")filter=args[4];
    else if(args.size()!=3)return usage_error();
    auto path=std::filesystem::path(std::u8string(args[2].begin(),args[2].end()));
    if(path.extension()!=".hua"){std::cerr<<"error[E0004]: hua test requires a .hua source file\n";return 2;}
    hua::ModuleLoader loader;
    std::vector<std::pair<std::string,hua::SourceSpan>> tests;
    std::unique_ptr<hua::SemanticModel> model;std::unique_ptr<hua::Bytecode> bytecode;
    try{
        const auto& tree=loader.load(path);
        model=std::make_unique<hua::SemanticModel>(hua::SemanticAnalyzer{}.analyze(tree,loader.externals()));
        bytecode=std::make_unique<hua::Bytecode>(hua::BytecodeCompiler{}.compile(tree,*model));
        for(const auto& n:tree.children)if(n->kind==hua::NodeKind::Function&&n->text.find(" @test")!=std::string::npos){
            auto name=hua::declared_name(*n);if(name.find(filter)!=std::string::npos)tests.emplace_back(name,n->span);
        }
    }catch(const hua::Diagnostic& error){
        if(const auto* origin=loader.source(error.span().file_id))std::cerr<<error.render(*origin);
        else std::cerr<<"error["<<error.code()<<"]: "<<error.what()<<'\n';
        return hua::diagnostic_exit_code(error.code());
    }
    std::size_t passed=0,failed=0;
    for(const auto& [name,span]:tests){
        std::string failure;
        try{
            hua::reset_report_state();
            hua::RuntimeContext context;context.input=&std::cin;context.working_directory=std::filesystem::current_path();
            hua::VirtualMachine vm(*model,std::cout,&context);vm.set_entry(name);
            auto result=vm.run(*bytecode);
            if(result.exit_code){failure=result.message;if(failure.rfind("main returned Err: ",0)==0)failure="returned Err: "+failure.substr(19);}
        }catch(const hua::Diagnostic& error){
            failure=error.code()+": "+error.what();
            if(!error.span().file_id.empty())failure+=" ("+error.span().file_id+":"+std::to_string(error.span().line)+":"+std::to_string(error.span().column)+")";
        }catch(const hua::ProgramExit& exit){failure="test called os.exit("+std::to_string(exit.code)+")";}
        std::cout.flush();
        if(failure.empty()){++passed;std::cout<<"PASS "<<name<<'\n';}
        else{++failed;std::cout<<"FAIL "<<name<<": "<<failure<<'\n';}
    }
    std::cout<<passed<<" passed, "<<failed<<" failed\n";
    return failed?1:0;
}
int cli(const std::vector<std::string>& args, std::ostream& output = std::cout) {
#ifdef _WIN32
    if (args.size() >= 4 && args[1] == "--release-sdk-inputs-v2") return hua::release_inputs::grouped_command(args);
    if (args.size() >= 4 && args[1] == "--release-sdk-inputs") return hua::release_inputs::command(args);
#endif
    if(args.size()>=3&&args[1]=="test")return run_tests(args);
    if (args.size() == 2 && args[1] == "lsp") return hua::run_lsp();
    if (args.size() == 2 && (args[1] == "version" || args[1] == "--version")) {
        output << "Hua 0.1.0-dev\nspec 0.1\nabi 1\nbytecode 6\n"; return 0;
    }
    if (args.size() == 2 && (args[1] == "--help" || args[1] == "help")) { usage(output); return 0; }
    if(args.size()==5&&args[1]=="native"&&args[3]=="-o"){
        hua::ModuleLoader native_loader;
        try{
            const auto& tree=native_loader.load(std::filesystem::path(std::u8string(args[2].begin(),args[2].end())));auto model=hua::SemanticAnalyzer{}.analyze(tree,native_loader.externals());
            const auto code=hua::emit_native(tree,model);
            std::ofstream out(std::filesystem::path(std::u8string(args[4].begin(),args[4].end())),std::ios::binary);out<<code;
            if(!out){std::cerr<<"error[E0001]: cannot write "<<args[4]<<char(10);return 1;}
            output<<"Generated "<<args[4]<<char(10);return 0;
        }catch(const hua::NativeUnsupported& u){std::cerr<<"native: unsupported "<<u.what<<" at "<<u.span.file_id<<':'<<u.span.line<<':'<<u.span.column<<char(10);return 2;}
        catch(const hua::Diagnostic& d){std::cerr<<"error["<<d.code()<<"]: "<<d.what()<<char(10);return 1;}
    }
    if(args.size()==3&&args[1]=="typed-ast"){
        hua::ModuleLoader typed_loader;
        try{const auto& tree=typed_loader.load(std::filesystem::path(std::u8string(args[2].begin(),args[2].end())));auto model=hua::SemanticAnalyzer{}.analyze(tree,typed_loader.externals());output<<hua::dump_typed_ast(tree,model);return 0;}
        catch(const hua::Diagnostic& d){std::cerr<<"error["<<d.code()<<"]: "<<d.what()<<char(10);return 1;}
    }
    if (args.size() >= 3 && args[1] == "build" && args[2] == "--release") {
        if (!(args.size() == 4 || (args.size() == 6 && args[4] == "-o"))) return usage_error();
#ifdef _WIN32
        const std::filesystem::path binary_dir = [] {
            std::wstring binary(32768, L'\0');
            const auto size = GetModuleFileNameW(nullptr, binary.data(), static_cast<DWORD>(binary.size()));
            if (size == 0 || size >= binary.size()) throw std::runtime_error("cannot locate Hua executable");
            binary.resize(size);
            return std::filesystem::path(binary).parent_path();
        }();
        const auto sdk = binary_dir.parent_path();
        const auto driver = sdk / "lib" / "compiler" / "release.huab";
        const auto driver_path = driver.u8string();
        const std::string driver_utf8(driver_path.begin(), driver_path.end());
        if (!std::filesystem::is_regular_file(driver)) {
            std::cerr << "error[E0001]: release driver missing from Hua SDK: " << driver_utf8 << '\n';
            return 1;
        }
        const auto sdk_path = sdk.u8string();
        std::vector<std::string> command{args[0], "run",
            driver_utf8, "--execution-profile", "compiler", "--",
            std::string(sdk_path.begin(), sdk_path.end())};
        command.push_back(args[3]);
        if (args.size() == 6) command.push_back(args[5]);
        return cli(command, output);
#else
        std::cerr << "error[E0004]: release builds require the Windows x64 Hua SDK\n";
        return 2;
#endif
    }
    if (args.size() >= 3 && args[1] == "run" && args[2] == "--release") {
        if (args.size() < 4 || (args.size() > 4 && args[4] != "--")) return usage_error();
#ifdef _WIN32
        const hua::native_run::TemporaryProgram program;
        std::ostringstream receipt;
        const auto status = cli({args[0], "build", "--release", args[3], "-o",
                                 hua::native_run::utf8(program.image())}, receipt);
        if (status != 0) return status;
        return hua::native_run::execute(program.image(), args);
#else
        std::cerr << "error[E0004]: native runs require the Windows x64 Hua SDK\n";
        return 2;
#endif
    }
    bool build = args.size() >= 3 && args[1] == "build";
    bool execute=args.size()>=3&&(args[1]=="run"||args[1]=="interpret");
    hua::ExecutionOptions execution;
    bool valid_size=execute?hua::execution_options(args,execution):args.size()==3;
    if (build ? !(args.size() == 3 || (args.size() == 5 && args[3] == "-o")) :
        (!valid_size || (args[1] != "check" && args[1] != "ast" && args[1] != "run" && args[1] != "interpret" && args[1] != "bytecode"))) { return usage_error(); }
    auto metrics=execution.report?std::make_shared<hua::ExecutionMetrics>():nullptr;
    hua::ExecutionReport budget_report{execution,metrics,std::cerr};
    std::unique_ptr<hua::Source> source;
    hua::ModuleLoader loader;
    std::unique_ptr<hua::BytecodeImage> image;
    try {
        hua::RuntimeContext context;context.input=&std::cin;context.working_directory=std::filesystem::current_path();context.execution_limits=execution.limits;context.execution_metrics=metrics;if(execute)context.arguments.assign(args.begin()+execution.arguments,args.end());
        auto path = std::filesystem::path(std::u8string(args[2].begin(), args[2].end()));
        if(path.extension()==".huab") {
            if(build || args[1]=="ast" || args[1]=="interpret") { return usage_error(); }
            image=std::make_unique<hua::BytecodeImage>(hua::read_huab(path));
            if(args[1]=="check")output<<"OK\n";
            else if(args[1]=="bytecode")output<<hua::disassemble(image->bytecode);
            else return hua::report_entry(hua::VirtualMachine(image->model,output,&context).run(image->bytecode),std::cerr);
            return 0;
        }
        if(build&&args.size()==5&&std::filesystem::path(std::u8string(args[4].begin(),args[4].end())).extension()!=".huab")return usage_error();
        if (args[1] != "ast") {
            const auto& tree = loader.load(path);
            auto model = hua::SemanticAnalyzer{}.analyze(tree,loader.externals());
            if (args[1] == "check") output << "OK\n";
            else if (args[1] == "interpret") return hua::report_entry(hua::Interpreter(model, output,&context).run(tree),std::cerr);
            else {
                auto bytecode = hua::BytecodeCompiler{}.compile(tree, model);
                if(build) {
                    auto out=path;out.replace_extension(".huab");
                    if(args.size()==5)out=std::filesystem::path(std::u8string(args[4].begin(),args[4].end()));
                    if(out.extension()!=".huab") { return usage_error(); }
                    hua::write_huab(out,bytecode,model,loader.sources());auto out_utf8=out.u8string();output<<"Built "<<std::string(out_utf8.begin(),out_utf8.end())<<'\n';
                } else if (args[1] == "bytecode") output << hua::disassemble(bytecode);
                else return hua::report_entry(hua::VirtualMachine(model, output,&context).run(bytecode),std::cerr);
            }
            return 0;
        }
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) { std::cerr << "error[E0001]: cannot read source file: " << args[2] << '\n'; return 1; }
        const auto size = input.tellg();
        if (size < 0 || size > 16 * 1024 * 1024) {
            std::cerr << "error[E0001]: source file is unreadable or exceeds 16 MiB\n"; return 1;
        }
        std::string text(static_cast<std::size_t>(size), '\0');
        input.seekg(0);
        if (!text.empty() && !input.read(text.data(), static_cast<std::streamsize>(text.size()))) {
            std::cerr << "error[E0001]: failed to read complete source file\n"; return 1;
        }
        source = std::make_unique<hua::Source>(args[2], std::move(text));
        hua::Lexer lexer(*source);
        hua::Parser parser(lexer.scan());
        auto ast = parser.parse();
        output << hua::print_ast(*ast);
        return 0;
    } catch (const hua::Diagnostic& error) {
        const auto* origin = loader.source(error.span().file_id);
        if (!origin && image) origin=image->source(error.span().file_id);
        if (!origin && source && source->filename()==error.span().file_id) origin = source.get();
        if (origin) std::cerr << error.render(*origin);
        else {
            std::cerr << "error[" << error.code() << "]: " << error.what() << '\n';
            if(!error.span().file_id.empty())std::cerr << " --> " << error.span().file_id << ':' << error.span().line << ':' << error.span().column << '\n';
            std::cerr<<hua::render_source_trace(error.trace());
        }
        return hua::diagnostic_exit_code(error.code());
    }
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    return hua::command_boundary([&] {
    SetConsoleCP(CP_UTF8);SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (size <= 0) return 2;
        std::string value(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, value.data(), size, nullptr, nullptr);
        value.pop_back(); args.push_back(std::move(value));
    }
    return cli(args);
    },std::cerr);
}
#else
int main(int argc, char** argv) { return hua::command_boundary([&]{return cli(std::vector<std::string>(argv, argv + argc));},std::cerr); }
#endif
