from pathlib import Path
import sys
root=Path(sys.argv[1])/'samples/basic_sample_frame_generation'
p=root/'NativeGate.h';s=p.read_text(encoding='utf-8')
def once(a,b):
 global s
 if s.count(a)!=1:raise RuntimeError('diagnostic anchor: '+a[:100])
 s=s.replace(a,b)
once('#include <d3d12.h>','#include <d3d12.h>\n#include <d3d12sdklayers.h>')
setup='''inline void DebugSetup() {
    if(!EnvFlag("XEFG_LAB_D3D_DEBUG"))return;
    std::ofstream f(EnvPath()+"\\\\d3d-debug-setup.txt");
    ComPtr<ID3D12Debug> debug;
    HRESULT hr=D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    f<<"debug_interface="<<std::hex<<hr<<"\\n";
    if(SUCCEEDED(hr))debug->EnableDebugLayer();
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
    hr=D3D12GetDebugInterface(IID_PPV_ARGS(&dred));
    f<<"dred_settings="<<std::hex<<hr<<"\\n";
    if(SUCCEEDED(hr)) {
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }
}
'''
once('inline std::atomic<double> sourcePeriod{0.0};',setup+'inline std::atomic<double> sourcePeriod{0.0};')
diagnostic='''        if(FAILED(h)) {
            ComPtr<ID3D12Device> dev; queue->GetDevice(IID_PPV_ARGS(&dev));
            std::ofstream f(path+"\\\\device-removed.txt",std::ios::app);
            f<<"present="<<std::hex<<h<<" removed="<<(dev?dev->GetDeviceRemovedReason():E_POINTER)<<"\\n";
            ComPtr<ID3D12InfoQueue> info;
            if(dev && SUCCEEDED(dev.As(&info))) {
                const UINT64 n=info->GetNumStoredMessages();
                for(UINT64 k=n>160?n-160:0;k<n;++k) {
                    SIZE_T bytes=0; info->GetMessage(k,nullptr,&bytes);
                    std::vector<unsigned char> buffer(bytes);
                    auto* message=reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
                    if(SUCCEEDED(info->GetMessage(k,message,&bytes)))
                        f<<"message "<<std::dec<<message->ID<<" severity "<<message->Severity<<": "<<message->pDescription<<"\\n";
                }
            }
            ComPtr<ID3D12DeviceRemovedExtendedData> dred;
            if(dev && SUCCEEDED(dev.As(&dred))) {
                D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT b{};
                if(SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&b))) {
                    for(auto n=b.pHeadAutoBreadcrumbNode;n;n=n->pNext) {
                        f<<"breadcrumb count="<<n->BreadcrumbCount<<" last="<<(n->pLastBreadcrumbValue?*n->pLastBreadcrumbValue:0);
                        if(n->pCommandQueueDebugNameA)f<<" queue="<<n->pCommandQueueDebugNameA;
                        f<<"\\n";
                    }
                }
            }
        }
'''
once('        records[i].leave=QpcMs()-epoch;records[i].result=h;','        records[i].leave=QpcMs()-epoch;records[i].result=h;\n'+diagnostic)
p.write_text(s,encoding='utf-8')
p=root/'basic_sample.cpp';s=p.read_text(encoding='utf-8')
once('void BasicSample::OnInit()\n{','void BasicSample::OnInit()\n{\n    NativeGateLab::DebugSetup();')
p.write_text(s,encoding='utf-8')
# Update the manifest to reflect the actual compiled, instrumented source.
import json,hashlib
m=root/'NATIVE-GATE-MANIFEST.json';d=json.loads(m.read_text());d['diagnostics_available']=True
for n in ['NativeGate.h','basic_sample.cpp']:d[n+'_sha256']=hashlib.sha256((root/n).read_bytes()).hexdigest()
m.write_text(json.dumps(d,indent=2),encoding='utf-8')
