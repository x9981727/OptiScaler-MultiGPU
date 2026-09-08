#!/usr/bin/env python3
"""Version-locked r3: finish NativeBridge -> direct NGX DLSSFG constants.
Run after r1 and r2 patches. Refuses any unexpected Magpie source.
"""
from __future__ import annotations
import argparse, hashlib, json
from pathlib import Path

BASE_COMMIT = "ac1cc8b0f2efc78323898395cc1336bcbecdc276"
REL = "src/Magpie.Core/DLSSFrameGenerator.cpp"
EXPECTED = "d1d32cc0b331bafa5759b428d94c8d257c719f88"  # post-r1/r2 blob

def git_blob(text: str) -> str:
    data=text.encode('utf-8')
    return hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()

def once(s: str, old: str, new: str) -> str:
    n=s.count(old)
    if n != 1: raise ValueError(f'expected one anchor, got {n}: {old[:120]!r}')
    return s.replace(old,new,1)

def patch(s: str) -> str:
    old='''\t\t\tstd::memcpy(optionalParams.cameraUp, camera.up, sizeof(camera.up));
\t\t\tstd::memcpy(optionalParams.cameraRight, camera.right, sizeof(camera.right));
\t\t\tstd::memcpy(optionalParams.cameraFwd, camera.forward, sizeof(camera.forward));
\t\t\toptionalParams.cameraNear = camera.nearPlane;'''
    new='''\t\t\tstd::memcpy(optionalParams.cameraPos, camera.position, sizeof(camera.position));
\t\t\tstd::memcpy(optionalParams.cameraUp, camera.up, sizeof(camera.up));
\t\t\tstd::memcpy(optionalParams.cameraRight, camera.right, sizeof(camera.right));
\t\t\tstd::memcpy(optionalParams.cameraFwd, camera.forward, sizeof(camera.forward));
\t\t\toptionalParams.cameraNear = camera.nearPlane;'''
    s=once(s,old,new)
    old='''\t\t\toptionalParams.motionVectorsDilated = (camera.flags & nb::MotionDilated) != 0;
\t\t\t// Transport input was converted to current->previous/source pixels once.
\t\t\toptionalParams.mvecScale[0] = optionalParams.mvecScale[1] = 1.0f;'''
    new='''\t\t\toptionalParams.motionVectorsDilated = (camera.flags & nb::MotionDilated) != 0;
\t\t\toptionalParams.motionVectorsInvalidValue = camera.invalidMotionValue;
\t\t\t// NativeBridge preserves the game's raw Streamline motion texture. The
\t\t\t// packet stores raw->pixel scale = sl::Constants::mvecScale * extent,
\t\t\t// so divide by extent to reconstruct the exact NGX normalization scale.
\t\t\toptionalParams.mvecScale[0] = camera.rawMotionToPixels[0] /
\t\t\t\tstatic_cast<float>(renderExtent.width);
\t\t\toptionalParams.mvecScale[1] = camera.rawMotionToPixels[1] /
\t\t\t\tstatic_cast<float>(renderExtent.height);'''
    s=once(s,old,new)
    return s

def main():
    ap=argparse.ArgumentParser(description=__doc__); ap.add_argument('source',type=Path); ap.add_argument('--apply',action='store_true'); a=ap.parse_args()
    p=a.source.resolve()/REL; before=p.read_text(encoding='utf-8'); actual=git_blob(before)
    if actual != EXPECTED: raise ValueError(f'{REL}: expected {EXPECTED}, got {actual}; refusing changed source')
    after=patch(before)
    report={'base_commit':BASE_COMMIT,'mode':'apply' if a.apply else 'check','dlssfg_native_depth':True,'dlssfg_native_camera':True,'dlssfg_native_motion_scale':True,'dlssfg_zero_fallback_in_native_mode':False}
    if a.apply:
        backup=a.source.resolve()/'.native-bridge-r3-dlssfg-backup'; backup.mkdir(exist_ok=False)
        bp=backup/REL; bp.parent.mkdir(parents=True,exist_ok=True); bp.write_text(before,encoding='utf-8',newline='\n')
        p.write_text(after,encoding='utf-8',newline='\n')
        (backup/'manifest.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(report,indent=2)); return 0

if __name__=='__main__':
    try: raise SystemExit(main())
    except (OSError,ValueError) as e: raise SystemExit(str(e))
