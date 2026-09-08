// SPDX-License-Identifier: MIT
// Included AFTER the existing r6 trace helpers and the r5 replay implementation.
#pragma once
#include "measurement_policy.hpp"
#include <optional>

static J r61_float_view(const std::vector<Byte>& bytes){
    size_t finite=0,nans=0,infs=0,nonzero=0;double lo=0,hi=0;
    for(size_t p=0;p+4<=bytes.size();p+=4){
        float v;std::memcpy(&v,bytes.data()+p,4);
        if(std::isnan(v)){++nans;continue;}if(!std::isfinite(v)){++infs;continue;}
        if(!finite){lo=hi=v;}else{lo=std::min(lo,double(v));hi=std::max(hi,double(v));}
        ++finite;if(v!=0)++nonzero;
    }
    return {{"interpretation","Diagnostic float32 view only; byte comparisons are authoritative"},
        {"finite_values",finite},{"nan_values",nans},{"infinite_values",infs},{"nonzero_finite_values",nonzero},
        {"finite_min",finite?J(lo):J(nullptr)},{"finite_max",finite?J(hi):J(nullptr)},
        {"trailing_uninterpreted_bytes",bytes.size()%4}};
}

static J r61_boundary_probes(ReplayR5& r,const std::vector<Byte>& input,
    const std::vector<std::vector<Byte>>& baseline,const std::vector<Byte>& weights,
    const fs::path& path,J& report){
    delivery_stage("r6.1 isolating the final boundary with original arguments unchanged");
    const size_t last=r.plan.commands.size()-1;const auto& c=r.plan.commands.at(last);
    r5::need(last==157,"Unexpected pinned last command");r5::Plan::check_special_boundary(c,false);
    const auto observed=r.plan.lastObserved;
    const size_t outBytes=r.plan.allocations.at(observed.allocation)-observed.offset;
    struct Target{size_t slot;U id;bool f32;};
    const std::array<Target,3> targets{{{152,26,false},{0,5,false},{120,1,true}}};
    J info={{"scope","Isolated original final kernel after a fresh complete original prefix for EVERY trial; no candidate or game deployment is enabled"},
        {"last_command",last},{"original_arguments_changed",false},{"control_scalar_changes",J::array()},
        {"full_prefix_replayed_per_trial",true},{"trials",J::array()},{"complete",false}};
    J scalars=J::array();
    // Preserve raw values rather than guessing names such as exposure/strength.
    for(size_t at:{80u,84u,88u,92u,96u,100u,104u,108u,128u,132u,136u,140u,144u,148u}){
        bool pointer=false;for(const auto& rel:c.args.at(0).relocations)if(at<rel.at+8 && rel.at<at+4)pointer=true;
        if(pointer)continue;
        auto bits=r5::le<std::uint32_t>(c.args.at(0).bytes,at);float v=std::bit_cast<float>(bits);
        scalars.push_back({{"byte_offset",at},{"u32_bits",bits},{"f32_view",std::isfinite(v)?J(v):J(nullptr)}});
    }
    info["uninterpreted_nonpointer_scalars"]=scalars;
    auto save=[&](){report["boundary_probe"]=info;save_r5(path,report);};save();
    auto prefix=[&](){
        r.memory.reset(input);for(size_t i=0;i<last;++i)r.one(i,0);
        r.h.check(r.h.stream_sync(r.stream),"r6.1 pre-final sync");
    };
    auto suffix=[&](){r.one(last,0);r.h.check(r.h.stream_sync(r.stream),"r6.1 final sync");r.memory.guards();return r.memory.get(observed,outBytes);};
    prefix();
    std::map<U,std::string> prefixHashes;
    for(const auto& t:targets){auto ref=r5::Plan::pointer(c,t.slot);r5::need(ref.allocation==t.id && ref.offset==0,"Boundary probe target changed");
        prefixHashes[t.id]=hash_r5(r.memory.get(ref,r.plan.allocations.at(t.id)));
    }
    auto original=suffix();
    const auto& expected=baseline.at(observed.allocation);
    r5::need(observed.offset==0 && expected.size()==outBytes,"Unsupported observer range");
    r5::need(original==expected,"Original final output failed fresh-prefix reproduction");
    info["original_output_sha256"]=hash_r5(original);info["original_output_float32_view"]=r61_float_view(original);
    info["original_prefix_and_final_reproduced"]=true;save();
    std::vector<Byte> firstSeedOutput;
    for(int pattern:{0xa5,0x5a}){
        prefix();r.h.check(r.extra.memsetAsync(r.memory.pointer(observed),pattern,outBytes,r.stream),"r6.1 seed output sentinel");
        auto got=suffix();
        J row={{"kind","output_sentinel"},{"fill_byte",pattern},{"difference_from_normal_output",byte_diff_r5(original,got)},
            {"sha256",hash_r5(got)}};info["trials"].push_back(row);
        if(firstSeedOutput.empty())firstSeedOutput=std::move(got);
        else info["output_sentinel_comparison"]=byte_diff_r5(firstSeedOutput,got);
        save();
    }
    firstSeedOutput.clear();firstSeedOutput.shrink_to_fit();
    for(const auto& target:targets)for(bool zero:{true,false}){
        auto ref=r5::Plan::pointer(c,target.slot);const size_t bytes=r.plan.allocations.at(target.id);
        auto payload=r61::probe_bytes(bytes,target.f32,zero);
        J row={{"kind","single_boundary_buffer_perturbation"},{"argument_byte_offset",target.slot},
            {"allocation",target.id},{"mutated_bytes",bytes},{"pattern",zero?"all_zero":target.f32?"finite_float32_pattern":"finite_fp8_pattern"},
            {"original_parameters_modified",false},{"verified_overwrite",false},{"repeat_identical",false}};
        std::vector<Byte> first;
        for(U repeat=0;repeat<2;++repeat){
            prefix();auto before=r.memory.get(ref,bytes);
            r5::need(hash_r5(before)==prefixHashes.at(target.id),"Original pre-final target is nondeterministic");
            if(!repeat)row["target_byte_change"]=byte_diff_r5(before,payload);
            r.h.check(r.extra.memcpyAsync(r.memory.pointer(ref),payload.data(),payload.size(),1,r.stream),"r6.1 boundary overwrite");
            r.h.check(r.h.stream_sync(r.stream),"r6.1 overwrite sync");
            r5::need(r.memory.get(ref,bytes)==payload,"Boundary overwrite readback failed");
            row["verified_overwrite"]=true;
            auto got=suffix();
            if(!repeat){
                row["output_difference"]=byte_diff_r5(original,got);row["output_sha256"]=hash_r5(got);
                row["output_float32_view"]=r61_float_view(got);first=std::move(got);
                row["repetitions_completed"]=1;
                info["current_trial"]=row;save();
            }else{r5::need(first==got,"Perturbed final output is not repeatable");row["repeat_identical"]=true;row["repetitions_completed"]=2;}
        }
        row["effective_mutation"]=row.at("target_byte_change").at("mismatches").get<size_t>()!=0;
        row["output_sensitive_in_this_trial"]=row.at("effective_mutation").get<bool>() && row.at("output_difference").at("mismatches").get<size_t>()!=0;
        info["trials"].push_back(row);info.erase("current_trial");save();
        std::cout<<"[r6.1] Boundary slot "<<target.slot<<" / allocation "<<target.id<<" / "<<(zero?"zero":"finite")
                 <<": changed output bytes="<<row.at("output_difference").at("mismatches")<<std::endl;
    }
    r.memory.reset(input);r.run(0);
    auto restored=r.memory.compare(baseline,weights);
    r5::need(restored.at("mismatches").get<size_t>()==0,"Post-probe original-state restoration failed");
    info["original_full_state_restored"]=true;info["complete"]=true;
    info["probe_results_do_not_override_original_sensitivity_gate"]=true;
    save();return info;
}

