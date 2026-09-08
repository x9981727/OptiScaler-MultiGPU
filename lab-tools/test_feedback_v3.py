import math
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from display_feedback_v3 import MidpointController, Frame, NativeIndex, atomic_control

class Tests(unittest.TestCase):
    def simulate(self, change=False, commit=True, mode='Hardware: Independent Flip', model='latency'):
        c=MidpointController(model);proposals=[];period=1000/67
        for i in range(1,5001):
            phase=c.phase if i%2==0 else 0
            p=100000+i*period/2+phase
            offset=2.2 if not change or i<2500 else -1.2
            latency=4+(offset if i%2==0 else 0)
            v=c.observe(Frame(i,p,p+latency,period,phase,mode),p+700)
            if v is not None:
                proposals.append(v)
                if commit:c.commit(v,p+700)
        return c,proposals
    def test_latency_bias_converges_without_seed(self):
        c,p=self.simulate()
        self.assertLess(abs(c.phase+2.2),.15)
        self.assertTrue(all(abs(x['new_phase_ms']-x['old_phase_ms'])<=.350001 for x in p))
    def test_midpoint_bias_converges_in_synthetic_model(self):
        c,_=self.simulate(model='midpoint');self.assertLess(abs(c.phase+2.2),.15)
    def test_changed_bias_reconverges(self):
        c,_=self.simulate(change=True);self.assertLess(abs(c.phase-1.2),.15)
    def test_no_commit_keeps_state(self):
        c,p=self.simulate(commit=False);self.assertGreater(len(p),0);self.assertEqual(c.phase,0)
    def test_composed_mode_rejected(self):
        c,p=self.simulate(mode='Composed: Flip');self.assertEqual(p,[]);self.assertGreater(c.invalid['display_mode'],0)
    def test_bad_clocks(self):
        for p,now in [(math.nan,1),(10000,9999),(10000,20000)]:
            c=MidpointController();self.assertIsNone(c.observe(Frame(2,p,p+1,15,0,'Hardware'),now));self.assertTrue(c.invalid)
    def test_wrong_generation(self):
        c=MidpointController();c.phase=-1
        for i in range(1,101):self.assertIsNone(c.observe(Frame(i,10000+i*7.5,10001+i*7.5,15,0,'Hardware'),10010+i*7.5))
        self.assertGreater(c.invalid['old_generation'],0)
    def test_gap_never_forms_triplet(self):
        c=MidpointController()
        for i in range(1,100,2):self.assertIsNone(c.observe(Frame(i,10000+i*7.5,10001+i*7.5,15,0,'Hardware'),10010+i*7.5))
        self.assertGreater(c.invalid['sequence_gap'],0)
    def test_partial_line(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'native.csv';p.write_bytes(b'1,100,15,0,0\n2,108')
            n=NativeIndex(p);n.refresh();self.assertEqual(len(n.records),1)
            with p.open('ab') as f:f.write(b',15,0,0\n')
            n.refresh();self.assertEqual(n.match(108.1)[0],2);self.assertIsNone(n.match(109))
    def test_invalid_native_clock(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'native.csv';p.write_bytes(b'1,nan,15,0,0\n')
            n=NativeIndex(p);n.refresh();self.assertEqual(n.malformed,1)
    def test_atomic_write(self):
        with tempfile.TemporaryDirectory() as d:
            t=atomic_control(Path(d),-1.25,lambda:12345);self.assertEqual(t,12345)
            self.assertEqual((Path(d)/'phase-control.txt').read_text(),'1 -1.25000000 12345.000000\n')
    def test_atomic_write_retries_then_fails(self):
        with tempfile.TemporaryDirectory() as d,patch('display_feedback_v3.os.replace',side_effect=PermissionError('busy')) as m:
            with self.assertRaises(PermissionError):atomic_control(Path(d),0,lambda:1)
            self.assertEqual(m.call_count,4)
    def test_invalid_control_rejected(self):
        for x in (math.nan,math.inf,5,-5):
            with self.assertRaises(ValueError):atomic_control(Path('.'),x,lambda:1)
    def test_stale_commit_rejected(self):
        c=MidpointController()
        with self.assertRaises(ValueError):c.commit({'old_phase_ms':1,'new_phase_ms':2},0)
    def test_invalid_model_rejected(self):
        with self.assertRaises(ValueError):MidpointController('unknown')

if __name__=='__main__':unittest.main(verbosity=2)
