#include "hua/entry.hpp"
#include "hua/stdlib.hpp"
#include <filesystem>
#include <ostream>
namespace hua {
EntryResult entry_result(const Value& value,const SourceSpan& span) {
    auto result=std::get_if<ResultValue>(&value.data);
    if(!result || result->ok)return {};
    if(!result->payload)throw std::logic_error("entry Result error has no payload");
    EntryResult out{1,"main returned Err: ",span};
    if(result->failure_source)out.failure_source=*result->failure_source;
    if(auto text=hua::string_data(&result->payload->data))out.message+=*text;
    else if(auto error=std::get_if<ErrorValue>(&result->payload->data)) {
        if(!error->data)throw std::logic_error("entry Error has no data");
        const auto& data=*error->data;
        if(data.domain=="hua.cli") {
            if(data.code=="usage")out.exit_code=2;
            else if(data.code=="internal")out.exit_code=3;
        }
        out.message+=format_error(*error);
    } else {
        // Domain values have no implicit Error conversion or full-payload logging.
        out.message+="<"+display_type(result->error_type)+"> (format this domain error explicitly)";
    }
    return out;
}
int report_entry(const EntryResult& result,std::ostream& error) {
    if(result.exit_code) {
        error<<"error[E0003]: "<<result.message<<'\n';
        if(!result.span.file_id.empty())error<<" --> "<<result.span.file_id<<':'<<result.span.line<<':'<<result.span.column<<'\n';
        error<<render_source_trace(result.failure_source);
    }
    return result.exit_code;
}
int diagnostic_exit_code(std::string_view code) {
    return code=="E6001" || code=="E0002" ? 3 : 1;
}
int command_boundary(const std::function<int()>& command,std::ostream& error) {
    try{return command();}
    catch(const ProgramExit& exit){return exit.code;}
    catch(const Diagnostic& failure) {
        error<<"error["<<failure.code()<<"]: "<<failure.what()<<'\n';
        if(!failure.span().file_id.empty())error<<" --> "<<failure.span().file_id<<':'<<failure.span().line<<':'<<failure.span().column<<'\n';
        error<<render_source_trace(failure.trace());
        return diagnostic_exit_code(failure.code());
    } catch(const std::filesystem::filesystem_error& failure) {
        error<<"error[E0001]: "<<failure.what()<<'\n';return 1;
    } catch(const std::exception& failure) {
        error<<"error[E0002]: internal command failure: "<<failure.what()<<'\n';return 3;
    } catch(...) {
        error<<"error[E0002]: internal command failure: unknown host exception\n";return 3;
    }
}
}
