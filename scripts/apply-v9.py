"""Apply v9 to the exact v8 source. Fail closed on missing/duplicate anchors."""
from pathlib import Path
import re
import shutil

kit = Path(__file__).resolve().parents[1]
root = kit / "upstream" / "OptiScaler"


def replace(text, old, new, count=1):
    actual = text.count(old)
    if actual != count:
        raise RuntimeError(f"v9 anchor count {actual}, expected {count}: {old[:100]}")
    return text.replace(old, new)


def inject_function(text, signature, code):
    if text.count(signature) != 1:
        raise RuntimeError(f"v9 function anchor not unique: {signature}")
    start = text.index("{", text.index(signature)) + 1
    return text[:start] + "\n" + code + text[start:]


state = root / "State.h"
s = state.read_text(encoding="utf-8-sig")
s = replace(s, "    ScopedMultiGpuXeFGInit()", "    explicit ScopedMultiGpuXeFGInit(bool enabled = true)")
s = replace(s, "        State::multiGpuXeFGInit = true;", "        State::multiGpuXeFGInit = previousState || enabled;")
state.write_text(s, encoding="utf-8")

d3d = root / "hooks/D3D12_Hooks.cpp"
d = d3d.read_text(encoding="utf-8-sig")
d = replace(d, "static bool _creatingD3D12Device = false;", "static thread_local bool _creatingD3D12Device = false;")
old = '''    if (State::multiGpuXeFGInit)
    {
        LOG_TRACE("MultiGPU v7: passthrough D3D12CreateDevice during XeFG internal initialization");
        LOG_TRACE("MultiGPU v8: passthrough D3D12CreateDevice after FG adapter override");
        _creatingD3D12Device = true;
        auto result = o_D3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);
        _creatingD3D12Device = false;
        return result;
    }

'''
d = replace(d, old, "")
native = '''    if (State::multiGpuXeFGInit)
    {
        // XeFG already receives the selected device and queue explicitly. Driver
        // helper/probe calls retain their original adapter, including nullptr.
        struct CreationGuard
        {
            bool previous = _creatingD3D12Device;
            CreationGuard() { _creatingD3D12Device = true; }
            ~CreationGuard() { _creatingD3D12Device = previous; }
        } guard;
        LOG_INFO("MultiGPU v9: native D3D12 init call, original adapter={:X}, caller={}",
                 (size_t) pAdapter, Util::WhoIsTheCaller(_ReturnAddress()));
        auto result = o_D3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);
        LOG_INFO("MultiGPU v9: native D3D12 init returned HRESULT={:08X}; game device unchanged",
                 (UINT) result);
        return result;
    }

'''
d = replace(d, "    Microsoft::WRL::ComPtr<IDXGIAdapter1> forcedMultiGpuAdapter;",
            native + "    Microsoft::WRL::ComPtr<IDXGIAdapter1> forcedMultiGpuAdapter;")
d = inject_function(d, "static HRESULT hkCreateDevice(ID3D12DeviceFactory*", '''    if (State::multiGpuXeFGInit)
        return o_CreateDevice(pFactory, pAdapter, MinimumFeatureLevel, riid, ppDevice);
''')
d3d.write_text(d, encoding="utf-8")

# Native factory creation alone is insufficient: existing DXGI method detours
# are process-wide. Bypass those enumeration detours inside the same TLS scope.
dxgi = root / "hooks/Dxgi_Hooks.cpp"
t = dxgi.read_text(encoding="utf-8-sig")
for suffix, args in [("", "riid, ppFactory"), ("1", "riid, ppFactory"), ("2", "Flags, riid, ppFactory")]:
    t = inject_function(t, f"inline static HRESULT hkCreateDXGIFactory{suffix}(", f'''    if (State::multiGpuXeFGInit)
    {{
        LOG_TRACE("MultiGPU v9: native DXGI factory{suffix} during XeFG init");
        return o_CreateDXGIFactory{suffix}({args});
    }}
''')
dxgi.write_text(t, encoding="utf-8")

