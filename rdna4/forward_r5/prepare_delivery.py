# SPDX-License-Identifier: MIT
"""Deterministically prepare the delivery build without modifying the r4 GPU kernel.
The exact input Git blob is checked. Generated source is included in the ZIP.
No original model, checkpoint, or GPU execution is implied by this build step.
"""
import hashlib,json
from pathlib import Path
root=Path(__file__).resolve().parent
source=root/'forward_diff.cpp'
raw=source.read_bytes()
# Git hashes the LF source bytes with its blob header. Normalize checkout CRLF only.
raw=raw.replace(b'\r\n',b'\n')
sha=hashlib.sha1(b'blob '+str(len(raw)).encode()+b'\0'+raw).hexdigest()
assert sha=='989558143d6de714da9df072fba54a574e2ed507', 'Unexpected r5 source revision: '+sha
s=raw.decode('utf-8')
def replace(old,new):
    global s
    assert s.count(old)==1, 'Patch target is not unique: '+old[:90]
    s=s.replace(old,new)
replace('static constexpr char* unused_placeholder=nullptr;', '''static std::string delivery_phase="initializing";
static std::string delivery_last_operation;
static size_t delivery_last_command=SIZE_MAX;
static std::array<U,3> delivery_grid{},delivery_block{};
static U delivery_mode=0;
static void delivery_stage(const char* text){delivery_phase=text;std::cout<<"[r5] "<<text<<std::endl;}''')
replace("f<<report.dump(2)<<'\\n';", '''J saved=report;
    saved["delivery_revision"]="r5.1";
    saved["diagnostics"]={{"phase",delivery_phase},{"last_submitted_operation",delivery_last_operation},
        {"last_submitted_command",delivery_last_command==SIZE_MAX?J(nullptr):J(delivery_last_command)},
        {"last_grid",delivery_grid},{"last_block",delivery_block},{"last_mode",delivery_mode},
        {"note","Asynchronous GPU errors can surface after the command that caused them; last submitted does not prove fault location."}};
    f<<saved.dump(2)<<'\\n';''')
replace('auto& c=plan.commands[index];auto& call=calls[index];', '''auto& c=plan.commands[index];auto& call=calls[index];
        delivery_last_command=index;delivery_last_operation=c.copy?"device_memcpy":c.kernel;delivery_mode=mode;delivery_grid=c.grid;delivery_block=c.block;''')
replace('h.check(h.launch(function,grid[0],grid[1],grid[2],block[0],block[1],block[2],c.shared,stream,call.args.data(),nullptr),"Original-plan kernel launch");', '''delivery_grid=grid;delivery_block=block;
        h.check(h.launch(function,grid[0],grid[1],grid[2],block[0],block[1],block[2],c.shared,stream,call.args.data(),nullptr),"Original-plan kernel launch");''')
replace('auto home=executable_dir();', 'delivery_stage("Selecting and verifying the original checkpoint plan");\n    auto home=executable_dir();')
replace('auto plan=r5::Plan::parse(root);', 'delivery_stage("Validating the exact recovered plan contract");\n    auto plan=r5::Plan::parse(root);')
replace('auto container=read_r5(runtime,128*1024*1024);', 'delivery_stage("Reading original runtime as data and verifying model weights");\n    auto container=read_r5(runtime,128*1024*1024);')
replace('r5::need(hash_r5(code)==CANDIDATE_CODE_SHA,"Candidate code does not match this EXE");', '''r5::need(hash_r5(code)==CANDIDATE_CODE_SHA,"Candidate code does not match this EXE");
    r5::need(std::string(CANDIDATE_CODE_SHA)=="62c8ecf66e4290ffbe22230a376b475ef482807fe05c46b957457bff5826cffe", "This is not the byte-identical r4 GPU code validated on the user's RX9070XT");''')
replace('Hip h;ExtraHipR5 extra(h);', 'delivery_stage("Initializing HIP and selecting RX9070");\n    Hip h;ExtraHipR5 extra(h);')
replace('ArenaR5 arena(h,extra,original.stream,plan);', 'delivery_stage("Allocating guarded buffers and uploading original weights");\n    ArenaR5 arena(h,extra,original.stream,plan);')
replace('for(U scenario=0;scenario<3;++scenario){', '''for(U scenario=0;scenario<3;++scenario){
        delivery_stage("Comparing original and replacement full-plan executions");
        std::cout<<"[r5] Fixture "<<(scenario+1)<<"/3: "<<namesCase[scenario]<<std::endl;''')
replace('caseReport["head_fp8_nan_bytes"]=nan;', '''caseReport["head_fp8_nan_bytes"]=nan;
        caseReport["head_nonzero_magnitude_bytes"]=std::count_if(baseline.output.begin(),baseline.output.end(),[](Byte b){return (b&127)!=0;});''')
replace('if(scenario!=2)continue;', '''r5::need(nan==0,"An original-plan Head fixture contains FP8 NaNs; no performance or quality claim is valid");
        if(scenario!=2)continue;
        delivery_stage("Checking whether downstream output is sensitive to a corrupted Head");''')
