#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

// Record an exception escaping XeFG initialization, then continue the normal
// Windows exception search. Never turn a driver fault into a successful init.
namespace MultiGPU::Diagnostics
{
inline void WriteAddress(HANDLE file, const char* label, const void* address)
{
    MEMORY_BASIC_INFORMATION memory {};
    char module[MAX_PATH] = "<unknown>";
    uintptr_t base = 0;
    if (VirtualQuery(address, &memory, sizeof(memory)) != 0 && memory.Type == MEM_IMAGE)
    {
        base = reinterpret_cast<uintptr_t>(memory.AllocationBase);
        GetModuleFileNameA(static_cast<HMODULE>(memory.AllocationBase), module, MAX_PATH);
    }
    char line[768] {};
    const int length = _snprintf_s(line, sizeof(line), _TRUNCATE, "%s %p %s + 0x%llX\r\n",
                                  label, address, module,
                                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) - base));
    DWORD written = 0;
    if (length > 0)
        WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
}

inline LONG RecordException(EXCEPTION_POINTERS* exception)
{
    if (exception == nullptr || exception->ExceptionRecord == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;

    // A first-chance C++ exception can be normal library control flow.
    const auto code = exception->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_IN_PAGE_ERROR &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;

    wchar_t path[MAX_PATH] {};
    const auto length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return EXCEPTION_CONTINUE_SEARCH;
    auto separator = wcsrchr(path, L'\\');
    if (separator == nullptr)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto remaining = MAX_PATH - static_cast<size_t>(separator + 1 - path);
    constexpr wchar_t filename[] = L"OptiScaler-MultiGPU-exception.txt";
    if (remaining < _countof(filename))
        return EXCEPTION_CONTINUE_SEARCH;
    wcscpy_s(separator + 1, remaining, filename);

    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return EXCEPTION_CONTINUE_SEARCH;

    SYSTEMTIME now {};
    GetSystemTime(&now);
    char line[384] {};
    const int count = _snprintf_s(line, sizeof(line), _TRUNCATE,
        "MultiGPU v9 XeFG init exception (UTC %04u-%02u-%02u %02u:%02u:%02u)\r\n"
        "Code=0x%08lX Thread=%lu Parameters=%lu Info0=0x%llX Info1=0x%llX\r\n",
        static_cast<unsigned>(now.wYear), static_cast<unsigned>(now.wMonth),
        static_cast<unsigned>(now.wDay), static_cast<unsigned>(now.wHour),
        static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond),
        code, GetCurrentThreadId(), exception->ExceptionRecord->NumberParameters,
        static_cast<unsigned long long>(exception->ExceptionRecord->NumberParameters > 0
            ? exception->ExceptionRecord->ExceptionInformation[0] : 0),
        static_cast<unsigned long long>(exception->ExceptionRecord->NumberParameters > 1
            ? exception->ExceptionRecord->ExceptionInformation[1] : 0));
    DWORD written = 0;
    if (count > 0)
        WriteFile(file, line, static_cast<DWORD>(count), &written, nullptr);
    WriteAddress(file, "Fault", exception->ExceptionRecord->ExceptionAddress);

    // This is the filter's call stack. Fault above is the actual exception PC.
    void* frames[24] {};
    const auto frameCount = CaptureStackBackTrace(0, _countof(frames), frames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i)
        WriteAddress(file, "Stack", frames[i]);
    FlushFileBuffers(file);
    CloseHandle(file);
    return EXCEPTION_CONTINUE_SEARCH;
}

template <typename Call> auto Invoke(const Call& call) -> decltype(call())
{
    // No C++ objects requiring unwinding may be local to this SEH thunk.
    __try
    {
        return call();
    }
    __except (RecordException(GetExceptionInformation()))
    {
        // Unreachable: the filter always returns EXCEPTION_CONTINUE_SEARCH.
        return {};
    }
}
}
