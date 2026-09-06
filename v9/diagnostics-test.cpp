#include "XeFGInitDiagnostics.h"
#include <fstream>
#include <string>

static bool ExerciseExceptionPropagation()
{
    __try
    {
        MultiGPU::Diagnostics::Invoke([]() -> int {
            RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
            return 42;
        });
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        return true;
    }
    return false;
}

int main()
{
    if (MultiGPU::Diagnostics::Invoke([] { return 37; }) != 37)
        return 1;
    if (!ExerciseExceptionPropagation())
        return 2;
    wchar_t path[MAX_PATH] {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    auto separator = wcsrchr(path, L'\\');
    if (separator == nullptr)
        return 3;
    wcscpy_s(separator + 1, MAX_PATH - (separator + 1 - path), L"OptiScaler-MultiGPU-exception.txt");
    std::ifstream input(path);
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (text.find("Code=0xC0000005") == std::string::npos || text.find("Fault ") == std::string::npos ||
        text.find("Stack ") == std::string::npos)
        return 4;
    return 0;
}
