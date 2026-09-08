from pathlib import Path
import hashlib

p=Path(__file__).with_name('game_feedback.py')
original=p.read_bytes().replace(b'\r\n',b'\n')
expected='01ce1ac2faf8c8eba25fe1543abfffd9a7119566205c2b6c70c8bfc34ae6650a'
if hashlib.sha256(original).hexdigest()!=expected:
    raise SystemExit('Unexpected v26.1 game_feedback.py; refusing to patch')
s=original.decode('utf-8')

anchor="def locate_session(pid: int | None):\n"
if s.count(anchor)!=1: raise SystemExit('locate_session anchor mismatch')
helpers=r'''def _rank_focus_candidates(candidates):
    """Deterministic chooser for visible top-level windows owned by the game."""
    if not candidates:
        return None
    # Prefer unowned windows, non-empty titles, and then the largest visible area.
    return max(candidates, key=lambda x: (not x['owned'], bool(x['title'].strip()), x['area'], -int(x['hwnd'])))

def handoff_game_focus(pid: int):
    """Best-effort one-shot foreground handoff. Never changes game files/settings."""
    from ctypes import wintypes
    user=ctypes.WinDLL('user32',use_last_error=True)
    EnumProc=ctypes.WINFUNCTYPE(wintypes.BOOL,wintypes.HWND,wintypes.LPARAM)
    class RECT(ctypes.Structure):
        _fields_=[('left',ctypes.c_long),('top',ctypes.c_long),('right',ctypes.c_long),('bottom',ctypes.c_long)]
    user.GetForegroundWindow.restype=wintypes.HWND
    user.GetWindowThreadProcessId.argtypes=[wintypes.HWND,ctypes.POINTER(wintypes.DWORD)]
    user.GetWindowThreadProcessId.restype=wintypes.DWORD
    user.EnumWindows.argtypes=[EnumProc,wintypes.LPARAM]; user.EnumWindows.restype=wintypes.BOOL
    user.IsWindowVisible.argtypes=[wintypes.HWND]; user.IsWindowVisible.restype=wintypes.BOOL
    user.IsIconic.argtypes=[wintypes.HWND]; user.IsIconic.restype=wintypes.BOOL
    user.GetWindowRect.argtypes=[wintypes.HWND,ctypes.POINTER(RECT)]; user.GetWindowRect.restype=wintypes.BOOL
    user.GetWindowTextLengthW.argtypes=[wintypes.HWND]; user.GetWindowTextLengthW.restype=ctypes.c_int
    user.GetWindowTextW.argtypes=[wintypes.HWND,wintypes.LPWSTR,ctypes.c_int]; user.GetWindowTextW.restype=ctypes.c_int
    user.GetWindow.argtypes=[wintypes.HWND,wintypes.UINT]; user.GetWindow.restype=wintypes.HWND
    user.ShowWindowAsync.argtypes=[wintypes.HWND,ctypes.c_int]; user.ShowWindowAsync.restype=wintypes.BOOL
    user.SetForegroundWindow.argtypes=[wintypes.HWND]; user.SetForegroundWindow.restype=wintypes.BOOL
    user.BringWindowToTop.argtypes=[wintypes.HWND]; user.BringWindowToTop.restype=wintypes.BOOL

    def handle_value(hwnd):
        if hwnd is None: return 0
        if isinstance(hwnd,int): return hwnd
        return int(getattr(hwnd,'value',0) or 0)
    def window_pid(hwnd):
        value=wintypes.DWORD()
        user.GetWindowThreadProcessId(hwnd,ctypes.byref(value))
        return int(value.value)

    current=user.GetForegroundWindow()
    current_value=handle_value(current)
    if current_value and window_pid(current)==pid:
        return {'attempted':False,'ok':True,'reason':'already_foreground','hwnd':current_value,'candidates':0}

    candidates=[]
    @EnumProc
    def collect(hwnd,_):
        try:
            if not user.IsWindowVisible(hwnd) or window_pid(hwnd)!=pid:
                return True
            rect=RECT()
            if not user.GetWindowRect(hwnd,ctypes.byref(rect)):
                return True
            width=max(0,int(rect.right-rect.left)); height=max(0,int(rect.bottom-rect.top))
            area=width*height
            if area<640*360:
                return True
            length=max(0,user.GetWindowTextLengthW(hwnd)); buf=ctypes.create_unicode_buffer(length+1)
            if length:user.GetWindowTextW(hwnd,buf,length+1)
            owner=user.GetWindow(hwnd,4) # GW_OWNER
            candidates.append({'hwnd':handle_value(hwnd),'area':area,'title':buf.value,'owned':bool(handle_value(owner)),'iconic':bool(user.IsIconic(hwnd))})
        except Exception:
            pass
        return True
    user.EnumWindows(collect,0)
    target=_rank_focus_candidates(candidates)
    if target is None:
        return {'attempted':True,'ok':False,'reason':'no_visible_game_window','hwnd':0,'candidates':0}
    hwnd=wintypes.HWND(target['hwnd'])
    if target['iconic']:
        user.ShowWindowAsync(hwnd,9) # SW_RESTORE
        time.sleep(.05)
    user.BringWindowToTop(hwnd)
    requested=bool(user.SetForegroundWindow(hwnd))
    deadline=time.monotonic()+1.0
    while time.monotonic()<deadline:
        foreground=user.GetForegroundWindow()
        if foreground and handle_value(foreground)==target['hwnd']:
            return {'attempted':True,'ok':True,'reason':'foreground_handoff_succeeded','hwnd':target['hwnd'],'candidates':len(candidates),'setforeground_return':requested}
        time.sleep(.05)
    return {'attempted':True,'ok':False,'reason':'windows_rejected_foreground_handoff','hwnd':target['hwnd'],'candidates':len(candidates),'setforeground_return':requested}


'''
s=s.replace(anchor,helpers+anchor)

