/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vkrt_layer.h"
#include "vkrt_rt_capabilities.h"
#include "vkrt_resource_profile.h"
#include "vkrt_software_profile.h"
#include "bvh_vertex_format.h"
#include "vkrt_shader_objects.h"

static void clear_rt_properties(VkPhysicalDeviceProperties2 *properties) {
    for (VkBaseOutStructure *p = properties->pNext; p; p = p->pNext) {
        size_t offset = 0, size = 0;
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR) {
            offset = offsetof(VkPhysicalDeviceAccelerationStructurePropertiesKHR, maxGeometryCount);
            size = sizeof(VkPhysicalDeviceAccelerationStructurePropertiesKHR);
        } else if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR) {
            offset = offsetof(VkPhysicalDeviceRayTracingPipelinePropertiesKHR, shaderGroupHandleSize);
            size = sizeof(VkPhysicalDeviceRayTracingPipelinePropertiesKHR);
        }
        if (size) memset((char *)p + offset, 0, size - offset);
    }
}
static void apply_properties(struct vkrt_instance_data *inst, VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *properties) {
    struct vkrt_software_rt_profile profile;
    if (vkrt_virtual_rt_profile(inst, physical, &profile))
        vkrt_apply_software_rt_profile(&profile, properties);
    else
        clear_rt_properties(properties);
}
static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties(VkPhysicalDevice physical, VkPhysicalDeviceProperties *properties) {
    struct vkrt_instance_data *inst = VKRT_FIND(struct vkrt_instance_data, physical);
    if (!inst || !inst->vtable.GetPhysicalDeviceProperties) return;
    inst->vtable.GetPhysicalDeviceProperties(physical, properties);
    VkPhysicalDeviceProperties2 full = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .properties = *properties};
    apply_properties(inst, physical, &full);
    *properties = full.properties;
}
static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *properties) {
    /* Compose with the shader-object UUID wrapper, rather than shadowing it
     * with a second native Properties2 call that would restore native UUIDs. */
    PFN_vkGetPhysicalDeviceProperties2 query = (void *)vkrt_shader_object_instance_proc("vkGetPhysicalDeviceProperties2");
    clear_rt_properties(properties);
    query(physical, properties);
    struct vkrt_instance_data *inst = VKRT_FIND(struct vkrt_instance_data, physical);
    apply_properties(inst, physical, properties);
}

/* Discovery and device admission use the same software profile. Native RT
 * booleans and ignored extension output structs are not software capability
 * evidence. Optional RT features remain false until implemented. */
static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *features) {
    struct vkrt_instance_data *inst = VKRT_FIND(struct vkrt_instance_data, physical);
    const bool queried = inst && inst->vtable.GetPhysicalDeviceFeatures2;
    /* Older ICDs may ignore an unknown extension struct instead of writing its
     * feature. Never mistake the caller's previous output bytes for support. */
    for (VkBaseOutStructure *p = features->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR)
            ((VkPhysicalDeviceAccelerationStructureFeaturesKHR *)p)->accelerationStructure = VK_FALSE;
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR)
            ((VkPhysicalDeviceRayQueryFeaturesKHR *)p)->rayQuery = VK_FALSE;
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR)
            ((VkPhysicalDeviceRayTracingPipelineFeaturesKHR *)p)->rayTracingPipeline = VK_FALSE;
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PER_STAGE_DESCRIPTOR_SET_FEATURES_NV) {
            VkPhysicalDevicePerStageDescriptorSetFeaturesNV *per_stage = (void *)p;
            per_stage->perStageDescriptorSet = per_stage->dynamicPipelineLayout = VK_FALSE;
        }
    }
    if (queried) inst->vtable.GetPhysicalDeviceFeatures2(physical, features);
    struct vkrt_software_rt_profile profile;
    bool eligible = queried && vkrt_virtual_rt_profile(inst, physical, &profile);
    for (VkBaseOutStructure *p = features->pNext; p; p = p->pNext) {
        size_t offset, bytes;
        if (vkrt_unsupported_rt_feature_range(p->sType, &offset, &bytes)) memset((char *)p + offset, 0, bytes);
        if (eligible && vkrt_virtual_object_feature_range(p->sType, &offset, &bytes)) memset((char *)p + offset, 0, bytes);
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR)
            ((VkPhysicalDeviceAccelerationStructureFeaturesKHR *)p)->accelerationStructure = eligible;
        /* Ray query is outside this layer's contract. Its feature stays false
         * even when the backend is eligible, so a module using OpRayQuery* is
         * never advertised as supported. */
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR)
            ((VkPhysicalDeviceRayQueryFeaturesKHR *)p)->rayQuery = VK_FALSE;
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR)
            ((VkPhysicalDeviceRayTracingPipelineFeaturesKHR *)p)->rayTracingPipeline = eligible;
        /* A flat compute module cannot preserve independent per-stage binding
         * namespaces. The neighboring dynamicPipelineLayout feature has its
         * own implementation and must retain native truth. */
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PER_STAGE_DESCRIPTOR_SET_FEATURES_NV)
            ((VkPhysicalDevicePerStageDescriptorSetFeaturesNV *)p)->perStageDescriptorSet = VK_FALSE;
    }
}

