// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>
namespace r62 {
using Byte=unsigned char;
inline constexpr size_t argument_bytes=168, control_offset=136;
inline void require(bool b,const char* message){if(!b)throw std::runtime_error(message);}
inline std::uint32_t bits_at(std::span<const Byte> s,size_t at){
    require(at<=s.size() && 4<=s.size()-at,"Control word outside argument");
    return std::uint32_t(s[at]) | std::uint32_t(s[at+1])<<8 | std::uint32_t(s[at+2])<<16 | std::uint32_t(s[at+3])<<24;
}
inline void write_word(std::span<Byte> s,size_t at,std::uint32_t bits){
    require(at<=s.size() && 4<=s.size()-at,"Control write outside argument");
    for(size_t j=0;j<4;++j)s[at+j]=Byte(bits>>(8*j));
}
inline bool permitted_gain(float g){return std::isfinite(g) && (g==0.0f || g==0.125f || g==1.0f);}
// The owner must synchronize outstanding GPU work BEFORE constructing this
// guard and BEFORE normal restoration. This changes four HOST argument bytes
// only; no device pointers, tensor layout or grid/block are edited.
class ScopedControl final {
    std::span<Byte> target_;
    std::array<Byte,argument_bytes> before_{};
public:
    ScopedControl(std::span<Byte> bytes,std::span<const size_t> pointer_slots,float value):target_(bytes){
        require(bytes.size()==argument_bytes,"Unsupported explicit argument length");
        require(permitted_gain(value),"Gain outside the bounded diagnostic set");
        for(size_t at:pointer_slots){
            require(at<=bytes.size() && 8<=bytes.size()-at,"Pointer slot outside argument");
            require(!(at<control_offset+4 && control_offset<at+8),"Diagnostic control overlaps a pointer");
        }
        std::copy(bytes.begin(),bytes.end(),before_.begin());
        write_word(target_,control_offset,std::bit_cast<std::uint32_t>(value));
    }
    ~ScopedControl(){std::copy(before_.begin(),before_.end(),target_.begin());}
    ScopedControl(const ScopedControl&)=delete;
    ScopedControl& operator=(const ScopedControl&)=delete;
    bool only_control_changed()const{
        for(size_t j=0;j<before_.size();++j)if((j<control_offset || j>=control_offset+4) && before_[j]!=target_[j])return false;
        return true;
    }
};
inline std::vector<Byte> mutate_head(std::span<const Byte> input,unsigned kind){
    require(kind==1 || kind==2,"Unknown Head perturbation");
    std::vector<Byte> p(input.begin(),input.end());
    for(size_t i=0;i<p.size();++i){
        require((p[i]&127)!=127,"Original Head fixture contains FP8 NaN");
        p[i]=kind==1?Byte((i&1)?0xb0:0x30):Byte(p[i]^128);
    }
    return p;
}
inline size_t self_test(){
    size_t checks=0;
    auto test=[&](bool b){require(b,"r6.2 control policy regression failed");++checks;};
    std::vector<Byte> v(argument_bytes);
    for(size_t i=0;i<v.size();++i)v[i]=Byte((i*37+11)%256);
    write_word(v,control_offset,0);
    const auto original=v;auto* address=v.data();
    const std::array<size_t,6> pointers{0,16,112,120,152,160};
    for(float gain:{0.0f,0.125f,1.0f}){
        {ScopedControl g(v,pointers,gain);test(g.only_control_changed());test(bits_at(v,control_offset)==std::bit_cast<std::uint32_t>(gain));test(v.data()==address);
         for(size_t i=0;i<v.size();++i)if(i<control_offset || i>=control_offset+4)test(v[i]==original[i]);}
        test(v==original);test(v.data()==address);
    }
    try{ScopedControl g(v,pointers,1.0f);throw 17;}catch(int){test(v==original);}
    auto rejected=[&](auto operation){bool failed=false;try{operation();}catch(const std::runtime_error&){failed=true;}test(failed);test(v==original);};
    for(float g:{-1.0f,0.5f,2.0f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        rejected([&](){ScopedControl guard(v,pointers,g);});
    for(size_t at:{129u,132u,136u,139u,161u,168u,SIZE_MAX}){
        std::array<size_t,1> bad{at};rejected([&](){ScopedControl guard(v,bad,1.0f);});
    }
    std::vector<Byte> shortArg(167);rejected([&](){ScopedControl guard(shortArg,pointers,1.0f);});
    std::vector<Byte> finite;
    for(unsigned i=0;i<256;++i)if((i&127)!=127)finite.push_back(Byte(i));
    for(unsigned kind:{1u,2u}){auto p=mutate_head(finite,kind);test(p.size()==finite.size());
        for(size_t i=0;i<p.size();++i){test((p[i]&127)!=127);if(kind==2)test(p[i]==Byte(finite[i]^128));}}
    bool bad=false;try{std::vector<Byte> nan{127};mutate_head(nan,2);}catch(const std::runtime_error&){bad=true;}test(bad);
    bad=false;try{mutate_head(finite,3);}catch(const std::runtime_error&){bad=true;}test(bad);
    return checks;
}
} // namespace r62
