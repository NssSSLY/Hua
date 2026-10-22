// Native per-function body sizes for joining with observed capability body work.
// Pure parse of user sources; keys match capabilities Environment.function ("file#name").
#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include "hua/value.hpp"
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#ifdef _WIN32
#include <windows.h>
#endif
using N=hua::NodeKind;
struct Body {std::uint64_t nodes{},depth{},calls{},members{},branches{},bindings{};};
int census(const std::vector<std::filesystem::path>& paths){
 std::map<std::string,Body> rows;
 for(const auto& path:paths){
  std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("source unavailable");std::string text{std::istreambuf_iterator<char>(in),{}};
  hua::Source source(path.string(),text);auto tree=hua::Parser(hua::Lexer(source).scan()).parse();
  for(const auto& fn:tree->children){if(fn->kind!=N::Function||fn->children.empty())continue;const auto& body=*fn->children.back();if(body.kind!=N::Block)continue;
   auto key=path.filename().string()+"#"+hua::declared_name(*fn);if(rows.contains(key))throw std::runtime_error("duplicate function key "+key);Body b;
   std::function<void(const hua::Node&,std::uint64_t)> visit=[&](const hua::Node& n,std::uint64_t d){
    ++b.nodes;b.depth=std::max(b.depth,d);
    if(n.kind==N::Call)++b.calls;if(n.kind==N::Member)++b.members;
    if(n.kind==N::If||n.kind==N::While||n.kind==N::For||n.kind==N::Match)++b.branches;
    if(n.kind==N::Let||n.kind==N::Var||n.kind==N::Const||n.kind==N::MultiBinding)++b.bindings;
    for(const auto& c:n.children)visit(*c,d+1);};
   visit(body,0);rows.emplace(key,b);
  }
 }
 std::cout<<"{\"schema\":\"hua.bootstrap.body-census.v1\",\"functions\":{";bool first=true;
 for(const auto& [k,b]:rows){if(!first)std::cout<<',';first=false;std::cout<<std::quoted(k)<<":{\"nodes\":"<<b.nodes<<",\"depth\":"<<b.depth<<",\"calls\":"<<b.calls<<",\"members\":"<<b.members<<",\"branches\":"<<b.branches<<",\"bindings\":"<<b.bindings<<'}';}
 std::cout<<"}}\n";return 0;
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){try{std::vector<std::filesystem::path> p;for(int i=1;i<argc;++i)p.emplace_back(argv[i]);return argc>=2?census(p):2;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#else
int main(int argc,char** argv){try{std::vector<std::filesystem::path> p;for(int i=1;i<argc;++i)p.emplace_back(argv[i]);return argc>=2?census(p):2;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#endif
