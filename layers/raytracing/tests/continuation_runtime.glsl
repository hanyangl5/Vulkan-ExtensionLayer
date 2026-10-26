// Derived from microsoft/DirectXShaderCompiler lib/DxrFallback/runtime/runtime.c.
// Pinned revision: 61e9771fc34bc914565dfc95a11f731f853e8ac9.
// SPDX-License-Identifier: NCSA
// See ../docs/upstream/dxc/LICENSE.TXT for the upstream copyright and license notice.
//
// Vulkan adaptations: pointer arguments become inout parameters; per-invocation
// stack storage is embedded in RuntimeData; matrices are scalar arrays. The
// generated state-machine linker must implement rewrite_dispatch,
// rewrite_setLaunchParams and rewrite_getStackSize. The stack size is measured
// in bytes, frame/attribute offsets in 32-bit words, as in the upstream runtime.
// No automatic integration with the legacy prototype linker is implied.
#ifndef VEL_STACK_WORDS
#define VEL_STACK_WORDS 256
#endif
struct RuntimeData
{
  int DispatchRaysIndex[2];
  int DispatchRaysDimensions[2];

  float RayTMin;
  float RayTCurrent;
  uint RayFlags;
  float WorldRayOrigin[3];
  float WorldRayDirection[3];
  float ObjectRayOrigin[3];
  float ObjectRayDirection[3];
  float ObjectToWorld[12];
  float WorldToObject[12];

  uint PrimitiveIndex;
  uint InstanceIndex;
  uint InstanceID;
  uint HitKind;
  uint ShaderRecordOffset;


  // Pending hit values - accessed in anyHit and intersection shaders before a hit has been committed
  float PendingRayTCurrent;
  uint PendingPrimitiveIndex;
  uint PendingInstanceIndex;
  uint PendingInstanceID;
  uint PendingHitKind;
  uint PendingShaderRecordOffset; 

  int GroupIndex; 
  int AnyHitResult;
  int AnyHitStateId;  // Originally temporary. We needed to avoid resource usage
                      // in ReportHit() because of linking issues so weset the value here first. 
                      // May be worth retaining to cache the value when fetching the intersection 
                      // stateId (fetch them both at once). 

  int PayloadOffset;            
  int CommittedAttrOffset;      
  int PendingAttrOffset;        
  
  int StackOffset; // offset from the start of the stack
  int Stack[VEL_STACK_WORDS];
};



struct TraceRaySpills_ClosestHit
{
  float RayTMin;                 
  float RayTCurrent;             
  uint RayFlags;             
  float WorldRayOrigin[3];       
  float WorldRayDirection[3];    
  float ObjectRayOrigin[3];      
  float ObjectRayDirection[3];   

  uint PrimitiveIndex;       
  uint InstanceIndex;        
  uint InstanceID;           
  uint HitKind;              
  uint ShaderRecordOffset;
};

struct TraceRaySpills_Miss
{
  float RayTMin;                 
  float RayTCurrent;             
  uint RayFlags;             
  float WorldRayOrigin[3];       
  float WorldRayDirection[3];    
            
  uint ShaderRecordOffset;
};


#define REF(x) (runtimeData.x)
#define REF_FLT(x) (runtimeData.x)
#define REF_STACK(offset) (runtimeData.Stack[runtimeData.StackOffset + offset])
#define REF_FLT_OFS(x, offset) (runtimeData.x[offset])

// Return next stateID
int rewrite_dispatch(inout RuntimeData runtimeData, int stateID);
void rewrite_setLaunchParams(inout RuntimeData runtimeData, uint dimx, uint dimy);
uint rewrite_getStackSize();


void stackInit(inout RuntimeData runtimeData, uint stackSize)
{
  REF(StackOffset) = int(stackSize/4u) - 1;
  REF(PayloadOffset)       = 1111; // recognizable bogus values
  REF(CommittedAttrOffset) = 2222;
  REF(PendingAttrOffset)   = 3333;
}

void stackFramePush(inout RuntimeData runtimeData, int size)
{
  REF(StackOffset) -= size;
}

void stackFramePop(inout RuntimeData runtimeData, int size)
{ 
  REF(StackOffset) += size;
}

int stackFrameOffset(inout RuntimeData runtimeData)
{
  return REF(StackOffset);
}

int payloadOffset(inout RuntimeData runtimeData)
{
  return REF(PayloadOffset);
}