enumerations = [
    ("EnumAdapters", "Adapter, ppAdapter"),
    ("EnumAdapters1", "Adapter, ppAdapter"),
    ("EnumAdapterByLuid", "AdapterLuid, riid, ppvAdapter"),
    ("EnumAdapterByGpuPreference", "Adapter, GpuPreference, riid, ppvAdapter"),
]
for filename, cls, prefix in [
    ("DxgiFactory_Hooks.cpp", "DxgiFactoryHooks", "o_"),
    ("DxgiFactory_WrappedCalls.cpp", "DxgiFactoryWrappedCalls", "realFactory->"),
]:
    p = root / "hooks" / filename
    t = p.read_text(encoding="utf-8-sig")
    for name, args in enumerations:
        call_args = "realFactory, " + args if prefix == "o_" else args
        t = inject_function(t, f"HRESULT {cls}::{name}(", f'''    if (State::multiGpuXeFGInit)
        return {prefix}{name}({call_args});
''')
    p.write_text(t, encoding="utf-8")

xefg = root / "framegen/xefg/XeFG_Dx12.cpp"
x = xefg.read_text(encoding="utf-8-sig")
x = replace(x, '#include "XeFG_Dx12.h"', '#include "XeFG_Dx12.h"\n#include "XeFGInitDiagnostics.h"')
# The old bool method returned non-zero SDK error codes as true.
start = x.index("bool XeFG_Dx12::CreateSwapchainContext(")
end = x.index("const char* XeFG_Dx12::Name()", start)
part = x[start:end]
assert part.count("return result;") == 3
part = part.replace("return result;", "return false;")
x = x[:start] + part + x[end:]

old_setup = '''        EnsureMultiGPURuntime(State::Instance().currentD3D12Device);
        auto xefgDevice = SelectedFGDevice(State::Instance().currentD3D12Device);
        CreateSwapchainContext(xefgDevice);'''
new_setup = '''        auto renderDevice = State::Instance().currentD3D12Device;
        {
            ScopedMultiGpuXeFGInit isolateSetup(Config::Instance()->FGMultiGPUEnabled.value_or_default());
            const bool contextCreated = MultiGPU::Diagnostics::Invoke([&]() {
                EnsureMultiGPURuntime(renderDevice);
                return CreateSwapchainContext(SelectedFGDevice(renderDevice));
            });
            if (!contextCreated)
                return false;
        }
        LOG_INFO("MultiGPU v9: context setup complete; preserved game device={:X}", (size_t) renderDevice);'''
x = replace(x, old_setup, new_setup, count=2)
x = replace(x, "    EnsureMultiGPURuntime(device);", '''    {
        ScopedMultiGpuXeFGInit isolateSetup(Config::Instance()->FGMultiGPUEnabled.value_or_default());
        EnsureMultiGPURuntime(device);
    }''')
for line in [
    '        D3D12Hooks::SetMultiGPUFGAdapterOverride(_multiGpuRuntime->FGDevice());\n',
    '        LOG_INFO("MultiGPU v8: armed XeFG internal D3D12 adapter override for secondary GPU");\n',
    '        D3D12Hooks::SetMultiGPUFGAdapterOverride(nullptr);\n',
    '        LOG_INFO("MultiGPU v8: disarmed XeFG internal D3D12 adapter override");\n',
]:
    x = replace(x, line, "", count=2)

pattern = r'(        ScopedMultiGpuXeFGInit isolateXeFGInit \{\};\n)        result = (XeFGProxy::D3D12InitFromSwapChainDesc\(\)\(.*?\);)'
def wrap_init(match):
    call = match[2][:-1]
    return match[1] + '''        LOG_INFO("MultiGPU v9: entering XeFG init with explicit FG device/queue and native driver calls");
        result = MultiGPU::Diagnostics::Invoke([&]() { return ''' + call + '''; });
        LOG_INFO("MultiGPU v9: XeFG init returned {} ({})", magic_enum::enum_name(result), (UINT) result);'''
x, count = re.subn(pattern, wrap_init, x, flags=re.S)
if count != 2:
    raise RuntimeError(f"v9 expected 2 scoped SDK calls, found {count}")
xefg.write_text(x, encoding="utf-8")
shutil.copyfile(kit / "v9/XeFGInitDiagnostics.h", xefg.parent / "XeFGInitDiagnostics.h")
print("v9 applied: preserve game device, native DXGI/D3D12 initialization, exception diagnostics")
