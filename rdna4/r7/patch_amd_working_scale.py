# SPDX-License-Identifier: MIT
"""Patch the pinned upstream AMD pre-SR backend with an experimental WorkingScale path.

Design rule: scale=1 follows the existing backend.  For scale<1 the full-resolution
base colour is preserved; only a reduced neural edit is computed.  The reduced edit
(processed - pre-NR base) is bilinearly enlarged and added to the untouched full-res
base before SR.  This is a performance/quality experiment, not a Swin ABI rewrite.
"""
from pathlib import Path
import hashlib, json, sys

if len(sys.argv) != 2:
    raise SystemExit('usage: patch_amd_working_scale.py <OptiScaler-DLSSNR-PreSR-Multipass-main>')
root = Path(sys.argv[1]).resolve()
cpp = root/'OptiScaler/dlssnr/amd/AmdPreSr.cpp'
hdr = root/'OptiScaler/dlssnr/amd/AmdPreSr.h'
bridge = root/'OptiScaler/dlssnr/amd/AmdBridge.cpp'
expected = {
    cpp:'eed37e6139d112c1596739b827a1bb3a93ec57ed3cd9aa2dcd495cd637644357',
    hdr:'c95e100f28c36331844ed2ca4dc36e981dac66f990f6fc3d4abe1b2629282a5',
    bridge:'673de48503972971fba109e30fd1ac2732a31ac558b71343a5914820234490291',
}
# The GitHub blob SHA values above are SHA-1, so independently pin exact file content
# with the SHA-256 values generated below on first verified source.  To avoid trusting
# guessed hashes, require unique source contracts in addition to the upstream git pin.
for p in expected:
    if not p.exists(): raise RuntimeError(f'missing pinned source: {p}')

def text(p): return p.read_text(encoding='utf-8').replace('\r\n','\n')
def once(s, old, new, label):
    n=s.count(old)
    if n != 1: raise RuntimeError(f'{label}: expected one patch target, got {n}')
    return s.replace(old,new)

h=text(hdr)
h=once(h,
'''struct Settings
{
    UINT passes = 1;
    float tone = 0, structure = 1, skin = 1;
};''',
'''struct Settings
{
    UINT passes = 1;
    float tone = 0, structure = 1, skin = 1;
    // Experimental AMD-only neural working raster.  1 keeps the legacy path.
    // Values below 1 preserve the full-resolution base and resize only the NR edit.
    float workingScale = 1.0f;
};''','settings ABI')
hdr.write_text(h,encoding='utf-8',newline='\n')

b=text(bridge)
b=once(b,
'''    s.skin = cfg.DlssNrSkinStructure.value_or_default();
    if (s.skin < 0)
        s.skin = s.structure;''',
'''    s.skin = cfg.DlssNrSkinStructure.value_or_default();
    if (s.skin < 0)
        s.skin = s.structure;
    // Unlike the generic NGX backend, AMD pre-SR did not consume WorkingScale.
    // r7 explicitly wires it here; supersampling (>1) remains disabled until the
    // reduced path is validated on the target GPU and in games.
    s.workingScale = std::clamp(cfg.DlssNrWorkingScale.value_or_default(), 0.25f, 1.0f);''','bridge WorkingScale')
bridge.write_text(b,encoding='utf-8',newline='\n')

