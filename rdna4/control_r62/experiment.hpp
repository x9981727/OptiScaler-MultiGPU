// SPDX-License-Identifier: MIT
// Included after the r5 replay and r6.1 diagnostics. No game hook or deployment.
#pragma once
#include "control_policy.hpp"

struct R62ControlGuard {
    ReplayR5& r;std::vector<Byte> original;float active=0;
    explicit R62ControlGuard(ReplayR5& replay):r(replay){
        r5::need(r.plan.commands.size()==158,"r6.2 requires the pinned 158-command plan");
        const auto& c=r.plan.commands.back();r5::Plan::check_special_boundary(c,false);
        r5::need(c.args.size()==1 && c.args[0].bytes.size()==168,"Final argument ABI changed");
        r5::need(r5::le<U>(c.args[0].bytes,136)==0 && r5::le<U>(c.args[0].bytes,140)==0 && r5::le<U>(c.args[0].bytes,144)==1,"Final baseline control fields differ from the audited zero-gain fixture");
        auto& call=r.calls.back();r5::need(call.storage.size()==1 && call.args.size()==1 && call.args[0]==call.storage[0].data(),"Unexpected argument backing storage");
        original=call.storage[0];
        for(size_t slot:{112u,120u,152u}){auto ref=r5::Plan::pointer(c,slot);r5::need(ref.offset==0,"Unexpected final boundary offset");}
    }
    void set(float gain){
        r.h.check(r.h.stream_sync(r.stream),"r6.2 synchronize before scalar experiment");
        auto& call=r.calls.back();r62::patch(call.storage[0],original,gain);
        r5::need(call.args[0]==call.storage[0].data(),"Argument storage moved during scalar experiment");active=gain;
    }
    void check(){r5::need(r62::isolated(r.calls.back().storage[0],original),"Unrelated final argument changed");}
    void restore(){r.h.check(r.h.stream_sync(r.stream),"r6.2 restoration synchronization");std::copy(original.begin(),original.end(),r.calls.back().storage[0].begin());active=0;r5::need(r.calls.back().storage[0]==original,"Original argument restoration failed");}
    ~R62ControlGuard(){r.h.stream_sync(r.stream);if(r.calls.back().storage[0].size()==original.size())std::copy(original.begin(),original.end(),r.calls.back().storage[0].begin());}
    R62ControlGuard(const R62ControlGuard&)=delete;
};
static std::vector<Byte> r62_observed(ReplayR5& r){auto q=r.plan.lastObserved;return r.memory.get(q,r.plan.allocations.at(q.allocation)-q.offset);}
static void r62_require_finite(const J& view){r5::need(view.at("nan_values").get<size_t>()==0 && view.at("infinite_values").get<size_t>()==0,"Experimental original output has nonfinite float32 values; no performance claim is valid");}

// Negative controls are compared ONLY against an original run with identical scalar values.
static J r62_perturb(ReplayR5& r,const std::vector<Byte>& input,const ContextR5& head,
    const std::vector<Byte>& normal,size_t pattern){
    size_t at=r.plan.heads.at(0);const auto& c=r.plan.commands.at(at);auto target=r5::Plan::pointer(c,8);
    std::vector<Byte> payload(head.output.size());
    for(size_t i=0;i<payload.size();++i)payload[i]=pattern==0?Byte((i&1)?0xb0:0x30):Byte(((i/17)&1)?0xb8:0x38);
    J info={{"pattern",pattern==0?"alternating_fp8_plus_minus_half":"striped_fp8_plus_minus_one"},{"comparisons",J::array()},{"repetitions",0},{"repeat_identical",false}};
    auto mutation=byte_diff_r5(head.output,payload);info["head_mutation"]=mutation;
    r5::need(mutation.at("mismatches").get<size_t>()>0,"Negative-control payload did not change the Head");
    std::vector<Byte> first;
    for(U repeat=0;repeat<2;++repeat){
        r.memory.reset(input);
        for(size_t i=0;i<=at;++i)r.one(i,0);
        r.h.check(r.h.stream_sync(r.stream),"r6.2 pre-perturbation sync");
        r5::need(r.memory.get(target,head.output.size())==head.output,"Original Head prefix is not repeatable under this control");
        r.h.check(r.extra.memcpyAsync(r.memory.pointer(target),payload.data(),payload.size(),1,r.stream),"r6.2 verified transient Head mutation");
        r.h.check(r.h.stream_sync(r.stream),"r6.2 perturbation upload sync");
        r5::need(r.memory.get(target,payload.size())==payload,"Head mutation readback failed");
        for(size_t i=at+1;i<r.calls.size();++i)r.one(i,0);
        r.h.check(r.h.stream_sync(r.stream),"r6.2 perturbed suffix sync");r.memory.guards();
        auto got=r62_observed(r);auto view=r61_float_view(got);r62_require_finite(view);
        info["comparisons"].push_back({{"repeat",repeat},{"output_difference",byte_diff_r5(normal,got)},{"output_sha256",hash_r5(got)},{"float32_view",view}});
        if(!repeat)first=std::move(got);else{r5::need(got==first,"Perturbed full-plan output is not repeatable");info["repeat_identical"]=true;}
        info["repetitions"]=repeat+1;
    }
    info["output_sensitive"]=info["comparisons"][0]["output_difference"]["mismatches"].get<size_t>()>0;
    info["all_guards_intact"]=true;return info;
}

