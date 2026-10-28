// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
struct NestedPayload { uint level; uint hash; uint visits; uint unwind; };
#ifdef NESTED_RAYGEN
layout(location=0) rayPayloadEXT NestedPayload payload;
#else
layout(location=0) rayPayloadInEXT NestedPayload payload;
#endif
layout(set=0,binding=0,std430) buffer Output { uint words[]; } outputData;
layout(set=0,binding=1) uniform accelerationStructureEXT scene;
layout(push_constant) uniform Push { uint limit; uint seed; uint flip; uint reserved; } pc;
uint nested_id() { return gl_LaunchIDEXT.x+gl_LaunchSizeEXT.x*(gl_LaunchIDEXT.y+gl_LaunchSizeEXT.y*gl_LaunchIDEXT.z); }
uint nested_mix(uint h,uint x) { return (h^x)*1664525u+1013904223u; }
uint nested_local(uint id,uint level,uint kind,uint i) {
    return (pc.seed+id*97u+level*257u+kind*13u+i*19u)^(0x9e3779b9u+i*0x10203u);
}
void nested_trace(uint level) {
    bool hit=((nested_id()+level+pc.flip)&1u)==0u;
    traceRayEXT(scene,gl_RayFlagsOpaqueEXT,255,0,0,0,vec3(hit?0.0:4.0,0,-1),0.0,vec3(0,0,1),10.0,0);
}
void nested_visit(uint kind) {
    uint id=nested_id(),level=payload.level;
    uint localValues[17];
    for(uint i=0;i<17u;++i) localValues[i]=nested_local(id,level,kind,i);
    payload.hash=nested_mix(payload.hash,level*37u+kind);
    payload.visits++;
    outputData.words[id*96u+16u+2u*(level-1u)]=payload.hash;
    if(level<pc.limit) {
        payload.level=level+1u;
        nested_trace(level+1u);
    }
    uint checksum=0u;
    for(uint i=0;i<17u;++i) checksum=(checksum*33u)^localValues[i];
    payload.hash=nested_mix(payload.hash,checksum^(level*0x10001u)^kind);
    payload.unwind+=checksum^level^kind;
    payload.level=level;
    outputData.words[id*96u+17u+2u*(level-1u)]=payload.hash;
}