s=text(cpp)
shader_anchor='''constexpr char DepthShader[] = R"(
Texture2D<float> src : register(t0);
RWTexture2D<float> dst : register(u0);
cbuffer Extent : register(b0) { uint w; uint h; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x<w && p.y<h) dst[p.xy]=src.Load(int3(p.xy,0));
})";'''
shaders=shader_anchor+r'''
// r7 AMD WorkingScale helpers.  Manual filtering avoids sampler-state dependence.
// The base frame remains at full resolution; only the neural work raster is reduced.
constexpr char ScaleColourShader[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
cbuffer Extent : register(b0) { uint srcW; uint srcH; uint dstW; uint dstH; };
float4 sampleBilinear(float2 q) {
 float2 b=floor(q); float2 f=q-b;
 int2 p0=int2(clamp(b,float2(0,0),float2(srcW-1,srcH-1)));
 int2 p1=min(p0+int2(1,1),int2(srcW-1,srcH-1));
 float4 a=lerp(src.Load(int3(p0.x,p0.y,0)),src.Load(int3(p1.x,p0.y,0)),f.x);
 float4 c=lerp(src.Load(int3(p0.x,p1.y,0)),src.Load(int3(p1.x,p1.y,0)),f.x);
 return lerp(a,c,f.y);
}
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=dstW||p.y>=dstH) return;
 float2 q=(float2(p.xy)+0.5)*float2(srcW,srcH)/float2(dstW,dstH)-0.5;
 dst[p.xy]=sampleBilinear(q);
})";
constexpr char ScaleMotionShader[] = R"(
Texture2D<float2> src : register(t0);
RWTexture2D<float2> dst : register(u0);
cbuffer Extent : register(b0) { uint srcW; uint srcH; uint dstW; uint dstH; };
float2 sampleBilinear(float2 q) {
 float2 b=floor(q); float2 f=q-b;
 int2 p0=int2(clamp(b,float2(0,0),float2(srcW-1,srcH-1)));
 int2 p1=min(p0+int2(1,1),int2(srcW-1,srcH-1));
 float2 a=lerp(src.Load(int3(p0.x,p0.y,0)),src.Load(int3(p1.x,p0.y,0)),f.x);
 float2 c=lerp(src.Load(int3(p0.x,p1.y,0)),src.Load(int3(p1.x,p1.y,0)),f.x);
 return lerp(a,c,f.y);
}
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=dstW||p.y>=dstH) return;
 float2 q=(float2(p.xy)+0.5)*float2(srcW,srcH)/float2(dstW,dstH)-0.5;
 dst[p.xy]=sampleBilinear(q);
})";
constexpr char ScaleDepthShader[] = R"(
Texture2D<float> src : register(t0);
RWTexture2D<float> dst : register(u0);
cbuffer Extent : register(b0) { uint srcW; uint srcH; uint dstW; uint dstH; };
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=dstW||p.y>=dstH) return;
 uint2 q=min(uint2((uint64_t(p.x)*srcW+dstW/2)/dstW,(uint64_t(p.y)*srcH+dstH/2)/dstH),uint2(srcW-1,srcH-1));
 dst[p.xy]=src.Load(int3(q,0));
})";
constexpr char CompositeEditShader[] = R"(
Texture2D<float4> fullBase : register(t0);
Texture2D<float4> workProcessed : register(t1);
Texture2D<float4> workBase : register(t2);
RWTexture2D<float4> fullOut : register(u0);
cbuffer Extent : register(b0) { uint fullW; uint fullH; uint workW; uint workH; };
float4 sampleTex(Texture2D<float4> t,float2 q) {
 float2 b=floor(q); float2 f=q-b;
 int2 p0=int2(clamp(b,float2(0,0),float2(workW-1,workH-1)));
 int2 p1=min(p0+int2(1,1),int2(workW-1,workH-1));
 float4 a=lerp(t.Load(int3(p0.x,p0.y,0)),t.Load(int3(p1.x,p0.y,0)),f.x);
 float4 c=lerp(t.Load(int3(p0.x,p1.y,0)),t.Load(int3(p1.x,p1.y,0)),f.x);
 return lerp(a,c,f.y);
}
[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
 if(p.x>=fullW||p.y>=fullH) return;
 float2 q=(float2(p.xy)+0.5)*float2(workW,workH)/float2(fullW,fullH)-0.5;
 float4 base=fullBase.Load(int3(p.xy,0));
 float4 edit=sampleTex(workProcessed,q)-sampleTex(workBase,q);
 float4 v=base+edit; v.a=base.a; fullOut[p.xy]=v;
})";'''
s=once(s,shader_anchor,shaders,'scale shaders')

s=once(s,
'''    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;''',
'''    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;''','motion read formats')

s=once(s,
'''    ComPtr<ID3D12Resource> colour;
    ComPtr<ID3D12Resource> motionCrop, depthCrop;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12PipelineState> depthPipeline;''',
'''    ComPtr<ID3D12Resource> colour;
    ComPtr<ID3D12Resource> motionCrop, depthCrop;
    // r7 reduced neural raster. colour remains the full-resolution output/base.
    ComPtr<ID3D12Resource> workColour, workBase, workMotion, workDepth;
    ComPtr<ID3D12DescriptorHeap> heap, scaleHeap;
    ComPtr<ID3D12RootSignature> root, scaleRoot;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12PipelineState> depthPipeline;
    ComPtr<ID3D12PipelineState> scaleColourPipeline, scaleMotionPipeline, scaleDepthPipeline, compositePipeline;''','impl resources')
