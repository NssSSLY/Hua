#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include "hua/interpreter.hpp"
#include "hua/vm.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
namespace hua {
struct LifetimeCapture {std::vector<Value> values;};
void heap_edges(const LifetimeCapture& c,const HeapVisitor& visit){for(const auto& v:c.values)heap_edges(v,visit);}
}
using namespace hua;
namespace {
unsigned checks{},cases{};
void check(bool ok,const char* detail){++checks;if(!ok)throw std::runtime_error(detail);}
template<class F> void rejects(F f,const char* code){try{f();throw std::runtime_error("expected rejection");}catch(const Diagnostic& d){check(d.code()==code,"wrong diagnostic code");}}
Value chain(unsigned count){
    Value value(std::int64_t{7});
    for(unsigned i=0;i<count;++i){
        auto children=managed<std::vector<Value>>();children->push_back(value);
        auto node=managed<StructData>();node->name="Node";
        node->fields.emplace("children",Value(SliceValue{children,0,1,true,"Node"}));
        value=Value(StructValue{node,true});
    }
    return value;
}
Callable captured(const std::shared_ptr<LifetimeCapture>& env){
    Callable result;result.closure=env;std::weak_ptr<LifetimeCapture> weak=env;
    result.resource_guard=[weak](unsigned depth){if(auto c=weak.lock())for(const auto& v:c->values)if(contains_file(v,depth+1))return true;return false;};
    return result;
}
void copy_limits(){
    ++cases;ManagedHeap heap;HeapScope scope(heap);
    {auto root=chain(64);auto before=heap.live();check(!contains_file(root),"64 node chain resource scan");
     {auto cloned=copy_value(root,true);check(heap.live()==before*2,"clone backings");check(!contains_file(cloned),"cloned chain");}
     check(heap.live()==before,"successful clone release");}
    check(heap.live()==0,"chain release");
    {auto root=chain(81);auto before=heap.live();
     for(unsigned i=0;i<20;++i){rejects([&]{copy_value(root,true);},"E4099");check(heap.live()==before,"failed clone leaked");}
     rejects([&]{contains_file(root);},"E4099");}
    heap.collect();check(heap.live()==0,"deep rejection release");
    auto list=managed<ListData>();list->values.emplace_back(ListValue{list,true});Value cycle(ListValue{list,true});
    check(!contains_file(cycle),"resource-free cycle is not File");auto before=heap.live();
    for(unsigned i=0;i<20;++i){rejects([&]{copy_value(cycle,true);},"E4099");check(heap.live()==before,"partial cyclic clone leaked");}
    list->values.emplace_back(FileValue{});rejects([&]{copy_value(cycle,true);},"E4007");
    list->values.pop_back();cycle={};std::weak_ptr<ListData> weak=list;list.reset();heap.collect();
    check(weak.expired()&&heap.live()==0,"cycle reclamation after rejection");
}
void views_and_aliases(){
    ++cases;ManagedHeap heap;HeapScope scope(heap);
    auto storage=managed<std::vector<Value>>();storage->emplace_back(std::int64_t{3});storage->emplace_back(FileValue{});
    Value safe(SliceValue{storage,0,1,true,""}),unsafe(SliceValue{storage,1,1,true,""});
    check(!contains_file(safe)&&contains_file(unsafe),"slice visible ranges");
    auto pair=managed<MultiData>();pair->values={safe,unsafe};rejects([&]{copy_value(Value(MultiValue{pair}),true);},"E4007");
    auto cloned=copy_value(safe,true);check(as_int(sequence_read(std::get<SliceValue>(cloned.data),0),{})==3,"safe view clone");
    auto child=managed<ListData>();child->values.emplace_back(std::int64_t{9});
    auto parent=managed<ListData>();parent->values={Value(ListValue{child,false}),Value(ListValue{child,false})};
    auto copied=std::get<ListValue>(copy_value(Value(ListValue{parent,false}),true).data);
    auto left=std::get<ListValue>(copied.data->values[0].data),right=std::get<ListValue>(copied.data->values[1].data);
    check(left.writable&&right.writable&&copied.writable,"clone restores writable containers");
    check(left.data!=child&&right.data!=child&&left.data!=right.data,"repeated child clone semantics");
    left.data->values[0]=Value(std::int64_t{11});check(as_int(right.data->values[0],{})==9&&as_int(child->values[0],{})==9,"clone isolation");
}
void closure_scan(){
    ++cases;ManagedHeap heap;HeapScope scope(heap);
    auto capture=managed<LifetimeCapture>();auto fn=captured(capture);capture->values.emplace_back(fn);
    check(!contains_file(Value(fn)),"cyclic closure scan");
    capture->values.emplace_back(FileValue{});rejects([&]{copy_value(Value(fn),true);},"E4007");capture->values.pop_back();
    capture->values.push_back(chain(81));rejects([&]{contains_file(Value(fn));},"E4099");capture->values.pop_back();
    check(!contains_file(Value(fn)),"exception restores resource scan context");
    fn={};std::weak_ptr<LifetimeCapture> weak=capture;capture.reset();heap.collect();check(weak.expired()&&heap.live()==0,"closure cycle released");
    Callable native;native.resource_guard=[](unsigned){return true;};rejects([&]{copy_value(Value(native),true);},"E4007");
}
void compiler_roots(){
    ++cases;ManagedHeap heap(8192);HeapScope scope(heap);
    auto nodes=managed<ListData>();nodes->element_type="Node";auto symbols=managed<MapData>();symbols->key_type="string";symbols->item_type="Node";
    std::vector<std::weak_ptr<StructData>> witnesses;
    for(unsigned i=0;i<2048;++i){
        auto node=managed<StructData>();node->name="Node";node->fields.emplace("text",Value(std::string(256,'x')+std::to_string(i)));
        auto children=managed<std::vector<Value>>();children->emplace_back(std::int64_t(i));node->fields.emplace("children",Value(SliceValue{children,0,1,true,"int"}));
        nodes->values.emplace_back(StructValue{node,false});symbols->entries.emplace(std::to_string(i),Value(ResultValue{true,managed<Value>(Value(StructValue{node,false})),"Node","string",false,false}));
        witnesses.push_back(node);
    }
    auto address=witnesses[0].lock().get();auto before=heap.live();check(heap.collect()==0&&heap.live()==before,"AST/symbol roots");
    for(unsigned i=0;i<witnesses.size();++i){auto node=witnesses[i].lock();check(node&&value_kind(node->fields.at("text"))==4,"AST/string root retained");}
    check(witnesses[0].lock().get()==address,"AST address stable");
    auto payload=std::get<ResultValue>(symbols->entries.at("0").data).payload;
    auto view=nodes->values[0];nodes.reset();symbols.reset();heap.collect();
    check(!witnesses[0].expired()&&witnesses[1].expired(),"Result and view roots");
    payload.reset();view={};heap.collect();check(heap.live()==0&&witnesses[0].expired(),"compiler graph release");
}
void engine_release(){
    ++cases;
    std::string stress="struct Node{value int\nchildren []Node}\n";
    for(unsigned i=0;i<1024;++i)stress+="let symbol"+std::to_string(i)+"=\"token-"+std::to_string(i)+"\"\n";
    stress+="fn main(){var root=Node{value:7,children:[]}\nfor i in 0..30{root=Node{value:i,children:[root]}}\nvar copy=clone(root)\nprint(copy.value,symbol1023)}\n";
    struct Spec{std::string text,output,error;};
    std::vector<Spec> specs{{stress,"29 token-1023\n",""},
      {"struct Node{value int\nchildren []Node}\nfn main(){var root=Node{value:0,children:[]}\nfor i in 0..80{root=Node{value:i,children:[root]}}\ndefer print(\"clean\")\nclone(root)}\n","clean\n","E4099"},
      {"fn main(){var a=[1,2,3]\ndefer print(a[0])\na[0]=9\nprint(1//0)}\n","1\n","E4004"}};
    for(const auto& spec:specs){Source source("a09-native.hua",spec.text);auto tree=Parser(Lexer(source).scan()).parse();auto model=SemanticAnalyzer{}.analyze(*tree);auto code=BytecodeCompiler{}.compile(*tree,model);
        for(bool vm:{false,true}){RuntimeContext context;context.heap=std::make_shared<ManagedHeap>();
            for(unsigned run=0;run<3;++run){std::ostringstream out;bool rejected=false;
                try{if(vm)VirtualMachine(model,out,&context).run(code);else Interpreter(model,out,&context).run(*tree);}
                catch(const Diagnostic& d){check(d.code()==spec.error,"engine failure classification");rejected=true;}
                check(rejected==!spec.error.empty()&&out.str()==spec.output,"engine output/cleanup");
                context.heap->collect();check(context.heap->live()==0,"engine root or partial clone leaked");
            }
        }
    }
}
}
int main(){try{copy_limits();views_and_aliases();closure_scan();compiler_roots();engine_release();
    std::cout<<"A09 native lifetime: "<<cases<<" cases, "<<checks<<" checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<"A09 lifetime failure: "<<e.what()<<'\n';return 1;}}
