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
def out(args, **kw):
    return subprocess.check_output([str(x) for x in args], text=True, **kw).strip()
once('#include <algorithm>\n','#include <algorithm>\n#include <cmath>\n','cmath include')
once('std::min(w, std::max(64u, static_cast<UINT>(w * workingScale + 0.5f)))',
     '(std::min)(w, (std::max)(64u, static_cast<UINT>(w * workingScale + 0.5f)))','work width minmax')
once('std::min(h, std::max(64u, static_cast<UINT>(h * workingScale + 0.5f)))',
     '(std::min)(h, (std::max)(64u, static_cast<UINT>(h * workingScale + 0.5f)))','work height minmax')
once('uint2 q=min(uint2((uint64_t(p.x)*srcW+dstW/2)/dstW,(uint64_t(p.y)*srcH+dstH/2)/dstH),uint2(srcW-1,srcH-1));',
     'uint2 q=min(uint2((p.x*srcW+dstW/2)/dstW,(p.y*srcH+dstH/2)/dstH),uint2(srcW-1,srcH-1));','SM5 depth coordinate')
once('''            dispatchScale(0,p->scaleColourPipeline.Get(),p->workColour.Get());
            dispatchScale(4,p->scaleMotionPipeline.Get(),p->workMotion.Get());''',
'''            Barrier(cmd,f.colour,f.colourState,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            dispatchScale(0,p->scaleColourPipeline.Get(),p->workColour.Get());
            Barrier(cmd,f.colour,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,f.colourState);
            dispatchScale(4,p->scaleMotionPipeline.Get(),p->workMotion.Get());''','colour source state')
# The NR contract explicitly says the game-provided MV scale is already independent of resource
# resolution/subrect. The reduced motion texture advertises its own work extent, so multiplying the
# scale by work/base would count the raster reduction twice and under-reproject temporal history.
contract=(root/'OptiScaler/shaders/dlssnr/DlssNr_Common.h').read_text(encoding='utf-8').replace('\r\n','\n')
if 'scaling by the resolution ratio on\n    // top of that counts it twice' not in contract:
    raise RuntimeError('Pinned DLSS-NR MV scale pass-through contract changed; re-audit before building')
once('''            packet.scaleX = scaled ? f.motionScaleX * (float(workW) / float(w)) : f.motionScaleX;
            packet.scaleY = scaled ? f.motionScaleY * (float(workH) / float(h)) : f.motionScaleY;''',
'''            // Preserve the game-provided MV encoding scale. The reduced resource extent already
            // describes the smaller raster; applying work/base here would double-count that resize.
            packet.scaleX = f.motionScaleX;
            packet.scaleY = f.motionScaleY;''','MV scale pass-through')
cpp.write_text(s,encoding='utf-8',newline='\n')

# The fork snapshot omitted OptiScaler's committed binary build libraries. Use a full shallow clone
# here: partial blob-less clones failed to materialize the committed .lib blobs on GitHub's Windows
# runner. The full DLL link below remains the ABI compatibility gate.
fork_project=root/'OptiScaler/OptiScaler.vcxproj'
official=Path('D:/r7-optiscaler-build-deps')
if official.exists(): shutil.rmtree(official)
run(['git','clone','--depth=1','https://github.com/optiscaler/OptiScaler.git',official])
official_commit=out(['git','-C',official,'rev-parse','HEAD'])
source_library=official/'OptiScaler/library'
freetype_src=official/'external/freetype/freetype.lib'
if not source_library.exists(): raise RuntimeError('Official OptiScaler/library missing after full clone')
if not freetype_src.exists(): raise RuntimeError('Official freetype.lib missing after full clone')
target_library=root/'OptiScaler/library'
if target_library.exists(): shutil.rmtree(target_library)
shutil.copytree(source_library,target_library)
freetype_dst=root/'external/freetype/freetype.lib'
freetype_dst.parent.mkdir(parents=True,exist_ok=True)
shutil.copy2(freetype_src,freetype_dst)

