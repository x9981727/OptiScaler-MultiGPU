// SPDX-License-Identifier: MIT
// r2: independent linear operators, NOT a Swin/NR runtime replacement.
// Reuse r1's exact math, lane convention and fused epilogue for comparison.
#include "linear.cl"

// Both r1 inputs have address src[k * Outer + outer].
// Packed layout = [outerTile][kTile][lane][8 bytes].
// Outer is padded to a multiple of 32; K to a multiple of 16.
// These are lossless byte permutations, NOT additional quantization.
__kernel __attribute__((reqd_work_group_size(32,1,1)))
void pack_kmajor_fp8(__global const uchar* src, __global i2* dst, uint Outer, uint K) {
    uint lane=__builtin_amdgcn_workitem_id_x();
    uint ot=__builtin_amdgcn_workgroup_id_x(), kt=__builtin_amdgcn_workgroup_id_y();
    uint ktiles=(K+15u)/16u;
    dst[((ulong)ot*ktiles+kt)*32u+lane]=load_x8(src,ot*16u,kt*16u,Outer,K,lane);
}
static inline __attribute__((always_inline)) i2 packed_fragment(
    __global const uchar* src, uint ot, uint kt, uint ktiles, uint lane) {
    return ((__global const i2*)src)[((ulong)ot*ktiles+kt)*32u+lane];
}
SIGNATURE(linear_fp8_packed16,uchar) {
    uint lane=__builtin_amdgcn_workitem_id_x();
    uint mt=__builtin_amdgcn_workgroup_id_x(), nt=__builtin_amdgcn_workgroup_id_y();
    uint ktiles=(K+15u)/16u;
    f8 c=(f8)(0.0f);
    #pragma unroll 1
    for(uint kt=0;kt<ktiles;++kt) {
        i2 a=packed_fragment(x,mt,kt,ktiles,lane), b=packed_fragment(w,nt,kt,ktiles,lane);
        c=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,c);
    }
    store_tile(c,y,bias,residual,mt*16u,nt*16u,M,N,lane,scale,flags);
}
SIGNATURE(linear_fp8_packed32,uchar) {
    uint lane=__builtin_amdgcn_workitem_id_x();
    uint mt=2u*__builtin_amdgcn_workgroup_id_x(), nt=2u*__builtin_amdgcn_workgroup_id_y();
    uint ktiles=(K+15u)/16u;
    f8 c00=(f8)(0.0f),c01=(f8)(0.0f),c10=(f8)(0.0f),c11=(f8)(0.0f);
    #pragma unroll 1
    for(uint kt=0;kt<ktiles;++kt) {
        i2 a0=packed_fragment(x,mt,kt,ktiles,lane), a1=packed_fragment(x,mt+1u,kt,ktiles,lane);
        i2 b0=packed_fragment(w,nt,kt,ktiles,lane), b1=packed_fragment(w,nt+1u,kt,ktiles,lane);
        c00=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a0,b0,c00);
        c01=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a0,b1,c01);
        c10=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a1,b0,c10);
        c11=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a1,b1,c11);
    }
    store_tile(c00,y,bias,residual,mt*16u,nt*16u,M,N,lane,scale,flags);
    store_tile(c01,y,bias,residual,mt*16u,(nt+1u)*16u,M,N,lane,scale,flags);
    store_tile(c10,y,bias,residual,(mt+1u)*16u,nt*16u,M,N,lane,scale,flags);
    store_tile(c11,y,bias,residual,(mt+1u)*16u,(nt+1u)*16u,M,N,lane,scale,flags);
}
// Four independent wave32 tiles in a workgroup. No inter-wave communication.
// Return is uniform inside each wave; there are no workgroup barriers.
__kernel __attribute__((reqd_work_group_size(128,1,1)))
void linear_fp8_packed16_w4(__global const uchar* x,__global const uchar* w,__global float* y,
 __global const float* bias,__global const float* residual,uint M,uint N,uint K,float scale,uint flags) {
    uint tid=__builtin_amdgcn_workitem_id_x(), lane=tid&31u;
    uint mt=4u*__builtin_amdgcn_workgroup_id_x()+(tid>>5u), nt=__builtin_amdgcn_workgroup_id_y();
    if(mt*16u>=M) return;
    uint ktiles=(K+15u)/16u;
    f8 c=(f8)(0.0f);
    #pragma unroll 1
    for(uint kt=0;kt<ktiles;++kt) {
        i2 a=packed_fragment(x,mt,kt,ktiles,lane), b=packed_fragment(w,nt,kt,ktiles,lane);
        c=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,c);
    }
    store_tile(c,y,bias,residual,mt*16u,nt*16u,M,N,lane,scale,flags);
}
