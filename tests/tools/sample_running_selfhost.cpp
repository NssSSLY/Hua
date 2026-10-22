#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <psapi.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>
using Clock=std::chrono::steady_clock;
struct Resume {HANDLE thread;~Resume(){ResumeThread(thread);}};
int main(int argc,char** argv){
 if(argc!=3)return 2;DWORD pid=std::stoul(argv[1]);std::ofstream out(argv[2]);if(!out)return 3;
 HANDLE process=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ|SYNCHRONIZE,FALSE,pid);if(!process){std::cerr<<GetLastError();return 4;}
 DWORD tid=0;HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);THREADENTRY32 entry{};entry.dwSize=sizeof(entry);
 if(Thread32First(snap,&entry))do{if(entry.th32OwnerProcessID==pid){tid=entry.th32ThreadID;break;}}while(Thread32Next(snap,&entry));CloseHandle(snap);
 HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,tid);if(!thread){std::cerr<<GetLastError();return 5;}
 SymSetOptions(SYMOPT_DEFERRED_LOADS|SYMOPT_UNDNAME|SYMOPT_FAIL_CRITICAL_ERRORS|SYMOPT_NO_PROMPTS);
 if(!SymInitialize(process,"",TRUE)){std::cerr<<GetLastError();return 6;}
 auto start=Clock::now();double suspended=0;unsigned samples=0;
 HMODULE mods[512];DWORD needed=0;
 out<<"{\"event\":\"modules\",\"pid\":"<<pid<<",\"tid\":"<<tid<<",\"modules\":[";bool first=true;
 if(EnumProcessModules(process,mods,sizeof(mods),&needed))for(unsigned i=0;i<needed/sizeof(HMODULE)&&i<512;++i){char path[MAX_PATH];MODULEINFO info{};
  if(GetModuleFileNameExA(process,mods[i],path,sizeof(path))&&GetModuleInformation(process,mods[i],&info,sizeof(info))){if(!first)out<<',';first=false;
   std::string name=path;for(auto& c:name)if(c=='\\')c='/';out<<"{\"path\":"<<std::quoted(name)<<",\"base\":"<<(std::uintptr_t)info.lpBaseOfDll<<",\"size\":"<<info.SizeOfImage<<'}';}}
 out<<"]}\n";out.flush();
 while(WaitForSingleObject(process,0)==WAIT_TIMEOUT&&std::chrono::duration<double>(Clock::now()-start).count()<605){
  std::vector<DWORD64> pcs;auto before=Clock::now();DWORD error=0;
  if(SuspendThread(thread)!=DWORD(-1)){{Resume resume{thread};CONTEXT context{};context.ContextFlags=CONTEXT_FULL;
   if(GetThreadContext(thread,&context)){STACKFRAME64 frame{};frame.AddrPC.Offset=context.Rip;frame.AddrPC.Mode=AddrModeFlat;frame.AddrStack.Offset=context.Rsp;frame.AddrStack.Mode=AddrModeFlat;frame.AddrFrame.Offset=context.Rbp;frame.AddrFrame.Mode=AddrModeFlat;
    pcs.push_back(context.Rip);for(int depth=0;depth<63;++depth){if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64,process,thread,&frame,&context,nullptr,SymFunctionTableAccess64,SymGetModuleBase64,nullptr)||!frame.AddrPC.Offset)break;if(pcs.back()!=frame.AddrPC.Offset)pcs.push_back(frame.AddrPC.Offset);}}
   else error=GetLastError();}}
  else error=GetLastError();
  auto after=Clock::now();double pause=std::chrono::duration<double>(after-before).count();suspended+=pause;++samples;
  out<<"{\"event\":\"sample\",\"seconds\":"<<std::chrono::duration<double>(after-start).count()<<",\"pause_seconds\":"<<pause<<",\"error\":"<<error<<",\"pc\":[";
  for(unsigned i=0;i<pcs.size();++i){if(i)out<<',';out<<pcs[i];}out<<"]}\n";out.flush();
  WaitForSingleObject(process,250);
 }
 out<<"{\"event\":\"end\",\"samples\":"<<samples<<",\"total_suspend_seconds\":"<<suspended<<"}\n";out.flush();SymCleanup(process);CloseHandle(thread);CloseHandle(process);std::cout<<"samples="<<samples<<" suspend_seconds="<<suspended<<'\n';return 0;
}
