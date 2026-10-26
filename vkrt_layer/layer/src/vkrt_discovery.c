/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vkrt_layer.h"
#include "vkrt_rt_capabilities.h"

/* A conservative mask for the current AS/RQ/RTP prototype. This deliberately adds
 * no extension or base feature on a no-RT driver: full emulation discovery is
 * gated on the complete mandatory functionality/limits, not this component. */
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
    }
    if (queried) inst->vtable.GetPhysicalDeviceFeatures2(physical, features);
    for (VkBaseOutStructure *p = features->pNext; p; p = p->pNext) {
        size_t offset, bytes;
        if (vkrt_unsupported_rt_feature_range(p->sType, &offset, &bytes)) memset((char *)p + offset, 0, bytes);
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice physical, const char *layer,
                                                                         uint32_t *count, VkExtensionProperties *properties) {
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    /* Deferred operations are completed synchronously and need no native RT.
     * Base AS/RQ/RTP advertisement still retains native driver truth until
     * the complete mandatory path can support no-native-RT discovery. */
    if (layer && !strcmp(layer, "VK_LAYER_VKRT_SOFTWARE_RAY_QUERY")) {
        if (!properties) {
            *count = 1;
            return VK_SUCCESS;
        }
        if (!*count) return VK_INCOMPLETE;
        properties[0] =
            (VkExtensionProperties){VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_SPEC_VERSION};
        *count = 1;
        return VK_SUCCESS;
    }
    struct vkrt_instance_data *inst = VKRT_FIND(struct vkrt_instance_data, physical);
    if (!inst || !inst->vtable.EnumerateDeviceExtensionProperties) return VK_ERROR_INITIALIZATION_FAILED;
    if (layer) return inst->vtable.EnumerateDeviceExtensionProperties(physical, layer, count, properties);

    VkExtensionProperties *native = NULL;
    uint32_t native_count = 0;
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    /* Device extensions are stable for a physical device. Retry an observed
     * count/fill race, while bounding a broken downstream's endless INCOMPLETE. */
    for (uint32_t attempt = 0; attempt < 8; ++attempt) {
        result = inst->vtable.EnumerateDeviceExtensionProperties(physical, NULL, &native_count, NULL);
        if (result != VK_SUCCESS) return result;
        if ((uint64_t)native_count > SIZE_MAX / sizeof(*native)) return VK_ERROR_OUT_OF_HOST_MEMORY;
        native = malloc((native_count ? (size_t)native_count : 1) * sizeof(*native));
        if (!native) return VK_ERROR_OUT_OF_HOST_MEMORY;
        result = inst->vtable.EnumerateDeviceExtensionProperties(physical, NULL, &native_count, native);
        if (result == VK_SUCCESS) break;
        free(native);
        native = NULL;
        if (result != VK_INCOMPLETE) return result;
    }
    if (!native) return VK_ERROR_INITIALIZATION_FAILED;

    const uint32_t capacity = properties ? *count : 0;
    uint32_t visible = 0, written = 0;
    bool deferred = false;
    for (uint32_t i = 0; i < native_count; ++i) {
        if (vkrt_incompatible_device_extension(native[i].extensionName)) continue;
        if (!strcmp(native[i].extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)) {
            deferred = true;
            native[i].specVersion = VK_KHR_DEFERRED_HOST_OPERATIONS_SPEC_VERSION;
        }
        if (properties && written < capacity) properties[written++] = native[i];
        ++visible;
    }
    if (!deferred) {
        if (properties && written < capacity)
            properties[written++] = (VkExtensionProperties){VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
                                                            VK_KHR_DEFERRED_HOST_OPERATIONS_SPEC_VERSION};
        ++visible;
    }
    free(native);
    *count = properties ? written : visible;
    return properties && written < visible ? VK_INCOMPLETE : VK_SUCCESS;
}

PFN_vkVoidFunction vkrt_discovery_proc(const char *name) {
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2") || !strcmp(name, "vkGetPhysicalDeviceFeatures2KHR"))
        return (PFN_vkVoidFunction)GetPhysicalDeviceFeatures2;
    if (!strcmp(name, "vkEnumerateDeviceExtensionProperties")) return (PFN_vkVoidFunction)EnumerateDeviceExtensionProperties;
    return NULL;
}
