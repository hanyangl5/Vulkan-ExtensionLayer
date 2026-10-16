// Copyright (c) 2015 Microsoft. All rights reserved.
// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: MIT
// Vulkan adaptation of the pinned D3D12 fallback CalculateMortonCodes,
// Bitonic{Pre,Outer,Inner}Sort, BuildBVHSplits and ComputeAABBs passes.
// Original license: upstream/d3d12/LICENSE.
// BDA records/headers replace D3D12 root UAVs/wrapped pointers. The sorting
// network retains 1024 threads and 2048 shared items per group. Fitting uses
// device-scope acquire/release atomics to publish children across workgroups.
#extension GL_EXT_shader_16bit_storage : require
#extension GL_EXT_shader_explicit_arithmetic_types_int16 : require
#extension GL_KHR_memory_scope_semantics : require
layout(buffer_reference, std430, buffer_reference_align=2) readonly buffer VELIndex16 { uint16_t index[]; };
struct GeometryInput {
    uint64_t vertices, indices, transform, stride;
    uint count, base, first, offset;
    uint transform_offset, index_type, geometry_type, flags;
};
layout(buffer_reference, std430, buffer_reference_align=8) readonly buffer VELGeometries { GeometryInput g[]; };
layout(push_constant) uniform BuildParameters {
    uint64_t dst, scratch, geometries, src;
    uint count, capacity, geometry_count, top;
    uint k, j, update, flags;
} b;
#if VEL_BUILD_PASS == 4 || VEL_BUILD_PASS == 5 || VEL_BUILD_PASS == 6
layout(local_size_x=1024) in;
#else
layout(local_size_x=256) in;
#endif
uint nodeCount() { return b.count==0u ? 1u : b.count*2u-1u; }
uint leafBase() { return b.count==0u ? 0u : b.count-1u; }
uint dataOffset() { return 16u+nodeCount()*8u; }
uint geometryOffset() { return dataOffset()+b.count*32u; }
uint inverseOffset() { return geometryOffset()+b.geometry_count; }
uint parentOffset() { return inverseOffset()+b.count; }
uint reductionCount() { return (b.count+255u)/256u; }
uint tempOffset() { return 8u+reductionCount()*8u; }
uint boxesOffset() { return tempOffset()+b.count*32u; }
uint keysOffset() { return boxesOffset()+b.count*8u; }
uint indicesOffset() { return keysOffset()+b.capacity; }
uint countersOffset() { return indicesOffset()+b.capacity; }
uint naddr(uint n) { return 16u+n*8u; }
vec3 infinity3() { return vec3(1.0/0.0); }
void bounds(uint64_t address,uint offset,vec3 lo,vec3 hi) { wv(address,offset,lo); wv(address,offset+4u,hi); }
GeometryInput geometryFor(uint element) {
    uint lo=0u,hi=b.geometry_count;
    while(lo+1u<hi) {
        uint mid=(lo+hi)/2u;
        if(VELGeometries(b.geometries).g[mid].base<=element) lo=mid; else hi=mid;
    }
    return VELGeometries(b.geometries).g[lo];
}
#if VEL_BUILD_PASS == 0
void main() {
    uint i=gl_GlobalInvocationID.x;
    if(i==0u) {
        for(uint k=0u;k<16u;k++) ws(b.dst,k,0u);
        ws(b.dst,0u,b.top); ws(b.dst,1u,b.count); ws(b.dst,2u,leafBase()); ws(b.dst,3u,dataOffset());
        ws(b.dst,4u,b.geometry_count); ws(b.dst,5u,geometryOffset()); ws(b.dst,6u,inverseOffset());
        ws(b.dst,7u,parentOffset()); ws(b.dst,8u,((parentOffset()+nodeCount())*4u+255u)&~255u);
        ws(b.dst,9u,b.flags); ws(b.dst,10u,2u);
        if(b.count==0u) { bounds(b.dst,16u,infinity3(),-infinity3()); ws(b.dst,19u,0x80000000u); ws(b.dst,23u,0u); }
    }
    if(i<nodeCount()) ws(b.dst,parentOffset()+i,0xffffffffu);
    if(i<b.geometry_count) ws(b.dst,geometryOffset()+i,VELGeometries(b.geometries).g[i].count);
}
#elif VEL_BUILD_PASS == 1
shared vec3 groupLo[256]; shared vec3 groupHi[256];
void main() {
    uint i=gl_GlobalInvocationID.x,tid=gl_LocalInvocationID.x;
    vec3 lo=infinity3(),hi=-lo;
    if(i<b.count) {
        GeometryInput g=geometryFor(i);
        uint local=i-g.base;
        uint slot=b.update!=0u ? vu(b.dst,inverseOffset()+i) : i;
        uint64_t outBuffer=b.update!=0u ? b.dst : b.scratch;
        uint record=(b.update!=0u ? dataOffset() : tempOffset())+slot*32u;
        for(uint k=0u;k<32u;k++) ws(outBuffer,record+k,0u);
        if(b.top!=0u) {
            uint64_t src=g.vertices+uint64_t(g.offset);
            src=g.index_type!=0u ? va(src,local*2u) : src+uint64_t(local)*64ul;
            for(uint k=0u;k<16u;k++) ws(outBuffer,record+k,vu(src,k));
            ws(outBuffer,record+28u,local);
            mat3 m=transpose(mat3(vv(src,0u),vv(src,4u),vv(src,8u)));
            mat3 inv=inverse(m);
            vec3 it=-inv*vec3(vf(src,3u),vf(src,7u),vf(src,11u));
            for(uint row=0u;row<3u;row++) {
                wv(outBuffer,record+16u+row*4u,vec3(inv[0][row],inv[1][row],inv[2][row]));
                wf(outBuffer,record+19u+row*4u,it[row]);
            }
            uint64_t blas=va(src,14u);
            if(blas!=0ul && vu(blas,1u)!=0u) {
                vec3 bl=vv(blas,16u),bh=vv(blas,20u);
                for(uint k=0u;k<8u;k++) {
                    vec3 p=tx(src,0u,vec3((k&1u)!=0u?bh.x:bl.x,(k&2u)!=0u?bh.y:bl.y,(k&4u)!=0u?bh.z:bl.z),1.0);
                    lo=min(lo,p); hi=max(hi,p);
                }
            }
        } else {
            if(g.geometry_type==0u) {
                bool primitiveActive=true;
                for(uint k=0u;k<3u;k++) {
                    uint idx=local*3u+k;
                    if(g.index_type==0u) idx=vu(g.indices+uint64_t(g.offset),idx);
                    else if(g.index_type==1u) idx=uint(VELIndex16(g.indices+uint64_t(g.offset)).index[idx]);
                    uint64_t vertex=g.vertices+uint64_t(idx+g.first)*g.stride;
                    if(g.index_type==2u) vertex+=uint64_t(g.offset);
                    vec3 p=vv(vertex,0u);
                    if(k==0u && isnan(p.x)) primitiveActive=false;
                    if(g.transform!=0ul) p=tx(g.transform,g.transform_offset/4u,p,1.0);
                    wv(outBuffer,record+k*4u,p);
                    lo=min(lo,p); hi=max(hi,p);
                }
                if(!primitiveActive) { lo=infinity3();hi=-lo; }
            } else {
                uint64_t a=g.vertices+uint64_t(g.offset)+uint64_t(local)*g.stride;
                lo=vv(a,0u); hi=vv(a,3u);
                wv(outBuffer,record,lo); wv(outBuffer,record+4u,hi);
                if(isnan(lo.x)) { lo=infinity3();hi=-lo; }
            }
            // The geometry's ordinal is its position in the input descriptor array.
            uint geo=0u;
            uint lower=0u,upper=b.geometry_count;
            while(lower+1u<upper) { uint mid=(lower+upper)/2u; if(VELGeometries(b.geometries).g[mid].base<=i) lower=mid; else upper=mid; }
            geo=lower;
            ws(outBuffer,record+12u,local); ws(outBuffer,record+13u,geo);
            ws(outBuffer,record+14u,g.flags); ws(outBuffer,record+15u,g.geometry_type);
        }
        if(b.update!=0u) bounds(b.dst,naddr(leafBase()+slot),lo,hi);
        else bounds(b.scratch,boxesOffset()+i*8u,lo,hi);
    }
    if(b.update!=0u) return;
    groupLo[tid]=lo; groupHi[tid]=hi; barrier();
    for(uint step=128u;step>0u;step/=2u) {
        if(tid<step) { groupLo[tid]=min(groupLo[tid],groupLo[tid+step]); groupHi[tid]=max(groupHi[tid],groupHi[tid+step]); }
        barrier();
    }
    if(tid==0u) bounds(b.scratch,8u+gl_WorkGroupID.x*8u,groupLo[0],groupHi[0]);
}
#elif VEL_BUILD_PASS == 2
shared vec3 groupLo[256]; shared vec3 groupHi[256];
void main() {
    uint tid=gl_LocalInvocationID.x;
    vec3 lo=infinity3(),hi=-lo;
    for(uint i=tid;i<reductionCount();i+=256u) {
        lo=min(lo,vv(b.scratch,8u+i*8u)); hi=max(hi,vv(b.scratch,12u+i*8u));
    }
    groupLo[tid]=lo; groupHi[tid]=hi; barrier();
    for(uint step=128u;step>0u;step/=2u) {
        if(tid<step) { groupLo[tid]=min(groupLo[tid],groupLo[tid+step]); groupHi[tid]=max(groupHi[tid],groupHi[tid+step]); }
        barrier();
    }
    if(tid==0u) bounds(b.scratch,0u,groupLo[0],groupHi[0]);
}
#elif VEL_BUILD_PASS == 3
void main() {
    uint i=gl_GlobalInvocationID.x;
    if(i>=b.capacity) return;
    uint code=0xffffffffu;
    if(i<b.count) {
        vec3 lo=vv(b.scratch,0u),sceneHi=vv(b.scratch,4u);
        bool emptyScene=any(isinf(lo))||any(isinf(sceneHi));
        vec3 extent=emptyScene?vec3(1.0):max(sceneHi-lo,vec3(0.00001));
        if(emptyScene) lo=vec3(0.0);
        vec3 centerPosition=(vv(b.scratch,boxesOffset()+i*8u)+vv(b.scratch,boxesOffset()+i*8u+4u))*0.5;
        // Inactive/empty instances sort at a deterministic sentinel position.
        if(any(isinf(centerPosition))||any(isnan(centerPosition))) centerPosition=lo;
        uvec3 xyz=uvec3(clamp((centerPosition-lo)/extent*1024.0,vec3(0),vec3(1023)));
        uvec3 coord=xyz.yxz; code=0u;
        for(uint bit=0u;bit<10u;bit++) for(uint axis=0u;axis<3u;axis++)
            if((coord[axis]&(1u<<bit))!=0u) code|=1u<<(bit*3u+axis);
    }
    ws(b.scratch,keysOffset()+i,code); ws(b.scratch,indicesOffset()+i,i<b.count?i:0xffffffffu);
}
#elif VEL_BUILD_PASS == 4 || VEL_BUILD_PASS == 6
shared uint sortKeys[2048]; shared uint sortIndices[2048];
uint insertBit(uint v,uint mask) { return ((v&~(mask-1u))<<1u)|(v&(mask-1u))|mask; }
void comparePair(uint a,uint c) {
    uint ka=sortKeys[a],kc=sortKeys[c],ia=sortIndices[a],ic=sortIndices[c];
    if(ka>kc||(ka==kc&&ia>ic)) { sortKeys[a]=kc;sortKeys[c]=ka;sortIndices[a]=ic;sortIndices[c]=ia; }
}
void main() {
    uint tid=gl_LocalInvocationID.x,base=gl_WorkGroupID.x*2048u;
    for(uint x=tid;x<2048u;x+=1024u) {
        uint i=base+x;
        sortKeys[x]=i<b.count?vu(b.scratch,keysOffset()+i):0xffffffffu;
        sortIndices[x]=i<b.count?vu(b.scratch,indicesOffset()+i):0xffffffffu;
    }
    barrier();
#if VEL_BUILD_PASS == 4
    for(uint k=2u;k<=2048u;k*=2u) for(uint j=k/2u;j>0u;j/=2u) {
        uint c=insertBit(tid,j),a=c^(k==2u*j?k-1u:j); comparePair(a,c); barrier();
    }
#else
    for(uint j=1024u;j>0u;j/=2u) { uint c=insertBit(tid,j);comparePair(c^j,c);barrier(); }
#endif
    for(uint x=tid;x<2048u;x+=1024u) if(base+x<b.capacity) {
        ws(b.scratch,keysOffset()+base+x,sortKeys[x]); ws(b.scratch,indicesOffset()+base+x,sortIndices[x]);
    }
}
#elif VEL_BUILD_PASS == 5
void main() {
    uint mask=b.j-1u,v=gl_GlobalInvocationID.x;
    uint c=((v&~mask)<<1u)|(v&mask)|b.j;
    uint a=c^(b.k==2u*b.j?b.k-1u:b.j);
    if(c>=b.capacity) return;
    uint ka=vu(b.scratch,keysOffset()+a),kc=vu(b.scratch,keysOffset()+c);
    uint ia=vu(b.scratch,indicesOffset()+a),ic=vu(b.scratch,indicesOffset()+c);
    if(ka>kc||(ka==kc&&ia>ic)) {
        ws(b.scratch,keysOffset()+a,kc);ws(b.scratch,keysOffset()+c,ka);
        ws(b.scratch,indicesOffset()+a,ic);ws(b.scratch,indicesOffset()+c,ia);
    }
}
#elif VEL_BUILD_PASS == 7
void main() {
    uint i=gl_GlobalInvocationID.x;
    if(i>=b.count) return;
    uint original=vu(b.scratch,indicesOffset()+i);
    for(uint k=0u;k<32u;k++) ws(b.dst,dataOffset()+i*32u+k,vu(b.scratch,tempOffset()+original*32u+k));
    ws(b.dst,inverseOffset()+original,i);
    uint node=naddr(leafBase()+i);
    bounds(b.dst,node,vv(b.scratch,boxesOffset()+original*8u),vv(b.scratch,boxesOffset()+original*8u+4u));
    ws(b.dst,node+3u,0x80000000u|i);ws(b.dst,node+7u,1u);
}
#elif VEL_BUILD_PASS == 8
int prefix(int a,int c) {
    if(a<0||c<0||a>=int(b.count)||c>=int(b.count)) return -1;
    uint ka=vu(b.scratch,keysOffset()+uint(a)),kc=vu(b.scratch,keysOffset()+uint(c));
    return ka!=kc?31-findMSB(ka^kc):31-findMSB(uint(a^c))+31;
}
void main() {
    uint idx=gl_GlobalInvocationID.x;
    if(idx+1u>=b.count) return;
    int i=int(idx),d=clamp(prefix(i,i+1)-prefix(i,i-1),-1,1);
    int minPrefix=prefix(i,i-d),maxLength=2;
    while(prefix(i,i+maxLength*d)>minPrefix) maxLength*=4;
    int length=0;
    for(int step=maxLength/2;step>0;step/=2) if(prefix(i,i+(length+step)*d)>minPrefix) length+=step;
    int end=i+length*d,first=min(i,end),last=max(i,end);
    int commonPrefix=prefix(first,last),split=first,step=last-first;
    do { step=(step+1)>>1;int candidate=split+step; if(candidate<last&&prefix(first,candidate)>commonPrefix) split=candidate; } while(step>1);
    uint left=split==first?leafBase()+uint(split):uint(split);
    uint right=split+1==last?leafBase()+uint(split+1):uint(split+1);
    ws(b.dst,naddr(idx)+3u,left);ws(b.dst,naddr(idx)+7u,right);
    ws(b.dst,parentOffset()+left,idx);ws(b.dst,parentOffset()+right,idx);
}
#elif VEL_BUILD_PASS == 9
void main() { uint i=gl_GlobalInvocationID.x; if(i+1u<b.count) ws(b.scratch,countersOffset()+i,0u); }
#elif VEL_BUILD_PASS == 10
void main() {
    uint i=gl_GlobalInvocationID.x;
    if(i>=b.count) return;
    uint node=leafBase()+i,count=1u;
    while(node!=0u) {
        uint parent=vu(b.dst,parentOffset()+node);
        // Atomic release publishes this child's AABB. The second child's
        // acquire observes it before reading both bounds and advancing upward.
        uint other=atomicAdd(VELMemory(b.scratch).w[countersOffset()+parent],count,
                             gl_ScopeDevice,gl_StorageSemanticsBuffer,gl_SemanticsAcquireRelease);
        if(other==0u) return;
        uint p=naddr(parent),left=vu(b.dst,p+3u),right=vu(b.dst,p+7u);
        bool isLeft=left==node,otherSmaller=count>other;
        if(isLeft?otherSmaller:!otherSmaller) { uint temp=left;left=right;right=temp;ws(b.dst,p+3u,left);ws(b.dst,p+7u,right); }
        bounds(b.dst,p,min(vv(b.dst,naddr(left)),vv(b.dst,naddr(right))),max(vv(b.dst,naddr(left)+4u),vv(b.dst,naddr(right)+4u)));
        node=parent;count+=other;
    }
}
#elif VEL_BUILD_PASS == 11
void main() {
    uint limit=min(b.count,vu(b.src,8u)/4u);
    for(uint i=gl_GlobalInvocationID.x;i<limit;i+=gl_NumWorkGroups.x*gl_WorkGroupSize.x) ws(b.dst,i,vu(b.src,i));
}
#endif
