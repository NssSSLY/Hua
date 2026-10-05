#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include "hua/interpreter.hpp"
#include "hua/vm.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
namespace {
std::string read(const std::filesystem::path& p){std::ifstream in(p,std::ios::binary);if(!in)throw std::runtime_error("missing fixture: "+p.string());return {std::istreambuf_iterator<char>(in),{}};}
int failures=0;
void check(bool ok,const std::string& label){if(!ok){++failures;std::cerr<<"FAIL: "<<label<<'\n';}}
}
int main(){
    try {
        std::vector<std::filesystem::path> fixtures;
        for(const auto& f:std::filesystem::directory_iterator(HUA_RUNTIME_TEST_ROOT))if(f.path().extension()==".hua")fixtures.push_back(f.path());
        std::sort(fixtures.begin(),fixtures.end());
        for(const auto& p:fixtures)for(bool vm:{false,true}){
            auto ep=p;ep.replace_extension(".expect");auto expectation=read(ep);std::istringstream expected(expectation);std::string header;std::getline(expected,header);
            auto output_path=p;output_path.replace_extension(".stdout");auto wanted=std::filesystem::exists(output_path)?read(output_path):"";
            std::ostringstream output;bool checked=false;
            hua::Source source(p.string(),read(p));
            try {
                auto tree=hua::Parser(hua::Lexer(source).scan()).parse();
                auto model=hua::SemanticAnalyzer{}.analyze(*tree);checked=true;
                check(!header.starts_with("CHECK_ERROR"),p.filename().string()+": expected semantic error");
                if(vm) {auto code=hua::BytecodeCompiler{}.compile(*tree,model);hua::VirtualMachine(model,output).run(code);}
                else hua::Interpreter(model,output).run(*tree);
                check(header=="OK",p.filename().string()+": expected runtime failure, got success");
            }catch(const hua::Diagnostic& error){
                check(header==(checked?"RUN_ERROR ":"CHECK_ERROR ")+error.code(),p.filename().string()+": "+error.code()+" "+error.what());
                std::string fragment;while(std::getline(expected,fragment))if(!fragment.empty())check(std::string(error.what()).find(fragment)!=std::string::npos,p.filename().string()+": expected reason "+fragment);
                check(error.span().file_id==source.filename() && error.render(source).find('^')!=std::string::npos,p.filename().string()+": located diagnostic");
            }
            check(output.str()==wanted,p.filename().string()+": expected stdout ["+wanted+"], got ["+output.str()+"]");
        }
        std::cout<<fixtures.size()<<" semantic/runtime fixtures x 2 engines, "<<failures<<" failures\n";
    }catch(const std::exception& e){check(false,e.what());}
    return failures?1:0;
}