int committedAttrOffset(inout RuntimeData runtimeData)
{
  return REF(CommittedAttrOffset);
}

int pendingAttrOffset(inout RuntimeData runtimeData)
{
  return REF(PendingAttrOffset);
}




void traceFramePush(inout RuntimeData runtimeData, int attrSize)
{
  // Save the old payload and attribute offsets
  REF_STACK(-1) = REF(CommittedAttrOffset);
  REF_STACK(-2) = REF(PendingAttrOffset);

  // Set new offsets
  REF(CommittedAttrOffset) = REF(StackOffset) - 2 - attrSize; 
  REF(PendingAttrOffset)   = REF(StackOffset) - 2 - 2 * attrSize; 
}

void traceFramePop(inout RuntimeData runtimeData)
{
  // Restore the old attribute offsets
  REF(CommittedAttrOffset) = REF_STACK(-1); 
  REF(PendingAttrOffset) = REF_STACK(-2);
}

void traceRaySave_ClosestHit(inout RuntimeData runtimeData, inout TraceRaySpills_ClosestHit spills)
{
  spills.RayFlags              = REF(RayFlags);
  spills.RayTCurrent           = REF_FLT(RayTCurrent);
  spills.RayTMin               = REF_FLT(RayTMin);
  spills.WorldRayOrigin[0]     = REF_FLT(WorldRayOrigin[0]);
  spills.WorldRayOrigin[1]     = REF_FLT(WorldRayOrigin[1]);
  spills.WorldRayOrigin[2]     = REF_FLT(WorldRayOrigin[2]);
  spills.WorldRayDirection[0]  = REF_FLT(WorldRayDirection[0]);
  spills.WorldRayDirection[1]  = REF_FLT(WorldRayDirection[1]);
  spills.WorldRayDirection[2]  = REF_FLT(WorldRayDirection[2]);
  spills.ObjectRayOrigin[0]    = REF_FLT(ObjectRayOrigin[0]);
  spills.ObjectRayOrigin[1]    = REF_FLT(ObjectRayOrigin[1]);
  spills.ObjectRayOrigin[2]    = REF_FLT(ObjectRayOrigin[2]);
  spills.ObjectRayDirection[0] = REF_FLT(ObjectRayDirection[0]);
  spills.ObjectRayDirection[1] = REF_FLT(ObjectRayDirection[1]);
  spills.ObjectRayDirection[2] = REF_FLT(ObjectRayDirection[2]);

  spills.PrimitiveIndex      = REF(PrimitiveIndex);       
  spills.InstanceIndex       = REF(InstanceIndex);        
  spills.InstanceID          = REF(InstanceID);           
  spills.HitKind             = REF(HitKind);              
  spills.ShaderRecordOffset  = REF(ShaderRecordOffset);  
}

void traceRayRestore_ClosestHit(inout RuntimeData runtimeData, inout TraceRaySpills_ClosestHit spills)
{
  REF(RayFlags)                  = spills.RayFlags;               
  REF_FLT(RayTCurrent)           = spills.RayTCurrent;            
  REF_FLT(RayTMin)               = spills.RayTMin;                
  REF_FLT(WorldRayOrigin[0])     = spills.WorldRayOrigin[0];      
  REF_FLT(WorldRayOrigin[1])     = spills.WorldRayOrigin[1];      
  REF_FLT(WorldRayOrigin[2])     = spills.WorldRayOrigin[2];      
  REF_FLT(WorldRayDirection[0])  = spills.WorldRayDirection[0];   
  REF_FLT(WorldRayDirection[1])  = spills.WorldRayDirection[1];   
  REF_FLT(WorldRayDirection[2])  = spills.WorldRayDirection[2];   
  REF_FLT(ObjectRayOrigin[0])    = spills.ObjectRayOrigin[0];     
  REF_FLT(ObjectRayOrigin[1])    = spills.ObjectRayOrigin[1];     
  REF_FLT(ObjectRayOrigin[2])    = spills.ObjectRayOrigin[2];     
  REF_FLT(ObjectRayDirection[0]) = spills.ObjectRayDirection[0];  
  REF_FLT(ObjectRayDirection[1]) = spills.ObjectRayDirection[1];  
  REF_FLT(ObjectRayDirection[2]) = spills.ObjectRayDirection[2];  

  REF(PrimitiveIndex)     = spills.PrimitiveIndex;          
  REF(InstanceIndex)      = spills.InstanceIndex;           
  REF(InstanceID)         = spills.InstanceID;              
  REF(HitKind)            = spills.HitKind;                 
  REF(ShaderRecordOffset) = spills.ShaderRecordOffset;    
}

