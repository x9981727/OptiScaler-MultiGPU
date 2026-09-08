from pathlib import Path
kit=Path(__file__).resolve().parents[1]
for p in [kit/'v26/GameOutputPacing-compiled.h',kit/'upstream/OptiScaler/framegen/xefg/GameOutputPacing.h']:
    text=p.read_text(encoding='utf-8')
    old='qdesc.Priority=D3D12_COMMAND_QUEUE_PRIORITY_HIGH;'
    if text.count(old)!=1:raise RuntimeError('queue priority anchor mismatch')
    # Native XeFG currently requests HIGH; normal queues in the WARP test remain
    # normal. Do not request unsupported realtime priority or alter global policy.
    text=text.replace(old,'qdesc.Priority=q->GetDesc().Priority;')
    p.write_text(text,encoding='utf-8')
project='''<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
<ItemGroup Label="ProjectConfigurations"><ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration></ItemGroup>
<PropertyGroup Label="Globals"><ProjectGuid>{37E9ABCB-FAAD-4782-BCF1-EEED626EC101}</ProjectGuid><WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion></PropertyGroup>
<Import Project="$(VCTargetsPath)\\Microsoft.Cpp.Default.props"/>
<PropertyGroup Label="Configuration"><ConfigurationType>Application</ConfigurationType><PlatformToolset>v143</PlatformToolset><UseDebugLibraries>false</UseDebugLibraries></PropertyGroup>
<Import Project="$(VCTargetsPath)\\Microsoft.Cpp.props"/>
<PropertyGroup><OutDir>$(ProjectDir)out\\</OutDir><IntDir>$(ProjectDir)obj\\</IntDir><TargetName>v26-lifecycle-test</TargetName></PropertyGroup>
<ItemDefinitionGroup><ClCompile><LanguageStandard>stdcpp17</LanguageStandard><RuntimeLibrary>MultiThreaded</RuntimeLibrary><ExceptionHandling>Sync</ExceptionHandling><WarningLevel>Level3</WarningLevel><PreprocessorDefinitions>NOMINMAX;WIN32_LEAN_AND_MEAN;%(PreprocessorDefinitions)</PreprocessorDefinitions></ClCompile><Link><AdditionalDependencies>d3d12.lib;dxgi.lib;dxguid.lib;user32.lib;%(AdditionalDependencies)</AdditionalDependencies><SubSystem>Console</SubSystem></Link></ItemDefinitionGroup>
<ItemGroup><ClCompile Include="lifecycle-test.cpp"/></ItemGroup><Import Project="$(VCTargetsPath)\\Microsoft.Cpp.targets"/>
</Project>'''
(kit/'v26/lifecycle-test.vcxproj').write_text(project,encoding='utf-8')
import json,hashlib
m=kit/'v26/INTEGRATION-MANIFEST.json';d=json.loads(m.read_text())
d['source_changes']['framegen/xefg/GameOutputPacing.h']=hashlib.sha256((kit/'v26/GameOutputPacing-compiled.h').read_bytes()).hexdigest()
d['display_priority']='inherited from SDK; no global or SDK priority override in RC1'
m.write_text(json.dumps(d,indent=2),encoding='utf-8')
