// SPDX-License-Identifier: MIT
// Included after r5's ReplayR5, ArenaR5, hashing and fixture helpers.
#pragma once
struct R6Fingerprint{U allocation=0;size_t bytes=0;std::string sha;};
static std::vector<R6Fingerprint> r6_fingerprints(ReplayR5& r,size_t index){
    // Whole allocations for every referenced data pointer; model and shared
    // workspace are excluded HERE, but remain checked by the full-state gate.
    std::set<U> ids{r.plan.lastObserved.allocation};const auto& c=r.plan.commands[index];
    if(c.copy){ids.insert(c.src.allocation);ids.insert(c.dst.allocation);}
    else for(const auto& a:c.args)for(const auto& p:a.relocations)ids.insert(p.ref.allocation);
    ids.erase(0);ids.erase(43);std::vector<R6Fingerprint> result;
    for(U id:ids){auto data=r.memory.get({id,0},r.plan.allocations.at(id));result.push_back({id,data.size(),hash_r5(data)});}
    return result;
}
static J r6_trace(ReplayR5& r,const std::vector<Byte>& input,const std::vector<std::vector<Byte>>& fullBaseline,const std::vector<Byte>& weights,const fs::path& path,J& report){
    const size_t head=r.plan.heads.at(0);const auto& h=r.plan.commands[head];
    const auto output=r5::Plan::pointer(h,8);
    std::vector<std::vector<R6Fingerprint>> baseline(r.plan.commands.size());
    J trace={{"scope","Whole-allocation SHA256 after each command from Head onward; all referenced non-weight/non-workspace buffers and the final observer are inspected. This is real GPU execution only when this mode is run on a supported device."},
        {"baseline_repeat_identical",false},{"mutation_verified_at_head",false},{"commands",J::array()},
        {"original_parameters_modified",false},{"original_model_files_modified",false}};
    std::vector<Byte> override(size_t(h.grid[0])*16384);
    for(size_t i=0;i<override.size();++i)override[i]=(i&1)?0xb0:0x30;
    const std::string mutationHash=hash_r5(override);bool finalSensitive=false;
    for(U pass=0;pass<3;++pass){
        delivery_stage(pass==0?"r6 tracing original baseline":pass==1?"r6 verifying repeatability of causal trace":"r6 tracing a deliberately perturbed Head");
        r.memory.reset(input);
        for(size_t i=0;i<r.plan.commands.size();++i){
            r.one(i,0);
            if(pass==2 && i==head)r.h.check(r.extra.memcpyAsync(r.memory.pointer(output),override.data(),override.size(),1,r.stream),"r6 transient Head overwrite");
            if(i<head)continue;
            r.h.check(r.h.stream_sync(r.stream),"r6 causal checkpoint synchronize");
            auto now=r6_fingerprints(r,i);
            if(pass==0){baseline[i]=std::move(now);continue;}
            const auto& old=baseline[i];r5::need(old.size()==now.size(),"Trace observation-set mismatch");
            J entry={{"command_index",i},{"operation",r.plan.commands[i].copy?"device_memcpy":r.plan.commands[i].kernel},{"buffers",J::array()}};
            bool observerChanged=false;
            for(size_t n=0;n<old.size();++n){
                r5::need(old[n].allocation==now[n].allocation && old[n].bytes==now[n].bytes,"Trace buffer mismatch");
                bool changed=old[n].sha!=now[n].sha;
                if(pass==1)r5::need(!changed,"Original full-plan trace is nondeterministic; candidate selection blocked");
                else{
                    entry["buffers"].push_back({{"allocation",now[n].allocation},{"bytes_hashed",now[n].bytes},{"baseline_sha256",old[n].sha},{"perturbed_sha256",now[n].sha},{"different",changed}});
                    if(now[n].allocation==r.plan.lastObserved.allocation)observerChanged=changed;
                    if(i==head && now[n].allocation==output.allocation){
                        auto actual=r.memory.get(output,override.size());
                        r5::need(hash_r5(actual)==mutationHash && changed,"Negative-control mutation was not actually visible at the Head output");
                        trace["mutation_verified_at_head"]=true;
                    }
                }
            }
            if(pass==2){entry["observer_different"]=observerChanged;trace["commands"].push_back(entry);if(i+1==r.plan.commands.size())finalSensitive=observerChanged;}
            if(pass==2 && (i==head || i%8==0 || i+1==r.plan.commands.size())){
                report["causal_trace"]=trace;save_r5(path,report);
                std::cout<<"[r6] Causal command "<<i<<"/"<<(r.plan.commands.size()-1)<<" saved"<<std::endl;
            }
        }
        r.memory.guards();
        if(pass==1){trace["baseline_repeat_identical"]=true;report["causal_trace"]=trace;save_r5(path,report);}
    }
    trace["final_observer_sensitive"]=finalSensitive;
    trace["final_allocation_difference"]=r.memory.compare(fullBaseline,weights);
    report["causal_trace"]=trace;save_r5(path,report);
    return {{"description","Verified temporary Head overwrite followed by per-command whole-allocation fingerprints and an original-repeat control"},
            {"observed_region_sensitive",finalSensitive},{"mutation_verified_at_head",trace["mutation_verified_at_head"]},
            {"last_observed_allocation",r.plan.lastObserved.allocation},{"last_observed_offset",r.plan.lastObserved.offset}};
}
static J r6_profile(ReplayR5& r,const std::vector<Byte>& input,void* begin,void* end){
    delivery_stage("r6 separate instrumented per-command profiling");
    std::vector<std::vector<double>> samples(r.plan.commands.size());
    for(U pass=0;pass<4;++pass){
        r.memory.reset(input);
        for(size_t i=0;i<r.plan.commands.size();++i){
            r.h.check(r.h.event_record(begin,r.stream),"r6 profile begin");r.one(i,0);
            r.h.check(r.h.event_record(end,r.stream),"r6 profile end");r.h.check(r.h.event_sync(end),"r6 profile synchronize");
            float ms=0;r.h.check(r.h.event_ms(&ms,begin,end),"r6 profile elapsed");r5::need(ms>0 && std::isfinite(ms),"Invalid instrumented timestamp");
            if(pass)samples[i].push_back(ms);
        }
    }
    J commands=J::array();std::map<std::string,double> families;
    for(size_t i=0;i<samples.size();++i){auto name=r.plan.commands[i].copy?"device_memcpy":r.plan.commands[i].kernel;double ms=autolab::median(samples[i]);families[name]+=ms;commands.push_back({{"command_index",i},{"kernel",name},{"samples_ms",samples[i]},{"median_ms",ms}});}
    return {{"scope","Instrumented isolated-dispatch GPU event timings; synchronization and instrumentation change scheduling. Do not divide these sums by uninstrumented full-graph time as an exact bottleneck percentage."},
            {"warmup_passes",1},{"measured_passes",3},{"commands",commands},{"sum_medians_by_symbol_ms",families}};
}
static J r6_screen_report(const J& timing,bool equal,bool sensitive){
    r5::need(timing.is_array() && timing.size()==3,"Unexpected complete timing results");
    const auto baseline=timing[0].at("holdout").at("samples_ms").get<std::vector<double>>();J decisions=J::array();
    for(size_t i=1;i<timing.size();++i){auto trial=timing[i].at("holdout").at("samples_ms").get<std::vector<double>>();auto d=autolab::screen(baseline,trial,equal,sensitive);
        decisions.push_back({{"mode",timing[i].at("mode")},{"accepted_for_further_validation",d.accepted},{"reason",d.reason},{"paired_mean_saved_ms",d.saved_ms},{"bootstrap_lower_ms",d.lower_ms},{"bootstrap_upper_ms",d.upper_ms},{"paired_relative_gain",d.relative}});}
    return {{"minimum_paired_holdout_samples",21},{"minimum_relative_improvement",0.01},{"bootstrap_resamples",2048},{"interpretation","Screening heuristic, not a proof of IID timing or optimality; neither candidate is approved for game deployment."},{"decisions",decisions},{"game_release_approved",false}};
}
