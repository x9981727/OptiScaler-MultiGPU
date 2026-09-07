# SPDX-License-Identifier: MIT
"""Prepare r5.2 from the exact r5.1 sources. The GPU kernels are unchanged.
This script does not execute a GPU or claim complete Forward equivalence.
"""
import hashlib,json,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parent
OUT=ROOT/'build';OUT.mkdir(exist_ok=True)

def git_blob(data):
    return hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()

def edit_once(text,old,new):
    if text.count(old)!=1:raise ValueError('Patch target is not unique: '+old[:100])
    return text.replace(old,new)

raw=(ROOT/'plan_core.hpp').read_bytes().replace(b'\r\n',b'\n')
assert git_blob(raw)=='f66ee5ade830868e167503cd23a74564bd787578','Unexpected original plan parser'
(ROOT/'r52_original_plan_core.hpp').write_bytes(raw)
s=raw.decode()
old='static Ref pointer(const Cmd& c,size_t at){need(c.args.size()==1,"Expected one by-value argument");for(auto& r:c.args[0].relocations)if(r.at==at)return r.ref;throw std::runtime_error("Required original pointer relocation absent");}'
new='''static Ref pointer(const Cmd& c,size_t at){
        need(c.args.size()==1,"Expected one by-value argument for "+c.kernel);
        std::string available;
        for(auto& r:c.args[0].relocations){if(r.at==at)return r.ref;available+=(available.empty()?"":",")+std::to_string(r.at);}
        throw std::runtime_error("Missing pointer relocation: kernel="+c.kernel+", requested_offset="+std::to_string(at)+", available_offsets=["+available+"]");
    }
    static void check_special_boundary(const Cmd& c,bool first){
        const std::string where=first?"first special Swin":"last special Swin";
        need(c.kernel=="_Z10k_swin_varILi32ELb1EEv9VarParams",where+": unexpected kernel");
        need(c.args.size()==1 && c.args[0].bytes.size()==168,where+": expected 168-byte argument");
        need(c.grid==std::array<U,3>{144,240,1} && c.block==std::array<U,3>{256,1,1} && c.shared==0,where+": launch geometry differs");
        const auto& bytes=c.args[0].bytes;
        need(le<U>(bytes,24)==1920 && le<U>(bytes,28)==1152 && le<U>(bytes,40)==(first?20u:32u),where+": scalar layout differs from recorded trace");
        const size_t nullSlot=first?0u:8u;
        need(std::all_of(bytes.begin()+nullSlot,bytes.begin()+nullSlot+8,[](B b){return b==0;}),where+": original null field was changed");
        const std::vector<Reloc> expected=first
            ?std::vector<Reloc>{{8,{5,0}},{16,{0,12}},{56,{6,0}},{64,{1,0}},{160,{43,0}}}
            :std::vector<Reloc>{{0,{5,0}},{16,{0,147397244}},{112,{2,0}},{120,{1,0}},{152,{26,0}},{160,{43,0}}};
        need(c.args[0].relocations.size()==expected.size(),where+": relocation count differs");
        for(const auto& e:expected){const auto actual=pointer(c,e.at);
            need(actual.allocation==e.ref.allocation && actual.offset==e.ref.offset,where+": reference mismatch at offset "+std::to_string(e.at));}
    }'''
s=edit_once(s,old,new)
old='''p.firstInput=pointer(p.commands.front(),0);p.lastObserved=pointer(p.commands.back(),8);
        need(p.firstInput.allocation!=0 && p.lastObserved.allocation!=0,"Boundary tensor points to weights");'''
new='''// Special first/last Swin variants do NOT have the interior 0/8 pointer ABI.
        // Select the reported boundary fields; never fill, redirect or remove a
        // null pointer in the actual original kernel argument bytes.
        const std::string edge="_Z10k_swin_varILi32ELb1EEv9VarParams";
        const bool special=p.commands.front().kernel==edge || p.commands.back().kernel==edge;
        size_t inputSlot=0,observedSlot=8;
        if(special){
            check_special_boundary(p.commands.front(),true);check_special_boundary(p.commands.back(),false);
            inputSlot=64;observedSlot=112;
            p.firstInput=pointer(p.commands.front(),inputSlot);p.lastObserved=pointer(p.commands.back(),observedSlot);
            need(p.allocations.at(p.firstInput.allocation)==26542080 && p.allocations.at(p.lastObserved.allocation)==35389440,"Recorded boundary allocation sizes differ");
            p.bounds(p.firstInput,26542080);p.bounds(p.lastObserved,35389440);
        }else{
            need(!fixed,"The pinned full plan requires its special boundary kernels");
            p.firstInput=pointer(p.commands.front(),inputSlot);p.lastObserved=pointer(p.commands.back(),observedSlot);
        }
        need(p.firstInput.allocation!=0 && p.lastObserved.allocation!=0,"Boundary tensor points to weights");'''
