#include "QueueIdle_Dx12.h"
#include <dxgi1_6.h>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
static void check(HRESULT hr, const char* step)
{
    if (FAILED(hr)) { std::cerr << "FAIL: " << step << ": " << std::hex << hr << '\n'; std::exit(1); }
}
static void require(bool ok, const char* step)
{
    if (!ok) { std::cerr << "FAIL: " << step << '\n'; std::exit(1); }
}
int main()
{
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> warp; ComPtr<ID3D12Device> device;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "WARP adapter");
    check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "WARP device");
    D3D12_COMMAND_QUEUE_DESC desc {};
    ComPtr<ID3D12CommandQueue> render, fg;
    check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&render)), "render queue");
    check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&fg)), "FG queue");
    check(MultiGPU::WaitForQueueIdle(render.Get()), "first wait creates its own fence");
    for (int i=0; i<10; ++i) check(MultiGPU::WaitForQueueIdle(fg.Get()), "repeat wait");

    ComPtr<ID3D12Fence> gate;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate fence");
    check(fg->Wait(gate.Get(), 1), "block FG queue independently of render queue");
    check(MultiGPU::WaitForQueueIdle(render.Get()), "render idle does not imply FG idle");
    std::promise<void> attempted; auto started=attempted.get_future();
    auto pending=std::async(std::launch::async, [&] { attempted.set_value(); return MultiGPU::WaitForQueueIdle(fg.Get()); });
    started.wait();
    require(pending.wait_for(40ms)==std::future_status::timeout, "FG wait must not return while GPU work is blocked");
    check(gate->Signal(1), "release FG queue");
    require(pending.wait_for(2s)==std::future_status::ready, "FG queue completion observed");
    check(pending.get(), "FG drain result");

    require(MultiGPU::WaitForFenceValue(gate.Get(), 2, 1)==HRESULT_FROM_WIN32(WAIT_TIMEOUT), "timeout reports failure");
    require(MultiGPU::WaitForQueueIdle(nullptr)==E_POINTER, "null queue reports failure");
    std::cout << "PASS: WARP queue drain, independent queues, first-use fence and timeout\n";
}
