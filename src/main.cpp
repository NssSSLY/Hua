#include "hua/ast.hpp"
#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include "hua/sema.hpp"
#include "hua/interpreter.hpp"
#include "hua/module.hpp"
#include "hua/vm.hpp"
#include "hua/archive.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace {
void usage(std::ostream& out) {
    out << "Hua language\n"
        << "Usage: hua version\n"
        << "       hua check <file.hua|file.huab>\n"
        << "       hua ast <file.hua>\n"
        << "       hua run <file.hua|file.huab> [-- args...]\n"
        << "       hua interpret <file.hua> [-- args...]\n"
        << "       hua bytecode <file.hua|file.huab>\n"
        << "       hua build <file.hua> [-o file.huab]\n";
}
int cli(const std::vector<std::string>& args) {
    if (args.size() == 2 && (args[1] == "version" || args[1] == "--version")) {
        std::cout << "Hua 0.1.0-dev\nspec 0.1\nabi 1\nbytecode 6\n"; return 0;
    }
    if (args.size() == 2 && (args[1] == "--help" || args[1] == "help")) { usage(std::cout); return 0; }
    bool build = args.size() >= 3 && args[1] == "build";
    bool execute=args.size()>=3&&(args[1]=="run"||args[1]=="interpret");
    bool valid_size=args.size()==3||(execute&&args.size()>=4&&args[3]=="--");
    if (build ? !(args.size() == 3 || (args.size() == 5 && args[3] == "-o")) :
        (!valid_size || (args[1] != "check" && args[1] != "ast" && args[1] != "run" && args[1] != "interpret" && args[1] != "bytecode"))) { usage(std::cerr); return 2; }
    std::unique_ptr<hua::Source> source;
    hua::ModuleLoader loader;
    std::unique_ptr<hua::BytecodeImage> image;
    try {
        hua::RuntimeContext context;context.input=&std::cin;context.working_directory=std::filesystem::current_path();if(execute&&args.size()>=4)context.arguments.assign(args.begin()+4,args.end());
        auto path = std::filesystem::path(std::u8string(args[2].begin(), args[2].end()));
        if(path.extension()==".huab") {
            if(build || args[1]=="ast" || args[1]=="interpret") { usage(std::cerr); return 2; }
            image=std::make_unique<hua::BytecodeImage>(hua::read_huab(path));
            if(args[1]=="check")std::cout<<"OK\n";
            else if(args[1]=="bytecode")std::cout<<hua::disassemble(image->bytecode);
            else hua::VirtualMachine(image->model,std::cout,&context).run(image->bytecode);
            return 0;
        }
        if (args[1] != "ast") {
            const auto& tree = loader.load(path);
            auto model = hua::SemanticAnalyzer{}.analyze(tree,loader.externals());
            if (args[1] == "check") std::cout << "OK\n";
            else if (args[1] == "interpret") hua::Interpreter(model, std::cout,&context).run(tree);
            else {
                auto bytecode = hua::BytecodeCompiler{}.compile(tree, model);
                if(build) {
                    auto out=path;out.replace_extension(".huab");
                    if(args.size()==5)out=std::filesystem::path(std::u8string(args[4].begin(),args[4].end()));
                    if(out.extension()!=".huab") { usage(std::cerr); return 2; }
                    hua::write_huab(out,bytecode,model,loader.sources());auto out_utf8=out.u8string();std::cout<<"Built "<<std::string(out_utf8.begin(),out_utf8.end())<<'\n';
                } else if (args[1] == "bytecode") std::cout << hua::disassemble(bytecode);
                else hua::VirtualMachine(model, std::cout,&context).run(bytecode);
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
        std::cout << hua::print_ast(*ast);
        return 0;
    } catch (const hua::Diagnostic& error) {
        const auto* origin = loader.source(error.span().file_id);
        if (!origin && image) origin=image->source(error.span().file_id);
        if (!origin) origin = source.get();
        if (origin) std::cerr << error.render(*origin);
        else {
            std::cerr << "error[" << error.code() << "]: " << error.what() << '\n';
            if(!error.span().file_id.empty())std::cerr << " --> " << error.span().file_id << ':' << error.span().line << ':' << error.span().column << '\n';
        }
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "error[E0001]: " << error.what() << '\n'; return 1;
    }
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
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
}
#else
int main(int argc, char** argv) { return cli(std::vector<std::string>(argv, argv + argc)); }
#endif
