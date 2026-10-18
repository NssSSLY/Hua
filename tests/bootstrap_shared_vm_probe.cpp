#include "bootstrap_phase_profile.hpp"
#include <iostream>
struct Observer final:hua::VMCallObserver {
 hua::bootstrap_phase::Session& session;explicit Observer(hua::bootstrap_phase::Session& s):session(s){}
 void enter(const hua::Node* n,const hua::Node* c,const std::vector<hua::Value>& a)override{session.enter(n,c,a);}
 void leave(const hua::Node* n,const hua::Value*)override{session.leave(n);}
};
int run(const std::filesystem::path& trace,const std::filesystem::path& compiler,const std::string& input,const std::string& output,bool enabled){
 if(std::filesystem::exists(trace)||std::filesystem::exists(std::filesystem::u8path(output)))throw std::runtime_error("evidence already exists");
 auto image=hua::read_huab(compiler);hua::RuntimeContext context;context.input=&std::cin;context.working_directory=std::filesystem::current_path();context.arguments={input,output};context.execution_limits={500000000,128,65536};context.execution_metrics=std::make_shared<hua::ExecutionMetrics>();
 std::ofstream file(trace);if(!file)throw std::runtime_error("cannot open phase trace");hua::bootstrap_phase::Session session(image.model,*context.execution_metrics,file);
 Observer observer(session);auto result=hua::report_entry(hua::VirtualMachine(image.model,std::cout,&context,enabled?&observer:nullptr).run(image.bytecode),std::cerr);if(result)throw std::runtime_error("compiler failed");session.finish();session.report(std::cerr);return 0;
}
#ifdef _WIN32
std::string utf8(const wchar_t* p){int n=WideCharToMultiByte(CP_UTF8,0,p,-1,nullptr,0,nullptr,nullptr);std::string s(n,'\0');WideCharToMultiByte(CP_UTF8,0,p,-1,s.data(),n,nullptr,nullptr);s.pop_back();return s;}
int wmain(int argc,wchar_t** argv){try{return (argc==5||argc==6)?run(argv[1],argv[2],utf8(argv[3]),utf8(argv[4]),argc==5):2;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#else
int main(int argc,char** argv){try{return (argc==5||argc==6)?run(argv[1],argv[2],argv[3],argv[4],argc==5):2;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
#endif
