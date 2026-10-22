#include "hua/vm.hpp"
#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include "hua/interpreter.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
namespace hua {
struct VmSlotTestAccess {
    static bool activation_leases(){
        ManagedHeap heap(65536);HeapScope heap_scope(heap);
        auto arena=std::make_shared<VirtualMachine::FrameArena>();VirtualMachine::ScopeLayout shape;shape.add("slot");
        auto first=arena->acquire(&shape,arena);auto* address=first.get();
        first->insert("slot",{Value(std::int64_t(1)),"int",true});
        ValueLocation retained{&first->own("slot")->value,"int",true,first};first.reset();
        auto second=arena->acquire(&shape,arena);if(second.get()==address)return false;
        second->insert("slot",{Value(std::int64_t(2)),"int",true});location_write(retained,Value(std::int64_t(9)));
        if(as_int(second->own("slot")->value,{})!=2||as_int(location_read(retained,{}),{})!=9)return false;
        retained.owner.reset();auto reused=arena->acquire(&shape,arena);
        if(reused.get()!=address||reused->own("slot"))return false;
        auto cycle=managed<ListData>();cycle->values.emplace_back(ListValue{cycle,true});std::weak_ptr<ListData> weak=cycle;
        reused->insert("slot",{Value(ListValue{cycle,true}),"List<int>",false});cycle.reset();heap.collect();if(weak.expired())return false;
        reused.reset();second.reset();heap.collect();
        return weak.expired()&&heap.live()==0&&arena->work.active==0&&arena->work.created==2&&arena->work.reused==1&&arena->work.retired==3;
    }
    static bool bounded_recency(){
        auto arena=std::make_shared<VirtualMachine::FrameArena>();
        std::array<VirtualMachine::ScopeLayout,65> shapes;
        for(auto& shape:shapes){shape.add("x");auto activation=arena->acquire(&shape,arena);}
        // The newest shape must displace a cold empty frame when capacity is full.
        // The former first-64 cache allocated all 1000 of these activations.
        for(unsigned i=0;i<1000;++i){auto activation=arena->acquire(&shapes.back(),arena);}
        return arena->free.size()==64&&arena->work.created==65&&arena->work.reused==1000&&arena->work.active==0;
    }
    static bool clears_unreachable_cycle(){
        ManagedHeap heap(256);HeapScope heap_scope(heap);
        VirtualMachine::ScopeLayout shape;shape.add("slot");
        auto env=managed<VirtualMachine::Environment>(&shape);
        auto a=managed<StructData>(),b=managed<StructData>(),c=managed<StructData>();
        std::weak_ptr<StructData> wa=a,wb=b,wc=c;std::weak_ptr<VirtualMachine::Environment> we=env;
        env->insert("slot",{Value(StructValue{a,true}),"Root",false});
        env->insert("dynamic",{Value(StructValue{b,true}),"Root",false});
        env->deferred.push_back({Callable{nullptr,"",{},env},{Value(StructValue{c,true})},Instruction{}});
        a.reset();b.reset();c.reset();env.reset();heap.collect();
        return we.expired()&&wa.expired()&&wb.expired()&&wc.expired()&&heap.live()==0;
    }
};
}
using namespace hua;
namespace {
unsigned checks=0;
void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
struct Builder {
    Bytecode bytecode;
    Builder(){bytecode.initializer.name="<slot-test>";}
    std::size_t emit(Op op,std::string text={}){Instruction i{};i.op=op;i.text=std::move(text);bytecode.initializer.instructions.push_back(std::move(i));return size()-1;}
    Instruction& at(std::size_t i){return bytecode.initializer.instructions.at(i);}
    std::size_t size()const{return bytecode.initializer.instructions.size();}
    void integer(std::int64_t value){at(emit(Op::Constant)).constant=Value(value);}
    void boolean(bool value){at(emit(Op::Constant)).constant=Value(value);}
    void bind(std::string name,std::string type="int"){auto& op=at(emit(Op::Bind,std::move(name)));op.type=std::move(type);op.flag=true;}
    void print(std::string name){emit(Op::Load,"print");emit(Op::Load,std::move(name));at(emit(Op::Call)).argument=1;emit(Op::Pop);}
    void finish(){emit(Op::Constant);emit(Op::Return);}
};
Builder late_shadow(){
    Builder b;b.integer(1);b.bind("x");b.boolean(false);b.bind("again","bool");b.emit(Op::EnterScope);
    auto repeat=b.size();b.print("x");b.emit(Op::Load,"again");auto done=b.emit(Op::JumpTrue);
    b.integer(2);b.bind("x");b.emit(Op::LocateName,"again");b.boolean(true);b.emit(Op::Store,"=");b.emit(Op::Pop);
    b.at(b.emit(Op::Jump)).target=repeat;b.at(done).target=b.size();b.emit(Op::LeaveScope);b.finish();return b;
}
Builder retained_location(){
    Builder b;b.integer(1);b.bind("x");b.emit(Op::LocateName,"x");
    // A location already points into the environment while more slots initialize.
    for(unsigned i=0;i<128;++i){b.integer(i);b.bind("later"+std::to_string(i));}
    b.integer(9);b.emit(Op::Store,"=");b.emit(Op::Pop);b.print("x");b.finish();return b;
}
Builder location_after_scope(){
    Builder b;b.emit(Op::EnterScope);b.integer(1);b.bind("x");b.emit(Op::LocateName,"x");b.emit(Op::LeaveScope);
    b.emit(Op::EnterScope);b.integer(2);b.bind("x");b.integer(9);b.emit(Op::Store,"=");b.emit(Op::Pop);b.print("x");b.emit(Op::LeaveScope);b.finish();return b;
}
Builder dictionary_fallback(){
    Builder b;b.integer(1);b.bind("x");b.emit(Op::EnterScope);auto skip=b.emit(Op::Jump);
    // Reachable flow keeps the child scope; the linear storage hint sees this leave.
    b.emit(Op::LeaveScope);b.at(skip).target=b.size();b.integer(2);b.bind("dynamic");b.print("dynamic");
    b.emit(Op::LeaveScope);b.print("x");b.finish();return b;
}
Builder uninitialized(){Builder b;b.print("later");b.integer(1);b.bind("later");b.finish();return b;}
void raw_protocol(){
    SemanticModel model;std::ostringstream output;VirtualMachine vm(model,output);
    for(unsigned repeat=0;repeat<8;++repeat){
        for(auto make:{late_shadow,retained_location,dictionary_fallback,location_after_scope}){
            auto code=make();output.str("");output.clear();check(vm.run(code.bytecode).exit_code==0,"raw entry failed");
            const auto want=make==late_shadow?"1\n2\n":make==retained_location?"9\n":make==location_after_scope?"2\n":"2\n1\n";
            check(output.str()==want,"slot route, fallback, or retained location mismatch");
        }
        auto code=uninitialized();bool rejected=false;
        try{vm.run(code.bytecode);}catch(const Diagnostic& error){rejected=error.code()=="E4001"&&std::string(error.what()).find("later")!=std::string::npos;}
        check(rejected,"reserved but uninitialized slot must retain E4001");
    }
}
void graph_roots(){
    const char* code=R"(struct Box{xs []int}
fn outer(){var box=Box{xs:[7]}
fn callback() int{return box.xs[0]}
for i in 0..64{var scratch=Box{xs:[i]}}
return callback}
fn value(b Box) int{return b.xs[0]}
fn main(){let callback=outer()
var sum=0
for i in 0..128{sum+=value(Box{xs:[i]})}
print(callback(),sum)}
)";
    Source source("slot-roots.hua",code);auto tree=Parser(Lexer(source).scan()).parse();auto model=SemanticAnalyzer{}.analyze(*tree);auto bytecode=BytecodeCompiler{}.compile(*tree,model);
    for(bool ast:{true,false}){RuntimeContext context;context.execution_limits.collection_interval=256;context.heap=std::make_shared<ManagedHeap>(256);HeapScope heap_scope(*context.heap);std::ostringstream output;
        auto entry=ast?Interpreter(model,output,&context).run(*tree):VirtualMachine(model,output,&context).run(bytecode);
        check(context.heap&&context.heap->collection_interval()==256&&context.heap->allocated()>256,"GC fixture did not reach its actual256 allocation interval");
        check(entry.exit_code==0,"forced collection entry failed");check(output.str()=="7 8128\n","slots/captures lost graph roots under forced collection or field identity reuse");
    }
}
}
int main(){try{raw_protocol();graph_roots();check(VmSlotTestAccess::bounded_recency(),"cold cached shapes starved a hot activation");check(VmSlotTestAccess::activation_leases(),"activation lease recycled a live slot or retained business roots");check(VmSlotTestAccess::clears_unreachable_cycle(),"GC clear must release slot, dictionary and deferred closure cycles");std::cout<<checks<<" VM slot, dynamic fallback, location and GC checks passed\n";return 0;}
catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
