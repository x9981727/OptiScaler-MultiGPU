from pathlib import Path
import sys
p=Path(sys.argv[1])/'buffered_output.h';s=p.read_text(encoding='utf-8-sig')
def once(a,b):
 global s
 if s.count(a)!=1:raise RuntimeError('Single copy anchor: '+a[:100])
 s=s.replace(a,b)
once(' bool copyEngine=false;', ' bool copyEngine=false;\n bool directNativeInput=false;\n std::vector<UINT64> nativeUse;\n HANDLE nativeReusable=nullptr;\n double nativeReuseWaitMs=0;')
once('  D3D12_RESOURCE_DESC texture{};bufferUse.resize(desc.BufferCount,0);', '''  D3D12_RESOURCE_DESC texture{};bufferUse.resize(desc.BufferCount,0);nativeUse.resize(desc.BufferCount,0);
  char single[8]{};directNativeInput=GetEnvironmentVariableA("XEFG_SINGLE_OUTPUT_COPY",single,8)>0&&single[0]=='1';
  nativeReusable=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  if(!nativeReusable)throw std::runtime_error("Missing native ownership event");
  std::fprintf(stderr,"NATIVE_SINGLE_COPY enabled=%d nativeBuffers=%u\\n",int(directNativeInput),desc.BufferCount);''')
once('  for(size_t i=0;i<Slots;++i){','  for(size_t i=0;!directNativeInput&&i<Slots;++i){')
once('   Check(allocators[slot]->Reset());Check(lists[slot]->Reset(allocators[slot].Get(),nullptr));auto list=lists[slot].Get();', '''   if(directNativeInput){
    const auto input=cursor.load();
    Check(producer->Signal(captured.Get(),id));
    nativeUse[input]=id;submitted=id;
    fifo.push_back({id,input,sync,flags});maxPending=std::max(maxPending.load(),id-completed.load());
    l.unlock();cv.notify_one();
    const auto next=(input+1)%static_cast<UINT>(virtualBuffers.size());
    const auto reading=nativeUse[next];const auto t=Clock::now();
    // The SDK must not be given a reusable native index until this index's
    // previous consumer read has finished. This is an actual CPU ownership
    // check, not a Wait on only one of the SDK's potentially multiple queues.
    if(reading&&shown->GetCompletedValue()<reading){
     Check(shown->SetEventOnCompletion(reading,nativeReusable));
     if(WaitForSingleObject(nativeReusable,3000)!=WAIT_OBJECT_0)throw std::runtime_error("Native input reuse timeout");
    }
    if(shown->GetCompletedValue()==UINT64_MAX)throw std::runtime_error("Native input device lost");
    nativeReuseWaitMs+=std::chrono::duration<double,std::milli>(Clock::now()-t).count();
    if(failed)return failure;
    cursor=next;return S_OK;
   }
   Check(allocators[slot]->Reset());Check(lists[slot]->Reset(allocators[slot].Get(),nullptr));auto list=lists[slot].Get();''')
once('   auto src=snapshots[job.slot].Get();auto dst=realBuffers[index].Get();','   auto src=directNativeInput?virtualBuffers[job.slot].Get():snapshots[job.slot].Get();auto dst=realBuffers[index].Get();')
once('  return safe;', '  std::fprintf(stderr,"NATIVE_INPUT_REUSE_WAIT totalMs=%.3f\\n",nativeReuseWaitMs);std::fflush(stderr);\n  return safe;')
once('~State(){if(worker.joinable())Shutdown();if(completionEvent)CloseHandle(completionEvent);if(timer)CloseHandle(timer);}', '~State(){if(worker.joinable())Shutdown();if(completionEvent)CloseHandle(completionEvent);if(timer)CloseHandle(timer);if(nativeReusable)CloseHandle(nativeReusable);}')
p.write_text(s,encoding='utf-8');print('Single-copy output control prepared. Accepted native outputs remain ordered; no source drop.')
