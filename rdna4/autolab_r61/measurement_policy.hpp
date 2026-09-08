// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>
namespace r61 {
enum class SampleKind { Positive, ZeroUnresolved, Invalid };
inline SampleKind classify(float ms) noexcept {
    if(!std::isfinite(ms) || ms<0) return SampleKind::Invalid;
    return ms==0 ? SampleKind::ZeroUnresolved : SampleKind::Positive;
}
inline const char* name(SampleKind k) noexcept {
    switch(k){case SampleKind::Positive:return "positive";
    case SampleKind::ZeroUnresolved:return "zero_unresolved_not_zero_cost";
    default:return "negative_or_nonfinite_invalid";}
}
inline std::optional<double> strict_median(const std::vector<float>& values){
    if(values.empty())return std::nullopt;
    for(float v:values)if(classify(v)!=SampleKind::Positive)return std::nullopt;
    auto v=values;std::sort(v.begin(),v.end());return double(v[v.size()/2]);
}
inline std::vector<unsigned char> probe_bytes(std::size_t bytes,bool f32,bool zero){
    if(!bytes || (f32 && bytes%4))throw std::runtime_error("Invalid probe byte count");
    std::vector<unsigned char> v(bytes,0);
    if(zero)return v;
    if(f32){
        constexpr float values[]={0.0625f,0.75f,0.125f,1.0f};
        for(std::size_t i=0;i<bytes/4;++i)std::memcpy(v.data()+4*i,&values[i%4],4);
    }else for(std::size_t i=0;i<bytes;++i)v[i]=(i&1)?0xb0:0x30;
    return v;
}
inline std::size_t self_test(){
    std::size_t checks=0;
    auto require=[&](bool b){++checks;if(!b)throw std::runtime_error("r6.1 measurement/probe regression failed");};
    require(classify(0)==SampleKind::ZeroUnresolved);
    require(classify(-0.0f)==SampleKind::ZeroUnresolved);
    require(classify(0.001f)==SampleKind::Positive);
    require(classify(std::numeric_limits<float>::denorm_min())==SampleKind::Positive);
    require(classify(-0.001f)==SampleKind::Invalid);
    require(classify(std::numeric_limits<float>::infinity())==SampleKind::Invalid);
    require(classify(-std::numeric_limits<float>::infinity())==SampleKind::Invalid);
    require(classify(std::numeric_limits<float>::quiet_NaN())==SampleKind::Invalid);
    require(!strict_median({}));require(!strict_median({0,0,0}));
    require(!strict_median({0,0.1f,0.2f}));require(!strict_median({0.1f,-1,0.2f}));
    require(!strict_median({0.1f,std::numeric_limits<float>::quiet_NaN(),0.2f}));
    require(std::abs(*strict_median({0.3f,0.1f,0.2f})-0.2)<1e-6);
    auto f=probe_bytes(64,true,false);for(std::size_t i=0;i<f.size();i+=4){float value;std::memcpy(&value,f.data()+i,4);require(std::isfinite(value));}
    auto p=probe_bytes(37,false,false);for(std::size_t i=0;i<p.size();++i)require(p[i]==((i&1)?0xb0:0x30));
    auto z=probe_bytes(37,false,true);for(auto b:z)require(b==0);
    bool rejected=false;try{probe_bytes(3,true,false);}catch(const std::runtime_error&){rejected=true;}require(rejected);
    rejected=false;try{probe_bytes(0,false,false);}catch(const std::runtime_error&){rejected=true;}require(rejected);
    // A zero observation is retained, but never promoted into a zero-time win.
    require(!strict_median({0,0.2f,0.2f}));
    return checks;
}
}
