#include "hua/entry.hpp"
#include <iostream>
#include <filesystem>
#include <sstream>
#include <stdexcept>
int main() {
    int checks=0;
    auto verify=[&](const std::function<int()>& work,int wanted,const std::string& code){
        std::ostringstream error;auto actual=hua::command_boundary(work,error);
        ++checks;if(actual!=wanted || (!code.empty()&&error.str().find(code)==std::string::npos) || (code.empty()&&!error.str().empty()))
            throw std::runtime_error("command boundary failure: "+error.str());
    };
    verify([]{return 0;},0,"");
    verify([]{return 2;},2,"");
    verify([]()->int{throw std::runtime_error("injected host failure");},3,"error[E0002]");
    verify([]()->int{throw std::logic_error("injected invariant failure");},3,"error[E0002]");
    verify([]()->int{throw std::bad_alloc();},3,"error[E0002]");
    verify([]()->int{throw 7;},3,"unknown host exception");
    verify([]()->int{throw std::filesystem::filesystem_error("publication",std::make_error_code(std::errc::permission_denied));},1,"error[E0001]");
    verify([]()->int{throw hua::Diagnostic("E3004",{},"type mismatch");},1,"error[E3004]");
    verify([]()->int{throw hua::Diagnostic("E6001",{},"invalid VM instruction/stack state");},3,"error[E6001]");
    std::cout<<checks<<" command boundary fault checks passed\n";
}
