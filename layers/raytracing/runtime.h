// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
namespace vel::rt {
inline constexpr const char *kMemory = R"glsl(
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
layout(buffer_reference, std430, buffer_reference_align=4) buffer VELMemory { uint w[]; };
uint vu(uint64_t a, uint i) { return VELMemory(a).w[i]; }
float vf(uint64_t a, uint i) { return uintBitsToFloat(vu(a,i)); }
vec3 vv(uint64_t a, uint i) { return vec3(vf(a,i),vf(a,i+1),vf(a,i+2)); }
uint64_t va(uint64_t a, uint i) { return uint64_t(vu(a,i)) | (uint64_t(vu(a,i+1))<<32); }
void ws(uint64_t a, uint i, uint v) { VELMemory(a).w[i]=v; }
void wf(uint64_t a, uint i, float v) { ws(a,i,floatBitsToUint(v)); }
void wv(uint64_t a, uint i, vec3 v) { wf(a,i,v.x); wf(a,i+1,v.y); wf(a,i+2,v.z); }
vec3 tx(uint64_t a, uint i, vec3 p, float h) {
    return vec3(dot(vec4(vv(a,i),vf(a,i+3)),vec4(p,h)),
                dot(vec4(vv(a,i+4),vf(a,i+7)),vec4(p,h)),
                dot(vec4(vv(a,i+8),vf(a,i+11)),vec4(p,h)));
}
)glsl";

inline constexpr const char *kBuilder = R"glsl(
#extension GL_EXT_shader_16bit_storage : require
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require
layout(buffer_reference, std430, buffer_reference_align=2) readonly buffer VELIndex16 { uint16_t index[]; };
layout(local_size_x=1) in;
layout(push_constant) uniform VELBuild {
    uint64_t dst; uint64_t vertices; uint64_t indices; uint64_t transform;
    uint count; uint capacity; uint stride; uint indexType;
    uint firstVertex; uint primitiveOffset; uint transformOffset; uint top;
} b;
void main() {
    uint data=4+(2*b.capacity-1)*8;
    ws(b.dst,0,b.top); ws(b.dst,1,b.count); ws(b.dst,2,b.capacity); ws(b.dst,3,data);
    for (uint j=0;j<b.capacity;j++) {
        vec3 lo=vec3(1.0/0.0), hi=-lo;
        if (j<b.count) {
            if (b.top==0) {
                uint outp=data+j*12;
                for (uint k=0;k<3;k++) {
                    uint idx=j*3+k;
                    if (b.indexType==0) idx=vu(b.indices+b.primitiveOffset,idx);
                    else if (b.indexType==1) {
                        idx=uint(VELIndex16(b.indices+b.primitiveOffset).index[idx]);
                    }
                    uint64_t v=b.vertices+uint64_t(idx+b.firstVertex)*b.stride;
                    if (b.indexType==2) v+=b.primitiveOffset;
                    vec3 p=vv(v,0);
                    if (b.transform!=0ul) p=tx(b.transform,b.transformOffset/4,p,1);
                    wv(b.dst,outp+k*4,p); ws(b.dst,outp+k*4+3,0);
                    lo=min(lo,p); hi=max(hi,p);
                }
            } else {
                uint64_t src=b.vertices+b.primitiveOffset+uint64_t(j)*64;
                uint outp=data+j*28;
                for(uint k=0;k<16;k++) ws(b.dst,outp+k,vu(src,k));
                mat3 m=transpose(mat3(vv(src,0),vv(src,4),vv(src,8)));
                mat3 inv=inverse(m);
                vec3 tr=vec3(vf(src,3),vf(src,7),vf(src,11));
                vec3 it=-inv*tr;
                for(uint row=0;row<3;row++) {
                    wv(b.dst,outp+16+row*4,vec3(inv[0][row],inv[1][row],inv[2][row]));
                    wf(b.dst,outp+19+row*4,it[row]);
                }
                uint64_t blas=va(src,14);
                if(vu(blas,1)>0) {
                    vec3 bl=vv(blas,4), bh=vv(blas,8);
                    for(uint k=0;k<8;k++) {
                        vec3 p=tx(src,0,vec3((k&1)!=0?bh.x:bl.x,(k&2)!=0?bh.y:bl.y,(k&4)!=0?bh.z:bl.z),1);
                        lo=min(lo,p); hi=max(hi,p);
                    }
                }
            }
        }
        uint n=4+(b.capacity-1+j)*8;
        wv(b.dst,n,lo); wv(b.dst,n+4,hi);
        ws(b.dst,n+3,0x80000000u|j); ws(b.dst,n+7,0);
    }
    for(int j=int(b.capacity)-2;j>=0;j--) {
        uint left=uint(j)*2+1, right=left+1;
        uint n=4+uint(j)*8,l=4+left*8,r=4+right*8;
        wv(b.dst,n,min(vv(b.dst,l),vv(b.dst,r)));
        wv(b.dst,n+4,max(vv(b.dst,l+4),vv(b.dst,r+4)));
        ws(b.dst,n+3,left); ws(b.dst,n+7,right);
    }
}
)glsl";

