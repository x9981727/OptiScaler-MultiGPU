#include <string>
// SPDX-License-Identifier: MIT
#include "ipc_pipe.hpp"
#include "native_bridge/wire.hpp"
#include "test_support.hpp"
#include <iostream>
#include <cstring>
#include <cwchar>
#include <stdexcept>

namespace {
void check(bool v,const char* why){if(!v)throw std::runtime_error(why);}
void good(HRESULT hr,const char* why){if(FAILED(hr)){std::cerr<<why<<" hr=0x"<<std::hex<<uint32_t(hr)<<'\n';throw std::runtime_error(why);}}
int child(DWORD pid,uint64_t instance){
    nb::ipc::Channel pipe;good(pipe.Connect(pid,instance,5000),"client connect/auth");
    check(pipe.PeerPid()==pid,"server identity");
    nb::ipc::Message message;good(pipe.Receive(message,5000),"handle receive");
    check(message.kind==nb::ipc::Kind::Handles&&message.length==8,"handle schema");
    uint64_t bits=0;std::memcpy(&bits,message.payload.data(),8);
    const HANDLE event=reinterpret_cast<HANDLE>(static_cast<uintptr_t>(bits));
    check(SetEvent(event)!=FALSE,"duplicated event works");CloseHandle(event);
    for(uint64_t i=1;i<=128;++i){
        good(pipe.Receive(message,5000),"frame receive");
        check(message.kind==nb::ipc::Kind::Frame&&message.sequence==i&&message.length==nb::wire::PacketBytes,"frame header");
        nb::Packet packet;check(nb::wire::decode(std::span(message.payload).first(message.length),packet)==nb::wire::Result::Ok,"wire decode");
        check(packet.key.frame==i&&packet.depth.key==packet.key&&packet.camera.key==packet.key,"same frame");
        message={};message.kind=nb::ipc::Kind::Release;message.sequence=i;
        good(pipe.Send(message,5000),"release send");
    }
    return 0;
}
int parent(){
    const auto instance=GetTickCount64()+1;nb::ipc::Channel pipe;
    good(pipe.Listen(instance),"listen");
    wchar_t exe[32768]{};const DWORD chars=GetModuleFileNameW(nullptr,exe,32768);
    check(chars>0&&chars<32768,"exe path");
    std::wstring cmd=L"\""+std::wstring(exe)+L"\" --child "+std::to_wstring(GetCurrentProcessId())+L" "+std::to_wstring(instance);
    STARTUPINFOW start{};start.cb=sizeof(start);PROCESS_INFORMATION process{};
    check(CreateProcessW(exe,cmd.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&start,&process)!=FALSE,"create child");
    CloseHandle(process.hThread);
    try{
        good(pipe.Accept(5000,process.dwProcessId),"accept/auth");
        check(pipe.PeerPid()==process.dwProcessId,"client identity");
        const HANDLE event=CreateEventW(nullptr,TRUE,FALSE,nullptr);check(event!=nullptr,"event");
        HANDLE peerEvent=nullptr;good(pipe.DuplicateToPeer(event,peerEvent),"duplicate event");
        nb::ipc::Message message{};message.kind=nb::ipc::Kind::Handles;message.length=8;
        const auto bits=uint64_t(reinterpret_cast<uintptr_t>(peerEvent));std::memcpy(message.payload.data(),&bits,8);
        good(pipe.Send(message,5000),"handle send");
        check(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"peer signaled event");CloseHandle(event);

        // The child is now blocked waiting for the first frame and sends nothing.
        // A receiver-poll timeout must not destroy the authenticated channel.
        const HRESULT idle=pipe.Receive(message,25);
        check(idle==HRESULT_FROM_WIN32(ERROR_TIMEOUT),"idle receive timeout");
        check(pipe.Connected()&&pipe.PeerPid()==process.dwProcessId,"timeout preserves authenticated channel");

        for(uint64_t i=1;i<=128;++i){
            auto p=test::packet(i);nb::wire::Bytes wire;check(nb::wire::encode(p,wire),"wire encode");
            message={};message.kind=nb::ipc::Kind::Frame;message.sequence=i;message.length=DWORD(wire.size());
            std::copy(wire.begin(),wire.end(),message.payload.begin());good(pipe.Send(message,5000),"frame send");
            good(pipe.Receive(message,5000),"release receive");
            check(message.kind==nb::ipc::Kind::Release&&message.sequence==i&&message.length==0,"release match");
        }
        check(WaitForSingleObject(process.hProcess,5000)==WAIT_OBJECT_0,"child finished");
        DWORD code=1;check(GetExitCodeProcess(process.hProcess,&code)&&code==0,"child success");
        CloseHandle(process.hProcess);process.hProcess=nullptr;
    }catch(...){
        if(process.hProcess){TerminateProcess(process.hProcess,2);WaitForSingleObject(process.hProcess,5000);CloseHandle(process.hProcess);}throw;
    }
    nb::ipc::Channel noClient;good(noClient.Listen(instance+1),"timeout listen");
    const auto begin=GetTickCount64();const HRESULT timeout=noClient.Accept(50);
    check(timeout==HRESULT_FROM_WIN32(ERROR_TIMEOUT),"accept timeout");
    check(GetTickCount64()-begin<3000,"bounded cancellation");
    std::cout<<"PASS: authenticated parent/child IPC, duplicated event, recoverable idle Receive timeout, 128 frame/release round trips, bounded Accept cancellation. NO dual-GPU/game test.\n";
    return 0;
}
}
int wmain(int argc,wchar_t**argv){
    try{
        if(argc==4&&std::wcscmp(argv[1],L"--child")==0)return child(DWORD(std::stoul(argv[2])),std::stoull(argv[3]));
        if(argc!=1) return 2;
        return parent();
    }catch(const std::exception&e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
