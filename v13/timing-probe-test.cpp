#include "QueueSpanProbe_Dx12.h"
#include "XeFGTimingCounters.h"
#include <dxgi1_6.h>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>
using Microsoft::WRL::ComPtr;
static void require(bool ok, const char* step)
{
    if (!ok) { std::cerr << "FAIL: " << step << '\n'; std::exit(1); }
}
static void check(HRESULT hr, const char* step) { require(SUCCEEDED(hr), step); }
int main()
{
    MultiGPU::XeFGTimingCounters counters;
    std::vector<std::thread> writers;
    for (int i=0;i<4;++i) writers.emplace_back([&] {
        for (int n=0;n<1000;++n) counters.Add(MultiGPU::XeFGCpuPhase::Sleep, 2.0);
    });
    for (auto& t:writers) t.join();
    counters.Add(MultiGPU::XeFGCpuPhase::Sleep, -1);
    counters.Add(MultiGPU::XeFGCpuPhase::Sleep, std::numeric_limits<double>::quiet_NaN());
    counters.Add(MultiGPU::XeFGCpuPhase::Count, 3);
    auto cpu = counters.Take();
    require(cpu[0].calls==4000 && cpu[0].Mean()==2.0, "thread-safe independent CPU call counts");
    require(cpu[1].Mean()==-1.0 && counters.Take()[0].Mean()==-1.0, "missing and reset timings are unknown");

    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> warp; ComPtr<ID3D12Device> device;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP");
    check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)), "device");
    D3D12_COMMAND_QUEUE_DESC desc {};
    ComPtr<ID3D12CommandQueue> queue; check(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)), "queue");
    ComPtr<ID3D12Fence> gate; check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)), "gate");
    MultiGPU::QueueSpanProbe probe;
    require(probe.Take().meanMs==-1, "unavailable GPU time is unknown");
    check(queue->Wait(gate.Get(),1), "block GPU");
    const auto start=std::chrono::steady_clock::now();
    for (int i=0;i<4;++i) { require(probe.Begin(queue.Get()), "record pending interval"); probe.End(); }
    require(!probe.Begin(queue.Get()), "busy query slots skipped");
    auto pending=probe.Take();
    require(pending.samples==0 && pending.meanMs==-1 && pending.skipped==1, "no premature readback");
    require(std::chrono::steady_clock::now()-start<std::chrono::seconds(2), "probe does not block on GPU");
    check(gate->Signal(1), "release gate"); check(probe.Drain(), "drain");
    auto done=probe.Take();
    require(done.samples==4 && done.meanMs>=0 && SUCCEEDED(done.error), "four timestamp pairs resolved");
    require(probe.Take().samples==0, "samples counted once");
    for (int i=0;i<8;++i)
    {
        require(probe.Begin(queue.Get()), "reuse retired query slot"); probe.End();
        check(probe.Drain(), "reuse drain");
        require(probe.Take().samples==1, "one reused interval");
    }
    MultiGPU::QueueSpanProbe invalid;
    require(!invalid.Begin(nullptr) && invalid.Take().error==E_POINTER, "invalid queue disables only probe");
    std::cout << "PASS: nonblocking GPU timestamps, safe slot reuse, unknown samples and CPU counters\n";
}
