#include <framegen/CopyCommonTexture.h>
#include <framegen/XeFGTransferPool.h>
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <array>
#include <vector>
#include <cstring>
#include <iostream>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
static void Check(bool ok, const char* text) { if (!ok) { std::cerr << "FAIL: " << text << '\n'; std::exit(1); } }
static void Hr(HRESULT hr, const char* text) { if (FAILED(hr)) { std::cerr << std::hex << (UINT)hr << ' '; Check(false,text); } }
static void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER v {}; v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition.pResource=r; v.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    v.Transition.StateBefore=a; v.Transition.StateAfter=b; list->ResourceBarrier(1,&v);
}
int main()
{
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> warp; ComPtr<ID3D12Device> device;
    Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
    Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"WARP");
    Hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
    ComPtr<ID3D12InfoQueue> messages; device.As(&messages);
    if(messages) messages->ClearStoredMessages();
    auto queue=[&](D3D12_COMMAND_LIST_TYPE type) {
        D3D12_COMMAND_QUEUE_DESC q {};q.Type=type;ComPtr<ID3D12CommandQueue> result;
        Hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&result)),"queue"); return result;
    };
    auto render=queue(D3D12_COMMAND_LIST_TYPE_DIRECT), copy=queue(D3D12_COMMAND_LIST_TYPE_COPY), fg=queue(D3D12_COMMAND_LIST_TYPE_DIRECT);
    auto buffer=[&](D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, UINT64 bytes) {
        D3D12_HEAP_PROPERTIES hp {}; hp.Type=type;hp.CreationNodeMask=hp.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC d {};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;
        d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;Hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)),"buffer"); return r;
    };
    constexpr UINT width=67, height=13, frames=33, slots=3;
    D3D12_RESOURCE_DESC desc {};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width=width;desc.Height=height;desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R10G10B10A2_UNORM;
    auto texture=[&]() {
        D3D12_HEAP_PROPERTIES hp {}; hp.Type=D3D12_HEAP_TYPE_DEFAULT; hp.CreationNodeMask=hp.VisibleNodeMask=1;
        ComPtr<ID3D12Resource> r; Hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r)),"texture");return r;
    };
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};UINT64 bytes=0;
    device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
    // Each frame begins at a legal placed-footprint offset, including the final row padding.
    const UINT64 stride=(bytes+511)&~UINT64(511);
    auto upload=buffer(D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,stride*frames);
    auto readback=buffer(D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST,stride*frames);
    auto value=[](UINT frame,UINT x,UINT y) { return 0xd328019fu ^ (frame*0x1020304u) ^ (y*width+x)*0x31415927u; };
    void* map=nullptr;Hr(upload->Map(0,nullptr,&map),"map upload");std::memset(map,0,static_cast<size_t>(stride*frames));
    for(UINT n=0;n<frames;++n) for(UINT y=0;y<height;++y) for(UINT x=0;x<width;++x) {
        const UINT32 v=value(n,x,y);std::memcpy(static_cast<char*>(map)+stride*n+footprint.Footprint.RowPitch*y+x*4,&v,4);
    }
    upload->Unmap(0,nullptr);
    std::array<ComPtr<ID3D12Resource>,slots> transport,staging,proxy;
    for(UINT i=0;i<slots;++i) {transport[i]=buffer(D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON,stride);staging[i]=texture();proxy[i]=texture();}
    ComPtr<ID3D12Fence> gate, observed;Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"gate");
    Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&observed)),"observed");
    // A pending prior SDK queue is modelled by this gate. It must not prevent the COPY upload.
    Hr(fg->Wait(gate.Get(),1),"block prior FG work");
    MultiGPU::XeFGTransferPool producers,imports,consumers;
    Check(imports.QueueWaitForSlot(fg.Get(),0)==DXGI_ERROR_INVALID_CALL,"reject unsubmitted slot");
    Check(imports.QueueWaitForSlot(nullptr,0)==E_POINTER,"reject null consumer");
    UINT accepted=0,delivered=0;
    for(UINT n=0;n<frames;++n)
    {
        const UINT slot=n%slots;
        // Same slot ownership as production: both previous import and consumer retire.
        Hr(consumers.WaitForSlot(slot),"consumer slot reuse");Hr(imports.WaitForSlot(slot),"import slot reuse");
        ID3D12GraphicsCommandList* list=nullptr;Hr(producers.Begin(render.Get(),slot,&list),"producer begin");
        list->CopyBufferRegion(transport[slot].Get(),0,upload.Get(),stride*n,bytes);Hr(producers.Submit(slot),"producer submit");
        Hr(imports.Begin(copy.Get(),slot,&list),"COPY begin");
        Hr(producers.QueueWaitForSlot(copy.Get(),slot),"COPY waits producer only");
        Barrier(list,transport[slot].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(list,staging[slot].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src {},dst {};src.pResource=transport[slot].Get();
        src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprint;
        dst.pResource=staging[slot].Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        Barrier(list,transport[slot].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        Barrier(list,staging[slot].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        Hr(imports.Submit(slot),"COPY submit");++accepted;
        if(n==1) {
            Hr(imports.WaitForSlot(slot),"next source imports while prior FG blocked");
            Check(observed->GetCompletedValue()==0,"FG gate bypassed");
            Hr(gate->Signal(1),"release prior FG gate");
        }
        Hr(consumers.Begin(fg.Get(),slot,&list),"consumer begin");
        Hr(imports.QueueWaitForSlot(fg.Get(),slot),"consumer waits exact import");
        Hr(MultiGPU::CopyCommonTexture(list,staging[slot].Get(),proxy[slot].Get()),"production lossless local handoff");
        Barrier(list,proxy[slot].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        src={};dst={};src.pResource=proxy[slot].Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
        dst.PlacedFootprint.Offset=stride*n;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        Barrier(list,proxy[slot].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        Hr(consumers.Submit(slot),"consumer submit");++delivered;
        if(n==0) Hr(fg->Signal(observed.Get(),1),"observe blocked consumer");
    }
    Hr(producers.Drain(),"render drain");Hr(imports.Drain(),"COPY drain");Hr(consumers.Drain(),"consumer drain");
    Check(accepted==frames&&delivered==frames,"source frame dropped or duplicated");
    Hr(readback->Map(0,nullptr,&map),"readback map");
    for(UINT n=0;n<frames;++n) for(UINT y=0;y<height;++y) for(UINT x=0;x<width;++x) {
        UINT32 v=0;std::memcpy(&v,static_cast<char*>(map)+stride*n+footprint.Footprint.RowPitch*y+x*4,4);
        Check(v==value(n,x,y),"pixel/alpha/slot data changed");
    }
    readback->Unmap(0,nullptr);
    if(messages) for(UINT64 i=0;i<messages->GetNumStoredMessages();++i) {
        SIZE_T size=0;messages->GetMessage(i,nullptr,&size);std::vector<char> data(size);
        auto m=reinterpret_cast<D3D12_MESSAGE*>(data.data());Hr(messages->GetMessage(i,m,&size),"debug message");
        if(m->Severity==D3D12_MESSAGE_SEVERITY_ERROR||m->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION) {std::cerr<<m->pDescription<<'\n';Check(false,"D3D12 validation");}
    }
    std::cout<<"PASS: 33 full-resolution HDR-format frames bit-exact through 3 slots; COPY progressed while DIRECT blocked; no dropped sources; debugLayer="<<(debug!=nullptr)<<'\n';
}
