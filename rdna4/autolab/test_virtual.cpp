// SPDX-License-Identifier: MIT
#include "lab_policy.hpp"
#include "../forward_r5/plan_core.hpp"
#include "../forward_r5/plan_regression_r52.hpp"
#include <fstream>
#include <iostream>
int main(int argc,char** argv){
    try{
        if(argc!=2)throw std::runtime_error("Expected output JSON path");
        auto numeric=autolab::cpu_lab();size_t head=head_contract::cpu_tests(),boundary=r52::regression_tests(),plan=r5::self_test(),fuzz=0;
        auto fixture=r5::fixture();
        for(unsigned at=0;at<80;++at){
            auto bad=fixture;bad["events"][0]["args"][0]["relocations"][0]["at"]=at;
            bool accepted=true;try{r5::Plan::parse(bad,false);}catch(const std::exception&){accepted=false;}
            autolab::require(accepted==(at==0),"Relocation overlap/bounds property failed");++fuzz;
        }
        for(unsigned off=0;off<256;++off){
            auto bad=fixture;bad["events"][0]["args"][0]["relocations"][0]["offset"]=off;
            bool accepted=true;try{r5::Plan::parse(bad,false);}catch(const std::exception&){accepted=false;}
            autolab::require(accepted==(off<32),"Allocation range property failed");++fuzz;
        }
        std::mt19937 rng(619072);
        for(unsigned i=0;i<4096;++i){
            auto bad=fixture;unsigned index=1+rng()%23;bad["events"][0]["args"][0]["relocations"][0]["at"]=index;
            bool rejected=false;try{r5::Plan::parse(bad,false);}catch(const std::exception&){rejected=true;}
            autolab::require(rejected,"Random overlapping pointer accepted");++fuzz;
        }
        r5::J report={{"status","cpu_virtual_lab_passed"},{"numerical_contract_checks",numeric.numerical_checks},{"acceptance_policy_checks",numeric.policy_checks},
            {"head_layout_and_rounding_checks",head},{"reported_boundary_regression_checks",boundary},{"plan_checks",plan},{"malformed_relocation_checks",fuzz},
            {"virtualization_scope","CPU arithmetic/layout/argument validation, not full GPU ISA emulation"},{"target_gpu_executed",false},{"original_full_checkpoint_executed",false},
            {"new_gpu_performance_measured",false},{"game_release_approved",false}};
        std::ofstream out(argv[1]);if(!out)throw std::runtime_error("Cannot create VM report");out<<report.dump(2)<<'\n';out.close();if(!out)throw std::runtime_error("VM report write failed");
        std::cout<<report.dump(2)<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
