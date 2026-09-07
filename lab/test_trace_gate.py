"""Synthetic parser controls are never hardware results."""
import csv,tempfile,unittest
from pathlib import Path
from evaluate_trace import evaluate
class TraceGateTests(unittest.TestCase):
 def fixture(self,root,rate=67.,share=.5,generated=True,seconds=42,drop=False,reverse=False):
  p=root/'synthetic.csv';rows=[];n=int(seconds*rate)
  for i in range(n):
   t=10000+i*1000/rate
   for typ,delta in [('Application',0),('Intel XeSS-FG',(1-share)*1000/rate)]:
    if typ!='Application' and not generated:continue
    rows.append({'Application':'wwm.exe','ProcessID':'1','SwapChainAddress':'0x1','TimeInMs':t+delta-0.1,'MsUntilDisplayed':0 if drop and i%10==0 else .1,'FrameType':typ,'AllowsTearing':0})
  if reverse:rows[1],rows[2]=rows[2],rows[1]
  with p.open('w',newline='') as f:
   w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
  return p
 def test_ideal_is_timing_only(self):
  with tempfile.TemporaryDirectory() as d:
   r=evaluate(self.fixture(Path(d)));self.assertTrue(r['timing_gate_passed']);self.assertFalse(r['final_release_allowed'])
 def test_v24_like_burst_is_rejected(self):
  with tempfile.TemporaryDirectory() as d:
   r=evaluate(self.fixture(Path(d),share=.054));self.assertIn('UNEVEN_ORIGINAL_GENERATED_RESIDENCE',r['timing_failures'])
 def test_v25_like_33_to_67_is_rejected(self):
  with tempfile.TemporaryDirectory() as d:
   r=evaluate(self.fixture(Path(d),rate=33.5));self.assertIn('SOURCE_RATE_NOT_PRESERVED',r['timing_failures']);self.assertFalse(r['timing_gate_passed'])
 def test_dropped_is_not_a_generated_frame(self):
  with tempfile.TemporaryDirectory() as d:
   r=evaluate(self.fixture(Path(d),drop=True));self.assertIn('DISPLAY_DROPS',r['timing_failures'])
 def test_no_generated_events_is_not_unknown_success(self):
  with tempfile.TemporaryDirectory() as d:
   with self.assertRaises(ValueError):evaluate(self.fixture(Path(d),generated=False))
 def test_short_is_not_pass(self):
  with tempfile.TemporaryDirectory() as d:
   r=evaluate(self.fixture(Path(d),seconds=10));self.assertIn('CAPTURE_TOO_SHORT',r['timing_failures'])
 def test_reversed_present_timestamps_rejected(self):
  with tempfile.TemporaryDirectory() as d:
   with self.assertRaises(ValueError):evaluate(self.fixture(Path(d),reverse=True))
 def test_absent_columns_rejected(self):
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/'bad.csv';p.write_text('Application,TimeInMs\nwwm.exe,1\n')
   with self.assertRaises(ValueError):evaluate(p)
 def test_missing_file_not_pass(self):
  with self.assertRaises(OSError):evaluate(Path('/no-such-capture.csv'))
if __name__=='__main__':unittest.main(verbosity=2)