replace('for(U mode=0;mode<3;++mode){\n            auto g=std::make_unique<Graph>(h);', '''delivery_stage("Capturing and verifying all three complete GPU graphs");
        for(U mode=0;mode<3;++mode){
            auto g=std::make_unique<Graph>(h);''')
old='for(U round=0;round<22;++round){std::shuffle(order.begin(),order.end(),rng);for(U mode:order){arena.reset(input);h.check(h.event_record(original.start,original.stream),"Full-plan event begin");h.check(h.graph_launch(graphs[mode]->exec,original.stream),"Timed full-plan graph");h.check(h.event_record(original.stop,original.stream),"Full-plan event end");h.check(h.event_sync(original.stop),"Full-plan event wait");float ms=0;h.check(h.event_ms(&ms,original.start,original.stop),"Full-plan event duration");r5::need(ms>0 && std::isfinite(ms),"Invalid full-plan GPU timestamp");(round<15?train[mode]:hold[mode]).push_back(ms);}}'
new='''delivery_stage("Measuring full-plan GPU time with reset state before every sample");
        for(U round=0;round<22;++round){
            std::shuffle(order.begin(),order.end(),rng);
            for(U mode:order){
                delivery_mode=mode;delivery_last_operation="complete_graph_replay";delivery_last_command=SIZE_MAX;
                arena.reset(input);
                h.check(h.event_record(original.start,original.stream),"Full-plan event begin");
                h.check(h.graph_launch(graphs[mode]->exec,original.stream),"Timed full-plan graph");
                h.check(h.event_record(original.stop,original.stream),"Full-plan event end");
                h.check(h.event_sync(original.stop),"Full-plan event wait");
                float ms=0;h.check(h.event_ms(&ms,original.start,original.stop),"Full-plan event duration");
                r5::need(ms>0 && std::isfinite(ms),"Invalid full-plan GPU timestamp");
                (round<15?train[mode]:hold[mode]).push_back(ms);
            }
            report["timing_progress"]={{"completed_rounds",round+1},{"required_rounds",22},{"training_ms_by_mode",train},{"holdout_ms_by_mode",hold},{"complete",round==21}};
            save_r5(out,report);
            std::cout<<"[r5] Timing round "<<(round+1)<<"/22 complete; samples saved"<<std::endl;
        }
        delivery_stage("Checking all allocations again after timing");'''
replace(old,new)
replace('report["game_deployment_approved"]=false;\n}', 'report["game_deployment_approved"]=false;\n    delivery_stage("Completed; review JSON scope before interpreting timing");\n}')
replace('int wmain(int argc,wchar_t** argv){','int r5_delivery_entry(int argc,wchar_t** argv){')
s+='''
// Tee the actual program output to a local log without PowerShell policy changes.
class DeliveryTee final : public std::streambuf {
    std::streambuf *console_,*file_;
protected:
    int_type overflow(int_type ch) override {
        if(traits_type::eq_int_type(ch,traits_type::eof()))return traits_type::not_eof(ch);
        auto a=console_->sputc(traits_type::to_char_type(ch));
        auto b=file_->sputc(traits_type::to_char_type(ch));
        return traits_type::eq_int_type(a,traits_type::eof()) || traits_type::eq_int_type(b,traits_type::eof()) ? traits_type::eof() : ch;
    }
    int sync() override {int a=console_->pubsync(),b=file_->pubsync();return a==0 && b==0?0:-1;}
public:
    DeliveryTee(std::streambuf* console,std::streambuf* file):console_(console),file_(file){}
};
int wmain(int argc,wchar_t** argv){
    std::ofstream log(L"rdna4-r5-console.txt",std::ios::binary|std::ios::trunc);
    if(!log){std::cerr<<"Cannot create rdna4-r5-console.txt. Extract the ZIP into a writable directory."<<std::endl;return 2;}
    DeliveryTee out(std::cout.rdbuf(),log.rdbuf()),err(std::cerr.rdbuf(),log.rdbuf());
    auto* oldOut=std::cout.rdbuf(&out);auto* oldErr=std::cerr.rdbuf(&err);
    std::cout<<"RDNA4 r5.1 Full-Plan Differential Test - not a game DLL"<<std::endl;
    int result=r5_delivery_entry(argc,argv);
    std::cout.flush();std::cerr.flush();std::cout.rdbuf(oldOut);std::cerr.rdbuf(oldErr);
    return result;
}
'''
(root/'forward_delivery.cpp').write_text(s,encoding='utf-8',newline='\n')
report={'source_git_blob':sha,'delivery_source_sha256':hashlib.sha256(s.encode()).hexdigest(),
        'changes':['Live C++ console/file logging','Last-submitted-operation diagnostics','Per-round partial timing saved',
                   'Require the exact r4 GPU code validated by the user','Reject FP8 NaNs in every original Head fixture'],
        'new_gpu_kernel':False,'original_plan_read_by_this_script':False,'gpu_executed':False,'game_tested':False}
(root/'build').mkdir(exist_ok=True)
(root/'build'/'delivery-source-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