static const VkExtensionProperties software_extensions[] = {
    {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_ACCELERATION_STRUCTURE_SPEC_VERSION},
    {VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, VK_KHR_RAY_TRACING_PIPELINE_SPEC_VERSION},
    {VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME, VK_KHR_PIPELINE_LIBRARY_SPEC_VERSION},
    {VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_SPEC_VERSION},
};
static bool software_extension(const char *name) {
    for (size_t i = 0; i < VKRT_ARRAY_SIZE(software_extensions); ++i)
        if (!strcmp(name, software_extensions[i].extensionName)) return true;
    return false;
}
static VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice physical, const char *layer,
                                                                         uint32_t *count, VkExtensionProperties *properties) {
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    struct vkrt_instance_data *inst = VKRT_FIND(struct vkrt_instance_data, physical);
    struct vkrt_software_rt_profile profile;
    bool eligible = inst && vkrt_virtual_rt_profile(inst, physical, &profile);
    /* The first three names need the software backend; the trailing deferred
     * host operations dependency is advertised on every device. */
    const uint32_t first = eligible ? 0 : 3;
    const uint32_t software_count = (uint32_t)VKRT_ARRAY_SIZE(software_extensions) - first;
    if (layer && !strcmp(layer, "VK_LAYER_VKRT_SOFTWARE_RAY_QUERY")) {
        const uint32_t written = properties && *count < software_count ? *count : software_count;
        if (properties) memcpy(properties, software_extensions + first, written * sizeof(*properties));
        *count = written;
        return properties && written < software_count ? VK_INCOMPLETE : VK_SUCCESS;
    }
    if (!inst || !inst->vtable.EnumerateDeviceExtensionProperties) return VK_ERROR_INITIALIZATION_FAILED;
    if (layer) return inst->vtable.EnumerateDeviceExtensionProperties(physical, layer, count, properties);
    VkExtensionProperties *native = NULL;
    uint32_t native_count = 0;
    for (uint32_t attempt = 0; attempt < 8; ++attempt) {
        VkResult result = inst->vtable.EnumerateDeviceExtensionProperties(physical, NULL, &native_count, NULL);
        if (result != VK_SUCCESS) return result;
        if ((uint64_t)native_count > SIZE_MAX / sizeof(*native) || native_count > UINT32_MAX - software_count)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        const uint32_t capacity = native_count;
        native = malloc((native_count ? (size_t)native_count : 1) * sizeof(*native));
        if (!native) return VK_ERROR_OUT_OF_HOST_MEMORY;
        result = inst->vtable.EnumerateDeviceExtensionProperties(physical, NULL, &native_count, native);
        if (result == VK_SUCCESS && native_count <= capacity) break;
        free(native);
        native = NULL;
        if (result != VK_INCOMPLETE) return result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result;
    }
    if (!native) return VK_ERROR_INITIALIZATION_FAILED;
    const uint32_t capacity = properties ? *count : 0;
    uint32_t visible = 0, written = 0;
    for (uint32_t i = 0; i < native_count; ++i) {
        /* A qualified software RTP profile owns its pipeline-library name too.
         * Otherwise preserve native library support used by ordinary GPL. */
        bool owned = software_extension(native[i].extensionName) &&
                     (eligible || strcmp(native[i].extensionName, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME));
        if (owned || vkrt_incompatible_device_extension(native[i].extensionName) ||
            (eligible && (vkrt_virtual_object_extension(native[i].extensionName) ||
                          vkrt_virtual_rt_incompatible_extension(native[i].extensionName))))
            continue;
        if (properties && written < capacity) properties[written++] = native[i];
        ++visible;
    }
    for (uint32_t i = first; i < VKRT_ARRAY_SIZE(software_extensions); ++i) {
        if (properties && written < capacity) properties[written++] = software_extensions[i];
        ++visible;
    }
    free(native);
    *count = properties ? written : visible;
    return properties && written < visible ? VK_INCOMPLETE : VK_SUCCESS;
}

