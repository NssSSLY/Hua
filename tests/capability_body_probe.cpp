// Complete, bounded compiler runs through the existing borrowed VM call observer.
#include "bootstrap_capability_profile.hpp"
#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include <iostream>
#include <functional>
struct Observer final:hua::VMCallObserver {
 hua::bootstrap_phase::Session& session;
 explicit Observer(hua::bootstrap_phase::Session& s):session(s){}
 void enter(const hua::Node* fn,const hua::Node* caller,const std::vector<hua::Value>& args)override{session.enter(fn,caller,args);}
 void leave(const hua::Node* fn,const hua::Value* result)override{session.leave(fn,result);}
};
int census(const std::vector<std::filesystem::path>& files){
 std::cout<<"{\"functions\":[";bool first=true;
 for(const auto& path:files){std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("census source unavailable");
  std::string text{std::istreambuf_iterator<char>(in),{}};hua::Source source(path.string(),text);
  auto tree=hua::Parser(hua::Lexer(source).scan()).parse();
  std::function<void(const hua::Node&)> visit=[&](const hua::Node& n){
   if(n.kind==hua::NodeKind::Function){const hua::Node* body=nullptr;std::uint64_t parameters=0;
    for(const auto& c:n.children){if(c->kind==hua::NodeKind::Block)body=c.get();if(c->kind==hua::NodeKind::Parameter)++parameters;}
    if(body){std::uint64_t nodes=0,members=0,controls=0;
     std::function<void(const hua::Node&)> count=[&](const hua::Node& x){++nodes;if(x.kind==hua::NodeKind::Member)++members;
      if(x.kind==hua::NodeKind::If||x.kind==hua::NodeKind::For||x.kind==hua::NodeKind::While||x.kind==hua::NodeKind::Match)++controls;
      for(const auto& c:x.children)count(*c);};count(*body);
     if(!first)std::cout<<',';first=false;std::cout<<"{\"module\":"<<std::quoted(path.filename().string())<<",\"name\":"<<std::quoted(hua::declared_name(n))
       <<",\"body_nodes\":"<<nodes<<",\"parameters\":"<<parameters<<",\"member_nodes\":"<<members<<",\"control_nodes\":"<<controls<<'}';
    }
   }for(const auto& c:n.children)visit(*c);
  };visit(*tree);
 }std::cout<<"]}\n";return 0;
}
int run(const std::vector<std::filesystem::path>& paths){
 if(paths.size()<4||paths.size()>5)throw std::runtime_error("expected trace compiler input output [off]");
 const auto& trace=paths[0];const auto& output=paths[3];
 if(std::filesystem::exists(trace)||std::filesystem::exists(output))throw std::runtime_error("evidence already exists");
 auto image=hua::read_huab(paths[1]);hua::RuntimeContext context;context.input=&std::cin;context.working_directory=std::filesystem::current_path();
 auto utf8=[](const std::filesystem::path& p){const auto value=p.u8string();return std::string(value.begin(),value.end());};
 context.arguments={utf8(paths[2]),utf8(output)};context.execution_limits={500000000,128,65536};context.execution_metrics=std::make_shared<hua::ExecutionMetrics>();
 std::ofstream file(trace);if(!file)throw std::runtime_error("cannot open trace");hua::bootstrap_phase::Session session(image.model,*context.execution_metrics,file);Observer observer(session);
 if(paths.size()==5&&paths[4]!=std::filesystem::path("off"))throw std::runtime_error("invalid observer mode");
 auto result=hua::report_entry(hua::VirtualMachine(image.model,std::cout,&context,paths.size()==5?nullptr:&observer).run(image.bytecode),std::cerr);
 if(result)throw std::runtime_error("compiler failed");session.finish();session.report(std::cerr);return 0;
}
int execute(const std::vector<std::filesystem::path>& paths){
 if(!paths.empty()&&paths[0]==std::filesystem::path("--census"))return census({paths.begin()+1,paths.end()});
 return run(paths);
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){try{std::vector<std::filesystem::path> paths;for(int i=1;i<argc;++i)paths.emplace_back(argv[i]);return execute(paths);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#else
int main(int argc,char** argv){try{std::vector<std::filesystem::path> paths;for(int i=1;i<argc;++i)paths.emplace_back(argv[i]);return execute(paths);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#endif