# The solution emits the main DLL to x64/Release while the forwarder goes to x64/Release/a.
# r7 packaging intentionally expects a single directory. Change only the Release|x64 main-project
# OutDir so the successfully linked OptiScaler.dll lands beside nvngx.dll_dlssnr.dll.
project=fork_project.read_text(encoding='utf-8-sig')
release_property_pattern=r'(<PropertyGroup Condition="\'\$\(Configuration\)\|\$\(Platform\)\'==\'Release\|x64\'" Label="Configuration">.*?</PropertyGroup>)'
matches=re.findall(release_property_pattern,project,re.S)
if len(matches)!=1: raise RuntimeError(f'Expected one Release|x64 Configuration PropertyGroup, got {len(matches)}')
group=matches[0]
if '<OutDir>' in group: raise RuntimeError('Release|x64 Configuration group already defines OutDir; re-audit output path')
new_group=group.replace('</PropertyGroup>','    <OutDir>$(SolutionDir)x64\\Release\\a\\</OutDir>\n  </PropertyGroup>',1)
project=project.replace(group,new_group,1)
fork_project.write_text(project,encoding='utf-8-sig',newline='\n')

release_group=re.search(r'<ItemDefinitionGroup Condition="\'\$\(Configuration\)\|\$\(Platform\)\'==\'Release\|x64\'">(.*?)</ItemDefinitionGroup>',project,re.S)
if not release_group: raise RuntimeError('Release|x64 project group missing')
deps_match=re.search(r'<AdditionalDependencies>(.*?)</AdditionalDependencies>',release_group.group(1),re.S)
if not deps_match: raise RuntimeError('Release|x64 dependencies missing')
release_deps=[x.strip() for x in deps_match.group(1).split(';') if x.strip().lower().endswith('.lib')]
private_prefixes=('ffx_','freetype','vulkan-1')
private_deps=[x for x in release_deps if x.lower().startswith(private_prefixes)]
search_dirs=[target_library/'fsr2',target_library/'fsr2_212',target_library/'fsr31',target_library/'vulkan',freetype_dst.parent,root/'OptiScaler']
missing=[name for name in private_deps if not any((d/name).exists() for d in search_dirs)]
if missing:
    raise RuntimeError('Official build dependency hydration incomplete: '+', '.join(missing))

vulkan_candidates=[target_library/'vulkan/vulkan-1.lib',Path('C:/vcpkg/installed/x64-windows/lib/vulkan-1.lib'),Path('C:/VulkanSDK/Lib/vulkan-1.lib')]
vulkan_source=next((p for p in vulkan_candidates if p.exists()),None)
if vulkan_source is None:
    vcpkg=Path('C:/vcpkg/vcpkg.exe')
    if not vcpkg.exists(): raise RuntimeError('vulkan-1.lib missing and GitHub runner vcpkg.exe not found')
    run([vcpkg,'install','vulkan-loader:x64-windows','--disable-metrics'])
    vulkan_source=Path('C:/vcpkg/installed/x64-windows/lib/vulkan-1.lib')
    if not vulkan_source.exists(): raise RuntimeError('vcpkg vulkan-loader installed but vulkan-1.lib is still missing')
vulkan_target=root/'OptiScaler/vulkan-1.lib'
shutil.copy2(vulkan_source,vulkan_target)

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
        'full_colour_state_restored':True,'motion_scale_passthrough':True,
        'motion_scale_double_resize_avoided':True,'release_output_aligned_to_packaging_dir':True,
        'dependency_source':'official OptiScaler full shallow clone',
        'official_dependency_commit':official_commit,'private_link_dependencies_verified':private_deps,
        'freetype_sha256':hashlib.sha256(freetype_dst.read_bytes()).hexdigest(),
        'vulkan_import_lib_source':str(vulkan_source),
        'vulkan_import_lib_sha256':hashlib.sha256(vulkan_target.read_bytes()).hexdigest(),
        'full_link_is_abi_gate':True,'gpu_executed':False}
(root/'r7-fixup-report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(json.dumps(report,indent=2))
