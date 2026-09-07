// SPDX-License-Identifier: MIT
// Standalone test of independent WMMA linear operators. NOT an NR model or game plugin.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using U = unsigned;
using Byte = unsigned char;
static_assert(sizeof(void*) == 8);

static std::string quote(const std::string& s) {
    std::ostringstream o; o << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o << '\\' << c;
        else if (c < 32) o << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << U(c) << std::dec;
        else o << c;
    }
    o << '"'; return o.str();
}
static fs::path executable_dir() {
    std::wstring s(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, s.data(), DWORD(s.size()));
    if (!n || n >= s.size()) throw std::runtime_error("Cannot locate executable");
    s.resize(n); return fs::path(s).parent_path();
}
static float fp8(Byte bits) {
    U e = (bits >> 3) & 15, m = bits & 7;
    if (e == 15 && m == 7) return std::numeric_limits<float>::quiet_NaN();
    float v = e ? std::ldexp(1.0f + float(m)/8.0f, int(e)-7) : std::ldexp(float(m), -9);
    return (bits & 128) ? -v : v;
}
static uint16_t fp8_to_half(Byte bits) {
    float value = fp8(bits);
    if (!std::isfinite(value)) return 0x7e00;
    uint32_t f = std::bit_cast<uint32_t>(value), sign = (f >> 16) & 0x8000;
    if ((f & 0x7fffffff) == 0) return uint16_t(sign);
    // All nonzero finite E4M3FN numbers are exactly representable normal binary16 values.
    int exponent = int((f >> 23) & 255) - 127 + 15;
    if (exponent <= 0 || exponent >= 31) throw std::runtime_error("FP8 to half range error");
    return uint16_t(sign | (U(exponent)<<10) | ((f & 0x7fffff)>>13));
}
static float half_to_float(uint16_t h) {
    U e=(h>>10)&31, m=h&1023;
    float v = e ? std::ldexp(1.0f+float(m)/1024.0f,int(e)-15) : std::ldexp(float(m),-24);
    return (h&0x8000) ? -v : v;
}
struct Shape { U m,n,k,flags; };
struct Inputs {
    Shape s;
    float scale=0.125f;
    std::vector<Byte> x,w;
    std::vector<uint16_t> x16,w16;
    std::vector<float> bias,residual;
    explicit Inputs(Shape shape) : s(shape), x(size_t(s.m)*s.k), w(size_t(s.k)*s.n),
        x16(x.size()),w16(w.size()),bias(s.n),residual(size_t(s.m)*s.n) {
        std::mt19937 rng(90210u+s.m*3+s.n*5+s.k*7+s.flags);
        auto next = [&]() { return Byte((rng()%72u) | ((rng()%2u)<<7)); };
        for(size_t i=0;i<x.size();++i) { x[i]=next(); x16[i]=fp8_to_half(x[i]); }
        for(size_t i=0;i<w.size();++i) { w[i]=next(); w16[i]=fp8_to_half(w[i]); }
        for(float& v:bias) v=float(int(rng()%33)-16)/32.0f;
        for(float& v:residual) v=float(int(rng()%65)-32)/64.0f;
    }
    float reference(size_t pos) const {
        U row=U(pos/s.n), col=U(pos%s.n);
        double sum=0;
        for(U j=0;j<s.k;++j) sum+=double(fp8(x[size_t(j)*s.m+row]))*fp8(w[size_t(j)*s.n+col]);
        float v=float(sum)*scale;
        if(s.flags&1) v+=bias[col];
        if(s.flags&2) v+=residual[pos];
        return v;
    }
};
// Independent CPU simulation of the lane-to-matrix mapping, including partial tiles.
static size_t cpu_self_test() {
    size_t tests=0;
    if(fp8(0x38)!=1.0f || fp8(0x01)!=1.0f/512 || fp8(0x7e)!=448.0f || !std::isnan(fp8(0x7f)))
        throw std::runtime_error("E4M3FN known-value test failed");
    for(U v=0;v<256;++v) if((v&127)!=127) {
        if(fp8(Byte(v))!=half_to_float(fp8_to_half(Byte(v)))) throw std::runtime_error("Exact half conversion failed");
        ++tests;
    }
    for(Shape s : {Shape{1,1,1,0},Shape{17,19,5,1},Shape{31,35,19,2},Shape{33,37,23,3}}) {
        Inputs in(s);
        for(U tile : {16u,32u}) {
            std::vector<float> out(size_t(s.m)*s.n,std::numeric_limits<float>::quiet_NaN());
            for(U bm=0;bm<s.m;bm+=tile) for(U bn=0;bn<s.n;bn+=tile)
              for(U sm=0;sm<tile;sm+=16) for(U sn=0;sn<tile;sn+=16) {
                float c[32][8]{};
                for(U kb=0;kb<s.k;kb+=16) {
                    float a[16][16]{},b[16][16]{};
                    for(U lane=0;lane<32;++lane) for(U j=0;j<8;++j) {
                        U r=lane&15, kk=(lane>>4)*8+j;
                        U mr=bm+sm+r, nc=bn+sn+r;
                        a[r][kk]=(mr<s.m && kb+kk<s.k)?fp8(in.x[size_t(kb+kk)*s.m+mr]):0;
                        b[kk][r]=(nc<s.n && kb+kk<s.k)?fp8(in.w[size_t(kb+kk)*s.n+nc]):0;
                    }
                    for(U lane=0;lane<32;++lane) for(U j=0;j<8;++j) {
                        U r=(lane>>4)*8+j, col=lane&15;
                        for(U k=0;k<16;++k) c[lane][j]+=a[r][k]*b[k][col];
                    }
                }
                for(U lane=0;lane<32;++lane) for(U j=0;j<8;++j) {
                    U r=bm+sm+(lane>>4)*8+j, col=bn+sn+(lane&15);
                    if(r<s.m && col<s.n) {
                        size_t p=size_t(r)*s.n+col;
                        float v=c[lane][j]*in.scale;
                        if(s.flags&1) v+=in.bias[col];
                        if(s.flags&2) v+=in.residual[p];
                        out[p]=v;
                    }
                }
              }
            for(size_t p=0;p<out.size();++p) {
                float want=in.reference(p);
                if(!std::isfinite(out[p]) || std::abs(out[p]-want)>1e-4f+std::abs(want)*1e-4f)
                    throw std::runtime_error("CPU tile mapping check failed");
                ++tests;
            }
        }
    }
    std::cout<<"CPU self-tests passed: "<<tests<<" checks. This is NOT GPU validation.\n";
    return tests;
}

