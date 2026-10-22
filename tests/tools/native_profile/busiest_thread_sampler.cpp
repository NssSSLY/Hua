// Profiling variant of tests/tools/native_cpu_sampler.cpp: each tick samples the child thread that
// used the most cycles since the previous tick. On Windows (2026-10-22) the main-thread original only
// recorded ntdll frames for `hua run`; this variant gets hua.exe frames. The cause was not established.
// Build: clang++ -O2 -std=c++20 -static busiest_thread_sampler.cpp -o tests/build-prof/sampler.exe -ldbghelp -lpsapi -municode
// Standalone Windows x64 sampler for its own child process only.
// DbgHelp unwinds PE runtime tables; LLVM symbolizes addresses offline.
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <map>
using Clock = std::chrono::steady_clock;
struct Handle {
    HANDLE value{};
    explicit Handle(HANDLE h = nullptr): value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
};
std::string utf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);
    std::string result(n,'\0');
    WideCharToMultiByte(CP_UTF8,0,s.data(),int(s.size()),result.data(),n,nullptr,nullptr);
    return result;
}
std::string json(const std::string& s) {
    std::string result = "\"";
    for (unsigned char c:s) {
        if (c=='"' || c=='\\') { result+='\\'; result+=char(c); }
        else if (c<32) { const char* h="0123456789abcdef"; result+="\\u00"; result+=h[c>>4]; result+=h[c&15]; }
        else result+=char(c);
    }
    return result+'"';
}
std::wstring quote(const std::wstring& s) {
    std::wstring result=L"\""; unsigned slashes=0;
    for (wchar_t c:s) {
        if (c==L'\\') { ++slashes; continue; }
        result.append(c==L'"' ? slashes*2+1 : slashes,L'\\'); slashes=0; result+=c;
    }
    result.append(slashes*2,L'\\'); return result+L'"';
}
double cpu(HANDLE h, bool thread) {
    FILETIME c{},e{},k{},u{};
    if (!(thread ? GetThreadTimes(h,&c,&e,&k,&u) : GetProcessTimes(h,&c,&e,&k,&u)))
        throw std::runtime_error("GetProcess/ThreadTimes failed");
    return double(((std::uint64_t(k.dwHighDateTime)<<32)|k.dwLowDateTime)+
                  ((std::uint64_t(u.dwHighDateTime)<<32)|u.dwLowDateTime))/1e7;
}
struct Module { std::string path; std::uint64_t base,size; };
std::vector<Module> modules(DWORD pid) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid));
    if (snapshot.value==INVALID_HANDLE_VALUE) throw std::runtime_error("module snapshot failed");
    MODULEENTRY32W entry{}; entry.dwSize=sizeof(entry); std::vector<Module> result;
    if (!Module32FirstW(snapshot.value,&entry)) throw std::runtime_error("module enumeration failed");
    do { result.push_back({utf8(entry.szExePath),reinterpret_cast<std::uint64_t>(entry.modBaseAddr),entry.modBaseSize}); }
    while (Module32NextW(snapshot.value,&entry));
    return result;
}
struct Sample { std::uint64_t ns,cycles,pause_ns; std::vector<DWORD64> frames; };
int wmain(int argc,wchar_t** argv) {
    if (argc<5) { std::cerr<<"usage: sampler trace.json interval_ms limit_ms exe [args...]\n"; return 2; }
    try {
        std::filesystem::path output(argv[1]);
        if (std::filesystem::exists(output)) throw std::runtime_error("trace already exists");
        int interval=std::stoi(argv[2]),limit=std::stoi(argv[3]);
        // Diagnostic watchdog, independent of Hua VM budgets. Long host compilation
        // can use an explicit finite limit after coarse stages have been measured.
        if(interval<1 || interval>20 || limit<100 || limit>180000) throw std::runtime_error("invalid sampling bounds");
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
        Handle out(CreateFileW((output.wstring()+L".stdout").c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_NEW,0,nullptr));
        Handle err(CreateFileW((output.wstring()+L".stderr").c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_NEW,0,nullptr));
        Handle in(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr));
        if(out.value==INVALID_HANDLE_VALUE || err.value==INVALID_HANDLE_VALUE || in.value==INVALID_HANDLE_VALUE)
            throw std::runtime_error("cannot create child streams");
        Handle job(CreateJobObjectW(nullptr,nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!job.value || !SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))
            throw std::runtime_error("cannot establish child cleanup job");
        std::wstring command;
        for(int i=4;i<argc;++i) { if(i>4) command+=L' '; command+=quote(argv[i]); }
        STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESTDHANDLES;
        startup.hStdOutput=out.value; startup.hStdError=err.value; startup.hStdInput=in.value;
        PROCESS_INFORMATION process{};
        if(!CreateProcessW(argv[4],command.data(),nullptr,nullptr,TRUE,CREATE_SUSPENDED|CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))
            throw std::runtime_error("CreateProcess failed: "+std::to_string(GetLastError()));
        Handle child(process.hProcess),thread(process.hThread);
        if(!AssignProcessToJobObject(job.value,child.value)) {
            TerminateProcess(child.value,125); throw std::runtime_error("cannot attach child cleanup job");
        }
        // Let the child's loader publish its module list before DbgHelp invades it.
        auto start=Clock::now(); ResumeThread(thread.value);
        if(WaitForSingleObject(child.value,20)!=WAIT_TIMEOUT)
            throw std::runtime_error("child exited before sampling initialization");
        // Local symbols only. Never contact a symbol server or inspect another process.
        SymSetOptions(SYMOPT_DEFERRED_LOADS|SYMOPT_FAIL_CRITICAL_ERRORS|SYMOPT_NO_PROMPTS|SYMOPT_IGNORE_NT_SYMPATH);
        if(!SymInitializeW(child.value,L".",TRUE))
            throw std::runtime_error("SymInitialize failed: "+std::to_string(GetLastError()));
        struct Cleanup { HANDLE h; ~Cleanup(){SymCleanup(h);} } cleanup{child.value};
        auto loaded_modules=modules(process.dwProcessId);
        Handle timer(CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_ALL_ACCESS));
        if(!timer.value) throw std::runtime_error("high resolution waitable timer unavailable");
        std::vector<Sample> samples; std::uint64_t previous=0;
        std::map<DWORD,std::pair<HANDLE,ULONG64>> threads; unsigned tick=0;
        unsigned inactive=0,context_failures=0; bool timeout=false,refreshed=false;
        HANDLE waits[]={child.value,timer.value};
        while(true) {
            LARGE_INTEGER due{}; due.QuadPart=-LONGLONG(interval)*10000;
            if(!SetWaitableTimer(timer.value,&due,0,nullptr,nullptr,FALSE)) throw std::runtime_error("timer failed");
            DWORD waited=WaitForMultipleObjects(2,waits,FALSE,INFINITE);
            if(waited==WAIT_OBJECT_0) break;
            if(waited!=WAIT_OBJECT_0+1) throw std::runtime_error("sample wait failed");
            if(!refreshed && Clock::now()-start>std::chrono::milliseconds(40)) {
                loaded_modules=modules(process.dwProcessId); SymRefreshModuleList(child.value); refreshed=true;
            }
            if(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count()>=limit) {
                timeout=true; TerminateJobObject(job.value,124); WaitForSingleObject(child.value,5000); break;
            }
            // Profiling copy: sample the busiest thread of the child since the previous tick.
            HANDLE busiest=nullptr; ULONG64 delta=0;
            if(tick++%256==0) { Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0)); THREADENTRY32 te{}; te.dwSize=sizeof(te);
              if(Thread32First(snap.value,&te)) do { if(te.th32OwnerProcessID!=process.dwProcessId) continue;
                auto& slot=threads[te.th32ThreadID];
                if(!slot.first) slot.first=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,te.th32ThreadID);
              } while(Thread32Next(snap.value,&te)); }
            for(auto& [id,slot]:threads){ ULONG64 c=0; if(!slot.first||!QueryThreadCycleTime(slot.first,&c)) continue;
                auto d=c-slot.second; slot.second=c; if(d>delta){delta=d;busiest=slot.first;} }
            (void)previous;
            if(!delta||!busiest) { ++inactive; continue; }
            auto paused=Clock::now();
            if(SuspendThread(busiest)==DWORD(-1)) { ++context_failures; continue; }
            struct Resume { HANDLE h; ~Resume(){ResumeThread(h);} } resume{busiest};
            CONTEXT context{}; context.ContextFlags=CONTEXT_FULL;
            if(!GetThreadContext(busiest,&context)) { ++context_failures; continue; }
            Sample sample{std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(paused-start).count()),delta,0,{context.Rip}};
            STACKFRAME64 frame{}; frame.AddrPC.Offset=context.Rip; frame.AddrPC.Mode=AddrModeFlat;
            frame.AddrFrame.Offset=context.Rbp; frame.AddrFrame.Mode=AddrModeFlat;
            frame.AddrStack.Offset=context.Rsp; frame.AddrStack.Mode=AddrModeFlat;
            DWORD64 last_sp=frame.AddrStack.Offset,last_pc=frame.AddrPC.Offset;
            for(int depth=0;depth<63;++depth) {
                if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64,child.value,busiest,&frame,&context,nullptr,
                                SymFunctionTableAccess64,SymGetModuleBase64,nullptr) || !frame.AddrPC.Offset) break;
                if(frame.AddrPC.Offset==last_pc && frame.AddrStack.Offset==last_sp) { if(depth==0) continue; break; }
                sample.frames.push_back(frame.AddrPC.Offset);
                last_pc=frame.AddrPC.Offset; last_sp=frame.AddrStack.Offset;
            }
            sample.pause_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-paused).count();
            samples.push_back(std::move(sample));
            if(samples.size()>120000) throw std::runtime_error("sample bound exceeded");
        }
        double wall=std::chrono::duration<double>(Clock::now()-start).count(); DWORD exit_code=0;
        GetExitCodeProcess(child.value,&exit_code);
        PROCESS_MEMORY_COUNTERS memory{}; memory.cb=sizeof(memory);
        GetProcessMemoryInfo(child.value,&memory,sizeof(memory));
        std::ofstream trace(output); trace<<std::setprecision(12);
        trace<<"{\"schema\":\"hua.native-cpu.samples.v1\",\"high_resolution_timer\":true,\"interval_ms\":"<<interval<<",\"limit_ms\":"<<limit
             <<",\"timeout\":"<<(timeout?"true":"false")<<",\"exit_code\":"<<exit_code<<",\"wall_seconds\":"<<wall
             <<",\"cpu_seconds\":"<<cpu(child.value,false)<<",\"main_thread_cpu_seconds\":"<<cpu(thread.value,true)
             <<",\"peak_working_bytes\":"<<memory.PeakWorkingSetSize<<",\"inactive_polls\":"<<inactive
             <<",\"context_failures\":"<<context_failures<<",\"modules\":[";
        for(std::size_t i=0;i<loaded_modules.size();++i) { const auto& m=loaded_modules[i];
            if(i)trace<<','; trace<<"{\"path\":"<<json(m.path)<<",\"base\":"<<m.base<<",\"size\":"<<m.size<<'}'; }
        trace<<"],\"samples\":[";
        for(std::size_t i=0;i<samples.size();++i) { const auto& s=samples[i]; if(i)trace<<',';
            trace<<"{\"ns\":"<<s.ns<<",\"cycle_delta\":"<<s.cycles<<",\"pause_ns\":"<<s.pause_ns<<",\"frames\":[";
            for(std::size_t j=0;j<s.frames.size();++j) { if(j)trace<<','; trace<<s.frames[j]; } trace<<"]}"; }
        trace<<"]}\n"; trace.close(); if(!trace)throw std::runtime_error("trace write failed");
        std::cout<<"samples="<<samples.size()<<" wall="<<wall<<" exit="<<exit_code<<" timeout="<<timeout<<'\n';
        return timeout?124:int(exit_code);
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 2; }
}
