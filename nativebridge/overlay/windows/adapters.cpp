// SPDX-License-Identifier: MIT
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <iomanip>
#include <iostream>
int main() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) return 1;
    for (UINT i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory->EnumAdapters1(i, adapter.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) return 2;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) return 3;
        std::wcout << L"index=" << std::dec << i << L" luid=" << std::hex
            << std::setfill(L'0') << std::setw(8) << static_cast<UINT>(desc.AdapterLuid.HighPart)
            << L":" << std::setw(8) << desc.AdapterLuid.LowPart << L" vendor=" << desc.VendorId
            << L" software=" << ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            << L" name=" << desc.Description << L"\n";
    }
    return 0;
}