void traceRaySave_Miss(inout RuntimeData runtimeData, inout TraceRaySpills_Miss spills)
{
  spills.RayFlags              = REF(RayFlags);
  spills.RayTCurrent           = REF_FLT(RayTCurrent);
  spills.RayTMin               = REF_FLT(RayTMin);
  spills.WorldRayOrigin[0]     = REF_FLT(WorldRayOrigin[0]);
  spills.WorldRayOrigin[1]     = REF_FLT(WorldRayOrigin[1]);
  spills.WorldRayOrigin[2]     = REF_FLT(WorldRayOrigin[2]);
  spills.WorldRayDirection[0]  = REF_FLT(WorldRayDirection[0]);
  spills.WorldRayDirection[1]  = REF_FLT(WorldRayDirection[1]);
  spills.WorldRayDirection[2]  = REF_FLT(WorldRayDirection[2]);

  spills.ShaderRecordOffset    = REF(ShaderRecordOffset);  
}

void traceRayRestore_Miss(inout RuntimeData runtimeData, inout TraceRaySpills_Miss spills)
{
  REF(RayFlags)                  = spills.RayFlags;               
  REF_FLT(RayTCurrent)           = spills.RayTCurrent;            
  REF_FLT(RayTMin)               = spills.RayTMin;                
  REF_FLT(WorldRayOrigin[0])     = spills.WorldRayOrigin[0];      
  REF_FLT(WorldRayOrigin[1])     = spills.WorldRayOrigin[1];      
  REF_FLT(WorldRayOrigin[2])     = spills.WorldRayOrigin[2];      
  REF_FLT(WorldRayDirection[0])  = spills.WorldRayDirection[0];   
  REF_FLT(WorldRayDirection[1])  = spills.WorldRayDirection[1];   
  REF_FLT(WorldRayDirection[2])  = spills.WorldRayDirection[2];   

  REF(ShaderRecordOffset) = spills.ShaderRecordOffset;    
}





//////////////////////////////////////////////////////////////////////////
//
// Intrinsics for the fallback layer
//
//////////////////////////////////////////////////////////////////////////

void fb_Fallback_Scheduler(int initialStateId, uint dimx, uint dimy)
{
  RuntimeData runtimeData;

  rewrite_setLaunchParams(runtimeData, dimx, dimy);
  if(REF(DispatchRaysIndex[0]) >= REF(DispatchRaysDimensions[0]) ||
     REF(DispatchRaysIndex[1]) >= REF(DispatchRaysDimensions[1]))
  { 
    return;
  }


  // Set final return stateID into reserved area at stack top
  uint stackSize = rewrite_getStackSize();
  stackInit(runtimeData, stackSize);
  int stackFrameOffs = stackFrameOffset(runtimeData);
  runtimeData.Stack[stackFrameOffs] = -1;

  int stateId = initialStateId;
  int count = 0;
  while( stateId >= 0 )
  {
    stateId = rewrite_dispatch(runtimeData, stateId);
  }
}

void fb_Fallback_SetLaunchParams(inout RuntimeData runtimeData, uint DTidx, uint DTidy, uint dimx, uint dimy, uint groupIndex)
{ 
  REF(DispatchRaysIndex[0]) = int(DTidx);
  REF(DispatchRaysIndex[1]) = int(DTidy);
  REF(DispatchRaysDimensions[0]) = int(dimx);
  REF(DispatchRaysDimensions[1]) = int(dimy);

  REF(GroupIndex) = int(groupIndex);
}

int fb_Fallback_TraceRayBegin(inout RuntimeData runtimeData, uint rayFlags, float ox, float oy, float oz, float tmin, float dx, float dy, float dz, float tmax, int newPayloadOffset)
{ 
  REF(RayFlags) = rayFlags;
  REF_FLT(WorldRayOrigin[0]) = ox;
  REF_FLT(WorldRayOrigin[1]) = oy;
  REF_FLT(WorldRayOrigin[2]) = oz;
  REF_FLT(WorldRayDirection[0]) = dx;
  REF_FLT(WorldRayDirection[1]) = dy;
  REF_FLT(WorldRayDirection[2]) = dz;
  REF_FLT(RayTCurrent) = tmax;
  REF_FLT(RayTMin) = tmin;

  int oldOffset = REF(PayloadOffset);
  REF(PayloadOffset) = newPayloadOffset;
  return oldOffset;
}

