# SPDX-License-Identifier: MIT
import contextlib,hashlib,io,json,os,platform,runpy,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parent
OUT=ROOT/'reports';OUT.mkdir(exist_ok=True)
BUILD=ROOT/'build';BUILD.mkdir(exist_ok=True)
configs=[('debug',['-O0']),('sanitized',['-O2','-fsanitize=address,undefined','-fno-omit-frame-pointer']),('optimized',['-O3'])]
records=[]
for name,flags in configs:
    obj=BUILD/f'puff-{name}.o';exe=BUILD/f'vm-{name}'
    subprocess.run(['clang-19',*flags,'-c',str(ROOT.parent/'forward_r5/deps/puff.c'),'-o',str(obj)],check=True)
    command=['clang++-19','-std=c++20','-ffp-contract=off',*flags,str(ROOT/'test_virtual.cpp'),str(obj),'-o',str(exe)]
    subprocess.run(command,check=True)
    env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=1:halt_on_error=1';env['UBSAN_OPTIONS']='halt_on_error=1'
    subprocess.run([str(exe),str(OUT/f'vm-{name}.json')],env=env,check=True,timeout=180)
    result=json.loads((OUT/f'vm-{name}.json').read_text())
    assert result['status']=='cpu_virtual_lab_passed' and result['target_gpu_executed'] is False
    records.append({'configuration':name,'compiler_command':command,'result':result})
summary={'platform':platform.platform(),'configurations':records,'continuous_trigger':'pushes to the isolated r6 branch, not an infinite paid loop',
 'actual_target_gpu_execution':False,'perfect_optimization_proved':False,'final_game_release_approved':False}
(OUT/'virtual-lab-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
# Inspect a concrete original output path. This remains static evidence, not a
# software execution of the 5090-line special Swin function.
log=io.StringIO()
with contextlib.redirect_stdout(log):data=runpy.run_path(str(ROOT/'inspect_boundary.py'))
(OUT/'inspection-console.txt').write_text(log.getvalue())
lines=data['lines']
focus='\n'.join(f'{i+1:5d} {s}' for i,s in enumerate(lines) if 3850<=i<4100)
(OUT/'output-path-focus.txt').write_text(focus+'\n')
print('=== STATIC FINAL-OUTPUT CONTROL / STORE REGION ===',flush=True)
print(focus,flush=True)
print('CPU virtual lab completed across debug, sanitized and optimized builds; no target GPU benchmark was run.',flush=True)
