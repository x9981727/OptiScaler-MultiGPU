from pathlib import Path
import hashlib,json,shutil
kit=Path(__file__).resolve().parents[1]
paths=[kit/'v26/GameOutputPacing-compiled.h',kit/'upstream/OptiScaler/framegen/xefg/GameOutputPacing.h']
s=paths[0].read_text(encoding='utf-8')
def once(a,b):
 global s
 if s.count(a)!=1:raise RuntimeError('RC2 anchor mismatch '+a[:100])
 s=s.replace(a,b)
once('#include <windows.h>','#include <windows.h>\n#include "PhaseOnlyDeadline.h"')
once('double phaseMs=-2.2, fixedPeriodMs=0;', 'double phaseMs=-2.2;')
once('''        s.fixedPeriodMs=number(L"SourcePeriodMs",0,0,100);
        if(s.fixedPeriodMs>0 && s.fixedPeriodMs<2) s.fixedPeriodMs=0;''','''        // SourcePeriodMs is deliberately ignored: this layer cannot set an FPS
        // target. Log the effective state even when the layer is disabled.
        std::ofstream effective(s.directory/L"XeFGPacing.log",std::ios::app);
        if(effective)effective<<"v26 RC2 effective-config enabled="<<s.enabled
            <<" mode=phase-only fixedRateClock=removed SourcePeriodMs=ignored phaseMs="
            <<s.phaseMs<<" parity="<<s.phaseParity<<" trace="<<s.trace<<"\\n";''')
once('double period=0,submitted=0,deadline=0,presented=0;',
     'double period=0,submitted=0,deadline=0,presented=0,readyAt=0,phaseWaitMs=0;bool pressureBypass=false;')
once('HANDLE readyEvent=nullptr,copyEvent=nullptr,timer=nullptr,stopEvent=nullptr;',
     'HANDLE readyEvent=nullptr,copyEvent=nullptr,timer=nullptr,stopEvent=nullptr,workEvent=nullptr;')
once('bool stopped=false;double nextDeadline=0,lastReport=0;UINT consumerEpoch=0;',
     'bool stopped=false;double lastReport=0;std::atomic<UINT64> pressureBypasses{0};')
once('''            <<" sourceMs="<<sourcePeriod.load()<<" phaseMs="<<options.phaseMs<<"\\n";log.flush();}''',
'''            <<" sourceMs="<<sourcePeriod.load()<<" phaseMs="<<options.phaseMs
            <<" mode=phase-only fixedRateClock=removed pressureBypasses="<<pressureBypasses.load()<<"\\n";log.flush();}''')
once('source_period_ms,result\\n";', 'source_period_ms,result,ready_qpc_ms,phase_wait_ms,pressure_bypass\\n";')
once('''        timer=CreateWaitableTimerExW(nullptr,nullptr,2,TIMER_ALL_ACCESS);''',
'''        workEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        timer=CreateWaitableTimerExW(nullptr,nullptr,2,TIMER_ALL_ACCESS);''')
once('if(!readyEvent||!copyEvent||!stopEvent||!timer)', 'if(!readyEvent||!copyEvent||!stopEvent||!timer||!workEvent)')
once('''        if(active.exchange(enabled)!=enabled)++epoch;
        if(std::isfinite(ms)&&ms>=2&&ms<=100){double old=sourcePeriod.load();sourcePeriod=old>0?old*.85+ms*.15:ms;}''',
'''        if(active.exchange(enabled)!=enabled){sourcePeriod=0;++epoch;SetEvent(workEvent);}
        // A faster source immediately reduces the maximum optional phase hold.
        // This value is NOT used to construct an output rate or recurring clock.
        if(std::isfinite(ms)&&ms>0&&ms<=1000){double old=sourcePeriod.load();
            sourcePeriod=old<=0||ms<old?ms:old*.85+ms*.15;}''')
once('p->period=options.fixedPeriodMs>0?options.fixedPeriodMs:sourcePeriod.load();',
     'p->period=sourcePeriod.load();')
once('p->paced=!synchronous&&p->period>=2&&p->period<=100;',
     'p->paced=!synchronous&&std::isfinite(p->period)&&p->period>0;')
