/* SPDX-License-Identifier: MIT
 *
 * vkrt_spirv.c - classify application SPIR-V that this layer cannot forward.
 *
 * The delivered contract is VK_KHR_acceleration_structure plus
 * VK_KHR_ray_tracing_pipeline. Modules that reach the layer's ordinary
 * vkCreateShaderModule path must therefore be ordinary graphics/compute
 * modules: Ray Query is not advertised and has no lowering path, and RT
 * pipeline shaders are owned by the typed compute runtime through
 * vkrt_rtp_create_shader. Either construct is rejected explicitly instead of
 * being forwarded to an ICD whose capabilities may differ from ours.
 *
 * The scan is intentionally a classification pass: it never rewrites the
 * application's module.
 */

#include "vkrt_layer.h"

/* SPIR-V opcodes we care about. Values from the SPIR-V registry
 * (spirv.core.grammar.json); kept local so we do not need SPIRV-Headers. */
enum {
    SpvOpEntryPoint = 15,
    SpvOpTypeAccelerationStructureKHR = 5341,
    SpvOpTypeRayQueryKHR = 4472,
    SpvOpRayQueryInitializeKHR = 4473,
    SpvOpRayQueryTerminateKHR = 4474,
    SpvOpRayQueryGenerateIntersectionKHR = 4475,
    SpvOpRayQueryConfirmIntersectionKHR = 4476,
    SpvOpRayQueryProceedKHR = 4477,
    SpvOpRayQueryGetIntersectionTypeKHR = 4479,
    SpvOpTraceRayKHR = 4445,
    SpvOpTraceRayMotionNV = 5088,
    SpvOpExecuteCallableKHR = 4446,
    SpvOpConvertUToAccelerationStructureKHR = 4447,
    SpvOpIgnoreIntersectionKHR = 4448,
    SpvOpTerminateRayKHR = 4449,
    SpvOpReportIntersectionKHR = 5334,
};

/* Opcodes that belong to the RT pipeline surface. */
static bool spv_is_rtp_op(uint32_t op) {
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

/* The Ray Query instruction family is NOT contiguous: the core ops are
 * 4472-4479, TriangleVertexPositions is 5340, and the
 * VK_KHR_ray_tracing_maintenance1 accessors are 6016-6032. */
static bool spv_is_ray_query_op(uint32_t op) {
    if (op == SpvOpTypeRayQueryKHR) return true;
    if (op >= 4473 && op <= 4479) /* Initialize..GetIntersectionType */
        return true;
    if (op == 5340) /* GetIntersectionTriangleVertexPositions */
        return true;
    if (op >= 6016 && op <= 6032) /* maintenance1 accessors */
        return true;
    return false;
}

/* Execution models that belong to the ray-tracing pipeline. */
enum {
    SpvExecutionModelRayGenerationKHR = 5313,
    SpvExecutionModelIntersectionKHR = 5314,
    SpvExecutionModelAnyHitKHR = 5315,
    SpvExecutionModelClosestHitKHR = 5316,
    SpvExecutionModelMissKHR = 5317,
    SpvExecutionModelCallableKHR = 5318,
};

static bool spv_is_rtp_execution_model(uint32_t model) {
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

VkResult vkrt_rewrite_shader_module(struct vkrt_device_data *dev, const VkShaderModuleCreateInfo *ci,
                                    VkShaderModuleCreateInfo *out_ci, bool *uses_ray_query, bool *uses_ray_tracing_pipeline) {
    *uses_ray_query = false;
    *uses_ray_tracing_pipeline = false;
    *out_ci = *ci;
    if (!dev->as_registry) return VK_SUCCESS;

    const uint32_t *words = ci->pCode;
    const size_t count = ci->codeSize / 4;

    if (count < 5 || words[0] != 0x07230203u) {
        VKRT_LOG_ERR("CreateShaderModule: not a SPIR-V module");
        return VK_ERROR_INVALID_SHADER_NV;
    }

    bool saw_rq = false;
    bool saw_rtp = false;

    for (size_t i = 5; i < count;) {
        uint32_t w0 = words[i];
        uint32_t op = SPV_OPCODE(w0);
        uint32_t len = SPV_LENGTH(w0);
        if (len == 0 || i + len > count) {
            VKRT_LOG_ERR("CreateShaderModule: corrupt SPIR-V instruction at word %zu", i);
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
            if (spv_is_rtp_execution_model(words[i + 1])) saw_rtp = true;
        }

        if (saw_rtp) {
            *uses_ray_tracing_pipeline = true;
            VKRT_LOG_ERR(
                "shader module uses ray-tracing pipeline constructs and "
                "was not captured by the pipeline shader path");
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        if (saw_rq) break;

        i += len;
    }

    *uses_ray_query = saw_rq;
    if (saw_rq) {
        /* Ray Query is not advertised by this layer and no longer lowered in
         * the consuming stage. Never forward the module to an unrelated ICD. */
        VKRT_LOG_ERR(
            "ray query is not supported by this layer; the shader "
            "module contains OpRayQuery*/OpTypeRayQueryKHR");
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    return VK_SUCCESS;
}
