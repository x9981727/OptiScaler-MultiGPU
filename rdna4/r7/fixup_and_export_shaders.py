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
def run(args, **kw):
    subprocess.run([str(x) for x in args], check=True, **kw)
def out(args):
    return subprocess.check_output([str(x) for x in args], text=True).strip()
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

# The fork snapshot omitted OptiScaler's committed binary build libraries.  Do NOT mix arbitrary
# current binaries with the pinned source: locate the exact historical OptiScaler commit whose
# OptiScaler.vcxproj Git blob is byte-identical to the fork snapshot, then hydrate only the build
# libraries and freetype.lib from that exact commit. This mirrors official CI's complete checkout.
expected_project_blob='08359e44cbabcf199f29d7b7d15bc662ddc0acf4'
official=Path('D:/r7-optiscaler-build-deps')
if official.exists(): shutil.rmtree(official)
run(['git','clone','--filter=blob:none','--no-checkout','https://github.com/optiscaler/OptiScaler.git',official])
matching=out(['git','-C',official,'log','--all','--find-object='+expected_project_blob,'--format=%H','-1'])
if not matching:
    raise RuntimeError('Could not find exact upstream OptiScaler build-dependency commit for project blob '+expected_project_blob)
actual_blob=out(['git','-C',official,'rev-parse',matching+':OptiScaler/OptiScaler.vcxproj'])
if actual_blob != expected_project_blob:
    raise RuntimeError(f'Exact dependency commit verification failed: {actual_blob}')
run(['git','-C',official,'checkout',matching,'--','OptiScaler/library','external/freetype/freetype.lib'])
source_library=official/'OptiScaler/library'
target_library=root/'OptiScaler/library'
if target_library.exists(): shutil.rmtree(target_library)
shutil.copytree(source_library,target_library)
freetype_src=official/'external/freetype/freetype.lib'
freetype_dst=root/'external/freetype/freetype.lib'
shutil.copy2(freetype_src,freetype_dst)
# Contract: every release library named by the pinned project must now resolve in one of its LibraryPath dirs.
project=(root/'OptiScaler/OptiScaler.vcxproj').read_text(encoding='utf-8-sig')
release_group=re.search(r'<ItemDefinitionGroup Condition="\'\$\(Configuration\)\|\$\(Platform\)\'==\'Release\|x64\'">(.*?)</ItemDefinitionGroup>',project,re.S)
if not release_group: raise RuntimeError('Release|x64 project group missing')
deps_match=re.search(r'<AdditionalDependencies>(.*?)</AdditionalDependencies>',release_group.group(1),re.S)
if not deps_match: raise RuntimeError('Release|x64 dependencies missing')
release_deps=[x.strip() for x in deps_match.group(1).split(';') if x.strip().lower().endswith('.lib')]
# Windows SDK/system libs are intentionally excluded; verify the private/build-time dependency families.
private_prefixes=('ffx_','freetype','vulkan-1')
private_deps=[x for x in release_deps if x.lower().startswith(private_prefixes)]
search_dirs=[target_library/'fsr2',target_library/'fsr2_212',target_library/'fsr31',target_library/'vulkan',freetype_dst.parent,root/'OptiScaler']
missing=[]
for name in private_deps:
    if not any((d/name).exists() for d in search_dirs): missing.append(name)
if missing:
    raise RuntimeError('Exact build dependency hydration incomplete: '+', '.join(missing))

# CI fallback/verification for Vulkan: exact historical library should normally resolve from library/vulkan.
# If that historical tree lacks it, use the official Khronos loader from the GitHub runner's vcpkg.
vulkan_candidates=[target_library/'vulkan/vulkan-1.lib',Path('C:/vcpkg/installed/x64-windows/lib/vulkan-1.lib'),Path('C:/VulkanSDK/Lib/vulkan-1.lib')]
vulkan_source=next((p for p in vulkan_candidates if p.exists()),None)
if vulkan_source is None:
    vcpkg=Path('C:/vcpkg/vcpkg.exe')
    if not vcpkg.exists(): raise RuntimeError('vulkan-1.lib missing and GitHub runner vcpkg.exe not found')
    run([vcpkg,'install','vulkan-loader:x64-windows','--disable-metrics'])
    vulkan_source=Path('C:/vcpkg/installed/x64-windows/lib/vulkan-1.lib')
    if not vulkan_source.exists(): raise RuntimeError('vcpkg vulkan-loader installed but vulkan-1.lib is still missing')
# Copy beside vcxproj as a last-resort direct linker search location; exact project LibraryPath still wins.
vulkan_target=root/'OptiScaler/vulkan-1.lib'
shutil.copy2(vulkan_source,vulkan_target)

# Export the exact embedded shaders so CI can compile them with the Windows SDK compiler.
outdir=root/'r7-shaders';outdir.mkdir(exist_ok=True)
exports={}
for name in ('ScaleColourShader','ScaleMotionShader','ScaleDepthShader','CompositeEditShader'):
    m=re.search(r'constexpr char '+name+r'\[\] = R"\((.*?)\)";',s,re.S)
    if not m: raise RuntimeError('embedded shader missing: '+name)
    code=m.group(1).lstrip('\n')
    path=outdir/(name+'.hlsl');path.write_text(code,encoding='utf-8',newline='\n')
    exports[name]={'sha256':hashlib.sha256(code.encode()).hexdigest(),'bytes':len(code.encode())}
report={'backend_sha256':hashlib.sha256(cpp.read_bytes()).hexdigest(),'shaders':exports,
        'windows_minmax_macro_avoided':True,'sm5_uint64_dependency_removed':True,
        'full_colour_state_restored':True,'exact_optiscaler_dependency_commit':matching,
        'exact_project_blob':actual_blob,'private_link_dependencies_verified':private_deps,
        'freetype_sha256':hashlib.sha256(freetype_dst.read_bytes()).hexdigest(),
        'vulkan_import_lib_source':str(vulkan_source),
        'vulkan_import_lib_sha256':hashlib.sha256(vulkan_target.read_bytes()).hexdigest(),
        'gpu_executed':False}
(root/'r7-fixup-report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(json.dumps(report,indent=2))
