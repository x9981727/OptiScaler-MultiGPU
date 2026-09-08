#pragma once
// ISOLATED REFERENCE ONLY. Not a game-ready DXGI replacement: resize, fullscreen,
// HDR and device recreation require separate integration and tests.
// No output is discarded. Accepted native output submissions are followed by a
// real Present on a consumer thread. Asynchronous failures are sticky and exposed
// by subsequent submissions; acceptance is not a claim of completed scanout.
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "NativeFrameStage.h"
namespace NativeGateLab {
using Microsoft::WRL::ComPtr;
inline double QpcMs(){LARGE_INTEGER a,b;QueryPerformanceCounter(&a);QueryPerformanceFrequency(&b);return 1000.0*static_cast<double>(a.QuadPart)/static_cast<double>(b.QuadPart);}
inline bool EnvFlag(const char* n){char b[32]{};return GetEnvironmentVariableA(n,b,sizeof(b)) && b[0]=='1';}
inline std::string EnvPath(){char b[32768]{};DWORD n=GetEnvironmentVariableA("XEFG_LAB_TRACE_DIR",b,sizeof(b));return n && n<sizeof(b)?std::string(b):".";}
inline std::atomic<double> sourcePeriod{0};
inline std::atomic<bool> active{false};
inline void Source(double ms,bool enabled){active=enabled;if(std::isfinite(ms)&&ms>=2&&ms<=100){double old=sourcePeriod.load();sourcePeriod=old>0?0.85*old+0.15*ms:ms;}}
inline float Hint(float actual){char b[32]{};if(!GetEnvironmentVariableA("XEFG_LAB_HINT_MS",b,sizeof(b)))return actual;char* end=nullptr;double v=strtod(b,&end);return end!=b&&*end==0&&std::isfinite(v)&&v>=0&&v<100?static_cast<float>(v):actual;}
inline void DebugSetup(){} // Existing system runtime; no optional runtime installation.
inline thread_local bool insideNative=false;
struct NativeScope{bool old=insideNative;NativeScope(){insideNative=true;}~NativeScope(){insideNative=old;}};
struct Record{
 UINT64 id=0;UINT sourceIndex=0,actualIndex=0,sync=0,flags=0;DWORD producerThread=0;
 bool usePresent1=false,paced=false;
 double submitted=0,ready=0,copied=0,deadline=0,presentEnter=0,presentReturn=0,period=0;
 HRESULT result=E_PENDING;
};
struct State{
 using PresentFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
 using Present1Fn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*);
 using GetBufferFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,REFIID,void**);
 using GetIndexFn=UINT(STDMETHODCALLTYPE*)(IDXGISwapChain3*);
 ComPtr<IDXGISwapChain4> chain;ComPtr<ID3D12CommandQueue> producer,display;
 ComPtr<ID3D12Fence> readyFence;
 std::unique_ptr<NativeFrameStage> images;
 void** oldVtable=nullptr;std::array<void*,41> table{};
 PresentFn present=nullptr;Present1Fn present1=nullptr;GetBufferFn getBuffer=nullptr;GetIndexFn getIndex=nullptr;
 std::recursive_mutex submissions;
 std::mutex mutex;std::condition_variable changed;
 std::deque<size_t> pending;std::vector<Record> records;std::vector<UINT64> leases;
 std::atomic<UINT> cursor{0};std::atomic<UINT64> lastSerial{0};
 std::atomic<HRESULT> fatal{S_OK};std::atomic<bool> stop{false};
 std::thread worker;HANDLE readyEvent=nullptr,copyEvent=nullptr,timer=nullptr,stopEvent=nullptr;
 size_t inFlight=0,peakInFlight=0;UINT64 actualPresents=0;unsigned timeouts=0;
 bool requested=false;double epoch=QpcMs(),nextDeadline=0;std::string path=EnvPath();
 void Init(IDXGISwapChain1* sc,ID3D12CommandQueue* q,ID3D12CommandQueue* output){
  if(FAILED(sc->QueryInterface(IID_PPV_ARGS(&chain)))||chain.Get()!=sc)throw std::runtime_error("native identity unsupported");
  producer=q;display=output;requested=EnvFlag("XEFG_LAB_GATE");
  ComPtr<ID3D12Device> d;if(FAILED(q->GetDevice(IID_PPV_ARGS(&d))))throw std::runtime_error("device query");
  if(FAILED(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&readyFence))))throw std::runtime_error("ready fence");
  DXGI_SWAP_CHAIN_DESC1 desc{};if(FAILED(sc->GetDesc1(&desc))||desc.BufferCount<2||desc.BufferCount>8)throw std::runtime_error("native descriptor");
  std::vector<ComPtr<ID3D12Resource>> targets(desc.BufferCount);
  for(UINT i=0;i<desc.BufferCount;++i)if(FAILED(chain->GetBuffer(i,IID_PPV_ARGS(&targets[i]))))throw std::runtime_error("native buffer");
  images=std::make_unique<NativeFrameStage>();if(FAILED(images->Initialize(d.Get(),output,targets)))throw std::runtime_error("image staging");
  leases.resize(desc.BufferCount);cursor=chain->GetCurrentBackBufferIndex();records.reserve(200000);
  readyEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);copyEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
  timer=CreateWaitableTimerExW(nullptr,nullptr,2,TIMER_ALL_ACCESS);if(!timer)timer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
  if(!readyEvent||!copyEvent||!stopEvent||!timer)throw std::runtime_error("wait handles");
  oldVtable=*reinterpret_cast<void***>(chain.Get());std::copy_n(oldVtable,table.size(),table.begin());
  present=reinterpret_cast<PresentFn>(oldVtable[8]);present1=reinterpret_cast<Present1Fn>(oldVtable[22]);
  getBuffer=reinterpret_cast<GetBufferFn>(oldVtable[9]);getIndex=reinterpret_cast<GetIndexFn>(oldVtable[36]);
  worker=std::thread([this]{Run();});
 }
 HRESULT Protect(UINT i){if(i>=leases.size())return E_INVALIDARG;UINT64 id=leases[i];if(id&&images->CopyFence()->GetCompletedValue()<id)return producer->Wait(images->CopyFence(),id);return S_OK;}
 HRESULT Submit(UINT sync,UINT flags,bool use1){
  std::lock_guard<std::recursive_mutex> serial(submissions);
  if(FAILED(fatal.load()))return fatal.load();
  if(sync!=0||(flags&~DXGI_PRESENT_ALLOW_TEARING))return DXGI_ERROR_INVALID_CALL;
  std::unique_lock<std::mutex> lock(mutex);
  if(!changed.wait_for(lock,std::chrono::milliseconds(1500),[&]{return inFlight<leases.size()||FAILED(fatal.load())||stop.load();}))return Fail(HRESULT_FROM_WIN32(WAIT_TIMEOUT));
  if(stop||FAILED(fatal.load()))return FAILED(fatal.load())?fatal.load():E_ABORT;
  if(records.size()>=199990)return Fail(E_OUTOFMEMORY);
  Record r;r.id=++lastSerial;r.sourceIndex=cursor.load();r.sync=sync;r.flags=flags;r.usePresent1=use1;r.producerThread=GetCurrentThreadId();
  r.period=sourcePeriod.load();r.paced=requested&&active.load()&&r.period>0;r.submitted=QpcMs()-epoch;
  HRESULT hr=producer->Signal(readyFence.Get(),r.id);if(FAILED(hr))return Fail(hr);
  const size_t record=records.size();records.push_back(r);pending.push_back(record);++inFlight;peakInFlight=(std::max)(peakInFlight,inFlight);
  leases[r.sourceIndex]=r.id;cursor=(r.sourceIndex+1)%static_cast<UINT>(leases.size());
  // This dependency is only for a reused immutable output image. It does not
  // wait for that image's later display deadline after its raw copy has retired.
  hr=Protect(cursor.load());if(FAILED(hr))return Fail(hr);
  lock.unlock();changed.notify_all();return S_OK; // bounded queue acceptance, NOT a scanout completion claim
 }
 HRESULT Fail(HRESULT hr){fatal=hr;changed.notify_all();return hr;}
 bool WaitFence(ID3D12Fence* f,UINT64 id,HANDLE event){
  if(f->GetCompletedValue()>=id)return true;
  if(FAILED(f->SetEventOnCompletion(id,event)))return false;
  const DWORD status=WaitForSingleObject(event,1500);
  if(status!=WAIT_OBJECT_0){++timeouts;return false;}return f->GetCompletedValue()>=id;
 }
 void SleepUntil(double due){
  double left=due-QpcMs();if(left<=0)return;
  LARGE_INTEGER t;t.QuadPart=-static_cast<LONGLONG>(std::ceil(left*10000.0));
  if(SetWaitableTimer(timer,&t,0,nullptr,nullptr,FALSE)){HANDLE hs[]={timer,stopEvent};WaitForMultipleObjects(2,hs,FALSE,100);}
 }
 void Run()noexcept{
  try{for(;;){size_t i;Record r;
   {std::unique_lock<std::mutex> lock(mutex);changed.wait(lock,[&]{return stop||!pending.empty();});if(pending.empty()){if(stop)break;else continue;}i=pending.front();pending.pop_front();r=records[i];}
   HRESULT hr=S_OK;
   if(!WaitFence(readyFence.Get(),r.id,readyEvent))hr=HRESULT_FROM_WIN32(WAIT_TIMEOUT);
   r.ready=QpcMs()-epoch;r.actualIndex=getIndex(chain.Get());
   if(SUCCEEDED(hr)&&r.actualIndex!=r.sourceIndex)hr=DXGI_ERROR_INVALID_CALL;
   if(SUCCEEDED(hr))hr=images->Prepare(r.sourceIndex);
   if(SUCCEEDED(hr))hr=display->Wait(readyFence.Get(),r.id);
   if(SUCCEEDED(hr))hr=images->Copy(r.sourceIndex,r.id);
   if(SUCCEEDED(hr)&&!WaitFence(images->CopyFence(),r.id,copyEvent))hr=HRESULT_FROM_WIN32(WAIT_TIMEOUT);
   r.copied=QpcMs()-epoch;double now=QpcMs();
   if(SUCCEEDED(hr)&&r.paced&&!stop&&active){
    const double step=r.period*0.5;
    if(nextDeadline<=0)nextDeadline=now+step;
    else nextDeadline+=step;
    if(now>nextDeadline+step||nextDeadline>now+r.period)nextDeadline=now;
    r.deadline=nextDeadline-epoch;SleepUntil(nextDeadline);
   }else{nextDeadline=0;r.deadline=now-epoch;}
   r.presentEnter=QpcMs()-epoch;
   if(SUCCEEDED(hr)){
    NativeScope scope;const DXGI_PRESENT_PARAMETERS empty{};
    hr=r.usePresent1?present1(chain.Get(),r.sync,r.flags,&empty):present(chain.Get(),r.sync,r.flags);
    ++actualPresents;
   }
   r.presentReturn=QpcMs()-epoch;r.result=hr;
   {std::lock_guard<std::mutex> lock(mutex);records[i]=r;--inFlight;}
   if(FAILED(hr)){
    Fail(hr);images->CopyFence()->Signal(lastSerial.load());
    std::ofstream f(path+"\\device-removed.txt");ComPtr<ID3D12Device> d;producer->GetDevice(IID_PPV_ARGS(&d));
    f<<"output="<<std::hex<<hr<<" device="<<(d?d->GetDeviceRemovedReason():E_POINTER)<<"\n";
    break;
   }
   changed.notify_all();
  }}catch(...){Fail(E_UNEXPECTED);if(images)images->CopyFence()->Signal(lastSerial.load());}
 }
 HRESULT Drain(){std::unique_lock<std::mutex> lock(mutex);if(!changed.wait_for(lock,std::chrono::milliseconds(2000),[&]{return inFlight==0||FAILED(fatal.load());}))return Fail(HRESULT_FROM_WIN32(WAIT_TIMEOUT));return fatal.load();}
 void Shutdown(){
  active=false;Drain();stop=true;SetEvent(stopEvent);changed.notify_all();
  if(worker.joinable())worker.join();if(images&&FAILED(images->Drain()))Fail(E_FAIL);
  if(chain&&oldVtable)*reinterpret_cast<void***>(chain.Get())=oldVtable;
  std::ofstream f(path+"\\native-present.csv");f<<"id,producer_thread,source_index,actual_index,submitted_ms,source_ready_ms,copy_done_ms,deadline_ms,present_enter_ms,present_return_ms,source_period_ms,paced,result\n"<<std::fixed<<std::setprecision(5);
  for(const auto&r:records)f<<r.id<<','<<r.producerThread<<','<<r.sourceIndex<<','<<r.actualIndex<<','<<r.submitted<<','<<r.ready<<','<<r.copied<<','<<r.deadline<<','<<r.presentEnter<<','<<r.presentReturn<<','<<r.period<<','<<r.paced<<','<<static_cast<long>(r.result)<<'\n';
  std::ofstream m(path+"\\native-gate-meta.json");m<<"{\"kind\":\"isolated_complete_output_fifo\",\"requested_gate\":"<<(requested?"true":"false")<<",\"accepted\":"<<records.size()<<",\"actual_present_calls\":"<<actualPresents<<",\"peak_pending\":"<<peakInFlight<<",\"slots\":"<<leases.size()<<",\"fatal_hresult\":"<<static_cast<long>(fatal.load())<<",\"timeouts\":"<<timeouts<<",\"native_frame_labels_may_be_unavailable\":true,\"game_modified\":false,\"resize_supported\":false}\n";
 }
 ~State(){if(worker.joinable())Shutdown();for(auto h:{readyEvent,copyEvent,timer,stopEvent})if(h)CloseHandle(h);}
};
inline std::unique_ptr<State> state;
inline HRESULT STDMETHODCALLTYPE OnPresent(IDXGISwapChain* sc,UINT i,UINT f){State*s=state.get();if(!s)return E_UNEXPECTED;if(insideNative)return s->present(sc,i,f);if(f&DXGI_PRESENT_TEST){auto hr=s->Drain();return FAILED(hr)?hr:s->present(sc,i,f);}return s->Submit(i,f,false);}
inline HRESULT STDMETHODCALLTYPE OnPresent1(IDXGISwapChain1* sc,UINT i,UINT f,const DXGI_PRESENT_PARAMETERS* p){State*s=state.get();if(!s)return E_UNEXPECTED;if(insideNative)return s->present1(sc,i,f,p);if(p&&(p->DirtyRectsCount||p->pScrollOffset||p->pScrollRect))return DXGI_ERROR_INVALID_CALL;if(f&DXGI_PRESENT_TEST){auto hr=s->Drain();return FAILED(hr)?hr:s->present1(sc,i,f,p);}return s->Submit(i,f,true);}
inline HRESULT STDMETHODCALLTYPE OnGetBuffer(IDXGISwapChain*sc,UINT i,REFIID iid,void**p){State*s=state.get();if(!s)return E_UNEXPECTED;return insideNative?s->getBuffer(sc,i,iid,p):s->images->GetBuffer(i,iid,p);}
inline UINT STDMETHODCALLTYPE OnGetIndex(IDXGISwapChain3*sc){State*s=state.get();if(!s)return 0;if(insideNative)return s->getIndex(sc);std::lock_guard<std::recursive_mutex> lock(s->submissions);UINT i=s->cursor.load();if(FAILED(s->Protect(i)))s->Fail(E_FAIL);return i;}
inline void Finish(){if(state){state->Shutdown();state.reset();}}
class FactoryTap{
 using CreateFn=HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*,IUnknown*,HWND,const DXGI_SWAP_CHAIN_DESC1*,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,IDXGIOutput*,IDXGISwapChain1**);
 ComPtr<IDXGIFactory7> factory;void**old=nullptr;std::array<void*,32> table{};CreateFn original=nullptr;inline static FactoryTap*current=nullptr;
 static HRESULT STDMETHODCALLTYPE Create(IDXGIFactory2*f,IUnknown*d,HWND w,const DXGI_SWAP_CHAIN_DESC1*sd,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*fd,IDXGIOutput*o,IDXGISwapChain1**result){
  FactoryTap*t=current;ComPtr<ID3D12CommandQueue>q,display;ComPtr<ID3D12Device>dev;
  if(!d||FAILED(d->QueryInterface(IID_PPV_ARGS(&q)))||state)return t->original(f,d,w,sd,fd,o,result);
  HRESULT hr=q->GetDevice(IID_PPV_ARGS(&dev));if(FAILED(hr))return hr;auto desc=q->GetDesc();
  hr=dev->CreateCommandQueue(&desc,IID_PPV_ARGS(&display));if(FAILED(hr))return hr;
  hr=t->original(f,display.Get(),w,sd,fd,o,result);if(FAILED(hr))return hr;
  try{auto next=std::make_unique<State>();next->Init(*result,q.Get(),display.Get());
   next->table[8]=reinterpret_cast<void*>(&OnPresent);next->table[22]=reinterpret_cast<void*>(&OnPresent1);next->table[9]=reinterpret_cast<void*>(&OnGetBuffer);next->table[36]=reinterpret_cast<void*>(&OnGetIndex);
   state=std::move(next);*reinterpret_cast<void***>(state->chain.Get())=state->table.data();return hr;
  }catch(...){if(result&&*result){(*result)->Release();*result=nullptr;}return E_FAIL;}
 }
public:
 FactoryTap(IDXGIFactory*f,ID3D12CommandQueue*){if(current||FAILED(f->QueryInterface(IID_PPV_ARGS(&factory)))||factory.Get()!=f)throw std::runtime_error("factory identity");old=*reinterpret_cast<void***>(f);std::copy_n(old,table.size(),table.begin());original=reinterpret_cast<CreateFn>(old[15]);table[15]=reinterpret_cast<void*>(&Create);current=this;*reinterpret_cast<void***>(f)=table.data();}
 ~FactoryTap(){if(factory&&old)*reinterpret_cast<void***>(factory.Get())=old;current=nullptr;}
};
}
