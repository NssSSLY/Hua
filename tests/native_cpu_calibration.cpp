// Fixed independent 1:3 CPU ratio, a known parent frame, and an idle interval.
#include <windows.h>
#include <chrono>
#include <cstdint>
#include <iostream>
volatile std::uint64_t sink=1;
using Clock=std::chrono::steady_clock;
__attribute__((noinline)) void calibration_quarter() {
    auto until=Clock::now()+std::chrono::milliseconds(400);
    do { for(int i=0;i<4096;++i) sink=(sink*1664525+1013904223)^std::uint64_t(i); }
    while(Clock::now()<until);
}
__attribute__((noinline)) void calibration_three_quarters() {
    auto until=Clock::now()+std::chrono::milliseconds(1200);
    do { for(int i=0;i<4096;++i) sink=(sink*1664525+1013904223)^std::uint64_t(i); }
    while(Clock::now()<until);
}
__attribute__((noinline)) void calibration_parent() {
    calibration_quarter(); calibration_three_quarters(); Sleep(200);
}
int main(int argc,char**) {
    if(argc>1) { Sleep(1000); return 0; }
    calibration_parent(); std::cout<<"calibration completed\n";
}
