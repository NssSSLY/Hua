// Uses the same public native pipeline as main.cpp; traces coarse stage boundaries only.
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include "hua/module.hpp"
#include "hua/sema.hpp"
#include "hua/archive.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

using Clock=std::chrono::steady_clock;
double cpu() {
    FILETIME c{},e{},k{},u{};
    if(!GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u))throw std::runtime_error("GetProcessTimes failed");
    return double(((std::uint64_t(k.dwHighDateTime)<<32)|k.dwLowDateTime)+
                  ((std::uint64_t(u.dwHighDateTime)<<32)|u.dwLowDateTime))/1e7;
}
struct Trace {
    std::ofstream file;Clock::time_point start=Clock::now();double initial_cpu=cpu();
    explicit Trace(const std::filesystem::path& path):file(path){if(!file)throw std::runtime_error("cannot create stage trace");}
    void record(const char* event,const char* phase) {
        PROCESS_MEMORY_COUNTERS memory{};memory.cb=sizeof(memory);
        GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory));
        file<<std::setprecision(12)<<"{\"event\":\""<<event<<"\",\"phase\":\""<<phase
            <<"\",\"seconds\":"<<std::chrono::duration<double>(Clock::now()-start).count()
            <<",\"cpu_seconds\":"<<cpu()-initial_cpu<<",\"peak_working_bytes\":"<<memory.PeakWorkingSetSize<<"}\n";
        file.flush();if(!file)throw std::runtime_error("stage trace write failed");
    }
};
int wmain(int argc,wchar_t** argv) {
    if(argc!=4){std::cerr<<"usage: native_compile_probe trace.jsonl entry.hua output.huab\n";return 2;}
    try {
        if(std::filesystem::exists(argv[1])||std::filesystem::exists(argv[3]))throw std::runtime_error("evidence already exists");
        Trace trace(argv[1]);trace.record("begin","execution");
        try {
            {
                trace.record("begin","load");hua::ModuleLoader loader;const auto& tree=loader.load(argv[2]);trace.record("end","load");
                trace.record("begin","sema");auto model=hua::SemanticAnalyzer{}.analyze(tree,loader.externals());trace.record("end","sema");
                trace.record("begin","bytecode");auto bytecode=hua::BytecodeCompiler{}.compile(tree,model);trace.record("end","bytecode");
                trace.record("begin","archive");hua::write_huab(argv[3],bytecode,model,loader.sources());trace.record("end","archive");
                trace.record("begin","destroy");
            }
            trace.record("end","destroy");trace.record("end","execution");
        }catch(...){trace.record("failure","execution");throw;}
        std::cout<<"Built\n";return 0;
    }catch(const hua::Diagnostic& error){std::cerr<<"error["<<error.code()<<"] "<<error.what()<<'\n';return 1;}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 2;}
}
