// SPDX-License-Identifier: MIT
// Independent candidates for the SHA-pinned _Z12k_final_head10HeadParams contract.
// Fixed shape per spatial group: 16 tokens x 512 input -> 16 x 1024 output.
// This is NOT a full Swin implementation or a game DLL.
// Preserve two FP8 WMMA steps, then FP16 rounding, for EACH 32-channel chunk.
// Input/output use the original 16-token/32-channel byte-swizzled layout.
// Weight fragment addressing is derived from the pinned original GPU ISA.
#pragma OPENCL EXTENSION cl_khr_fp16 : enable
typedef float f8 __attribute__((ext_vector_type(8)));
typedef half h8 __attribute__((ext_vector_type(8)));
typedef int i2 __attribute__((ext_vector_type(2)));

typedef struct {
    __global const uchar* input;
    __global uchar* output;
    __global const uchar* weights;
} HeadArgs;
_Static_assert(sizeof(HeadArgs)==24,"Original explicit argument size");

static inline __attribute__((always_inline)) uint read4(__global const uchar* p) {
    return *(__global const uint*)p;
}
static inline __attribute__((always_inline)) uchar quantize(half h) {
    float v=(float)h+0.0f; // Match original signed-zero normalization.
    if(v!=v) return (uchar)0x7f;
    v=v < -448.0f ? -448.0f : (v > 448.0f ? 448.0f : v);
    return (uchar)__builtin_amdgcn_cvt_pk_fp8_f32(v,0.0f,0,0);
}
static inline __attribute__((always_inline)) uint output_base(uint lane,uint tile) {
    return (tile>>1)*512u + (tile&1u)*8u + (lane&3u) + ((lane&15u)>>2)*16u + (lane>>4)*4u;
}
static inline __attribute__((always_inline)) uint weight_base(uint lane,uint tile) {
    return tile*512u + (((lane>>1)&6u)|(lane&1u))*64u + ((lane>>4)*32u) + ((lane&2u)*4u);
}
static inline __attribute__((always_inline)) void execute_head(HeadArgs p,uint group,uint tile,uint lane) {
    h8 acc=(h8)((half)0);
    __global const uchar* x=p.input+(ulong)group*8192u+(lane&7u)*64u+((lane&15u)>>3)*4u+(lane>>4)*32u;
    __global const uchar* w=p.weights+weight_base(lane,tile);
    #pragma unroll 1
    for(uint k32=0;k32<16;++k32) {
        __global const uchar* a=x+k32*512u;
        __global const uchar* b=w+k32*32768u;
        i2 a0=(i2)((int)read4(a),(int)read4(a+16));
        i2 a1=(i2)((int)read4(a+8),(int)read4(a+24));
        i2 b0=(i2)((int)read4(b),(int)read4(b+16));
        i2 b1=(i2)((int)read4(b+4),(int)read4(b+20));
        f8 chunk=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a0,b0,(f8)(0.0f));
        chunk=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a1,b1,chunk);
        #pragma unroll
        for(uint j=0;j<8;++j) acc[j]=(half)(chunk[j]+(float)acc[j]);
    }
    ulong out=(ulong)group*16384u+output_base(lane,tile);
    #pragma unroll
    for(uint j=0;j<8;++j) p.output[out+j*64u]=quantize(acc[j]);
}
__kernel __attribute__((reqd_work_group_size(32,1,1))) void head_direct16_w1(HeadArgs p) {
    uint lane=__builtin_amdgcn_workitem_id_x();
    execute_head(p,__builtin_amdgcn_workgroup_id_x(),__builtin_amdgcn_workgroup_id_y(),lane);
}
__kernel __attribute__((reqd_work_group_size(128,1,1))) void head_direct16_w4(HeadArgs p) {
    uint tid=__builtin_amdgcn_workitem_id_x();
    execute_head(p,__builtin_amdgcn_workgroup_id_x(),__builtin_amdgcn_workgroup_id_y()*4u+(tid>>5),tid&31u);
}
__kernel __attribute__((reqd_work_group_size(128,1,1))) void head_direct32_w4(HeadArgs p) {
    uint tid=__builtin_amdgcn_workitem_id_x(),lane=tid&31u;
    uint group=__builtin_amdgcn_workgroup_id_x();
    uint tile=__builtin_amdgcn_workgroup_id_y()*8u+(tid>>5)*2u;
    h8 c0=(h8)((half)0),c1=(h8)((half)0);
    __global const uchar* x=p.input+(ulong)group*8192u+(lane&7u)*64u+((lane&15u)>>3)*4u+(lane>>4)*32u;
    __global const uchar* w=p.weights+weight_base(lane,tile);
    #pragma unroll 1
    for(uint k32=0;k32<16;++k32) {
        __global const uchar* a=x+k32*512u;
        __global const uchar* b=w+k32*32768u;
        i2 a0=(i2)((int)read4(a),(int)read4(a+16));
        i2 a1=(i2)((int)read4(a+8),(int)read4(a+24));
        i2 b00=(i2)((int)read4(b),(int)read4(b+16));
        i2 b01=(i2)((int)read4(b+4),(int)read4(b+20));
        i2 b10=(i2)((int)read4(b+512),(int)read4(b+528));
        i2 b11=(i2)((int)read4(b+516),(int)read4(b+532));
        f8 z0=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a0,b00,(f8)(0.0f));
        f8 z1=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a0,b10,(f8)(0.0f));
        z0=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a1,b01,z0);
        z1=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a1,b11,z1);
        #pragma unroll
        for(uint j=0;j<8;++j) { c0[j]=(half)(z0[j]+(float)c0[j]); c1[j]=(half)(z1[j]+(float)c1[j]); }
    }
    ulong o0=(ulong)group*16384u+output_base(lane,tile),o1=(ulong)group*16384u+output_base(lane,tile+1);
    #pragma unroll
    for(uint j=0;j<8;++j) {p.output[o0+j*64u]=quantize(c0[j]);p.output[o1+j*64u]=quantize(c1[j]);}
}
