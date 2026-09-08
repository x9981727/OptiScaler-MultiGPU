"""Apply the v26.1 controller-only fix to the checksum-verified v26 source."""
from pathlib import Path
import hashlib
p=Path(__file__).parent/'game_feedback.py'
s=p.read_text(encoding='utf-8')
# The pinned file is extracted from the preceding successful CI artifact.
expected='09a21045238c86453bbf9f73ae136e3482c04f96b5a83000d88302b0d245b3c3'
if hashlib.sha256(p.read_bytes()).hexdigest()!=expected:
    raise RuntimeError('Unrecognized v26 controller source; refusing text edits')
def replace(old,new):
    global s
    if s.count(old)!=1: raise RuntimeError('Expected one patch anchor: '+old[:80])
    s=s.replace(old,new)
replace('import queue, statistics, subprocess, sys, threading, time',
        'import queue, statistics, subprocess, sys, threading, time\nfrom correlation import NativeMatcher')
replace("parser.add_argument('--pid',type=int); parser.add_argument('--seconds',type=int,default=180)",
        "parser.add_argument('--pid',type=int); parser.add_argument('--seconds',type=int,default=180)\n    parser.add_argument('--observe-only',action='store_true',help='Record and verify correlation; never enable extra waits')")
replace("control=folder/'phase-control.txt'; c=Controller(); native=Tail(folder/'native-live.csv'); source=Tail(folder/'source-live.csv')",
        "control=folder/'phase-control.txt'; c=Controller(); native=Tail(folder/'native-live.csv'); source=Tail(folder/'source-live.csv')\n    matcher=NativeMatcher(pid); pending=collections.deque(); last_stats=0.; active_heartbeats=0")
replace("stamp=time.strftime('%Y%m%d-%H%M%S'); out=folder/('capture-'+stamp);out.mkdir()",
        "stamp=time.strftime('%Y%m%d-%H%M%S'); out=folder/('capture-v261-'+stamp);out.mkdir()")
replace("print('XeFG v26 GAME TEST. Return to the game now. Ctrl+C stops extra waits.',flush=True)",
        "print('XeFG v26.1 CONTROLLER HOTFIX. Game DLL unchanged. Ctrl+C stops extra waits.',flush=True)\n    print('OBSERVE ONLY - extra waits disabled' if args.observe_only else 'EXPERIMENTAL AUTO - return to the game now',flush=True)")
replace("required={'QPCTime','msUntilDisplayed','Dropped','SwapChainAddress'}",
        "required={'QPCTime','msUntilDisplayed','Dropped','SwapChainAddress','ProcessID','Application','Runtime'}")
replace("r=native.match(row)\n                    if r is not None:c.consume(r,source)",
        "pending.append((now,row))\n                # DLL trace files flush in blocks of eight. Retry briefly when\n                # ETW arrives before the corresponding native block is on disk.\n                while pending:\n                    arrival,event=pending[0]\n                    try: event_ms=float(event['QPCTime'])*1000.0\n                    except (KeyError,ValueError):event_ms=float('nan')\n                    if (not native.times or (math.isfinite(event_ms) and native.times[-1]<event_ms)) and now-arrival<500:\n                        break\n                    pending.popleft()\n                    r=matcher.match(event,native)\n                    if r is not None:c.consume(r,source)\n                if matcher.fatal and c.stage!='stopped':\n                    c.disable('unsafe event association: '+matcher.fatal)")
replace("enabled=c.stage=='active' and fresh and c.foreground",
        "enabled=(not args.observe_only and matcher.locked and not matcher.fatal\n                             and c.stage=='active' and fresh and c.foreground)\n                    if enabled: active_heartbeats+=1")
replace("if c.reason!=last_notice:\n                    print(c.reason,flush=True);last_notice=c.reason",
        "if c.reason!=last_notice:\n                    print(c.reason,flush=True);last_notice=c.reason\n                if now-last_stats>=2000:\n                    last_stats=now\n                    mode='observe-only' if args.observe_only else c.stage\n                    print(f'rows={count} matched={c.matched} association={matcher.locked} mode={mode} phase_ms={c.phase:.3f} last={matcher.last_reason}',flush=True)")
replace("report={'kind':'v26_synchronous_game_native_pacing_experiment'",
        "report={'controller_revision':'26.1','association':matcher.summary(),'observe_only':args.observe_only,'enabled_heartbeat_count':active_heartbeats,'pending_events_at_exit':len(pending),'kind':'v26_synchronous_game_native_pacing_experiment'")
p.write_text(s,encoding='utf-8',newline='\n')
print('Applied v26.1 controller-only fix. Pacing math, DLL and game INI unchanged.')
