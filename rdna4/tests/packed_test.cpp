// SPDX-License-Identifier: MIT
// Reuse r1's tested HIP loader, input generator and CPU reference unchanged.
// Only this wmain is the entry point. r1 remains available as a reference source.
#define wmain rdna4_r1_reference_entry
#include "linear_test.cpp"
#undef wmain
#include <chrono>
#include <memory>
#include <numeric>

constexpr U BATCH=64, TRAIN=15, HOLDOUT=7;
struct Spec { const char* name; U tile,waves; bool packed,fp16; };
constexpr Spec specs[]={
 {"linear_fp8_tile16",16,1,false,false}, {"linear_fp8_tile32",32,1,false,false},
 {"linear_fp16_tile32",32,1,false,true}, {"linear_fp8_packed16",16,1,true,false},
 {"linear_fp8_packed32",32,1,true,false}, {"linear_fp8_packed16_w4",16,4,true,false}};
constexpr U VARIANTS=U(sizeof(specs)/sizeof(specs[0]));
static size_t packed_bytes(U outer,U k){return size_t((outer+31)/32)*32*((k+15)/16)*16;}
static std::vector<Byte> cpu_pack(const std::vector<Byte>& src,U outer,U k){
    if(src.size()!=size_t(outer)*k)throw std::runtime_error("Bad CPU pack input");
    U po=(outer+31)/32*32,pk=(k+15)/16*16,kt=pk/16;
    std::vector<Byte> out(size_t(po)*pk,0);
    for(U o=0;o<outer;++o)for(U j=0;j<k;++j){
        size_t p=(((size_t(o/16)*kt+j/16)*32+((j%16)/8)*16+o%16)*8+j%8);
        out[p]=src[size_t(j)*outer+o];
    }
    return out;
}
static size_t pack_self_test(){
    size_t checks=0;
    for(auto shape: {std::pair<U,U>{1,1},{17,19},{65,33},{128,256}}){
        U outer=shape.first,k=shape.second,kt=(k+15)/16,ot=(outer+31)/32*2;
        std::vector<Byte> src(size_t(outer)*k);
        for(size_t i=0;i<src.size();++i)src[i]=Byte((i*73+29)%256);
        auto packed=cpu_pack(src,outer,k);size_t p=0;
        // Independent sequential decoding checks every byte, including padding.
        for(U a=0;a<ot;++a)for(U b=0;b<kt;++b)for(U lane=0;lane<32;++lane)for(U j=0;j<8;++j){
            U o=a*16+(lane&15),kk=b*16+(lane>>4)*8+j;
            Byte expected=(o<outer && kk<k)?src[size_t(kk)*outer+o]:0;
            if(packed.at(p++)!=expected)throw std::runtime_error("CPU lossless packing check failed");
            ++checks;
        }
        if(p!=packed.size())throw std::runtime_error("CPU pack coverage failed");
    }
    return checks;
}
static double median(std::vector<float> v){
    if(v.empty())return 0;std::sort(v.begin(),v.end());return v[v.size()/2];
}
static double spread(std::vector<float> v){
    if(v.empty())return 0;std::sort(v.begin(),v.end());
    size_t lo=size_t(0.1*double(v.size()-1)),hi=size_t(0.9*double(v.size()-1));
    return v[lo]>0 ? double(v[hi])/v[lo] : 1e30;
}
struct R2Row { Shape s{};U variant=0,mode=0;size_t checked=0,bad=0,nonfinite=0;
    bool guards=true,pack_exact=false;double max_error=0;std::vector<float> train,holdout; };
