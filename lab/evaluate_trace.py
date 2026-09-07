"""PresentMon v1-metrics timing gate; a pass is not hardware/image certification.
Residence is assigned to the current row after reconstructing display start as
TimeInMs + MsUntilDisplayed. Tearing can mean only partial frame scanout.
"""
from __future__ import annotations
import argparse,csv,hashlib,json,math,statistics
from pathlib import Path
from collections import Counter
APP='Application'
GEN='Intel XeSS-FG'
REQUIRED={'Application','ProcessID','SwapChainAddress','TimeInMs','MsUntilDisplayed','FrameType'}
def num(value):
    try:
        out=float(value)
        return out if math.isfinite(out) else None
    except (ValueError,TypeError):return None

def percentile(values,p):
    if not values:return None
    a=sorted(values);q=(len(a)-1)*p;k=int(q)
    return a[k]+(a[min(k+1,len(a)-1)]-a[k])*(q-k)

def summary(a):
    return {'count':len(a),'mean':statistics.fmean(a) if a else None,'p50':percentile(a,.5),'p95':percentile(a,.95),'p99':percentile(a,.99),'max':max(a) if a else None}

def evaluate(path:Path,target=67.,warmup=5.,min_seconds=30.,process='wwm.exe'):
    if not math.isfinite(target) or target<=0 or warmup<0 or min_seconds<=0:raise ValueError('Invalid target or capture window')
    with path.open(encoding='utf-8-sig',newline='') as stream:
        reader=csv.DictReader(stream)
        if not REQUIRED.issubset(reader.fieldnames or []):raise ValueError('Required PresentMon v1 display columns missing')
        rows=[r for r in reader if r['Application'].casefold()==process.casefold()]
    if not rows:raise ValueError('No target process rows')
    streams={(r['ProcessID'],r['SwapChainAddress']) for r in rows}
    if len(streams)!=1:raise ValueError('Multiple process/swapchain streams; split explicitly, do not silently choose fastest')
    if any(num(r['TimeInMs']) is None for r in rows):raise ValueError('Invalid present timestamp')
    times=[num(r['TimeInMs']) for r in rows]
    if any(b<a for a,b in zip(times,times[1:])):raise ValueError('Present timestamps reversed')
    start=min(times)+warmup*1000;end=max(times);seconds=(end-start)/1000
    if seconds<=0:raise ValueError('Empty measurement window')
    sampled=[r for r in rows if start<=num(r['TimeInMs'])<=end]
    valid=[];unknown=0;dropped=0;types=Counter()
    for r in sampled:
        types[r['FrameType']]+=1;delay=num(r['MsUntilDisplayed'])
        if delay is None:unknown+=1;continue
        if delay<0:raise ValueError('Negative display latency')
        if delay==0:dropped+=1;continue
        valid.append((num(r['TimeInMs'])+delay,r['FrameType'],delay,num(r.get('AllowsTearing'))))
    valid.sort(key=lambda x:x[0]);counts=Counter(v[1] for v in valid)
    if not counts[APP] or not counts[GEN]:raise ValueError('Original/generated display events not both available')
    residences={APP:[],GEN:[]};intervals=[];shares=[]
    for i in range(len(valid)-1):
        duration=valid[i+1][0]-valid[i][0]
        if duration<=0:raise ValueError('Duplicate/reversed display timestamp')
        intervals.append(duration)
        if valid[i][1] in residences:residences[valid[i][1]].append(duration)
        if i+2<len(valid) and [valid[j][1] for j in (i,i+1,i+2)]==[APP,GEN,APP]:
            pair=valid[i+2][0]-valid[i][0];shares.append((valid[i+2][0]-valid[i+1][0])/pair)
    source=counts[APP]/seconds;generated=counts[GEN]/seconds;total=len(valid)/seconds
    multiplier=len(valid)/counts[APP];source_ratio=source/target;half=500./target
    # All complete windows, including original-only fallbacks, are retained.
    windows=[]
    for k in range(int(seconds//5)):
        lo=start+k*5000;hi=lo+5000;c=Counter(v[1] for v in valid if lo<=v[0]<hi)
        windows.append({'start_seconds_after_warmup':k*5,'source_fps':c[APP]/5,'generated_fps':c[GEN]/5})
    reasons=[]
    if seconds<min_seconds:reasons.append('CAPTURE_TOO_SHORT')
    if not 0.98<=source_ratio<=1.50:reasons.append('SOURCE_RATE_NOT_PRESERVED')
    if not 1.99<=multiplier<=2.01:reasons.append('DISPLAYED_MULTIPLIER_NOT_2X')
    if total<target*1.96:reasons.append('DISPLAYED_RATE_BELOW_TARGET')
    if unknown:reasons.append('DISPLAY_DATA_INCOMPLETE')
    if dropped/max(1,len(sampled))>.005:reasons.append('DISPLAY_DROPS')
    if set(types)-{APP,GEN}:reasons.append('UNEXPECTED_FRAME_TYPES')
    if not shares or not .35<=percentile(shares,.5)<=.65 or percentile(shares,.95)>.80 or percentile(shares,.05)<.20:reasons.append('UNEVEN_ORIGINAL_GENERATED_RESIDENCE')
    if percentile(intervals,.99)>half*1.8:reasons.append('DISPLAY_INTERVAL_TAIL_TOO_LONG')
    if windows and min(w['source_fps'] for w in windows)<target*.90:reasons.append('SUSTAINED_SLOW_WINDOW')
    return {'kind':'observed_presentmon_trace','file':path.name,'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
        'target_source_fps':target,'warmup_seconds':warmup,'measurement_seconds':seconds,
        'source_display_fps':source,'generated_display_fps':generated,'total_display_events_fps':total,
        'displayed_multiplier':multiplier,'source_retention_vs_target':source_ratio,'frame_counts':dict(counts),
        'known_dropped_rows':dropped,'unknown_display_rows':unknown,
        'original_residence_ms':summary(residences[APP]),'generated_residence_ms':summary(residences[GEN]),
        'generated_share_of_pair':summary(shares),'display_intervals_ms':summary(intervals),
        'tearing_allowed':any(v[3]==1 for v in valid),'windows':windows,'timing_gate_passed':not reasons,
        'timing_failures':reasons,'hardware_flicker_checked':False,'final_release_allowed':False,
        'unverified':['same-scene FG-off measured baseline','GPU-specific saturation','frame pixels/flicker/ghosting','end-to-end input latency','candidate binary identity'],
        'note':'Timing-only pass is never final certification. These are display changes; tearing may allow only partial frames.'}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('csv',type=Path);p.add_argument('--target-source-fps',type=float,default=67.)
    p.add_argument('--warmup-seconds',type=float,default=5.);p.add_argument('--min-seconds',type=float,default=30.)
    p.add_argument('--process',default='wwm.exe');p.add_argument('--output',type=Path);p.add_argument('--strict',action='store_true')
    a=p.parse_args()
    try:r=evaluate(a.csv,a.target_source_fps,a.warmup_seconds,a.min_seconds,a.process)
    except (ValueError,OSError) as e:r={'timing_gate_passed':False,'final_release_allowed':False,'error':str(e)}
    text=json.dumps(r,ensure_ascii=False,indent=2,allow_nan=False)+'\n'
    if a.output:a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(text,encoding='utf-8')
    print(text,end='')
    if 'error' in r:return 3
    return 2 if a.strict and not r['timing_gate_passed'] else 0
if __name__=='__main__':raise SystemExit(main())