static PFN_vkVoidFunction native_instance_proc(struct vkrt_instance_data *inst, const char *name) {
    return inst && inst->vtable.GetInstanceProcAddr ? inst->vtable.GetInstanceProcAddr(inst->instance, name) : NULL;
}
static bool format_eligible(struct vkrt_instance_data *inst, VkPhysicalDevice physical, VkFormat format) {
    struct vkrt_software_rt_profile profile;
    return bvh_supported_vertex_format(format) && vkrt_virtual_rt_profile(inst, physical, &profile);
}
static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFormatProperties(VkPhysicalDevice physical, VkFormat format,
                                                                    VkFormatProperties *properties) {
    struct vkrt_instance_data *inst = VKRT_FIND(struct vkrt_instance_data, physical);
    PFN_vkGetPhysicalDeviceFormatProperties native = (void *)native_instance_proc(inst, "vkGetPhysicalDeviceFormatProperties");
    *properties = (VkFormatProperties){0};
    if (native) native(physical, format, properties);
    properties->bufferFeatures &= ~VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR;
    if (format_eligible(inst, physical, format))
        properties->bufferFeatures |= VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR;
}
static VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFormatProperties2(VkPhysicalDevice physical, VkFormat format,
                                                                     VkFormatProperties2 *properties) {
    struct vkrt_instance_data *inst = VKRT_FIND(struct vkrt_instance_data, physical);
    PFN_vkGetPhysicalDeviceFormatProperties2 native = (void *)native_instance_proc(inst, "vkGetPhysicalDeviceFormatProperties2");
    if (!native) native = (void *)native_instance_proc(inst, "vkGetPhysicalDeviceFormatProperties2KHR");
    properties->formatProperties = (VkFormatProperties){0};
    for (VkBaseOutStructure *p = properties->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
            VkFormatProperties3 *flags = (void *)p;
            flags->linearTilingFeatures = flags->optimalTilingFeatures = flags->bufferFeatures = 0;
        }
    if (native)
        native(physical, format, properties);
    else {
        PFN_vkGetPhysicalDeviceFormatProperties legacy = (void *)native_instance_proc(inst, "vkGetPhysicalDeviceFormatProperties");
        if (legacy) legacy(physical, format, &properties->formatProperties);
        for (VkBaseOutStructure *p = properties->pNext; p; p = p->pNext)
            if (p->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
                VkFormatProperties3 *flags = (void *)p;
                flags->linearTilingFeatures = properties->formatProperties.linearTilingFeatures;
                flags->optimalTilingFeatures = properties->formatProperties.optimalTilingFeatures;
                flags->bufferFeatures = properties->formatProperties.bufferFeatures;
            }
    }
    const bool eligible = format_eligible(inst, physical, format);
    properties->formatProperties.bufferFeatures &= ~VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR;
    if (eligible) properties->formatProperties.bufferFeatures |= VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR;
    for (VkBaseOutStructure *p = properties->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
            VkFormatProperties3 *flags = (void *)p;
            flags->bufferFeatures &= ~VK_FORMAT_FEATURE_2_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR;
            if (eligible) flags->bufferFeatures |= VK_FORMAT_FEATURE_2_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR;
        }
}

PFN_vkVoidFunction vkrt_discovery_proc(const char *name) {
    if (!strcmp(name, "vkGetPhysicalDeviceProperties")) return (PFN_vkVoidFunction)GetPhysicalDeviceProperties;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2") || !strcmp(name, "vkGetPhysicalDeviceProperties2KHR"))
        return (PFN_vkVoidFunction)GetPhysicalDeviceProperties2;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2") || !strcmp(name, "vkGetPhysicalDeviceFeatures2KHR"))
        return (PFN_vkVoidFunction)GetPhysicalDeviceFeatures2;
    if (!strcmp(name, "vkGetPhysicalDeviceFormatProperties")) return (PFN_vkVoidFunction)GetPhysicalDeviceFormatProperties;
    if (!strcmp(name, "vkGetPhysicalDeviceFormatProperties2") || !strcmp(name, "vkGetPhysicalDeviceFormatProperties2KHR"))
        return (PFN_vkVoidFunction)GetPhysicalDeviceFormatProperties2;
    if (!strcmp(name, "vkEnumerateDeviceExtensionProperties")) return (PFN_vkVoidFunction)EnumerateDeviceExtensionProperties;
    return NULL;
}
