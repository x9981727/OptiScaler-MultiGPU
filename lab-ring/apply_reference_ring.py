"""Isolated Intel reference experiment, not a game patch or hardware acceptance.
Apply only to intel/xess 207b703ad215da5b86dde04819a16277a96980aa.
The stock sample globally drains the queue every frame. This variant retires only
reused slots, while preserving native Sleep, frame IDs, timing hints and Present.
"""
from pathlib import Path
import sys, hashlib, json
root=Path(sys.argv[1])/"samples/basic_sample_frame_generation"
cpp_path=root/"basic_sample.cpp"; h_path=root/"basic_sample.h"
cpp=cpp_path.read_text(encoding="utf-8-sig"); header=h_path.read_text(encoding="utf-8-sig")
before={str(p.name):hashlib.sha256(p.read_bytes()).hexdigest() for p in (cpp_path,h_path)}
def replace_once(text,old,new):
    if text.count(old)!=1: raise RuntimeError("Unexpected source anchor: "+old[:100])
    return text.replace(old,new,1)
header=replace_once(header,"    ComPtr<ID3D12CommandAllocator> m_commandAllocator;","""    ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    ComPtr<ID3D12CommandAllocator> m_slotAllocators[FrameCount];
    UINT64 m_slotFences[FrameCount] = {};
    static const UINT CBVStart = AppDescriptorCount - FrameCount;
    static_assert(CBVStart > DescriptorsPerFrame * FrameCount, "Descriptor ranges overlap");""")
header=replace_once(header,"    void WaitForPreviousFrame();","    void WaitForPreviousFrame();\n    void AdvanceFrameSlot();")
old="    ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator)));"
cpp=replace_once(cpp,old,"""    // Each in-flight slot owns its command memory; resets require its fence.
    for (UINT i = 0; i < FrameCount; ++i)
    {
        m_slotAllocators[i].Reset();
        ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&m_slotAllocators[i])));
        m_slotFences[i] = 0;
    }
    m_commandAllocator = m_slotAllocators[m_backBufferIndex];""")
cpp=replace_once(cpp,"CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize);","CD3DX12_RESOURCE_DESC::Buffer(static_cast<UINT64>(constantBufferSize) * FrameCount);")
old="""        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        cbvDesc.BufferLocation = m_constantBuffer->GetGPUVirtualAddress();
        cbvDesc.SizeInBytes = constantBufferSize;
        m_device->CreateConstantBufferView(&cbvDesc, m_appDescriptorHeap->GetCPUDescriptorHandleForHeapStart());"""
cpp=replace_once(cpp,old,"""        // CPU writes cannot alter a constant buffer still consumed by the GPU.
        for (UINT i = 0; i < FrameCount; ++i)
        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
            cbvDesc.BufferLocation = m_constantBuffer->GetGPUVirtualAddress() +
                                     static_cast<UINT64>(i) * constantBufferSize;
            cbvDesc.SizeInBytes = constantBufferSize;
            CD3DX12_CPU_DESCRIPTOR_HANDLE handle(
                m_appDescriptorHeap->GetCPUDescriptorHandleForHeapStart(),
                static_cast<INT>(CBVStart + i), m_uavDescriptorSize);
            m_device->CreateConstantBufferView(&cbvDesc, handle);
        }""")
old="memcpy(m_pCbvDataBegin, &m_constantBufferData, sizeof(m_constantBufferData));"
if cpp.count(old)!=2: raise RuntimeError("Unexpected constant-buffer writes")
cpp=cpp.replace(old,"memcpy(m_pCbvDataBegin + sizeof(SceneConstantBuffer) * m_backBufferIndex,\n               &m_constantBufferData, sizeof(m_constantBufferData));")
old="""        m_commandList->SetGraphicsRootDescriptorTable(
            0, m_appDescriptorHeap->GetGPUDescriptorHandleForHeapStart());"""
cpp=replace_once(cpp,old,"""        CD3DX12_GPU_DESCRIPTOR_HANDLE cbvHandle(
            m_appDescriptorHeap->GetGPUDescriptorHandleForHeapStart(),
            static_cast<INT>(CBVStart + m_backBufferIndex), m_uavDescriptorSize);
        m_commandList->SetGraphicsRootDescriptorTable(0, cbvHandle);""")
# ONLY_NOW creates SDK-owned input data ordered on the application's queue.
cpp=replace_once(cpp,"hudlessColor.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;",
                 "hudlessColor.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;")
cpp=replace_once(cpp,"xefgSwapChainD3D12TagFrameResource(m_xefgSwapChain, nullptr, m_frameCounter, &hudlessColor);",
                 "ThrowIfFailed(xefgSwapChainD3D12TagFrameResource(m_xefgSwapChain, m_commandList.Get(), m_frameCounter, &hudlessColor), \"Color snapshot tag failed\");")
cpp=replace_once(cpp,"    ++m_frameCounter;\n    WaitForPreviousFrame();",
                 "    ++m_frameCounter;\n    AdvanceFrameSlot();")
old="void BasicSample::WaitForPreviousFrame()\n{"
cpp=replace_once(cpp,old,"""void BasicSample::AdvanceFrameSlot()
{
    const UINT previous = m_backBufferIndex;
    const UINT64 submitted = m_fenceValue++;
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), submitted));
    m_slotFences[previous] = submitted;
    m_backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
    if (m_backBufferIndex >= FrameCount)
        throw std::runtime_error("Invalid swapchain slot index");
    const UINT64 required = m_slotFences[m_backBufferIndex];
    const UINT64 completed = m_fence->GetCompletedValue();
    if (completed == static_cast<UINT64>(-1))
        throw std::runtime_error("Device removed while retiring frame slot");
    if (required != 0 && completed < required)
        ThrowIfFailed(m_fence->SetEventOnCompletion(required, nullptr));
    m_commandAllocator = m_slotAllocators[m_backBufferIndex];
}

void BasicSample::WaitForPreviousFrame()
{""")
# Setup, shutdown and resize still fully drain before resources are replaced.
old="    WaitForExec();\n\n    m_backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();"
cpp=replace_once(cpp,old,old+"\n    m_commandAllocator = m_slotAllocators[m_backBufferIndex];")
for marker in ("xellSleep(m_xellContext, m_frameCounter);",
               "constData.frameRenderTime = m_lastFrameTimeMS;",
               "m_swapChain->Present(syncInterval, presentFlags)"):
    if cpp.count(marker)!=1: raise RuntimeError("Native scheduling contract changed: "+marker)
cpp_path.write_text(cpp,encoding="utf-8"); h_path.write_text(header,encoding="utf-8")
manifest={"kind":"reference_ring_experiment_not_game_release","base_commit":"207b703ad215da5b86dde04819a16277a96980aa",
          "before_sha256":before,"after_sha256":{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (cpp_path,h_path)},
          "slots":4,"native_xell_sleep_preserved":True,"native_frame_time_preserved":True,
          "global_drains_preserved_for_lifecycle":True,"shader_or_resolution_changes":False,
          "game_acceptance":False}
(root/"RING-MANIFEST.json").write_text(json.dumps(manifest,indent=2),encoding="utf-8")
print(json.dumps(manifest))
