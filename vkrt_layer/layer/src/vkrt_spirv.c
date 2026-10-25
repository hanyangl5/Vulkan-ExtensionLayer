/* SPDX-License-Identifier: MIT
 *
 * vkrt_spirv.c - SPIR-V inspection and rewrite of ray-query ops.
 *
 * Design doc section 11.1 contract:
 *   in : compute/fragment SPIR-V with OpTypeAccelerationStructureKHR,
 *        OpTypeRayQueryKHR, OpRayQuery*KHR
 *   out: ordinary SPIR-V with no KHR AS/RayQuery types or instructions,
 *        containing the layer-injected traversal functions and
 *        custom-BVH buffer accesses.
 *
 *   OpTraceRayKHR and raygen/hit/miss/callable execution models ->
 *   report unsupported immediately, never forwarded to the ICD.
 *
 * This file performs detection and fail-fast checks. The validated
 * instruction rewrite lives in vkrt_spirv_lower.cpp.
 */

#include "vkrt_layer.h"
#include "vkrt_spirv_lower.h"

#include "sw_ray_query.spv.h"
#define vkrt_traversal_words vkrt_spv_traversal
#define vkrt_traversal_word_count VKRT_ARRAY_SIZE(vkrt_spv_traversal)

/* SPIR-V opcodes we care about. Values from the SPIR-V registry
 * (spirv.core.grammar.json); kept local so we do not need SPIRV-Headers. */
enum {
   SpvOpNop                     = 0,
   SpvOpUndef                   = 1,
   SpvOpName                    = 5,
   SpvOpMemberName              = 6,
   SpvOpExtInstImport           = 11,
   SpvOpExtInst                 = 12,
   SpvOpMemoryModel             = 14,
   SpvOpEntryPoint              = 15,
   SpvOpExecutionMode           = 16,
   SpvOpCapability              = 17,
   SpvOpTypeVoid                = 19,
   SpvOpTypeBool                = 20,
   SpvOpTypeInt                 = 21,
   SpvOpTypeFloat               = 22,
   SpvOpTypeVector              = 23,
   SpvOpTypeMatrix              = 24,
   SpvOpTypeArray               = 28,
   SpvOpTypeStruct              = 30,
   SpvOpTypePointer             = 32,
   SpvOpTypeFunction            = 33,
   SpvOpConstant                = 43,
   SpvOpFunction                = 54,
   SpvOpFunctionEnd             = 56,
   SpvOpVariable                = 59,
   SpvOpLoad                    = 61,
   SpvOpStore                   = 62,
   SpvOpAccessChain             = 65,
   SpvOpDecorate                 = 71,
   SpvOpMemberDecorate           = 72,
   SpvOpTypeAccelerationStructureKHR = 5341,
   SpvOpTypeRayQueryKHR          = 4472,
   SpvOpRayQueryInitializeKHR    = 4473,
   SpvOpRayQueryTerminateKHR     = 4474,
   SpvOpRayQueryGenerateIntersectionKHR = 4475,
   SpvOpRayQueryConfirmIntersectionKHR  = 4476,
   SpvOpRayQueryProceedKHR       = 4477,
   SpvOpRayQueryGetIntersectionTypeKHR  = 4479,
   SpvOpTraceRayKHR              = 4445,
   SpvOpTraceRayMotionNV         = 5088,
   SpvOpExecuteCallableKHR      = 4446,
   SpvOpConvertUToAccelerationStructureKHR = 4447,
   SpvOpIgnoreIntersectionKHR    = 4448,
   SpvOpTerminateRayKHR          = 4449,
   SpvOpReportIntersectionKHR    = 5334,
};