s=once(s,
'''    UINT width = 0, height = 0, activePasses = 0, lastPasses = 0;''',
'''    UINT width = 0, height = 0, workWidth = 0, workHeight = 0, activePasses = 0, lastPasses = 0;''','work dimensions')

init_tail='''        D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "Crop heap");'''
init_new=init_tail+r'''
        // Separate immutable descriptor sets for colour, motion, depth and final edit composition.
        D3D12_DESCRIPTOR_RANGE scaleRanges[2] {};
        scaleRanges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 };
        scaleRanges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3 };
        D3D12_ROOT_PARAMETER scaleParams[2] {};
        scaleParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        scaleParams[0].DescriptorTable = { 2, scaleRanges };
        scaleParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        scaleParams[1].Constants = { 0, 0, 4 };
        D3D12_ROOT_SIGNATURE_DESC scaleDesc { 2, scaleParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        Check(D3D12SerializeRootSignature(&scaleDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
              "Scale root signature serialize");
        Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&scaleRoot)),
              "Scale root signature create");
        auto makeScalePipeline = [&](const char* code, size_t bytes, const char* name, ComPtr<ID3D12PipelineState>& out)
        {
            ComPtr<ID3DBlob> shader, compileError;
            Check(D3DCompile(code, bytes, name, nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
                             0, &shader, &compileError), name);
            D3D12_COMPUTE_PIPELINE_STATE_DESC d {};
            d.pRootSignature = scaleRoot.Get();
            d.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
            Check(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&out)), name);
        };
        makeScalePipeline(ScaleColourShader, sizeof(ScaleColourShader), "AMD r7 colour scale", scaleColourPipeline);
        makeScalePipeline(ScaleMotionShader, sizeof(ScaleMotionShader), "AMD r7 motion scale", scaleMotionPipeline);
        makeScalePipeline(ScaleDepthShader, sizeof(ScaleDepthShader), "AMD r7 depth scale", scaleDepthPipeline);
        makeScalePipeline(CompositeEditShader, sizeof(CompositeEditShader), "AMD r7 edit composite", compositePipeline);
        hd.NumDescriptors = 16;
        Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&scaleHeap)), "AMD r7 scale heap");'''
s=once(s,init_tail,init_new,'scale shader initialization')

# Working extent is calculated after base layout validation.  Keep 1.0 exact and avoid tiny model extents.
anchor='''        if (f.motion->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
            throw std::runtime_error("Unsupported depth-stencil motion buffer: " + Layout(f.motion));
        p->activePasses = std::clamp(cfg.passes, 1u, 3u);'''
replacement='''        if (f.motion->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
            throw std::runtime_error("Unsupported depth-stencil motion buffer: " + Layout(f.motion));
        const float workingScale = std::clamp(std::isfinite(cfg.workingScale) ? cfg.workingScale : 1.0f, 0.25f, 1.0f);
        const UINT workW = workingScale >= 0.9999f ? w : std::min(w, std::max(64u, static_cast<UINT>(w * workingScale + 0.5f)));
        const UINT workH = workingScale >= 0.9999f ? h : std::min(h, std::max(64u, static_cast<UINT>(h * workingScale + 0.5f)));
        const bool scaled = workW != w || workH != h;
        const auto motionFormat = ReadFormat(f.motion->GetDesc().Format);
        if (scaled && motionFormat != DXGI_FORMAT_R16G16_FLOAT && motionFormat != DXGI_FORMAT_R32G32_FLOAT)
            throw std::runtime_error("AMD WorkingScale requires R16G16_FLOAT/R32G32_FLOAT motion; got " + Layout(f.motion));
        p->activePasses = std::clamp(cfg.passes, 1u, 3u);'''
s=once(s,anchor,replacement,'working extent')

s=once(s,
'''        bool resize = p->width != w || p->height != h;
        if (resize)''',
'''        bool resize = p->width != w || p->height != h;
        const bool workResize = p->workWidth != workW || p->workHeight != workH;
        if (resize)''','work resize')

# Allocate reduced resources after the legacy full-resolution output allocation.
alloc_anchor='''            p->width = w;
            p->height = h;
        }
        auto prepareGuide = [&](ID3D12Resource* source, ComPtr<ID3D12Resource>& crop)'''
