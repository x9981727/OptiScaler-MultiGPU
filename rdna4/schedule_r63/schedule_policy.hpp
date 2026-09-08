// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
namespace r63 {
using Byte=unsigned char;
constexpr std::uint32_t rr_mask=1u<<21;
constexpr unsigned training_rounds=36,holdout_rounds=30,total_rounds=66;
struct Patch{const char* symbol;std::size_t offset;std::uint32_t original,replacement;};
inline void require(bool value,const char* error){if(!value)throw std::runtime_error(error);}
inline std::uint32_t read32(const std::vector<Byte>& bytes,std::size_t at){
    require(at<=bytes.size() && bytes.size()-at>=4,"Scheduling descriptor out of bounds");
    return std::uint32_t(bytes[at])|(std::uint32_t(bytes[at+1])<<8)|(std::uint32_t(bytes[at+2])<<16)|(std::uint32_t(bytes[at+3])<<24);
}
inline void write32(std::vector<Byte>& bytes,std::size_t at,std::uint32_t value){
    (void)read32(bytes,at);for(unsigned i=0;i<4;++i)bytes[at+i]=Byte(value>>(8*i));
}
inline std::vector<Byte> patch(const std::vector<Byte>& original,const std::vector<Patch>& specs){
    require(!specs.empty(),"No scheduling descriptor edits");
    // Validate ALL descriptors before constructing a modified module.
    std::vector<std::size_t> offsets;
    for(const auto& p:specs){
        require(p.symbol && std::string(p.symbol).find("_Z10k_swin_var")==0,"Non-Swin patch rejected");
        require((p.offset&3)==0,"Misaligned scheduling descriptor");
        require((p.original^p.replacement)==rr_mask,"Patch changes more than the GFX12 scheduling bit");
        require(read32(original,p.offset)==p.original,"Original scheduling descriptor differs");
        offsets.push_back(p.offset);
    }
    std::sort(offsets.begin(),offsets.end());
    for(std::size_t i=1;i<offsets.size();++i)require(offsets[i]>=offsets[i-1]+4,"Overlapping descriptor edits");
    auto result=original;for(const auto& p:specs)write32(result,p.offset,p.replacement);
    std::size_t changed=0;
    for(std::size_t i=0;i<original.size();++i)if(original[i]!=result[i]){
        ++changed;bool allowed=false;for(const auto& p:specs)if(i==p.offset+2 && (original[i]^result[i])==0x20)allowed=true;
        require(allowed,"Unexpected byte changed outside scheduling descriptors");
    }
    require(changed==specs.size(),"Unexpected scheduling patch byte count");return result;
}
inline bool is_swin(const std::string& symbol){
    static const std::array<std::string,5> allowed{
        "_Z10k_swin_varILi32ELb1EEv9VarParams","_Z10k_swin_varILi32ELb0EEv9VarParams",
        "_Z10k_swin_varILi64ELb0EEv9VarParams","_Z10k_swin_varILi128ELb0EEv9VarParams",
        "_Z10k_swin_varILi256ELb0EEv9VarParams"};
    return std::find(allowed.begin(),allowed.end(),symbol)!=allowed.end();
}
inline bool selected(const std::string& symbol,unsigned mode){
    require(mode<3,"Unknown scheduling mode");
    return mode==1?symbol=="_Z10k_swin_varILi32ELb1EEv9VarParams":mode==2 && is_swin(symbol);
}
inline std::vector<std::array<unsigned,3>> orders(){
    std::array<std::array<unsigned,3>,6> permutations{{{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}}};
    std::vector<std::array<unsigned,3>> result;std::mt19937 rng(630908);
    for(unsigned block=0;block<total_rounds/6;++block){std::shuffle(permutations.begin(),permutations.end(),rng);for(auto p:permutations)result.push_back(p);}
    return result;
}
inline std::size_t self_test(){
    std::size_t count=0;auto test=[&](bool ok){require(ok,"r6.3 CPU scheduling regression failed");++count;};
    const char* s="_Z10k_swin_varILi32ELb1EEv9VarParams";
    std::vector<Byte> data(128,0);write32(data,48,0x600c0047);auto before=data;
    std::vector<Patch> edit{{s,48,0x600c0047,0x602c0047}};
    auto out=patch(data,edit);test(data==before);test(read32(out,48)==0x602c0047);
    for(std::size_t i=0;i<data.size();++i)test(i==50?((data[i]^out[i])==0x20):data[i]==out[i]);
    auto back=patch(out,{{s,48,0x602c0047,0x600c0047}});test(back==before);
    auto reject=[&](std::vector<Patch> p){bool bad=false;try{patch(data,p);}catch(const std::runtime_error&){bad=true;}test(bad);test(data==before);};
    reject({});reject({{s,128,0,rr_mask}});reject({{s,SIZE_MAX,0,rr_mask}});
    reject({{s,49,0,rr_mask}});reject({{s,48,0,rr_mask}});
    reject({{s,48,0x600c0047,0x600c1047}});reject({{s,48,0x600c0047,0x602c1047}});
    reject({edit[0],edit[0]});reject({{"other_kernel",48,0x600c0047,0x602c0047}});
    auto order=orders();test(order==orders());test(order.size()==total_rounds);
    for(std::size_t start=0;start<order.size();start+=6){
        unsigned hits[3][3]{};
        for(std::size_t j=start;j<start+6;++j){auto row=order[j];auto sorted=row;std::sort(sorted.begin(),sorted.end());test(sorted==std::array<unsigned,3>{0,1,2});for(unsigned k=0;k<3;++k)++hits[row[k]][k];}
        for(auto& row:hits)for(auto n:row)test(n==2);
    }
    test(training_rounds%6==0 && holdout_rounds%6==0);
    test(!selected(s,0));test(selected(s,1));test(selected(s,2));test(!selected("device_memcpy",2));
    test(!selected("_Z10k_swin_varILi64ELb0EEv9VarParams",1));test(selected("_Z10k_swin_varILi64ELb0EEv9VarParams",2));
    return count;
}
}