void fb_Fallback_TraceRayEnd(inout RuntimeData runtimeData, int oldPayloadOffset)
{
  REF(PayloadOffset) = oldPayloadOffset;
}

void fb_Fallback_SetPendingTriVals(inout RuntimeData runtimeData, uint shaderRecordOffset, uint primitiveIndex, uint instanceIndex, uint instanceID, float t, uint hitKind)
{
  REF(PendingShaderRecordOffset) = shaderRecordOffset;
  REF(PendingPrimitiveIndex) = primitiveIndex;
  REF(PendingInstanceIndex) = instanceIndex;
  REF(PendingInstanceID) = instanceID;
  REF_FLT(PendingRayTCurrent) = t;
  REF(PendingHitKind) = hitKind;
}

void fb_Fallback_SetPendingCustomVals(inout RuntimeData runtimeData, uint shaderRecordOffset, uint primitiveIndex, uint instanceIndex, uint instanceID)
{
  REF(PendingShaderRecordOffset) = shaderRecordOffset;
  REF(PendingPrimitiveIndex) = primitiveIndex;
  REF(PendingInstanceIndex) = instanceIndex;
  REF(PendingInstanceID) = instanceID;
}

void fb_Fallback_CommitHit(inout RuntimeData runtimeData)
{
  REF_FLT(RayTCurrent)    = REF_FLT(PendingRayTCurrent);
  REF(ShaderRecordOffset) = REF(PendingShaderRecordOffset);
  REF(PrimitiveIndex)     = REF(PendingPrimitiveIndex);
  REF(InstanceIndex)      = REF(PendingInstanceIndex);
  REF(InstanceID)         = REF(PendingInstanceID);
  REF(HitKind)            = REF(PendingHitKind);  

  int PendingAttrOffset = REF(PendingAttrOffset);
  REF(PendingAttrOffset) = REF(CommittedAttrOffset);
  REF(CommittedAttrOffset) = PendingAttrOffset;
}


int fb_Fallback_RuntimeDataLoadInt(inout RuntimeData runtimeData, int offset)
{
  return runtimeData.Stack[offset];
}

void fb_Fallback_RuntimeDataStoreInt(inout RuntimeData runtimeData, int offset, int val)
{
  runtimeData.Stack[offset] = val;
}

uint fb_dxop_dispatchRaysIndex(inout RuntimeData runtimeData, int i)
{  
  return uint(REF(DispatchRaysIndex[i]));
}

uint fb_dxop_dispatchRaysDimensions(inout RuntimeData runtimeData, int i)
{  
  return uint(REF(DispatchRaysDimensions[i]));
}

float fb_dxop_rayTMin(inout RuntimeData runtimeData)
{
  return REF_FLT(RayTMin);
}

float fb_Fallback_RayTMin(inout RuntimeData runtimeData)
{
  return REF_FLT(RayTMin);
}

void fb_Fallback_SetRayTMin(inout RuntimeData runtimeData, float t)
{
  REF_FLT(RayTMin) = t;
}

float fb_dxop_rayTCurrent(inout RuntimeData runtimeData)
{
  return REF_FLT(RayTCurrent);
}

float fb_Fallback_RayTCurrent(inout RuntimeData runtimeData)
{
  return REF_FLT(RayTCurrent);
}

void fb_Fallback_SetRayTCurrent(inout RuntimeData runtimeData, float t)
{
  REF_FLT(RayTCurrent) = t;
}

uint fb_dxop_rayFlags(inout RuntimeData runtimeData)
{
  return REF(RayFlags);
}

uint fb_Fallback_RayFlags(inout RuntimeData runtimeData)
{
  return REF(RayFlags);
}

void fb_Fallback_SetRayFlags(inout RuntimeData runtimeData, uint flags)
{
  REF(RayFlags) = flags;
}

float fb_dxop_worldRayOrigin(inout RuntimeData runtimeData, int i)
{ 
  return REF_FLT(WorldRayOrigin[i]);
}

