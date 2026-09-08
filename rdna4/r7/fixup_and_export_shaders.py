# SPDX-License-Identifier: MIT
from pathlib import Path
import hashlib, json, re, sys, shutil, subprocess
if len(sys.argv)!=2: raise SystemExit('usage: fixup_and_export_shaders.py <OptiScaler root>')
root=Path(sys.argv[1]).resolve(); cpp=root/'OptiScaler/dlssnr/amd/AmdPreSr.cpp'
s=cpp.read_text(encoding='utf-8').replace('\r\n','\n')
def once(old,new,label):
    global s
    n=s.count(old)
    if n!=1: raise RuntimeError(f'{label}: expected one target, got {n}')
    s=s.replace(old,new)
once('#include <algorithm>\n','#include <algorithm>\n#include <cmath>\n','cmath include')
# Windows headers may define function-like min/max macros. Parenthesized std functions are macro-proof.
once('std::min(w, std::max(64u, static_cast<UINT>(w * workingScale + 0.5f)))',
     '(std::min)(w, (std::max)(64u, static_cast<UINT>(w * workingScale + 0.5f)))','work width minmax')
once('std::min(h, std::max(64u, static_cast<UINT>(h * workingScale + 0.5f)))',
     '(std::min)(h, (std::max)(64u, static_cast<UINT>(h * workingScale + 0.5f)))','work height minmax')
# Shader model 5 has no portable uint64_t requirement here; 8K-class products are safely 32-bit.
once('uint2 q=min(uint2((uint64_t(p.x)*srcW+dstW/2)/dstW,(uint64_t(p.y)*srcH+dstH/2)/dstH),uint2(srcW-1,srcH-1));',
     'uint2 q=min(uint2((p.x*srcW+dstW/2)/dstW,(p.y*srcH+dstH/2)/dstH),uint2(srcW-1,srcH-1));','SM5 depth coordinate')
# The legacy full-colour copy restores the game resource state before the scale dispatch.
# Re-enter SRV state only around the reduced colour read, then restore it immediately.
once('''            dispatchScale(0,p->scaleColourPipeline.Get(),p->workColour.Get());
            dispatchScale(4,p->scaleMotionPipeline.Get(),p->workMotion.Get());''',
'''            Barrier(cmd,f.colour,f.colourState,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            dispatchScale(0,p->scaleColourPipeline.Get(),p->workColour.Get());
            Barrier(cmd,f.colour,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,f.colourState);
            dispatchScale(4,p->scaleMotionPipeline.Get(),p->workMotion.Get());''','colour source state')
cpp.write_text(s,encoding='utf-8',newline='\n')

# CI dependency only: OptiScaler links against vulkan-1.lib even though this r7 test targets D3D12/AMD.
# Prefer a preinstalled import library. If the GitHub Windows image lacks it, use its bundled vcpkg
# to install the official Vulkan loader and copy only the import library beside the vcxproj so MSBuild
# resolves the existing link dependency without altering runtime/game code.
vulkan_target=root/'OptiScaler/vulkan-1.lib'
vulkan_source=None
for candidate in (
    Path('C:/vcpkg/installed/x64-windows/lib/vulkan-1.lib'),
    Path('C:/VulkanSDK/Lib/vulkan-1.lib'),
):
    if candidate.exists():
        vulkan_source=candidate
        break
if vulkan_source is None:
    sdk_roots=Path('C:/VulkanSDK')
    if sdk_roots.exists():
        found=list(sdk_roots.glob('*/Lib/vulkan-1.lib'))
        if found: vulkan_source=sorted(found)[-1]
if vulkan_source is None:
    vcpkg=Path('C:/vcpkg/vcpkg.exe')
    if not vcpkg.exists():
        raise RuntimeError('vulkan-1.lib missing and GitHub runner vcpkg.exe not found')
    subprocess.run([str(vcpkg),'install','vulkan-loader:x64-windows','--disable-metrics'],check=True)
    candidate=Path('C:/vcpkg/installed/x64-windows/lib/vulkan-1.lib')
    if not candidate.exists():
        raise RuntimeError('vcpkg vulkan-loader installed but vulkan-1.lib is still missing')
    vulkan_source=candidate
shutil.copy2(vulkan_source,vulkan_target)

# Export the exact embedded shaders so CI can compile them with the Windows SDK compiler.
out=root/'r7-shaders';out.mkdir(exist_ok=True)
exports={}
for name in ('ScaleColourShader','ScaleMotionShader','ScaleDepthShader','CompositeEditShader'):
    m=re.search(r'constexpr char '+name+r'\[\] = R"\((.*?)\)";',s,re.S)
    if not m: raise RuntimeError('embedded shader missing: '+name)
    code=m.group(1).lstrip('\n')
    path=out/(name+'.hlsl');path.write_text(code,encoding='utf-8',newline='\n')
    exports[name]={'sha256':hashlib.sha256(code.encode()).hexdigest(),'bytes':len(code.encode())}
report={'backend_sha256':hashlib.sha256(cpp.read_bytes()).hexdigest(),'shaders':exports,
        'windows_minmax_macro_avoided':True,'sm5_uint64_dependency_removed':True,
        'full_colour_state_restored':True,'vulkan_import_lib_source':str(vulkan_source),
        'vulkan_import_lib_sha256':hashlib.sha256(vulkan_target.read_bytes()).hexdigest(),
        'gpu_executed':False}
(root/'r7-fixup-report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(json.dumps(report,indent=2))
