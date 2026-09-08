"""Isolated reference controller, not a game installer or background service.
Run for a bounded capture; only writes phase-control.txt in the supplied lab
folder. Output period, shaders, GPU settings and game files are never changed.
"""
from pathlib import Path
import bisect, collections, csv, ctypes, json, math, os, statistics, subprocess, sys, time
out=Path(sys.argv[1]);pid=int(sys.argv[2]);seconds=int(sys.argv[3]);pm=Path(sys.argv[4])
if not 3<=seconds<=180 or not out.is_dir():raise ValueError('Invalid bounded capture')
freq=ctypes.c_longlong();qpc=ctypes.c_longlong();kernel=ctypes.WinDLL('kernel32')
kernel.QueryPerformanceFrequency(ctypes.byref(freq))
def now_ms():
 kernel.QueryPerformanceCounter(ctypes.byref(qpc));return qpc.value*1000/freq.value
cmd=[str(pm),'--process_id',str(pid),'--output_stdout','--v1_metrics','--qpc_time_ms',
 '--no_console_stats','--session_name','XeFGFeedback-'+str(pid),'--timed',str(seconds),'--terminate_after_timed']
records=[];times=[];position=0;last_read=0;last_update=-1e30;phase=0.0;sequence=0
pairs=collections.deque(maxlen=96);last_match=None;matched=unmatched=rows=0;updates=[]
errors=[]
def refresh():
 global position,last_read
 if time.monotonic()-last_read<0.10:return
 last_read=time.monotonic();p=out/'native-live.csv'
 if not p.exists():return
 with p.open('r',encoding='utf-8') as f:
  f.seek(position)
  while True:
   start=f.tell();line=f.readline()
   if not line:break
   if not line.endswith('\n'):f.seek(start);break
   try:
    values=line.strip().split(',');r={'id':int(values[0]),'qpc_ms':float(values[1]),'period':float(values[2]),'phase':float(values[3]),'result':int(values[4])}
    if r['result']==0 and (not times or r['qpc_ms']>=times[-1]):records.append(r);times.append(r['qpc_ms'])
   except (ValueError,IndexError):errors.append('malformed_native_record')
  position=f.tell()
def write_phase(value):
 global sequence
 sequence+=1;stamp=now_ms();p=out/'phase-control.txt';temp=out/'phase-control.pending'
 temp.write_text(f'1 {value:.6f} {stamp:.6f}\n',encoding='ascii');os.replace(temp,p)
 return stamp
def consume(row):
 global matched,unmatched,last_match,last_update,phase
 refresh()
 try:
  # PresentMon v1's QPCTime is expressed in seconds even with --qpc_time_ms.
  # Every sample must match our absolute native call clock within 0.5 ms;
  # unsupported schemas/scales fail closed rather than silently misaligning IDs.
  native_time=float(row['QPCTime'])*1000;latency=float(row['msUntilDisplayed'])
  if row['Dropped']!='0' or not math.isfinite(latency) or latency<0:return
 except (ValueError,KeyError):unmatched+=1;return
 j=bisect.bisect_left(times,native_time)
 candidates=[i for i in (j-1,j) if 0<=i<len(times)]
 if not candidates:unmatched+=1;return
 i=min(candidates,key=lambda i:abs(times[i]-native_time))
 if abs(times[i]-native_time)>0.5:unmatched+=1;return
 r=dict(records[i]);r['latency']=latency;r['match_error']=abs(times[i]-native_time);matched+=1
 if last_match and r['id']==last_match['id']+1 and r['id']%2==0:
  difference=latency-last_match['latency']
  if abs(difference)<r['period']*.8 and abs(r['phase']-phase)<.05:
   pairs.append((r['qpc_ms'],difference,r['phase'],r['period']))
 last_match=r
 fresh=[x for x in pairs if r['qpc_ms']-x[0]<1600 and abs(x[2]-phase)<.05]
 if len(fresh)<24 or r['qpc_ms']-last_update<800:return
 target=-statistics.median(x[1] for x in fresh);bound=min(4.0,r['period']*.24)
 target=max(-bound,min(bound,target));delta=max(-.35,min(.35,(target-phase)*.5))
 if abs(delta)<.04:return
 old=phase;phase=max(-bound,min(bound,phase+delta));created=write_phase(phase)
 updates.append({'feedback_qpc_ms':r['qpc_ms'],'control_written_qpc_ms':created,'old_phase_ms':old,'new_phase_ms':phase,'target_ms':target,'pairs':len(fresh),'last_native_id':r['id']})
 last_update=r['qpc_ms'];pairs.clear()
 with (out/'feedback-updates.jsonl').open('a',encoding='utf-8') as f:f.write(json.dumps(updates[-1])+'\n')
proc=None
try:
 with (out/'feedback-pm-stderr.txt').open('wb') as stderr,(out/'capture.csv').open('w',encoding='utf-8',newline='') as output:
  proc=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=stderr,text=True,encoding='utf-8-sig',errors='replace',bufsize=1)
  reader=csv.DictReader(proc.stdout);writer=None
  for row in reader:
   if writer is None:writer=csv.DictWriter(output,fieldnames=reader.fieldnames);writer.writeheader()
   writer.writerow(row);rows+=1
   try:consume(row)
   except Exception as error:errors.append(type(error).__name__+': '+str(error))
  code=proc.wait(timeout=15)
finally:
 if proc is not None and proc.poll() is None:proc.terminate();proc.wait(timeout=5)
report={'kind':'isolated_ETW_phase_feedback','process_id':pid,'requested_seconds':seconds,'rows':rows,'matched_native_calls':matched,'unmatched_rows':unmatched,'changes':updates,'errors':errors,'final_phase_ms':phase,'presentmon_exit':code,'period_changed':False,'game_modified':False,'convergence_is_not_game_acceptance':True}
(out/'feedback-report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report),flush=True)
