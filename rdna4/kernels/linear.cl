// SPDX-License-Identifier: MIT
// Independent linear-operator candidates for an RDNA4 neural backend.
// NOT an implementation of the proprietary Swin/NR network or its runtime ABI.
// X: column-major M x K. W: row-major K x N. Y, residual: row-major M x N.
// Y = (X*W)*scale + bias + residual. No activation or model quantization is implied.
// FP8 inputs use gfx1201 E4M3FN, not CDNA FNUZ. Accumulation/output: FP32.
#pragma OPENCL EXTENSION cl_khr_fp16 : enable

typedef float f8 __attribute__((ext_vector_type(8)));
typedef half h8 __attribute__((ext_vector_type(8)));
typedef int i2 __attribute__((ext_vector_type(2)));

static inline __attribute__((always_inline)) i2 load_x8(
    __global const uchar* x, uint m, uint k, uint M, uint K, uint lane) {
    uint lo = 0, hi = 0;
    uint row = m + (lane & 15u), start = k + (lane >> 4u)*8u;
    for (uint j=0; j<8; ++j) {
        uint v = (row<M && start+j<K) ? x[(ulong)(start+j)*M+row] : 0;
        if(j<4) lo |= v << (j*8); else hi |= v << ((j-4)*8);
    }
    return (i2)((int)lo,(int)hi);
}
static inline __attribute__((always_inline)) i2 load_w8(
    __global const uchar* w, uint n, uint k, uint N, uint K, uint lane) {
    uint lo = 0, hi = 0;
    uint col = n + (lane & 15u), start = k + (lane >> 4u)*8u;
    for (uint j=0; j<8; ++j) {
        uint v = (col<N && start+j<K) ? w[(ulong)(start+j)*N+col] : 0;
        if(j<4) lo |= v << (j*8); else hi |= v << ((j-4)*8);
    }
    return (i2)((int)lo,(int)hi);
}
static inline __attribute__((always_inline)) h8 load_x16(
    __global const half* x, uint m, uint k, uint M, uint K, uint lane) {
    h8 a;
    uint row=m+(lane&15u), start=k+(lane>>4u)*8u;
    for(uint j=0;j<8;++j) a[j]=(row<M && start+j<K) ? x[(ulong)(start+j)*M+row] : (half)0;
    return a;
}
static inline __attribute__((always_inline)) h8 load_w16(
    __global const half* w, uint n, uint k, uint N, uint K, uint lane) {
    h8 b;
    uint col=n+(lane&15u), start=k+(lane>>4u)*8u;
    for(uint j=0;j<8;++j) b[j]=(col<N && start+j<K) ? w[(ulong)(start+j)*N+col] : (half)0;
    return b;
}
static inline __attribute__((always_inline)) void store_tile(
    f8 value, __global float* y, __global const float* bias, __global const float* residual,
    uint m, uint n, uint M, uint N, uint lane, float scale, uint flags) {
    uint col=n+(lane&15u), row=m+(lane>>4u)*8u;
    for(uint j=0;j<8;++j) {
        if(row+j<M && col<N) {
            ulong pos=(ulong)(row+j)*N+col;
            float v=value[j]*scale;
            if(flags&1u) v+=bias[col];
            if(flags&2u) v+=residual[pos];
            y[pos]=v;
        }
    }
}
#define SIGNATURE(NAME,TYPE) __kernel __attribute__((reqd_work_group_size(32,1,1))) void NAME( \
 __global const TYPE* x, __global const TYPE* w, __global float* y, \
 __global const float* bias, __global const float* residual, uint M, uint N, uint K, float scale, uint flags)

SIGNATURE(linear_fp8_tile16,uchar) {
    uint lane=__builtin_amdgcn_workitem_id_x();
    uint m=16u*__builtin_amdgcn_workgroup_id_x(), n=16u*__builtin_amdgcn_workgroup_id_y();
    f8 c=(f8)(0.0f);
    #pragma unroll 1
    for(uint k=0;k<K;k+=16) {
        i2 a=load_x8(x,m,k,M,K,lane), b=load_w8(w,n,k,N,K,lane);
        c=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a,b,c);
    }
    store_tile(c,y,bias,residual,m,n,M,N,lane,scale,flags);
}
SIGNATURE(linear_fp8_tile32,uchar) {
    uint lane=__builtin_amdgcn_workitem_id_x();
    uint m=32u*__builtin_amdgcn_workgroup_id_x(), n=32u*__builtin_amdgcn_workgroup_id_y();
    f8 c00=(f8)(0.0f), c01=(f8)(0.0f), c10=(f8)(0.0f), c11=(f8)(0.0f);
    #pragma unroll 1
    for(uint k=0;k<K;k+=16) {
        i2 a0=load_x8(x,m,k,M,K,lane), a1=load_x8(x,m+16,k,M,K,lane);
        i2 b0=load_w8(w,n,k,N,K,lane), b1=load_w8(w,n+16,k,N,K,lane);
        c00=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a0,b0,c00);
        c01=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a0,b1,c01);
        c10=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a1,b0,c10);
        c11=__builtin_amdgcn_wmma_f32_16x16x16_fp8_fp8_w32_gfx12(a1,b1,c11);
    }
    store_tile(c00,y,bias,residual,m,n,M,N,lane,scale,flags);
    store_tile(c01,y,bias,residual,m,n+16,M,N,lane,scale,flags);
    store_tile(c10,y,bias,residual,m+16,n,M,N,lane,scale,flags);
    store_tile(c11,y,bias,residual,m+16,n+16,M,N,lane,scale,flags);
}
SIGNATURE(linear_fp16_tile32,half) {
    uint lane=__builtin_amdgcn_workitem_id_x();
    uint m=32u*__builtin_amdgcn_workgroup_id_x(), n=32u*__builtin_amdgcn_workgroup_id_y();
    f8 c00=(f8)(0.0f), c01=(f8)(0.0f), c10=(f8)(0.0f), c11=(f8)(0.0f);
    #pragma unroll 1
    for(uint k=0;k<K;k+=16) {
        h8 a0=load_x16(x,m,k,M,K,lane), a1=load_x16(x,m+16,k,M,K,lane);
        h8 b0=load_w16(w,n,k,N,K,lane), b1=load_w16(w,n+16,k,N,K,lane);
        c00=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a0,b0,c00);
        c01=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a0,b1,c01);
        c10=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a1,b0,c10);
        c11=__builtin_amdgcn_wmma_f32_16x16x16_f16_w32_gfx12(a1,b1,c11);
    }
    store_tile(c00,y,bias,residual,m,n,M,N,lane,scale,flags);
    store_tile(c01,y,bias,residual,m,n+16,M,N,lane,scale,flags);
    store_tile(c10,y,bias,residual,m+16,n,M,N,lane,scale,flags);
    store_tile(c11,y,bias,residual,m+16,n+16,M,N,lane,scale,flags);
}
