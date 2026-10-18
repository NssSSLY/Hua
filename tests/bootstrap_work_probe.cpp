// Pure structural features of real user sources, excluding native std source declarations.
#include "hua/archive.hpp"
#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#ifdef _WIN32
#include <windows.h>
#endif
using N=hua::NodeKind;
int inspect(const std::vector<std::filesystem::path>& paths){
 auto image=hua::read_huab(paths.at(0));std::map<std::string,std::uint64_t> f={{"constant",1}};std::vector<hua::NodePtr> trees;
 for(std::size_t i=1;i<paths.size();++i){std::ifstream in(paths[i],std::ios::binary);if(!in)throw std::runtime_error("source unavailable");std::string text{std::istreambuf_iterator<char>(in),{}};f["source_bytes"]+=text.size();hua::Source source(paths[i].string(),text);auto tokens=hua::Lexer(source).scan();f["tokens"]+=tokens.size();trees.push_back(hua::Parser(std::move(tokens)).parse());}
 std::uint64_t globals=0;for(const auto& t:trees)for(const auto& n:t->children)if(n->kind==N::Let||n->kind==N::Var||n->kind==N::Const)++globals;
 std::function<void(const hua::Node&,std::uint64_t,std::uint64_t,bool)> visit;
 visit=[&](const hua::Node& n,std::uint64_t depth,std::uint64_t live,bool function){
  ++f["ast_nodes"];f["depth_work"]+=depth+1;f["ast_depth"]=std::max(f["ast_depth"],depth);
  if(n.kind==N::Function){++f["source_functions"];live=globals;for(const auto& c:n.children)if(c->kind==N::Parameter)++live;function=true;}
  if(n.kind==N::If||n.kind==N::While||n.kind==N::For||n.kind==N::Match||n.kind==N::Defer||n.kind==N::Call||n.kind==N::Assignment||n.kind==N::MultiAssignment)f["state_exposure"]+=live+1;
  if(n.kind==N::StructLiteral)f["field_work"]+=n.children.size()+1;
  if(n.kind==N::Member){auto* p=&n;std::uint64_t length=0;while(p->kind==N::Member&&!p->children.empty()){++length;p=p->children[0].get();}f["field_work"]+=length;}
  if(n.kind==N::TypeName||n.kind==N::GenericType||n.kind==N::SliceType||n.kind==N::ArrayType||n.kind==N::OptionalType||n.kind==N::MutableType)f["type_work"]+=n.text.size()+1;
  for(const auto& c:n.children){visit(*c,depth+1,live,function);if(function&&n.kind==N::Block&&(c->kind==N::Let||c->kind==N::Var||c->kind==N::Const||c->kind==N::MultiBinding))++live;}
 };
 for(const auto& tree:trees)visit(*tree,0,globals,false);
 f["globals_work"]=globals*(f["source_functions"]+1);f["modules"]=trees.size();
 auto code=[&](const hua::Code& c){f["instructions"]+=c.instructions.size();f["max_code"]=std::max<std::uint64_t>(f["max_code"],c.instructions.size());for(const auto& i:c.instructions)if(i.op==hua::Op::Jump||i.op==hua::Op::JumpTrue||i.op==hua::Op::JumpFalse||i.op==hua::Op::IterNext)++f["branches"];};
 code(image.bytecode.initializer);for(const auto& [_,c]:image.bytecode.functions)code(c);f["functions"]=image.bytecode.functions.size();
 for(auto name:{"state_exposure","field_work","type_work","branches","globals_work"})f[name]+=0;
 std::cout<<'{';bool first=true;for(const auto& [name,value]:f){if(!first)std::cout<<',';first=false;std::cout<<'"'<<name<<"\":"<<value;}std::cout<<"}\n";return 0;
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){try{std::vector<std::filesystem::path> p;for(int i=1;i<argc;++i)p.emplace_back(argv[i]);return argc>=3?inspect(p):2;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#else
int main(int argc,char** argv){try{std::vector<std::filesystem::path> p;for(int i=1;i<argc;++i)p.emplace_back(argv[i]);return argc>=3?inspect(p):2;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#endif
