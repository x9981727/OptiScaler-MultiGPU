"""Apply the XeFG adapter-lifetime fix after v9; validate all anchors before writing."""
from pathlib import Path

kit = Path(__file__).resolve().parents[1]
root = kit / "upstream" / "OptiScaler"
changes = {}


def read(path):
    return (root / path).read_text(encoding="utf-8-sig")


def replace(text, old, new, count=1):
    actual = text.count(old)
    if actual != count:
        raise RuntimeError(f"v10 anchor count {actual}, expected {count}: {old[:120]}")
    return text.replace(old, new)


# Keep the guard in the shared entry point as well as removing XeFG's activation
# call, so a future call cannot destroy a runtime still owned by a live swapchain.
base_header = "framegen/IFGFeature_Dx12.h"
h = read(base_header)
h = replace(h, "    bool EnsureMultiGPURuntime(ID3D12Device* renderDevice);", """    virtual bool MultiGPUSelectionLocked() const { return false; }
    bool EnsureMultiGPURuntime(ID3D12Device* renderDevice);""")
h = replace(h, "    bool MultiGPUActive() const { return IsMultiGPUActive(); }", """    virtual bool MultiGPUAdapterChangePending() const { return false; }
    bool MultiGPUActive() const { return IsMultiGPUActive(); }""")
changes[base_header] = h

base_source = "framegen/IFGFeature_Dx12.cpp"
b = read(base_source)
b = replace(b, """bool IFGFeature_Dx12::EnsureMultiGPURuntime(ID3D12Device* renderDevice)
{
    auto config = Config::Instance();""", """bool IFGFeature_Dx12::EnsureMultiGPURuntime(ID3D12Device* renderDevice)
{
    // A live XeFG SDK swapchain must retain its device, queue and bridge runtime.
    // FSRFG keeps its existing context-recreation behavior.
    if (MultiGPUSelectionLocked())
    {
        LOG_DEBUG("MultiGPU v10: retaining runtime bound to the live XeFG swapchain");
        return IsMultiGPUActive();
    }

    auto config = Config::Instance();""")
changes[base_source] = b

xefg_header = "framegen/xefg/XeFG_Dx12.h"
h = read(xefg_header)
h = replace(h, '#include <framegen/IFGFeature_Dx12.h>', '#include <framegen/IFGFeature_Dx12.h>\n#include "AdapterSelectionBinding.h"')
h = replace(h, "    xefg_swapchain_handle_t _fgContext = nullptr;", """    xefg_swapchain_handle_t _fgContext = nullptr;
    MultiGPU::AdapterSelectionBinding _adapterBinding;""")
h = replace(h, "  protected:\n", """  protected:
    bool MultiGPUSelectionLocked() const override final { return _swapChainContext != nullptr; }
""")
h = replace(h, "    // IFGFeature_Dx12\n", """    // IFGFeature_Dx12
    bool MultiGPUAdapterChangePending() const override final;
""")
changes[xefg_header] = h

xefg_source = "framegen/xefg/XeFG_Dx12.cpp"
x = read(xefg_source)
x = replace(x, "using namespace DirectX;", """using namespace DirectX;

namespace
{
MultiGPU::AdapterSelection RequestedXeFGAdapter()
{
    auto config = Config::Instance();
    MultiGPU::AdapterSelection selection { config->FGMultiGPUEnabled.value_or_default(), std::nullopt };
    LUID luid {};
    if (selection.enabled && config->FGMultiGPUAdapterLuid.has_value() &&
        MultiGPU::StringToLuid(config->FGMultiGPUAdapterLuid.value(), luid))
    {
        selection.luid = (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32) |
                         static_cast<uint64_t>(luid.LowPart);
    }
    return selection;
}
} // namespace

bool XeFG_Dx12::MultiGPUAdapterChangePending() const
{
    return _adapterBinding.RequiresRestart(RequestedXeFGAdapter());
}""")
x = replace(x, '        LOG_INFO("XeFG context created");', """        _adapterBinding.Bind(RequestedXeFGAdapter());
        LOG_INFO("MultiGPU v10: XeFG adapter bound for swapchain lifetime; mode={}, device={:X}, LUID={}",
                 IsMultiGPUActive() ? "secondary GPU" : "game GPU", (size_t) device,
                 wstring_to_string(MultiGPU::LuidToString(device->GetAdapterLuid())));
        LOG_INFO("XeFG context created");""")
x = replace(x, """        else
        {
            if (XeLLProxy::Context() != nullptr)
                XeLLProxy::DestroyXeLLContext();""", """        else
        {
            // Frame-resource teardown does not reset this binding; SDK destruction does.
            _adapterBinding.Reset();
            if (XeLLProxy::Context() != nullptr)
                XeLLProxy::DestroyXeLLContext();""")