struct Hip {
    HMODULE library=nullptr;
    template<class T> T load(const char* name) {
        auto p=GetProcAddress(library,name);
        if(!p) throw std::runtime_error(std::string("HIP API unavailable: ")+name);
        return reinterpret_cast<T>(p);
    }
    int (*init)(U)=nullptr;
    int (*count)(int*)=nullptr;
    int (*name)(char*,int,int)=nullptr;
    int (*set)(int)=nullptr;
    int (*runtime_version)(int*)=nullptr;
    int (*driver_version)(int*)=nullptr;
    const char* (*error_text)(int)=nullptr;
    int (*malloc_)(void**,size_t)=nullptr;
    int (*free_)(void*)=nullptr;
    int (*copy)(void*,const void*,size_t,int)=nullptr;
    int (*module_load)(void**,const void*)=nullptr;
    int (*module_unload)(void*)=nullptr;
    int (*function)(void**,void*,const char*)=nullptr;
    int (*launch)(void*,U,U,U,U,U,U,U,void*,void**,void**)=nullptr;
    int (*stream_create)(void**)=nullptr;
    int (*stream_destroy)(void*)=nullptr;
    int (*stream_sync)(void*)=nullptr;
    int (*event_create)(void**)=nullptr;
    int (*event_destroy)(void*)=nullptr;
    int (*event_record)(void*,void*)=nullptr;
    int (*event_sync)(void*)=nullptr;
    int (*event_ms)(float*,void*,void*)=nullptr;
    int (*begin_capture)(void*,int)=nullptr;
    int (*end_capture)(void*,void**)=nullptr;
    int (*instantiate)(void**,void*,unsigned long long)=nullptr;
    int (*graph_launch)(void*,void*)=nullptr;
    int (*graph_destroy)(void*)=nullptr;
    int (*exec_destroy)(void*)=nullptr;
    Hip() {
        library=LoadLibraryExW(L"amdhip64_7.dll",nullptr,LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!library) {
            std::array<wchar_t,32768> p{};
            DWORD n=GetEnvironmentVariableW(L"HIP_PATH",p.data(),DWORD(p.size()));
            if(n>0 && n<p.size()) {
                fs::path candidate=fs::path(p.data())/L"bin"/L"amdhip64_7.dll";
                library=LoadLibraryExW(candidate.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            }
        }
        if(!library) throw std::runtime_error("Cannot load AMD HIP 7 (amdhip64_7.dll). HIP 6 is not sufficient. No files or settings were changed.");
        try {
#define API(MEMBER,NAME) MEMBER=load<decltype(MEMBER)>(NAME)
            API(init,"hipInit"); API(count,"hipGetDeviceCount"); API(name,"hipDeviceGetName"); API(set,"hipSetDevice");
            API(runtime_version,"hipRuntimeGetVersion"); API(driver_version,"hipDriverGetVersion"); API(error_text,"hipGetErrorString");
            API(malloc_,"hipMalloc"); API(free_,"hipFree"); API(copy,"hipMemcpy");
            API(module_load,"hipModuleLoadData"); API(module_unload,"hipModuleUnload"); API(function,"hipModuleGetFunction");
            API(launch,"hipModuleLaunchKernel"); API(stream_create,"hipStreamCreate"); API(stream_destroy,"hipStreamDestroy"); API(stream_sync,"hipStreamSynchronize");
            API(event_create,"hipEventCreate"); API(event_destroy,"hipEventDestroy"); API(event_record,"hipEventRecord");
            API(event_sync,"hipEventSynchronize"); API(event_ms,"hipEventElapsedTime");
            API(begin_capture,"hipStreamBeginCapture"); API(end_capture,"hipStreamEndCapture");
            API(instantiate,"hipGraphInstantiateWithFlags"); API(graph_launch,"hipGraphLaunch");
            API(graph_destroy,"hipGraphDestroy"); API(exec_destroy,"hipGraphExecDestroy");
#undef API
        } catch(...) { FreeLibrary(library);library=nullptr;throw; }
    }
    ~Hip(){ if(library) FreeLibrary(library); }
    Hip(const Hip&)=delete;
    void check(int code,const char* operation) const {
        if(code) throw std::runtime_error(std::string(operation)+": "+std::to_string(code)+" "+error_text(code));
    }
};
struct Buffer {
    Hip& h; void* p=nullptr; size_t bytes;
    Buffer(Hip& api,size_t size):h(api),bytes(size){ h.check(h.malloc_(&p,bytes),"hipMalloc"); }
    ~Buffer(){ if(p) h.free_(p); }
    Buffer(const Buffer&)=delete;
    void upload(const void* src){h.check(h.copy(p,src,bytes,1),"upload");}
};
struct Resources {
    Hip& h; void *stream=nullptr,*module=nullptr,*start=nullptr,*stop=nullptr;
    explicit Resources(Hip& api):h(api){}
    ~Resources(){
        if(stream)h.stream_sync(stream);
        if(start)h.event_destroy(start); if(stop)h.event_destroy(stop);
        if(module)h.module_unload(module); if(stream)h.stream_destroy(stream);
    }
};
struct Graph {
    Hip& h; void *g=nullptr,*exec=nullptr;
    explicit Graph(Hip& api):h(api){}
    ~Graph(){if(exec)h.exec_destroy(exec);if(g)h.graph_destroy(g);}
};
struct Result {
    Shape s{}; std::string kernel;
    size_t checked=0,bad=0,nonfinite=0; bool guards=true;
    double max_abs=0; std::vector<float> samples;
};
struct Report {
    std::string status="not_started",error,device;
    int index=-1,runtime=0,driver=0;
    size_t cpu_checks=0;
    bool gpu_attempted=false;
    std::vector<Result> rows;
    void save(const fs::path& path) const {
        std::ofstream o(path,std::ios::binary|std::ios::trunc);
        if(!o) throw std::runtime_error("Cannot write result JSON");
        o<<std::setprecision(10)<<"{\n  \"schema\": 1,\n  \"scope\": \"Independent WMMA linear operators; NOT original Swin, NR inference, or game FPS\",\n"
         <<"  \"status\": "<<quote(status)<<",\n  \"error\": "<<quote(error)<<",\n"
         <<"  \"device\": "<<quote(device)<<",\n  \"device_index\": "<<index<<",\n  \"hip_runtime\": "<<runtime<<",\n  \"hip_driver\": "<<driver<<",\n"
         <<"  \"cpu_checks\": "<<cpu_checks<<",\n  \"gpu_attempted\": "<<(gpu_attempted?"true":"false")<<",\n"
         <<"  \"original_model_equivalence_tested\": false,\n  \"game_tested\": false,\n"
         <<"  \"timing\": \"HIP graph replay: 32 repeated operators per sample, 7 samples; excludes CPU packing, uploads, integration and model execution\",\n"
         <<"  \"results\": [\n";
        for(size_t i=0;i<rows.size();++i){
            const auto& r=rows[i];
            o<<"    {\"kernel\":"<<quote(r.kernel)<<",\"m\":"<<r.s.m<<",\"n\":"<<r.s.n<<",\"k\":"<<r.s.k<<",\"flags\":"<<r.s.flags
             <<",\"elements_checked\":"<<r.checked<<",\"total_elements\":"<<size_t(r.s.m)*r.s.n<<",\"mismatches\":"<<r.bad
             <<",\"nonfinite\":"<<r.nonfinite<<",\"guards_intact\":"<<(r.guards?"true":"false")<<",\"max_abs_error\":"<<r.max_abs<<",\"samples_ms\":[";
            for(size_t j=0;j<r.samples.size();++j){if(j)o<<',';o<<r.samples[j];} o<<"]";
            if(!r.samples.empty()){auto sorted=r.samples;std::sort(sorted.begin(),sorted.end());o<<",\"median_ms\":"<<sorted[sorted.size()/2];}
            o<<'}'<<(i+1<rows.size()?",":"")<<'\n';
        }
        o<<"  ]\n}\n";
        o.flush();if(!o)throw std::runtime_error("Result JSON write failed");
    }
};
static void run_gpu(Report& report,int requested) {
    Hip h; h.check(h.init(0),"hipInit");
    h.check(h.runtime_version(&report.runtime),"hipRuntimeGetVersion");
    h.check(h.driver_version(&report.driver),"hipDriverGetVersion");
    int count=0;h.check(h.count(&count),"hipGetDeviceCount");
    std::vector<std::string> devices;
    for(int i=0;i<count;++i){char name[256]{};h.check(h.name(name,256,i),"hipDeviceGetName");devices.emplace_back(name);std::cout<<"GPU "<<i<<": "<<name<<'\n';}
    int selected=requested;
    if(selected<0)for(int i=0;i<count;++i)if(devices[size_t(i)].find("9070")!=std::string::npos){selected=i;break;}
    if(selected<0 || selected>=count)throw std::runtime_error("RX 9070 device not found. Use --device INDEX only for a gfx1201 GPU; RX6600XT is not supported by this test.");
    report.index=selected;report.device=devices[size_t(selected)];
    if(report.device.find("9070")==std::string::npos)throw std::runtime_error("Selected GPU is not in the RX9070/gfx1201 allowlist. Refusing to run specialized kernels.");
    h.check(h.set(selected),"hipSetDevice");
    auto code_path=executable_dir()/L"linear_gfx1201.hsaco";
    std::ifstream file(code_path,std::ios::binary);
    std::vector<char> code((std::istreambuf_iterator<char>(file)),{});
    if(code.size()<64 || code[0]!='\x7f' || code[1]!='E' || code[2]!='L' || code[3]!='F')throw std::runtime_error("Missing/invalid linear_gfx1201.hsaco next to the EXE");
    Resources r(h);
    h.check(h.module_load(&r.module,code.data()),"hipModuleLoadData(gfx1201)");
    h.check(h.stream_create(&r.stream),"hipStreamCreate");
    h.check(h.event_create(&r.start),"hipEventCreate"); h.check(h.event_create(&r.stop),"hipEventCreate");
    const char* names[]={"linear_fp8_tile16","linear_fp8_tile32","linear_fp16_tile32"};
    std::array<void*,3> functions{};
    for(U i=0;i<3;++i)h.check(h.function(&functions[i],r.module,names[i]),"hipModuleGetFunction");
    const Shape cases[]={ {1,1,1,0},{17,19,5,1},{31,35,19,2},{33,37,23,3},
        {129,32,32,3},{257,64,64,3},{129,128,128,3},{65,256,256,3},
        {4096,32,32,3},{4096,64,64,3},{4096,128,128,3},{4096,256,256,3} };
    U case_index=0;
    for(Shape s:cases){
        Inputs in(s);size_t elements=size_t(s.m)*s.n;
        Buffer x8(h,in.x.size()),w8(h,in.w.size()),x16(h,in.x16.size()*2),w16(h,in.w16.size()*2);
        Buffer bias(h,in.bias.size()*4),residual(h,in.residual.size()*4),output(h,(elements+64)*4);
        x8.upload(in.x.data());w8.upload(in.w.data());x16.upload(in.x16.data());w16.upload(in.w16.data());
        bias.upload(in.bias.data());residual.upload(in.residual.data());
        std::vector<size_t> positions;
        if(elements<=65536){for(size_t p=0;p<elements;++p)positions.push_back(p);}
        else {
            positions={0,1,size_t(s.n-1),size_t(s.n),elements-1,elements-2,elements-s.n};
            std::mt19937 rng(71+s.n);
            for(U j=0;j<4096;++j)positions.push_back(rng()%elements);
            std::sort(positions.begin(),positions.end());positions.erase(std::unique(positions.begin(),positions.end()),positions.end());
        }
        std::vector<float> expected;expected.reserve(positions.size());for(size_t p:positions)expected.push_back(in.reference(p));
        // Rotate variant order across shapes to reduce consistently favoring one variant with cold clocks.
        for(U v=0;v<3;++v){
            U variant=(v+case_index)%3,tile=(variant==0?16u:32u);
            report.rows.push_back(Result{});Result& row=report.rows.back();row.s=s;row.kernel=names[variant];
            std::vector<float> host(elements+64,123456.75f);
            std::fill(host.begin()+32,host.end()-32,std::numeric_limits<float>::quiet_NaN());output.upload(host.data());
            void *px=(variant==2?x16.p:x8.p),*pw=(variant==2?w16.p:w8.p),*py=static_cast<Byte*>(output.p)+128;
            void *pb=bias.p,*pr=residual.p;float scale=in.scale;
            void* args[]={&px,&pw,&py,&pb,&pr,&s.m,&s.n,&s.k,&scale,&s.flags};
            auto launch=[&](){h.check(h.launch(functions[variant],(s.m+tile-1)/tile,(s.n+tile-1)/tile,1,32,1,1,0,r.stream,args,nullptr),"hipModuleLaunchKernel");};
            report.gpu_attempted=true;launch();h.check(h.stream_sync(r.stream),"correctness synchronize");
            h.check(h.copy(host.data(),output.p,output.bytes,2),"download");
            for(size_t p=0;p<32;++p)if(host[p]!=123456.75f || host[elements+32+p]!=123456.75f)row.guards=false;
            for(size_t p=0;p<elements;++p)if(!std::isfinite(host[p+32]))++row.nonfinite;
            for(size_t j=0;j<positions.size();++j){
                float got=host[positions[j]+32],want=expected[j];++row.checked;
                if(!std::isfinite(got)){++row.bad;continue;}
                double error=std::abs(double(got)-want);row.max_abs=std::max(row.max_abs,error);
                if(error>1e-4+std::abs(double(want))*1e-4)++row.bad;
            }
            if(row.bad || row.nonfinite || !row.guards)throw std::runtime_error("GPU correctness failed for "+row.kernel+"; no performance claim is valid");
            Graph g(h);
            h.check(h.begin_capture(r.stream,0),"hipStreamBeginCapture");
            try {for(U j=0;j<32;++j)launch();}
            catch(...){void* discarded=nullptr;h.end_capture(r.stream,&discarded);if(discarded)h.graph_destroy(discarded);throw;}
            h.check(h.end_capture(r.stream,&g.g),"hipStreamEndCapture");
            h.check(h.instantiate(&g.exec,g.g,0),"hipGraphInstantiateWithFlags");
            for(U j=0;j<3;++j)h.check(h.graph_launch(g.exec,r.stream),"graph warmup");
            h.check(h.stream_sync(r.stream),"warmup synchronize");
            for(U j=0;j<7;++j){
                h.check(h.event_record(r.start,r.stream),"event start");h.check(h.graph_launch(g.exec,r.stream),"graph replay");
                h.check(h.event_record(r.stop,r.stream),"event stop");h.check(h.event_sync(r.stop),"event sync");
                float ms=0;h.check(h.event_ms(&ms,r.start,r.stop),"event elapsed");
                if(!(ms>0) || !std::isfinite(ms))throw std::runtime_error("Invalid GPU event timing");
                row.samples.push_back(ms/32.0f);
            }
            auto sorted=row.samples;std::sort(sorted.begin(),sorted.end());
            std::cout<<row.kernel<<" M="<<s.m<<" N="<<s.n<<" K="<<s.k<<" checked="<<row.checked<<" error="<<row.max_abs<<" median_ms="<<sorted[3]<<'\n';
        }
        ++case_index;
    }
    report.status="operator_tests_passed_not_model_validation";
}
int wmain(int argc,wchar_t** argv){
    Report report;fs::path output=L"rdna4-result.json";int requested=-1;bool cpu=false;
    try {
        for(int i=1;i<argc;++i){
            std::wstring a=argv[i];
            if(a==L"--cpu-self-test")cpu=true;
            else if(a==L"--device" && i+1<argc)requested=std::stoi(argv[++i]);
            else if(a==L"--output" && i+1<argc)output=argv[++i];
            else throw std::runtime_error("Usage: rdna4-test.exe [--cpu-self-test] [--device INDEX] [--output FILE]");
        }
        report.cpu_checks=cpu_self_test();
        if(cpu)report.status="cpu_tests_passed_gpu_not_run";else run_gpu(report,requested);
        report.save(output);
        std::cout<<"Saved result JSON. No game files or registry settings were changed.\n";
        return 0;
    }catch(const std::exception& e){
        report.status="failed";report.error=e.what();
        std::cerr<<"ERROR: "<<e.what()<<'\n';
        try{report.save(output);}catch(const std::exception& write_error){std::cerr<<write_error.what()<<'\n';}
        return 1;
    }
}