float fb_Fallback_WorldRayOrigin(inout RuntimeData runtimeData, int i)
{ 
  return REF_FLT(WorldRayOrigin[i]);
}

void fb_Fallback_SetWorldRayOrigin(inout RuntimeData runtimeData, float x, float y, float z)
{ 
  REF_FLT(WorldRayOrigin[0]) = x;
  REF_FLT(WorldRayOrigin[1]) = y;
  REF_FLT(WorldRayOrigin[2]) = z;
}

float fb_dxop_worldRayDirection(inout RuntimeData runtimeData, int i)
{  
  return REF_FLT(WorldRayDirection[i]);
}

float fb_Fallback_WorldRayDirection(inout RuntimeData runtimeData, int i)
{  
  return REF_FLT(WorldRayDirection[i]);
}

void fb_Fallback_SetWorldRayDirection(inout RuntimeData runtimeData, float x, float y, float z)
{ 
  REF_FLT(WorldRayDirection[0]) = x;
  REF_FLT(WorldRayDirection[1]) = y;
  REF_FLT(WorldRayDirection[2]) = z;
}

float fb_dxop_objectRayOrigin(inout RuntimeData runtimeData, int i)
{ 
  return REF_FLT(ObjectRayOrigin[i]);
}

float fb_Fallback_ObjectRayOrigin(inout RuntimeData runtimeData, int i)
{ 
  return REF_FLT(ObjectRayOrigin[i]);
}

void fb_Fallback_SetObjectRayOrigin(inout RuntimeData runtimeData, float x, float y, float z)
{ 
  REF_FLT(ObjectRayOrigin[0]) = x;
  REF_FLT(ObjectRayOrigin[1]) = y;
  REF_FLT(ObjectRayOrigin[2]) = z;
}

float fb_dxop_objectRayDirection(inout RuntimeData runtimeData, int i)
{  
  return REF_FLT(ObjectRayDirection[i]);
}

float fb_Fallback_ObjectRayDirection(inout RuntimeData runtimeData, int i)
{  
  return REF_FLT(ObjectRayDirection[i]);
}

void fb_Fallback_SetObjectRayDirection(inout RuntimeData runtimeData, float x, float y, float z)
{ 
  REF_FLT(ObjectRayDirection[0]) = x;
  REF_FLT(ObjectRayDirection[1]) = y;
  REF_FLT(ObjectRayDirection[2]) = z;
}

float fb_dxop_objectToWorld(inout RuntimeData runtimeData, int r, int c)
{
  int i = r * 4 + c;
  return REF_FLT_OFS(ObjectToWorld, i);
}

void fb_Fallback_SetObjectToWorld(inout RuntimeData runtimeData, float M[12])
{
  REF_FLT_OFS(ObjectToWorld, 0)  = M[0]; 
  REF_FLT_OFS(ObjectToWorld, 1)  = M[1]; 
  REF_FLT_OFS(ObjectToWorld, 2)  = M[2]; 
  REF_FLT_OFS(ObjectToWorld, 3)  = M[3]; 
  REF_FLT_OFS(ObjectToWorld, 4)  = M[4]; 
  REF_FLT_OFS(ObjectToWorld, 5)  = M[5]; 
  REF_FLT_OFS(ObjectToWorld, 6)  = M[6]; 
  REF_FLT_OFS(ObjectToWorld, 7)  = M[7]; 
  REF_FLT_OFS(ObjectToWorld, 8)  = M[8]; 
  REF_FLT_OFS(ObjectToWorld, 9)  = M[9]; 
  REF_FLT_OFS(ObjectToWorld, 10) = M[10];
  REF_FLT_OFS(ObjectToWorld, 11) = M[11];
}

float fb_dxop_worldToObject(inout RuntimeData runtimeData, int r, int c)
{
  int i = r * 4 + c;
  return REF_FLT_OFS(WorldToObject, i);
}

void fb_Fallback_SetWorldToObject(inout RuntimeData runtimeData, float M[12])
{
  REF_FLT_OFS(WorldToObject, 0)  = M[0]; 
  REF_FLT_OFS(WorldToObject, 1)  = M[1]; 
  REF_FLT_OFS(WorldToObject, 2)  = M[2]; 
  REF_FLT_OFS(WorldToObject, 3)  = M[3]; 
  REF_FLT_OFS(WorldToObject, 4)  = M[4]; 
  REF_FLT_OFS(WorldToObject, 5)  = M[5]; 
  REF_FLT_OFS(WorldToObject, 6)  = M[6]; 
  REF_FLT_OFS(WorldToObject, 7)  = M[7]; 
  REF_FLT_OFS(WorldToObject, 8)  = M[8]; 
  REF_FLT_OFS(WorldToObject, 9)  = M[9]; 
  REF_FLT_OFS(WorldToObject, 10) = M[10];
  REF_FLT_OFS(WorldToObject, 11) = M[11];
}

