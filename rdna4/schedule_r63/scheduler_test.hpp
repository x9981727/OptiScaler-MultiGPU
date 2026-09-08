// SPDX-License-Identifier: MIT
// Included after the verified replay and r62 control helpers.
#pragma once
#include "patch_manifest.hpp"

static const char* R63_MODES[]={"original_swin_scheduling","alternate_special32_scheduling","alternate_all_swin_scheduling"};
struct R63Functions {
    ReplayR5& r;std::vector<std::pair<size_t,void*>> saved;
    R63Functions(ReplayR5& replay,const std::map<std::string,void*>& alternate,U mode):r(replay){
        r5::need(mode<3,"Unknown Swin scheduler mode");
        for(size_t i=0;i<r.plan.commands.size();++i){const auto& c=r.plan.commands[i];
            if(!c.copy && r63::selected(c.kernel,mode)){r5::need(alternate.contains(c.kernel),"Missing alternate Swin symbol");saved.emplace_back(i,r.calls[i].original);}
        }
        r5::need(saved.size()==(mode==0?0u:mode==1?3u:46u),"Unexpected number of affected Swin calls");
        for(const auto& [i,unused]:saved){(void)unused;r.calls[i].original=alternate.at(r.plan.commands[i].kernel);}
    }
    ~R63Functions(){r.h.stream_sync(r.stream);for(const auto& [i,p]:saved)r.calls[i].original=p;}
    R63Functions(const R63Functions&)=delete;
};
static std::vector<size_t> r63_slots(const ReplayR5& r){
    std::vector<size_t> slots;for(const auto& rel:r.plan.commands.back().args.at(0).relocations)slots.push_back(rel.at);return slots;
}
static ContextR5 r63_context(ReplayR5& r,const std::map<std::string,void*>& alternate,
    const std::vector<Byte>& input,float gain,U mode){
    r.memory.reset(input);R63Functions functions(r,alternate,mode);
    auto slots=r63_slots(r);r62::ScopedControl control(r.calls.back().storage.at(0),slots,gain);
    auto result=contextual_r5(r,0); // ALWAYS original Head; only Swin scheduling is varied.
    r5::need(control.only_control_changed(),"Unexpected non-control argument modification");return result;
}
static R62Trial r63_perturb(ReplayR5& r,const std::map<std::string,void*>& alternate,
    const std::vector<Byte>& input,float gain,U mode,U mutation,int seed=-1){
    R63Functions functions(r,alternate,mode);return r62_trial(r,input,gain,mutation,0,seed);
}
static bool r63_compare_state(const J& diff){return diff.at("mismatches").get<size_t>()==0 && diff.at("guards_intact").get<bool>();}

