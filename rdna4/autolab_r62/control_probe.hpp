// SPDX-License-Identifier: MIT
// Included after r61 diagnostics; no original GPU code is modified.
#pragma once
#include "control_policy.hpp"

struct R62Trial {
    std::vector<Byte> output;
    std::string headSha,tailSha;
    size_t mutationBytes=0;
};
static J r62_output_view(const std::vector<Byte>& data){
    J view=r61_float_view(data);J channels=J::array();
    for(size_t channel=0;channel<4;++channel){
        size_t finite=0,nonfinite=0,nonzero=0;double low=0,high=0;
        for(size_t p=channel*4;p+4<=data.size();p+=16){float v;std::memcpy(&v,data.data()+p,4);
            if(!std::isfinite(v)){++nonfinite;continue;}
            if(!finite){low=high=v;}else{low=std::min(low,double(v));high=std::max(high,double(v));}
            ++finite;if(v!=0)++nonzero;
        }
        channels.push_back({{"channel",channel},{"finite",finite},{"nonfinite",nonfinite},{"nonzero",nonzero},
            {"min",finite?J(low):J(nullptr)},{"max",finite?J(high):J(nullptr)}});
    }
    view["float4_channels_diagnostic_only"]=channels;return view;
}
static bool r62_finite(const std::vector<Byte>& data){
    if(data.empty() || data.size()%4)return false;
    for(size_t p=0;p<data.size();p+=4){float v;std::memcpy(&v,data.data()+p,4);if(!std::isfinite(v))return false;}
    return true;
}
static R62Trial r62_trial(ReplayR5& r,const std::vector<Byte>& input,float gain,unsigned mutation,U mode,int outputSeed=-1){
    const size_t last=r.plan.commands.size()-1,head=r.plan.heads.at(0);
    auto& storage=r.calls.at(last).storage.at(0);const auto before=storage;
    std::vector<size_t> slots;for(const auto& p:r.plan.commands.at(last).args.at(0).relocations)slots.push_back(p.at);
    const auto headOut=r5::Plan::pointer(r.plan.commands.at(head),8);
    const auto tail=r5::Plan::pointer(r.plan.commands.at(last),152);
    R62Trial result;r.memory.reset(input);
    for(size_t i=0;i<last;++i){
        r.one(i,mode);
        if(i==head){
            r.h.check(r.h.stream_sync(r.stream),"r6.2 Head inspection sync");
            auto value=r.memory.get(headOut,size_t(r.plan.commands.at(head).grid[0])*16384);
            if(mutation){
                auto payload=r62::mutate_head(value,mutation);
                result.mutationBytes=byte_diff_r5(value,payload).at("mismatches").get<size_t>();
                r5::need(result.mutationBytes>0,"Head perturbation did not change any byte");
                r.h.check(r.extra.memcpyAsync(r.memory.pointer(headOut),payload.data(),payload.size(),1,r.stream),"r6.2 diagnostic Head overwrite");
                r.h.check(r.h.stream_sync(r.stream),"r6.2 overwrite synchronization");
                value=r.memory.get(headOut,payload.size());r5::need(value==payload,"Head mutation readback failed");
            }
            result.headSha=hash_r5(value);
        }
    }
    r.h.check(r.h.stream_sync(r.stream),"r6.2 pre-final synchronization");
    result.tailSha=hash_r5(r.memory.get(tail,r.plan.allocations.at(tail.allocation)-tail.offset));
    const auto observed=r.plan.lastObserved;const size_t bytes=r.plan.allocations.at(observed.allocation)-observed.offset;
    if(outputSeed>=0)r.h.check(r.extra.memsetAsync(r.memory.pointer(observed),outputSeed,bytes,r.stream),"r6.2 output coverage seed");
    {
        r62::ScopedControl patch(storage,slots,gain);
        r5::need(patch.only_control_changed(),"Unexpected modification outside terminal control");
        r.one(last,mode);
        // Argument storage stays at the same address, and is restored only
        // after successful completion (RAII also restores it on exceptions).
        r.h.check(r.h.stream_sync(r.stream),"r6.2 terminal control execution");
    }
    r5::need(storage==before,"Terminal argument restoration failed");r.memory.guards();
    result.output=r.memory.get(observed,bytes);return result;
}