/* Opcodes we refuse to forward (ray-tracing pipeline surface). */
static bool
spv_is_rtp_op(uint32_t op)
{
   switch (op) {
   case SpvOpTraceRayKHR:
   case SpvOpTraceRayMotionNV:
   case SpvOpExecuteCallableKHR:
   case SpvOpIgnoreIntersectionKHR:
   case SpvOpTerminateRayKHR:
   case SpvOpReportIntersectionKHR:
   case 5385: /* OpHitObjectTraceRayNV */
   case 5386: /* OpHitObjectTraceRayMotionNV */
   case 5387: /* OpHitObjectRecordShaderBindingTableRecordOffsetNV */
   case 5388: /* OpHitObjectRecordEmptyNV */
   case 5389: /* OpHitObjectRecordHitMotionNV */
   case 5390: /* OpHitObjectRecordHitNV */
   case 5391: /* OpHitObjectRecordHitWithIndexMotionNV */
   case 5392: /* OpHitObjectRecordHitWithIndexNV */
   case 5393: /* OpHitObjectRecordMissMotionNV */
   case 5394: /* OpHitObjectRecordMissNV */
   case 5395: /* OpHitObjectGetShaderRecordBufferHandleNV */
   case 5396: /* OpHitObjectGetShaderBindingTableRecordIndexNV */
   case 5397: /* OpHitObjectGetObjectRayOriginNV */
   case 5398: /* OpHitObjectGetObjectToWorldNV */
   case 5399: /* OpHitObjectGetWorldToObjectNV */
   case 5400: /* OpHitObjectGetWorldRayOriginNV */
   case 5401: /* OpHitObjectGetWorldRayDirectionNV */
   case 5402: /* OpHitObjectGetTriangleVertexPositionsNV */
   case 5403: /* OpHitObjectGetInstanceCustomIndexNV */
   case 5404: /* OpHitObjectGetInstanceIdNV */
   case 5405: /* OpHitObjectGetGeometryIndexNV */
   case 5406: /* OpHitObjectGetPrimitiveIndexNV */
   case 5407: /* OpHitObjectGetHitKindNV */
   case 5408: /* OpHitObjectGetRayTMinNV */
   case 5409: /* OpHitObjectGetRayTNV */
   case 5411: /* OpHitObjectGetAttributesNV */
   case 5412: /* OpHitObjectGetCurrentTimeNV */
   case 5337: /* OpExecuteCallableNV */
      return true;
   default:
      return false;
   }
}

/* Ray-query ops we will rewrite. The family is NOT contiguous: the core
 * ops are 4472-4479, TriangleVertexPositions is 5340, and the
 * VK_KHR_ray_tracing_maintenance1 accessors are 6016-6032. */
static bool
spv_is_ray_query_op(uint32_t op)
{
   if (op == SpvOpTypeRayQueryKHR)
      return true;
   if (op >= 4473 && op <= 4479)   /* Initialize..GetIntersectionType */
      return true;
   if (op == 5340)                 /* GetIntersectionTriangleVertexPositions */
      return true;
   if (op >= 6016 && op <= 6032)   /* maintenance1 accessors */
      return true;
   return false;
}

/* Execution models that belong to the ray-tracing pipeline. */
enum {
   SpvExecutionModelVertex                = 0,
   SpvExecutionModelTessellationControl   = 1,
   SpvExecutionModelTessellationEvaluation = 2,
   SpvExecutionModelGeometry              = 3,
   SpvExecutionModelFragment              = 4,
   SpvExecutionModelGLCompute             = 5,
   SpvExecutionModelKernel                = 6,
   SpvExecutionModelTaskNV                = 5267,
   SpvExecutionModelMeshNV                = 5268,
   SpvExecutionModelRayGenerationKHR      = 5313,
   SpvExecutionModelIntersectionKHR       = 5314,
   SpvExecutionModelAnyHitKHR             = 5315,
   SpvExecutionModelClosestHitKHR         = 5316,
   SpvExecutionModelMissKHR               = 5317,
   SpvExecutionModelCallableKHR           = 5318,
   SpvExecutionModelTaskEXT               = 5364,
   SpvExecutionModelMeshEXT               = 5365,
};

static bool
spv_is_rtp_execution_model(uint32_t model)
{
   switch (model) {
   case SpvExecutionModelRayGenerationKHR:
   case SpvExecutionModelIntersectionKHR:
   case SpvExecutionModelAnyHitKHR:
   case SpvExecutionModelClosestHitKHR:
   case SpvExecutionModelMissKHR:
   case SpvExecutionModelCallableKHR:
      return true;
   default:
      return false;
   }
}

