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
inline constexpr std::size_t ARG_BYTES=168,GAIN_AT=136;
// Experimental values only, not a claim about correct game/runtime defaults.
inline constexpr std::array<float,4> GAINS{0.0f,0.03125f,0.125f,1.0f};
inline void need(bool b,const char* msg){if(!b)throw std::runtime_error(msg);}
inline bool allowed(float f){return std::isfinite(f)&&std::any_of(GAINS.begin(),GAINS.end(),[&](float g){return std::bit_cast<std::uint32_t>(g)==std::bit_cast<std::uint32_t>(f);});}
inline void patch(std::span<Byte> bytes,std::span<const Byte> baseline,float gain){
    need(bytes.size()==ARG_BYTES && baseline.size()==ARG_BYTES,"Unsupported final scalar argument size");
    need(allowed(gain),"Gain outside finite, explicitly allowed experimental values");
    for(std::size_t i=0;i<ARG_BYTES;++i)if(i<GAIN_AT || i>=GAIN_AT+4)need(bytes[i]==baseline[i],"Another final argument byte was changed");
    // Do not resize the buffer: kernelParams retains a pointer into this storage.
    std::memcpy(bytes.data()+GAIN_AT,&gain,4);
}
inline bool isolated(std::span<const Byte> bytes,std::span<const Byte> baseline){
    if(bytes.size()!=ARG_BYTES || baseline.size()!=ARG_BYTES)return false;
    for(std::size_t i=0;i<ARG_BYTES;++i)if((i<GAIN_AT || i>=GAIN_AT+4)&&bytes[i]!=baseline[i])return false;
    return true;
}
inline float original_epilogue_hypothesis(float rgb,float network,float gain){
    // Specific static ISA path: optional-history pointer null, control uint144=1.
    // This is a falsifiable reference, not a general DLSSNR transfer function.
    float encoded=std::fma(0.125f,rgb,-0.0625f);
    float residual=std::fma(gain,network,encoded);
    return std::clamp(std::fma(8.0f,residual,0.5f),0.0f,1.0f);
}
inline std::vector<Byte> rgb_reference(std::span<const Byte> rgb){
    need(rgb.size()%12==0,"Expected bounded RGB float32 test input");
    std::vector<Byte> out((rgb.size()/12)*16);
    for(std::size_t p=0;p<rgb.size()/12;++p){
        for(std::size_t c=0;c<3;++c){float x;std::memcpy(&x,rgb.data()+p*12+c*4,4);need(std::isfinite(x),"Nonfinite RGB fixture");float y=original_epilogue_hypothesis(x,0.0f,0.0f);std::memcpy(out.data()+p*16+c*4,&y,4);}
        float a=1.0f;std::memcpy(out.data()+p*16+12,&a,4);
    }
    return out;
}
inline std::size_t self_test(){
    std::size_t checks=0;std::array<Byte,ARG_BYTES> base{};
    for(std::size_t i=0;i<base.size();++i)base[i]=Byte(i*37u+11u);
    std::fill_n(base.begin()+GAIN_AT,4,0);
    for(float g:GAINS){auto bytes=base;auto* address=bytes.data();patch(bytes,base,g);need(address==bytes.data()&&isolated(bytes,base),"Patch allocation/byte isolation");float actual;std::memcpy(&actual,bytes.data()+GAIN_AT,4);need(actual==g,"Gain patch mismatch");std::copy(base.begin(),base.end(),bytes.begin());need(bytes==base,"Restoration failed");checks+=3;}
    for(std::size_t i=0;i<base.size();++i)if(i<GAIN_AT || i>=GAIN_AT+4){auto bytes=base;bytes[i]^=1;bool rejected=false;try{patch(bytes,base,0.125f);}catch(const std::exception&){rejected=true;}need(rejected,"Unrelated byte change accepted");++checks;}
    for(float bad:{-0.0f,-1.0f,0.1f,2.0f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}){bool rejected=false;auto b=base;try{patch(b,base,bad);}catch(const std::exception&){rejected=true;}need(rejected,"Invalid experimental gain accepted");++checks;}
    bool rejected=false;try{std::vector<Byte> b(167);patch(b,base,0.125f);}catch(const std::exception&){rejected=true;}need(rejected,"Wrong argument length accepted");++checks;
    for(unsigned i=0;i<=256;++i){float rgb=float(i)/256.0f;need(original_epilogue_hypothesis(rgb,-2.0f,0.0f)==rgb,"Zero gain negative-control regression");need(original_epilogue_hypothesis(rgb,2.0f,0.0f)==rgb,"Zero gain positive-control regression");checks+=2;}
    need(original_epilogue_hypothesis(0.5f,0.5f,0.03125f)==0.625f,"Nonzero gain must preserve a testable network contribution");++checks;
    std::vector<Byte> rgb(120);auto rgba=rgb_reference(rgb);need(rgba.size()==160,"RGB/RGBA bounds");
    for(std::size_t p=0;p<10;++p)for(std::size_t c=0;c<4;++c){float f;std::memcpy(&f,rgba.data()+p*16+c*4,4);need(f==(c==3?1.0f:0.0f),"RGB output reference mismatch");++checks;}
    return checks;
}
}
