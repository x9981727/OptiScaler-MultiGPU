from pathlib import Path
import sys
root=Path(sys.argv[1]);hp=root/'buffered_output.h';h=hp.read_text(encoding='utf-8-sig')
def once(s,a,b):
 if s.count(a)!=1:raise RuntimeError('Precision anchor: '+a[:100])
 return s.replace(a,b)
h=once(h,'#include <stdexcept>','#include <stdexcept>\n#include <fstream>\n#include <iomanip>')
priority='''
inline double LabQpcMs(){
 static const double factor=[](){LARGE_INTEGER f;QueryPerformanceFrequency(&f);return 1000.0/static_cast<double>(f.QuadPart);}();
 LARGE_INTEGER t;QueryPerformanceCounter(&t);return static_cast<double>(t.QuadPart)*factor;
}
struct SchedulingScope {
 HMODULE module=nullptr;HANDLE task=nullptr;int oldPriority=THREAD_PRIORITY_NORMAL;bool enabled=false;
 using Begin=HANDLE(WINAPI*)(LPCWSTR,LPDWORD);using Set=BOOL(WINAPI*)(HANDLE,int);using End=BOOL(WINAPI*)(HANDLE);
 End end=nullptr;
 SchedulingScope(){
  char b[8]{};if(!GetEnvironmentVariableA("XEFG_PACER_PRIORITY",b,8)||b[0]=='0')return;
  enabled=true;oldPriority=GetThreadPriority(GetCurrentThread());
  const BOOL high=SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_HIGHEST);
  if(b[0]=='2'){
   module=LoadLibraryExW(L"avrt.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
   if(module){
    auto begin=reinterpret_cast<Begin>(GetProcAddress(module,"AvSetMmThreadCharacteristicsW"));
    auto set=reinterpret_cast<Set>(GetProcAddress(module,"AvSetMmThreadPriority"));
    end=reinterpret_cast<End>(GetProcAddress(module,"AvRevertMmThreadCharacteristics"));
    if(begin&&set&&end){DWORD index=0;task=begin(L"Games",&index);if(task)set(task,1);}
   }
  }
  std::fprintf(stderr,"PACER_THREAD_PRIORITY mode=%c thread=%lu highest=%d mmcss=%d\\n",b[0],GetCurrentThreadId(),int(high),int(task!=nullptr));std::fflush(stderr);
 }
 ~SchedulingScope(){if(task&&end)end(task);if(module)FreeLibrary(module);if(enabled&&oldPriority!=THREAD_PRIORITY_ERROR_RETURN)SetThreadPriority(GetCurrentThread(),oldPriority);}
};
'''
h=once(h,'struct State {',priority+'\nstruct State {')
h=once(h,' double blockedMs=0,copyMs=0,paceMs=0;', ' double blockedMs=0,copyMs=0,paceMs=0;\n std::vector<std::array<double,5>> outputRecords;')
h=once(h,' void Work() noexcept{',' void Work() noexcept{\n  SchedulingScope scheduling;')
h=once(h,'   const UINT index=Original<IndexFn>(36)(chain.Get());','   UINT index;{Actual scope;index=Original<IndexFn>(36)(chain.Get());}')
h=once(h,'   const auto begin=Clock::now();','   const auto begin=Clock::now();const double beginQpc=LabQpcMs();')
h=once(h,'   if(scheduleOutputGate)Check(scheduleOutputGate());','   const double readyQpc=LabQpcMs();\n   if(scheduleOutputGate)Check(scheduleOutputGate());\n   const double presentQpc=LabQpcMs();')
h=once(h,'   completed=job.id;cv.notify_all();','   if(outputRecords.size()<100000)outputRecords.push_back({static_cast<double>(job.id),beginQpc,readyQpc,presentQpc,LabQpcMs()});\n   completed=job.id;cv.notify_all();')
h=once(h,'  return safe;', '''  char path[32768]{};DWORD count=GetEnvironmentVariableA("XEFG_LAB_TRACE_DIR",path,sizeof(path));
  if(count&&count<sizeof(path)){
   std::ofstream f(std::string(path)+"/output-cadence.csv");f<<"output_id,copy_begin_qpc_ms,ready_qpc_ms,present_begin_qpc_ms,present_end_qpc_ms\\n"<<std::fixed<<std::setprecision(5);
   for(auto& r:outputRecords)f<<r[0]<<','<<r[1]<<','<<r[2]<<','<<r[3]<<','<<r[4]<<'\\n';
  }
  return safe;''')
hp.write_text(h,encoding='utf-8')
p=root/'native_gate.cpp';s=p.read_text(encoding='utf-8-sig')
s=once(s,'    double waitMs = 0, elapsedMs = 0;', '    double waitMs = 0, elapsedMs = 0;\n    std::vector<std::array<double,5>> releaseRecords;')
s=once(s,'    void Worker() {','    void Worker() {\n        BufferedOutputLab::SchedulingScope scheduling;')
s=once(s,'            UINT64 id;', '            UINT64 id;double readyQpc=0,targetQpc=0;')
s=once(s,'                auto now=Clock::now();','                auto now=Clock::now();readyQpc=BufferedOutputLab::LabQpcMs();')
s=once(s,'                if (!failed && deadline>now) {','                targetQpc=BufferedOutputLab::LabQpcMs()+std::chrono::duration<double,std::milli>(deadline-Clock::now()).count();\n                if (!failed && deadline>now) {')
s=once(s,'            auto hr=ReleaseTo(id);','            const double releaseBefore=BufferedOutputLab::LabQpcMs();\n            auto hr=ReleaseTo(id);\n            if(releaseRecords.size()<100000)releaseRecords.push_back({static_cast<double>(id),readyQpc,targetQpc,releaseBefore,BufferedOutputLab::LabQpcMs()});')
s=once(s,'        readyEvent=timer=nullptr;', '''        readyEvent=timer=nullptr;
        char path[32768]{};DWORD count=GetEnvironmentVariableA("XEFG_LAB_TRACE_DIR",path,sizeof(path));
        if(count&&count<sizeof(path)){
            std::ofstream f(std::string(path)+"/release-cadence.csv");f<<"output_id,gpu_ready_qpc_ms,target_qpc_ms,signal_begin_qpc_ms,signal_end_qpc_ms\\n"<<std::fixed<<std::setprecision(5);
            for(auto& r:releaseRecords)f<<r[0]<<','<<r[1]<<','<<r[2]<<','<<r[3]<<','<<r[4]<<'\\n';
        }''')
p.write_text(s,encoding='utf-8');print('Own-thread priority is optional; QPC deadline telemetry enabled. No system tuning.')