once('lock.unlock();changed.notify_all();', 'lock.unlock();changed.notify_all();SetEvent(workEvent);')
start=s.index('            double now=ClockMs();\n            if(consumerEpoch')
end=s.index('            p->presented=ClockMs();',start)
s=s[:start]+'''            p->readyAt=ClockMs();
            bool pressure=false;
            {std::lock_guard<std::mutex> lock(mutex);pressure=inFlight>=leases.size();}
            p->deadline=PhaseOnlyDeadline(p->submitted,p->readyAt,p->id,options.phaseMs,
                options.phaseParity,p->period,SUCCEEDED(hr)&&p->paced&&active&&!stopping,pressure);
            if(pressure){p->pressureBypass=true;++pressureBypasses;}
            const double phaseStart=ClockMs();
            while(SUCCEEDED(hr)&&p->paced&&active&&!stopping&&p->epoch==epoch.load()) {
                const double wait=p->deadline-ClockMs();if(wait<=0)break;
                {std::lock_guard<std::mutex> lock(mutex);
                    if(inFlight>=leases.size()){p->pressureBypass=true;++pressureBypasses;break;}}
                LARGE_INTEGER due;due.QuadPart=-static_cast<LONGLONG>(std::ceil(wait*10000));
                if(!SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE))break;
                HANDLE hs[]={timer,stopEvent,workEvent};
                const DWORD wake=WaitForMultipleObjects(3,hs,FALSE,100);
                if(wake==WAIT_FAILED||wake==WAIT_TIMEOUT||wake==WAIT_OBJECT_0+1)break;
                // New submissions interrupt the optional delay; once all slots
                // are occupied we release immediately, retaining every packet.
            }
            p->phaseWaitMs=(std::max)(0.0,ClockMs()-phaseStart);
''' +s[end:]
once("<<static_cast<long>(hr)<<'\\n';}",
     "<<static_cast<long>(hr)<<','<<p->readyAt<<','<<p->phaseWaitMs<<','<<p->pressureBypass<<'\\n';}")
once('for(HANDLE h:{readyEvent,copyEvent,timer,stopEvent})',
     'for(HANDLE h:{readyEvent,copyEvent,timer,stopEvent,workEvent})')
s=s.replace('v26 RC1','v26 RC2')
assert 'nextDeadline' not in s and 'fixedPeriodMs' not in s
for p in paths:
 p.write_text(s,encoding='utf-8');shutil.copyfile(kit/'v26rc2/PhaseOnlyDeadline.h',p.parent/'PhaseOnlyDeadline.h')
p=kit/'upstream/OptiScaler/framegen/xefg/XeFG_Dx12.cpp'
c=p.read_text(encoding='utf-8');c=c.replace('MultiGPU v26 RC1: optional native output pacing, configuration XeFGPacing.ini; game-test candidate',
'MultiGPU v26 RC2: phase-only native output; recurring FPS clock removed; configuration XeFGPacing.ini')
p.write_text(c,encoding='utf-8')
manifest=kit/'v26/INTEGRATION-MANIFEST.json';d=json.loads(manifest.read_text())
d.update(kind='OptiScaler-v26-RC2-phase-only-game-candidate',source_period_override='removed_and_ignored',recurring_deadline_clock=False,
         phase_model='submission-relative bounded parity shift; late or full-queue packets get no optional wait',
         runtime_state_logged_when_disabled=True,automatic_display_feedback=False,
         actual_game_test_passed=False,zero_fps_cost_claim=False,perfect_pacing_claim=False)
d['source_changes']={str(p.relative_to(kit/'upstream/OptiScaler')):hashlib.sha256(p.read_bytes()).hexdigest() for p in [paths[1],paths[1].parent/'PhaseOnlyDeadline.h',kit/'upstream/OptiScaler/framegen/xefg/XeFG_Dx12.cpp']}
manifest.write_text(json.dumps(d,indent=2),encoding='utf-8')
print(json.dumps(d),flush=True)

# Exercise the exact policy header in the existing Windows lifecycle executable.
shutil.copyfile(kit/'v26rc2/phase-policy-test.cpp',kit/'v26/phase-policy-test.cpp')
test=kit/'v26/lifecycle-test.cpp'
t=test.read_text(encoding='utf-8')
if t.count('int main(){')!=1:raise RuntimeError('lifecycle main anchor')
t=t.replace('int main(){','#include "phase-policy-test.cpp"\nint main(){\n RunPhaseOnlyTests();')
t=t.replace('WritePrivateProfileStringW(L"OutputPacing",L"Enabled",L"1",path.c_str());',
 'WritePrivateProfileStringW(L"OutputPacing",L"Enabled",L"1",path.c_str());\n WritePrivateProfileStringW(L"OutputPacing",L"SourcePeriodMs",L"100",path.c_str()); // explicitly ignored by RC2')
test.write_text(t,encoding='utf-8')
