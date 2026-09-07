#include "DeferredPresent.h"
#include "XeFGPresentPolicy.h"
#include "QueueIdle_Dx12.h"
#include <dxgi1_6.h>
#include <future>
#include <iostream>
#include <cstdlib>
#include <cstring>

using Microsoft::WRL::ComPtr;
static void Check(bool pass, const char* why)
{
    if (!pass) { std::cerr << "FAIL: " << why << '\n'; std::exit(1); }
}
static void Hr(HRESULT hr, const char* why) { Check(SUCCEEDED(hr), why); }

int main()
{
    using namespace std::chrono_literals;
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> adapter; ComPtr<ID3D12Device> device;
    Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP");
    Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
    D3D12_COMMAND_QUEUE_DESC q {}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> render, fg;
    Hr(device->CreateCommandQueue(&q, IID_PPV_ARGS(&render)), "render queue");
    Hr(device->CreateCommandQueue(&q, IID_PPV_ARGS(&fg)), "FG queue");
    ComPtr<ID3D12Fence> producer, gate;
    Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&producer)), "producer fence");
    Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate fence");

    auto buffer = [&](D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, UINT64 bytes) {
        D3D12_HEAP_PROPERTIES heap {}; heap.Type = type; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes; desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> out;
        Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                           IID_PPV_ARGS(&out)), "buffer");
        return out;
    };
    auto upload = buffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, 8);
    const UINT32 values[] = {0x11223344u, 0xaabbccddu};
    void* mapped = nullptr;
    Hr(upload->Map(0, nullptr, &mapped), "upload map");
    std::memcpy(mapped, values, sizeof(values)); upload->Unmap(0, nullptr);
    ComPtr<ID3D12Resource> primary[2] = {
        buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, 4),
        buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, 4)};
    auto output = buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, 4);
    ComPtr<ID3D12CommandAllocator> allocators[3];
    ComPtr<ID3D12GraphicsCommandList> lists[3];
    for (UINT i = 0; i < 3; ++i)
    {
        Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])), "allocator");
        Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr,
                                     IID_PPV_ARGS(&lists[i])), "list");
    }
    for (UINT i = 0; i < 2; ++i)
    {
        lists[i]->CopyBufferRegion(primary[i].Get(), 0, upload.Get(), i * 4, 4);
        Hr(lists[i]->Close(), "close producer");
    }
    lists[2]->CopyBufferRegion(output.Get(), 0, primary[0].Get(), 0, 4);
    Hr(lists[2]->Close(), "close consumer");
    ID3D12CommandList* first[] = {lists[0].Get()};
    render->ExecuteCommandLists(1, first); Hr(render->Signal(producer.Get(), 1), "export ready");
    Hr(fg->Wait(producer.Get(), 1), "consumer waits for export");
    Hr(fg->Wait(gate.Get(), 1), "block secondary");
    ID3D12CommandList* consume[] = {lists[2].Get()}; fg->ExecuteCommandLists(1, consume);

    MultiGPU::DeferredPresent worker;
    MultiGPU::VirtualBackbufferCursor cursor; cursor.Reset(2);
    std::promise<void> entered;
    Check(worker.Submit([&] {
        entered.set_value();
        return static_cast<std::int32_t>(MultiGPU::WaitForQueueIdle(fg.Get()));
    }), "handoff");
    Check(entered.get_future().wait_for(2s) == std::future_status::ready, "worker entry");
    cursor.Advance(true, 0);
    Check(cursor.Current() == 1, "independent next render buffer");
    ID3D12CommandList* second[] = {lists[cursor.Current()].Get()};
    render->ExecuteCommandLists(1, second);
    Hr(MultiGPU::WaitForQueueIdle(render.Get()), "next render progresses while FG blocked");
    Check(!worker.ReadyWithin(0ms), "FG completed before gate release");
    Hr(gate->Signal(1), "release secondary");
    Check(worker.ReadyWithin(2s), "worker completion");
    Check(worker.Take() == S_OK, "worker result");
    Hr(output->Map(0, nullptr, &mapped), "readback");
    UINT32 observed = 0; std::memcpy(&observed, mapped, 4); output->Unmap(0, nullptr);
    Check(observed == values[0], "pending frame was overwritten by next render");
    // Model resize/teardown: collect the Present first, drain both queues, then
    // destroy resources and reset the independently maintained application index.
    worker.Stop();
    Hr(MultiGPU::WaitForQueueIdle(render.Get()), "resize render drain");
    Hr(MultiGPU::WaitForQueueIdle(fg.Get()), "resize FG drain");
    cursor.Reset(3); Check(cursor.Current() == 0, "resize cursor");
    std::cout << "PASS: WARP next-frame render overlaps blocked FG and preserves prior-frame data before teardown\n";
}
