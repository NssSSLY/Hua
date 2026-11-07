#include "hua/runtime.hpp"
#include "hua/tasks.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
using namespace hua;
using I=std::int64_t;
int main(){
    int checks=0;std::ostringstream output;
    auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("math arithmetic native contract failed");};
    auto diagnostic=[&](const char* code,auto operation){try{operation();}catch(const Diagnostic& e){check(e.code()==code);return;}throw std::runtime_error("missing diagnostic");};
    auto call=[&](const StandardFunction& f,std::vector<Value> args){return invoke_standard(f,args,{},output,nullptr);};
    RuntimeContext canceled;canceled.cancellation=std::make_shared<Cancellation>();canceled.cancellation->requested=true;
    for(const auto& f:standard_functions())if(f.since==22){
        std::vector<Value> args;
        for(const auto& type:f.parameters)args.emplace_back(type=="float"?Value(1.0):Value(I{1}));
        auto wrong=args;wrong[0]=Value(std::string("bad"));diagnostic("E4003",[&]{call(f,wrong);});
        auto exact=args;exact[0]=f.parameters[0]=="float"?convert_numeric(Value(1.0),"f32",{},true):convert_numeric(Value(I{1}),"i32",{},true);
        diagnostic("E4003",[&]{call(f,exact);});
        diagnostic("E4003",[&]{call(f,{});});
        diagnostic("E4101",[&]{invoke_algorithm_standard(f,args,{},&canceled);});
    }
    for(const char* name:{"sinh","cosh","tanh","asinh","acosh","atanh"}){
        const auto& f=*standard_function(std::string("$std$math$")+name);
        for(double input:{std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){
            auto v=call(f,{Value(input)});const auto& result=std::get<ResultValue>(v.data);
            check(!result.ok&&string_value(result.payload->data).starts_with("MATH_DOMAIN:"));
        }
    }
    for(const char* name:{"sinh","tanh","asinh","atanh"}){
        auto v=call(*standard_function(std::string("$std$math$")+name),{Value(-0.0)});
        const auto& result=std::get<ResultValue>(v.data);check(result.ok&&std::signbit(std::get<double>(result.payload->data)));
    }
    auto v=call(*standard_function("$std$math$acosh"),{Value(1.0)});
    const auto& result=std::get<ResultValue>(v.data);check(result.ok&&std::get<double>(result.payload->data)==0.0&&!std::signbit(std::get<double>(result.payload->data)));
    std::cout<<checks<<" math arithmetic native type/arity/cancellation/finite/zero checks passed\n";
}