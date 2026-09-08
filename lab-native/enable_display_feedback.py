from pathlib import Path
import sys,hashlib,json
root=Path(sys.argv[1])/'samples/basic_sample_frame_generation'
p=root/'NativeGate.h';s=p.read_text(encoding='utf-8')
def once(a,b):
 global s
 if s.count(a)!=1:raise RuntimeError('display feedback anchor '+a[:120])
 s=s.replace(a,b)
code=r'''inline double DisplayFeedbackPhase(double period) {
 // Only the native output consumer reads this lab-only control. It never changes
 // the mean output period and rejects stale, malformed and oversized commands.
 static double phase=0,lastRead=0;const double now=QpcMs();
 if(now-lastRead>=200){
  lastRead=now;std::ifstream f(EnvPath()+"\\phase-control.txt");
  unsigned version=0;double value=0,created=0;
  if(f>>version>>value>>created){
   if(version==1&&std::isfinite(value)&&std::isfinite(created)&&
      value>=-4&&value<=4&&now>=created&&now-created<5000)phase=value;
  }
 }
 const double bound=period*0.24;
 return (std::max)(-bound,(std::min)(bound,phase));
}
inline void LivePresentRecord(UINT64 id,double time,double period,double phase,HRESULT hr){
 if(!EnvFlag("XEFG_LAB_ETW_PHASE"))return;
 static std::ofstream f(EnvPath()+"\\native-live.csv");
 f<<std::fixed<<std::setprecision(6)<<id<<','<<time<<','<<period<<','<<phase<<','<<static_cast<long>(hr)<<'\n';
 if((id%16)==0)f.flush();
}
'''
once('inline double PhaseOffset(UINT64 id,double period) {',code+'\ninline double PhaseOffset(UINT64 id,double period) {')
once(' const double bound=period*0.24;\n return (id%2)==0?',
 ' if(EnvFlag("XEFG_LAB_ETW_PHASE"))return (id%2)==0?DisplayFeedbackPhase(period):0.0;\n const double bound=period*0.24;\n return (id%2)==0?')
once('r.presentReturn=QpcMs()-epoch;r.result=hr;',
 'r.presentReturn=QpcMs()-epoch;r.result=hr;\n   LivePresentRecord(r.id,r.presentEnter+epoch,r.period,r.appliedPhase,hr);')
p.write_text(s,encoding='utf-8')
m=root/'NATIVE-GATE-MANIFEST.json';d=json.loads(m.read_text());d.update(etw_phase_feedback_lab_interface=True,feedback_changes_mean_period=False,feedback_default=False,game_modified=False)
d['NativeGate.h_sha256']=hashlib.sha256(p.read_bytes()).hexdigest();m.write_text(json.dumps(d,indent=2),encoding='utf-8')