static J r62_control_probes(ReplayR5& r,const std::vector<Byte>& input,
    const std::vector<std::vector<Byte>>& baseline,const std::vector<Byte>& weights,
    const fs::path& path,J& report){
    delivery_stage("r6.2 testing the recorded zero terminal control against explicit nonzero experiments");
    const size_t last=r.plan.commands.size()-1;
    r5::need(last==157 && r.plan.heads.at(0)==59,"Unexpected fixed-plan command boundaries");
    const auto& c=r.plan.commands.at(last);r5::Plan::check_special_boundary(c,false);
    r5::need(c.args.size()==1 && c.args.at(0).bytes.size()==r62::argument_bytes,"Terminal ABI differs");
    r5::need(r.plan.lastObserved.allocation==2 && r.plan.lastObserved.offset==0,"Unexpected terminal observer");
    const auto savedStorage=r.calls.at(last).storage.at(0);
    const auto recordedBits=r62::bits_at(c.args.at(0).bytes,r62::control_offset);
    r5::need(recordedBits==0 && r62::bits_at(savedStorage,r62::control_offset)==recordedBits,"Recorded control is not the expected positive zero");
    J info={{"scope","Byte 136 final-kernel control A/B on the SHA-pinned synthetic plan. Nonzero controls are EXPERIMENTAL, not recovered game settings or a repaired original plan."},
        {"control_byte_offset",136},{"recorded_u32_bits",recordedBits},{"recorded_float32",0.0},
        {"experiment_values",J::array({0.0,0.125,1.0})},{"pointer_or_geometry_changes",false},
        {"gpu_code_changes",false},{"original_plan_file_changes",false},{"trials",J::array()},
        {"head_mode_changes_only_in_candidate_trials",true},{"complete",false}};
    report["release_gate"]={{"approved",false},{"reason","Output-control investigation; no game release approval"}};
    auto save=[&](){report["output_control_experiment"]=info;save_r5(path,report);};save();
    bool originalSensitive=false,gainOneSensitive=false;bool originalMutationConfirmed=false;
    size_t nonzeroSensitiveControls=0;
    for(float gain:{0.0f,0.125f,1.0f}){
        J row={{"terminal_control",gain},{"differs_from_recorded_control",gain!=0},
            {"repeated_baseline_identical",false},{"perturbations",J::array()},{"head_candidates",J::array()}};
        auto progress=[&](const char* operation){info["active_trial"]={{"control",gain},{"operation",operation}};info["current_control"]=row;save();};
        progress("fresh original baseline");auto original=r62_trial(r,input,gain,0,0);
        progress("repeat original baseline");auto repeated=r62_trial(r,input,gain,0,0);
        r5::need(original.output==repeated.output && original.headSha==repeated.headSha && original.tailSha==repeated.tailSha,"Control baseline is nondeterministic");
        row["repeated_baseline_identical"]=true;row["baseline_output_sha256"]=hash_r5(original.output);
        row["baseline_float32_view"]=r62_output_view(original.output);row["baseline_all_float32_finite"]=r62_finite(original.output);
        row["baseline_head_sha256"]=original.headSha;row["baseline_pre_final_allocation26_sha256"]=original.tailSha;
        if(gain==0)r5::need(original.output==baseline.at(2),"Unmodified control did not reproduce the already-validated original output");
        std::vector<Byte>().swap(repeated.output);
        bool coverage=true;
        for(int seed:{0xa5,0x5a}){
            progress("output overwrite coverage");auto sentinel=r62_trial(r,input,gain,0,0,seed);
            auto diff=byte_diff_r5(original.output,sentinel.output);coverage=coverage && diff.at("mismatches").get<size_t>()==0;
            row["coverage_trials"].push_back({{"fill_byte",seed},{"difference",diff}});
        }
        row["entire_observer_independent_of_two_output_seeds"]=coverage;
        bool controlSensitive=false;
        for(unsigned mutation:{1u,2u}){
            progress(mutation==1?"alternating finite FP8 Head perturbation":"Head sign-bit perturbation");
            auto changed=r62_trial(r,input,gain,mutation,0);
            auto again=r62_trial(r,input,gain,mutation,0);
            r5::need(changed.output==again.output && changed.headSha==again.headSha && changed.tailSha==again.tailSha,"Control perturbation is nondeterministic");
            auto diff=byte_diff_r5(original.output,changed.output);
            bool finite=r62_finite(original.output) && r62_finite(changed.output);
            bool tailDifferent=changed.tailSha!=original.tailSha;
            bool sensitive=changed.mutationBytes>0 && tailDifferent && diff.at("mismatches").get<size_t>()>0 && finite && coverage;
            row["perturbations"].push_back({{"pattern",mutation==1?"alternating_finite_fp8":"flip_fp8_sign_bit"},
                {"mutation_bytes",changed.mutationBytes},{"gpu_overwrite_readback_verified",true},
                {"repeat_identical",true},{"tail_allocation26_different",tailDifferent},
                {"mutated_head_sha256",changed.headSha},{"pre_final_allocation26_sha256",changed.tailSha},
                {"output_difference",diff},{"output_sha256",hash_r5(changed.output)},
                {"output_float32_view",r62_output_view(changed.output)},
                {"finite_and_fully_written_sensitive_output",sensitive}});
            controlSensitive=controlSensitive || sensitive;
            if(gain==0)originalMutationConfirmed=originalMutationConfirmed || changed.mutationBytes>0;
            progress("perturbation result saved");
        }
        row["head_to_output_sensitive_under_this_control"]=controlSensitive;
        if(gain==0)originalSensitive=controlSensitive;else if(controlSensitive)++nonzeroSensitiveControls;
        if(gain==1){
            gainOneSensitive=controlSensitive;
            if(controlSensitive){
                progress("capture full original state with explicit control 1");
                auto initial=r62_trial(r,input,gain,0,0);auto controlState=r.memory.snapshot();
                r5::need(initial.output==original.output,"Nonzero-control baseline changed before candidate checks");
                for(U mode:{1u,2u}){
                    progress(mode==1?"full-state head_direct16_w4 comparison at control 1":"full-state head_direct32_w4 comparison at control 1");
                    auto candidate=r62_trial(r,input,gain,0,mode);auto comparison=r.memory.compare(controlState,weights);
                    comparison["mode"]=MODES_R5[mode];comparison["control_value"]=gain;
                    comparison["head_bytes_sha_equal"]=candidate.headSha==original.headSha;
                    comparison["terminal_output_difference"]=byte_diff_r5(original.output,candidate.output);
                    row["head_candidates"].push_back(comparison);progress("candidate state comparison saved");
                    r5::need(candidate.headSha==original.headSha && candidate.output==original.output && comparison.at("mismatches").get<size_t>()==0,"Head substitution failed the explicit nonzero-control full-state comparison");
                }
                row["both_head_candidates_full_state_equal_under_control1"]=true;
            }else row["candidate_comparisons_skipped_reason"]="No finite fully-written Head-sensitive output under control 1";
        }
        info["trials"].push_back(row);info.erase("current_control");info.erase("active_trial");save();
        std::cout<<"[r6.2] Explicit terminal control "<<gain<<": Head-to-output sensitivity="<<(controlSensitive?"observed":"not established")<<std::endl;
    }
    r5::need(r.calls.at(last).storage.at(0)==savedStorage,"Original argument bytes not restored");
    delivery_stage("r6.2 restoring the entire original zero-control plan state");
    r.memory.reset(input);r.run(0);auto restored=r.memory.compare(baseline,weights);
    r5::need(restored.at("mismatches").get<size_t>()==0,"Original state differs after control experiments");
    info["original_argument_bytes_restored"]=true;info["original_allocation_state_restored"]=true;
    info["original_control_head_to_output_sensitive"]=originalSensitive;
    info["experimental_control1_head_to_output_sensitive"]=gainOneSensitive;
    info["nonzero_sensitive_control_count"]=nonzeroSensitiveControls;
    info["zero_control_suppression_hypothesis_supported_by_trials"]=!originalSensitive && nonzeroSensitiveControls==2;
    info["interpretation"]="Even a supported zero-control suppression hypothesis is not proof of the intended game control value, real-frame quality, preprocessing, history or runtime binding.";
    info["complete"]=true;save();
    return {{"description","Original ZERO-control gate only; separately labelled nonzero experiments cannot open this gate"},
        {"observed_region_sensitive",originalSensitive},{"mutation_verified_at_head",originalMutationConfirmed},
        {"last_observed_allocation",2},{"last_observed_offset",0},
        {"experimental_nonzero_result_does_not_override_original_gate",true}};
}
