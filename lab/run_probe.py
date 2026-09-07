from pathlib import Path
import subprocess,json,sys,os
root=Path(__file__).resolve().parents[1]
out=root/'lab-results';out.mkdir(exist_ok=True)
exe=root/'lab-build/Release/sdk-probe.exe'
sdk=root/'upstream/external/xess/bin'
record={'kind':'actual_sdk_capability_probe','is_hardware_acceptance':False}
try:
 r=subprocess.run([str(exe),str(sdk)],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=60)
 (out/'sdk-probe-stdout.txt').write_text(r.stdout,encoding='utf-8')
 (out/'sdk-probe-stderr.txt').write_text(r.stderr,encoding='utf-8')
 record['returncode']=r.returncode
 try:record['observation']=json.loads(r.stdout)
 except json.JSONDecodeError:record['observation']={'result':'unparseable_or_aborted','target_pair_available':False}
except subprocess.TimeoutExpired as e:
 record['observation']={'result':'timed_out','timeout_seconds':60,'target_pair_available':False}
 (out/'sdk-probe-timeout.txt').write_text(str(e))
(out/'sdk-capability.json').write_text(json.dumps(record,indent=2)+'\n',encoding='utf-8')
# This branch is deliberately a test laboratory, not a game-DLL release job.
# Physical tests require the exact card pair and matching display/game capture.
state={'final_release_allowed':False,'candidate_branch':os.environ.get('GITHUB_REF_NAME'),
 'candidate_commit':os.environ.get('GITHUB_SHA'),'run':os.environ.get('GITHUB_RUN_ID'),
 'blocking_evidence':['AMD 9070 XT + 6600 XT same-scene OFF/ON throughput','actual generated-frame display cadence on this candidate','pixel/flicker/ghosting validation','end-to-end latency and non-growing queue'],
 'note':'Passing software/WARP/protocol tests does not satisfy the physical acceptance criteria. No game DLL is published by this workflow.'}
(out/'release-state.json').write_text(json.dumps(state,indent=2)+'\n',encoding='utf-8')
print(json.dumps(record,indent=2));print(json.dumps(state,indent=2))