alloc_new='''            p->width = w;
            p->height = h;
        }
        if (workResize)
        {
            p->workColour.Reset(); p->workBase.Reset(); p->workMotion.Reset(); p->workDepth.Reset();
            p->workWidth = workW; p->workHeight = workH;
        }
        if (scaled && (!p->workColour || !p->workBase || !p->workMotion || !p->workDepth))
        {
            D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            auto makeWork = [&](ComPtr<ID3D12Resource>& out, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                const char* what)
            {
                D3D12_RESOURCE_DESC rd {};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = workW; rd.Height = workH;
                rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = format; rd.SampleDesc.Count = 1; rd.Flags = flags;
                Check(p->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&out)), what);
            };
            makeWork(p->workColour, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                     "AMD r7 work colour");
            makeWork(p->workBase, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, "AMD r7 work base");
            makeWork(p->workMotion, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                     "AMD r7 work motion");
            makeWork(p->workDepth, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                     "AMD r7 work depth");
            p->Log("AMD WorkingScale=" + std::to_string(workingScale) + " base=" + std::to_string(w) + "x" +
                   std::to_string(h) + " work=" + std::to_string(workW) + "x" + std::to_string(workH));
        }
        auto prepareGuide = [&](ID3D12Resource* source, ComPtr<ID3D12Resource>& crop)'''
s=once(s,alloc_anchor,alloc_new,'work resource allocation')

# Split guide selection: legacy uses exact crops; scaled mode gets purpose-built reduced resources.
old='''        auto motion = prepareGuide(f.motion, p->motionCrop);
        ID3D12Resource* depth = nullptr;
        if (convertDepth)
        {
            if (!p->depthCrop || p->depthCrop->GetDesc().Width != w || p->depthCrop->GetDesc().Height != h ||
                p->depthCrop->GetDesc().Format != DXGI_FORMAT_R32_FLOAT ||
                !(p->depthCrop->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
            {
                p->depthCrop.Reset();
                auto rd = p->colour->GetDesc();
                rd.Format = DXGI_FORMAT_R32_FLOAT;
                D3D12_HEAP_PROPERTIES hp {};
                hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                Check(p->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                         IID_PPV_ARGS(&p->depthCrop)),
                      "Depth R32 texture");
            }
            depth = p->depthCrop.Get();
        }
        else
            depth = prepareGuide(f.depth, p->depthCrop);'''
new='''        auto motion = scaled ? p->workMotion.Get() : prepareGuide(f.motion, p->motionCrop);
        ID3D12Resource* depth = scaled ? p->workDepth.Get() : nullptr;
        if (!scaled)
        {
            if (convertDepth)
            {
                if (!p->depthCrop || p->depthCrop->GetDesc().Width != w || p->depthCrop->GetDesc().Height != h ||
                    p->depthCrop->GetDesc().Format != DXGI_FORMAT_R32_FLOAT ||
                    !(p->depthCrop->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
                {
                    p->depthCrop.Reset();
                    auto rd = p->colour->GetDesc(); rd.Format = DXGI_FORMAT_R32_FLOAT;
                    D3D12_HEAP_PROPERTIES hp {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                    Check(p->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&p->depthCrop)),
                        "Depth R32 texture");
                }
                depth = p->depthCrop.Get();
            }
            else depth = prepareGuide(f.depth, p->depthCrop);
        }'''
s=once(s,old,new,'guide selection')

# Existing depth descriptors/dispatch are only valid at 1:1.  Leave the legacy path untouched there.
s=s.replace('''        if (convertDepth)
        {
            // Distinct descriptor slots:''','''        if (!scaled && convertDepth)
        {
            // Distinct descriptor slots:''',1)
s=s.replace('''        if (convertDepth)
        {
            Barrier(cmd, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);''','''        if (!scaled && convertDepth)
        {
            Barrier(cmd, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);''',1)
