#include "hua/enum_value.hpp"
#include "hua/archive.hpp"
#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include "hua/module.hpp"
#include <fstream>
#include <chrono>
#include "hua/interpreter.hpp"
#include "hua/vm.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace hua;
using I=std::int64_t;
namespace {
unsigned checks=0;
void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
template<class F>void rejects(F f,const std::string& code="E4003"){
    try{f();throw std::runtime_error("expected rejection");}
    catch(const Diagnostic& error){check(error.code()==code,"wrong rejection code");}
}
void field(Node& n,std::string name,std::string type=""){
    auto f=std::make_unique<Node>(NodeKind::Field,SourceSpan{},std::move(name));
    f->add(std::make_unique<Node>(NodeKind::TypeName,SourceSpan{},std::move(type)));
    n.add(std::move(f));
}
NodePtr definition(std::vector<std::string> names,bool tag=true,std::string name="E enum"){
    auto n=std::make_unique<Node>(NodeKind::Struct,SourceSpan{},std::move(name));
    if(tag)field(*n,"$case","string");
    for(auto& key:names)field(*n,std::move(key));
    return n;
}
std::shared_ptr<StructData> object(const std::shared_ptr<const EnumLayout>& layout){
    auto result=managed<StructData>();result->name=layout->name;result->enumeration=layout;return result;
}
Value wrap(const std::shared_ptr<StructData>& data){return Value(StructValue{data,true});}
void layouts(){
    auto ordinary=definition({"$A#0#int"},true,"E");
    check(!enum_layout(*ordinary),"ordinary struct was inferred as enum");
    for(auto names:std::vector<std::vector<std::string>>{
        {},{"$A#1#int"},{"$A#x#int"},{"$A#00#int"},{"$A#4096#int"},
        {"$A#0#void","$A#1#int"},{"$A#0#int","$A#0#string"},
        {"$A#0#int","$A#0#int"},{"$A#0#"},{"$#0#int"},{"public"}}){
        auto n=definition(names);rejects([&]{enum_layout(*n);});
        SemanticModel model;model.structures.emplace("E",n.get());Bytecode code;
        rejects([&]{verify_bytecode(code,model);},"E6002");
    }
    auto missing=definition({"$A#0#int"},false);rejects([&]{enum_layout(*missing);});
    auto wrong=definition({"$A#0#int"});wrong->children[0]->children[0]->text="int";
    rejects([&]{enum_layout(*wrong);});
    auto inconsistent=definition({"$A#0#int"});inconsistent->children[1]->children[0]->text="string";
    rejects([&]{enum_layout(*inconsistent);});
}
void construction(){
    auto n=definition({"$A#0#int","$B#0#int?","$Empty#0#void"});
    auto layout=enum_layout(*n);n.reset();
    check(layout->variants.size()==3,"layout lifetime or variant count");
    for(bool tag_first:{true,false}){
        auto data=object(layout);
        if(tag_first)initialize_field(*data,"$case",Value(std::string("A")),{});
        initialize_field(*data,"$A#0#int",Value(I(17)),{});
        initialize_field(*data,"$B#0#int?",Value{},{});
        initialize_field(*data,"$Empty#0#void",Value{},{});
        if(!tag_first)initialize_field(*data,"$case",Value(std::string("A")),{});
        require_complete_enum(*data);
        check(data->fields.size()==2,"inactive fields retained");
        check(show(wrap(data))=="E.A(17)","tag order changed display");
        auto copied=copy_value(read_only(wrap(data)));
        check(std::get<StructValue>(copied.data).data->enumeration==layout,"copy lost identity");
        check(show(copied)=="E.A(17)","copy changed display");
        rejects([&]{initialize_field(*data,"$A#0#int",Value(I(2)),{});});
        rejects([&]{initialize_field(*data,"$case",Value(std::string("A")),{});});
        rejects([&]{initialize_field(*data,"$B#0#int?",Value(I(2)),{});});
        rejects([&]{initialize_field(*data,"unknown",Value{},{});});
    }
    auto optional=object(layout);initialize_field(*optional,"$case",Value(std::string("B")),{});
    rejects([&]{copy_value(wrap(optional));});
    initialize_field(*optional,"$B#0#int?",Value{},{});
    require_complete_enum(*optional);
    check(optional->fields.contains("$B#0#int?"),"active optional nil was pruned");
    check(show(wrap(optional))=="E.B(nil)","optional display");
    auto empty=object(layout);initialize_field(*empty,"$case",Value(std::string("Empty")),{});
    initialize_field(*empty,"$Empty#0#void",Value{},{});
    check(show(copy_value(wrap(empty)))=="E.Empty()","empty variant changed");
    for(const Value& tag:{Value{},Value(I(1)),Value(std::string("Missing"))}){
        auto data=object(layout);rejects([&]{initialize_field(*data,"$case",tag,{});});
    }
    auto bad=object(layout);initialize_field(*bad,"$case",Value(std::string("A")),{});
    rejects([&]{initialize_field(*bad,"$A#0#int",Value(std::string("wrong")),{});});
    auto before_tag=object(layout);initialize_field(*before_tag,"$B#0#int?",Value(I(2)),{});
    rejects([&]{initialize_field(*before_tag,"$case",Value(std::string("A")),{});});
    auto missing_tag=object(layout);initialize_field(*missing_tag,"$A#0#int",Value(I(1)),{});
    rejects([&]{copy_value(wrap(missing_tag));});
    auto plain=managed<StructData>();plain->name="Plain";
    initialize_field(*plain,"$case",Value(std::string("A")),{});
    initialize_field(*plain,"$A#0#int",Value(I(1)),{});
    initialize_field(*plain,"$B#0#int",Value{},{});
    check(plain->fields.size()==3,"unmarked ordinary struct was compacted");
}
void scale_and_views(){
    std::vector<std::string> keys;
    for(unsigned i=0;i<59;++i)keys.push_back("$V"+std::to_string(i)+"#0#int");
    auto n=definition(keys);auto layout=enum_layout(*n);
    for(unsigned active:{0u,58u}){
        auto data=object(layout);initialize_field(*data,"$case",Value("V"+std::to_string(active)),{});
        for(unsigned i=0;i<59;++i)initialize_field(*data,keys[i],i==active?Value(I(17)):Value{},{});
        auto copied=copy_value(read_only(wrap(data)));
        auto copy=std::get<StructValue>(copied.data).data;
        check(copy->fields.size()==2,"large enum still expands while copying");
        copy->fields.at(keys[active])=Value(I(99));
        check(show(wrap(data))=="E.V"+std::to_string(active)+"(17)","scalar copy aliases");
        check(copy->enumeration==layout&&copy->variant==data->variant,"copy layout ownership");
    }
    auto boxed=definition({"$Box#0#Box","$List#0#List<int>","$File#0#std.file.File"});
    auto types=enum_layout(*boxed);
    auto box=managed<StructData>();box->name="Box";box->fields.emplace("value",Value(I(7)));
    auto data=object(types);initialize_field(*data,"$case",Value(std::string("Box")),{});
    initialize_field(*data,"$Box#0#Box",read_only(wrap(box)),{});
    auto copied=copy_value(read_only(wrap(data)));
    auto child=std::get<StructValue>(std::get<StructValue>(copied.data).data->fields.at("$Box#0#Box").data);
    child.data->fields.at("value")=Value(I(9));
    check(show(wrap(data))=="E.Box(Box{value: 7})","nested record copy aliases");
    auto list=managed<ListData>();list->element_type="int";list->values.emplace_back(I(7));
    auto shared=object(types);initialize_field(*shared,"$case",Value(std::string("List")),{});
    initialize_field(*shared,"$List#0#List<int>",Value(ListValue{list,true}),{});
    auto snapshot=copy_value(read_only(wrap(shared)));
    auto item=std::get<ListValue>(std::get<StructValue>(snapshot.data).data->fields.at("$List#0#List<int>").data);
    check(item.data==list&&!item.writable,"readonly shared child capability");
    auto cloned=copy_value(snapshot,true);
    auto owned=std::get<ListValue>(std::get<StructValue>(cloned.data).data->fields.at("$List#0#List<int>").data);
    check(owned.data!=list&&owned.writable,"deep clone lost owned child");
    owned.data->values[0]=Value(I(99));check(as_int(list->values[0],{})==7,"deep clone aliases");
    auto resource=object(types);initialize_field(*resource,"$case",Value(std::string("File")),{});
    initialize_field(*resource,"$File#0#std.file.File",Value(FileValue{}),{});
    check(contains_file(wrap(resource)),"active File hidden");
    rejects([&]{copy_value(wrap(resource),true);},"E4007");
    rejects([&]{initialize_field(*shared,"$File#0#std.file.File",Value(FileValue{}),{});});
}
void cycles(){
    auto n=definition({"$Link#0#List<E>"});auto layout=enum_layout(*n);
    ManagedHeap heap;HeapScope scope(heap);std::weak_ptr<StructData> weak;
    {
        auto list=managed<ListData>();list->element_type="E";
        auto data=object(layout);initialize_field(*data,"$case",Value(std::string("Link")),{});
        initialize_field(*data,"$Link#0#List<E>",Value(ListValue{list,true}),{});
        list->values.push_back(wrap(data));weak=data;
        check(!contains_file(wrap(data)),"resource-free enum cycle");
        rejects([&]{copy_value(wrap(data),true);},"E4099");
        check(!weak.expired(),"failed clone removed live root");
    }
    heap.collect();check(weak.expired()&&heap.live()==0,"enum cycle or failed clone leaked");
}
void reused_engines(){
    SemanticModel model;std::ostringstream output;VirtualMachine vm(model,output);
    Interpreter interpreter(model,output);
    auto path=std::filesystem::temp_directory_path()/("hua-enum-reuse-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".hua");
    struct Remove {std::filesystem::path path;~Remove(){std::error_code ec;std::filesystem::remove(path,ec);}} remove{path};
    for(unsigned i=0;i<16;++i){
        bool text=i%2;auto type=text?"string":"int";auto literal=text?"\"text\"":"7";
        {std::ofstream file(path,std::ios::binary);
         file<<"enum E{A("+std::string(type)+"),B}\nfn pass(x E) E{return x}\nfn main(){print(pass(E.A("+literal+")))}\n";
         if(!file)throw std::runtime_error("cannot write enum reuse fixture");}
        ModuleLoader loader;const auto& tree=loader.load(path);
        model=SemanticAnalyzer{}.analyze(tree,loader.externals());auto bytecode=BytecodeCompiler{}.compile(tree,model);
        for(bool ast:{true,false}){
            output.str("");output.clear();
            auto result=ast?interpreter.run(tree):vm.run(bytecode);
            check(result.exit_code==0,"reused engine entry");
            check(output.str()==(text?"E.A(text)\n":"E.A(7)\n"),"stale enum layout after AST destruction");
        }
    }
}
}
int main(){try{
    layouts();construction();scale_and_views();cycles();reused_engines();
    std::cout<<checks<<" enum layout checks passed\n";return 0;
}catch(const std::exception& error){std::cerr<<"after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
