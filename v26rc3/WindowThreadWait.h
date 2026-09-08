#pragma once
// Process only cross-thread sent messages while the HWND owner must wait.
// Never remove posted game input, WM_QUIT, timers or queued resize requests.
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <mutex>
namespace XeFGGamePacing {
inline std::atomic<unsigned long long> sentMessageServiceCalls{0};
inline bool OwnsWindow(HWND window) {
    return window && GetWindowThreadProcessId(window,nullptr)==GetCurrentThreadId();
}
inline void ServiceSentMessages() {
    MSG ignored{};
    PeekMessageW(&ignored,nullptr,0,0,PM_NOREMOVE|PM_QS_SENDMESSAGE);
    ++sentMessageServiceCalls;
}
inline DWORD WindowHandleWait(HWND window,HANDLE handle,DWORD milliseconds) {
    if(!OwnsWindow(window))return WaitForSingleObject(handle,milliseconds);
    const ULONGLONG end=GetTickCount64()+milliseconds;
    for(;;) {
        DWORD signalled=WaitForSingleObject(handle,0);
        if(signalled!=WAIT_TIMEOUT)return signalled;
        const ULONGLONG now=GetTickCount64();
        if(now>=end)return WAIT_TIMEOUT;
        const DWORD remaining=static_cast<DWORD>(end-now);
        DWORD result=MsgWaitForMultipleObjectsEx(1,&handle,remaining,QS_SENDMESSAGE,MWMO_INPUTAVAILABLE);
        if(result==WAIT_OBJECT_0+1){ServiceSentMessages();continue;}
        return result;
    }
}
// Fast uncontended path is a normal recursive mutex. Only the HWND owner pumps
// sent messages if another submitting thread currently owns the lock.
class WindowSubmissionMutex {
    std::recursive_mutex value;
    HWND window=nullptr;
public:
    void SetWindow(HWND h){window=h;}
    void lock(){
        if(value.try_lock())return;
        if(!OwnsWindow(window)){value.lock();return;}
        while(!value.try_lock()) {
            DWORD r=MsgWaitForMultipleObjectsEx(0,nullptr,1,QS_SENDMESSAGE,MWMO_INPUTAVAILABLE);
            if(r==WAIT_OBJECT_0)ServiceSentMessages();
            else if(r==WAIT_FAILED)Sleep(1);
        }
    }
    bool try_lock(){return value.try_lock();}
    void unlock(){value.unlock();}
};
template<class Predicate>
inline bool WindowConditionWait(HWND window,HANDLE progress,std::condition_variable& changed,
                                std::unique_lock<std::mutex>& lock,DWORD milliseconds,Predicate predicate) {
    if(!OwnsWindow(window))
        return changed.wait_for(lock,std::chrono::milliseconds(milliseconds),predicate);
    const ULONGLONG end=GetTickCount64()+milliseconds;
    while(!predicate()) {
        if(GetTickCount64()>=end)return false;
        ResetEvent(progress);
        // Recheck atomic terminal predicates in case they changed before reset.
        if(predicate())return true;
        const ULONGLONG now=GetTickCount64();
        if(now>=end)return false;
        const DWORD remaining=static_cast<DWORD>(end-now);
        lock.unlock();
        const DWORD result=WindowHandleWait(window,progress,remaining);
        lock.lock();
        if(result==WAIT_FAILED)return false;
        if(result==WAIT_TIMEOUT&&!predicate())return false;
    }
    return true;
}
struct SignalOnExit {
    HANDLE event;
    ~SignalOnExit(){if(event)SetEvent(event);}
};
} // namespace XeFGGamePacing
