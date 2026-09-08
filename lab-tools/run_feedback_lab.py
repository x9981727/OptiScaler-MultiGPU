"""Run only the bundled XeFG sample. Never install anything into a game.
Default controller is the original ETW latency model. Other models remain
explicit experiments. This script does not prove optical or game correctness.
"""
from __future__ import annotations
import argparse, ctypes as C, ctypes.wintypes as W
import hashlib, json, os, subprocess, sys, time
from pathlib import Path
from analyze_feedback import analyze

EXE_SHA='bd3ea992e388dc809635727d5e41023b48c08e788a74c2a512d289579c9fb415'
PM_SHA='9bec3083069f58f911e6a512f4806db51a27bd096103087bc1d05ef54c80a191'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--gpu',type=int,default=0)
    p.add_argument('--fps',type=float,default=67)
    p.add_argument('--seconds',type=int,default=60)
    p.add_argument('--width',type=int,default=3840)
    p.add_argument('--height',type=int,default=2160)
    p.add_argument('--controller',choices=['original','latency','midpoint','off'],default='original')
    p.add_argument('--dynamic-period',action='store_true',help='Experimental: feedback-coupled period has shown source FPS loss')
    p.add_argument('--dry-run',action='store_true')
    a=p.parse_args()
    if not 5<=a.seconds<=60 or not 30<=a.fps<=100 or a.gpu<0 or not 64<=a.width<=7680 or not 64<=a.height<=4320:
        p.error('Invalid bounded test arguments')
    root=Path(__file__).resolve().parent.parent
    exe=root/'bin/Release/basic_xess_fg_sample.exe'
    pm=root/'tools/PresentMon-2.5.1-x64.exe'
    for path,expected in [(exe,EXE_SHA),(pm,PM_SHA)]:
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=expected:
            raise SystemExit('Missing or changed verified binary: '+str(path))
    if a.dry_run:
        print(json.dumps({'kind':'dry_run_no_program_started','sample':str(exe),'presentmon':str(pm),'args':vars(a)}));return 0
    if os.name!='nt':raise SystemExit('Windows only')
    out=root/'results'/(time.strftime('%Y%m%d-%H%M%S')+'-'+a.controller+'-'+str(a.fps))
    out.mkdir(parents=True,exist_ok=False)
    user=C.WinDLL('user32',use_last_error=True)
    C.windll.kernel32.SetErrorMode(3)
    user.GetForegroundWindow.restype=W.HWND
    user.GetWindowThreadProcessId.argtypes=[W.HWND,C.POINTER(W.DWORD)]
    user.PostMessageW.argtypes=[W.HWND,W.UINT,W.WPARAM,W.LPARAM]
    user.GetWindowTextW.argtypes=[W.HWND,W.LPWSTR,C.c_int]
    user.IsWindowVisible.argtypes=[W.HWND]
    user.SetProcessDPIAware()
    callback=C.WINFUNCTYPE(W.BOOL,W.HWND,W.LPARAM)
    def windows(pid):
        result=[]
        def visit(h,_):
            owner=W.DWORD();user.GetWindowThreadProcessId(h,C.byref(owner))
            if owner.value==pid and user.IsWindowVisible(h):
                text=C.create_unicode_buffer(512);user.GetWindowTextW(h,text,512)
                result.append({'hwnd':int(h),'title':text.value})
            return True
        user.EnumWindows(callback(visit),0);return result
    env={k:v for k,v in os.environ.items() if not k.startswith('XEFG_LAB_')}
    env.update(XEFG_LAB_TRACE_DIR=str(out),XEFG_LAB_ETW_PHASE='1',XEFG_LAB_AUTO_PHASE='0',
               XEFG_LAB_GATE='1',XEFG_LAB_DEMOTE_SDK='1',XEFG_LAB_DISPLAY_PRIORITY='100',
               XEFG_LAB_SLEEP_BYPASS='1',XEFG_LAB_HINT_MS='9',XEFG_LAB_EVEN_PHASE_MS='0')
    if not a.dynamic_period:env['XEFG_LAB_OUTPUT_PERIOD_MS']=str(1000/a.fps)
    command=[str(exe),'-gpu_id',str(a.gpu),'-width',str(a.width),'-height',str(a.height),
             '-borderless','-tag_interpolated_frames','0','-max_frames','100000','-fps',str(a.fps)]
    record={'kind':'isolated_single_adapter_lab_not_game_acceptance','sample_sha256':EXE_SHA,
            'command':command,'experimental_env':{k:v for k,v in env.items() if k.startswith('XEFG_LAB_')},
            'requested':vars(a),'game_modified':False,'observations':[]}
    sample=None;control=None;failure=None
    try:
        sample=subprocess.Popen(command,cwd=exe.parent,env=env)
        record['sample_pid']=sample.pid
        end=time.monotonic()+20
        while time.monotonic()<end and not windows(sample.pid):
            if sample.poll() is not None:raise RuntimeError('Sample failed at initialization')
            time.sleep(.2)
        if not windows(sample.pid):raise RuntimeError('No visible sample window')
        time.sleep(5)
        script='display_feedback.py' if a.controller=='original' else 'display_feedback_v3.py'
        cmd=[sys.executable,str(root/'tools'/script),str(out),str(sample.pid),str(a.seconds),str(pm)]
        if a.controller=='off':cmd.append('--observe-only')
        elif a.controller in ('latency','midpoint'):cmd+=['--model',a.controller]
        record['capture_command']=cmd
        with (out/'controller-stdout.txt').open('w',encoding='utf-8') as so,(out/'controller-stderr.txt').open('w',encoding='utf-8') as se:
            control=subprocess.Popen(cmd,cwd=root/'tools',stdout=so,stderr=se)
            end=time.monotonic()+a.seconds+15
            while control.poll() is None:
                owner=W.DWORD();user.GetWindowThreadProcessId(user.GetForegroundWindow(),C.byref(owner))
                record['observations'].append({'seconds':time.monotonic(),'foreground_pid':owner.value,'alive':sample.poll() is None})
                if time.monotonic()>end:raise TimeoutError('Controller watchdog')
                time.sleep(.5)
            record['presentmon_exit']=control.returncode
            if control.returncode:raise RuntimeError('Controller capture failed')
        record['window_end']=windows(sample.pid)
        record['foreground_valid']=all(x['foreground_pid']==sample.pid and x['alive'] for x in record['observations'])
        if not record['foreground_valid']:failure='Foreground changed: benchmark invalid'
    except (Exception,KeyboardInterrupt) as e:failure=f'{type(e).__name__}: {e}'
    finally:
        if control is not None and control.poll() is None:
            subprocess.run(['taskkill','/PID',str(control.pid),'/T','/F'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=5)
        if sample is not None and sample.poll() is None:
            for w in windows(sample.pid):user.PostMessageW(w['hwnd'],0x0010,0,0)
            try:sample.wait(timeout=8)
            except subprocess.TimeoutExpired:
                sample.terminate();sample.wait(timeout=5);record['forced_close']=True
        record['sample_exit']=sample.returncode if sample else None
        record['failure']=failure
        (out/'metadata.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
    if failure is None:
        try:analyze(out,a.seconds,a.fps)
        except Exception as e:failure='Analysis: '+str(e)
    print(json.dumps({'results':str(out),'failure':failure,'game_modified':False},ensure_ascii=False))
    return 1 if failure else 0

if __name__=='__main__':raise SystemExit(main())