static J r62_benchmark(ReplayR5& r,const std::vector<Byte>& input,
    const std::vector<std::vector<Byte>>& reference,const std::vector<Byte>& weights,
    void* begin,void* end,const fs::path& out,J& report){
    // Predeclared profile: gain=1/32 and varying fixture. Not selected by benchmark outcomes.
    delivery_stage("r6.2 same-control full-graph timing; experimental gain 1/32, not original default or game FPS");
    std::array<std::unique_ptr<Graph>,3> graphs;
    J info={{"scope","All original calls and copies with final scalar136=1/32 in ALL modes; only Head implementation differs. Reset/upload excluded. No game or original-default speedup claim."},
        {"gain",0.03125f},{"training_rounds",45},{"holdout_rounds",21},{"graph_validation",J::array()},{"raw_rounds",J::array()},{"timing",J::array()},{"complete",false}};
    auto save=[&](){report["experimental_same_control_timing"]=info;save_r5(out,report);};save();
    for(U mode=0;mode<3;++mode){
        auto g=std::make_unique<Graph>(r.h);r.h.check(r.h.begin_capture(r.stream,0),"r6.2 begin graph capture");
        try{r.run(mode);}catch(...){void* discarded=nullptr;r.h.end_capture(r.stream,&discarded);if(discarded)r.h.graph_destroy(discarded);throw;}
        r.h.check(r.h.end_capture(r.stream,&g->g),"r6.2 end graph capture");r.h.check(r.h.instantiate(&g->exec,g->g,0),"r6.2 instantiate graph");
        r.memory.reset(input);r.h.check(r.h.graph_launch(g->exec,r.stream),"r6.2 validate graph");
        J valid=r.memory.compare(reference,weights);valid["mode"]=MODES_R5[mode];info["graph_validation"].push_back(valid);save();
        r5::need(valid.at("mismatches").get<size_t>()==0,"Captured experimental graph differs from same-control original");graphs[mode]=std::move(g);
    }
    for(U i=0;i<3;++i)for(auto& g:graphs){r.memory.reset(input);r.h.check(r.h.graph_launch(g->exec,r.stream),"r6.2 warmup");r.h.check(r.h.stream_sync(r.stream),"r6.2 warmup sync");}
    std::array<std::vector<float>,3> train,hold;std::array<U,3> order{0,1,2};std::mt19937 rng(620136);
    for(U round=0;round<66;++round){
        std::shuffle(order.begin(),order.end(),rng);J row={{"round",round},{"phase",round<45?"training":"holdout"},{"order",order},{"observations",J::array()}};
        for(U mode:order){
            r.memory.reset(input);delivery_mode=mode;delivery_last_operation="r6.2 complete same-control graph";delivery_last_command=SIZE_MAX;
            r.h.check(r.h.event_record(begin,r.stream),"r6.2 timing start");r.h.check(r.h.graph_launch(graphs[mode]->exec,r.stream),"r6.2 timed graph");r.h.check(r.h.event_record(end,r.stream),"r6.2 timing end");r.h.check(r.h.event_sync(end),"r6.2 timing completion");
            float ms=std::numeric_limits<float>::quiet_NaN();int rc=r.h.event_ms(&ms,begin,end);auto kind=r61::classify(ms);
            row["observations"].push_back({{"mode",mode},{"hip_return_code",rc},{"raw_f32_bits",std::bit_cast<U>(ms)},{"raw_ms",std::isfinite(ms)?J(ms):J(nullptr)},{"classification",rc?"hip_api_error":r61::name(kind)}});
            if(rc || !(ms>0) || !std::isfinite(ms)){info["interrupted_round"]=row;info["status"]="timestamp_unresolved_or_invalid_no_speed_claim";save();if(rc)r.h.check(rc,"r6.2 elapsed-time API");throw std::runtime_error("Unresolved full-graph timestamp saved without substitution");}
            (round<45?train[mode]:hold[mode]).push_back(ms);
        }
        info["raw_rounds"].push_back(row);info["completed_rounds"]=round+1;save();
        if(round%3==2)std::cout<<"[r6.2] Same-control timing "<<round+1<<"/66 saved"<<std::endl;
    }
    for(U mode=0;mode<3;++mode){
        info["timing"].push_back({{"mode",MODES_R5[mode]},{"training",distribution_r5(train[mode])},{"holdout",distribution_r5(hold[mode])},{"holdout_ratio_same_gain_original",median_r5(hold[0])/median_r5(hold[mode])}});
        r.memory.reset(input);r.h.check(r.h.graph_launch(graphs[mode]->exec,r.stream),"r6.2 post-timing validation");auto valid=r.memory.compare(reference,weights);valid["mode"]=MODES_R5[mode];info["post_timing_validation"].push_back(valid);save();r5::need(valid.at("mismatches").get<size_t>()==0,"Post-timing experimental graph mismatch");
    }
    info["complete"]=true;info["status"]="same_control_experimental_graph_measured_not_game_validation";save();return info;
}

