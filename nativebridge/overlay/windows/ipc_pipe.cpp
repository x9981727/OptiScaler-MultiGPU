// SPDX-License-Identifier: MIT
#include "ipc_pipe.hpp"
#include "native_bridge/wire.hpp"
#include <sddl.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace nb::ipc {
namespace {
constexpr size_t Bytes=1152, Header=32;
constexpr uint32_t Magic=0x3250494e;
struct Handle {
    HANDLE h{};
    ~Handle(){if(h && h!=INVALID_HANDLE_VALUE) CloseHandle(h);}
};
struct Local {
    void* p{};
    ~Local(){if(p)LocalFree(p);}
};
void u32(std::byte* p,uint32_t x){for(unsigned i=0;i<4;++i)p[i]=std::byte((x>>(8*i))&255);}
uint32_t r32(const std::byte* p){uint32_t x=0;for(unsigned i=0;i<4;++i)x|=uint32_t(p[i])<<(8*i);return x;}
void u64(std::byte* p,uint64_t x){u32(p,uint32_t(x));u32(p+4,uint32_t(x>>32));}
uint64_t r64(const std::byte* p){return r32(p)|(uint64_t(r32(p+4))<<32);}
HRESULT last(){return HRESULT_FROM_WIN32(GetLastError());}
std::wstring name(DWORD pid,uint64_t instance){
    return L"\\\\.\\pipe\\MagpieNativeBridge-v2-"+std::to_wstring(pid)+L"-"+std::to_wstring(instance);
}
bool tokenInfo(HANDLE token,TOKEN_INFORMATION_CLASS cls,std::vector<std::byte>& result){
    DWORD size=0;GetTokenInformation(token,cls,nullptr,0,&size);
    if(!size || size>65536)return false;
    result.resize(size);
    return GetTokenInformation(token,cls,result.data(),size,&size)!=FALSE;
}
HRESULT finish(HANDLE pipe,OVERLAPPED& ov,BOOL immediate,DWORD immediateError,DWORD timeout,DWORD& count){
    if(immediate)return GetOverlappedResult(pipe,&ov,&count,FALSE)?S_OK:last();
    if(immediateError!=ERROR_IO_PENDING)return HRESULT_FROM_WIN32(immediateError);
    const DWORD wait=WaitForSingleObject(ov.hEvent,timeout);
    if(wait==WAIT_OBJECT_0)return GetOverlappedResult(pipe,&ov,&count,FALSE)?S_OK:last();
    const DWORD waitError=wait==WAIT_TIMEOUT?ERROR_TIMEOUT:GetLastError();
    // Cancellation is not completion. Drain the OVERLAPPED before it leaves the
    // stack. If the I/O won the race with the timeout, preserve that completion.
    CancelIoEx(pipe,&ov);
    DWORD drained=0;
    if(GetOverlappedResult(pipe,&ov,&drained,TRUE)){
        count=drained;
        return S_OK;
    }
    const DWORD completionError=GetLastError();
    if(wait==WAIT_TIMEOUT && completionError==ERROR_OPERATION_ABORTED)
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    return HRESULT_FROM_WIN32(wait==WAIT_TIMEOUT?completionError:waitError);
}
}
Channel::~Channel(){Close();}
void Channel::Close() noexcept{
    if(peer_){CloseHandle(peer_);peer_=nullptr;}
    if(pipe_!=INVALID_HANDLE_VALUE){
        if(listener_)DisconnectNamedPipe(pipe_);
        CloseHandle(pipe_);pipe_=INVALID_HANDLE_VALUE;
    }
    listener_=false;peerPid_=0;
}
HRESULT Channel::Listen(uint64_t instance) noexcept{
    if(pipe_!=INVALID_HANDLE_VALUE || !instance)return E_INVALIDARG;
    try{
        Handle token; if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.h))return last();
        std::vector<std::byte> user;
        if(!tokenInfo(token.h,TokenUser,user))return E_ACCESSDENIED;
        LPWSTR sid=nullptr;
        if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid,&sid))return last();
        Local sidOwner{sid};
        const std::wstring sddl=L"D:P(A;;GA;;;"+std::wstring(sid)+L")";
        PSECURITY_DESCRIPTOR descriptor=nullptr;
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr))return last();
        Local descriptorOwner{descriptor};
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),descriptor,FALSE};
        pipe_=CreateNamedPipeW(name(GetCurrentProcessId(),instance).c_str(),
            PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,
            1,DWORD(Bytes*4),DWORD(Bytes*4),0,&security);
        if(pipe_==INVALID_HANDLE_VALUE)return last();
        listener_=true;return S_OK;
    }catch(...){Close();return E_OUTOFMEMORY;}
}
HRESULT Channel::Authenticate(bool server,DWORD expected) noexcept{
    ULONG pid=0;
    if(!(server?GetNamedPipeClientProcessId(pipe_,&pid):GetNamedPipeServerProcessId(pipe_,&pid)))return last();
    if(!pid || (expected && pid!=expected))return E_ACCESSDENIED;
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_DUP_HANDLE,FALSE,pid)};
    if(!process.h)return last();
    Handle selfToken,otherToken;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&selfToken.h) ||
       !OpenProcessToken(process.h,TOKEN_QUERY,&otherToken.h))return last();
    try{
        std::vector<std::byte> a,b;
        if(!tokenInfo(selfToken.h,TokenUser,a)||!tokenInfo(otherToken.h,TokenUser,b))return E_ACCESSDENIED;
        if(!EqualSid(reinterpret_cast<TOKEN_USER*>(a.data())->User.Sid,
                     reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid))return E_ACCESSDENIED;
        TOKEN_STATISTICS sa{},sb{};DWORD bytes=0,sessionA=0,sessionB=0;
        if(!GetTokenInformation(selfToken.h,TokenStatistics,&sa,sizeof(sa),&bytes)||
           !GetTokenInformation(otherToken.h,TokenStatistics,&sb,sizeof(sb),&bytes)||
           !GetTokenInformation(selfToken.h,TokenSessionId,&sessionA,sizeof(sessionA),&bytes)||
           !GetTokenInformation(otherToken.h,TokenSessionId,&sessionB,sizeof(sessionB),&bytes))return last();
        if(sa.AuthenticationId.LowPart!=sb.AuthenticationId.LowPart||
           sa.AuthenticationId.HighPart!=sb.AuthenticationId.HighPart||sessionA!=sessionB)return E_ACCESSDENIED;
        peer_=process.h;process.h=nullptr;peerPid_=pid;return S_OK;
    }catch(...){return E_OUTOFMEMORY;}
}
HRESULT Channel::Accept(DWORD timeout,DWORD expected) noexcept{
    if(pipe_==INVALID_HANDLE_VALUE||!listener_||peer_||!timeout||timeout==INFINITE)return E_INVALIDARG;
    Handle event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};if(!event.h)return last();
    OVERLAPPED ov{};ov.hEvent=event.h;DWORD count=0;
    const BOOL ok=ConnectNamedPipe(pipe_,&ov);const DWORD error=ok?ERROR_SUCCESS:GetLastError();
    HRESULT hr=error==ERROR_PIPE_CONNECTED?S_OK:finish(pipe_,ov,ok,error,timeout,count);
    if(SUCCEEDED(hr))hr=Authenticate(true,expected);
    if(FAILED(hr))Close();return hr;
}
HRESULT Channel::Connect(DWORD pid,uint64_t instance,DWORD timeout) noexcept{
    if(pipe_!=INVALID_HANDLE_VALUE||!pid||!instance||!timeout||timeout==INFINITE)return E_INVALIDARG;
    try{
        const auto path=name(pid,instance);const auto start=GetTickCount64();
        while(true){
            pipe_=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
            if(pipe_!=INVALID_HANDLE_VALUE)break;
            const DWORD error=GetLastError();
            if(error!=ERROR_PIPE_BUSY && error!=ERROR_FILE_NOT_FOUND)return HRESULT_FROM_WIN32(error);
            if(GetTickCount64()-start>=timeout)return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            Sleep(1);
        }
        DWORD mode=PIPE_READMODE_MESSAGE;
        if(!SetNamedPipeHandleState(pipe_,&mode,nullptr,nullptr)){const HRESULT hr=last();Close();return hr;}
        const HRESULT hr=Authenticate(false,pid);if(FAILED(hr))Close();return hr;
    }catch(...){Close();return E_OUTOFMEMORY;}
}
HRESULT Channel::Transfer(bool write,std::span<std::byte> data,DWORD timeout) noexcept{
    if(!Connected()||!timeout||timeout==INFINITE||data.size()!=Bytes)return E_INVALIDARG;
    Handle event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};if(!event.h)return last();
    OVERLAPPED ov{};ov.hEvent=event.h;DWORD count=0;
    const BOOL ok=write?WriteFile(pipe_,data.data(),DWORD(data.size()),nullptr,&ov):
                         ReadFile(pipe_,data.data(),DWORD(data.size()),nullptr,&ov);
    const DWORD error=ok?ERROR_SUCCESS:GetLastError();
    HRESULT hr=finish(pipe_,ov,ok,error,timeout,count);
    if(SUCCEEDED(hr)&&count!=data.size())hr=HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    // Idle receive polling is expected in the game/Magpie receiver loop. An
    // aborted read after its deadline is recoverable. A timed-out write is not:
    // delivery would be ambiguous, so fail closed and tear down the session.
    const HRESULT timeoutHr=HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if(FAILED(hr) && (write || hr!=timeoutHr))Close();
    return hr;
}
HRESULT Channel::Send(const Message& msg,DWORD timeout) noexcept{
    const auto kind=static_cast<uint32_t>(msg.kind);
    if(kind<1||kind>5||msg.length>PayloadCapacity)return E_INVALIDARG;
    std::array<std::byte,Bytes> bytes{};
    u32(bytes.data(),Magic);u32(bytes.data()+4,kind);u64(bytes.data()+8,msg.sequence);
    u32(bytes.data()+16,msg.slot);u32(bytes.data()+20,msg.length);
    std::copy_n(msg.payload.begin(),msg.length,bytes.begin()+Header);
    u32(bytes.data()+Bytes-4,wire::crc32(std::span(bytes).first(Bytes-4)));
    return Transfer(true,bytes,timeout);
}
HRESULT Channel::Receive(Message& output,DWORD timeout) noexcept{
    std::array<std::byte,Bytes> bytes{};HRESULT hr=Transfer(false,bytes,timeout);if(FAILED(hr))return hr;
    const auto type=r32(bytes.data()+4),length=r32(bytes.data()+20);
    if(r32(bytes.data())!=Magic || type<1 || type>5 || length>PayloadCapacity ||
       r32(bytes.data()+Bytes-4)!=wire::crc32(std::span(bytes).first(Bytes-4)) ||
       std::any_of(bytes.begin()+24,bytes.begin()+Header,[](auto v){return v!=std::byte{};})||
       std::any_of(bytes.begin()+Header+length,bytes.end()-4,[](auto v){return v!=std::byte{};})){
        Close();return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    Message msg{};msg.kind=static_cast<Kind>(type);msg.sequence=r64(bytes.data()+8);
    msg.slot=r32(bytes.data()+16);msg.length=length;
    std::copy_n(bytes.begin()+Header,length,msg.payload.begin());output=msg;return S_OK;
}
HRESULT Channel::DuplicateToPeer(HANDLE source,HANDLE& output)const noexcept{
    output=nullptr;if(!Connected()||!source||source==INVALID_HANDLE_VALUE)return E_INVALIDARG;
    if(!DuplicateHandle(GetCurrentProcess(),source,peer_,&output,0,FALSE,DUPLICATE_SAME_ACCESS))return last();
    return S_OK;
}
HRESULT Channel::RevokeUnsent(HANDLE handle)const noexcept{
    if(!Connected()||!handle||handle==INVALID_HANDLE_VALUE)return E_INVALIDARG;
    HANDLE local=nullptr;
    if(!DuplicateHandle(peer_,handle,GetCurrentProcess(),&local,0,FALSE,DUPLICATE_CLOSE_SOURCE|DUPLICATE_SAME_ACCESS))return last();
    if(local)CloseHandle(local);return S_OK;
}
}
