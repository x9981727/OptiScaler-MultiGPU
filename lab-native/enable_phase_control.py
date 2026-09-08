from pathlib import Path
import sys,hashlib,json
root=Path(sys.argv[1])/'samples/basic_sample_frame_generation'
p=root/'NativeGate.h';s=p.read_text(encoding='utf-8')
def once(a,b):
 global s
 if s.count(a)!=1:raise RuntimeError('phase control anchor '+a[:100])
 s=s.replace(a,b)
once('inline void DebugSetup(){}', '''inline double PhaseOffset(UINT64 id,double period) {
 static const double configured=[] {char b[64]{};if(!GetEnvironmentVariableA("XEFG_LAB_EVEN_PHASE_MS",b,sizeof(b)))return 0.0;
  char* e=nullptr;double x=strtod(b,&e);return e!=b&&*e==0&&std::isfinite(x)&&x>=-4&&x<=4?x:0.0;}();
 const double bound=period*0.24;
 return (id%2)==0?(std::max)(-bound,(std::min)(bound,configured)):0.0;
}
struct SourceStatus {UINT64 id;double qpcMs;int status;UINT enabled,outputs;};
inline std::mutex sourceTraceMutex;
inline std::vector<SourceStatus> sourceTrace;
inline void RecordSourceStatus(UINT64 id,int status,UINT enabled,UINT outputs) {
 std::lock_guard<std::mutex> lock(sourceTraceMutex);
 if(sourceTrace.size()<100000)sourceTrace.push_back({id,QpcMs(),status,enabled,outputs});
}
inline void DumpSourceStatus() {
 std::ofstream f(EnvPath()+"\\\\source-sdk-status.csv");
 f<<"source_id,status_qpc_ms,interpolation_result,enabled,queued_output_count\\n"<<std::fixed<<std::setprecision(5);
 for(const auto&r:sourceTrace)f<<r.id<<','<<r.qpcMs<<','<<r.status<<','<<r.enabled<<','<<r.outputs<<'\\n';
}
inline void DebugSetup(){}''')
once('r.deadline=nextDeadline-epoch;SleepUntil(nextDeadline);',
     'const double adjusted=nextDeadline+PhaseOffset(r.id,r.period);r.deadline=adjusted-epoch;SleepUntil(adjusted);')
once('inline void Finish(){if(state){state->Shutdown();state.reset();}}',
     'inline void Finish(){if(state){state->Shutdown();state.reset();}DumpSourceStatus();}')
p.write_text(s,encoding='utf-8')
p=root/'basic_sample.cpp';s=p.read_text(encoding='utf-8')
once('    if (m_lastPresentStatus.frameGenResult != XEFG_SWAPCHAIN_RESULT_SUCCESS)',
     '    NativeGateLab::RecordSourceStatus(m_frameCounter,static_cast<int>(m_lastPresentStatus.frameGenResult),m_lastPresentStatus.isFrameGenEnabled,m_lastPresentStatus.framesPresented);\n\n    if (m_lastPresentStatus.frameGenResult != XEFG_SWAPCHAIN_RESULT_SUCCESS)')
p.write_text(s,encoding='utf-8')
m=root/'NATIVE-GATE-MANIFEST.json';d=json.loads(m.read_text());d.update(experimental_even_phase_default_ms=0,independent_source_status_trace=True,phase_changes_average_target_period=False,game_modified=False)
for n in ['NativeGate.h','basic_sample.cpp']:d[n+'_sha256']=hashlib.sha256((root/n).read_bytes()).hexdigest()
m.write_text(json.dumps(d,indent=2),encoding='utf-8')
