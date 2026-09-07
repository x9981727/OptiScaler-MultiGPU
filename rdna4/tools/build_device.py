# SPDX-License-Identifier: MIT
"""Compile actual GPU code, inspect ABI and resources. Does NOT run a GPU."""
import collections, io, json, re, subprocess
from pathlib import Path
import msgpack
from elftools.elf.elffile import ELFFile

root = Path(__file__).resolve().parents[1]
out = root / 'build'
out.mkdir(exist_ok=True)
obj = out / 'linear.o'
code = out / 'linear_gfx1201.hsaco'
cmd = ['clang-19', '-x', 'cl', '-cl-std=CL2.0', '--target=amdgcn-amd-amdhsa',
       '-mcpu=gfx1201', '-mno-wavefrontsize64', '-mcode-object-version=5', '-nogpulib',
       '-O3', '-ffp-contract=off', '-c', str(root/'kernels/linear.cl'), '-o', str(obj)]
print(' '.join(cmd), flush=True)
subprocess.run(cmd, check=True)
subprocess.run(['ld.lld-19', '-shared', str(obj), '-o', str(code)], check=True)
text = subprocess.check_output(['llvm-objdump-19', '-d', '--mcpu=gfx1201', str(code)], text=True)
(out/'linear_gfx1201.isa.txt').write_text(text)
elf = ELFFile(io.BytesIO(code.read_bytes()))
metadata = None
for section in elf.iter_sections():
    if section.header.sh_type == 'SHT_NOTE':
        for note in section.iter_notes():
            if note['n_name'] == 'AMDGPU':
                metadata = msgpack.unpackb(note['n_desc'], raw=False, strict_map_key=False)
assert metadata is not None
assert 'gfx1201' in metadata['amdhsa.target']
chunks = re.split(r'^[0-9a-fA-F]+ <([^>]+)>:\s*$', text, flags=re.M)
functions = dict(zip(chunks[1::2], chunks[2::2]))
report = {'compile_passed': True, 'gpu_execution_tested': False, 'model_equivalence_tested': False,
          'compiler': subprocess.check_output(['clang-19','--version'],text=True).splitlines()[0],
          'command': cmd, 'metadata': metadata, 'kernels': []}
for kernel in metadata['amdhsa.kernels']:
    name = kernel['.name']
    body = functions[name]
    ops = collections.Counter(re.findall(r'\b(v_wmma_[a-zA-Z0-9_]*)\b',body))
    expected = 'v_wmma_f32_16x16x16_fp8_fp8' if 'fp8' in name else 'v_wmma_f32_16x16x16_f16'
    assert ops[expected] > 0, (name,ops)
    assert kernel['.wavefront_size'] == 32
    assert kernel['.group_segment_fixed_size'] == 0, 'Unexpected LDS allocation'
    assert kernel.get('.vgpr_spill_count',0) == 0, 'Unexpected VGPR spills'
    assert kernel['.private_segment_fixed_size'] == 0, 'Unexpected scratch allocation'
    actual = [a['.offset'] for a in kernel['.args'] if not a['.value_kind'].startswith('hidden_')]
    assert actual == [0,8,16,24,32,40,44,48,52,56], actual
    summary = {'name':name,'vgpr':kernel['.vgpr_count'],'lds_bytes':kernel['.group_segment_fixed_size'],
               'scratch_bytes':kernel['.private_segment_fixed_size'],'matrix_instructions_static':dict(ops)}
    report['kernels'].append(summary)
    print(json.dumps(summary))
(out/'compile-report.json').write_text(json.dumps(report,indent=2))
print('DEVICE CODE COMPILED. GPU execution and NR-model equivalence remain untested.')