x = replace(x, """    _device = device;
    {
        ScopedMultiGpuXeFGInit isolateSetup(Config::Instance()->FGMultiGPUEnabled.value_or_default());
        EnsureMultiGPURuntime(device);
    }
    CreateObjects(device);""", """    _device = device;
    // CreateSwapchain/1 already chose the SDK context's adapter. Re-selecting here
    // would send secondary-device resources into a surviving primary-device context.
    if (_swapChainContext == nullptr)
        return;
    if (MultiGPUAdapterChangePending())
        LOG_WARN("MultiGPU v10: XeFG GPU change pending; Save Settings and restart the game");
    CreateObjects(device);""")
x = replace(x, """void XeFG_Dx12::Activate()
{
    LOG_DEBUG("");""", """void XeFG_Dx12::Activate()
{
    LOG_DEBUG("");

    // Also guard activation requested by game FG inputs or configuration reloads.
    if (MultiGPUAdapterChangePending())
        return;""")
changes[xefg_source] = x

menu = "menu/menu_common.cpp"
m = read(menu)
m = replace(m, """                    bool multiGpuEnabled = config->FGMultiGPUEnabled.value_or_default();""", """                    const bool xefgAdapterNeedsRestart = state.activeFgOutput == FGOutput::XeFG ||
                                                        config->FGOutput.value_or_default() == FGOutput::XeFG;
                    bool multiGpuEnabled = config->FGMultiGPUEnabled.value_or_default();""")
for old in [
    """                        config->FGMultiGPUEnabled = multiGpuEnabled;
                        state.FGchanged = true;
                        state.SCchanged = true;""",
    """                                    config->FGMultiGPUAdapterLuid = luidText;
                                    state.FGchanged = true;
                                    state.SCchanged = true;""",
]:
    lines = old.splitlines()
    indent = lines[1][:len(lines[1]) - len(lines[1].lstrip())]
    new = lines[0] + "\n" + indent + "if (!xefgAdapterNeedsRestart)\n" + indent + "{\n"
    new += "\n".join("    " + line for line in lines[1:]) + "\n" + indent + "}"
    m = replace(m, old, new)
m = replace(m, """                    auto static fgInputOverridden = false;""", """                    if (xefgAdapterNeedsRestart)
                    {
                        ImGui::TextDisabled("XeFG GPU changes require Save Settings and a game restart.");
                        if (state.activeFgOutput == FGOutput::XeFG && state.currentFG != nullptr)
                        {
                            auto runtime = state.currentFG->MultiGPURuntime();
                            if (state.currentFG->MultiGPUActive() && runtime != nullptr)
                                ImGui::Text("XeFG GPU this launch: %s",
                                            wstring_to_string(runtime->FGAdapterName()).c_str());
                            else
                                ImGui::Text("XeFG GPU this launch: Game/Render GPU");
                        }
                    }

                    const bool adapterChangePending = state.currentFG != nullptr &&
                                                      state.currentFG->MultiGPUAdapterChangePending();
                    auto static fgInputOverridden = false;""")
m = replace(m, """                                              state.activeFgInput != config->FGInput.value_or_default();""", """                                              state.activeFgInput != config->FGInput.value_or_default() ||
                                              adapterChangePending;""")
m = replace(m, """                        bool fgActive = config->FGEnabled.value_or_default();
                        if (ImGui::Checkbox("Active##3", &fgActive))""", """                        bool fgActive = config->FGEnabled.value_or_default();
                        const bool adapterPending = fgOutput && fgOutput->MultiGPUAdapterChangePending();
                        if (adapterPending)
                            ImGui::TextColored(toneMapColor(ImVec4(1.f, 0.65f, 0.f, 1.f)),
                                               "GPU change pending: Save Settings and restart before enabling XeFG.");
                        // An already active session can still be switched off.
                        ImGui::BeginDisabled(adapterPending && !fgActive);
                        if (ImGui::Checkbox("Active##3", &fgActive))""")
start = m.index('                        if (ImGui::Checkbox("Active##3", &fgActive))')
pos = m.index('                        ShowHelpMarker("Enable Frame Generation");', start)
m = m[:pos] + "                        ImGui::EndDisabled();\n\n" + m[pos:]
changes[menu] = m
changes["framegen/xefg/AdapterSelectionBinding.h"] = (kit / "v10/AdapterSelectionBinding.h").read_text(encoding="utf-8")

for path, content in changes.items():
    (root / path).write_text(content, encoding="utf-8")
print("v10 applied: XeFG swapchain adapter binding, guarded runtime lifetime, restart UI and activation guard")
