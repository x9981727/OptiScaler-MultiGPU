from pathlib import Path
import sys
root=Path(sys.argv[1]);p=root/'native_gate.cpp';s=p.read_text(encoding='utf-8-sig')
def replace(s,a,b):
 if s.count(a)!=1:raise RuntimeError('Queue isolation anchor: '+a[:100])
 return s.replace(a,b)
s=replace(s,'        h=device->CreateCommandQueue(&description,IID_PPV_ARGS(&screen));', '''        for(int priority: {0,100,10000}){
            D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY caps{};caps.CommandListType=D3D12_COMMAND_LIST_TYPE_DIRECT;caps.Priority=priority;
            const auto c=device->CheckFeatureSupport(D3D12_FEATURE_COMMAND_QUEUE_PRIORITY,&caps,sizeof(caps));
            std::fprintf(stderr,"OUTPUT_QUEUE_CAP priority=%d supported=%d hr=0x%08X\\n",priority,int(caps.PriorityForTypeIsSupported),unsigned(c));
        }
        if(Gate::Yes("XEFG_OUTPUT_REALTIME"))description.Priority=D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
        if(Gate::Yes("XEFG_OUTPUT_CREATOR_ID")){
            ComPtr<ID3D12Device9> newer;
            h=device->QueryInterface(IID_PPV_ARGS(&newer));
            if(SUCCEEDED(h)){
                const GUID creator={0x572470f6,0xcbd4,0x496e,{0x95,0xd4,0xa3,0x9f,0xf2,0x04,0xf0,0xa7}};
                h=newer->CreateCommandQueue1(&description,creator,IID_PPV_ARGS(&screen));
            }
        }else h=device->CreateCommandQueue(&description,IID_PPV_ARGS(&screen));
        std::fprintf(stderr,"OUTPUT_QUEUE_CREATE creatorId=%d priority=%d hr=0x%08X\\n",int(Gate::Yes("XEFG_OUTPUT_CREATOR_ID")),description.Priority,unsigned(h));std::fflush(stderr);''')
p.write_text(s,encoding='utf-8')
hp=root/'buffered_output.h';h=hp.read_text(encoding='utf-8-sig')
h=replace(h,'#include <d3d12.h>','#include <d3d12.h>\n#include <d3d12sdklayers.h>')
h=replace(h,'inline void Check(HRESULT hr){if(FAILED(hr))throw std::runtime_error("Buffered output D3D12 failure");}', 'inline void Check(HRESULT hr){if(FAILED(hr)){char b[96];std::snprintf(b,sizeof(b),"Buffered output D3D12 HRESULT=0x%08X",unsigned(hr));throw std::runtime_error(b);}}')
helper='''
inline void DumpDeviceErrors(ID3D12Device* device){
 if(!device)return;
 std::fprintf(stderr,"OUTPUT_DEVICE_REMOVED_REASON hr=0x%08X\\n",unsigned(device->GetDeviceRemovedReason()));
 ComPtr<ID3D12InfoQueue> info;
 if(SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&info)))){
  auto count=info->GetNumStoredMessagesAllowedByRetrievalFilter();UINT printed=0;
  for(UINT64 i=0;i<count&&printed<16;++i){
   SIZE_T size=0;if(FAILED(info->GetMessage(i,nullptr,&size))||size>65536)continue;
   std::vector<unsigned char> buffer(size);auto m=reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
   if(SUCCEEDED(info->GetMessage(i,m,&size))&&m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){
    std::fprintf(stderr,"OUTPUT_D3D12_MESSAGE id=%d severity=%d %s\\n",int(m->ID),int(m->Severity),m->pDescription);++printed;
   }
  }
 }
 std::fflush(stderr);
}
'''
h=replace(h,'struct State {',helper+'\nstruct State {')
h=replace(h,'  }catch(...){Fail(E_FAIL);return E_FAIL;}','  }catch(const std::exception& e){std::fprintf(stderr,"OUTPUT_CAPTURE_FAILURE %s\\n",e.what());DumpDeviceErrors(device.Get());Fail(E_FAIL);return E_FAIL;}catch(...){Fail(E_FAIL);return E_FAIL;}')
h=replace(h,'  }}catch(...){Fail(E_FAIL);}','  }}catch(const std::exception& e){std::fprintf(stderr,"OUTPUT_WORKER_FAILURE %s\\n",e.what());DumpDeviceErrors(device.Get());Fail(E_FAIL);}catch(...){Fail(E_FAIL);}')
hp.write_text(h,encoding='utf-8')
f=root/'basic_sample.cpp';b=f.read_text(encoding='utf-8-sig')
b=replace(b,'void BasicSample::LoadDX12()\n{','''void BasicSample::LoadDX12()
{
    char debugOption[8]{};
    if(GetEnvironmentVariableA("XEFG_LAB_DEBUG",debugOption,8)>0&&debugOption[0]=='1'){
        ComPtr<ID3D12Debug> debug;
        const auto status=D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
        if(SUCCEEDED(status))debug->EnableDebugLayer();
        std::fprintf(stderr,"LAB_DEBUG_LAYER hr=0x%08X\\n",unsigned(status));std::fflush(stderr);
    }''')
f.write_text(b,encoding='utf-8');print('Opt-in own-queue creator ID and diagnostics prepared. No global OS settings changed.')
