#include "hua/lexer.hpp"
#include "hua/value.hpp"
#include <iostream>
#include <stdexcept>
using namespace hua;
namespace {unsigned checks{};void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}}
int main(){try{
    for(const auto& raw:{std::string("a\nb\n"),std::string("a\rb\r"),std::string("a\r\nb\r\n"),std::string("\xef\xbb\xbf" "a\r\nb\r\n")}){
        Source source("normalized.hua",raw);check(source.text()=="a\nb\n","normalization");
        for(std::size_t offset=0;offset<=source.text().size()+2;++offset){auto span=source.span(offset,offset+2);auto start=std::min(offset,source.text().size());
            std::size_t line=1,column=1;for(std::size_t i=0;i<start;++i){if(source.text()[i]=='\n'){++line;column=1;}else ++column;}
            check(span.start_offset==start&&span.end_offset>=span.start_offset&&span.end_offset<=source.text().size()&&span.line==line&&span.column==column,"independent byte location");
        }
        check(source.span(3,1).end_offset==3,"inverted range clamps to empty");
    }
    Source unicode("unicode.hua","\tprint(\"汉é🙂\",missing)\n");auto offset=unicode.text().find("missing");auto span=unicode.span(offset,offset+7);
    check(span.column==offset+1&&span.line==1,"UTF8 byte column");
    Diagnostic original("E3001",span,"unknown name","use a declaration");auto annotated=original.located(unicode.span(0,1));
    check(annotated.span()==span&&annotated.code()==original.code(),"primary origin preserved");
    annotated.add_context(unicode.span(0,1),"called from");check(annotated.render(unicode).find("help: use a declaration")!=std::string::npos,"help preserved");
    check(annotated.render(Source("different.hua","wrong source")).find("wrong source")==std::string::npos,"wrong source never displayed");
    auto located=Diagnostic("E4003",{},"copy failed","help survives").located(span);check(located.span()==span&&located.render(unicode).find("help survives")!=std::string::npos,"unlocated diagnostic repair");
    for(unsigned i=0;i<80;++i)annotated.add_context({"context.hua",i,i+1,i+1,1},"called from");
    check(annotated.trace().sites.size()==32&&annotated.trace().truncated,"bounded contexts");check(annotated.render(unicode).find("limit 32")!=std::string::npos,"visible truncation");
    for(const auto& raw:{std::string("\x80",1),std::string("\xc0\xaf",2),std::string("\xe2\x82",2),std::string("\xed\xa0\x80",3),std::string("\xf4\x90\x80\x80",4),std::string("\xff",1)}){
        Source source("invalid.hua",raw);try{Lexer(source).scan();throw std::runtime_error("invalid UTF8 accepted");}
        catch(const Diagnostic& d){check(d.code()=="E1009"&&d.span().start_offset==0,"UTF8 classification and byte offset");check(d.render(source).find("\\x")!=std::string::npos,"invalid bytes escaped");}
    }
    Source multiline("tokens.hua","#* outer\n#* inner *#\n*#\nlet a=\"汉\"\n");auto tokens=Lexer(multiline).scan();
    for(const auto& token:tokens){auto expected=multiline.span(token.span.start_offset,token.span.end_offset);check(token.span==expected,"token source identity");}
    SourceTrace trace;for(unsigned i=0;i<80;++i)trace.add({"failure.hua",i,i+1,i+1,1},"Result propagated here");
    auto value=Value(ResultValue{false,managed<Value>(Value(std::string("bad"))),"int","string",true,false});
    value=record_failure_source(std::move(value),span);auto copied=copy_value(value,true);auto propagated=propagated_error(copied,unicode.span(0,1));
    auto& result=std::get<ResultValue>(propagated.data);check(result.failure_source&&result.failure_source->sites.size()==2,"failure source survives clone/propagation");
    check(std::get<ResultValue>(value.data).failure_source->sites.size()==1,"failure source copy isolation");
    try{result_payload(propagated,true,span);throw std::runtime_error("unwrap error accepted");}catch(const Diagnostic& d){check(d.code()=="E4009"&&!d.trace().sites.empty(),"unwrap includes failure source");}
    check(trace.sites.size()==32&&trace.truncated,"Result trace budget");
    std::cout<<"A10 source/diagnostic: "<<checks<<" checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
