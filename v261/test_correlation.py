import math, unittest
from pathlib import Path
from types import SimpleNamespace
from correlation import NativeMatcher
import game_feedback as gf

def traces(count=140, period=7.5, native_address=1234):
    times=[1000.0+i*period for i in range(count)]
    records=[dict(id=str(i+1),qpc_ms=str(t),swapchain=str(native_address),hresult='0',
                  generation='1',eligible='1',paced='0',ordinal='0',phase_ms='0')
             for i,t in enumerate(times)]
    rows=[dict(ProcessID='77',Application='wwm.exe',Runtime='DXGI',Dropped='0',
               QPCTime=str((t+.005)/1000),msUntilDisplayed='10',SwapChainAddress='0x9876')
          for t in times]
    return SimpleNamespace(times=times,records=records),rows

class CorrelationTests(unittest.TestCase):
    def test_old_address_equality_rejects_alias(self):
        tail,rows=traces();old=gf.Tail(Path('absent'));old.times=tail.times;old.records=tail.records
        self.assertTrue(all(old.match(r) is None for r in rows))
    def test_no_early_acceptance(self):
        tail,rows=traces();m=NativeMatcher(77)
        self.assertTrue(all(m.match(r,tail) is None for r in rows[:95]))
        self.assertIsNotNone(m.match(rows[95],tail));self.assertTrue(m.locked)
    def test_alias_works_without_fixed_pointer_delta(self):
        for address in (11,1234,0x10000102030):
            tail,rows=traces(native_address=address);m=NativeMatcher(77)
            matched=[m.match(r,tail) for r in rows]
            self.assertEqual(sum(x is not None for x in matched),len(rows)-95)
            self.assertEqual(m.mapping,(address,0x9876))
    def test_same_address_also_requires_sequence(self):
        tail,rows=traces(native_address=0x9876);m=NativeMatcher(77)
        self.assertIsNone(m.match(rows[0],tail))
    def test_process_and_runtime_guard(self):
        for key,value in [('ProcessID','78'),('Runtime','D3D9'),('Application','other.exe')]:
            tail,rows=traces();m=NativeMatcher(77)
            for r in rows:r[key]=value
            self.assertTrue(all(m.match(r,tail) is None for r in rows));self.assertFalse(m.locked)
    def test_nonfinite_and_dropped_events(self):
        for key,value in [('QPCTime','nan'),('QPCTime','inf'),('msUntilDisplayed','nan'),('msUntilDisplayed','-1'),('Dropped','1'),('SwapChainAddress','0x0')]:
            tail,rows=traces();m=NativeMatcher(77)
            for r in rows:r[key]=value
            self.assertTrue(all(m.match(r,tail) is None for r in rows))
    def test_causal_not_nearest_future(self):
        tail,rows=traces();m=NativeMatcher(77);rows[0]['QPCTime']=str((tail.times[0]-.005)/1000)
        self.assertIsNone(m.match(rows[0],tail));self.assertEqual(m.counts['no_causal_native_in_125us'],1)
    def test_no_wide_time_guess(self):
        tail,rows=traces();m=NativeMatcher(77)
        for i,r in enumerate(rows):r['QPCTime']=str((tail.times[i]+.25)/1000)
        self.assertTrue(all(m.match(r,tail) is None for r in rows))
    def test_ambiguous_calls_refused(self):
        tail,rows=traces(1);tail.times.append(1000.003);tail.records.append(dict(tail.records[0],id='2',qpc_ms='1000.003'))
        m=NativeMatcher(77);self.assertIsNone(m.match(rows[0],tail));self.assertEqual(m.counts['ambiguous_native_candidates'],1)
    def test_hresult_refused(self):
        tail,rows=traces();m=NativeMatcher(77)
        for r in tail.records:r['hresult']='-2005270523'
        self.assertTrue(all(m.match(r,tail) is None for r in rows))
    def test_duplicate_events_not_reused(self):
        tail,rows=traces();m=NativeMatcher(77)
        for r in rows[:100]:m.match(r,tail)
        before=m.counts['matched'];self.assertIsNone(m.match(rows[99],tail));self.assertEqual(m.counts['matched'],before)
    def test_same_native_cannot_match_twice(self):
        tail,rows=traces();m=NativeMatcher(77)
        for r in rows[:100]:m.match(r,tail)
        again=dict(rows[99],QPCTime=str((tail.times[99]+.010)/1000))
        self.assertIsNone(m.match(again,tail));self.assertEqual(m.counts['duplicate_or_reordered_native'],1)
    def test_competing_chain_latches_off(self):
        tail,rows=traces();m=NativeMatcher(77)
        for r in rows[:100]:m.match(r,tail)
        rows[100]['SwapChainAddress']='0x7654';self.assertIsNone(m.match(rows[100],tail));self.assertIsNotNone(m.fatal)
        self.assertIsNone(m.match(rows[101],tail));self.assertFalse(m.locked)
    def test_competing_chain_during_learning_refused(self):
        tail,rows=traces();m=NativeMatcher(77);m.match(rows[0],tail)
        rows[1]['SwapChainAddress']='0x7654';m.match(rows[1],tail);self.assertIsNotNone(m.fatal)
    def test_repeated_ids_do_not_warmup(self):
        tail,rows=traces();m=NativeMatcher(77)
        for r in tail.records:r['id']='1'
        self.assertTrue(all(m.match(r,tail) is None for r in rows))
    def test_generation_transition_keeps_mapping_not_heartbeat_oscillation(self):
        tail,rows=traces();m=NativeMatcher(77)
        for r in rows[:100]:m.match(r,tail)
        tail.records[100]['generation']='2';tail.records[100]['paced']='1'
        self.assertIsNotNone(m.match(rows[100],tail));self.assertTrue(m.locked)
        tail.records[101]['generation']='3'
        self.assertIsNotNone(m.match(rows[101],tail));self.assertTrue(m.locked)
    def test_minimum_span(self):
        tail,rows=traces(120,period=1.0);m=NativeMatcher(77)
        self.assertTrue(all(m.match(r,tail) is None for r in rows));self.assertFalse(m.locked)
    def test_generation_discontinuity_prevents_initial_binding(self):
        tail,rows=traces();m=NativeMatcher(77)
        for i,r in enumerate(tail.records):r['generation']=str(i)
        self.assertTrue(all(m.match(r,tail) is None for r in rows))
    def test_missing_native_during_learning_restarts_streak(self):
        tail,rows=traces();m=NativeMatcher(77)
        for r in rows[:80]:m.match(r,tail)
        self.assertTrue(all(m.match(r,tail) is None for r in rows[81:]))
    def test_separate_sessions_do_not_reuse_association(self):
        tail,rows=traces();m=NativeMatcher(77)
        for r in rows:m.match(r,tail)
        new=NativeMatcher(77);self.assertIsNone(new.match(rows[130],tail));self.assertFalse(new.locked)
    def test_summary_explicit_correlation_not_identity(self):
        self.assertIn('not_com_identity_proof',NativeMatcher(77).summary()['method'])
if __name__=='__main__':unittest.main(verbosity=2)
