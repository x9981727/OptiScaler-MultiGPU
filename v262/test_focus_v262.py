import unittest
import game_feedback as gf

class FocusRankingTests(unittest.TestCase):
    def test_prefers_unowned_nonempty_large_window(self):
        rows=[
            {'hwnd':3,'area':8000000,'title':'','owned':False,'iconic':False},
            {'hwnd':2,'area':5000000,'title':'Where Winds Meet','owned':False,'iconic':False},
            {'hwnd':1,'area':9000000,'title':'Owned','owned':True,'iconic':False},
        ]
        self.assertEqual(gf._rank_focus_candidates(rows)['hwnd'],2)

    def test_larger_window_wins_after_safety_rank(self):
        rows=[
            {'hwnd':11,'area':1000000,'title':'Game','owned':False,'iconic':False},
            {'hwnd':12,'area':2000000,'title':'Game','owned':False,'iconic':True},
        ]
        self.assertEqual(gf._rank_focus_candidates(rows)['hwnd'],12)

    def test_empty_returns_none(self):
        self.assertIsNone(gf._rank_focus_candidates([]))

class ControllerFocusGateTests(unittest.TestCase):
    def test_ineligible_never_arms_or_paces(self):
        c=gf.Controller()
        for i in range(300):
            c.consume({'qpc_ms':str(i*16.0),'eligible':'0','latency':10,'paced':'0','id':str(i+1),'generation':'1','ordinal':'0','phase_ms':'0'},None)
        self.assertEqual(c.stage,'baseline')
        self.assertEqual(c.phase,0.0)
        self.assertIn('foreground',c.reason)

if __name__=='__main__':
    unittest.main()