static void r63_run(ReplayR5& r,const std::vector<Byte>& originalCode,const std::vector<Byte>& weights,const fs::path& path,J& report){
    report["scope"]="Pinned complete 1080p synthetic Forward: original Swin arithmetic with alternate GFX12 scheduling descriptors; original Head throughout. Not a game DLL or a rewritten Swin math implementation.";
    report["timing_contract"]="One complete 154-launch plus 4-copy graph per sample. Same explicit gain=1 in all modes. State reset and upload excluded. 36 training and 30 holdout rounds; every six rounds balance each mode in every order position.";
    report["scheduling_gate"]={{"approved",false},{"reason","Validation not yet completed"}};
    report["release_gate"]={{"approved",false},{"reason","Synthetic plan only; game binding, temporal correctness and real-frame performance unverified"}};
    report["status"]="r63_validating_swin_scheduling";
    report["original_head_used_in_all_scheduler_modes"]=true;
    report["original_arithmetic_instruction_bytes_changed"]=false;
    report["original_barriers_removed"]=false;
    report["hardware_scheduling_register_read_back"]=false;
    report["synthetic_full_plan_head_substitution_byte_identical"]=false; // Not the purpose of this run.
    const auto initialArguments=r.calls.back().storage.at(0);
    r5::need(r62::bits_at(initialArguments,136)==0,"Expected original zero output-control value");
    delivery_stage("r6.3 verifying and loading the alternate Swin scheduling descriptors");
    r5::need(hash_r5(originalCode)==r63::original_sha,"Original module hash differs from scheduling contract");
    auto alternateCode=r63::patch(originalCode,r63::patches);
    r5::need(hash_r5(alternateCode)==r63::patched_sha,"Constructed scheduling module hash mismatch");
    report["scheduler_module"]={{"original_sha256",r63::original_sha},{"alternate_sha256",r63::patched_sha},
        {"descriptor_word_bit",21},{"changed_byte_count",r63::patches.size()},{"original_files_modified",false},
        {"instruction_sections_unchanged",true},{"edits",J::array()}};
    for(const auto& p:r63::patches)report["scheduler_module"]["edits"].push_back({{"symbol",p.symbol},{"word_file_offset",p.offset},
        {"original_word",p.original},{"alternate_word",p.replacement},{"original_round_robin",bool(p.original&r63::rr_mask)},
        {"alternate_round_robin",bool(p.replacement&r63::rr_mask)}});
    save_r5(path,report);
    Resources alternative(r.h);r.h.check(r.h.module_load(&alternative.module,alternateCode.data()),"Load alternate Swin scheduler module");
    std::map<std::string,void*> functions;
    for(const auto& p:r63::patches){void* f=nullptr;r.h.check(r.h.function(&f,alternative.module,p.symbol),"Resolve alternate Swin scheduler");functions.emplace(p.symbol,f);}
    const auto varying=fixture_r5(r.plan,2);
    delivery_stage("r6.3 retaining the original zero-control reference and its closed sensitivity gate");
    auto zero=r63_context(r,functions,varying,0,0);auto zeroState=r.memory.snapshot();
    report["recorded_zero_control"]={{"value",0.0},{"output_sha256",hash_r5(zeroState.at(2))},{"perturbations",J::array()}};
    bool zeroSensitive=false;
    for(U mutation:{1u,2u}){
        auto changed=r63_perturb(r,functions,varying,0,0,mutation);
        auto diff=byte_diff_r5(zeroState.at(2),changed.output);zeroSensitive=zeroSensitive || diff.at("mismatches").get<size_t>()>0;
        report["recorded_zero_control"]["perturbations"].push_back({{"pattern",mutation},{"head_changed_bytes",changed.mutationBytes},{"output_difference",diff}});
        save_r5(path,report);
    }
    report["negative_control"]={{"observed_region_sensitive",zeroSensitive},{"control_value",0},{"not_overridden_by_experimental_control",true}};
    r5::need(!zeroSensitive,"Original zero-control result differs from the preceding r6.2 evidence");
    report["scheduler_correctness"]=J::array();
    const char* fixtures[]={"zero_f32_fixture","constant_f32_fixture","varying_f32_fixture"};
    std::vector<std::vector<Byte>> timedState;
    for(U fixture=0;fixture<3;++fixture){
        delivery_stage("r6.3 checking full-plan scheduling candidates at the SAME explicit control 1");
        auto input=fixture_r5(r.plan,fixture);auto context=r63_context(r,functions,input,1,0);auto baseline=r.memory.snapshot();
        r5::need(r62_finite(baseline.at(2)),"Nonfinite original output at control 1");
        report["scheduler_correctness"].push_back({{"fixture",fixtures[fixture]},{"experimental_control",1.0},
            {"original_output_sha256",hash_r5(baseline.at(2))},{"comparisons",J::array()},{"coverage",J::array()}});
        auto& row=report["scheduler_correctness"].back();save_r5(path,report);
        for(U mode=0;mode<3;++mode){
            report["active_scheduler_mode"]=R63_MODES[mode];
            auto got=r63_context(r,functions,input,1,mode);auto diff=r.memory.compare(baseline,weights);
            diff["mode"]=R63_MODES[mode];diff["head_input"]=byte_diff_r5(context.input,got.input);diff["head_output"]=byte_diff_r5(context.output,got.output);
            row["comparisons"].push_back(diff);save_r5(path,report);
            r5::need(r63_compare_state(diff) && diff["head_input"]["mismatches"]==0 && diff["head_output"]["mismatches"]==0,
                "Swin scheduling candidate differs from original full state; no timing or adoption allowed");
            std::cout<<"[r6.3] "<<fixtures[fixture]<<" / "<<R63_MODES[mode]<<": all "<<r.plan.total<<" bytes identical, guards intact"<<std::endl;
        }
        for(int seed:{0xa5,0x5a}){
            auto output=r63_perturb(r,functions,input,1,0,0,seed);auto diff=byte_diff_r5(baseline.at(2),output.output);
            row["coverage"].push_back({{"seed",seed},{"difference",diff}});save_r5(path,report);
            r5::need(diff.at("mismatches")==0,"Original nonzero-control output depends on sentinel seed");
        }
        if(fixture==2)timedState=std::move(baseline);
    }
    delivery_stage("r6.3 confirming both Head interventions reach the output under each scheduling policy");
    report["active_model_gate"]={{"control",1.0},{"game_control_value_verified",false},{"patterns",J::array()},{"passed",false}};
    for(U mutation:{1u,2u}){
        auto first=r63_perturb(r,functions,varying,1,0,mutation);
        auto repeat=r63_perturb(r,functions,varying,1,0,mutation);
        auto sensitivity=byte_diff_r5(timedState.at(2),first.output);
        J row={{"pattern",mutation},{"mutated_head_bytes",first.mutationBytes},{"output_difference",sensitivity},
            {"original_repeat_equal",first.output==repeat.output && first.tailSha==repeat.tailSha && first.headSha==repeat.headSha},
            {"original_changed_output_finite",r62_finite(first.output)},{"candidates",J::array()}};
        report["active_model_gate"]["patterns"].push_back(row);save_r5(path,report);
        r5::need(row["original_repeat_equal"].get<bool>() && first.mutationBytes>0 && r62_finite(first.output) && sensitivity.at("mismatches").get<size_t>()>0,
            "Active neural-output sensitivity failed; scheduling benchmark blocked");
        for(U mode:{1u,2u})for(U repetition:{0u,1u}){
            auto changed=r63_perturb(r,functions,varying,1,mode,mutation);
            auto diff=byte_diff_r5(first.output,changed.output);
            report["active_model_gate"]["patterns"].back()["candidates"].push_back({{"mode",R63_MODES[mode]},{"repeat",repetition},
                {"output_difference_from_original_intervention",diff},{"head_equal",changed.headSha==first.headSha},{"prefinal_equal",changed.tailSha==first.tailSha}});
            save_r5(path,report);
            r5::need(diff.at("mismatches")==0 && changed.headSha==first.headSha && changed.tailSha==first.tailSha,
                "Scheduling candidate failed repeated perturbed-neural-output comparison");
        }
    }
    report["active_model_gate"]["passed"]=true;
    report["scheduling_gate"]={{"approved",true},{"scope","Synthetic fixed control=1 benchmark only; NOT game deployment"}};save_r5(path,report);
    delivery_stage("r6.3 capturing complete graphs and validating the nonzero control was captured");
    std::vector<std::unique_ptr<Graph>> graphs;
    for(U mode=0;mode<3;++mode){
        auto g=std::make_unique<Graph>(r.h);r.memory.reset(varying);
        {
            R63Functions selected(r,functions,mode);auto slots=r63_slots(r);r62::ScopedControl control(r.calls.back().storage.at(0),slots,1);
            r.h.check(r.h.begin_capture(r.stream,0),"r6.3 begin capture");
            try{r.run(0);}catch(...){void* discarded=nullptr;r.h.end_capture(r.stream,&discarded);if(discarded)r.h.graph_destroy(discarded);throw;}
            r.h.check(r.h.end_capture(r.stream,&g->g),"r6.3 end capture");
            r.h.check(r.h.instantiate(&g->exec,g->g,0),"r6.3 instantiate scheduler graph");
        }
        r5::need(r.calls.back().storage.at(0)==initialArguments,"Live arguments were not restored after graph capture");
        // Host argument storage is zero again; this comparison checks that the
        // graph owns the intended captured nonzero argument and kernel handles.
        r.memory.reset(varying);r.h.check(r.h.graph_launch(g->exec,r.stream),"r6.3 graph validation replay");
        auto diff=r.memory.compare(timedState,weights);diff["mode"]=R63_MODES[mode];report["graph_validation"].push_back(diff);save_r5(path,report);
        r5::need(r63_compare_state(diff),"Captured graph does not reproduce active-control reference state");graphs.push_back(std::move(g));
    }
    auto order=r63::orders();report["scheduler_timing_design"]={{"training_rounds",r63::training_rounds},{"holdout_rounds",r63::holdout_rounds},
        {"balanced_blocks_of_rounds",6},{"control_value",1},{"order",order},{"original_head_all_modes",true},
        {"clock_locked",false},{"power_measured",false},{"hardware_policy_read_back",false},
        {"scope","Kernel descriptors differ as recorded; direct readback of the hardware scheduler register is not performed."}};
    delivery_stage("r6.3 warming all complete graphs with identical reset state");
    for(U round=0;round<6;++round)for(U mode:order[round]){r.memory.reset(varying);r.h.check(r.h.graph_launch(graphs[mode]->exec,r.stream),"r6.3 warmup");r.h.check(r.h.stream_sync(r.stream),"r6.3 warmup wait");}
    Resources events(r.h);r.h.check(r.h.event_create(&events.start),"r6.3 create timing event");r.h.check(r.h.event_create(&events.stop),"r6.3 create timing event");
    std::array<std::vector<float>,3> train,hold;
    report["scheduler_raw_timing"]=J::array();report["timing"]=J::array();
    delivery_stage("r6.3 measuring 66 balanced rounds at one fixed active-model control");
    for(U round=0;round<r63::total_rounds;++round){
        for(U mode:order[round]){
            report["active_scheduler_mode"]=R63_MODES[mode];delivery_last_operation="complete_scheduler_graph";delivery_last_command=SIZE_MAX;
            r.memory.reset(varying);
            r.h.check(r.h.event_record(events.start,r.stream),"r6.3 timing begin");
            r.h.check(r.h.graph_launch(graphs[mode]->exec,r.stream),"r6.3 timed full graph");
            r.h.check(r.h.event_record(events.stop,r.stream),"r6.3 timing end");r.h.check(r.h.event_sync(events.stop),"r6.3 timed graph wait");
            float ms=std::numeric_limits<float>::quiet_NaN();int rc=r.h.event_ms(&ms,events.start,events.stop);auto kind=r61::classify(ms);
            J sample={{"round",round},{"mode",R63_MODES[mode]},{"hip_return_code",rc},{"raw_f32_bits",std::bit_cast<std::uint32_t>(ms)},
                {"raw_ms",std::isfinite(ms)?J(ms):J(nullptr)},{"classification",rc?"hip_error":r61::name(kind)},
                {"partition",round<r63::training_rounds?"training":"holdout"}};
            report["scheduler_raw_timing"].push_back(sample);
            if(rc || !std::isfinite(ms) || ms<=0){report["scheduler_timestamp_failure"]=sample;save_r5(path,report);
                if(rc)r.h.check(rc,"r6.3 timing API");throw std::runtime_error("Invalid or unresolved scheduler timestamp; raw bits retained, no sample censored or replaced");}
            (round<r63::training_rounds?train[mode]:hold[mode]).push_back(ms);
        }
        report["timing_progress"]={{"completed_rounds",round+1},{"required_rounds",r63::total_rounds},{"complete",round+1==r63::total_rounds}};
        save_r5(path,report);std::cout<<"[r6.3] Timing "<<round+1<<"/66 saved"<<std::endl;
    }
    for(U mode=0;mode<3;++mode){
        r.memory.reset(varying);r.h.check(r.h.graph_launch(graphs[mode]->exec,r.stream),"r6.3 post-timing validation");
        auto diff=r.memory.compare(timedState,weights);diff["mode"]=R63_MODES[mode];report["post_timing_validation"].push_back(diff);save_r5(path,report);
        r5::need(r63_compare_state(diff),"Scheduling graph failed post-timing full-state validation");
        report["timing"].push_back({{"mode",R63_MODES[mode]},{"experimental_control",1.0},{"training",distribution_r5(train[mode])},{"holdout",distribution_r5(hold[mode])},
            {"holdout_saved_ms",median_r5(hold[0])-median_r5(hold[mode])},{"holdout_speed_ratio_vs_original",median_r5(hold[0])/median_r5(hold[mode])}});
        std::cout<<"[r6.3] "<<R63_MODES[mode]<<" holdout="<<median_r5(hold[mode])<<" ms"<<std::endl;
    }
    report["scheduler_holdout_screen"]=r6_screen_report(report["timing"],true,true);
    auto trainingRows=report["timing"];for(auto& row:trainingRows)row["holdout"]=row["training"];
    report["scheduler_training_screen"]=r6_screen_report(trainingRows,true,true);
    report["scheduler_candidate_decisions"]=J::array();
    for(U index=0;index<2;++index){bool a=report["scheduler_training_screen"]["decisions"][index]["accepted_for_further_validation"].get<bool>();
        bool b=report["scheduler_holdout_screen"]["decisions"][index]["accepted_for_further_validation"].get<bool>();
        report["scheduler_candidate_decisions"].push_back({{"mode",R63_MODES[index+1]},{"accepted_for_further_validation",a&&b},
            {"training_pass",a},{"holdout_pass",b},{"game_release_approved",false},
            {"scope","Both partitions must pass the existing >=1% bootstrap screening heuristic. Timing samples may be correlated; not proof of optimality."}});
    }
    graphs.clear();delivery_stage("r6.3 restoring the complete original zero-control state");
    r5::need(r.calls.back().storage.at(0)==initialArguments,"Original control bytes not restored");
    r.memory.reset(varying);r.run(0);auto restored=r.memory.compare(zeroState,weights);
    report["original_state_restoration"]=restored;save_r5(path,report);
    r5::need(r63_compare_state(restored),"Original plan restoration differs after scheduling tests");
    report["status"]="r63_swin_scheduling_test_complete_not_game_validation";
    report["scheduler_candidates_full_state_equal"]=true;
    report["original_plan_file_modified"]=false;report["game_deployment_approved"]=false;
    delivery_stage("r6.3 complete: inspect correctness and timing decisions; no game files modified");save_r5(path,report);
}