s=s.replace('''        else
            copyGuide(f.depth, depth);
        UINT accepted = 0;''','''        else if (!scaled)
            copyGuide(f.depth, depth);

        // Reduced AMD path: derive colour/motion/depth at work resolution without touching the full base.
        if (scaled)
        {
            const UINT inc = p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto cpu0 = p->scaleHeap->GetCPUDescriptorHandleForHeapStart();
            auto gpu0 = p->scaleHeap->GetGPUDescriptorHandleForHeapStart();
            auto cpuAt = [&](UINT slot){ auto v=cpu0; v.ptr += SIZE_T(slot)*inc; return v; };
            auto gpuAt = [&](UINT slot){ auto v=gpu0; v.ptr += UINT64(slot)*inc; return v; };
            auto writeSrv = [&](UINT slot, ID3D12Resource* resource, DXGI_FORMAT format)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC v {}; v.Format=format; v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
                v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; v.Texture2D.MipLevels=1;
                p->device->CreateShaderResourceView(resource,&v,cpuAt(slot));
            };
            auto writeUav = [&](UINT slot, ID3D12Resource* resource, DXGI_FORMAT format)
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC v {}; v.Format=format; v.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
                p->device->CreateUnorderedAccessView(resource,nullptr,&v,cpuAt(slot));
            };
            auto setUnused = [&](UINT base, ID3D12Resource* r, DXGI_FORMAT f){writeSrv(base,r,f);writeSrv(base+1,r,f);writeSrv(base+2,r,f);};
            setUnused(0, f.colour, ReadFormat(desc.Format)); writeUav(3,p->workColour.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);
            setUnused(4, f.motion, motionFormat); writeUav(7,p->workMotion.Get(),DXGI_FORMAT_R16G16_FLOAT);
            setUnused(8, f.depth, DepthReadFormat(depthDesc.Format)); writeUav(11,p->workDepth.Get(),DXGI_FORMAT_R32_FLOAT);
            ID3D12DescriptorHeap* heaps[] { p->scaleHeap.Get() };
            cmd->SetDescriptorHeaps(1,heaps); cmd->SetComputeRootSignature(p->scaleRoot.Get());
            UINT dims4[] { w,h,workW,workH };
            auto dispatchScale = [&](UINT base, ID3D12PipelineState* ps, ID3D12Resource* target)
            {
                Barrier(cmd,target,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                cmd->SetPipelineState(ps); cmd->SetComputeRootDescriptorTable(0,gpuAt(base));
                cmd->SetComputeRoot32BitConstants(1,4,dims4,0); cmd->Dispatch((workW+7)/8,(workH+7)/8,1);
                Barrier(cmd,target,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            };
            dispatchScale(0,p->scaleColourPipeline.Get(),p->workColour.Get());
            dispatchScale(4,p->scaleMotionPipeline.Get(),p->workMotion.Get());
            dispatchScale(8,p->scaleDepthPipeline.Get(),p->workDepth.Get());
            Barrier(cmd,p->workColour.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(cmd,p->workBase.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyResource(p->workBase.Get(),p->workColour.Get());
            Barrier(cmd,p->workBase.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd,p->workColour.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        UINT accepted = 0;''',1)

# Reset temporal history whenever neural dimensions/scale change.
s=once(s,
'''        const bool settingsChanged = !p->haveSettings || cfg.tone != p->lastSettings.tone ||
                                     cfg.structure != p->lastSettings.structure || cfg.skin != p->lastSettings.skin;''',
'''        const bool settingsChanged = !p->haveSettings || cfg.tone != p->lastSettings.tone ||
                                     cfg.structure != p->lastSettings.structure || cfg.skin != p->lastSettings.skin ||
                                     cfg.workingScale != p->lastSettings.workingScale;''','history scale change')
s=s.replace('''            if (f.reset || resize || passChange || p->resetAfterTimeout || settingsChanged || explicitReset || gap)''',
'''            if (f.reset || resize || workResize || passChange || p->resetAfterTimeout || settingsChanged || explicitReset || gap)''',1)

# Feed reduced resources to HIP while preserving the legacy path at 1.0.
s=s.replace('''            packet.colour = p->colour.Get();''','''            packet.colour = scaled ? p->workColour.Get() : p->colour.Get();''',1)
s=s.replace('''            packet.scaleX = f.motionScaleX;
            packet.scaleY = f.motionScaleY;''','''            packet.scaleX = scaled ? f.motionScaleX * (float(workW) / float(w)) : f.motionScaleX;
            packet.scaleY = scaled ? f.motionScaleY * (float(workH) / float(h)) : f.motionScaleY;''',1)

