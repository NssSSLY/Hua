#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include "hua/vm.hpp"
#include <sstream>
#include <iostream>
#include <stdexcept>
struct Observer final:hua::VMCallObserver {
 std::vector<const hua::Node*> stack;unsigned entries{},exits{},arguments{},returns{};
 void enter(const hua::Node* n,const hua::Node* caller,const std::vector<hua::Value>& args)override{
  if(!n||(!stack.empty()&&caller!=stack.back()))throw std::runtime_error("observer caller mismatch");
  stack.push_back(n);++entries;arguments+=args.size();
 }
 void leave(const hua::Node* n,const hua::Value* result)override{
  if(!n)return;if(stack.empty()||stack.back()!=n||!result)throw std::runtime_error("observer return mismatch");
  stack.pop_back();++exits;++returns;
 }
};
int main(){try{
 unsigned checks=0;
 for(auto text:{
  "fn end(){print(9)}\nfn f(n int) int{if n==0{return 1}\nreturn n*f(n-1)}\nfn main(){defer end()\nprint(f(5))}\n",
  "struct Box{x int}\nfn touch(b mut Box){b.x+=1}\nfn read(b Box) int{return b.x}\nfn main(){var b=Box{x:3}\ntouch(b)\nprint(read(b))}\n",
  "fn f() Result<int>{return ok(7)}\nfn main() Result<void>{print(unwrap(f()))\nreturn err(\"bad\")}\n"}){
  hua::Source source("observer.hua",text);auto tree=hua::Parser(hua::Lexer(source).scan()).parse();auto model=hua::SemanticAnalyzer{}.analyze(*tree);auto code=hua::BytecodeCompiler{}.compile(*tree,model);
  std::ostringstream plain,watched;hua::RuntimeContext a,b;a.execution_limits=b.execution_limits={1000000,128,256};a.execution_metrics=std::make_shared<hua::ExecutionMetrics>();b.execution_metrics=std::make_shared<hua::ExecutionMetrics>();Observer observer;
  auto x=hua::VirtualMachine(model,plain,&a).run(code),y=hua::VirtualMachine(model,watched,&b,&observer).run(code);
  auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("observer changed VM behavior or counters");};
  check(x.exit_code==y.exit_code);check(plain.str()==watched.str());check(a.execution_metrics->steps==b.execution_metrics->steps);check(a.execution_metrics->peak_calls==b.execution_metrics->peak_calls);check(observer.stack.empty());check(observer.entries==observer.exits&&observer.returns==observer.exits);check(observer.entries>0);
 }
 std::cout<<checks<<" shared VM observer checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
