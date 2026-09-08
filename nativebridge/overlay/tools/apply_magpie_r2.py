#!/usr/bin/env python3
"""Apply the version-locked NativeBridge r2 Magpie runtime integration.
Run after apply_magpie.py has installed the r1 native-guidance contract patch.
Default is check-only; --apply writes changes and creates a separate backup.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE_COMMIT = "ac1cc8b0f2efc78323898395cc1336bcbecdc276"
EXPECTED = {
    "src/Magpie.Core/FrameSourceBase.h": "d5a744acfb358b2612448ee7142edd06a39aebec",
    "src/Magpie.Core/FrameSourceBase.cpp": "78869e8a0d570a5c274eefacbb89d658abed190a",
    "src/Magpie.Core/Renderer.cpp": "4ed46e1935c427a963d05a453826e30ca93e7298",
    # Post-r1 project hash: r1 adds contract.hpp + NativeBridgeContract.cpp only.
    "src/Magpie.Core/Magpie.Core.vcxproj": "6d739abd02ef9fc1f140f2897a4fa3682798d094",
    "src/Magpie/Magpie.vcxproj": "1bd8fbf90bf606a13d2a2f179288b2f04c8e55e6",
}

def git_blob(text: str) -> str:
    data = text.encode("utf-8")
    return hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()

def once(text: str, old: str, new: str) -> str:
    count = text.count(old)
    if count != 1:
        raise ValueError(f"expected exactly one anchor, found {count}: {old[:100]!r}")
    return text.replace(old, new, 1)

def patch_frame_source_h(s: str) -> str:
    s = once(s, '#pragma once\n', '#pragma once\n#include "FrameGuidanceTypes.h"\n')
    return once(s,
        '\tvirtual HANDLE FrameArrivedEvent() const noexcept { return nullptr; }',
        '''\tvirtual HANDLE FrameArrivedEvent() const noexcept { return nullptr; }
\t// NativeBridge packets carry exact same-frame depth/motion/camera metadata.
\tvirtual bool UsesNativeGuidance() const noexcept { return false; }
\tvirtual bool GetNativeGuidance(FrameGuidanceFrameId, FrameGuidanceView&) const noexcept { return false; }''')

def patch_frame_source_cpp(s: str) -> str:
    return once(s, '\tconst FrameSourceState state = _Update();', '''\tconst FrameSourceState state = _Update();
\t// Pixel equality is not frame identity for a native paired stream: depth,
\t// motion or camera can change while color remains byte-identical.
\tif (UsesNativeGuidance()) return state;''')

def patch_renderer(s: str) -> str:
    s = once(s, '#include "GraphicsCaptureFrameSource.h"',
        '#include "GraphicsCaptureFrameSource.h"\n#include "NativeBridgeFrameSource.h"')
    s = once(s, '''bool Renderer::_InitFrameSource() noexcept {
\tswitch (ScalingWindow::Get().Options().captureMethod) {''', '''bool Renderer::_InitFrameSource() noexcept {
\t// Explicit opt-in. The game-side exporter must already be loaded in the
\t// selected source process; otherwise initialization fails closed.
\tif (GetEnvironmentVariableW(L"MAGPIE_NATIVE_BRIDGE", nullptr, 0) > 0) {
\t\t_frameSource = std::make_unique<NativeBridgeFrameSource>();
\t} else switch (ScalingWindow::Get().Options().captureMethod) {''')
    s = once(s, '''\t_frameSource->ForceDuplicateFrameDetection(forceDuplicateFrameDetection);
\tif (forceDuplicateFrameDetection) {''', '''\tif (!_frameSource->UsesNativeGuidance())
\t\t_frameSource->ForceDuplicateFrameDetection(forceDuplicateFrameDetection);
\tif (forceDuplicateFrameDetection && !_frameSource->UsesNativeGuidance()) {''')
    s = once(s, '''\tif (guidanceRequirements.Any()) {
\t\tguidanceRequirements.ForEachMotion([&]([[maybe_unused]] MotionVectorRequest request) {''', '''\tif (guidanceRequirements.Any()) {
\t\tconst bool nativeGuidance = _frameSource->UsesNativeGuidance();
\t\tif (!nativeGuidance) guidanceRequirements.ForEachMotion([&]([[maybe_unused]] MotionVectorRequest request) {''')
    s = once(s, '''\t\t});
\t\tif (!_frameGuidanceService.Initialize(
\t\t\t_backendResources, _frameSource->GetOutput(), guidanceRequirements)) {''', '''\t\t});
\t\tconst FrameGuidanceRequirements initRequirements = nativeGuidance ?
\t\t\tFrameGuidanceRequirements{ .zero = true } : guidanceRequirements;
\t\tif (!_frameGuidanceService.Initialize(
\t\t\t_backendResources, _frameSource->GetOutput(), initRequirements)) {''')
    s = once(s, '''\t\tif (_capturedFrameId != 0 && !_frameGuidanceService.BeginFrame(
\t\t\t_capturedFrameId, inOutTexture, guidanceRequirements
\t\t).IsValidFor(_capturedFrameId, sourceExtent)) {
\t\t\tLogger::Get().Error("Produce Frame Guidance after resize failed");
\t\t\treturn nullptr;
\t\t}''', '''\t\tif (_capturedFrameId != 0) {
\t\t\tbool valid = false;
\t\t\tif (_frameSource->UsesNativeGuidance()) {
\t\t\t\tFrameGuidanceView nativeView{};
\t\t\t\tvalid = _frameSource->GetNativeGuidance(_capturedFrameId, nativeView) &&
\t\t\t\t\t_frameGuidanceService.BeginNativeFrame(
\t\t\t\t\t\t_capturedFrameId, inOutTexture, nativeView);
\t\t\t} else {
\t\t\t\tvalid = _frameGuidanceService.BeginFrame(
\t\t\t\t\t_capturedFrameId, inOutTexture, guidanceRequirements
\t\t\t\t).IsValidFor(_capturedFrameId, sourceExtent);
\t\t\t}
\t\t\tif (!valid) {
\t\t\t\tLogger::Get().Error("Produce Frame Guidance after resize failed");
\t\t\t\treturn nullptr;
\t\t\t}
\t\t}''')
    s = once(s, '''\t\tif (_frameGuidanceService.IsInitialized()) {
\t\t\tFrameTrace::Scope traceGuidance(FrameTrace::Event::Guidance);
\t\t\t_frameGuidanceService.BeginFrame(
\t\t\t\t_capturedFrameId, _frameSource->GetOutput(), guidanceRequirements);
\t\t}''', '''\t\tif (_frameGuidanceService.IsInitialized()) {
\t\t\tFrameTrace::Scope traceGuidance(FrameTrace::Event::Guidance);
\t\t\tif (_frameSource->UsesNativeGuidance()) {
\t\t\t\tFrameGuidanceView nativeView{};
\t\t\t\tif (!_frameSource->GetNativeGuidance(_capturedFrameId, nativeView) ||
\t\t\t\t\t!_frameGuidanceService.BeginNativeFrame(
\t\t\t\t\t\t_capturedFrameId, _frameSource->GetOutput(), nativeView)) {
\t\t\t\t\tLogger::Get().Error("NativeBridge same-frame guidance rejected; dropping frame");
\t\t\t\t\treturn;
\t\t\t\t}
\t\t\t} else {
\t\t\t\t_frameGuidanceService.BeginFrame(
\t\t\t\t\t_capturedFrameId, _frameSource->GetOutput(), guidanceRequirements);
\t\t\t}
\t\t}''')
    return s

def patch_core_project(s: str) -> str:
    # NativeBridge source files include headers as "native_bridge/...".  The
    # project historically only exposes Magpie.Core/include, so add the project
    # directory itself as an explicit include root rather than relying on the
    # compiler's source-file-relative lookup.
    s = once(s,
        '<AdditionalIncludeDirectories>include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>',
        '<AdditionalIncludeDirectories>.;include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>')
    return once(s, '</Project>', '''  <ItemGroup>
    <ClInclude Include="NativeBridgeFrameSource.h" />
    <ClInclude Include="native_bridge\\ledger.hpp" />
    <ClInclude Include="native_bridge\\wire.hpp" />
    <ClInclude Include="native_bridge\\windows\\ipc_pipe.hpp" />
    <ClInclude Include="native_bridge\\windows\\local_interop.hpp" />
    <ClInclude Include="native_bridge\\windows\\session_protocol.hpp" />
    <ClInclude Include="native_bridge\\windows\\runtime_session.hpp" />
    <ClInclude Include="native_bridge\\windows\\transport_d3d12.hpp" />
    <ClCompile Include="NativeBridgeFrameSource.cpp" />
    <ClCompile Include="native_bridge\\ledger.cpp" />
    <ClCompile Include="native_bridge\\wire.cpp" />
    <ClCompile Include="native_bridge\\windows\\ipc_pipe.cpp" />
    <ClCompile Include="native_bridge\\windows\\local_interop.cpp" />
    <ClCompile Include="native_bridge\\windows\\session_protocol.cpp" />
    <ClCompile Include="native_bridge\\windows\\runtime_session.cpp" />
    <ClCompile Include="native_bridge\\windows\\transport_d3d12.cpp" />
  </ItemGroup>
</Project>''')

def patch_app_project(s: str) -> str:
    old = '<AdditionalDependencies>Gdi32.lib;Dwmapi.lib;Shell32.lib;Ole32.lib;Imagehlp.lib;Comctl32.lib;Shlwapi.lib;Magnification.lib;bcp47mrm.lib;Shcore.lib;Uxtheme.lib;Taskschd.lib;Dcomp.lib;%(AdditionalDependencies)</AdditionalDependencies>'
    new = '<AdditionalDependencies>Gdi32.lib;Dwmapi.lib;Shell32.lib;Ole32.lib;Imagehlp.lib;Comctl32.lib;Shlwapi.lib;Magnification.lib;bcp47mrm.lib;Shcore.lib;Uxtheme.lib;Taskschd.lib;Dcomp.lib;D3D12.lib;Advapi32.lib;%(AdditionalDependencies)</AdditionalDependencies>'
    return once(s, old, new)

def cpp_with_pch(text: str) -> str:
    return text if text.startswith('#include "pch.h"') else '#include "pch.h"\n' + text

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    source = args.source.resolve()
    patchers = {
        'src/Magpie.Core/FrameSourceBase.h': patch_frame_source_h,
        'src/Magpie.Core/FrameSourceBase.cpp': patch_frame_source_cpp,
        'src/Magpie.Core/Renderer.cpp': patch_renderer,
        'src/Magpie.Core/Magpie.Core.vcxproj': patch_core_project,
        'src/Magpie/Magpie.vcxproj': patch_app_project,
    }
    before = {}
    after = {}
    for rel, expected in EXPECTED.items():
        text = (source / rel).read_text(encoding='utf-8')
        actual = git_blob(text)
        if actual != expected:
            raise ValueError(f'{rel}: expected blob {expected}, got {actual}; refusing changed source')
        before[rel] = text
        after[rel] = patchers[rel](text)

    additions = {
        'src/Magpie.Core/NativeBridgeFrameSource.h': (ROOT/'magpie/NativeBridgeFrameSource.h').read_text(),
        'src/Magpie.Core/NativeBridgeFrameSource.cpp': (ROOT/'magpie/NativeBridgeFrameSource.cpp').read_text(),
        'src/Magpie.Core/native_bridge/ledger.hpp': (ROOT/'include/native_bridge/ledger.hpp').read_text(),
        'src/Magpie.Core/native_bridge/wire.hpp': (ROOT/'include/native_bridge/wire.hpp').read_text(),
        'src/Magpie.Core/native_bridge/ledger.cpp': cpp_with_pch((ROOT/'src/ledger.cpp').read_text()),
        'src/Magpie.Core/native_bridge/wire.cpp': cpp_with_pch((ROOT/'src/wire.cpp').read_text()),
    }
    for name in ('ipc_pipe.hpp','local_interop.hpp','session_protocol.hpp','runtime_session.hpp','transport_d3d12.hpp'):
        additions[f'src/Magpie.Core/native_bridge/windows/{name}'] = (ROOT/'windows'/name).read_text()
    for name in ('ipc_pipe.cpp','local_interop.cpp','session_protocol.cpp','runtime_session.cpp','transport_d3d12.cpp'):
        additions[f'src/Magpie.Core/native_bridge/windows/{name}'] = cpp_with_pch((ROOT/'windows'/name).read_text())
    for rel in additions:
        if (source/rel).exists():
            raise FileExistsError(source/rel)

    backup = source/'.native-bridge-r2-runtime-backup'
    if backup.exists():
        raise FileExistsError(backup)
    report = {
        'base_commit': BASE_COMMIT,
        'mode': 'apply' if args.apply else 'check',
        'files_checked': len(before),
        'consumer_runtime_installed': True,
        'consumer_runtime_session_compiled': True,
        'consumer_only': True,
        'game_hook_installed': False,
        'runtime_tested': False,
    }
    if args.apply:
        backup.mkdir()
        try:
            for rel, text in before.items():
                path = backup/rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding='utf-8', newline='\n')
            for rel, text in {**after, **additions}.items():
                path = source/rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding='utf-8', newline='\n')
            (backup/'manifest.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        except Exception:
            for rel, text in before.items():
                path = source/rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text, encoding='utf-8', newline='\n')
            for rel in additions:
                (source/rel).unlink(missing_ok=True)
            raise
    print(json.dumps(report, indent=2))
    return 0

if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError) as exc:
        raise SystemExit(str(exc))