# Add the reduced edit to an untouched full-resolution base after all private passes have been recorded.
composite_anchor='''        Barrier(cmd, f.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, f.motionState);
        Barrier(cmd, f.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, f.depthState);'''
composite_new='''        if (scaled && accepted && !p->failed)
        {
            const UINT inc = p->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto cpu=p->scaleHeap->GetCPUDescriptorHandleForHeapStart(); auto gpu=p->scaleHeap->GetGPUDescriptorHandleForHeapStart();
            cpu.ptr += SIZE_T(12)*inc; gpu.ptr += UINT64(12)*inc;
            D3D12_SHADER_RESOURCE_VIEW_DESC sv {}; sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels=1;
            sv.Format=ReadFormat(desc.Format); p->device->CreateShaderResourceView(f.colour,&sv,cpu); cpu.ptr+=inc;
            sv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; p->device->CreateShaderResourceView(p->workColour.Get(),&sv,cpu); cpu.ptr+=inc;
            p->device->CreateShaderResourceView(p->workBase.Get(),&sv,cpu); cpu.ptr+=inc;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uv {}; uv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; uv.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            p->device->CreateUnorderedAccessView(p->colour.Get(),nullptr,&uv,cpu);
            Barrier(cmd,f.colour,f.colourState,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd,p->colour.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D12DescriptorHeap* heaps[] { p->scaleHeap.Get() }; cmd->SetDescriptorHeaps(1,heaps);
            cmd->SetComputeRootSignature(p->scaleRoot.Get()); cmd->SetPipelineState(p->compositePipeline.Get());
            cmd->SetComputeRootDescriptorTable(0,gpu); UINT cdim[] { w,h,workW,workH };
            cmd->SetComputeRoot32BitConstants(1,4,cdim,0); cmd->Dispatch((w+7)/8,(h+7)/8,1);
            Barrier(cmd,p->colour.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(cmd,f.colour,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,f.colourState);
        }
        Barrier(cmd, f.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, f.motionState);
        Barrier(cmd, f.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, f.depthState);'''
s=once(s,composite_anchor,composite_new,'edit composite')

# More useful runtime log, while retaining base dimensions for compatibility.
s=s.replace('''            p->Log("Recorded pre-SR " + std::to_string(w) + "x" + std::to_string(h) +
                   " passes=" + std::to_string(p->activePasses));''',
'''            p->Log("Recorded pre-SR base=" + std::to_string(w) + "x" + std::to_string(h) +
                   " work=" + std::to_string(workW) + "x" + std::to_string(workH) +
                   " scale=" + std::to_string(workingScale) + " passes=" + std::to_string(p->activePasses));''',1)
s=s.replace('''        auto completed = "Completed AMD pre-SR passes=" + std::to_string(p->activePasses) + " at " +
                         std::to_string(p->width) + "x" + std::to_string(p->height);''',
'''        auto completed = "Completed AMD pre-SR passes=" + std::to_string(p->activePasses) + " base=" +
                         std::to_string(p->width) + "x" + std::to_string(p->height) + " work=" +
                         std::to_string(p->workWidth) + "x" + std::to_string(p->workHeight);''',1)
cpp.write_text(s,encoding='utf-8',newline='\n')

# Static build-time contract checks: no false claim of GPU validation.
checks = {
    'amd_header_sha256': hashlib.sha256(hdr.read_bytes()).hexdigest(),
    'amd_bridge_sha256': hashlib.sha256(bridge.read_bytes()).hexdigest(),
    'amd_backend_sha256': hashlib.sha256(cpp.read_bytes()).hexdigest(),
    'scale1_legacy_branch_present': 'scaled ? p->workColour.Get() : p->colour.Get()' in s,
    'full_base_residual_composite_present': 'workProcessed' in s and 'workBase' in s and 'base+edit' in s,
    'motion_scale_adjusted': 'float(workW) / float(w)' in s,
    'history_reset_on_work_resize': 'workResize || passChange' in s,
    'gpu_executed': False,
    'game_tested': False,
}
if not all(v for k,v in checks.items() if k not in ('gpu_executed','game_tested','amd_header_sha256','amd_bridge_sha256','amd_backend_sha256')):
    raise RuntimeError('r7 static contract check failed')
(root/'r7-patch-report.json').write_text(json.dumps(checks,indent=2)+'\n',encoding='utf-8')
print(json.dumps(checks,indent=2))
