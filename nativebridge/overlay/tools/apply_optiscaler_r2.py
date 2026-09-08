#!/usr/bin/env python3
"""Install NativeBridge r2 producer into pinned OptiScaler.
Fails closed if the two upstream integration files differ from the audited commit.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BASE_COMMIT = "da70e61e1542a0b99adcb24168ff941e42109567"
EXPECTED = {
    "OptiScaler/hooks/Streamline_Hooks.cpp": "c8344ad99dd7b9daae2773573330d4a3f6b4db9e",
    "OptiScaler/OptiScaler.vcxproj": "b71f642d74aaaa3ea15ad7f7a5073327ae24a289",
}

def git_blob(text: str) -> str:
    data = text.encode("utf-8")
    return hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()

def once(text: str, old: str, new: str) -> str:
    count = text.count(old)
    if count != 1:
        raise ValueError(f"expected one anchor, found {count}: {old[:100]!r}")
    return text.replace(old, new, 1)

def patch_hooks(s: str) -> str:
    s = once(s, '#include <hooks/Reflex_Hooks.h>\n',
        '#include <hooks/Reflex_Hooks.h>\n#include <native_bridge/NativeBridgeCapture.h>\n')

    constants_anchor = '''    LOG_TRACE("called with frameIndex: {}, viewport: {}", (unsigned int) frame, (unsigned int) viewport);\n\n    State::Instance().slFGInputs.setConstants(values, (uint32_t) frame);'''
    constants_new = '''    LOG_TRACE("called with frameIndex: {}, viewport: {}", (unsigned int) frame, (unsigned int) viewport);\n\n    NativeBridgeCapture::OnConstants(values, static_cast<uint32_t>(frame), static_cast<uint32_t>(viewport));\n    State::Instance().slFGInputs.setConstants(values, (uint32_t) frame);'''
    s = once(s, constants_anchor, constants_new)

    tag_anchor = '''    LOG_DEBUG("frameIndex: {}", static_cast<uint32_t>(frame));\n\n    if (State::Instance().activeFgInput == FGInput::DLSSG &&'''
    tag_new = '''    LOG_DEBUG("frameIndex: {}", static_cast<uint32_t>(frame));\n\n    NativeBridgeCapture::OnTags(\n        static_cast<uint32_t>(frame), static_cast<uint32_t>(viewport), resources, numResources,\n        reinterpret_cast<ID3D12GraphicsCommandList*>(cmdBuffer));\n\n    if (State::Instance().activeFgInput == FGInput::DLSSG &&'''
    s = once(s, tag_anchor, tag_new)

    marker_anchor = '''sl::Result StreamlineHooks::hkslPCLSetMarker(sl::PCLMarker marker, const sl::FrameToken& frame)\n{\n'''
    marker_new = '''sl::Result StreamlineHooks::hkslPCLSetMarker(sl::PCLMarker marker, const sl::FrameToken& frame)\n{\n    if (marker == sl::PCLMarker::ePresentStart)\n        NativeBridgeCapture::OnPresentStart(\n            static_cast<uint32_t>(frame), State::Instance().currentCommandQueue);\n\n'''
    s = once(s, marker_anchor, marker_new)

    reflex_hook = '''    if (strcmp(functionName, "slReflexSetMarker") == 0 &&\n        (State::Instance().gameQuirks & GameQuirk::FixSlSimulationMarkers ||\n         State::Instance().activeFgInput == FGInput::DLSSG))'''
    reflex_new = '''    if (strcmp(functionName, "slReflexSetMarker") == 0 &&\n        (State::Instance().gameQuirks & GameQuirk::FixSlSimulationMarkers ||\n         State::Instance().activeFgInput == FGInput::DLSSG || NativeBridgeCapture::Enabled()))'''
    s = once(s, reflex_hook, reflex_new)

    pcl_hook = '''    if (strcmp(functionName, "slPCLSetMarker") == 0 &&\n        (State::Instance().gameQuirks & GameQuirk::FixSlSimulationMarkers ||\n         State::Instance().activeFgInput == FGInput::DLSSG))'''
    pcl_new = '''    if (strcmp(functionName, "slPCLSetMarker") == 0 &&\n        (State::Instance().gameQuirks & GameQuirk::FixSlSimulationMarkers ||\n         State::Instance().activeFgInput == FGInput::DLSSG || NativeBridgeCapture::Enabled()))'''
    s = once(s, pcl_hook, pcl_new)
    return s

def patch_project(s: str) -> str:
    # OptiScaler x64 already exposes $(ProjectDir), so native_bridge/... resolves.
    # Advapi32 is required by the authenticated named-pipe security/token code.
    s = s.replace('d3d12.lib;', 'd3d12.lib;Advapi32.lib;')
    group = '''  <ItemGroup>\n    <ClInclude Include="native_bridge\\NativeBridgeCapture.h" />\n    <ClInclude Include="native_bridge\\contract.hpp" />\n    <ClInclude Include="native_bridge\\ledger.hpp" />\n    <ClInclude Include="native_bridge\\wire.hpp" />\n    <ClInclude Include="native_bridge\\windows\\ipc_pipe.hpp" />\n    <ClInclude Include="native_bridge\\windows\\runtime_session.hpp" />\n    <ClInclude Include="native_bridge\\windows\\session_protocol.hpp" />\n    <ClInclude Include="native_bridge\\windows\\transport_d3d12.hpp" />\n    <ClCompile Include="native_bridge\\NativeBridgeCapture.cpp" />\n    <ClCompile Include="native_bridge\\contract.cpp" />\n    <ClCompile Include="native_bridge\\ledger.cpp" />\n    <ClCompile Include="native_bridge\\wire.cpp" />\n    <ClCompile Include="native_bridge\\windows\\ipc_pipe.cpp" />\n    <ClCompile Include="native_bridge\\windows\\runtime_session.cpp" />\n    <ClCompile Include="native_bridge\\windows\\session_protocol.cpp" />\n    <ClCompile Include="native_bridge\\windows\\transport_d3d12.cpp" />\n  </ItemGroup>\n'''
    return once(s, '</Project>', group + '</Project>')

def with_pch(text: str) -> str:
    return text if text.startswith('#include "pch.h"') else '#include "pch.h"\n' + text

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    source = args.source.resolve()

    before: dict[str,str] = {}
    after: dict[str,str] = {}
    patchers = {
        'OptiScaler/hooks/Streamline_Hooks.cpp': patch_hooks,
        'OptiScaler/OptiScaler.vcxproj': patch_project,
    }
    for rel, expected in EXPECTED.items():
        text = (source/rel).read_text(encoding='utf-8')
        actual = git_blob(text)
        if actual != expected:
            raise ValueError(f'{rel}: expected blob {expected}, got {actual}; refusing changed source')
        before[rel] = text
        after[rel] = patchers[rel](text)

    capture_root = ROOT/'integrations/optiscaler'
    additions: dict[str,str] = {
        'OptiScaler/native_bridge/NativeBridgeCapture.h': (capture_root/'NativeBridgeCapture.h').read_text(),
        'OptiScaler/native_bridge/NativeBridgeCapture.cpp': (capture_root/'NativeBridgeCapture.cpp').read_text(),
        'OptiScaler/native_bridge/contract.hpp': (ROOT/'include/native_bridge/contract.hpp').read_text(),
        'OptiScaler/native_bridge/ledger.hpp': (ROOT/'include/native_bridge/ledger.hpp').read_text(),
        'OptiScaler/native_bridge/wire.hpp': (ROOT/'include/native_bridge/wire.hpp').read_text(),
        'OptiScaler/native_bridge/contract.cpp': with_pch((ROOT/'src/contract.cpp').read_text()),
        'OptiScaler/native_bridge/ledger.cpp': with_pch((ROOT/'src/ledger.cpp').read_text()),
        'OptiScaler/native_bridge/wire.cpp': with_pch((ROOT/'src/wire.cpp').read_text()),
    }
    for name in ('ipc_pipe.hpp','runtime_session.hpp','session_protocol.hpp','transport_d3d12.hpp'):
        additions[f'OptiScaler/native_bridge/windows/{name}'] = (ROOT/'windows'/name).read_text()
    for name in ('ipc_pipe.cpp','runtime_session.cpp','session_protocol.cpp','transport_d3d12.cpp'):
        additions[f'OptiScaler/native_bridge/windows/{name}'] = with_pch((ROOT/'windows'/name).read_text())

    for rel in additions:
        if (source/rel).exists(): raise FileExistsError(source/rel)
    report = {
        'base_commit': BASE_COMMIT,
        'mode': 'apply' if args.apply else 'check',
        'files_checked': len(before),
        'game_hook_installed': True,
        'native_streamline_tags': True,
        'native_streamline_camera': True,
        'present_queue_signal': True,
        'runtime_tested': False,
    }
    backup = source/'.native-bridge-r2-optiscaler-backup'
    if args.apply:
        if backup.exists(): raise FileExistsError(backup)
        backup.mkdir()
        try:
            for rel, text in before.items():
                path=backup/rel; path.parent.mkdir(parents=True,exist_ok=True); path.write_text(text,encoding='utf-8',newline='\n')
            for rel, text in {**after, **additions}.items():
                path=source/rel; path.parent.mkdir(parents=True,exist_ok=True); path.write_text(text,encoding='utf-8',newline='\n')
            (backup/'manifest.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
        except Exception:
            for rel,text in before.items():
                path=source/rel; path.parent.mkdir(parents=True,exist_ok=True); path.write_text(text,encoding='utf-8',newline='\n')
            for rel in additions: (source/rel).unlink(missing_ok=True)
            raise
    print(json.dumps(report,indent=2))
    return 0

if __name__ == '__main__':
    try: raise SystemExit(main())
    except (ValueError,OSError) as exc: raise SystemExit(str(exc))