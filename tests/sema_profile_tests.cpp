#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include "hua/sema.hpp"
#include "hua/runtime.hpp"
#include <iostream>
#include <stdexcept>
using namespace hua;
int main(){try{
 unsigned checks=0;auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("semantic snapshot meter regression");};SemanticAnalyzer analyzer;
 auto inspect=[&](const std::string& text){sema_profile::Counters counters;Source source("meter.hua",text);auto tree=Parser(Lexer(source).scan()).parse();{sema_profile::Scope scope(counters);analyzer.analyze(*tree);}return counters;};
 auto eager=inspect("fn work() int{var a=1\nvar b=2\na+b\nreturn a}\nfn main(){}\n");
 check(eager.passes.size()==2&&!eager.passes.back().changed);check(eager.area_calls[std::size_t(sema_profile::Area::binary_eager)]==0);
 for(auto value:eager.copies[std::size_t(sema_profile::Area::binary_eager)])check(value==0);
 for(const std::string& op:{"&&","||"}){
  auto logic=inspect("fn work(flag bool){var x=1\nflag"+op+"flag\n}\nfn main(){}\n");
  auto row=logic.copies[std::size_t(sema_profile::Area::binary_logic)];check(logic.passes.size()==2);check(logic.area_calls[std::size_t(sema_profile::Area::binary_logic)]==3);
  check(row[4]+row[5]==3*(2*builtin_names().size()+2+1+1));check(row[0]+row[1]==0);
 }
 auto again=inspect("fn work() int{var a=1\nvar b=2\na+b\nreturn a}\nfn main(){}\n");check(again.copies==eager.copies);check(again.passes.size()==eager.passes.size());
 // Distinct collector scopes never inherit previous dependency versions or counters.
 check(sema_profile::active==nullptr&&sema_profile::function==nullptr);
 std::cout<<checks<<" semantic copy-meter, logic snapshot and analyzer reuse checks passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
