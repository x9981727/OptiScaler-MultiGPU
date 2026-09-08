#pragma once
// Isolated reference experiment. Changes only HIGH queues created during this
// sample's XeFG initialization, to NORMAL. No driver files, registry, tokens,
// global realtime queues, clocks, or other processes are modified.
#include <windows.h>
#include <d3d12.h>
#include <detours.h>
#include <atomic>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
namespace NativeGateLab {
class SdkQueuePriorityScope {
 using CreateFn=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_COMMAND_QUEUE_DESC*,REFIID,void**);
 inline static CreateFn original=nullptr;
 inline static std::atomic<bool> enabled{false};
 inline static std::atomic<unsigned> changed{0};
 inline static thread_local bool preserveDisplay=false;
 inline static std::mutex logMutex;
 inline static std::string logPath;
 static HRESULT STDMETHODCALLTYPE Create(ID3D12Device*d,const D3D12_COMMAND_QUEUE_DESC*desc,REFIID iid,void**out){
  if(!desc)return original(d,desc,iid,out);
  auto copy=*desc;bool change=enabled.load()&&!preserveDisplay&&copy.Type==D3D12_COMMAND_LIST_TYPE_DIRECT&&copy.Priority==D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
  if(change)copy.Priority=D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  HRESULT hr=original(d,&copy,iid,out);
  if(change){++changed;std::lock_guard<std::mutex> lock(logMutex);std::ofstream f(logPath,std::ios::app);f<<GetCurrentThreadId()<<",100,0,"<<static_cast<long>(hr)<<"\n";}
  return hr;
 }
public:
 struct DisplayGuard{bool previous=preserveDisplay;DisplayGuard(){preserveDisplay=true;}~DisplayGuard(){preserveDisplay=previous;}};
 SdkQueuePriorityScope(ID3D12Device*d,const std::string&path){
  if(!d||enabled.load())throw std::runtime_error("invalid priority experiment scope");
  logPath=path+"\\sdk-queue-priority.csv";
  {std::ofstream f(logPath);f<<"thread,requested_priority,experiment_priority,result\n";}
  if(!original){
   original=reinterpret_cast<CreateFn>((*reinterpret_cast<void***>(d))[8]);
   LONG hr=DetourTransactionBegin();if(hr!=NO_ERROR)throw std::runtime_error("priority transaction begin");
   hr=DetourUpdateThread(GetCurrentThread());
   if(hr==NO_ERROR)hr=DetourAttach(reinterpret_cast<PVOID*>(&original),reinterpret_cast<PVOID>(&Create));
   if(hr!=NO_ERROR){DetourTransactionAbort();original=nullptr;throw std::runtime_error("priority hook unavailable");}
   hr=DetourTransactionCommit();if(hr!=NO_ERROR){original=nullptr;throw std::runtime_error("priority hook commit");}
  }
  enabled=true;
 }
 ~SdkQueuePriorityScope(){
  enabled=false;
  // Keep the inert forwarding trampoline valid until this short-lived process
  // exits. Do not detach executable code while SDK background threads may use it.
 }
};
}