struct Choice {Shape s{};std::string proposed,chosen,reason;double holdout_speedup=1;};
static std::string label(U variant,U mode){
    return std::string(specs[variant].name)+":"+(mode==0?(specs[variant].packed?"packed_core":"raw_core"):(mode==1?"pack_x_and_core":"pack_both_and_core"));
}
struct R2Report {
    std::string status="not_started",error,device;int device_index=-1,runtime=0,driver=0;
    size_t cpu_checks=0,pack_bytes_checked=0;bool gpu_attempted=false;
    std::vector<R2Row> rows;std::vector<Choice> choices;
    void save(const fs::path& file)const{
        std::ofstream o(file,std::ios::binary|std::ios::trunc);
        if(!o)throw std::runtime_error("Cannot write r2 report");
        o<<std::setprecision(10)<<"{\n\"schema\":2,\n\"scope\":\"Independent linear operators and lossless packing; NOT Swin, NR model or game FPS\",\n"
         <<"\"status\":"<<quote(status)<<",\n\"error\":"<<quote(error)<<",\n\"device\":"<<quote(device)<<",\n\"device_index\":"<<device_index
         <<",\n\"hip_runtime\":"<<runtime<<",\n\"hip_driver\":"<<driver<<",\n\"cpu_checks\":"<<cpu_checks
         <<",\n\"pack_bytes_checked\":"<<pack_bytes_checked<<",\n\"gpu_attempted\":"<<(gpu_attempted?"true":"false")
         <<",\n\"model_equivalence_tested\":false,\n\"game_tested\":false,\n\"clock_controlled\":false,\n\"power_state_measured\":false,\n"
         <<"\"benchmark\":{\"batch\":"<<BATCH<<",\"training_rounds\":"<<TRAIN<<",\"holdout_rounds\":"<<HOLDOUT
         <<",\"minimum_active_warmup_ms_per_shape\":150,\"order\":\"deterministically shuffled each round\",\"excluded\":\"CPU preparation, uploads, full model, game; prepacked weight setup excluded from pack_x_and_core\"},\n\"results\":[\n";
        auto samples=[&](const std::vector<float>& v){o<<'[';for(size_t i=0;i<v.size();++i){if(i)o<<',';o<<v[i];}o<<']';};
        for(size_t i=0;i<rows.size();++i){const auto& r=rows[i];
            o<<"{\"test\":"<<quote(label(r.variant,r.mode))<<",\"m\":"<<r.s.m<<",\"n\":"<<r.s.n<<",\"k\":"<<r.s.k<<",\"flags\":"<<r.s.flags
             <<",\"elements_checked\":"<<r.checked<<",\"total_elements\":"<<size_t(r.s.m)*r.s.n<<",\"mismatches\":"<<r.bad
             <<",\"nonfinite\":"<<r.nonfinite<<",\"guards_intact\":"<<(r.guards?"true":"false")<<",\"max_abs_error\":"<<r.max_error
             <<",\"packed_input_verified\":"<<(r.pack_exact?"true":"false")<<",\"train_samples_ms\":";samples(r.train);
            o<<",\"holdout_samples_ms\":";samples(r.holdout);
            o<<",\"train_median_ms\":"<<median(r.train)<<",\"holdout_median_ms\":"<<median(r.holdout)
             <<",\"train_p90_p10\":"<<spread(r.train)<<",\"holdout_p90_p10\":"<<spread(r.holdout)<<'}'<<(i+1<rows.size()?",":"")<<'\n';
        }
        o<<"],\n\"recommendations_operator_only\":[\n";
        for(size_t i=0;i<choices.size();++i){const auto& c=choices[i];
            o<<"{\"m\":"<<c.s.m<<",\"n\":"<<c.s.n<<",\"k\":"<<c.s.k<<",\"proposed\":"<<quote(c.proposed)<<",\"chosen\":"<<quote(c.chosen)
             <<",\"reason\":"<<quote(c.reason)<<",\"holdout_speedup_vs_r1_tile16\":"<<c.holdout_speedup<<'}'<<(i+1<choices.size()?",":"")<<'\n';
        }
        o<<"]\n}\n";o.flush();if(!o)throw std::runtime_error("r2 report write failed");
    }
};
struct SyncOnExit{Hip& h;void* stream;~SyncOnExit(){h.stream_sync(stream);}};
static void run_r2(R2Report& report,int requested,const fs::path& report_path){
    Hip h;h.check(h.init(0),"hipInit");h.check(h.runtime_version(&report.runtime),"HIP runtime");h.check(h.driver_version(&report.driver),"HIP driver");
    int count=0;h.check(h.count(&count),"HIP devices");std::vector<std::string> names;
    for(int i=0;i<count;++i){char n[256]{};h.check(h.name(n,256,i),"device name");names.emplace_back(n);std::cout<<"GPU "<<i<<": "<<n<<std::endl;}
    if(requested<0)for(int i=0;i<count;++i)if(names[size_t(i)].find("9070")!=std::string::npos){requested=i;break;}
    if(requested<0 || requested>=count || names[size_t(requested)].find("9070")==std::string::npos)
        throw std::runtime_error("No RX9070/gfx1201 GPU selected; refusing unsupported device");
    report.device_index=requested;report.device=names[size_t(requested)];h.check(h.set(requested),"select GPU");
    std::ifstream file(executable_dir()/L"packed_r2_gfx1201.hsaco",std::ios::binary);
    std::vector<char> code((std::istreambuf_iterator<char>(file)),{});
    if(code.size()<64 || code[0]!='\x7f' || code[1]!='E' || code[2]!='L' || code[3]!='F')throw std::runtime_error("Missing/invalid packed_r2_gfx1201.hsaco");
    Resources r(h);h.check(h.module_load(&r.module,code.data()),"load gfx1201 module");h.check(h.stream_create(&r.stream),"stream create");
    h.check(h.event_create(&r.start),"start event");h.check(h.event_create(&r.stop),"stop event");
    std::array<void*,VARIANTS> functions{};void* pack_fn=nullptr;
    for(U i=0;i<VARIANTS;++i)h.check(h.function(&functions[i],r.module,specs[i].name),"find linear kernel");
    h.check(h.function(&pack_fn,r.module,"pack_kmajor_fp8"),"find pack kernel");
    const Shape shapes[]={ {1,1,1,0},{17,19,5,1},{31,35,19,2},{33,37,23,3},
        {129,32,32,3},{257,64,64,3},{129,128,128,3},{65,256,256,3},
        {4096,32,32,3},{4096,64,64,3},{4096,128,128,3},{4096,256,256,3},
        {64,32,32,3},{64,64,64,3},{64,128,128,3},{64,256,256,3},
        {4096,96,32,3},{4096,32,96,3}};
    report.status="running";
    for(Shape s:shapes){
        Inputs in(s);size_t elements=size_t(s.m)*s.n;
        Buffer x8(h,in.x.size()),w8(h,in.w.size()),x16(h,in.x16.size()*2),w16(h,in.w16.size()*2);
        Buffer bias(h,in.bias.size()*4),residual(h,in.residual.size()*4),output(h,(elements+64)*4);
        Buffer xp(h,packed_bytes(s.m,s.k)+256),wp(h,packed_bytes(s.n,s.k)+256);
        struct Task {size_t row;U variant,mode;std::unique_ptr<Graph> graph;};
        std::vector<Task> tasks;SyncOnExit sync{h,r.stream};
        x8.upload(in.x.data());w8.upload(in.w.data());x16.upload(in.x16.data());w16.upload(in.w16.data());
        bias.upload(in.bias.data());residual.upload(in.residual.data());
        void* xpacked=static_cast<Byte*>(xp.p)+128;void* wpacked=static_cast<Byte*>(wp.p)+128;
        auto pack=[&](void* src,void* dst,U outer){
            void* args[]={&src,&dst,&outer,&s.k};
            h.check(h.launch(pack_fn,(outer+31)/32*2,(s.k+15)/16,1,32,1,1,0,r.stream,args,nullptr),"GPU pack");
        };
        auto verify_pack=[&](Buffer& src,Buffer& dst,void* packed,const std::vector<Byte>& raw,U outer){
            std::vector<Byte> host(dst.bytes,0xa5);dst.upload(host.data());report.gpu_attempted=true;
            pack(src.p,packed,outer);h.check(h.stream_sync(r.stream),"pack sync");h.check(h.copy(host.data(),dst.p,dst.bytes,2),"pack download");
            for(size_t j=0;j<128;++j)if(host[j]!=0xa5 || host[host.size()-128+j]!=0xa5)throw std::runtime_error("GPU pack guard overwritten");
            auto want=cpu_pack(raw,outer,s.k);
            if(!std::equal(want.begin(),want.end(),host.begin()+128))throw std::runtime_error("GPU pack is not byte-exact");
            report.pack_bytes_checked+=want.size();
        };
        verify_pack(x8,xp,xpacked,in.x,s.m);verify_pack(w8,wp,wpacked,in.w,s.n);
        std::vector<size_t> positions;
        if(elements<=65536){positions.resize(elements);std::iota(positions.begin(),positions.end(),size_t(0));}
        else {positions={0,1,size_t(s.n-1),size_t(s.n),elements-1,elements-s.n};std::mt19937 rng(71+s.n);
            for(U j=0;j<4096;++j)positions.push_back(rng()%elements);
            std::sort(positions.begin(),positions.end());positions.erase(std::unique(positions.begin(),positions.end()),positions.end());}
        std::vector<float> expected;for(size_t p:positions)expected.push_back(in.reference(p));
        auto launch=[&](U vi,U mode){
            const auto& v=specs[vi];
            if(v.packed && mode>=1)pack(x8.p,xpacked,s.m);
            if(v.packed && mode>=2)pack(w8.p,wpacked,s.n);
            void *px=v.packed?xpacked:(v.fp16?x16.p:x8.p),*pw=v.packed?wpacked:(v.fp16?w16.p:w8.p);
            void *py=static_cast<Byte*>(output.p)+128,*pb=bias.p,*pr=residual.p;float scale=in.scale;
            void* args[]={&px,&pw,&py,&pb,&pr,&s.m,&s.n,&s.k,&scale,&s.flags};
            U gx=(s.m+v.tile*v.waves-1)/(v.tile*v.waves),gy=(s.n+v.tile-1)/v.tile;
            h.check(h.launch(functions[vi],gx,gy,1,32*v.waves,1,1,0,r.stream,args,nullptr),"linear launch");
        };
        auto init_output=[&](){std::vector<float> host(elements+64,123456.75f);
            std::fill(host.begin()+32,host.end()-32,std::numeric_limits<float>::quiet_NaN());output.upload(host.data());};
        auto validate=[&](R2Row& row){
            h.check(h.stream_sync(r.stream),"validation sync");std::vector<float> host(elements+64);
            h.check(h.copy(host.data(),output.p,output.bytes,2),"output download");
            for(size_t p=0;p<32;++p)if(host[p]!=123456.75f || host[elements+32+p]!=123456.75f)row.guards=false;
            for(size_t p=0;p<elements;++p)if(!std::isfinite(host[p+32]))++row.nonfinite;
            for(size_t j=0;j<positions.size();++j){float got=host[positions[j]+32],want=expected[j];++row.checked;
                if(!std::isfinite(got)){++row.bad;continue;}double e=std::abs(double(got)-want);row.max_error=std::max(row.max_error,e);
                if(e>1e-4+std::abs(double(want))*1e-4)++row.bad;}
            if(row.bad || row.nonfinite || !row.guards)throw std::runtime_error("GPU correctness failed: "+label(row.variant,row.mode));
        };
        for(U vi=0;vi<VARIANTS;++vi)for(U mode=0;mode<(specs[vi].packed?3u:1u);++mode){
            R2Row row;row.s=s;row.variant=vi;row.mode=mode;row.pack_exact=specs[vi].packed;
            init_output();launch(vi,mode);validate(row);
            auto g=std::make_unique<Graph>(h);h.check(h.begin_capture(r.stream,0),"begin graph");
            try{for(U j=0;j<BATCH;++j)launch(vi,mode);}catch(...){void* unused=nullptr;h.end_capture(r.stream,&unused);if(unused)h.graph_destroy(unused);throw;}
            h.check(h.end_capture(r.stream,&g->g),"end graph");h.check(h.instantiate(&g->exec,g->g,0),"instantiate graph");
            init_output();h.check(h.graph_launch(g->exec,r.stream),"graph correctness");
            row.checked=0;validate(row);
            tasks.push_back(Task{report.rows.size(),vi,mode,std::move(g)});report.rows.push_back(std::move(row));
        }
        std::cout<<"Shape "<<s.m<<'x'<<s.n<<'x'<<s.k<<": correctness passed; interleaved timing"<<std::endl;
        auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(150);
        do {for(const auto& t:tasks)h.check(h.graph_launch(t.graph->exec,r.stream),"active warmup");h.check(h.stream_sync(r.stream),"warmup sync");}
        while(std::chrono::steady_clock::now()<deadline);
        std::vector<size_t> order(tasks.size());std::iota(order.begin(),order.end(),size_t(0));std::mt19937 rng(123+s.m+s.n+s.k);
        size_t proposed=0;
        for(U round=0;round<TRAIN+HOLDOUT;++round){
            if(round==TRAIN){
                const auto& base=report.rows[tasks[0].row];double best=median(base.train);
                if(spread(base.train)<=1.25)for(size_t ti=1;ti<tasks.size();++ti){const auto& t=tasks[ti];const auto& row=report.rows[t.row];
                    if(specs[t.variant].fp16 || (specs[t.variant].packed && t.mode!=1))continue;
                    U wins=0;for(U j=0;j<TRAIN;++j)if(row.train[j]<base.train[j]*0.95f)++wins;
                    double m=median(row.train);
                    if(spread(row.train)<=1.25 && wins>=12 && m<best*0.95){best=m;proposed=ti;}
                }
            }
            std::shuffle(order.begin(),order.end(),rng);
            for(size_t ti:order){auto& t=tasks[ti];
                h.check(h.event_record(r.start,r.stream),"timer start");h.check(h.graph_launch(t.graph->exec,r.stream),"timed graph");
                h.check(h.event_record(r.stop,r.stream),"timer stop");h.check(h.event_sync(r.stop),"timer sync");float ms=0;
                h.check(h.event_ms(&ms,r.start,r.stop),"elapsed");if(!(ms>0) || !std::isfinite(ms))throw std::runtime_error("Invalid event timer");
                auto& row=report.rows[t.row];(round<TRAIN?row.train:row.holdout).push_back(ms/BATCH);
            }
        }
        const auto& base=report.rows[tasks[0].row];const auto& candidate=report.rows[tasks[proposed].row];
        Choice choice;choice.s=s;choice.proposed=label(candidate.variant,candidate.mode);choice.chosen=label(0,0);
        choice.reason="Keep r1 tile16: no stable training improvement";
        if(proposed){
            U wins=0;for(U j=0;j<HOLDOUT;++j)if(candidate.holdout[j]<base.holdout[j]*0.95f)++wins;
            double ratio=median(base.holdout)/median(candidate.holdout);
            if(wins>=5 && ratio>1.0/0.95 && spread(base.holdout)<=1.25 && spread(candidate.holdout)<=1.25){
                choice.chosen=choice.proposed;choice.reason="Accepted for this operator shape only: packing-aware holdout passed";choice.holdout_speedup=ratio;
            } else choice.reason="Keep r1 tile16: proposed variant failed holdout/stability gate";
        }
        for(const auto& t:tasks){const auto& row=report.rows[t.row];
            std::cout<<label(t.variant,t.mode)<<" train_ms="<<median(row.train)<<" holdout_ms="<<median(row.holdout)<<" spread="<<spread(row.holdout)<<std::endl;}
        std::cout<<"SELECTED (operator only): "<<choice.chosen<<std::endl;report.choices.push_back(choice);report.save(report_path);
    }
    report.status="operator_and_pack_tests_passed_not_model_validation";
}
int wmain(int argc,wchar_t** argv){
    R2Report report;fs::path output=L"rdna4-r2-result.json";bool cpu=false;int requested=-1;
    try{
        for(int i=1;i<argc;++i){std::wstring a=argv[i];if(a==L"--cpu-self-test")cpu=true;
            else if(a==L"--device" && i+1<argc)requested=std::stoi(argv[++i]);
            else if(a==L"--output" && i+1<argc)output=argv[++i];else throw std::runtime_error("Usage: rdna4-r2.exe [--cpu-self-test] [--device INDEX] [--output FILE]");}
        report.cpu_checks=cpu_self_test()+pack_self_test();
        std::cout<<"r2 total CPU checks: "<<report.cpu_checks<<"; no GPU claim from CPU checks"<<std::endl;
        report.save(output); // Check report destination before starting GPU work.
        if(cpu)report.status="cpu_tests_passed_gpu_not_run";else run_r2(report,requested,output);
        report.save(output);std::cout<<"Result saved. No game files or system settings changed."<<std::endl;return 0;
    }catch(const std::exception& e){report.status="failed";report.error=e.what();std::cerr<<"ERROR: "<<e.what()<<std::endl;
        try{report.save(output);}catch(const std::exception& e2){std::cerr<<e2.what()<<std::endl;}return 1;}
}
