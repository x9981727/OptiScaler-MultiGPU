// SPDX-License-Identifier: MIT
// Fixed recovered plan replay. This is NOT a game DLL or a complete new neural backend.
#define wmain r5_unused_r1_main
#include "../tests/linear_test.cpp"
#undef wmain
#include "../layer_r4/head_contract.hpp"
#include "../layer_r4/candidate_hash.hpp"
#include "plan_core.hpp"
#include <bcrypt.h>
#include <commdlg.h>
#include <cstring>
#include <memory>
#include <numeric>
#pragma comment(lib,"bcrypt.lib")
#pragma comment(lib,"comdlg32.lib")
using r5::J;
static constexpr char ORIGINAL_CODE_SHA_R5[]="dd38e6ede167c7a5886ae5d079022f63065b6775384d7588f185036b55877afb";
static constexpr char WEIGHTS_SHA_R5[]="6bf8dc931ef3ccffe18c82de26ab374156e7f19539ffcf8eabaa25dca5cf15ab";
static constexpr char* unused_placeholder=nullptr;
static const char* MODES_R5[]={"original_full_plan","head_direct16_w4_in_full_plan","head_direct32_w4_in_full_plan"};
static std::string hash_r5(const std::vector<Byte>& data){
    r5::need(data.size()<=ULONG_MAX,"SHA256 input too large");BCRYPT_ALG_HANDLE alg=nullptr;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("SHA256 provider failed");
    Byte digest[32]{};auto rc=BCryptHash(alg,nullptr,0,const_cast<Byte*>(data.data()),ULONG(data.size()),digest,32);BCryptCloseAlgorithmProvider(alg,0);
    r5::need(rc>=0,"SHA256 calculation failed");std::ostringstream out;for(Byte b:digest)out<<std::hex<<std::setw(2)<<std::setfill('0')<<U(b);return out.str();
}
static std::vector<Byte> read_r5(const fs::path& path,size_t cap){
    std::ifstream f(path,std::ios::binary|std::ios::ate);r5::need(bool(f),"Cannot open selected file");auto end=f.tellg();r5::need(end>0 && std::uint64_t(end)<=cap,"Selected file size unsupported");
    std::vector<Byte> bytes(static_cast<size_t>(end));f.seekg(0);f.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()));r5::need(bool(f),"Incomplete file read");return bytes;
}
static fs::path browse_r5(const wchar_t* title,const wchar_t* filter){
    std::array<wchar_t,32768> data{};OPENFILENAMEW o{};o.lStructSize=sizeof(o);o.lpstrFile=data.data();o.nMaxFile=DWORD(data.size());o.lpstrTitle=title;o.lpstrFilter=filter;o.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_DONTADDTORECENT;
    r5::need(GetOpenFileNameW(&o)!=0,"File selection cancelled or unavailable");return fs::path(data.data());
}
static void save_r5(const fs::path& path,const J& report){
    auto temp=path;temp+=L".tmp";std::ofstream f(temp,std::ios::binary|std::ios::trunc);r5::need(bool(f),"Cannot create report beside test EXE");f<<report.dump(2)<<'\n';f.close();r5::need(bool(f),"Report write incomplete");
    r5::need(MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0,"Cannot finalize JSON report");
}
static std::vector<Byte> original_code_r5(const std::vector<Byte>& data){
    std::vector<Byte> answer;
    for(size_t p=0;p+64<=data.size();++p){
        if(data[p]!=127 || data[p+1]!='E' || data[p+2]!='L' || data[p+3]!='F' || data[p+4]!=2 || data[p+5]!=1)continue;
        if(r5::le<std::uint16_t>(data,p+18)!=224)continue;auto off=r5::le<std::uint64_t>(data,p+40);auto es=r5::le<std::uint16_t>(data,p+58),n=r5::le<std::uint16_t>(data,p+60);
        if(es!=64 || !n || n>4096 || off>data.size()-p)continue;auto size=off+std::uint64_t(es)*n;if(size<64 || size>data.size()-p)continue;
        std::vector<Byte> obj(data.begin()+p,data.begin()+p+size);if(hash_r5(obj)==ORIGINAL_CODE_SHA_R5){r5::need(answer.empty(),"Duplicate matching original code");answer=std::move(obj);}
    }
    r5::need(!answer.empty(),"Selected runtime does not contain the pinned gfx1201 code. Original files were not changed.");return answer;
}
struct WeightEntryR5{std::string name;size_t archive=0,size=0,gpu=0;};
struct WeightsR5{std::vector<Byte> packed;std::map<size_t,WeightEntryR5> at;};
static WeightsR5 weights_r5(const std::vector<Byte>& raw,size_t expected){
    r5::need(hash_r5(raw)==WEIGHTS_SHA_R5,"Weight archive hash mismatch");r5::need(raw.size()>16 && std::memcmp(raw.data(),"DLSSNRW1",8)==0,"Weight archive signature");
    U n=r5::le<U>(raw,8),base=r5::le<U>(raw,12);r5::need(n==153 && base==5673,"Weight directory differs from recovered plan");size_t pos=16;std::map<std::string,WeightEntryR5> entries;
    for(U i=0;i<n;++i){r5::need(pos<base,"Truncated weight directory");size_t len=raw[pos++];r5::need(len && len<=base-pos && base-pos-len>=16,"Invalid weight entry");std::string name(reinterpret_cast<const char*>(raw.data()+pos),len);pos+=len;
        auto off=r5::le<std::uint64_t>(raw,pos),size=r5::le<std::uint64_t>(raw,pos+8);pos+=16;r5::need(off<=raw.size()-base && size<=raw.size()-base-off,"Weight data range");
        r5::need(entries.emplace(name,WeightEntryR5{name,size_t(base+off),size_t(size),0}).second,"Duplicate weight name");}
    r5::need(pos==base,"Weight directory boundary");WeightsR5 result;result.packed.resize(expected,0);size_t end=0;
    for(auto& [name,e]:entries){size_t target=((end+15)&~size_t(15))|size_t(12);r5::need(target<=expected && e.size<=expected-target,"Recovered upload exceeds allocation zero");e.gpu=target;
        std::copy_n(raw.begin()+e.archive,e.size,result.packed.begin()+target);result.at.emplace(target,e);end=target+e.size;}
    r5::need(end==expected,"Original sorted/aligned weight-upload size mismatch");return result;
}
struct ExtraHipR5{
    int (*memsetAsync)(void*,int,size_t,void*);int (*memcpyAsync)(void*,const void*,size_t,int,void*);
    explicit ExtraHipR5(Hip& h):memsetAsync(h.load<decltype(memsetAsync)>("hipMemsetAsync")),memcpyAsync(h.load<decltype(memcpyAsync)>("hipMemcpyAsync")){}
};
struct ArenaR5{
    Hip& h;ExtraHipR5& extra;void* stream;const r5::Plan& plan;std::vector<std::unique_ptr<Buffer>> buffers;
    ArenaR5(Hip& api,ExtraHipR5& x,void* s,const r5::Plan& p):h(api),extra(x),stream(s),plan(p){
        for(size_t size:p.allocations){buffers.push_back(std::make_unique<Buffer>(h,size+256));h.check(extra.memsetAsync(buffers.back()->p,0xd3,size+256,stream),"Initialize guards");}
        h.check(h.stream_sync(stream),"Guard initialization sync");
    }
    ~ArenaR5(){h.stream_sync(stream);}
    void* pointer(r5::Ref r)const{return static_cast<Byte*>(buffers.at(r.allocation)->p)+128+r.offset;}
    void reset(const std::vector<Byte>& fixture){
        for(U i=1;i<buffers.size();++i)h.check(extra.memsetAsync(pointer({i,0}),0,plan.allocations[i],stream),"Reset synthetic plan allocation");
        h.check(extra.memcpyAsync(pointer(plan.firstInput),fixture.data(),fixture.size(),1,stream),"Upload finite synthetic input bit pattern");h.check(h.stream_sync(stream),"Input reset sync");
    }
    std::vector<Byte> get(r5::Ref r,size_t size){plan.bounds(r,size);std::vector<Byte> data(size);h.check(h.copy(data.data(),pointer(r),size,2),"Read plan allocation");return data;}
    void guards(){
        std::array<Byte,256> got{};
        for(U i=0;i<buffers.size();++i){h.check(h.copy(got.data(),buffers[i]->p,128,2),"Read leading guard");h.check(h.copy(got.data()+128,static_cast<Byte*>(pointer({i,0}))+plan.allocations[i],128,2),"Read trailing guard");
            r5::need(std::all_of(got.begin(),got.end(),[](Byte b){return b==0xd3;}),"Device memory guard changed for allocation "+std::to_string(i));}
    }
    std::vector<std::vector<Byte>> snapshot(){
        h.check(h.stream_sync(stream),"Full plan snapshot sync");guards();std::vector<std::vector<Byte>> out(buffers.size());for(U i=1;i<buffers.size();++i)out[i]=get({i,0},plan.allocations[i]);return out;
    }
    J compare(const std::vector<std::vector<Byte>>& base,const std::vector<Byte>& weights){
        h.check(h.stream_sync(stream),"Full plan comparison sync");guards();J rows=J::array();size_t total=0,changed=0;
        constexpr size_t chunk=4*1024*1024;std::vector<Byte> temp(chunk);
        for(U i=0;i<buffers.size();++i){const auto& expected=i?base.at(i):weights;size_t mismatches=0,first=SIZE_MAX;for(size_t at=0;at<expected.size();at+=chunk){size_t n=std::min(chunk,expected.size()-at);h.check(h.copy(temp.data(),static_cast<Byte*>(pointer({i,0}))+at,n,2),"Chunked byte comparison");for(size_t j=0;j<n;++j)if(temp[j]!=expected[at+j]){if(first==SIZE_MAX)first=at+j;++mismatches;}}
            total+=expected.size();changed+=mismatches;rows.push_back({{"allocation",i},{"bytes_compared",expected.size()},{"mismatches",mismatches},{"first_mismatch",first==SIZE_MAX?J(nullptr):J(first)}});}
        return {{"total_bytes_compared",total},{"mismatches",changed},{"guards_intact",true},{"allocations",rows}};
    }
};
struct CompiledCallR5{void* original=nullptr;std::vector<std::vector<Byte>> storage;std::vector<void*> args;};
struct ReplayR5{
    Hip& h;ExtraHipR5& extra;ArenaR5& memory;void* stream;const r5::Plan& plan;std::vector<CompiledCallR5> calls;std::array<void*,2> replacements{};
    ReplayR5(Hip& api,ExtraHipR5& x,ArenaR5& mem,void* s,const r5::Plan& p,void* original,void* candidate):h(api),extra(x),memory(mem),stream(s),plan(p){
        calls.resize(p.commands.size());for(size_t index=0;index<p.commands.size();++index){auto& command=p.commands[index];auto& call=calls[index];if(command.copy)continue;
            h.check(h.function(&call.original,original,command.kernel.c_str()),"Resolve original full-plan kernel");
            for(auto& arg:command.args){call.storage.push_back(arg.bytes);auto& bytes=call.storage.back();for(auto& r:arg.relocations){void* pointer=memory.pointer(r.ref);std::memcpy(bytes.data()+r.at,&pointer,8);}}
            for(auto& bytes:call.storage)call.args.push_back(bytes.data());}
        h.check(h.function(&replacements[0],candidate,"head_direct16_w4"),"Resolve verified r4 16_w4 candidate");h.check(h.function(&replacements[1],candidate,"head_direct32_w4"),"Resolve verified r4 32_w4 candidate");
    }
    void one(size_t index,U mode){
        auto& c=plan.commands[index];auto& call=calls[index];
        if(c.copy){h.check(extra.memcpyAsync(memory.pointer(c.dst),memory.pointer(c.src),c.bytes,3,stream),"Original device-to-device copy");return;}
        auto grid=c.grid,block=c.block;void* function=call.original;
        if(c.kernel==r5::HEAD && mode){function=replacements.at(mode-1);block={128,1,1};grid[1]=mode==1?16:8;}
        h.check(h.launch(function,grid[0],grid[1],grid[2],block[0],block[1],block[2],c.shared,stream,call.args.data(),nullptr),"Original-plan kernel launch");
    }
    void run(U mode){for(size_t i=0;i<calls.size();++i)one(i,mode);}
};
struct ContextR5{std::vector<Byte> input,output;};
static J byte_diff_r5(const std::vector<Byte>& a,const std::vector<Byte>& b){r5::need(a.size()==b.size(),"Context byte counts differ");size_t mismatches=0,first=SIZE_MAX;for(size_t i=0;i<a.size();++i)if(a[i]!=b[i]){if(first==SIZE_MAX)first=i;++mismatches;}return {{"bytes_compared",a.size()},{"mismatches",mismatches},{"first_mismatch",first==SIZE_MAX?J(nullptr):J(first)}};}
static ContextR5 contextual_r5(ReplayR5& replay,U mode){
    ContextR5 result;size_t head=replay.plan.heads.at(0);auto& cmd=replay.plan.commands.at(head);
    for(size_t i=0;i<replay.calls.size();++i){
        if(i==head){replay.h.check(replay.h.stream_sync(replay.stream),"Pre-Head context sync");result.input=replay.memory.get(r5::Plan::pointer(cmd,0),size_t(cmd.grid[0])*8192);}
        replay.one(i,mode);
        if(i==head){replay.h.check(replay.h.stream_sync(replay.stream),"Post-Head context sync");result.output=replay.memory.get(r5::Plan::pointer(cmd,8),size_t(cmd.grid[0])*16384);}
    }
    replay.h.check(replay.h.stream_sync(replay.stream),"Contextual full-plan sync");return result;
}
static std::vector<Byte> fixture_r5(const r5::Plan& plan,U which){
    size_t n=plan.allocations.at(plan.firstInput.allocation)-plan.firstInput.offset;r5::need(n%4==0,"Boundary input region is not divisible by four");std::vector<Byte> result(n,0);
    if(which==0)return result;
    constexpr std::array<float,4> constant{0.1f,0.2f,0.3f,1.0f};
    for(size_t i=0;i<n/4;++i){float v=constant[i%4];if(which==2 && i%4!=3){U x=U(i/4);x^=x>>16;x*=0x7feb352du;x^=x>>15;v=0.03125f+float((x+U(i%4)*73u)%224u)/256.0f;}std::memcpy(result.data()+i*4,&v,4);}return result;
}
static double median_r5(std::vector<float> v){r5::need(!v.empty(),"No timing samples");std::sort(v.begin(),v.end());return v[v.size()/2];}
static J distribution_r5(std::vector<float> v){auto raw=v;std::sort(v.begin(),v.end());return {{"samples_ms",raw},{"median_ms",v[v.size()/2]},{"min_ms",v.front()},{"max_ms",v.back()},{"p90_p10",v[size_t(.9*(v.size()-1))]/v[size_t(.1*(v.size()-1))]}};}
static void gpu_r5(J& report,const fs::path& out,fs::path runtime,fs::path weights,fs::path planPath,int selected){
    auto home=executable_dir();
    if(planPath.empty()){for(const auto* n:{L"1080p.json",L"DLSSNR-rewrite-source-checkpoint.zip"})if(fs::exists(home/n)){planPath=home/n;break;}}
    if(planPath.empty()){std::cout<<"Select the existing DLSSNR-rewrite-source-checkpoint.zip or its plans/1080p.json. No archive entries are extracted to disk."<<std::endl;planPath=browse_r5(L"選擇先前的 DLSSNR-rewrite-source-checkpoint.zip 或 plans/1080p.json",L"Original recovered plan\0*.zip;*.json\0All files\0*.*\0\0");}
    auto rawPlan=r5::plan_bytes(read_r5(planPath,32*1024*1024));auto planHash=hash_r5(rawPlan);report["plan_sha256"]=planHash;r5::need(planHash==r5::PLAN_SHA,"Not the exact recovered 1080p plan; refusing to infer another plan");
    J root=J::parse(rawPlan.begin(),rawPlan.end());report["source_plan_top_level_keys"]=J::array();for(auto it=root.begin();it!=root.end();++it)report["source_plan_top_level_keys"].push_back(it.key());
    // Preserve normalized trace evidence in the local JSON even when a later HIP step fails.
    report["normalized_plan_source"]={{"allocations",root.at("allocations")},{"events",root.at("events")}};save_r5(out,report);
    auto plan=r5::Plan::parse(root);report["planned_allocations"]=plan.allocations.size();report["planned_bytes"]=plan.total;report["plan_contract"]=plan.trace;report["normalized_plan_source"]=nullptr;
    report["plan_validation_passed"]=true;save_r5(out,report);
    if(runtime.empty())for(const auto* n:{L"dlssnr_amd_pass1.dll",L"version.dll"})if(fs::exists(home/n)){runtime=home/n;break;}
    if(weights.empty() && fs::exists(home/L"dlssnr_on_amd_weights.bin"))weights=home/L"dlssnr_on_amd_weights.bin";
    if(runtime.empty())runtime=browse_r5(L"選擇現有 dlssnr_amd_pass1.dll 或原版 version.dll（唯讀）",L"AMD NR runtime\0*.dll;*.hsaco\0All files\0*.*\0\0");
    if(weights.empty())weights=browse_r5(L"選擇現有 dlssnr_on_amd_weights.bin（唯讀）",L"DLSSNR weights\0*.bin\0All files\0*.*\0\0");
    auto container=read_r5(runtime,128*1024*1024);report["runtime_container_sha256"]=hash_r5(container);auto originalCode=original_code_r5(container);container.clear();container.shrink_to_fit();
    auto model=weights_r5(read_r5(weights,192*1024*1024),plan.allocations[0]);auto code=read_r5(home/L"head_r4_gfx1201.hsaco",16*1024*1024);r5::need(hash_r5(code)==CANDIDATE_CODE_SHA,"Candidate code does not match this EXE");
    auto hi=plan.heads[0];auto& head=plan.commands[hi];auto wr=r5::Plan::pointer(head,16);auto found=model.at.find(wr.offset);r5::need(found!=model.at.end() && found->second.size>=524288,"Head does not point to a named, sufficiently large model blob");
    report["head_binding"]={{"command_index",hi},{"groups",head.grid[0]},{"weight_blob",found->second.name},{"weight_gpu_offset",wr.offset},{"weight_blob_bytes",found->second.size},{"source","SHA-pinned recovered command plan; not live game interception"}};
    report["head_weight_binding_in_recovered_plan_verified"]=true;report["weight_archive_sha256"]=WEIGHTS_SHA_R5;report["original_code_sha256"]=ORIGINAL_CODE_SHA_R5;report["candidate_code_sha256"]=CANDIDATE_CODE_SHA;save_r5(out,report);
    std::cout<<"Pinned plan: 44 allocations, 154 launches, 4 copies. Head command="<<hi<<" groups="<<head.grid[0]<<" blob="<<found->second.name<<std::endl;
    Hip h;ExtraHipR5 extra(h);h.check(h.init(0),"HIP initialization");int version=0;h.check(h.runtime_version(&version),"HIP version");report["hip_runtime"]=version;h.check(h.driver_version(&version),"HIP driver version");report["hip_driver"]=version;
    int n=0;h.check(h.count(&n),"GPU enumeration");std::vector<std::string> names;
    for(int i=0;i<n;++i){char name[256]{};h.check(h.name(name,256,i),"GPU name");names.emplace_back(name);std::cout<<"GPU "<<i<<": "<<name<<std::endl;}
    if(selected<0)for(int i=0;i<n;++i)if(names[size_t(i)].find("9070")!=std::string::npos){selected=i;break;}
    r5::need(selected>=0 && selected<n && names[size_t(selected)].find("9070")!=std::string::npos,"No allowed RX9070/gfx1201 device selected; RX6600XT is not supported");
    h.check(h.set(selected),"Select 9070 GPU");report["device_index"]=selected;report["device"]=names[size_t(selected)];save_r5(out,report);
    Resources original(h),replacement(h);h.check(h.module_load(&original.module,originalCode.data()),"Load original GPU module");h.check(h.module_load(&replacement.module,code.data()),"Load r4 replacement GPU module");h.check(h.stream_create(&original.stream),"Create replay stream");h.check(h.event_create(&original.start),"Create start event");h.check(h.event_create(&original.stop),"Create stop event");
    ArenaR5 arena(h,extra,original.stream,plan);h.check(h.copy(arena.pointer({0,0}),model.packed.data(),model.packed.size(),1),"Upload all 153 sorted original weight blobs");
    ReplayR5 replay(h,extra,arena,original.stream,plan,original.module,replacement.module);report["status"]="running_fixed_synthetic_full_plan_differential";report["gpu_attempted"]=true;save_r5(out,report);
    const char* namesCase[]={"zero_first_input_region","constant_f32_bit_pattern","varying_f32_bit_pattern"};
    std::vector<std::unique_ptr<Graph>> graphs;
    for(U scenario=0;scenario<3;++scenario){
        auto input=fixture_r5(plan,scenario);arena.reset(input);ContextR5 baseline=contextual_r5(replay,0);auto state=arena.snapshot();
        J caseReport={{"fixture",namesCase[scenario]},{"first_input_initialized_bytes",input.size()},{"initial_other_tensor_bytes",0},{"context_head_input_bytes",baseline.input.size()},{"context_head_input_sha256",hash_r5(baseline.input)},{"context_head_output_bytes",baseline.output.size()},{"context_head_output_sha256",hash_r5(baseline.output)},{"comparisons",J::array()}};
        size_t nan=std::count_if(baseline.output.begin(),baseline.output.end(),[](Byte b){return (b&127)==127;});caseReport["head_fp8_nan_bytes"]=nan;
        report["cases"].push_back(caseReport);size_t ci=report["cases"].size()-1;save_r5(out,report);
        for(U mode=0;mode<3;++mode){
            arena.reset(input);auto got=contextual_r5(replay,mode);auto inDiff=byte_diff_r5(baseline.input,got.input),outDiff=byte_diff_r5(baseline.output,got.output);auto full=arena.compare(state,model.packed);
            full["mode"]=MODES_R5[mode];full["head_input"]=inDiff;full["head_output"]=outDiff;report["cases"][ci]["comparisons"].push_back(full);save_r5(out,report);
            r5::need(inDiff["mismatches"]==0 && outDiff["mismatches"]==0 && full["mismatches"]==0,"Full-plan or contextual Head byte difference; performance result is not valid");
            std::cout<<namesCase[scenario]<<" / "<<MODES_R5[mode]<<": contextual Head and all "<<plan.total<<" allocation bytes identical, guards intact"<<std::endl;
        }
        if(scenario!=2)continue;
        // Negative control changes only temporary internal Head output, never model/game files.
        // This tests whether a visible end-of-plan region can hide a broken replacement.
        arena.reset(input);std::vector<Byte> changed(size_t(head.grid[0])*16384);for(size_t i=0;i<changed.size();++i)changed[i]=(i&1)?0xb0:0x30;
        for(size_t i=0;i<plan.commands.size();++i){replay.one(i,0);if(i==hi)h.check(extra.memcpyAsync(arena.pointer(r5::Plan::pointer(head,8)),changed.data(),changed.size(),1,original.stream),"Negative-control transient Head override");}
        h.check(h.stream_sync(original.stream),"Negative control sync");arena.guards();auto observed=arena.get(plan.lastObserved,plan.allocations[plan.lastObserved.allocation]-plan.lastObserved.offset);
        const auto& b=state[plan.lastObserved.allocation];std::vector<Byte> before(b.begin()+plan.lastObserved.offset,b.end());auto sensitivity=byte_diff_r5(before,observed);
        report["negative_control"]={{"description","Replace only temporary Head output with alternating finite FP8 values; observed pointer-at8 region of final launch is not asserted to be final game RGB"},{"last_observed_allocation",plan.lastObserved.allocation},{"last_observed_offset",plan.lastObserved.offset},{"comparison",sensitivity},{"observed_region_sensitive",sensitivity["mismatches"].get<size_t>()>0}};save_r5(out,report);
        r5::need(nan==0,"Full-plan Head contains FP8 NaNs; stop before performance interpretation");
        // Capture exactly one original Forward per graph, not repeated forwards with drifting state.
        for(U mode=0;mode<3;++mode){
            auto g=std::make_unique<Graph>(h);h.check(h.begin_capture(original.stream,0),"Begin full-plan graph");
            try{replay.run(mode);}catch(...){void* discarded=nullptr;h.end_capture(original.stream,&discarded);if(discarded)h.graph_destroy(discarded);throw;}
            h.check(h.end_capture(original.stream,&g->g),"End full-plan graph");h.check(h.instantiate(&g->exec,g->g,0),"Instantiate full-plan graph");
            arena.reset(input);h.check(h.graph_launch(g->exec,original.stream),"Validate full-plan graph");auto check=arena.compare(state,model.packed);check["mode"]=MODES_R5[mode];report["graph_validation"].push_back(check);save_r5(out,report);r5::need(check["mismatches"]==0,"Graph replay differs from direct original plan");graphs.push_back(std::move(g));
        }
        for(int warm=0;warm<3;++warm)for(auto& g:graphs){arena.reset(input);h.check(h.graph_launch(g->exec,original.stream),"Full-plan warmup");h.check(h.stream_sync(original.stream),"Full-plan warmup sync");}
        std::array<std::vector<float>,3> train,hold;std::array<U,3> order{0,1,2};std::mt19937 rng(590701);
        for(U round=0;round<22;++round){std::shuffle(order.begin(),order.end(),rng);for(U mode:order){arena.reset(input);h.check(h.event_record(original.start,original.stream),"Full-plan event begin");h.check(h.graph_launch(graphs[mode]->exec,original.stream),"Timed full-plan graph");h.check(h.event_record(original.stop,original.stream),"Full-plan event end");h.check(h.event_sync(original.stop),"Full-plan event wait");float ms=0;h.check(h.event_ms(&ms,original.start,original.stop),"Full-plan event duration");r5::need(ms>0 && std::isfinite(ms),"Invalid full-plan GPU timestamp");(round<15?train[mode]:hold[mode]).push_back(ms);}}
        for(U mode=0;mode<3;++mode){report["timing"].push_back({{"mode",MODES_R5[mode]},{"training",distribution_r5(train[mode])},{"holdout",distribution_r5(hold[mode])},{"holdout_speed_ratio_vs_original",median_r5(hold[0])/median_r5(hold[mode])},{"holdout_saved_ms",median_r5(hold[0])-median_r5(hold[mode])}});std::cout<<MODES_R5[mode]<<" full-plan holdout_ms="<<median_r5(hold[mode])<<std::endl;}
        // Revalidate each path after timed execution; a negative control was never timed or adopted.
        for(U mode=0;mode<3;++mode){arena.reset(input);h.check(h.graph_launch(graphs[mode]->exec,original.stream),"Post-timing replay validation");auto check=arena.compare(state,model.packed);check["mode"]=MODES_R5[mode];report["post_timing_validation"].push_back(check);save_r5(out,report);r5::need(check["mismatches"]==0,"Post-timing full-plan byte mismatch");}
    }
    report["synthetic_full_plan_head_substitution_byte_identical"]=true;
    bool sensitive=report.at("negative_control").at("observed_region_sensitive").get<bool>();
    report["status"]=sensitive?"fixed_synthetic_full_plan_passed_not_game_validation":"fixed_synthetic_full_plan_equal_but_output_sensitivity_unconfirmed";
    report["game_deployment_approved"]=false;
}
int wmain(int argc,wchar_t** argv){
    J report={{"schema",5},{"scope","Exact recovered 1080p command-plan replay with all original kernels except one optional r4 Head; synthetic input bit patterns, no game hooks, no live frame capture"},{"status","initializing"},{"error",""},{"gpu_attempted",false},{"plan_validation_passed",false},{"head_weight_binding_in_recovered_plan_verified",false},{"live_runtime_weight_binding_verified",false},{"synthetic_full_plan_head_substitution_byte_identical",false},{"full_model_quality_verified",false},{"game_tested",false},{"game_deployment_approved",false},{"clock_controlled",false},{"power_state_measured",false},
        {"initialization_contract","Every non-weight allocation is zeroed; the complete remaining first-input allocation region is filled with finite float32 bit patterns. Original scalar argument bytes are not edited. This is not a reconstruction of game preprocessing or recurrent state."},
        {"timing_contract","One Forward per HIP graph. State reset and input upload precede each sample and are excluded. 15 training plus 7 holdout rounds; mode order shuffled. All 154 launches and 4 original D2D copies are timed."},
        {"cases",J::array()},{"graph_validation",J::array()},{"post_timing_validation",J::array()},{"timing",J::array()}};
    fs::path out=L"rdna4-r5-result.json",runtime,weights,plan,fixture;int device=-1;bool cpu=false;
    try{
        for(int i=1;i<argc;++i){std::wstring a=argv[i];if(a==L"--cpu-self-test")cpu=true;else if(a==L"--output" && i+1<argc)out=argv[++i];else if(a==L"--runtime" && i+1<argc)runtime=argv[++i];else if(a==L"--weights" && i+1<argc)weights=argv[++i];else if(a==L"--plan" && i+1<argc)plan=argv[++i];else if(a==L"--fixture" && i+1<argc)fixture=argv[++i];else if(a==L"--device" && i+1<argc)device=std::stoi(argv[++i]);else throw std::runtime_error("Usage: rdna4-r5-test.exe [--plan checkpoint.zip|1080p.json] [--runtime DLL] [--weights BIN] [--device INDEX] [--output JSON] [--cpu-self-test --fixture test.zip]");}
        report["cpu_plan_checks"]=r5::self_test();report["cpu_head_contract_checks"]=head_contract::cpu_tests();
        if(cpu){if(!fixture.empty()){auto raw=r5::plan_bytes(read_r5(fixture,32*1024*1024));r5::need(J::parse(raw.begin(),raw.end())==r5::fixture(),"CPU ZIP fixture differs from expected JSON");report["cpu_zip_fixture_passed"]=true;}report["status"]="cpu_tests_passed_original_plan_and_gpu_not_tested";}
        else{r5::need(fixture.empty(),"--fixture is restricted to CPU self-tests");gpu_r5(report,out,runtime,weights,plan,device);}
        save_r5(out,report);std::cout<<"Saved rdna4-r5-result.json. No original DLL, model file, game setting, clock or voltage changed."<<std::endl;return 0;
    }catch(const std::exception& e){report["status"]="failed";report["error"]=e.what();std::cerr<<"ERROR: "<<e.what()<<std::endl;try{save_r5(out,report);}catch(const std::exception& x){std::cerr<<x.what()<<std::endl;}return 1;}
}
