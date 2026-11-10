#include "hua/native_rt.hpp"
#include <sstream>
namespace hn {
[[noreturn]] void step_limit_exceeded(){hua::runtime_error({},"execution step limit exceeded ("+std::to_string(state.step_limit)+")","E4099");}
[[noreturn]] void depth_limit_exceeded(const SourceSpan& site){hua::runtime_error(site,"function call depth limit exceeded ("+std::to_string(state.call_limit)+")","E4099");}
[[noreturn]] void bad_unbox(const char* expected){hua::runtime_error({},std::string("native bridge: library result is not ")+expected,"E8002");}
[[noreturn]] void fail(const SourceSpan& span,const char* message,const char* code){hua::runtime_error(span,message,code);}
Value call_builtin(const char* name,const SourceSpan& span,std::vector<Value> args,const std::vector<bool>& contextual,const char* result_type){
    return hua::invoke_builtin(name,args,span,std::cout,contextual,result_type,state.context);
}
Value call_bridge(Bridge& site,const SourceSpan& span,std::initializer_list<Value> args){
    if(site.busy){std::vector<Value> own(args);return hua::invoke_builtin(*site.info,site.name,own,span,std::cout,site.contextual,site.result_type,state.context);} // re-entered
    struct Lease{Bridge& s;~Lease(){s.args.clear();s.busy=false;}} lease{site};site.busy=true;
    site.args.assign(args.begin(),args.end());
    return hua::invoke_builtin(*site.info,site.name,site.args,span,std::cout,site.contextual,site.result_type,state.context);
}
void print(const SourceSpan& span,std::vector<Value> args){hua::invoke_builtin("print",args,span,std::cout,{},"void",state.context);}
int run_main(int argc,char** argv,const SourceSpan& main_site,Value (*body)()){
    hua::RuntimeContext base;base.input=&std::cin;base.working_directory=std::filesystem::current_path();
    base.execution_limits={state.step_limit,state.call_limit,65536};
    for(int i=1;i<argc;++i){if(i==1&&std::string(argv[i])=="--")continue;base.arguments.emplace_back(argv[i]);}
    try{
        Value returned;std::exception_ptr error;
        {
            hua::RuntimeContext* context=&base;hua::ExecutionScope execution(context,std::cout);
            static const hua::TypeRelations no_relations;context->type_relations=&no_relations;state.context=context;
            state.sites[0]=&main_site;state.depth=1;
            try{returned=body();}catch(...){error=std::current_exception();}
            try{execution.finish();}catch(...){if(!error)error=std::current_exception();}
            state.context=nullptr;
            if(error)returned=Value{};
        }
        if(error)std::rethrow_exception(error);
        return hua::report_entry(hua::entry_result(returned,main_site),std::cerr);
    }catch(const hua::ProgramExit& exit){return exit.code;}
    catch(const hua::Diagnostic& failure){
        // The VM adds one "called from" per active frame, innermost first.
        auto reported=failure;for(unsigned i=state.depth;i-->0;)reported.add_context(*state.sites[i],"called from");
        const auto& file=reported.span().file_id;std::ifstream input(std::filesystem::path(std::u8string(file.begin(),file.end())),std::ios::binary);
        if(!file.empty()&&input){
            std::ostringstream text;text<<input.rdbuf();hua::Source source(file,text.str());std::cerr<<reported.render(source);
        }else{
            std::cerr<<"error["<<reported.code()<<"]: "<<reported.what()<<'\n';
            if(!file.empty())std::cerr<<" --> "<<file<<':'<<reported.span().line<<':'<<reported.span().column<<'\n';
            std::cerr<<hua::render_source_trace(reported.trace());
        }
        return hua::diagnostic_exit_code(reported.code());
    }catch(const std::exception& failure){std::cerr<<"error[E0002]: internal command failure: "<<failure.what()<<'\n';return 3;}
}
}
