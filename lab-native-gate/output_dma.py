from pathlib import Path
import sys
p=Path(sys.argv[1])/'buffered_output.h';s=p.read_text(encoding='utf-8-sig')
def once(a,b):
 global s
 if s.count(a)!=1:raise RuntimeError('Output DMA anchor: '+a[:100])
 s=s.replace(a,b)
once(' ComPtr<ID3D12CommandQueue> producer,output;', ' ComPtr<ID3D12CommandQueue> producer,output,outputTransfer;\n ComPtr<ID3D12Fence> presented;\n std::vector<UINT64> bufferUse;\n bool copyEngine=false;')
once('  Check(source->GetDevice(IID_PPV_ARGS(&device)));', '''  Check(source->GetDevice(IID_PPV_ARGS(&device)));
  char copyValue[8]{};copyEngine=GetEnvironmentVariableA("XEFG_OUTPUT_COPY_QUEUE",copyValue,8)>0&&copyValue[0]=='1';
  if(copyEngine){
   D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_COPY;
   Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&outputTransfer)));
   outputTransfer->SetName(L"XeFG lab isolated output DMA");
  } else outputTransfer=output;
  Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&presented)));
  std::fprintf(stderr,"OUTPUT_DMA_CONTROL copyEngine=%d\\n",int(copyEngine));''')
once('  D3D12_RESOURCE_DESC texture{};', '  D3D12_RESOURCE_DESC texture{};bufferUse.resize(desc.BufferCount,0);')
once('  Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&outputAllocator)));','  const auto outputType=copyEngine?D3D12_COMMAND_LIST_TYPE_COPY:D3D12_COMMAND_LIST_TYPE_DIRECT;\n  Check(device->CreateCommandAllocator(outputType,IID_PPV_ARGS(&outputAllocator)));')
once('  Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,outputAllocator.Get(),nullptr,IID_PPV_ARGS(&outputList)));Check(outputList->Close());','  Check(device->CreateCommandList(0,outputType,outputAllocator.Get(),nullptr,IID_PPV_ARGS(&outputList)));Check(outputList->Close());')
once('   Check(output->Wait(captured.Get(),job.id));','   if(bufferUse[index])Check(outputTransfer->Wait(presented.Get(),bufferUse[index]));\n   Check(outputTransfer->Wait(captured.Get(),job.id));')
once('   Check(outputList->Close());ID3D12CommandList* commands[]={outputList.Get()};output->ExecuteCommandLists(1,commands);','   Check(outputList->Close());ID3D12CommandList* commands[]={outputList.Get()};outputTransfer->ExecuteCommandLists(1,commands);')
once('   Check(output->Signal(shown.Get(),job.id));Check(shown->SetEventOnCompletion(job.id,completionEvent));','   Check(outputTransfer->Signal(shown.Get(),job.id));Check(shown->SetEventOnCompletion(job.id,completionEvent));')
once('   {Actual scope;Check(Original<PresentFn>(8)(chain.Get(),job.sync,job.flags));}', '''   if(copyEngine)Check(output->Wait(shown.Get(),job.id));
   {Actual scope;Check(Original<PresentFn>(8)(chain.Get(),job.sync,job.flags));}
   Check(output->Signal(presented.Get(),job.id));bufferUse[index]=job.id;''')
once('  const bool safe=Drain(producer.Get())&&Drain(output.Get());','  const bool safe=Drain(producer.Get())&&Drain(outputTransfer.Get())&&Drain(output.Get());')
p.write_text(s,encoding='utf-8');print('Output DMA control prepared with explicit present/read/reuse dependencies.')