/* SPIR-V instruction word 0: length in high 16, opcode in low 16. */
#define SPV_OPCODE(w) ((w) & 0xFFFFu)
#define SPV_LENGTH(w) ((w) >> 16)

/* Embedded with the DLL, independent of the application's working directory. */
static bool vkrt_load_traversal_library(void) { return true; }

VkResult
vkrt_rewrite_shader_module(struct vkrt_device_data *dev,
                           const VkShaderModuleCreateInfo *ci,
                           VkShaderModuleCreateInfo *out_ci,
                           bool *uses_ray_query,
                           bool *uses_ray_tracing_pipeline)
{
   *uses_ray_query             = false;
   *uses_ray_tracing_pipeline  = false;
   *out_ci                     = *ci;
   (void)dev;

   const uint32_t *words = ci->pCode;
   const size_t    count = ci->codeSize / 4;

   if (count < 5 || words[0] != 0x07230203u) {
      VKRT_LOG_ERR("CreateShaderModule: not a SPIR-V module");
      return VK_ERROR_INVALID_SHADER_NV;
   }

   bool saw_rq = false;
   bool saw_rtp = false;

   for (size_t i = 5; i < count;) {
      uint32_t w0   = words[i];
      uint32_t op   = SPV_OPCODE(w0);
      uint32_t len  = SPV_LENGTH(w0);
      if (len == 0 || i + len > count) {
         VKRT_LOG_ERR("CreateShaderModule: corrupt SPIR-V instruction at word %zu",
                      i);
         return VK_ERROR_INVALID_SHADER_NV;
      }

      if (op == SpvOpTypeRayQueryKHR || op == SpvOpTypeAccelerationStructureKHR) {
         saw_rq = true;
      } else if (spv_is_ray_query_op(op)) {
         saw_rq = true;
      } else if (spv_is_rtp_op(op)) {
         saw_rtp = true;
      } else if (op == SpvOpEntryPoint && len >= 4) {
         /* OpEntryPoint <execution model> <entry point> "name" ... */
         if (spv_is_rtp_execution_model(words[i + 1]))
            saw_rtp = true;
      }

      if (saw_rtp)
         break;

      i += len;
   }

   *uses_ray_query            = saw_rq;
   *uses_ray_tracing_pipeline = saw_rtp;

   if (saw_rtp) {
      VKRT_LOG_ERR(
         "shader module uses VK_KHR_ray_tracing_pipeline constructs "
         "(OpTraceRayKHR / raygen|miss|hit|callable execution model). "
         "This layer implements VK_KHR_ray_query only.");
      return VK_ERROR_FEATURE_NOT_PRESENT;
   }

   if (!saw_rq) {
      /* Nothing to rewrite; forward the module unchanged. */
      return VK_SUCCESS;
   }

   /* ------------------------------------------------------------------
    * Lower OpRayQuery*KHR to OpFunctionCall into the software traversal
    * (design doc section 11.1). The rewritten module has no KHR
    * ray-query types or instructions left.
    * ------------------------------------------------------------------ */
   if (!vkrt_load_traversal_library()) {
      VKRT_LOG_ERR("ray-query shader seen but the traversal library is "
                   "unavailable; cannot lower");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   uint32_t *lowered = NULL;
   size_t    lowered_words = 0;
   char      err[512];
   int rc = vkrt_spirv_lower(words, count,
                            vkrt_traversal_words, vkrt_traversal_word_count,
                            &lowered, &lowered_words,
                            err, sizeof(err));
   if (rc != 0) {
      VKRT_LOG_ERR("SPIR-V ray-query lowering failed: %s", err);
      return VK_ERROR_INVALID_SHADER_NV;
   }

   out_ci->pCode   = lowered;
   out_ci->codeSize = lowered_words * 4;

   VKRT_LOG_DBG("lowered ray-query module: %zu -> %zu words",
                count, lowered_words);
   return VK_SUCCESS;
}