static void r62_experiments(ReplayR5& r,const std::vector<Byte>& weights,void* begin,void* end,const fs::path& out,J& report){
    delivery_stage("r6.2 preserving zero-gain baseline and testing a bounded output-control hypothesis");
    R62ControlGuard control(r);const auto observed=r.plan.lastObserved;
    r5::need(observed.allocation==2 && observed.offset==0 && r.plan.firstInput.allocation==1 && r.plan.firstInput.offset==0,"Unexpected RGB/observer binding");
    r5::need(r.plan.allocations[1]/12==r.plan.allocations[2]/16 && r.plan.allocations[1]%12==0 && r.plan.allocations[2]%16==0,"Unexpected pixel counts for the zero-gain output equation");
    J info={{"scope","Explicit experimental final scalar136 values; not a silently corrected checkpoint, game default, or new GPU kernel"},{"original_gain_bits",0},{"changed_argument_byte_range",{136,140}},{"gain_values_predeclared",r62::GAINS},
        {"original_zero_gain_sensitivity_preserved",false},{"original_output_equation_hypothesis","clamp(8*fma(gain,network,fma(1/8,rgb,-1/16))+1/2), alpha=1; history-pointer null, control144=1"},
        {"profiles",J::array()},{"source_files_modified",false},{"pointers_or_geometry_modified",false},{"weights_modified",false},{"control_restore_complete",false},{"complete",false}};
    report["release_gate"]={{"approved",false},{"reason","Experimental output contribution validation only; original-default sensitivity is kept separate from alternative controls"}};
    report["initialization_contract"]="Non-weight buffers zeroed with finite input fixtures. Original checkpoint parsed and hashed unchanged; ONLY the final compiled call scalar136 is varied explicitly for experimental profiles. Original baseline and all other bytes are preserved.";
    auto save=[&](){report["neural_contribution_experiment"]=info;save_r5(out,report);};save();
    bool allPositiveSensitive=true,zeroBlocked=true;size_t positives=0;
    std::vector<std::vector<Byte>> finalBaseline;
    for(U fixture:{2u,1u}){
        auto input=fixture_r5(r.plan,fixture);std::vector<Byte> zeroOutput;
        for(float gain:r62::GAINS){
            if(fixture==1 && gain>0.03125f)continue;
            control.set(gain);control.check();
            delivery_stage("r6.2 full-plan byte validation with explicitly recorded control value");
            std::cout<<"[r6.2] Fixture="<<fixture<<" final scalar136="<<gain<<" (EXPERIMENTAL, not a game setting)"<<std::endl;
            r.memory.reset(input);auto head=contextual_r5(r,0);auto state=r.memory.snapshot();auto normal=state.at(observed.allocation);
            auto view=r61_float_view(normal);r62_require_finite(view);
            J profile={{"fixture",fixture==2?"varying_f32_bit_pattern":"constant_f32_bit_pattern"},{"scalar136",gain},{"scalar136_bits",std::bit_cast<U>(gain)},
                {"same_control_comparisons",J::array()},{"head_output_sha256",hash_r5(head.output)},{"normal_output_sha256",hash_r5(normal)},{"normal_float32_view",view},{"negative_controls",J::array()},
                {"only_scalar136_modified",r62::isolated(r.calls.back().storage[0],control.original)},{"complete",false}};
            size_t headNan=std::count_if(head.output.begin(),head.output.end(),[](Byte b){return (b&127)==127;});profile["head_fp8_nan_bytes"]=headNan;r5::need(headNan==0,"Nonfinite original Head values invalidate this experiment");
            if(gain==0){
                zeroOutput=normal;auto predicted=r62::rgb_reference(input);profile["zero_gain_cpu_equation_comparison"]=byte_diff_r5(predicted,normal);
                profile["zero_gain_equation_byte_identical"]=predicted==normal;
                if(fixture==2)finalBaseline=state;
            }
            profile["output_difference_vs_same_fixture_zero_gain"]=byte_diff_r5(zeroOutput,normal);
            info["current_profile"]=profile;save();
            for(U mode=0;mode<3;++mode){
                r.memory.reset(input);auto got=contextual_r5(r,mode);auto check=r.memory.compare(state,weights);
                check["mode"]=MODES_R5[mode];check["head_input"]=byte_diff_r5(head.input,got.input);check["head_output"]=byte_diff_r5(head.output,got.output);
                profile["same_control_comparisons"].push_back(check);info["current_profile"]=profile;save();
                r5::need(check["mismatches"].get<size_t>()==0 && check["head_input"]["mismatches"].get<size_t>()==0 && check["head_output"]["mismatches"].get<size_t>()==0,"Same-control original/candidate full-plan byte mismatch");
            }
            bool sensitive=true;
            for(size_t pattern=0;pattern<2;++pattern){
                auto neg=r62_perturb(r,input,head,normal,pattern);sensitive=sensitive&&neg.at("output_sensitive").get<bool>();profile["negative_controls"].push_back(neg);info["current_profile"]=profile;save();
            }
            profile["both_independent_head_perturbations_reach_output"]=sensitive;
            if(gain==0){for(const auto& n:profile["negative_controls"])zeroBlocked=zeroBlocked&&!n["output_sensitive"].get<bool>();}
            else{++positives;allPositiveSensitive=allPositiveSensitive&&sensitive;}
            r.memory.reset(input);r.run(0);auto restored=r.memory.compare(state,weights);r5::need(restored["mismatches"].get<size_t>()==0,"Post-perturbation state not restored");
            profile["state_restored_after_perturbations"]=true;profile["complete"]=true;info["profiles"].push_back(profile);info.erase("current_profile");save();
            std::cout<<"[r6.2] gain="<<gain<<": output changed vs zero="<<profile["output_difference_vs_same_fixture_zero_gain"]["mismatches"]<<", both Head controls sensitive="<<sensitive<<std::endl;
            if(fixture==2 && gain==0.03125f){
                if(sensitive)report["experimental_same_control_timing"]=r62_benchmark(r,input,state,weights,begin,end,out,report);
                else{report["experimental_same_control_timing"]={{"complete",false},{"status","skipped_predeclared_gain_did_not_pass_both_sensitivity_tests"}};save();}
            }
            control.check();
        }
    }
    control.restore();auto input=fixture_r5(r.plan,2);r.memory.reset(input);r.run(0);auto full=r.memory.compare(finalBaseline,weights);
    r5::need(full["mismatches"].get<size_t>()==0,"Original zero-gain full state failed final restoration");
    info["original_zero_gain_sensitivity_preserved"]=zeroBlocked;info["all_tested_positive_profiles_sensitive"]=allPositiveSensitive&&positives>0;
    info["positive_profiles_checked"]=positives;info["control_restore_complete"]=true;info["all_original_state_restored"]=true;info["complete"]=true;
    report["status"]=(zeroBlocked&&allPositiveSensitive&&positives>0)?"experimental_gain_restores_neural_sensitivity_not_game_validation":"control_experiment_complete_hypothesis_not_fully_confirmed";
    report["original_default_output_sensitivity_passed"]=!zeroBlocked;
    // Do not equate a changed-control experiment with validating original game behavior.
    report["full_model_quality_verified"]=false;report["game_tested"]=false;report["game_deployment_approved"]=false;
    delivery_stage("r6.2 complete: original control and state restored; experimental findings saved, no game deployment");save();
}
