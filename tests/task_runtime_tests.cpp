#include "hua/tasks.hpp"
#include "hua/stdlib.hpp"
#include <iostream>
#include <stdexcept>
int main(){using namespace hua;using namespace std::chrono_literals;
 RuntimeContext context;std::ostringstream output;TaskRuntime runtime(context,output);context.tasks=&runtime;
 std::atomic<unsigned> ready{0};std::atomic<bool> release{false};std::vector<Value> tasks;
 auto owner=std::this_thread::get_id();
 for(unsigned i=0;i<8;++i)tasks.push_back(runtime.start([&,i](RuntimeContext& c,std::ostream&){++ready;while(!release){task_checkpoint(&c,{});std::this_thread::yield();}if(owner==std::this_thread::get_id())throw std::runtime_error("task ran on caller thread");return Value(std::int64_t(i));},"int",{}));
 auto deadline=std::chrono::steady_clock::now()+5s;while(ready<8&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
 if(ready!=8)throw std::runtime_error("workers did not execute concurrently");release=true;
 for(unsigned i=0;i<8;++i)if(as_int(runtime.await(tasks[i],{}),{})!=i)throw std::runtime_error("result order");runtime.finish();tasks.clear();
 auto slow=runtime.start([](RuntimeContext& c,std::ostream&)->Value{for(;;){task_checkpoint(&c,{});std::this_thread::sleep_for(1ms);}},"int",{});
 auto result=runtime.await(slow,{},1ms);if(show(result)!="err(timeout)")throw std::runtime_error("timeout");runtime.finish();slow={};
 context.cancellation=std::make_shared<Cancellation>();auto failed=runtime.start([](RuntimeContext&,std::ostream&)->Value{runtime_error({},"failure","E4004");},"int",{});
 runtime.await(failed,{},1000ms);context.cancellation->requested=true;
 if(show(task_standard("state",{failed},{},&context))!="failed")throw std::runtime_error("completed failure changed status after parent cancellation");runtime.finish();failed={};context.cancellation.reset();
 release=false;for(unsigned i=0;i<64;++i)tasks.push_back(runtime.start([&](RuntimeContext& c,std::ostream&)->Value{while(!release){task_checkpoint(&c,{});std::this_thread::sleep_for(1ms);}return Value(std::int64_t(1));},"int",{}));
 bool limited=false;try{runtime.start([](RuntimeContext&,std::ostream&){return Value(std::int64_t(0));},"int",{});}catch(const Diagnostic& d){limited=d.code()=="E4099";}
 if(!limited)throw std::runtime_error("concurrent task bound");runtime.finish(true);tasks.clear();
 std::cout<<"Task concurrency barrier, ordering, timeout and 64-worker bound OK\n";
}