struct R61Events{
    Hip& h;void* stream;std::vector<void*> values;
    R61Events(Hip& api,void* s,size_t count):h(api),stream(s){
        values.reserve(count);
        try{for(size_t i=0;i<count;++i){void* e=nullptr;h.check(h.event_create(&e),"r6.1 event creation");values.push_back(e);}}
        catch(...){for(void* e:values)h.event_destroy(e);throw;}
    }
    ~R61Events(){h.stream_sync(stream);for(void* e:values)h.event_destroy(e);}
    R61Events(const R61Events&)=delete;
};

static J r61_profile(ReplayR5& r,const std::vector<Byte>& input,
    const std::vector<std::vector<Byte>>& baseline,const std::vector<Byte>& weights,
    const fs::path& path,J& report){
    delivery_stage("r6.1 queued per-command profiling with unresolved-zero handling");
    const size_t n=r.plan.commands.size();constexpr U passes=7;
    R61Events events(r.h,r.stream,n+1);
    J profile={{"scope","Consecutive distinct HIP events on the original stream; one synchronization after each whole plan. Instrumentation and CPU submission still change scheduling. Not a game benchmark or a share of uninstrumented graph time."},
        {"event_pair_reused_within_pass",false},{"host_sync_between_commands",false},{"commands_per_pass",n},
        {"required_measured_passes",passes},{"completed_passes",0},{"raw_observations",J::array()},
        {"commands",J::array()},{"zero_policy","Retain as unresolved; never interpret as free work, replace with epsilon, or discard silently"},
        {"complete",false}};
    auto save=[&](){report["instrumented_profile"]=profile;save_r5(path,report);};save();
    std::vector<std::vector<float>> samples(n);
    r.memory.reset(input);r.run(0);r.h.check(r.h.stream_sync(r.stream),"r6.1 profile warmup");
    for(U pass=0;pass<passes;++pass){
        r.memory.reset(input);profile["active_pass"]=pass;save();
        r.h.check(r.h.event_record(events.values[0],r.stream),"r6.1 profile first event");
        for(size_t i=0;i<n;++i){r.one(i,0);r.h.check(r.h.event_record(events.values[i+1],r.stream),"r6.1 profile boundary event");}
        r.h.check(r.h.event_sync(events.values.back()),"r6.1 whole-profile-pass synchronization");
        r.memory.guards();
        for(size_t i=0;i<n;++i){
            float ms=std::numeric_limits<float>::quiet_NaN();
            const int rc=r.h.event_ms(&ms,events.values[i],events.values[i+1]);
            auto kind=r61::classify(ms);
            const auto& c=r.plan.commands[i];const std::string symbol=c.copy?"device_memcpy":c.kernel;
            J row={{"pass",pass},{"command_index",i},{"operation",symbol},{"hip_return_code",rc},
                {"raw_f32_bits",std::bit_cast<std::uint32_t>(ms)},{"raw_ms",std::isfinite(ms)?J(ms):J(nullptr)},
                {"classification",rc?"hip_api_error":r61::name(kind)}};
            profile["raw_observations"].push_back(row);
            if(rc || kind==r61::SampleKind::Invalid){
                profile["failure"]=row;save();
                if(rc)r.h.check(rc,"r6.1 elapsed-time API");
                throw std::runtime_error("Invalid GPU timestamp at command "+std::to_string(i)+"; raw value and bits were preserved in JSON");
            }
            samples[i].push_back(ms);
        }
        profile["completed_passes"]=pass+1;save();
        std::cout<<"[r6.1] Profile pass "<<(pass+1)<<"/"<<passes<<": raw timestamps saved"<<std::endl;
    }
    std::map<std::string,double> partialSums;std::map<std::string,size_t> counts,unresolved;
    J ranking=J::array();size_t zeros=0;
    for(size_t i=0;i<n;++i){
        const std::string name=r.plan.commands[i].copy?"device_memcpy":r.plan.commands[i].kernel;
        auto median=r61::strict_median(samples[i]);
        size_t z=std::count(samples[i].begin(),samples[i].end(),0.0f);zeros+=z;++counts[name];
        J row={{"command_index",i},{"kernel",name},{"raw_samples_ms",samples[i]},
            {"zero_samples",z},{"strict_median_ms",median?J(*median):J(nullptr)},
            {"timing_resolved",bool(median)}};
        profile["commands"].push_back(row);
        if(median){partialSums[name]+=*median;ranking.push_back(row);}else ++unresolved[name];
    }
    std::sort(ranking.begin(),ranking.end(),[](const J& a,const J& b){return a.at("strict_median_ms").get<double>()>b.at("strict_median_ms").get<double>();});
    J families=J::array();for(const auto& [name,count]:counts){
        families.push_back({{"symbol",name},{"command_count",count},{"unresolved_commands",unresolved[name]},
            {"resolved_command_median_sum_ms",partialSums[name]},
            {"complete_family_median_sum_ms",unresolved[name]?J(nullptr):J(partialSums[name])}});
    }
    profile["families"]=families;profile["ranked_resolved_commands"]=ranking;
    profile["zero_observations"]=zeros;profile["all_commands_timing_resolved"]=zeros==0;
    auto same=r.memory.compare(baseline,weights);
    r5::need(same.at("mismatches").get<size_t>()==0,"Instrumented original plan changed the reference state");
    profile["original_full_state_equal_after_profile"]=true;profile["complete"]=true;
    profile["status"]=zeros?"completed_with_unresolved_zero_intervals":"completed_all_intervals_positive";
    save();return profile;
}
