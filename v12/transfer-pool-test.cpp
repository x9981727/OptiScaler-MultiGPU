#include "XeFGTransferPool.h"
#include <dxgi1_6.h>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <cstring>

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
static void require(bool ok, const char* step)
{
    if (!ok) { std::cerr << "FAIL: " << step << '\n'; std::exit(1); }
}
static void check(HRESULT hr, const char* step)
{
    if (FAILED(hr)) { std::cerr << "FAIL: " << step << ": " << std::hex << hr << '\n'; std::exit(1); }
}
static ComPtr<ID3D12Resource> buffer(ID3D12Device* device, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap {}; heap.Type=heapType; heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc {}; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width=256; desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;
    desc.SampleDesc.Count=1; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&resource)),"buffer");
    return resource;
}
int main()
{
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> warp; ComPtr<ID3D12Device> device;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"WARP adapter");
    check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
    D3D12_COMMAND_QUEUE_DESC desc {};
    ComPtr<ID3D12CommandQueue> queue, otherQueue;
    check(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)),"queue");
    check(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&otherQueue)),"other queue");
    MultiGPU::XeFGTransferPool inputs, backbuffers;
    ID3D12GraphicsCommandList* cmd=nullptr;
    check(inputs.Begin(queue.Get(),0,&cmd),"initialize input pool"); inputs.Abort(0);
    check(backbuffers.Begin(queue.Get(),0,&cmd),"initialize independent pool"); backbuffers.Abort(0);

    auto upload=buffer(device.Get(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto intermediate=buffer(device.Get(),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
    auto readback=buffer(device.Get(),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    void* mapped=nullptr; D3D12_RANGE noRead {0,0};
    check(upload->Map(0,&noRead,&mapped),"map upload");
    for(unsigned i=0;i<256;++i) static_cast<unsigned char*>(mapped)[i]=static_cast<unsigned char>(i^0x5A);
    upload->Unmap(0,nullptr);
    ComPtr<ID3D12Fence> gate;
    check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"gate");
    check(queue->Wait(gate.Get(),1),"hold GPU work behind gate");

    // CPU records both the import and the dependent SDK-style copy while the
    // GPU is blocked. Submit must return without waiting on this current frame.
    auto batch=std::async(std::launch::async,[&] {
        ID3D12GraphicsCommandList* list=nullptr;
        HRESULT hr=inputs.Begin(queue.Get(),0,&list);
        if(FAILED(hr)) return hr;
        list->CopyBufferRegion(intermediate.Get(),0,upload.Get(),0,256);
        D3D12_RESOURCE_BARRIER barrier {}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource=intermediate.Get();
        barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1,&barrier);
        list->CopyBufferRegion(readback.Get(),0,intermediate.Get(),0,256);
        return inputs.Submit(0);
    });
    require(batch.wait_for(2s)==std::future_status::ready,"record + submit does not wait for current GPU batch");
    check(batch.get(),"batched import and dependent copy submission");
    require(inputs.WaitForSlot(0,10)==HRESULT_FROM_WIN32(WAIT_TIMEOUT),"producer cannot overwrite a pending slot");

    // Distinct slots and the other pool remain usable while slot 0 is pending.
    for(UINT i=1;i<4;++i) { check(inputs.Begin(queue.Get(),i,&cmd),"next frame can be recorded"); check(inputs.Submit(i),"next frame can be submitted"); }
    check(backbuffers.Begin(queue.Get(),0,&cmd),"backbuffer pool is independent"); backbuffers.Abort(0);
    require(inputs.Begin(otherQueue.Get(),0,&cmd)==DXGI_ERROR_INVALID_CALL,"reject accidental queue rebinding");

    std::promise<void> attempting; auto started=attempting.get_future();
    auto reuse=std::async(std::launch::async,[&] {
        attempting.set_value(); ID3D12GraphicsCommandList* list=nullptr;
        auto hr=inputs.Begin(queue.Get(),4,&list);
        if(SUCCEEDED(hr)) inputs.Abort(4);
        return hr;
    });
    started.wait();
    require(reuse.wait_for(40ms)==std::future_status::timeout,"ring wrap waits before allocator reuse");
    check(gate->Signal(1),"release GPU gate");
    require(reuse.wait_for(2s)==std::future_status::ready,"reuse completes after old GPU work");
    check(reuse.get(),"reuse result");
    check(inputs.Drain(),"drain before resource release");
    D3D12_RANGE readRange {0,256}; check(readback->Map(0,&readRange,&mapped),"map readback");
    for(unsigned i=0;i<256;++i) require(static_cast<unsigned char*>(mapped)[i]==static_cast<unsigned char>(i^0x5A),"dependent copy sees imported data");
    readback->Unmap(0,&noRead);
    check(inputs.Begin(queue.Get(),2,&cmd),"begin after abort/reuse");
    require(inputs.Submit(3)==DXGI_ERROR_INVALID_CALL,"wrong-slot submit rejected");
    inputs.Abort(2);
    check(inputs.Begin(queue.Get(),2,&cmd),"aborted slot reusable"); check(inputs.Submit(2),"empty batch submit");
    require(inputs.Begin(nullptr,0,&cmd)==E_POINTER,"null queue rejected");
    check(inputs.Drain(),"final drain");
    std::cout << "PASS: XeFG combined batch, independent pool, producer/allocator reuse and GPU copy ordering\n";
}
