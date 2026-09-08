"""Reconcile native submission and legacy PresentMon on one absolute QPC clock.
No frame-type label is interpreted as a source or generated-image proof.
"""
from __future__ import annotations
import argparse, bisect, collections, csv, hashlib, json, math, statistics
from pathlib import Path


def readcsv(path):
    with path.open(encoding='utf-8-sig', newline='') as f:
        return list(csv.DictReader(f))


def percentile(values, p):
    if not values:return None
    x=sorted(values);i=(len(x)-1)*p;j=int(i)
    return x[j]+(x[min(j+1,len(x)-1)]-x[j])*(i-j)


def dist(values):
    return {'n':len(values),'mean_ms':statistics.mean(values) if values else None,
            'median_ms':statistics.median(values) if values else None,
            'p05_ms':percentile(values,.05),'p95_ms':percentile(values,.95),
            'p99_ms':percentile(values,.99),'min_ms':min(values,default=None),
            'max_ms':max(values,default=None),'std_ms':statistics.pstdev(values) if values else None}


def analyze(folder:Path, seconds:int, source_fps:float=67):
    pm=readcsv(folder/'capture.csv');native=readcsv(folder/'native-present.csv')
    sdk=readcsv(folder/'source-sdk-status.csv');meta=json.loads((folder/'metadata.json').read_text())
    feedback=json.loads((folder/'feedback-report.json').read_text())
    nmeta=json.loads((folder/'native-gate-meta.json').read_text())
    nmap={int(r['id']):r for r in native}
    with (folder/'native-live.csv').open(encoding='ascii') as f: live=list(csv.reader(f))
    lt=[float(r[1]) for r in live];epoch_samples=[float(r[1])-float(nmap[int(r[0])]['present_enter_ms']) for r in live if int(r[0]) in nmap]
    epoch=statistics.median(epoch_samples)
    origins=[float(r['QPCTime'])*1000-float(r['TimeInSeconds'])*1000 for r in pm]
    origin=statistics.median(origins)
    display=[];matches=[];unmatched=0;input_order_inversions=0;last_display=None
    for row in pm:
        if row['Dropped']!='0':continue
        try:p=float(row['QPCTime'])*1000;d=p+float(row['msUntilDisplayed'])
        except ValueError:continue
        if not math.isfinite(d):continue
        if last_display is not None and d<last_display:input_order_inversions+=1
        last_display=d
        display.append((d,row['PresentMode']))
        j=bisect.bisect_left(lt,p);choices=[k for k in (j-1,j) if 0<=k<len(lt)]
        if not choices:unmatched+=1;continue
        k=min(choices,key=lambda i:abs(lt[i]-p))
        if abs(lt[k]-p)>.5:unmatched+=1;continue
        ident=int(live[k][0]);r=nmap[ident]
        matches.append({'id':ident,'display_ms':d,'present_ms':p,'match_error_ms':abs(lt[k]-p),
                        'phase_ms':float(r['phase_ms']),
                        'output_submit_to_display_ms':d-epoch-float(r['submitted_ms'])})
    display.sort()
    windows=[]
    for a,b in [(1,seconds-1),(seconds/2,seconds-1)]:
        lo=origin+a*1000;hi=origin+b*1000;w=b-a
        points=[t for t,mode in display if lo<=t<hi]
        gaps=[v-u for u,v in zip(points,points[1:])]
        srows=[r for r in sdk if lo<=float(r['status_qpc_ms'])<hi]
        matched=[r for r in matches if lo<=r['display_ms']<hi]
        lat=[r['output_submit_to_display_ms'] for r in matched]
        phase=[r['phase_ms'] for r in matched if r['id']%2==0]
        slope=None
        if len(matched)>1:
            slope=statistics.linear_regression([(r['display_ms']-lo)/1000 for r in matched],lat).slope
        windows.append({'window_seconds':[a,b],'source_status_per_s':len(srows)/w,
                        'display_events_per_s':len(points)/w,'display_gap':dist(gaps),
                        'display_gap_nonpositive':sum(x<=0 for x in gaps),
                        'within_1ms_of_target_fraction':sum(abs(x-500/source_fps)<=1 for x in gaps)/len(gaps) if gaps else None,
                        'target_step_ms':500/source_fps,'source_sdk_results':dict(collections.Counter(r['interpolation_result'] for r in srows)),
                        'even_phase':dist(phase),'output_submit_to_display':dist(lat),
                        'output_latency_linear_slope_ms_per_s':slope,
                        'present_modes':dict(collections.Counter(m for t,m in display if lo<=t<hi))})
    bins=[]
    for a in range(0,seconds,10):
        b=min(a+10,seconds);lo=origin+a*1000;hi=origin+b*1000
        vals=[r['output_submit_to_display_ms'] for r in matches if lo<=r['display_ms']<hi]
        bins.append({'seconds':[a,b],'latency_median_ms':statistics.median(vals) if vals else None})
    required=['capture.csv','native-live.csv','native-present.csv','source-sdk-status.csv','metadata.json','feedback-report.json']
    result={'kind':'same_clock_display_feedback_analysis','case':folder.name,
            'foreground_valid':meta.get('foreground_valid'),'sample_sha256':meta.get('sample_sha256'),
            'sample_exit':meta.get('sample_exit'),'capture_exit':meta.get('presentmon_exit'),
            'counts':{'presentmon_rows':len(pm),'dropped_rows':sum(r['Dropped']!='0' for r in pm),
                      'matched_native_rows':len(matches),'unmatched_native_rows':unmatched,
                      'duplicate_matched_ids':len(matches)-len({r['id'] for r in matches}),
                      'nonmonotonic_display_records':input_order_inversions},
            'clock_check':{'native_epoch_spread_ms':max(epoch_samples)-min(epoch_samples),
                           'presentmon_epoch_spread_ms':max(origins)-min(origins),
                           'maximum_native_match_error_ms':max((r['match_error_ms'] for r in matches),default=None)},
            'native_summary':nmeta,'feedback_final_phase_ms':feedback.get('final_phase_ms'),
            'feedback_updates':len(feedback.get('changes',[])),
            'feedback_failure':feedback.get('failure'),'windows':windows,'latency_10s_bins':bins,
            'hashes':{n:hashlib.sha256((folder/n).read_bytes()).hexdigest() for n in required},
            'limits':['Single-adapter SDK sample, not the dual-GPU game.',
                      'Displayed events may represent partial scanout when tearing is enabled.',
                      'Latency is native output submission to ETW display event, NOT input-to-photon.',
                      'SDK statuses are timestamps after source submission, not optical unique-image counts.',
                      'Game HDR, resize, reset, image correctness and zero total FPS cost not certified.']}
    (folder/'same-clock-analysis.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('directory',type=Path);p.add_argument('--seconds',type=int,required=True);p.add_argument('--fps',type=float,default=67)
    a=p.parse_args();print(json.dumps(analyze(a.directory,a.seconds,a.fps),indent=2))
