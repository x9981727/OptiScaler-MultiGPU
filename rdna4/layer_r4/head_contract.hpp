// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>
namespace head_contract {
using u32=std::uint32_t;using u16=std::uint16_t;using u8=std::uint8_t;
constexpr u32 K=512,N=1024,M=16;
constexpr std::size_t INPUT_BYTES=M*K,OUTPUT_BYTES=M*N,WEIGHT_BYTES=K*N;
inline void require(bool b,const char* s){if(!b)throw std::runtime_error(s);}
// Logical row/token, column/channel -> original byte layout, for one 16-token block.
constexpr u32 tensor(u32 r,u32 c) {
    return (c/32)*512+(r%8)*64+((c%16)/4)*16+((c%32)/16)*8+(r/8)*4+c%4;
}
// Independently simplified integer address sequence in the original head load loop.
constexpr u32 isa_input(u32 i) {
    const u32 lane=i&31u;
    const int b=int((i<<4)&0x1e00u)+int(((i>>3)&0x3c0u)|(i&3u))+(i>4095?-508:0);
    return u32(b)|(lane>15?8u:0u)|((lane%16u)/4u)*16u;
}
// Weight storage: [K/32][N/16][512-byte fragment block]. No generic GEMM layout assumption.
constexpr u32 weight(u32 k,u32 n) {
    u32 col=n%16,lane=col+16*((k%16)/8),j=k%8;
    u32 b=(k/32)*32768+(n/16)*512+(((lane>>1)&6u)|(lane&1u))*64+(lane/16)*32+(lane&2u)*4;
    return b+(k%32>=16?4u:0u)+(j<4?j:16+j-4);
}
inline float decode8(u8 b) {
    unsigned e=(b>>3)&15,m=b&7;
    if(e==15 && m==7)return std::numeric_limits<float>::quiet_NaN();
    float v=e?std::ldexp(1.0f+float(m)/8,int(e)-7):std::ldexp(float(m),-9);
    return (b&128)?-v:v;
}
inline float decode16(u16 h) {
    u32 sign=u32(h&0x8000)<<16,e=(h>>10)&31,m=h&1023;
    if(e==31)return std::bit_cast<float>(sign|0x7f800000u|(m<<13));
    if(!e){if(!m)return std::bit_cast<float>(sign);float f=std::ldexp(float(m),-24);return sign?-f:f;}
    return std::bit_cast<float>(sign|((e+112)<<23)|(m<<13));
}
inline u16 encode16(float v) {
    u32 b=std::bit_cast<u32>(v),s=(b>>16)&0x8000,e=(b>>23)&255,m=b&0x7fffff;
    if(e==255)return u16(s|0x7c00|(m?0x200:0));
    int he=int(e)-112;
    if(he>=31)return u16(s|0x7c00);
    if(he<=0){
        if(he < -10)return u16(s);
        m|=0x800000;u32 shift=u32(14-he),q=m>>shift,rem=m&((u32(1)<<shift)-1),half=u32(1)<<(shift-1);
        if(rem>half || (rem==half && (q&1)))++q;
        return u16(s|q);
    }
    u32 q=m>>13,rem=m&8191;
    if(rem>4096 || (rem==4096 && (q&1)))++q;
    if(q==1024){q=0;if(++he==31)return u16(s|0x7c00);}
    return u16(s|(u32(he)<<10)|q);
}
inline float round16(float v){return decode16(encode16(v));}
inline u8 encode8(float value) {
    value=value+0.0f;
    if(std::isnan(value))return 0x7f;
    bool neg=std::signbit(value);float a=std::min(std::abs(value),448.0f);
    unsigned best=0;float distance=std::numeric_limits<float>::infinity();
    for(unsigned b=0;b<127;++b){float d=std::abs(a-decode8(u8(b)));if(d<distance || (d==distance && !(b&1u))){distance=d;best=b;}}
    return u8(best|(neg?128:0));
}
inline std::size_t cpu_tests() {
    std::size_t checks=0;
    for(unsigned c:{512u,1024u}){
        std::vector<unsigned> visited(std::size_t(16)*c,0);
        for(unsigned r=0;r<16;++r)for(unsigned n=0;n<c;++n){u32 p=tensor(r,n);require(p<visited.size(),"Tensor address outside block");require(++visited[p]==1,"Tensor alias");++checks;
            if(c==512){require(p==isa_input(r*512+n),"Tensor formula differs from original input address sequence");++checks;}}
    }
    std::vector<unsigned> visited(WEIGHT_BYTES,0);
    for(unsigned k=0;k<K;++k)for(unsigned n=0;n<N;++n){auto p=weight(k,n);require(p<visited.size(),"Weight address range");require(++visited[p]==1,"Weight mapping alias");++checks;}
    // Separately walk the actual packed fragment loads used by the candidate.
    for(unsigned tile=0;tile<64;++tile)for(unsigned lane=0;lane<32;++lane)for(unsigned kb=0;kb<512;kb+=32){
        u32 b=(kb/32)*32768+tile*512+(((lane>>1)&6u)|(lane&1u))*64+(lane>>4)*32+(lane&2u)*4;
        for(unsigned part=0;part<2;++part)for(unsigned j=0;j<8;++j){u32 addr=b+(part?4:0)+(j<4?j:16+j-4);
            require(addr==weight(kb+part*16+(lane>>4)*8+j,tile*16+(lane&15)),"Fragment/weight reference mismatch");++checks;}
    }
    for(unsigned h=0;h<65536;++h){if(((h>>10)&31)==31)continue;require(encode16(decode16(u16(h)))==h,"FP16 roundtrip failed");++checks;}
    require(encode16(1.0f+1.0f/2048)==0x3c00,"Half even tie");
    require(encode16(1.0f+3.0f/2048)==0x3c02,"Half odd tie");
    require(encode16(std::ldexp(1.0f,-25))==0,"Half subnormal tie");
    require(encode16(65520.0f)==0x7c00,"Half overflow threshold");
    require(round16(64.0f+0.03125f)==64.0f,"Required chunk-rounding regression");
    for(unsigned b=0;b<256;++b){auto f=decode8(u8(b));u8 expect=(b&127)==127?0x7f:((b&127)==0?0:u8(b));
        require(encode8(round16(f))==expect,"FP8/FP16 contract roundtrip");++checks;}
    return checks+5;
}
}