s=edit_once(s,old,new)
s=edit_once(s,'{"first_input_ref",{{"allocation",p.firstInput.allocation},{"offset",p.firstInput.offset}}},{"last_observed_pointer_at8",{{"allocation",p.lastObserved.allocation},{"offset",p.lastObserved.offset}}}',
               '{"first_input_ref",{{"allocation",p.firstInput.allocation},{"offset",p.firstInput.offset},{"argument_offset",inputSlot}}},{"last_observed_ref",{{"allocation",p.lastObserved.allocation},{"offset",p.lastObserved.offset},{"argument_offset",observedSlot}}},{"original_argument_bytes_modified",false}')
(ROOT/'plan_core.hpp').write_text(s,encoding='utf-8',newline='\n')
# Keep the old source and patch script in the source distribution for reproducibility.
subprocess.run([sys.executable,str(ROOT/'prepare_delivery.py')],check=True)
f=(ROOT/'forward_delivery.cpp').read_text(encoding='utf-8')
f=edit_once(f,'#include "plan_core.hpp"','#include "plan_core.hpp"\n#include "plan_regression_r52.hpp"')
f=f.replace('r5.1','r5.2')
f=f.replace('last_observed_pointer_at8','last_observed_pointer_at112')
f=f.replace('observed pointer-at8 region of final launch','observed pointer-at112 region of final launch')
f=edit_once(f,'fs::path planPath,int selected){','fs::path planPath,int selected,bool validateOnly){')
f=edit_once(f,'int device=-1;bool cpu=false;','int device=-1;bool cpu=false,validateOnly=false;')
f=edit_once(f,'if(a==L"--cpu-self-test")cpu=true;','if(a==L"--cpu-self-test")cpu=true;else if(a==L"--validate-plan-only")validateOnly=true;')
f=edit_once(f,'report["cpu_plan_checks"]=r5::self_test();','report["cpu_boundary_regression_checks"]=r52::regression_tests();report["cpu_plan_checks"]=r5::self_test();')
f=edit_once(f,'gpu_r5(report,out,runtime,weights,plan,device);','gpu_r5(report,out,runtime,weights,plan,device,validateOnly);')
f=edit_once(f,'report["plan_validation_passed"]=true;save_r5(out,report);','''report["plan_validation_passed"]=true;
    report["boundary_contract"]={{"first_input_argument_offset",64},{"first_input_allocation",plan.firstInput.allocation},
        {"last_observed_argument_offset",112},{"last_observed_allocation",plan.lastObserved.allocation},
        {"original_null_first_slot0_preserved",true},{"original_null_final_slot8_preserved",true},
        {"original_kernel_arguments_changed",false}};
    report["source_plan_definitions"]=J::object();
    for(const char* key:{"input","output","initialization","controls","padded_resolution","scope"})
        if(root.contains(key))report["source_plan_definitions"][key]=root.at(key);
    std::cout<<"[r5.2] Original plan parsed: 154 launches, 4 copies; first input slot64/allocation1; last observed slot112/allocation2."<<std::endl;
    save_r5(out,report);
    if(validateOnly){
        report["status"]="original_plan_cpu_validation_passed_gpu_not_run";
        delivery_stage("Original checkpoint CPU validation complete; GPU was not initialized");
        save_r5(out,report);return;
    }''')
f=edit_once(f,'[--cpu-self-test --fixture test.zip]','[--validate-plan-only] [--cpu-self-test --fixture test.zip]')
f=edit_once(f,'if(cpu){if(!fixture.empty())','r5::need(!(cpu && validateOnly),"Choose either --cpu-self-test or --validate-plan-only");\n        if(cpu){if(!fixture.empty())')
(ROOT/'forward_delivery.cpp').write_text(f,encoding='utf-8',newline='\n')
report={'revision':'r5.2','source_original_parser_git_blob':git_blob(raw),
        'patched_parser_sha256':hashlib.sha256(s.encode()).hexdigest(),
        'compiled_source_sha256':hashlib.sha256(f.encode()).hexdigest(),
        'reason':'r5.1 used first slot0/final slot8, which are null in the recorded special Swin boundary calls',
        'selected_first_slot':64,'selected_last_slot':112,
        'original_gpu_argument_bytes_changed':False,'original_plan_hash_gate_removed':False,
        'regression_fixture_scope':'Three reported calls and their allocation sizes, NOT the complete 154-launch trace',
        'gpu_executed':False,'game_tested':False}
(OUT/'r52-source-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2),flush=True)