inline constexpr const char *kTraversal = R"glsl(
layout(local_size_x=8,local_size_y=8,local_size_z=1) in;
// The runtime descriptor is appended to the application's set layouts.
layout(set=VEL_SET,binding=0,std430) readonly buffer VELDispatch {
    uint64_t raygen; uint64_t miss; uint64_t hit; uint64_t missStride;
    uint64_t hitStride; uvec2 padding; uvec4 dims;
} vel_d;
vec4 vel_payload;
vec2 vel_bary;
vec3 vel_worldOrigin,vel_worldDirection,vel_objectOrigin,vel_objectDirection;
float vel_tmin,vel_tmax;
uint vel_primitive,vel_instance,vel_custom,vel_geometry,vel_hitKind,vel_flags;
uint64_t vel_record;
void vel_hit(uint group);
void vel_miss(uint group);
bool boxHit(vec3 o,vec3 d,vec3 lo,vec3 hi,float mn,float mx) {
    if(any(greaterThan(lo,hi))) return false;
    for(int k=0;k<3;k++) {
        if(d[k]==0) { if(o[k]<lo[k]||o[k]>hi[k]) return false; }
        else { float a=(lo[k]-o[k])/d[k], b=(hi[k]-o[k])/d[k]; mn=max(mn,min(a,b)); mx=min(mx,max(a,b)); }
    }
    return mx>=mn;
}
bool triHit(vec3 o,vec3 d,vec3 a,vec3 b,vec3 c,float mn,inout float mx,out vec2 uv,out bool front) {
    vec3 e=b-a,f=c-a,p=cross(d,f); float det=dot(e,p); front=det>0;
    if(abs(det)<1e-20) return false;
    vec3 s=o-a,q=cross(s,e);
    uv=vec2(dot(s,p),dot(d,q))/det;
    float t=dot(f,q)/det;
    if(uv.x<0||uv.y<0||uv.x+uv.y>1||t<mn||t>mx) return false;
    mx=t; return true;
}
void vel_trace(uint64_t scene,uint flags,uint mask,uint sbtOffset,uint sbtStride,uint missIndex,
               vec3 origin,float tmin,vec3 direction,float tmax,int payloadLocation) {
    float best=tmax; bool found=false;
    uint tstack[64]; uint tsp=1; tstack[0]=0;
    uint bestSbt=0;
    while(tsp>0) {
        uint n=4+tstack[--tsp]*8;
        if(!boxHit(origin,direction,vv(scene,n),vv(scene,n+4),tmin,best)) continue;
        uint child=vu(scene,n+3);
        if((child&0x80000000u)==0) { tstack[tsp++]=child; tstack[tsp++]=vu(scene,n+7); continue; }
        uint inst=child&0x7fffffffu;
        if(inst>=vu(scene,1)) continue;
        uint io=vu(scene,3)+inst*28;
        uint packed=vu(scene,io+12), attr=vu(scene,io+13);
        if(((packed>>24)&mask&255u)==0) continue;
        uint64_t blas=va(scene,io+14);
        vec3 o=tx(scene,io+16,origin,1),d=tx(scene,io+16,direction,0);
        uint stack[64]; uint sp=1; stack[0]=0;
        while(sp>0) {
            uint bn=4+stack[--sp]*8;
            if(!boxHit(o,d,vv(blas,bn),vv(blas,bn+4),tmin,best)) continue;
            uint bc=vu(blas,bn+3);
            if((bc&0x80000000u)==0) { stack[sp++]=bc; stack[sp++]=vu(blas,bn+7); continue; }
            uint prim=bc&0x7fffffffu;
            if(prim>=vu(blas,1)) continue;
            uint po=vu(blas,3)+prim*12;
            vec2 uv; bool front; float candidate=best;
            if(!triHit(o,d,vv(blas,po),vv(blas,po+4),vv(blas,po+8),tmin,candidate,uv,front)) continue;
            front=front!=((attr&0x02000000u)!=0);
            if((attr&0x01000000u)==0 && (((flags&16u)!=0&&!front)||((flags&32u)!=0&&front))) continue;
            // Geometry is opaque; ray and instance overrides can still make it nonopaque.
            bool opaque=(flags&1u)!=0 || ((flags&2u)==0 && (attr&0x08000000u)==0);
            if(((flags&64u)!=0&&opaque)||((flags&128u)!=0&&!opaque)) continue;
            best=candidate; found=true; vel_bary=uv; vel_primitive=prim; vel_instance=inst;
            vel_custom=packed&0xffffffu; vel_geometry=0; vel_hitKind=front?254u:255u;
            vel_objectOrigin=o; vel_objectDirection=d;
            bestSbt=(attr&0xffffffu)+(sbtOffset&15u); // Single geometry has geometryIndex zero.
            if((flags&4u)!=0) break;
        }
        if(found&&(flags&4u)!=0) break;
    }
    vel_worldOrigin=origin; vel_worldDirection=direction; vel_tmin=tmin; vel_tmax=best; vel_flags=flags;
    if(found) {
        if((flags&8u)==0) { vel_record=vel_d.hit+uint64_t(bestSbt)*vel_d.hitStride; vel_hit(vu(vel_record,0)); }
    } else { vel_record=vel_d.miss+uint64_t(missIndex&65535u)*vel_d.missStride; vel_miss(vu(vel_record,0)); }
}
)glsl";
}  // namespace vel::rt
