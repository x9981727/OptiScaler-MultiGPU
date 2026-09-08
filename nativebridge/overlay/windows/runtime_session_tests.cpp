// SPDX-License-Identifier: MIT
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "runtime_session.hpp"
#include "test_support.hpp"

using Microsoft::WRL::ComPtr;
namespace {
void check(bool v,const char* why){if(!v)throw std::runtime_error(why);}
uint64_t now_ns(){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count());}

int run(IDXGIAdapter1* adapter){
    ComPtr<ID3D12Device> producerDevice, consumerDevice;
    if(FAILED(D3D12CreateDevice(adapter,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(producerDevice.ReleaseAndGetAddressOf()))))return 77;
    if(FAILED(D3D12CreateDevice(adapter,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(consumerDevice.ReleaseAndGetAddressOf()))))return 77;

    constexpr uint64_t instance=0x4e425232ULL;
    constexpr uint64_t session=0x1111222233334444ULL;
    constexpr uint64_t generation=3;
    constexpr uint64_t viewport=77;
    const nb::Extent extent{1920,1080};

    nb::session::ProducerSession producer;
    nb::session::ProducerConfig pc{};
    pc.instance=instance;pc.session=session;pc.generation=generation;pc.viewport=viewport;
    pc.extent=extent;pc.colorFormat=nb::Format::Rgba8;pc.requireDifferentAdapters=false;
    const HRESULT listen=producer.Listen(producerDevice.Get(),pc);
    if(listen==DXGI_ERROR_UNSUPPORTED || listen==E_NOTIMPL)return 77;
    check(SUCCEEDED(listen),"producer listen");
    check(producer.Accept(20)==HRESULT_FROM_WIN32(ERROR_TIMEOUT),"accept timeout");

    HRESULT acceptHr=E_PENDING;
    std::thread acceptThread([&]{acceptHr=producer.Accept(5000);});
    nb::session::ConsumerSession consumer;
    nb::session::ConsumerConfig cc{};
    cc.instance=instance;cc.viewport=viewport;cc.colorFormat=nb::Format::Rgba8;cc.requireDifferentAdapters=false;
    const HRESULT connect=consumer.Connect(GetCurrentProcessId(),consumerDevice.Get(),cc,5000);
    acceptThread.join();
    check(SUCCEEDED(connect),"consumer connect after producer timeout");
    check(SUCCEEDED(acceptHr),"producer accept after timeout");
    check(producer.Connected()&&consumer.Connected(),"connected state");
    check(producer.PeerPid()==GetCurrentProcessId()&&consumer.PeerPid()==GetCurrentProcessId(),"authenticated peer pid");
    check(producer.GetPolicy().session==session&&consumer.GetPolicy().generation==generation,"session policy");
    check(producer.GetPolicy().renderAdapter==consumer.GetPolicy().renderAdapter,"render adapter policy");
    check(producer.GetPolicy().processingAdapter==consumer.GetPolicy().processingAdapter,"processing adapter policy");

    nb::Token timedToken{};nb::Packet timedPacket{};
    const HRESULT timeout=consumer.ReceiveFrame(timedToken,timedPacket,now_ns(),20);
    check(timeout==HRESULT_FROM_WIN32(ERROR_TIMEOUT),"idle receive timeout");
    check(consumer.Connected(),"receive timeout preserves authenticated connection");

    auto packet=test::packet(9);
    packet.key={session,generation,9,viewport};packet.previousFrame=8;packet.timestampNs=now_ns();
    packet.renderAdapter=producer.GetPolicy().renderAdapter;
    packet.processingAdapter=producer.GetPolicy().processingAdapter;
    packet.color.key=packet.depth.key=packet.motion.key=packet.camera.key=packet.key;
    packet.color.extent=packet.depth.extent=packet.motion.extent=extent;
    const nb::Rect full{0,0,extent.width,extent.height};
    packet.color.validRect=packet.depth.validRect=packet.motion.validRect=full;
    const nb::Token token{1,42};
    check(SUCCEEDED(producer.SendFrame(token,packet,packet.timestampNs,5000)),"producer send frame");

    nb::Token received{};nb::Packet packet2{};
    check(SUCCEEDED(consumer.ReceiveFrame(received,packet2,packet.timestampNs,5000)),"consumer receive frame");
    check(received==token&&packet2.key==packet.key,"frame identity");
    check(std::memcmp(&packet2.camera.viewToClip,&packet.camera.viewToClip,sizeof(nb::Matrix))==0,
        "camera matrices survive runtime session");

    check(SUCCEEDED(consumer.SendRelease(token,5000)),"consumer send release");
    nb::Token released{};
    check(SUCCEEDED(producer.ReceiveRelease(released,5000))&&released==token,"producer receive release");
    check(SUCCEEDED(producer.SendStop(5000)),"producer send stop");
    check(SUCCEEDED(consumer.ReceiveStop(5000)),"consumer receive stop");
    consumer.Close();producer.Close();
    std::cout<<"PASS runtime session: accept/receive timeout recovery, auth, handles, frame/camera/release/stop\n";
    return 0;
}
}

int main(){
    try{
        ComPtr<IDXGIFactory1> factory;
        if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()))))return 1;
        for(UINT i=0;;++i){
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT hr=factory->EnumAdapters1(i,adapter.ReleaseAndGetAddressOf());
            if(hr==DXGI_ERROR_NOT_FOUND)break;if(FAILED(hr))return 2;
            const int rc=run(adapter.Get());if(rc==77)continue;return rc;
        }
        std::cout<<"SKIP no D3D12 adapter supports NativeBridge runtime session\n";return 77;
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
