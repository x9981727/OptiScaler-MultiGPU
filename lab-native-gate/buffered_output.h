#pragma once
// Experimental output staging: exact native-output copies, ordered bounded FIFO.
// Intentionally not production: resize/fullscreen transitions are refused by hooks.
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <stdexcept>
namespace BufferedOutputLab {
using Microsoft::WRL::ComPtr;
using Clock=std::chrono::steady_clock;
inline bool Enabled(){char b[8]{};return GetEnvironmentVariableA("XEFG_BUFFERED_OUTPUT",b,8)>0&&b[0]=='1';}
inline void Check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Buffered output D3D12 failure");}
inline void Barrier(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
 D3D12_RESOURCE_BARRIER t{};t.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;t.Transition.pResource=r;
 t.Transition.StateBefore=a;t.Transition.StateAfter=b;t.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;l->ResourceBarrier(1,&t);
}
inline thread_local bool actual=false;
struct Actual {bool old=actual;Actual(){actual=true;}~Actual(){actual=old;}};
struct State {
 static constexpr size_t Slots=8;
 using PresentFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
 using Present1Fn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*);
 using BufferFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,REFIID,void**);
 using IndexFn=UINT(STDMETHODCALLTYPE*)(IDXGISwapChain3*);
 using CountFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT*);
 using ResizeFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT);
 using Resize1Fn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*,UINT,UINT,UINT,DXGI_FORMAT,UINT,const UINT*,IUnknown*const*);
 ComPtr<IDXGISwapChain4> chain;
 ComPtr<ID3D12Device> device;
 ComPtr<ID3D12CommandQueue> producer,output;
 ComPtr<ID3D12Fence> captured,shown;
 ComPtr<ID3D12CommandAllocator> outputAllocator;
 ComPtr<ID3D12GraphicsCommandList> outputList;
 std::array<ComPtr<ID3D12Resource>,Slots> snapshots;
 std::array<ComPtr<ID3D12CommandAllocator>,Slots> allocators;
 std::array<ComPtr<ID3D12GraphicsCommandList>,Slots> lists;
 std::vector<ComPtr<ID3D12Resource>> virtualBuffers;
 std::vector<ComPtr<ID3D12Resource>> realBuffers;
 std::array<void*,41> table{};void** oldTable=nullptr;
 HANDLE completionEvent=nullptr,timer=nullptr;
 struct Job{UINT64 id;size_t slot;UINT sync,flags;};
 std::deque<Job> fifo;
 std::mutex mutex;
 std::condition_variable cv;
 std::thread worker;
 std::atomic<bool> stop{false},failed{false};
 std::atomic<HRESULT> failure{S_OK};
 std::atomic<UINT64> submitted{0},completed{0},maxPending{0};
 std::atomic<UINT> cursor{0};
 bool paced=false;
 double intervalUs=7462.686567;
 double blockedMs=0,copyMs=0,paceMs=0;
 template<class T>T Original(size_t i){return reinterpret_cast<T>(oldTable[i]);}
 void Fail(HRESULT hr){failure=hr;failed=true;cv.notify_all();}
 void Init(IDXGISwapChain1* sc,ID3D12CommandQueue* source,ID3D12CommandQueue* screen){
  producer=source;output=screen;
  Check(sc->QueryInterface(IID_PPV_ARGS(&chain)));
  if(chain.Get()!=sc)throw std::runtime_error("Native interface identity differs");
  if(source==screen)throw std::runtime_error("Output queue must be independent");
  oldTable=*reinterpret_cast<void***>(chain.Get());std::copy_n(oldTable,41,table.begin());
  Check(source->GetDevice(IID_PPV_ARGS(&device)));
  DXGI_SWAP_CHAIN_DESC1 desc{};Check(chain->GetDesc1(&desc));
  if(desc.BufferCount<2||desc.BufferCount>8)throw std::runtime_error("Unsupported native buffer count");
  D3D12_RESOURCE_DESC texture{};
  for(UINT i=0;i<desc.BufferCount;++i){ComPtr<ID3D12Resource>b;Check(chain->GetBuffer(i,IID_PPV_ARGS(&b)));texture=b->GetDesc();realBuffers.push_back(b);}
  D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=1;heap.VisibleNodeMask=1;
  for(UINT i=0;i<desc.BufferCount;++i){ComPtr<ID3D12Resource>b;Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&b)));virtualBuffers.push_back(b);}
  texture.Flags=D3D12_RESOURCE_FLAG_NONE;
  for(size_t i=0;i<Slots;++i){
   Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&snapshots[i])));
   Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocators[i])));
   Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocators[i].Get(),nullptr,IID_PPV_ARGS(&lists[i])));Check(lists[i]->Close());
  }
  Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&outputAllocator)));
  Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,outputAllocator.Get(),nullptr,IID_PPV_ARGS(&outputList)));Check(outputList->Close());
  Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&captured)));
  Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&shown)));
  completionEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
  if(!timer)timer=CreateWaitableTimerW(nullptr,FALSE,nullptr);
  if(!completionEvent||!timer)throw std::runtime_error("Output timing handles unavailable");
  char b[64]{};paced=GetEnvironmentVariableA("XEFG_NATIVE_GATE",b,64)>0&&b[0]=='1';
  if(GetEnvironmentVariableA("XEFG_GATE_INTERVAL_US",b,64)){double x=std::atof(b);if(x>=1000&&x<=50000)intervalUs=x;}
  std::fprintf(stderr,"BUFFERED_OUTPUT_SETUP paced=%d intervalUs=%.6f width=%u height=%u nativeBuffers=%u slots=%zu distinctQueue=1\n",paced,intervalUs,desc.Width,desc.Height,desc.BufferCount,Slots);std::fflush(stderr);
  worker=std::thread([this]{Work();});
 }
 HRESULT Capture(UINT sync,UINT flags){
  if(flags&DXGI_PRESENT_TEST){Actual scope;return Original<PresentFn>(8)(chain.Get(),sync,flags);}
  if(sync!=0||(flags&DXGI_PRESENT_DO_NOT_WAIT))return DXGI_ERROR_INVALID_CALL;
  std::unique_lock l(mutex);const auto t0=Clock::now();
  if(!cv.wait_for(l,std::chrono::seconds(3),[&]{return submitted-completed<Slots||stop||failed;})){Fail(DXGI_ERROR_DEVICE_HUNG);return failure;}
  blockedMs+=std::chrono::duration<double,std::milli>(Clock::now()-t0).count();
  if(failed||stop)return failed?failure.load():E_ABORT;
  const auto id=submitted.load()+1;const auto slot=static_cast<size_t>((id-1)%Slots);
  try{
   Check(allocators[slot]->Reset());Check(lists[slot]->Reset(allocators[slot].Get(),nullptr));auto list=lists[slot].Get();
   auto src=virtualBuffers[cursor.load()].Get();auto dst=snapshots[slot].Get();
   Barrier(list,src,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
   Barrier(list,dst,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
   list->CopyResource(dst,src);
   Barrier(list,src,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
   Barrier(list,dst,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
   Check(list->Close());ID3D12CommandList* commands[]={list};producer->ExecuteCommandLists(1,commands);Check(producer->Signal(captured.Get(),id));
   submitted=id;cursor=(cursor.load()+1)%static_cast<UINT>(virtualBuffers.size());
   fifo.push_back({id,slot,sync,flags});maxPending=std::max(maxPending.load(),id-completed.load());
   l.unlock();cv.notify_one();return S_OK;
  }catch(...){Fail(E_FAIL);return E_FAIL;}
 }
 void Work() noexcept{
  auto deadline=Clock::time_point{};
  try{for(;;){
   Job job;
   {std::unique_lock l(mutex);cv.wait(l,[&]{return stop||!fifo.empty();});if(fifo.empty()){if(stop)break;continue;}job=fifo.front();fifo.pop_front();}
   if(failed){completed=job.id;cv.notify_all();continue;}
   const auto begin=Clock::now();
   Check(outputAllocator->Reset());Check(outputList->Reset(outputAllocator.Get(),nullptr));
   const UINT index=Original<IndexFn>(36)(chain.Get());
   if(index>=realBuffers.size())throw std::runtime_error("Invalid native index");
   auto src=snapshots[job.slot].Get();auto dst=realBuffers[index].Get();
   Check(output->Wait(captured.Get(),job.id));
   Barrier(outputList.Get(),src,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
   Barrier(outputList.Get(),dst,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
   outputList->CopyResource(dst,src);
   Barrier(outputList.Get(),src,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
   Barrier(outputList.Get(),dst,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
   Check(outputList->Close());ID3D12CommandList* commands[]={outputList.Get()};output->ExecuteCommandLists(1,commands);
   Check(output->Signal(shown.Get(),job.id));Check(shown->SetEventOnCompletion(job.id,completionEvent));
   if(WaitForSingleObject(completionEvent,3000)!=WAIT_OBJECT_0)throw std::runtime_error("Output copy timeout");
   if(shown->GetCompletedValue()==UINT64_MAX)throw std::runtime_error("Device removed");
   copyMs+=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
   auto now=Clock::now();
   if(deadline.time_since_epoch().count()==0||!paced||stop)deadline=now;
   else deadline=std::max(deadline+std::chrono::nanoseconds(static_cast<long long>(intervalUs*1000)),now);
   const auto delay=deadline-now;
   if(delay>std::chrono::microseconds(200)){
    LARGE_INTEGER due{};due.QuadPart=-std::chrono::duration_cast<std::chrono::nanoseconds>(delay-std::chrono::microseconds(150)).count()/100;
    if(SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE))WaitForSingleObject(timer,100);
   }
   while(Clock::now()<deadline)YieldProcessor();
   paceMs+=std::chrono::duration<double,std::milli>(Clock::now()-now).count();
   {Actual scope;Check(Original<PresentFn>(8)(chain.Get(),job.sync,job.flags));}
   completed=job.id;cv.notify_all();
  }}catch(...){Fail(E_FAIL);}
 }
 bool Drain(ID3D12CommandQueue* q){
  if(!q||!device)return true;
  ComPtr<ID3D12Fence> f;
  if(FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f))))return false;
  HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!e)return false;
  HRESULT h=q->Signal(f.Get(),1);if(SUCCEEDED(h))h=f->SetEventOnCompletion(1,e);
  bool ok=SUCCEEDED(h)&&WaitForSingleObject(e,3000)==WAIT_OBJECT_0;CloseHandle(e);return ok;
 }
 bool Shutdown(){
  stop=true;cv.notify_all();if(worker.joinable())worker.join();
  const bool safe=Drain(producer.Get())&&Drain(output.Get());
  if(!safe)Fail(DXGI_ERROR_DEVICE_HUNG);
  if(chain&&oldTable)*reinterpret_cast<void***>(chain.Get())=oldTable;
  std::fprintf(stderr,"BUFFERED_OUTPUT_SUMMARY submitted=%llu completed=%llu maxPending=%llu producerBlockedMs=%.3f copyMs=%.3f paceMs=%.3f failed=%d drained=%d hr=0x%08X\n",submitted.load(),completed.load(),maxPending.load(),blockedMs,copyMs,paceMs,int(failed.load()),int(safe),unsigned(failure.load()));std::fflush(stderr);
  return safe;
 }
 ~State(){if(worker.joinable())Shutdown();if(completionEvent)CloseHandle(completionEvent);if(timer)CloseHandle(timer);}
};
inline std::unique_ptr<State> state;
inline HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* s,UINT i,UINT f){if(actual)return state->Original<State::PresentFn>(8)(s,i,f);return state->Capture(i,f);}
inline HRESULT STDMETHODCALLTYPE Present1(IDXGISwapChain1* s,UINT i,UINT f,const DXGI_PRESENT_PARAMETERS* p){
 if(actual)return state->Original<State::Present1Fn>(22)(s,i,f,p);
 if(p&&(p->DirtyRectsCount||p->pScrollRect||p->pScrollOffset))return DXGI_ERROR_INVALID_CALL;
 return state->Capture(i,f);
}
inline HRESULT STDMETHODCALLTYPE GetBuffer(IDXGISwapChain* s,UINT i,REFIID riid,void** out){
 if(actual)return state->Original<State::BufferFn>(9)(s,i,riid,out);
 if(!out)return E_POINTER;
 *out=nullptr;if(i>=state->virtualBuffers.size())return DXGI_ERROR_INVALID_CALL;
 return state->virtualBuffers[i]->QueryInterface(riid,out);
}
inline UINT STDMETHODCALLTYPE GetIndex(IDXGISwapChain3* s){return actual?state->Original<State::IndexFn>(36)(s):state->cursor.load();}
inline HRESULT STDMETHODCALLTYPE GetCount(IDXGISwapChain* s,UINT* p){if(actual)return state->Original<State::CountFn>(17)(s,p);if(!p)return E_POINTER;*p=static_cast<UINT>(state->submitted.load());return S_OK;}
inline HRESULT STDMETHODCALLTYPE Resize(IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT){return DXGI_ERROR_INVALID_CALL;}
inline HRESULT STDMETHODCALLTYPE Resize1(IDXGISwapChain3*,UINT,UINT,UINT,DXGI_FORMAT,UINT,const UINT*,IUnknown*const*){return DXGI_ERROR_INVALID_CALL;}
inline void Initialize(IDXGISwapChain1* sc,ID3D12CommandQueue* producer,ID3D12CommandQueue* output){
 if(state)throw std::runtime_error("Only one output stage allowed");
 auto next=std::make_unique<State>();next->Init(sc,producer,output);
 next->table[8]=reinterpret_cast<void*>(&Present);next->table[22]=reinterpret_cast<void*>(&Present1);
 next->table[9]=reinterpret_cast<void*>(&GetBuffer);next->table[36]=reinterpret_cast<void*>(&GetIndex);
 next->table[17]=reinterpret_cast<void*>(&GetCount);next->table[13]=reinterpret_cast<void*>(&Resize);next->table[39]=reinterpret_cast<void*>(&Resize1);
 state=std::move(next);*reinterpret_cast<void***>(state->chain.Get())=state->table.data();
}
inline void Finish(){if(state){if(state->Shutdown())state.reset();else (void)state.release();}}
}
