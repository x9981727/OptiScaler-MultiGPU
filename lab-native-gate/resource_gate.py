from pathlib import Path
import sys
p=Path(sys.argv[1])/'buffered_output.h';s=p.read_text(encoding='utf-8-sig')
def once(a,b):
 global s
 if s.count(a)!=1:raise RuntimeError('Resource gate anchor: '+a[:100])
 s=s.replace(a,b)
once(' bool paced=false;', ' bool paced=false;\n bool gateBeforeCopy=false;')
once('  char b[64]{};paced=GetEnvironmentVariableA("XEFG_NATIVE_GATE",b,64)>0&&b[0]==\'1\';', '  char b[64]{};gateBeforeCopy=GetEnvironmentVariableA("XEFG_GATE_BEFORE_COPY",b,64)>0&&b[0]==\'1\';\n  paced=GetEnvironmentVariableA("XEFG_NATIVE_GATE",b,64)>0&&b[0]==\'1\';\n  std::fprintf(stderr,"BACKBUFFER_WRITE_GATE beforeCopy=%d\\n",int(gateBeforeCopy));')
once('   Check(output->Wait(captured.Get(),job.id));','   Check(output->Wait(captured.Get(),job.id));\n   // A queue-only wait after the completed write need not delay the resource\n   // presentation dependency. This alternative puts the write behind the gate.\n   if(scheduleOutputGate&&gateBeforeCopy)Check(scheduleOutputGate());')
once('   if(scheduleOutputGate)Check(scheduleOutputGate());','   if(scheduleOutputGate&&!gateBeforeCopy)Check(scheduleOutputGate());')
p.write_text(s,encoding='utf-8');print('Backbuffer-write gate control prepared; requires hardware A/B evidence.')
