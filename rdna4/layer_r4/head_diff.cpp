// SPDX-License-Identifier: MIT
// Standalone original-head differential test. No game injection and no host loading of the original DLL.
#define wmain rdna4_r1_reference_entry
#include "../tests/linear_test.cpp"
#undef wmain
#include "head_contract.hpp"
#include "candidate_hash.hpp"
#include <bcrypt.h>
#include <commdlg.h>
#include <chrono>
#include <memory>
#include <numeric>
#pragma comment(lib,"bcrypt.lib")
#pragma comment(lib,"comdlg32.lib")
namespace hc=head_contract;
constexpr char ORIGINAL_CODE_SHA[]="dd38e6ede167c7a5886ae5d079022f63065b6775384d7588f185036b55877afb";
constexpr char WEIGHTS_FILE_SHA[]="6bf8dc931ef3ccffe18c82de26ab374156e7f19539ffcf8eabaa25dca5cf15ab";
constexpr char BLOCK_NAME[]="block30.layer4.layer";
constexpr char ORIGINAL_FUNCTION[]="_Z12k_final_head10HeadParams";
constexpr unsigned BATCH_R4=16,TRAIN_R4=15,HOLD_R4=7;
static std::string sha256(const std::vector<Byte>& v){
    if(v.size()>ULONG_MAX)throw std::runtime_error("SHA256 input exceeds supported size");
    BCRYPT_ALG_HANDLE a=nullptr;
    if(BCryptOpenAlgorithmProvider(&a,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("SHA256 provider");
    Byte digest[32]{};auto status=BCryptHash(a,nullptr,0,const_cast<Byte*>(v.data()),ULONG(v.size()),digest,32);
    BCryptCloseAlgorithmProvider(a,0);if(status<0)throw std::runtime_error("SHA256 hash");
    std::ostringstream o;for(Byte b:digest)o<<std::hex<<std::setfill('0')<<std::setw(2)<<unsigned(b);return o.str();
}
static std::vector<Byte> read_bytes(const fs::path& p,std::uint64_t limit){
    std::ifstream f(p,std::ios::binary|std::ios::ate);if(!f)throw std::runtime_error("Cannot open selected file");
    auto end=f.tellg();if(end<=0 || std::uint64_t(end)>limit)throw std::runtime_error("Selected file has unsupported size");
    std::vector<Byte> v(static_cast<size_t>(end));f.seekg(0);
    f.read(reinterpret_cast<char*>(v.data()),std::streamsize(v.size()));if(!f)throw std::runtime_error("File read incomplete");return v;
}
template<class T>static T little(const std::vector<Byte>& v,size_t at){
    if(at>v.size() || sizeof(T)>v.size()-at)throw std::runtime_error("Binary field out of range");
    T n{};std::memcpy(&n,v.data()+at,sizeof(T));return n;
}
static std::vector<Byte> extract_original(const std::vector<Byte>& data){
    std::vector<Byte> result;
    for(size_t p=0;p+64<=data.size();++p){
        if(data[p]!=0x7f || data[p+1]!='E' || data[p+2]!='L' || data[p+3]!='F' || data[p+4]!=2 || data[p+5]!=1)continue;
        if(little<uint16_t>(data,p+18)!=224)continue;
        auto shoff=little<uint64_t>(data,p+40);auto es=little<uint16_t>(data,p+58),num=little<uint16_t>(data,p+60);
        if(es!=64 || !num || num>4096 || shoff>data.size()-p)continue;
        uint64_t size=shoff+uint64_t(es)*num;if(size>data.size()-p || size<64)continue;
        std::vector<Byte> raw(data.begin()+p,data.begin()+p+size);
        if(sha256(raw)==ORIGINAL_CODE_SHA){if(!result.empty())throw std::runtime_error("Duplicate matching original code object");result=std::move(raw);}
    }
    if(result.empty())throw std::runtime_error("Selected DLL has no matching original gfx1201 code object. Do not rename another DLL or bypass this check.");
    return result;
}
static std::vector<Byte> extract_weight(const std::vector<Byte>& v){
    if(sha256(v)!=WEIGHTS_FILE_SHA)throw std::runtime_error("Wrong weight SHA256; expected the previously tested 147689451-byte model");
    if(v.size()<16 || std::memcmp(v.data(),"DLSSNRW1",8))throw std::runtime_error("Weight archive magic");
    U count=little<U>(v,8),base=little<U>(v,12);if(count!=153 || base>v.size())throw std::runtime_error("Weight directory dimensions");
    size_t pos=16;std::vector<Byte> selected;std::vector<std::string> names;
    for(U i=0;i<count;++i){
        if(pos>=base)throw std::runtime_error("Truncated weight directory");U n=v[pos++];
        if(!n || n>base-pos || base-pos-n<16)throw std::runtime_error("Invalid weight entry");
        std::string name(reinterpret_cast<const char*>(v.data()+pos),n);pos+=n;
        if(std::find(names.begin(),names.end(),name)!=names.end())throw std::runtime_error("Duplicate weight name");names.push_back(name);
        uint64_t off=little<uint64_t>(v,pos),size=little<uint64_t>(v,pos+8);pos+=16;
        if(off>v.size()-base || size>v.size()-base-off)throw std::runtime_error("Weight payload out of bounds");
        if(name==BLOCK_NAME){if(size!=hc::WEIGHT_BYTES+16)throw std::runtime_error("Unexpected selected block size");selected.assign(v.begin()+base+off,v.begin()+base+off+size);}
    }
    if(pos!=base || selected.empty())throw std::runtime_error("Selected model block missing");
    // A basis-vector test would be nondiscriminating for a column full of NaNs; fail closed.
    for(size_t p=0;p<hc::WEIGHT_BYTES;++p)if((selected[p]&127u)==127u)
        throw std::runtime_error("Selected weight block contains FP8 NaN encodings; the inferred matrix contract needs further inspection");
    return selected;
}
static fs::path browse(const wchar_t* title,const wchar_t* filter){
    std::array<wchar_t,32768> buf{};OPENFILENAMEW o{};o.lStructSize=sizeof(o);o.lpstrFile=buf.data();o.nMaxFile=DWORD(buf.size());
    o.lpstrTitle=title;o.lpstrFilter=filter;o.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_DONTADDTORECENT;
    if(!GetOpenFileNameW(&o))throw std::runtime_error("File selection cancelled or unavailable");return fs::path(buf.data());
}
struct Guarded {
    Buffer b;size_t bytes,prefix;std::vector<Byte> host;
    Guarded(Hip& h,size_t n,size_t skew=0):b(h,n+256+skew),bytes(n),prefix(128+skew),host(b.bytes,0xd3){}
    void* ptr()const{return static_cast<Byte*>(b.p)+prefix;}
    void fill(Byte pattern){std::fill(host.begin(),host.end(),0xd3);std::fill(host.begin()+prefix,host.begin()+prefix+bytes,pattern);b.upload(host.data());}
    void upload(const std::vector<Byte>& v){if(v.size()!=bytes)throw std::runtime_error("Guarded upload size");std::fill(host.begin(),host.end(),0xd3);std::copy(v.begin(),v.end(),host.begin()+prefix);b.upload(host.data());}
    std::vector<Byte> download(){b.h.check(b.h.copy(host.data(),b.p,b.bytes,2),"guarded readback");
        if(!std::all_of(host.begin(),host.begin()+prefix,[](Byte x){return x==0xd3;}) ||
           !std::all_of(host.begin()+prefix+bytes,host.end(),[](Byte x){return x==0xd3;}))throw std::runtime_error("GPU memory guard modified; stop using this experiment");
        return std::vector<Byte>(host.begin()+prefix,host.begin()+prefix+bytes);
    }
};
struct HeadArgs{void* input;void* output;void* weights;};
static_assert(sizeof(HeadArgs)==24 && offsetof(HeadArgs,weights)==16);
struct Candidate{const char* name;U block,grid_y;};
constexpr Candidate candidates[]={ {ORIGINAL_FUNCTION,256,1},{"head_direct16_w1",32,64},{"head_direct16_w4",128,16},{"head_direct32_w4",128,8} };
struct DiffRow{std::string test,variant;U groups=0;size_t checked=0,mismatch=0,first=SIZE_MAX;bool repeated=false;std::string output_sha;};
struct TimingRow{U groups=0;std::string variant;std::vector<float> train,hold;};
static double middle(std::vector<float> v){if(v.empty())return 0;std::sort(v.begin(),v.end());return v[v.size()/2];}
static double variation(std::vector<float> v){if(v.empty())return 0;std::sort(v.begin(),v.end());return v[size_t(.9*(v.size()-1))]/v[size_t(.1*(v.size()-1))];}
struct R4Report{
    std::string status="initializing",error,device,runtime_file_sha,weight_block_sha;int index=-1,hip_runtime=0,hip_driver=0;
    size_t cpu_checks=0,basis_coefficients_checked=0;bool gpu_attempted=false,layer_passed=false;
    std::vector<DiffRow> diffs;std::vector<TimingRow> timing;
    void save(const fs::path& path)const{
        auto temporary=path;temporary+=L".tmp";
        std::ofstream o(temporary,std::ios::binary|std::ios::trunc);if(!o)throw std::runtime_error("Cannot write r4 report");
        o<<std::setprecision(10)<<"{\n\"schema\":4,\n\"scope\":\"One original head kernel versus independent replacements, real weight block, synthetic internal inputs; NOT complete Swin, model or game\",\n"
         <<"\"status\":"<<quote(status)<<",\n\"error\":"<<quote(error)<<",\n\"device\":"<<quote(device)<<",\n\"device_index\":"<<index
         <<",\n\"hip_runtime\":"<<hip_runtime<<",\n\"hip_driver\":"<<hip_driver<<",\n\"cpu_checks\":"<<cpu_checks
         <<",\n\"gpu_attempted\":"<<(gpu_attempted?"true":"false")<<",\n\"original_head_layer_differential_passed\":"<<(layer_passed?"true":"false")
         <<",\n\"basis_coefficients_checked\":"<<basis_coefficients_checked<<",\n\"runtime_container_sha256\":"<<quote(runtime_file_sha)
         <<",\n\"original_code_sha256\":"<<quote(ORIGINAL_CODE_SHA)<<",\n\"candidate_code_sha256\":"<<quote(CANDIDATE_CODE_SHA)
         <<",\n\"weight_archive_sha256\":"<<quote(WEIGHTS_FILE_SHA)<<",\n\"selected_weight_block\":"<<quote(BLOCK_NAME)<<",\n\"weight_block_sha256\":"<<quote(weight_block_sha)
         <<",\n\"full_forward_weight_binding_verified\":false,\n\"full_model_equivalence_tested\":false,\n\"game_tested\":false,\n\"clock_controlled\":false,\n\"power_state_measured\":false,\n"
         <<"\"timing_method\":{\"batch\":16,\"train_rounds\":15,\"holdout_rounds\":7,\"order\":\"shuffled within each round\",\"excluded\":\"CPU setup, uploads, full forward, game, format transitions between other layers\"},\n\"differential\":[\n";
        for(size_t i=0;i<diffs.size();++i){auto& d=diffs[i];o<<"{\"case\":"<<quote(d.test)<<",\"variant\":"<<quote(d.variant)<<",\"groups\":"<<d.groups<<",\"bytes_compared\":"<<d.checked<<",\"mismatches\":"<<d.mismatch<<",\"repeat_identical\":"<<(d.repeated?"true":"false")<<",\"output_sha256\":"<<quote(d.output_sha)<<",\"first_mismatch\":";if(d.first==SIZE_MAX)o<<"null";else o<<d.first;o<<'}'<<(i+1<diffs.size()?",":"")<<'\n';}
        o<<"],\n\"timing\":[\n";
        auto samples=[&](const std::vector<float>& v){o<<'[';for(size_t j=0;j<v.size();++j){if(j)o<<',';o<<v[j];}o<<']';};
        for(size_t i=0;i<timing.size();++i){auto& t=timing[i];o<<"{\"groups\":"<<t.groups<<",\"variant\":"<<quote(t.variant)<<",\"train_ms\":";samples(t.train);o<<",\"holdout_ms\":";samples(t.hold);o<<",\"train_median_ms\":"<<middle(t.train)<<",\"holdout_median_ms\":"<<middle(t.hold)<<",\"holdout_p90_p10\":"<<variation(t.hold)<<'}'<<(i+1<timing.size()?",":"")<<'\n';}
        o<<"]\n}\n";o.close();if(!o)throw std::runtime_error("Report write incomplete");
        if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot finalize r4 report");
    }
};
static size_t compare_bytes(const std::vector<Byte>& a,const std::vector<Byte>& b,size_t& first){
    if(a.size()!=b.size())throw std::runtime_error("Differential size mismatch");size_t count=0;first=SIZE_MAX;
    for(size_t i=0;i<a.size();++i)if(a[i]!=b[i]){if(first==SIZE_MAX)first=i;++count;}return count;
}
static std::vector<Byte> basis_expected(const std::vector<Byte>& w,U start){
    std::vector<Byte> expected(hc::OUTPUT_BYTES);
    for(U r=0;r<16;++r)for(U n=0;n<1024;++n){Byte v=w[hc::weight(start+r,n)];if((v&127)==0)v=0;expected[hc::tensor(r,n)]=v;}
    return expected;
}
static std::vector<Byte> input_for(U groups,int mode,U seed){
    std::vector<Byte> x(size_t(groups)*hc::INPUT_BYTES,0);std::mt19937 rng(seed);
    if(mode>=0){for(U r=0;r<16;++r)x[hc::tensor(r,U(mode)+r)]=0x38;return x;}
    for(Byte& b:x){
        if(mode==-1)b=0;
        else if(mode==-2)b=Byte((rng()%72u)|((rng()%2u)<<7));
        else if(mode==-3)b=Byte((rng()%127u)|((rng()%2u)<<7));
        else b=Byte((rng()%2)?0x80:0x00);
    }return x;
}
static void run_r4(R4Report& report,fs::path runtime,fs::path weights,int requested,const fs::path& out){
    // HIP is loaded before opening dialogs so a missing runtime fails with a clear report.
    Hip hip;hip.check(hip.init(0),"HIP init");hip.check(hip.runtime_version(&report.hip_runtime),"HIP version");hip.check(hip.driver_version(&report.hip_driver),"driver version");
    int count=0;hip.check(hip.count(&count),"HIP device count");std::vector<std::string> names;
    for(int i=0;i<count;++i){char n[256]{};hip.check(hip.name(n,256,i),"GPU name");names.emplace_back(n);std::cout<<"GPU "<<i<<": "<<n<<std::endl;}
    if(requested<0)for(int i=0;i<count;++i)if(names[size_t(i)].find("9070")!=std::string::npos){requested=i;break;}
    if(requested<0 || requested>=count || names[size_t(requested)].find("9070")==std::string::npos)throw std::runtime_error("Select an RX9070/gfx1201 GPU, not RX6600XT");
    report.index=requested;report.device=names[size_t(requested)];hip.check(hip.set(requested),"select GPU");report.save(out);
    auto home=executable_dir();
    if(runtime.empty())for(auto n:{L"dlssnr_amd_pass1.dll",L"version.dll"})if(fs::exists(home/n)){runtime=home/n;break;}
    if(weights.empty() && fs::exists(home/L"dlssnr_on_amd_weights.bin"))weights=home/L"dlssnr_on_amd_weights.bin";
    if(runtime.empty()){std::cout<<"Select existing dlssnr_amd_pass1.dll or original version.dll; it will be read as data, not loaded as a host DLL."<<std::endl;
        runtime=browse(L"選擇現有 dlssnr_amd_pass1.dll 或原版 version.dll（唯讀）",L"AMD NR runtime\0*.dll;*.hsaco\0All files\0*.*\0\0");}
    if(weights.empty()){std::cout<<"Select existing dlssnr_on_amd_weights.bin (read-only)."<<std::endl;
        weights=browse(L"選擇現有 dlssnr_on_amd_weights.bin（唯讀）",L"DLSSNR weights\0*.bin\0All files\0*.*\0\0");}
    auto originalContainer=read_bytes(runtime,128ULL*1024*1024);report.runtime_file_sha=sha256(originalContainer);
    auto originalCode=extract_original(originalContainer);originalContainer.clear();originalContainer.shrink_to_fit();
    auto wb=extract_weight(read_bytes(weights,192ULL*1024*1024));report.weight_block_sha=sha256(wb);
    auto newCode=read_bytes(home/L"head_r4_gfx1201.hsaco",16ULL*1024*1024);
    if(sha256(newCode)!=CANDIDATE_CODE_SHA)throw std::runtime_error("Candidate code hash mismatch; extract the complete r4 kit into a new directory");
    std::cout<<"Original GPU code and real model weight archive SHA256 verified."<<std::endl;
    Resources original(hip),replacement(hip);
    hip.check(hip.module_load(&original.module,originalCode.data()),"load original AMDGPU code object");
    hip.check(hip.module_load(&replacement.module,newCode.data()),"load candidate AMDGPU code object");
    hip.check(hip.stream_create(&original.stream),"create stream");hip.check(hip.event_create(&original.start),"start event");hip.check(hip.event_create(&original.stop),"stop event");
    std::array<void*,4> functions{};
    for(U v=0;v<4;++v)hip.check(hip.function(&functions[v],v?replacement.module:original.module,candidates[v].name),"find head kernel");
    Guarded deviceWeights(hip,wb.size(),12);deviceWeights.upload(wb);
    report.status="running_original_head_differential";report.save(out);
    auto execute_case=[&](const std::string& name,U groups,int mode,U seed,bool measure){
        auto x=input_for(groups,mode,seed);Guarded dx(hip,x.size()),dy(hip,size_t(groups)*hc::OUTPUT_BYTES);dx.upload(x);
        struct Finish{Hip& h;void* s;~Finish(){h.stream_sync(s);}} finish{hip,original.stream};
        HeadArgs p{dx.ptr(),dy.ptr(),deviceWeights.ptr()};void* args[]={&p};
        auto launch=[&](U v){report.gpu_attempted=true;hip.check(hip.launch(functions[v],groups,candidates[v].grid_y,1,candidates[v].block,1,1,0,original.stream,args,nullptr),"head launch");};
        auto once=[&](U v,Byte sentinel){dy.fill(sentinel);launch(v);hip.check(hip.stream_sync(original.stream),"head synchronize");return dy.download();};
        std::vector<Byte> reference;
        for(U v=0;v<4;++v){
            DiffRow row;row.test=name;row.variant=v?candidates[v].name:"original_head";row.groups=groups;
            auto a=once(v,0xa5),b=once(v,0x5a);row.checked=a.size();size_t first=SIZE_MAX;
            row.repeated=compare_bytes(a,b,first)==0;
            if(!row.repeated){row.mismatch=compare_bytes(a,b,row.first);report.diffs.push_back(row);report.save(out);throw std::runtime_error("Nondeterministic or incomplete output: "+row.variant);}
            row.output_sha=sha256(a);
            if(v==0){
                reference=a;
                if(mode>=0){auto expected=basis_expected(wb,U(mode));row.mismatch=compare_bytes(a,expected,row.first);
                    if(row.mismatch){report.diffs.push_back(row);report.save(out);throw std::runtime_error("Original GPU head disagrees with the recovered basis-vector contract; no speedup result is valid");}
                    report.basis_coefficients_checked+=hc::OUTPUT_BYTES;
                }
            }else row.mismatch=compare_bytes(reference,a,row.first);
            report.diffs.push_back(row);
            if(row.mismatch){report.save(out);throw std::runtime_error("Candidate is NOT byte-identical to original head: "+row.variant);}
        }
        if(dx.download()!=x)throw std::runtime_error("Input tensor was modified");
        if(deviceWeights.download()!=wb)throw std::runtime_error("Model weight buffer was modified");
        std::cout<<name<<" groups="<<groups<<": all candidates byte-identical; all output bytes checked, guards intact"<<std::endl;
        report.save(out);
        if(!measure)return;
        std::array<std::unique_ptr<Graph>,4> graphs;size_t offset=report.timing.size();
        for(U v=0;v<4;++v){
            graphs[v]=std::make_unique<Graph>(hip);auto& g=*graphs[v];
            hip.check(hip.begin_capture(original.stream,0),"begin head graph");
            try{for(U j=0;j<BATCH_R4;++j)launch(v);}catch(...){void* discarded=nullptr;hip.end_capture(original.stream,&discarded);if(discarded)hip.graph_destroy(discarded);throw;}
            hip.check(hip.end_capture(original.stream,&g.g),"end head graph");hip.check(hip.instantiate(&g.exec,g.g,0),"instantiate head graph");
            dy.fill(0xa5);hip.check(hip.graph_launch(g.exec,original.stream),"validate graph replay");hip.check(hip.stream_sync(original.stream),"validate graph synchronize");
            auto actual=dy.download();size_t first=SIZE_MAX;if(compare_bytes(reference,actual,first))throw std::runtime_error("Graph replay differs from direct original output");
            TimingRow t;t.groups=groups;t.variant=v?candidates[v].name:"original_head";report.timing.push_back(t);
        }
        auto warmStart=std::chrono::steady_clock::now();
        do{for(auto& g:graphs)hip.check(hip.graph_launch(g->exec,original.stream),"warmup");hip.check(hip.stream_sync(original.stream),"warmup sync");}
        while(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-warmStart).count()<250.0);
        std::mt19937 orderRng(62197+groups);std::array<U,4> order{0,1,2,3};
        for(U round=0;round<TRAIN_R4+HOLD_R4;++round){std::shuffle(order.begin(),order.end(),orderRng);
            for(U v:order){hip.check(hip.event_record(original.start,original.stream),"timestamp begin");hip.check(hip.graph_launch(graphs[v]->exec,original.stream),"timed head graph");hip.check(hip.event_record(original.stop,original.stream),"timestamp end");hip.check(hip.event_sync(original.stop),"wait timing");float ms=0;hip.check(hip.event_ms(&ms,original.start,original.stop),"read timing");
                if(!(ms>0) || !std::isfinite(ms))throw std::runtime_error("Invalid GPU timestamp sample");
                auto& t=report.timing[offset+v];(round<TRAIN_R4?t.train:t.hold).push_back(ms/BATCH_R4);
            }
        }
        for(U v=0;v<4;++v){auto& t=report.timing[offset+v];std::cout<<t.variant<<" groups="<<groups<<" holdout_ms="<<middle(t.hold)<<" p90/p10="<<variation(t.hold)<<std::endl;}
        // Timing does not automatically enable a candidate or modify any game settings.
        report.save(out);
    };
    execute_case("all_zero",1,-1,0,false);
    // All 512 logical input channels are covered: 32 passes x 16 independent basis rows.
    // This independently checks every real matrix coefficient/address against original GPU output.
    for(U start=0;start<512;start+=16)execute_case("basis_"+std::to_string(start),1,int(start),0,false);
    execute_case("signed_zero",17,-4,731,false);
    execute_case("dense_small",1,-2,1001,false);
    execute_case("dense_17",17,-2,1002,true);
    execute_case("dense_256",256,-2,1003,true);
    execute_case("full_finite_fp8_range",17,-3,1004,false);
    if(report.basis_coefficients_checked!=hc::WEIGHT_BYTES)throw std::runtime_error("Incomplete basis coverage");
    report.layer_passed=true;report.status="original_head_differential_passed_not_full_model";
}
int wmain(int argc,wchar_t** argv){
    R4Report report;fs::path output=L"rdna4-r4-result.json",runtime,weights;bool cpu=false;int device=-1;
    try{
        for(int i=1;i<argc;++i){std::wstring a=argv[i];
            if(a==L"--cpu-self-test")cpu=true;
            else if(a==L"--runtime" && i+1<argc)runtime=argv[++i];
            else if(a==L"--weights" && i+1<argc)weights=argv[++i];
            else if(a==L"--output" && i+1<argc)output=argv[++i];
            else if(a==L"--device" && i+1<argc)device=std::stoi(argv[++i]);
            else throw std::runtime_error("Usage: rdna4-r4-test.exe [--cpu-self-test] [--runtime DLL] [--weights BIN] [--device INDEX] [--output JSON]");
        }
        report.cpu_checks=hc::cpu_tests();std::cout<<"CPU layout and rounding checks passed: "<<report.cpu_checks<<". This is NOT GPU validation."<<std::endl;
        if(cpu)report.status="cpu_tests_passed_gpu_not_run";else run_r4(report,runtime,weights,device,output);
        report.save(output);std::cout<<"Saved r4 report. No game files, model files, registry, clocks or voltages changed."<<std::endl;return 0;
    }catch(const std::exception& e){report.status="failed";report.error=e.what();std::cerr<<"ERROR: "<<e.what()<<std::endl;try{report.save(output);}catch(const std::exception& x){std::cerr<<x.what()<<std::endl;}return 1;}
}
