"""Reconstruct the reviewable v3 controller from the pinned v2 source.
The latency model has hardware evidence; the midpoint model is retained only
as an explicit A/B control, not silently selected as a successful replacement.
"""
from pathlib import Path
import hashlib,json
root=Path(__file__).resolve().parent
p=root/'display_feedback_v2.py';s=p.read_text(encoding='utf-8')
def once(a,b):
 global s
 if s.count(a)!=1:raise RuntimeError('v3 source anchor: '+a[:100])
 s=s.replace(a,b)
once('def __init__(self) -> None:\n        self.phase = 0.0',
     "def __init__(self, model: str = 'midpoint') -> None:\n        if model not in ('latency','midpoint'):raise ValueError('Unknown feedback model')\n        self.model = model\n        self.phase = 0.0")
once('residual = mid.display_ms - (left.display_ms + right.display_ms)*.5',
     "residual = ((mid.display_ms-mid.present_ms)-(left.display_ms-left.present_ms)) if self.model == 'latency' else mid.display_ms - (left.display_ms + right.display_ms)*.5")
once('step = max(-.35, min(.35, -.45*residual))',
     "error = self.phase+residual if self.model == 'latency' else residual\n        step = max(-.35, min(.35, -.5*error))")
once("'midpoint_error_ms': residual, 'mad_ms': mad", "'model': self.model, 'observed_signal_ms': residual, 'remaining_error_ms': error, 'mad_ms': mad")
once('if abs(residual) < .12 or abs(proposed-self.phase) < .025:',
     'if abs(error) < .12 or abs(proposed-self.phase) < .025:')
once("parser.add_argument('--observe-only', action='store_true')", "parser.add_argument('--observe-only', action='store_true')\n    parser.add_argument('--model', choices=['latency','midpoint'], default='latency')")
once('control = MidpointController()', 'control = MidpointController(args.model)')
once("mode = 'observe_only' if args.observe_only else 'midpoint_feedback'", "mode = 'observe_only' if args.observe_only else args.model+'_feedback'")
once("'kind': 'isolated_midpoint_feedback_v2'", "'kind': 'isolated_transactional_feedback_v3'")
output=root/'display_feedback_v3.py';output.write_text(s,encoding='utf-8')
(root/'feedback-v3-source.json').write_text(json.dumps({'kind':'isolated_controller_reconstruction','input_sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'output_sha256':hashlib.sha256(output.read_bytes()).hexdigest(),'runtime_model_default':'latency','game_modified':False,'game_certified':False},indent=2),encoding='utf-8')
print('v3 controller ready:',hashlib.sha256(output.read_bytes()).hexdigest())