old="    parser.add_argument('--observe-only',action='store_true',help='Record and verify correlation; never enable extra waits')\n"
new=old+"    parser.add_argument('--no-auto-focus',action='store_true',help='Do not request a one-shot foreground handoff to wwm.exe')\n"
if s.count(old)!=1: raise SystemExit('arg anchor mismatch')
s=s.replace(old,new)

old="    folder,meta=locate_session(args.pid); pid=int(meta['pid']); clock=WindowsClock()\n"
new="""    folder,meta=locate_session(args.pid); pid=int(meta['pid']); clock=WindowsClock()
    focus_result={'attempted':False,'ok':False,'reason':'disabled_by_user','hwnd':0,'candidates':0}
    if not args.no_auto_focus:
        try: focus_result=handoff_game_focus(pid)
        except Exception as exc: focus_result={'attempted':True,'ok':False,'reason':f'focus_helper_error:{type(exc).__name__}','hwnd':0,'candidates':0}
"""
if s.count(old)!=1: raise SystemExit('session anchor mismatch')
s=s.replace(old,new)

s=s.replace("stamp=time.strftime('%Y%m%d-%H%M%S'); out=folder/('capture-v261-'+stamp);out.mkdir()",
            "stamp=time.strftime('%Y%m%d-%H%M%S'); out=folder/('capture-v262-'+stamp);out.mkdir()")
s=s.replace("XeFG v26.1 CONTROLLER HOTFIX.", "XeFG v26.2 AUTO-FOCUS HOTFIX.")
s=s.replace("Eight-second foreground baseline; then bounded automatic phase correction.",
            "One-shot game-focus handoff, then eight-second eligible foreground baseline and bounded automatic phase correction.")

old="    print('No source-rate or image-quality guarantee. Log folder:',out,flush=True)\n"
new=old+"""    print(f"focus_handoff ok={focus_result.get('ok')} reason={focus_result.get('reason')} hwnd={focus_result.get('hwnd')}",flush=True)
    if not focus_result.get('ok'):
        print('GAME FOCUS REQUIRED: click the game window and leave it foreground until mode=active.',flush=True)
"""
if s.count(old)!=1: raise SystemExit('print anchor mismatch')
s=s.replace(old,new)

old="    messages=queue.Queue(maxsize=20000); proc=None; code=None; count=0; error=None; last_publish=0; last_notice=''; last_refresh=0\n"
new="    messages=queue.Queue(maxsize=20000); proc=None; code=None; count=0; error=None; last_publish=0; last_notice=''; last_refresh=0; last_focus_warning=-1e30\n"
if s.count(old)!=1: raise SystemExit('state anchor mismatch')
s=s.replace(old,new)

old="                    print(f'rows={count} matched={c.matched} association={matcher.locked} mode={mode} phase_ms={c.phase:.3f} last={matcher.last_reason}',flush=True)\n"
new=old+"""                if matcher.locked and c.matched>=100 and c.stage=='baseline' and not c.foreground and now-last_focus_warning>=3000:
                    last_focus_warning=now
                    print('GAME NOT ELIGIBLE/FOREGROUND - click wwm.exe; no extra wait is being applied.',flush=True)
"""
if s.count(old)!=1: raise SystemExit('stats anchor mismatch')
s=s.replace(old,new)

s=s.replace("'controller_revision':'26.1'", "'controller_revision':'26.2'")
old="'association':matcher.summary(),'observe_only':args.observe_only,"
new="'association':matcher.summary(),'focus_handoff':focus_result,'observe_only':args.observe_only,"
if s.count(old)!=1: raise SystemExit('report anchor mismatch')
s=s.replace(old,new)

out=s.encode('utf-8')
final='ab1b47c8cb635a72c27a6e3cd6404f8f5d4edd12ecd26b5885979a46b4a027d1'
if hashlib.sha256(out).hexdigest()!=final:
    raise SystemExit('v26.2 output hash mismatch: '+hashlib.sha256(out).hexdigest())
p.write_bytes(out)
print('Patched v26.1 -> v26.2',final)
