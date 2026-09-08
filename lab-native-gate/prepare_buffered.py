from pathlib import Path
import json,hashlib,shutil,sys
repo=Path(__file__).resolve().parents[1]
root=Path(sys.argv[1]).resolve()/'samples/basic_sample_frame_generation'
p=root/'native_gate.cpp';s=p.read_text(encoding='utf-8-sig')
def once(old,new):
 global s
 if s.count(old)!=1:raise RuntimeError('Buffered source anchor: '+old[:100])
 s=s.replace(old,new)
once('#include <algorithm>','#include <algorithm>\n#include "buffered_output.h"')
old='    auto hr=realCreate(f,q,hwnd,d,fs,o,sc);'
new='''    if (gate.armed && hwnd==gate.target && BufferedOutputLab::Enabled()) {
        ComPtr<ID3D12CommandQueue> producer;
        HRESULT h=q->QueryInterface(IID_PPV_ARGS(&producer));
        if(FAILED(h))return h;
        ComPtr<ID3D12Device> device;h=producer->GetDevice(IID_PPV_ARGS(&device));
        if(FAILED(h))return h;
        ComPtr<ID3D12CommandQueue> screen;
        auto description=producer->GetDesc();
        h=device->CreateCommandQueue(&description,IID_PPV_ARGS(&screen));
        if(FAILED(h))return h;
        h=realCreate(f,screen.Get(),hwnd,d,fs,o,sc);
        if(FAILED(h))return h;
        if(!sc||!*sc)return E_UNEXPECTED;
        try {BufferedOutputLab::Initialize(*sc,producer.Get(),screen.Get());}
        catch(...) {(*sc)->Release();*sc=nullptr;return E_FAIL;}
        return h;
    }
''' + old
once(old,new)
once('extern "C" void FinishNativeGate() {gate.Finish();}', 'extern "C" void FinishNativeGate() {BufferedOutputLab::Finish();gate.Finish();}')
p.write_text(s,encoding='utf-8');shutil.copyfile(repo/'lab-native-gate/buffered_output.h',root/'buffered_output.h')
m={'kind':'isolated_buffered_native_output_prototype','game_modified':False,'game_release':False,'input_shaders_or_resolution_changed':False,'native_outputs_fifo':True,'snapshot_slots':8,'fullscreen_resize_not_supported':True,'performance_and_pixel_acceptance':False,'native_source_sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'staging_header_sha256':hashlib.sha256((root/'buffered_output.h').read_bytes()).hexdigest()}
(root/'BUFFERED-OUTPUT-MANIFEST.json').write_text(json.dumps(m,indent=2),encoding='utf-8')
print('Isolated buffered output prototype prepared. Not suitable for game deployment.')
