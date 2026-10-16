// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
namespace vel::rt {
inline constexpr const char *kRuntimeTestShader = R"glsl(
layout(local_size_x=8,local_size_y=4,local_size_z=1) in;
layout(push_constant) uniform Result { uint64_t address; } result;
uint rewrite_getStackSize() { return VEL_STACK_WORDS*4u; }
void rewrite_setLaunchParams(inout RuntimeData r, uint x, uint y) {
    fb_Fallback_SetLaunchParams(r,gl_GlobalInvocationID.x,gl_GlobalInvocationID.y,x,y,gl_LocalInvocationIndex);
}
int rewrite_dispatch(inout RuntimeData r, int state) {
    uint b=(gl_GlobalInvocationID.y*8u+gl_GlobalInvocationID.x)*16u;
    if(state==1) {
        uint checks=vu(result.address,b);
        if(fb_Fallback_AnyHitResult(r)==13 && vu(result.address,b+1u)==0xabcdef01u) checks|=32768u;
        ws(result.address,b,checks);
        return r.Stack[stackFrameOffset(r)];
    }
    uint checks=0u;
    if(fb_dxop_dispatchRaysIndex(r,0)==gl_GlobalInvocationID.x &&
       fb_dxop_dispatchRaysIndex(r,1)==gl_GlobalInvocationID.y &&
       fb_dxop_dispatchRaysDimensions(r,0)==7u && fb_dxop_dispatchRaysDimensions(r,1)==3u &&
       fb_Fallback_GroupIndex(r)==gl_LocalInvocationIndex) checks|=1u;
    int top=stackFrameOffset(r);
    if(top==VEL_STACK_WORDS-1 && r.Stack[top]==-1) checks|=2u;
    if(payloadOffset(r)==1111 && committedAttrOffset(r)==2222 && pendingAttrOffset(r)==3333) checks|=4u;
    stackFramePush(r,4);
    fb_Fallback_RuntimeDataStoreInt(r,stackFrameOffset(r),12345);
    bool frame=stackFrameOffset(r)==top-4 && fb_Fallback_RuntimeDataLoadInt(r,top-4)==12345;
    stackFramePop(r,4);
    if(frame && stackFrameOffset(r)==top) checks|=8u;
    traceFramePush(r,4);
    int pending=pendingAttrOffset(r),committed=committedAttrOffset(r);
    if(committed==top-6 && pending==top-10) checks|=16u;
    fb_Fallback_RuntimeDataStoreInt(r,pending,0x12345678);
    fb_Fallback_SetPendingTriVals(r,19u,2u,3u,4u,5.0,254u);
    fb_Fallback_CommitHit(r);
    if(committedAttrOffset(r)==pending && pendingAttrOffset(r)==committed &&
       fb_Fallback_RuntimeDataLoadInt(r,committedAttrOffset(r))==0x12345678) checks|=32u;
    if(fb_dxop_rayTCurrent(r)==5.0 && fb_dxop_primitiveID(r)==2u && fb_dxop_instanceIndex(r)==3u &&
       fb_dxop_instanceID(r)==4u && fb_dxop_hitKind(r)==254u && fb_Fallback_ShaderRecordOffset(r)==19u) checks|=64u;
    fb_Fallback_SetPendingTriVals(r,31u,32u,33u,34u,0.5,255u);
    if(fb_dxop_rayTCurrent(r)==5.0 && fb_dxop_primitiveID(r)==2u &&
       fb_dxop_pending_primitiveID(r)==32u && fb_dxop_pending_rayTCurrent(r)==0.5) checks|=128u;
    fb_Fallback_SetRayFlags(r,5u);
    fb_Fallback_SetRayTMin(r,1.0);
    fb_Fallback_SetWorldRayOrigin(r,1.0,2.0,3.0);
    fb_Fallback_SetWorldRayDirection(r,4.0,5.0,6.0);
    fb_Fallback_SetObjectRayOrigin(r,7.0,8.0,9.0);
    fb_Fallback_SetObjectRayDirection(r,10.0,11.0,12.0);
    TraceRaySpills_ClosestHit hit;
    traceRaySave_ClosestHit(r,hit);
    int old=fb_Fallback_TraceRayBegin(r,31u,9.0,8.0,7.0,0.1,6.0,5.0,4.0,99.0,17);
    if(old==1111 && payloadOffset(r)==17 && fb_dxop_rayFlags(r)==31u && fb_dxop_rayTCurrent(r)==99.0 &&
       fb_dxop_worldRayOrigin(r,0)==9.0 && fb_dxop_worldRayDirection(r,2)==4.0 && fb_dxop_rayTMin(r)==0.1) checks|=256u;
    fb_Fallback_TraceRayEnd(r,old);
    if(payloadOffset(r)==1111) checks|=512u;
    fb_Fallback_SetPrimitiveIndex(r,999u);
    fb_Fallback_SetInstanceIndex(r,998u);
    fb_Fallback_SetInstanceID(r,997u);
    fb_Fallback_SetHitKind(r,996u);
    fb_Fallback_SetShaderRecordOffset(r,995u);
    fb_Fallback_SetObjectRayOrigin(r,0.0,0.0,0.0);
    fb_Fallback_SetObjectRayDirection(r,0.0,0.0,0.0);
    traceRayRestore_ClosestHit(r,hit);
    if(fb_dxop_rayFlags(r)==5u && fb_dxop_rayTCurrent(r)==5.0 && fb_dxop_rayTMin(r)==1.0 &&
       fb_dxop_worldRayOrigin(r,1)==2.0 && fb_dxop_worldRayDirection(r,2)==6.0 &&
       fb_dxop_objectRayOrigin(r,1)==8.0 && fb_dxop_objectRayDirection(r,2)==12.0 &&
       fb_dxop_primitiveID(r)==2u && fb_dxop_instanceIndex(r)==3u && fb_dxop_instanceID(r)==4u &&
       fb_dxop_hitKind(r)==254u && fb_Fallback_ShaderRecordOffset(r)==19u) checks|=1024u;
    TraceRaySpills_Miss miss;
    traceRaySave_Miss(r,miss);
    fb_Fallback_TraceRayBegin(r,0u,0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0,18);
    fb_Fallback_SetShaderRecordOffset(r,123u);
    traceRayRestore_Miss(r,miss);
    if(fb_dxop_rayFlags(r)==5u && fb_dxop_rayTCurrent(r)==5.0 && fb_dxop_rayTMin(r)==1.0 &&
       fb_dxop_worldRayOrigin(r,1)==2.0 && fb_dxop_worldRayDirection(r,2)==6.0 &&
       fb_Fallback_ShaderRecordOffset(r)==19u) checks|=2048u;
    float matrix[12]=float[](0.0,1.0,2.0,3.0,4.0,5.0,6.0,7.0,8.0,9.0,10.0,11.0);
    fb_Fallback_SetObjectToWorld(r,matrix);
    fb_Fallback_SetWorldToObject(r,matrix);
    if(fb_dxop_objectToWorld(r,2,3)==11.0 && fb_dxop_worldToObject(r,1,2)==6.0) checks|=4096u;
    fb_Fallback_SetPendingCustomVals(r,41u,42u,43u,44u);
    fb_Fallback_SetPendingRayTCurrent(r,0.25);
    fb_Fallback_SetPendingHitKind(r,45u);
    if(fb_Fallback_PendingShaderRecordOffset(r)==41u && fb_dxop_pending_primitiveID(r)==42u &&
       fb_dxop_pending_instanceIndex(r)==43u && fb_dxop_pending_instanceID(r)==44u &&
       fb_dxop_pending_hitKind(r)==45u && fb_dxop_pending_rayTCurrent(r)==0.25 && fb_dxop_rayTCurrent(r)==5.0) checks|=8192u;
    traceFramePop(r);
    fb_Fallback_SetAnyHitStateId(r,27);
    fb_Fallback_SetAnyHitResult(r,13);
    if(committedAttrOffset(r)==2222 && pendingAttrOffset(r)==3333 && fb_Fallback_AnyHitStateId(r)==27 &&
       fb_Fallback_AnyHitResult(r)==13 && stackFrameOffset(r)==top) checks|=16384u;
    ws(result.address,b,checks);
    ws(result.address,b+1u,0xabcdef01u);
    return 1;
}
void main() { fb_Fallback_Scheduler(0,7u,3u); }
)glsl";
}
