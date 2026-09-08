from pathlib import Path
import sys,hashlib,json
root=Path(sys.argv[1])/'samples/basic_sample_frame_generation'
source=Path(__file__).parent/'NativeOutputMailbox.h'
(root/'NativeGate.h').write_bytes(source.read_bytes())
m=root/'NATIVE-GATE-MANIFEST.json';d=json.loads(m.read_text())
d.update(kind='isolated_native_output_fifo_not_game_patch',deadline_placement='CPU_before_real_native_Present_after_completed_raw_copy',native_submission='bounded_acceptance_then_real_Present_with_sticky_asynchronous_errors',game_modified=False,resize_supported=False,scanout_or_pixel_quality_certified=False)
d['NativeGate.h_sha256']=hashlib.sha256((root/'NativeGate.h').read_bytes()).hexdigest()
m.write_text(json.dumps(d,indent=2),encoding='utf-8')
print('Selected complete-image native output FIFO; not a shipping DXGI or game patch.')
