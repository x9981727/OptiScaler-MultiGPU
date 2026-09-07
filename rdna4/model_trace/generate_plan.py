# SPDX-License-Identifier: MIT
"""Expand the recovered, address-free 1080p host trace. No model weights or executable bytes.
Provenance: DLSSNR-rewrite-source-checkpoint, plans/1080p.json, SHA256
0867c68b824b3a6e4a8e7b1681efc80a1ab8251a50384bf005f600a80fe89a93.
The opaque arguments and launch ordering are preserved, not reinterpreted as GEMMs.
"""
import base64, hashlib, json, pathlib, struct, zlib
DATA = '''eNrtXVtvG7kV/i9+1sO58fCwb0WbxRbYh22B9qFGIGgVBQ3sdVw7dVoU/e89lOYmZUaxEo9MezlAwrE4F5Lf+fjxdob/vVhdX39crz59+Hhzf/G7S5SopgSyIA1CYLDgwJZEYOSXCDGa+S8YNUmkwYmZRFa/iRBNvgzbeBFita+Hh/cdhm08IoREYynqT9pLJkKmmN+oIeQHB0tGXhzscUnbPx8RHD6WjBFDIHi7uLja3uLja3N1srnOBXyz/jnC1vP/84Wb5sLr7008fmN789Au+efOQ/ra6+3l1t/r1/mIxcRl89TKVR12GZI+6joKOXxevlu/ff37nKPyQg+ET8Gq5/njzsLzb3Hv0H/z0MPqfVw/L1adPN4Twew+GsdrfvHz4sPl8j5Sf8PP18CLyl3+4WV0v/7FZvUP40f8fRNuV3327Wl8h/WUbDuLS1XLz79vVjSec3mxPvkz5p7v3mxfake'''
# Replaced below with the exact compressed, checksummed plan in the next commit.

def expand(encoded):
    raw=zlib.decompress(base64.b64decode(encoded))
    if hashlib.sha256(raw).hexdigest()!='e78d665fbb66e42e0914144f4f3f931a32ece1469136c5522931fc1369fe4c55':
        raise ValueError('Recovered plan hash mismatch')
    return json.loads(raw)

def emit(plan, target):
    sizes=plan['allocations']; events=plan['events']
    assert len(sizes)==44 and sum(sizes)==806544188
    assert len(events)==158 and sum(e[0]!='copy' for e in events)==154
    assert sizes[1]==1920*1152*3*4 and sizes[2]==1920*1152*4*4
    out=['// Generated from the checksum-pinned recovered host trace.','static TracePlan make_plan() {',' TracePlan p;',
         ' p.sizes={'+','.join(str(x)+'ULL' for x in sizes)+'};',
         ' p.names={'+','.join(json.dumps(n) for n in plan['kernels'])+'};']
    for event in events:
        if event[0]=='copy':
            _,sa,so,da,do,n=event
            assert so+n<=sizes[sa] and do+n<=sizes[da]
            out.append(f' p.ops.push_back(TraceOp::copy({sa},{so},{da},{do},{n}));')
        else:
            ki,grid,block,shared,args=event
            assert len(args)==1 and len(grid)==len(block)==3
            rawhex,relocs=args[0]; raw=bytes.fromhex(rawhex)
            for at,alloc,offset in relocs:
                assert at%8==0 and at+8<=len(raw) and offset<sizes[alloc]
                assert raw[at:at+8]==bytes(8)
            rr='{'+','.join('{'+','.join(map(str,r))+'}' for r in relocs)+'}'
            gg='{'+','.join(map(str,grid))+'}'
            bb='{'+','.join(map(str,block))+'}'
            out.append(f' p.ops.push_back(TraceOp::kernel({ki},{gg},{bb},{shared},"{rawhex}",{rr}));')
    out += [' return p;','}']
    target.write_text('\n'.join(out)+'\n',encoding='utf-8')

if __name__=='__main__':
    root=pathlib.Path(__file__).resolve().parent
    plan=expand(DATA)
    emit(plan,root/'plan1080.hpp')
    (root/'plan1080.json').write_text(json.dumps(plan,indent=2)+'\n',encoding='utf-8')
    print('PASS: 44 allocations, 154 original launches, 4 copies, all relocation ranges validated')