uint fb_dxop_primitiveID(inout RuntimeData runtimeData)
//uint fb_dxop_primitiveIndex(inout RuntimeData runtimeData)
{
  return REF(PrimitiveIndex);
}

uint fb_Fallback_PrimitiveIndex(inout RuntimeData runtimeData)
{
  return REF(PrimitiveIndex);
}

void fb_Fallback_SetPrimitiveIndex(inout RuntimeData runtimeData, uint i)
{
  REF(PrimitiveIndex) = i;
}

uint fb_Fallback_ShaderRecordOffset(inout RuntimeData runtimeData)
{
  return REF(ShaderRecordOffset);
}

void fb_Fallback_SetShaderRecordOffset(inout RuntimeData runtimeData, uint shaderRecordOffset)
{
  REF(ShaderRecordOffset) = shaderRecordOffset;
}

uint fb_dxop_instanceIndex(inout RuntimeData runtimeData)
{
  return REF(InstanceIndex);
}

uint fb_Fallback_InstanceIndex(inout RuntimeData runtimeData)
{
  return REF(InstanceIndex);
}

void fb_Fallback_SetInstanceIndex(inout RuntimeData runtimeData, uint i)
{
  REF(InstanceIndex) = i;
}

uint fb_dxop_instanceID(inout RuntimeData runtimeData)
{
  return REF(InstanceID);
}

uint fb_Fallback_InstanceID(inout RuntimeData runtimeData)
{
  return REF(InstanceID);
}

void fb_Fallback_SetInstanceID(inout RuntimeData runtimeData, uint i)
{
  REF(InstanceID) = i;
}

uint fb_dxop_hitKind(inout RuntimeData runtimeData)
{
  return REF(HitKind);
}

uint fb_Fallback_HitKind(inout RuntimeData runtimeData)
{
  return REF(HitKind);
}

void fb_Fallback_SetHitKind(inout RuntimeData runtimeData, uint i)
{
  REF(HitKind) = i;
}

float fb_dxop_pending_rayTCurrent(inout RuntimeData runtimeData)
{
  return REF_FLT(PendingRayTCurrent);
}

void fb_Fallback_SetPendingRayTCurrent(inout RuntimeData runtimeData, float t)
{
  REF_FLT(PendingRayTCurrent) = t;
}

uint fb_dxop_pending_primitiveID(inout RuntimeData runtimeData)
//uint fb_dxop_pending_primitiveIndex(inout RuntimeData runtimeData)
{
  return REF(PendingPrimitiveIndex);
}

uint fb_Fallback_PendingShaderRecordOffset(inout RuntimeData runtimeData)
{
  return REF(PendingShaderRecordOffset);
}

uint fb_dxop_pending_instanceIndex(inout RuntimeData runtimeData)
{
  return REF(PendingInstanceIndex);
}

uint fb_dxop_pending_instanceID(inout RuntimeData runtimeData)
{
  return REF(PendingInstanceID);
}

uint fb_dxop_pending_hitKind(inout RuntimeData runtimeData)
{
  return REF(PendingHitKind);
}

void fb_Fallback_SetPendingHitKind(inout RuntimeData runtimeData, uint i)
{
  REF(PendingHitKind) = i;
}

uint fb_Fallback_GroupIndex(inout RuntimeData runtimeData)
{ 
  return uint(REF(GroupIndex));
}

int fb_Fallback_AnyHitResult(inout RuntimeData runtimeData)
{
  return REF(AnyHitResult);
}

void fb_Fallback_SetAnyHitResult(inout RuntimeData runtimeData, int result)
{
  REF(AnyHitResult) = result;
}

int fb_Fallback_AnyHitStateId(inout RuntimeData runtimeData)
{
  return REF(AnyHitStateId);
}

void fb_Fallback_SetAnyHitStateId(inout RuntimeData runtimeData, int id)
{
  REF(AnyHitStateId) = id;
}
