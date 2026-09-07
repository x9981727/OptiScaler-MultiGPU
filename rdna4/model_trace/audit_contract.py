# SPDX-License-Identifier: MIT
"""Map original Swin launches to model blobs without executing or editing a DLL.
Requires the recovered 1080p plan and the user's existing model archive.
Opaque blob contents and unknown control fields are NOT interpreted as GEMMs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from collections import Counter

PLAN_SHA='0867c68b824b3a6e4a8e7b1681efc80a1ab8251a50384bf005f600a80fe89a93'
WEIGHTS_SHA='6bf8dc931ef3ccffe18c82de26ab374156e7f19539ffcf8eabaa25dca5cf15ab'

def require(condition,message):
    if not condition:
        raise ValueError(message)

def inspect(plan_path,weights_path):
    raw=Path(plan_path).read_bytes(); weights=Path(weights_path).read_bytes()
    require(hashlib.sha256(raw).hexdigest()==PLAN_SHA,'Unsupported recovered plan')
    require(hashlib.sha256(weights).hexdigest()==WEIGHTS_SHA,'Unsupported model archive')
    plan=json.loads(raw)
    require(weights[:8]==b'DLSSNRW1','Wrong archive signature')
    count,base=struct.unpack_from('<II',weights,8)
    require((count,base)==(153,5673),'Unexpected model directory')
    position=16; entries=[]; names=set()
    for _ in range(count):
        length=weights[position];position+=1
        name=weights[position:position+length].decode('ascii');position+=length
        offset,size=struct.unpack_from('<QQ',weights,position);position+=16
        require(name not in names and base+offset+size<=len(weights),'Invalid archive entry')
        names.add(name);entries.append((name,offset,size))
    require(position==base,'Directory boundary mismatch')
    lookup={};end=0
    for name,_,size in sorted(entries):
        target=((end+15)&~15)|12
        lookup[target]=(name,size);end=target+size
    sizes={a['id']:a['size'] for a in plan['allocations']}
    require(end==sizes[0],'Legacy upload size mismatch')
    events=[e for e in plan['events'] if e['op']!='stage']
    require(Counter(e['op'] for e in events)=={'launch':154,'memcpy':4},'Unexpected command sequence')
    result=[]
    for index,event in enumerate(events):
        if event['op']=='memcpy':
            for key in ['src','dst']:
                r=event[key]
                require(0<=r['offset'] and r['offset']+event['size']<=sizes[r['allocation']],'Invalid copy range')
            continue
        for argument in event['args']:
            content=bytes.fromhex(argument['bytes'])
            for r in argument['relocations']:
                require(0<=r['at'] and r['at']+8<=len(content),'Invalid argument relocation')
                require(0<=r['offset']<sizes[r['allocation']],'Invalid allocation offset')
                require(content[r['at']:r['at']+8]==bytes(8),'Unnormalized pointer')
        if 'k_swin_var' not in event['kernel']:
            continue
        require(len(event['args'])==1 and len(content)==168,'Unexpected Swin argument ABI')
        refs={r['at']:r for r in event['args'][0]['relocations']}
        weight=refs[16]
        require(weight['allocation']==0 and weight['offset'] in lookup,'Unknown Swin weight reference')
        name,size=lookup[weight['offset']]
        result.append(dict(command_index=index,kernel=event['kernel'],argument_bytes=168,
            grid=event['grid'],block=event['block'],dynamic_shared_bytes=event['shared'],
            u32_at24=struct.unpack_from('<I',content,24)[0],u32_at28=struct.unpack_from('<I',content,28)[0],
            i32_at32_36=list(struct.unpack_from('<ii',content,32)),u32_at40=struct.unpack_from('<I',content,40)[0],
            weight_blob=name,weight_blob_bytes=size,weight_gpu_offset=weight['offset'],
            relocations=event['args'][0]['relocations']))
    require(len(result)==46,'Incomplete Swin mapping')
    return dict(scope='Observed launch-to-blob correspondence only; NOT decoded model or new backend',
        source_plan_sha256=PLAN_SHA,model_archive_sha256=WEIGHTS_SHA,
        weight_blobs=count,legacy_upload_bytes=end,total_allocations=len(sizes),
        total_allocation_bytes=sum(sizes.values()),original_gpu_launches=154,original_device_copies=4,
        swin_launches=len(result),swin=result,
        limitations=['Inner matrix layouts and scaling are not decoded.',
            'Channel-count agreement does not establish equivalence to an r2 M/N/K case.',
            'No DLL or GPU code is modified or executed.'])

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan',type=Path,required=True)
    parser.add_argument('--weights',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    result=inspect(args.plan,args.weights)
    with args.out.open('x',encoding='utf-8') as stream:
        json.dump(result,stream,indent=2);stream.write('\n')
    print('PASS: 154 original launches; all 46 Swin weight references mapped. No GPU test performed.')

if __name__=='__main__':
    main()
