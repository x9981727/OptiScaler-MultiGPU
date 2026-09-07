// SPDX-License-Identifier: MIT
#pragma once
// This file is included AFTER plan_core.hpp. It contains three reported commands,
// not a reconstructed full Forward, model, benchmark, or GPU validation result.
namespace r52 {
inline r5::J boundary_fixture() {
    using r5::J;
    const std::size_t sizes[]={147686204,26542080,35389440,26542080,35389440,70778880,
        17694720,17694720,8847360,2211840,2211840,2211840,8847360,4423680,4423680,
        4423680,4423680,4423680,2211840,8847360,8847360,8847360,2211840,1105920,
        17694720,17694720,17694720,1105920,1105920,1105920,1105920,327680,655360,
        589824,2359296,589824,589824,589824,589824,589824,589824,1105920,1105920,283115520};
    J allocations=J::array();
    for(std::size_t i=0;i<44;++i)allocations.push_back({{"id",i},{"size",sizes[i]}});
    auto relocation=[](unsigned at,unsigned allocation,std::size_t offset){
        return J{{"at",at},{"allocation",allocation},{"offset",offset}};
    };
    // Verbatim normalized argument bytes in the uploaded r5.1 failure report.
    std::string first="00000000000000000000000000000000000000000000000080070000800400000000000000000000140000000000000000000000000000000000000000000000000000000000000000000000000000000000003d000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000";
    std::string last="000000000000000000000000000000000000000000000000800700008004000000000000000000002000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000010000000000000000000000000000000000000000000000";
    auto launch=[](std::string symbol,std::string bytes,J refs,J grid){
        return J{{"op","launch"},{"kernel",symbol},{"grid",grid},{"block",{256,1,1}},{"shared",0},
            {"args",J::array({{{"bytes",bytes},{"relocations",refs}}})}};
    };
    J events=J::array();
    events.push_back(launch("_Z10k_swin_varILi32ELb1EEv9VarParams",first,
        J::array({relocation(8,5,0),relocation(16,0,12),relocation(56,6,0),
                  relocation(64,1,0),relocation(160,43,0)}),J::array({144,240,1})));
    events.push_back(launch(r5::HEAD,std::string(48,'0'),
        J::array({relocation(0,31,0),relocation(8,32,0),relocation(16,0,22493628)}),J::array({40,1,1})));
    events.push_back(launch("_Z10k_swin_varILi32ELb1EEv9VarParams",last,
        J::array({relocation(0,5,0),relocation(16,0,147397244),relocation(112,2,0),
                  relocation(120,1,0),relocation(152,26,0),relocation(160,43,0)}),J::array({144,240,1})));
    return {{"fixture_scope","Three original reported calls only; not the complete 154-launch trace"},
            {"source_report_error","Required original pointer relocation absent"},
            {"source_original_plan_sha256",r5::PLAN_SHA},{"allocations",allocations},{"events",events}};
}
inline std::size_t regression_tests() {
    using r5::J;using r5::need;
    auto f=boundary_fixture();const auto before=f.dump();
    auto p=r5::Plan::parse(f,false);std::size_t checks=0;
    need(p.firstInput.allocation==1 && p.firstInput.offset==0,"First special Swin input must use slot 64/allocation 1");++checks;
    need(p.lastObserved.allocation==2 && p.lastObserved.offset==0,"Last special Swin observation must use slot 112/allocation 2");++checks;
    need(p.total==806544188 && p.allocations.size()==44,"Reported allocation contract changed");++checks;
    need(p.commands.size()==3 && p.heads==std::vector<std::size_t>{1},"Fixture must remain three commands only");++checks;
    auto& head=p.commands[1];need(head.grid==std::array<r5::U,3>{40,1,1},"Original Head workload is 40 groups");++checks;
    need(r5::Plan::pointer(head,16).offset==22493628,"Reported original Head binding changed");++checks;
    need(f.dump()==before,"Parser modified source argument bytes");++checks;
    need(p.commands[0].args[0].bytes==r5::hex(f["events"][0]["args"][0]["bytes"].get<std::string>()),"Parser changed first argument");++checks;
    need(p.commands[2].args[0].bytes==r5::hex(f["events"][2]["args"][0]["bytes"].get<std::string>()),"Parser changed final argument");++checks;
    auto rejects=[&](J bad,bool fixed=false){bool rejected=false;try{r5::Plan::parse(bad,fixed);}catch(const std::exception&){rejected=true;}need(rejected,"Malformed boundary trace was accepted");++checks;};
    // The original raw-plan hash gate remains outside Plan::parse. This small
    // regression fixture must NOT pass the complete 154-launch count contract.
    rejects(f,true);
    J bad=f;bad["events"][0]["args"][0]["relocations"].erase(3);rejects(bad);
    bad=f;bad["events"][2]["args"][0]["relocations"].erase(2);rejects(bad);
    bad=f;bad["events"][0]["args"][0]["relocations"][3]["allocation"]=0;rejects(bad);
    bad=f;bad["events"][2]["args"][0]["relocations"][2]["offset"]=4;rejects(bad);
    bad=f;bad["events"][0]["args"][0]["relocations"].push_back({{"at",0},{"allocation",1},{"offset",0}});rejects(bad);
    bad=f;bad["events"][2]["args"][0]["relocations"].push_back({{"at",8},{"allocation",2},{"offset",0}});rejects(bad);
    bad=f;bad["events"][0]["grid"]={143,240,1};rejects(bad);
    bad=f;bad["events"][2]["block"]={128,1,1};rejects(bad);
    bad=f;bad["allocations"][1]["size"]=4;rejects(bad);
    bad=f;bad["allocations"][2]["size"]=4;rejects(bad);
    bad=f;bad["events"][0]["args"][0]["bytes"]=std::string(336,'0');rejects(bad);
    bad=f;bad["events"][2]["args"][0]["relocations"][0]["allocation"]=6;rejects(bad);
    bad=f;bad["events"][1]["args"][0]["relocations"][1]["allocation"]=31;rejects(bad);
    bad=f;bad["events"][0]["kernel"]="_Z10k_swin_varILi32ELb0EEv9VarParams";rejects(bad);
    return checks;
}
} // namespace r52
