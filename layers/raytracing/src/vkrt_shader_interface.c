/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: MIT
 * Portable shader compilation interfaces. Wire fields are explicit LE words;
 * native handles occur only in the live tracker and materialized objects.
 */
#include "vkrt_shader_interface.h"
#include "vkrt_layer.h"
#include "vkrt_rt_capabilities.h"
#include "vkrt_ycbcr_format_metadata.h"
#include <math.h>

/* Wire v2: 32-byte header (magic/version/length64/setCount/pushCount/
 * sourceRobustnessMask/sourcePushPolicy), push ranges as three u32 words, then layouts.
 * Each 24-byte layout header has length64/sourcePolicy/flags/bindingCount/
 * chainMask. Each binding starts with eight u32 words: binding/type/count/
 * stages/bindingFlags/immutableCount/mutableCount/reserved; inline sampler
 * recipes and mutable type words follow. Sampler recipes are 128 bytes plus
 * an optional 48-byte inline YCbCr conversion, with explicit float bit words.
 * Layout policy maps AS->SSBO and RT stages->COMPUTE. Push policy instead
 * strips RT stages and omits RT-only ranges, matching native shader creation. */
enum {
    INTERFACE_MAGIC = 0x49464b56,
    INTERFACE_VERSION = 2,
    HEADER_BYTES = 32,
    LAYOUT_BYTES = 24,
    BINDING_BYTES = 32,
    SAMPLER_BYTES = 128,
    CONVERSION_BYTES = 48
};
enum { SAMPLER_REDUCTION = 1, SAMPLER_CUSTOM = 2, SAMPLER_MAPPING = 4, SAMPLER_CONVERSION = 8 };
enum { CHAIN_BINDING_FLAGS = 1, CHAIN_MUTABLE = 2 };
struct definition {
    struct definition *next;
    uint64_t handle, references;
    uint8_t *bytes;
    size_t size;
    bool known;
};
struct interface_features {
    VkBool32 anisotropy, ycbcr, mirror_clamp, minmax, inline_uniform, inline_uab, mutable;
    VkBool32 custom_border, custom_without_format, border_swizzle, non_seamless, cubic;
    VkBool32 geometry, tessellation, task, mesh, subgroup_control, full_subgroups;
    VkBool32 shading_rate_attachment, density_map, indexing64;
    VkPhysicalDeviceFeatures core;
    VkPhysicalDeviceVulkan11Features v11;
    VkPhysicalDeviceVulkan12Features v12;
    VkPhysicalDeviceVulkan13Features v13;
    VkBool32 image_int64_atomics, amd_float16, format_flags2;
    uint32_t robustness;
    VkPhysicalDeviceDescriptorIndexingFeatures indexing;
};
struct vkrt_shader_interface_state {
    vkrt_mutex_t lock;
    struct definition *samplers, *conversions, *layouts;
    bool custom;
    VkAllocationCallbacks allocator;
    struct interface_features enabled;
    VkPhysicalDeviceSubgroupSizeControlProperties subgroup;
    bool subgroup_known;
    uint32_t native_api;
    float max_anisotropy, max_lod_bias;
};
struct vkrt_shader_interface {
    VkDescriptorSetLayout *layouts;
    VkPushConstantRange *pushes;
    VkSampler *samplers;
    VkSamplerYcbcrConversion *conversions;
    uint32_t set_count, push_count;
    size_t sampler_count, conversion_count;
    bool custom;
    VkAllocationCallbacks allocator;
};
static const VkAllocationCallbacks *device_allocator(const struct vkrt_device_data *dev) {
    if (dev->queue_families_have_allocator) return &dev->queue_families_allocator;
    return dev->instance_data && dev->instance_data->has_allocator ? &dev->instance_data->allocator : NULL;
}
static void *allocate(const VkAllocationCallbacks *a, size_t size) {
    if (!size) return NULL;
    return a ? a->pfnAllocation(a->pUserData, size, 16, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE) : malloc(size);
}
static void release(const VkAllocationCallbacks *a, void *p) {
    if (!p) return;
    if (a)
        a->pfnFree(a->pUserData, p);
    else
        free(p);
}
static const VkAllocationCallbacks *state_allocator(const struct vkrt_shader_interface_state *s) {
    return s->custom ? &s->allocator : NULL;
}
static const VkAllocationCallbacks *owned_allocator(const struct vkrt_shader_interface *o) {
    return o->custom ? &o->allocator : NULL;
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }
static void put32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i * 8));
}
static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}
static uint32_t float_bits(float v) {
    uint32_t bits;
    memcpy(&bits, &v, 4);
    return bits;
}
static float bits_float(uint32_t bits) {
    float v;
    memcpy(&v, &bits, 4);
    return v;
}
_Static_assert(sizeof(float) == 4, "Vulkan float wire representation");
struct writer {
    const VkAllocationCallbacks *allocator;
    uint8_t *bytes;
    size_t size, capacity;
};
static bool append(struct writer *w, const void *bytes, size_t size) {
    if (size > SIZE_MAX - w->size) return false;
    size_t required = w->size + size;
    if (required > w->capacity) {
        size_t capacity = w->capacity > SIZE_MAX / 2 ? required : w->capacity * 2;
        if (capacity < required) capacity = required;
        uint8_t *p = allocate(w->allocator, capacity);
        if (!p) return false;
        if (w->size) memcpy(p, w->bytes, w->size);
        release(w->allocator, w->bytes);
        w->bytes = p;
        w->capacity = capacity;
    }
    if (size) memcpy(w->bytes + w->size, bytes, size);
    w->size = required;
    return true;
}
static bool append32(struct writer *w, uint32_t v) {
    uint8_t b[4];
    put32(b, v);
    return append(w, b, 4);
}
static struct definition *find(struct definition *head, uint64_t handle) {
    for (; head; head = head->next)
        if (head->handle == handle) return head;
    return NULL;
}
static void free_definition(struct vkrt_shader_interface_state *s, struct definition *d) {
    release(state_allocator(s), d->bytes);
    release(state_allocator(s), d);
}
struct binding_view {
    const uint8_t *header, *samplers, *types, *end;
};
/* Called only on tracker-owned or fully validated bytes. */
static struct binding_view binding_view(const uint8_t *p) {
    struct binding_view b = {p, p + BINDING_BYTES, NULL, NULL};
    p += BINDING_BYTES;
    for (uint32_t i = 0; i < get32(b.header + 20); ++i) p += (size_t)get64(p);
    b.types = p;
    b.end = p + (size_t)get32(b.header + 24) * 4;
    return b;
}
static VkDescriptorType descriptor_type(uint32_t type, bool translate) {
    return translate && type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                                              : (VkDescriptorType)type;
}
static bool equal_layout_native(const uint8_t *a, const uint8_t *b) {
    if (get32(a + 12) != get32(b + 12) || get32(a + 16) != get32(b + 16)) return false;
    bool ta = get32(a + 8) != 0, tb = get32(b + 8) != 0;
    const uint8_t *pa = a + LAYOUT_BYTES;
    for (uint32_t i = 0; i < get32(a + 16); ++i) {
        struct binding_view x = binding_view(pa), y = {0};
        const uint8_t *pb = b + LAYOUT_BYTES;
        for (uint32_t j = 0; j < get32(b + 16); ++j) {
            struct binding_view candidate = binding_view(pb);
            if (get32(candidate.header) == get32(x.header)) {
                y = candidate;
                break;
            }
            pb = candidate.end;
        }
        if (!y.header || get32(x.header + 8) != get32(y.header + 8) || get32(x.header + 16) != get32(y.header + 16) ||
            get32(x.header + 20) != get32(y.header + 20) || get32(x.header + 24) != get32(y.header + 24))
            return false;
        if (get32(x.header + 8) && (descriptor_type(get32(x.header + 4), ta) != descriptor_type(get32(y.header + 4), tb) ||
                                    (ta ? vkrt_compute_shader_stages(get32(x.header + 12)) : get32(x.header + 12)) !=
                                        (tb ? vkrt_compute_shader_stages(get32(y.header + 12)) : get32(y.header + 12))))
            return false;
        size_t sampler_size = (size_t)(x.types - x.samplers);
        if (sampler_size != (size_t)(y.types - y.samplers) || memcmp(x.samplers, y.samplers, sampler_size)) return false;
        for (uint32_t j = 0; j < get32(x.header + 24); ++j) {
            bool present = false;
            for (uint32_t k = 0; k < get32(y.header + 24); ++k)
                present |= descriptor_type(get32(x.types + j * 4), ta) == descriptor_type(get32(y.types + k * 4), tb);
            if (!present) return false;
        }
        pa = x.end;
    }
    return true;
}
static VkResult publish(struct vkrt_shader_interface_state *s, struct definition **head, uint64_t handle, uint8_t *bytes,
                        size_t size, bool known, bool layout) {
    struct definition *old = find(*head, handle);
    if (old) {
        if (old->references == UINT64_MAX) {
            release(state_allocator(s), bytes);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        ++old->references;
        bool equal = old->known && known &&
                     (layout ? equal_layout_native(old->bytes, bytes) : old->size == size && !memcmp(old->bytes, bytes, size));
        if (!equal) {
            old->known = false;
            release(state_allocator(s), old->bytes);
            old->bytes = NULL;
            old->size = 0;
        }
        release(state_allocator(s), bytes);
        return VK_SUCCESS;
    }
    struct definition *d = allocate(state_allocator(s), sizeof(*d));
    if (!d) {
        release(state_allocator(s), bytes);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *d = (struct definition){.next = *head, .handle = handle, .references = 1, .bytes = bytes, .size = size, .known = known};
    *head = d;
    return VK_SUCCESS;
}
static void forget(struct vkrt_shader_interface_state *s, struct definition **head, uint64_t handle) {
    struct definition **p = head;
    while (*p && (*p)->handle != handle) p = &(*p)->next;
    if (*p && --(*p)->references == 0) {
        struct definition *d = *p;
        *p = d->next;
        free_definition(s, d);
    }
}
static void enabled_features(struct interface_features *f, const VkDeviceCreateInfo *ci) {
    if (!ci) return;
    if (ci->pEnabledFeatures) {
        f->core = *ci->pEnabledFeatures;
        f->anisotropy = ci->pEnabledFeatures->samplerAnisotropy;
        f->geometry = ci->pEnabledFeatures->geometryShader;
        f->tessellation = ci->pEnabledFeatures->tessellationShader;
    }
    for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext) {
        switch (p->sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
                f->core = ((const VkPhysicalDeviceFeatures2 *)p)->features;
                f->anisotropy = ((const VkPhysicalDeviceFeatures2 *)p)->features.samplerAnisotropy;
                f->geometry = ((const VkPhysicalDeviceFeatures2 *)p)->features.geometryShader;
                f->tessellation = ((const VkPhysicalDeviceFeatures2 *)p)->features.tessellationShader;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
                f->v11 = *(const VkPhysicalDeviceVulkan11Features *)p;
                f->v11.pNext = NULL;
                f->ycbcr = ((const VkPhysicalDeviceVulkan11Features *)p)->samplerYcbcrConversion;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES: {
                const VkPhysicalDevice16BitStorageFeatures *v = (const void *)p;
                f->v11.storageBuffer16BitAccess = v->storageBuffer16BitAccess;
                f->v11.uniformAndStorageBuffer16BitAccess = v->uniformAndStorageBuffer16BitAccess;
                f->v11.storagePushConstant16 = v->storagePushConstant16;
                f->v11.storageInputOutput16 = v->storageInputOutput16;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES: {
                const VkPhysicalDeviceVariablePointersFeatures *v = (const void *)p;
                f->v11.variablePointersStorageBuffer = v->variablePointersStorageBuffer;
                f->v11.variablePointers = v->variablePointers;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
                f->v11.shaderDrawParameters = ((const VkPhysicalDeviceShaderDrawParametersFeatures *)p)->shaderDrawParameters;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:
                f->v11.multiview = ((const VkPhysicalDeviceMultiviewFeatures *)p)->multiview;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:
                f->ycbcr = ((const VkPhysicalDeviceSamplerYcbcrConversionFeatures *)p)->samplerYcbcrConversion;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
                const VkPhysicalDeviceVulkan12Features *v = (const void *)p;
                f->v12 = *v;
                f->v12.pNext = NULL;
                f->mirror_clamp = v->samplerMirrorClampToEdge;
                f->minmax = v->samplerFilterMinmax;
#define INDEX_FEATURE(name) f->indexing.name = v->name
                INDEX_FEATURE(shaderInputAttachmentArrayDynamicIndexing);
                INDEX_FEATURE(shaderUniformTexelBufferArrayDynamicIndexing);
                INDEX_FEATURE(shaderStorageTexelBufferArrayDynamicIndexing);
                INDEX_FEATURE(shaderUniformBufferArrayNonUniformIndexing);
                INDEX_FEATURE(shaderSampledImageArrayNonUniformIndexing);
                INDEX_FEATURE(shaderStorageBufferArrayNonUniformIndexing);
                INDEX_FEATURE(shaderStorageImageArrayNonUniformIndexing);
                INDEX_FEATURE(shaderInputAttachmentArrayNonUniformIndexing);
                INDEX_FEATURE(shaderUniformTexelBufferArrayNonUniformIndexing);
                INDEX_FEATURE(shaderStorageTexelBufferArrayNonUniformIndexing);
                INDEX_FEATURE(runtimeDescriptorArray);
                INDEX_FEATURE(descriptorBindingUniformBufferUpdateAfterBind);
                INDEX_FEATURE(descriptorBindingSampledImageUpdateAfterBind);
                INDEX_FEATURE(descriptorBindingStorageImageUpdateAfterBind);
                INDEX_FEATURE(descriptorBindingStorageBufferUpdateAfterBind);
                INDEX_FEATURE(descriptorBindingUniformTexelBufferUpdateAfterBind);
                INDEX_FEATURE(descriptorBindingStorageTexelBufferUpdateAfterBind);
                INDEX_FEATURE(descriptorBindingUpdateUnusedWhilePending);
                INDEX_FEATURE(descriptorBindingPartiallyBound);
                INDEX_FEATURE(descriptorBindingVariableDescriptorCount);
#undef INDEX_FEATURE
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
                f->v12.shaderFloat16 = ((const VkPhysicalDeviceShaderFloat16Int8Features *)p)->shaderFloat16;
                f->v12.shaderInt8 = ((const VkPhysicalDeviceShaderFloat16Int8Features *)p)->shaderInt8;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES:
                f->v12.shaderBufferInt64Atomics = ((const VkPhysicalDeviceShaderAtomicInt64Features *)p)->shaderBufferInt64Atomics;
                f->v12.shaderSharedInt64Atomics = ((const VkPhysicalDeviceShaderAtomicInt64Features *)p)->shaderSharedInt64Atomics;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT:
                f->image_int64_atomics = ((const VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT *)p)->shaderImageInt64Atomics;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES: {
                const VkPhysicalDevice8BitStorageFeatures *v = (const void *)p;
                f->v12.storageBuffer8BitAccess = v->storageBuffer8BitAccess;
                f->v12.uniformAndStorageBuffer8BitAccess = v->uniformAndStorageBuffer8BitAccess;
                f->v12.storagePushConstant8 = v->storagePushConstant8;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES: {
                const VkPhysicalDeviceVulkanMemoryModelFeatures *v = (const void *)p;
                f->v12.vulkanMemoryModel = v->vulkanMemoryModel;
                f->v12.vulkanMemoryModelDeviceScope = v->vulkanMemoryModelDeviceScope;
                f->v12.vulkanMemoryModelAvailabilityVisibilityChains = v->vulkanMemoryModelAvailabilityVisibilityChains;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
                f->v12.bufferDeviceAddress = ((const VkPhysicalDeviceBufferDeviceAddressFeatures *)p)->bufferDeviceAddress;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT:
                f->v12.bufferDeviceAddress = ((const VkPhysicalDeviceBufferDeviceAddressFeaturesEXT *)p)->bufferDeviceAddress;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
                f->indexing = *(const VkPhysicalDeviceDescriptorIndexingFeatures *)p;
                f->indexing.pNext = NULL;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
                f->v13 = *(const VkPhysicalDeviceVulkan13Features *)p;
                f->v13.pNext = NULL;
                f->inline_uniform = ((const VkPhysicalDeviceVulkan13Features *)p)->inlineUniformBlock;
                f->inline_uab = ((const VkPhysicalDeviceVulkan13Features *)p)->descriptorBindingInlineUniformBlockUpdateAfterBind;
                f->subgroup_control = ((const VkPhysicalDeviceVulkan13Features *)p)->subgroupSizeControl;
                f->full_subgroups = ((const VkPhysicalDeviceVulkan13Features *)p)->computeFullSubgroups;
                if (((const VkPhysicalDeviceVulkan13Features *)p)->robustImageAccess) f->robustness |= 4u;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES:
                f->v13.shaderDemoteToHelperInvocation =
                    ((const VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures *)p)->shaderDemoteToHelperInvocation;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES:
                f->v13.shaderIntegerDotProduct =
                    ((const VkPhysicalDeviceShaderIntegerDotProductFeatures *)p)->shaderIntegerDotProduct;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES:
                f->v13.maintenance4 = ((const VkPhysicalDeviceMaintenance4Features *)p)->maintenance4;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES:
                if (((const VkPhysicalDeviceImageRobustnessFeatures *)p)->robustImageAccess) f->robustness |= 4u;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
                if (((const VkPhysicalDeviceRobustness2FeaturesEXT *)p)->robustBufferAccess2) f->robustness |= 2u;
                if (((const VkPhysicalDeviceRobustness2FeaturesEXT *)p)->robustImageAccess2) f->robustness |= 8u;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES:
                f->subgroup_control = ((const VkPhysicalDeviceSubgroupSizeControlFeatures *)p)->subgroupSizeControl;
                f->full_subgroups = ((const VkPhysicalDeviceSubgroupSizeControlFeatures *)p)->computeFullSubgroups;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT:
                f->task = ((const VkPhysicalDeviceMeshShaderFeaturesEXT *)p)->taskShader;
                f->mesh = ((const VkPhysicalDeviceMeshShaderFeaturesEXT *)p)->meshShader;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR:
                f->shading_rate_attachment =
                    ((const VkPhysicalDeviceFragmentShadingRateFeaturesKHR *)p)->attachmentFragmentShadingRate;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT:
                f->density_map = ((const VkPhysicalDeviceFragmentDensityMapFeaturesEXT *)p)->fragmentDensityMap;
                break;
#ifdef VK_EXT_shader_64bit_indexing
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_64_BIT_INDEXING_FEATURES_EXT:
                f->indexing64 = ((const VkPhysicalDeviceShader64BitIndexingFeaturesEXT *)p)->shader64BitIndexing;
                break;
#endif
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES:
                f->inline_uniform = ((const VkPhysicalDeviceInlineUniformBlockFeatures *)p)->inlineUniformBlock;
                f->inline_uab =
                    ((const VkPhysicalDeviceInlineUniformBlockFeatures *)p)->descriptorBindingInlineUniformBlockUpdateAfterBind;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT:
                f->mutable = ((const VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT *)p)->mutableDescriptorType;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT:
                f->custom_border = ((const VkPhysicalDeviceCustomBorderColorFeaturesEXT *)p)->customBorderColors;
                f->custom_without_format =
                    ((const VkPhysicalDeviceCustomBorderColorFeaturesEXT *)p)->customBorderColorWithoutFormat;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BORDER_COLOR_SWIZZLE_FEATURES_EXT:
                f->border_swizzle = ((const VkPhysicalDeviceBorderColorSwizzleFeaturesEXT *)p)->borderColorSwizzle;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT:
                f->non_seamless = ((const VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT *)p)->nonSeamlessCubeMap;
                break;
            default:
                break;
        }
    }
    if (f->core.robustBufferAccess) f->robustness |= 1u;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) {
        const char *name = ci->ppEnabledExtensionNames[i];
        if (!strcmp(name, "VK_KHR_sampler_mirror_clamp_to_edge")) f->mirror_clamp = VK_TRUE;
        if (!strcmp(name, "VK_EXT_sampler_filter_minmax")) f->minmax = VK_TRUE;
        if (!strcmp(name, "VK_EXT_filter_cubic") || !strcmp(name, "VK_IMG_filter_cubic")) f->cubic = VK_TRUE;
        if (!strcmp(name, "VK_KHR_shader_draw_parameters")) f->v11.shaderDrawParameters = VK_TRUE;
        if (!strcmp(name, "VK_AMD_gpu_shader_half_float")) f->amd_float16 = VK_TRUE;
        if (!strcmp(name, "VK_KHR_format_feature_flags2")) f->format_flags2 = VK_TRUE;
    }
}
VkResult vkrt_shader_interface_init(struct vkrt_device_data *dev, const VkDeviceCreateInfo *original) {
    if (!dev->app_requested_shader_objects) return VK_SUCCESS;
    const VkAllocationCallbacks *a = device_allocator(dev);
    struct vkrt_shader_interface_state *s = allocate(a, sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(s, 0, sizeof(*s));
    if (a) {
        s->custom = true;
        s->allocator = *a;
    }
    enabled_features(&s->enabled, original);
    /* Conservative core minimums also keep isolated test/diagnostic devices
     * with no physical dispatch from forwarding obviously excessive values. */
    s->max_anisotropy = 16;
    s->max_lod_bias = 2;
    s->native_api = VK_API_VERSION_1_0;
    if (dev->instance_data && dev->instance_data->vtable.GetPhysicalDeviceProperties) {
        VkPhysicalDeviceProperties properties;
        dev->instance_data->vtable.GetPhysicalDeviceProperties(dev->physical_device, &properties);
        s->max_anisotropy = properties.limits.maxSamplerAnisotropy;
        s->max_lod_bias = properties.limits.maxSamplerLodBias;
        s->native_api = properties.apiVersion;
    }
    if (s->enabled.subgroup_control && dev->instance_data && dev->instance_data->vtable.GetPhysicalDeviceProperties2) {
        s->subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES;
        VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &s->subgroup};
        dev->instance_data->vtable.GetPhysicalDeviceProperties2(dev->physical_device, &properties);
        s->subgroup_known = s->subgroup.minSubgroupSize && s->subgroup.maxSubgroupSize;
        s->subgroup.pNext = NULL;
    }
#ifdef _WIN32
    vkrt_mutex_init(&s->lock);
#else
    if (pthread_mutex_init(&s->lock, NULL)) {
        release(a, s);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
#endif
    dev->shader_interfaces = s;
    return VK_SUCCESS;
}
void vkrt_shader_interface_finish(struct vkrt_device_data *dev) {
    struct vkrt_shader_interface_state *s = dev->shader_interfaces;
    if (!s) return;
    struct definition *heads[] = {s->samplers, s->conversions, s->layouts};
    for (unsigned i = 0; i < 3; ++i)
        while (heads[i]) {
            struct definition *next = heads[i]->next;
            free_definition(s, heads[i]);
            heads[i] = next;
        }
#ifndef _WIN32
    pthread_mutex_destroy(&s->lock);
#endif
    release(state_allocator(s), s);
    dev->shader_interfaces = NULL;
}
void vkrt_shader_interface_lock(struct vkrt_device_data *dev) {
    if (dev->shader_interfaces) vkrt_mutex_lock(&dev->shader_interfaces->lock);
}
void vkrt_shader_interface_unlock(struct vkrt_device_data *dev) {
    if (dev->shader_interfaces) vkrt_mutex_unlock(&dev->shader_interfaces->lock);
}

VkResult vkrt_shader_interface_track_conversion_locked(struct vkrt_device_data *dev, VkSamplerYcbcrConversion conversion,
                                                       const VkSamplerYcbcrConversionCreateInfo *info) {
    struct vkrt_shader_interface_state *s = dev->shader_interfaces;
    if (!s) return VK_SUCCESS;
    uint8_t data[CONVERSION_BYTES] = {0};
    bool known = !info->pNext;
    const uint32_t values[] = {info->format,
                               info->ycbcrModel,
                               info->ycbcrRange,
                               info->components.r,
                               info->components.g,
                               info->components.b,
                               info->components.a,
                               info->xChromaOffset,
                               info->yChromaOffset,
                               info->chromaFilter,
                               info->forceExplicitReconstruction};
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(values); ++i) put32(data + i * 4, values[i]);
    uint8_t *bytes = known ? allocate(state_allocator(s), sizeof(data)) : NULL;
    if (known && !bytes) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (known) memcpy(bytes, data, sizeof(data));
    return publish(s, &s->conversions, VKRT_HKEY(conversion), bytes, known ? sizeof(data) : 0, known, false);
}
void vkrt_shader_interface_forget_conversion_locked(struct vkrt_device_data *dev, VkSamplerYcbcrConversion conversion) {
    if (dev->shader_interfaces) forget(dev->shader_interfaces, &dev->shader_interfaces->conversions, VKRT_HKEY(conversion));
}
VkResult vkrt_shader_interface_track_sampler_locked(struct vkrt_device_data *dev, VkSampler sampler,
                                                    const VkSamplerCreateInfo *info) {
    struct vkrt_shader_interface_state *s = dev->shader_interfaces;
    if (!s) return VK_SUCCESS;
    uint8_t data[SAMPLER_BYTES + CONVERSION_BYTES] = {0};
    const uint32_t core[] = {info->flags,
                             info->magFilter,
                             info->minFilter,
                             info->mipmapMode,
                             info->addressModeU,
                             info->addressModeV,
                             info->addressModeW,
                             float_bits(info->mipLodBias),
                             info->anisotropyEnable,
                             float_bits(info->maxAnisotropy),
                             info->compareEnable,
                             info->compareOp,
                             float_bits(info->minLod),
                             float_bits(info->maxLod),
                             info->borderColor,
                             info->unnormalizedCoordinates};
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(core); ++i) put32(data + (3 + i) * 4, core[i]);
    uint32_t mask = 0;
    bool known = true;
    size_t size = SAMPLER_BYTES;
    for (const VkBaseInStructure *p = info->pNext; p; p = p->pNext) {
        uint32_t bit = 0;
        switch (p->sType) {
            case VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO:
                bit = SAMPLER_REDUCTION;
                put32(data + 19 * 4, ((const VkSamplerReductionModeCreateInfo *)p)->reductionMode);
                break;
            case VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT: {
                bit = SAMPLER_CUSTOM;
                const VkSamplerCustomBorderColorCreateInfoEXT *c = (const void *)p;
                for (unsigned i = 0; i < 4; ++i) put32(data + (20 + i) * 4, c->customBorderColor.uint32[i]);
                put32(data + 24 * 4, c->format);
                break;
            }
            case VK_STRUCTURE_TYPE_SAMPLER_BORDER_COLOR_COMPONENT_MAPPING_CREATE_INFO_EXT: {
                bit = SAMPLER_MAPPING;
                const VkSamplerBorderColorComponentMappingCreateInfoEXT *m = (const void *)p;
                put32(data + 25 * 4, m->components.r);
                put32(data + 26 * 4, m->components.g);
                put32(data + 27 * 4, m->components.b);
                put32(data + 28 * 4, m->components.a);
                put32(data + 29 * 4, m->srgb);
                break;
            }
            case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO: {
                bit = SAMPLER_CONVERSION;
                struct definition *c = find(s->conversions, VKRT_HKEY(((const VkSamplerYcbcrConversionInfo *)p)->conversion));
                if (!c || !c->known)
                    known = false;
                else {
                    memcpy(data + SAMPLER_BYTES, c->bytes, CONVERSION_BYTES);
                    size = SAMPLER_BYTES + CONVERSION_BYTES;
                    put32(data + 30 * 4, CONVERSION_BYTES);
                }
                break;
            }
            default:
                known = false;
                break;
        }
        if (mask & bit) known = false;
        mask |= bit;
    }
    put32(data + 8, mask);
    put64(data, size);
    uint8_t *bytes = known ? allocate(state_allocator(s), size) : NULL;
    if (known && !bytes) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (known) memcpy(bytes, data, size);
    return publish(s, &s->samplers, VKRT_HKEY(sampler), bytes, known ? size : 0, known, false);
}
void vkrt_shader_interface_forget_sampler_locked(struct vkrt_device_data *dev, VkSampler sampler) {
    if (dev->shader_interfaces) forget(dev->shader_interfaces, &dev->shader_interfaces->samplers, VKRT_HKEY(sampler));
}
VkResult vkrt_shader_interface_track_layout_locked(struct vkrt_device_data *dev, VkDescriptorSetLayout layout,
                                                   const VkDescriptorSetLayoutCreateInfo *info) {
    struct vkrt_shader_interface_state *s = dev->shader_interfaces;
    if (!s) return VK_SUCCESS;
    const VkDescriptorSetLayoutBindingFlagsCreateInfo *flags = NULL;
    const VkMutableDescriptorTypeCreateInfoEXT *mutable = NULL;
    bool known = true;
    uint32_t chains = 0;
    for (const VkBaseInStructure *p = info->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO && !flags) {
            flags = (const void *)p;
            chains |= CHAIN_BINDING_FLAGS;
        } else if (p->sType == VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT && !mutable) {
            mutable = (const void *)p;
            chains |= CHAIN_MUTABLE;
        } else
            known = false;
    }
    struct writer w = {.allocator = state_allocator(s)};
    uint8_t header[LAYOUT_BYTES] = {0};
    put32(header + 8, dev->virtual_rt);
    put32(header + 12, info->flags);
    put32(header + 16, info->bindingCount);
    put32(header + 20, chains);
    if (known && !append(&w, header, sizeof(header))) goto oom;
    for (uint32_t i = 0; known && i < info->bindingCount; ++i) {
        const VkDescriptorSetLayoutBinding *b = &info->pBindings[i];
        uint32_t immutable_count = b->descriptorCount && b->pImmutableSamplers &&
                                           (b->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                                            b->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
                                       ? b->descriptorCount
                                       : 0;
        const VkMutableDescriptorTypeListEXT *types = mutable && b->descriptorType == VK_DESCRIPTOR_TYPE_MUTABLE_EXT &&
                                                              b->descriptorCount && i < mutable->mutableDescriptorTypeListCount
                                                          ? &mutable->pMutableDescriptorTypeLists[i]
                                                          : NULL;
        uint32_t binding_flags = flags && flags->bindingCount && i < flags->bindingCount ? flags->pBindingFlags[i] : 0;
        const uint32_t values[] = {b->binding,
                                   b->descriptorType,
                                   b->descriptorCount,
                                   b->stageFlags,
                                   binding_flags,
                                   immutable_count,
                                   types ? types->descriptorTypeCount : 0,
                                   0};
        for (unsigned j = 0; j < VKRT_ARRAY_SIZE(values); ++j)
            if (!append32(&w, values[j])) goto oom;
        for (uint32_t j = 0; j < immutable_count; ++j) {
            struct definition *sampler = find(s->samplers, VKRT_HKEY(b->pImmutableSamplers[j]));
            if (!sampler || !sampler->known) {
                known = false;
                break;
            }
            if (!append(&w, sampler->bytes, sampler->size)) goto oom;
        }
        for (uint32_t j = 0; known && types && j < types->descriptorTypeCount; ++j)
            if (!append32(&w, types->pDescriptorTypes[j])) goto oom;
    }
    if (known)
        put64(w.bytes, w.size);
    else {
        release(w.allocator, w.bytes);
        w.bytes = NULL;
        w.size = 0;
    }
    return publish(s, &s->layouts, VKRT_HKEY(layout), w.bytes, w.size, known, true);
oom:
    release(w.allocator, w.bytes);
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}
void vkrt_shader_interface_forget_layout_locked(struct vkrt_device_data *dev, VkDescriptorSetLayout layout) {
    if (dev->shader_interfaces) forget(dev->shader_interfaces, &dev->shader_interfaces->layouts, VKRT_HKEY(layout));
}
VkResult vkrt_shader_interface_capture(struct vkrt_device_data *dev, const VkShaderCreateInfoEXT *info, void **bytes,
                                       size_t *size) {
    *bytes = NULL;
    *size = 0;
    struct vkrt_shader_interface_state *s = dev->shader_interfaces;
    if (!s) return VK_ERROR_FEATURE_NOT_PRESENT;
    struct writer w = {.allocator = state_allocator(s)};
    uint8_t header[HEADER_BYTES] = {0};
    put32(header, INTERFACE_MAGIC);
    put32(header + 4, INTERFACE_VERSION);
    put32(header + 16, info->setLayoutCount);
    put32(header + 20, info->pushConstantRangeCount);
    put32(header + 24, s->enabled.robustness);
    put32(header + 28, dev->virtual_rt);
    VkResult result = VK_SUCCESS;
    vkrt_mutex_lock(&s->lock);
    if (!append(&w, header, sizeof(header))) result = VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; result == VK_SUCCESS && i < info->pushConstantRangeCount; ++i) {
        const VkPushConstantRange *p = &info->pPushConstantRanges[i];
        if (!append32(&w, p->stageFlags) || !append32(&w, p->offset) || !append32(&w, p->size))
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    for (uint32_t i = 0; result == VK_SUCCESS && i < info->setLayoutCount; ++i) {
        struct definition *d = find(s->layouts, VKRT_HKEY(info->pSetLayouts[i]));
        if (!d || !d->known)
            result = VK_ERROR_FEATURE_NOT_PRESENT;
        else if (!append(&w, d->bytes, d->size))
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    vkrt_mutex_unlock(&s->lock);
    if (result != VK_SUCCESS) {
        release(w.allocator, w.bytes);
        return result;
    }
    put64(w.bytes + 8, w.size);
    *bytes = w.bytes;
    *size = w.size;
    return VK_SUCCESS;
}
void vkrt_shader_interface_free_bytes(struct vkrt_device_data *dev, void *bytes) { release(device_allocator(dev), bytes); }

struct recipe_stats {
    size_t samplers, conversions;
};
static bool in_range(size_t size, size_t offset, uint64_t length) { return offset <= size && length <= size - offset; }
static bool valid_filter(uint32_t f) { return f <= VK_FILTER_LINEAR || f == VK_FILTER_CUBIC_EXT; }
static bool valid_conversion(const uint8_t *p) {
    struct vkrt_ycbcr_format_metadata format = vkrt_ycbcr_format_metadata(get32(p));
    if (!format.unorm || get32(p + 4) > VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020 ||
        get32(p + 8) > VK_SAMPLER_YCBCR_RANGE_ITU_NARROW || get32(p + 28) > VK_CHROMA_LOCATION_MIDPOINT ||
        get32(p + 32) > VK_CHROMA_LOCATION_MIDPOINT || !valid_filter(get32(p + 36)) || get32(p + 40) > 1 || get32(p + 44))
        return false;
    uint32_t swizzle[4];
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t source = get32(p + 12 + i * 4);
        if (source > VK_COMPONENT_SWIZZLE_A) return false;
        swizzle[i] = source == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_R + i : source;
        if (i < 3) {
            uint32_t bits = source == VK_COMPONENT_SWIZZLE_ZERO || source == VK_COMPONENT_SWIZZLE_ONE
                                ? 255 /* Constant precision is not supplied by format metadata. */
                                : format.bits[swizzle[i] - VK_COMPONENT_SWIZZLE_R];
            if (get32(p + 4) != VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY &&
                (source == VK_COMPONENT_SWIZZLE_ZERO || source == VK_COMPONENT_SWIZZLE_ONE || !bits))
                return false; /* 01655: includes component substitution. */
            if (get32(p + 8) == VK_SAMPLER_YCBCR_RANGE_ITU_NARROW && bits < 8) return false; /* 02748 */
        }
    }
    if (format.subsampled && (swizzle[1] != VK_COMPONENT_SWIZZLE_G ||
                              (swizzle[3] != VK_COMPONENT_SWIZZLE_A && swizzle[3] != VK_COMPONENT_SWIZZLE_ONE &&
                               swizzle[3] != VK_COMPONENT_SWIZZLE_ZERO) ||
                              !((swizzle[0] == VK_COMPONENT_SWIZZLE_R && swizzle[2] == VK_COMPONENT_SWIZZLE_B) ||
                                (swizzle[0] == VK_COMPONENT_SWIZZLE_B && swizzle[2] == VK_COMPONENT_SWIZZLE_R))))
        return false; /* 02581..02585: explicit matching component is identity too. */
    return true;
}
static bool valid_descriptor(uint32_t type) {
    if (type <= VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) return true;
    switch (type) {
        case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV:
        case VK_DESCRIPTOR_TYPE_MUTABLE_EXT:
#ifdef VK_QCOM_image_processing
        case VK_DESCRIPTOR_TYPE_SAMPLE_WEIGHT_IMAGE_QCOM:
        case VK_DESCRIPTOR_TYPE_BLOCK_MATCH_IMAGE_QCOM:
#endif
#ifdef VK_ARM_tensors
        case VK_DESCRIPTOR_TYPE_TENSOR_ARM:
#endif
            return true;
        default:
            return false;
    }
}
static bool valid_sampler(const uint8_t *p, size_t remaining, struct recipe_stats *stats) {
    if (remaining < SAMPLER_BYTES) return false;
    uint64_t length = get64(p);
    uint32_t mask = get32(p + 8), conversion = get32(p + 120);
    if (mask & ~15u || get32(p + 124) || conversion != ((mask & SAMPLER_CONVERSION) ? CONVERSION_BYTES : 0) ||
        length != SAMPLER_BYTES + conversion || length > remaining)
        return false;
    if (get32(p + 44) > 1 || get32(p + 52) > 1 || get32(p + 72) > 1 || ((mask & SAMPLER_MAPPING) && get32(p + 116) > 1))
        return false;
    if ((get32(p + 12) & ~31u) || !valid_filter(get32(p + 16)) || !valid_filter(get32(p + 20)) ||
        get32(p + 24) > VK_SAMPLER_MIPMAP_MODE_LINEAR || get32(p + 28) > VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ||
        get32(p + 32) > VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ||
        get32(p + 36) > VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE || (get32(p + 52) && get32(p + 56) > VK_COMPARE_OP_ALWAYS))
        return false;
    float bias = bits_float(get32(p + 40)), min_lod = bits_float(get32(p + 60)), max_lod = bits_float(get32(p + 64));
    float anisotropy = bits_float(get32(p + 48));
    if (!isfinite(bias) || !(max_lod >= min_lod) || (get32(p + 44) && (!isfinite(anisotropy) || anisotropy < 1.0f))) return false;
    uint32_t u = get32(p + 28), v = get32(p + 32), w = get32(p + 36), flags = get32(p + 12);
    bool clamp_u = u == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE || u == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    bool clamp_v = v == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE || v == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    if (get32(p + 72) || (flags & VK_SAMPLER_CREATE_SUBSAMPLED_BIT_EXT)) {
        if (get32(p + 16) != get32(p + 20) || get32(p + 24) != VK_SAMPLER_MIPMAP_MODE_NEAREST || min_lod != 0.0f ||
            max_lod != 0.0f || !clamp_u || !clamp_v || get32(p + 44) || get32(p + 52))
            return false;
        if ((flags & VK_SAMPLER_CREATE_SUBSAMPLED_BIT_EXT) && get32(p + 72)) return false;
    }
    uint32_t reduction = mask & SAMPLER_REDUCTION ? get32(p + 76) : VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE;
    if (get32(p + 52) && reduction != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE) return false;
    if ((get32(p + 16) == VK_FILTER_CUBIC_EXT || get32(p + 20) == VK_FILTER_CUBIC_EXT) && get32(p + 44)) return false;
    if (conversion && (u != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE || v != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE ||
                       w != VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE || get32(p + 44) || get32(p + 72) ||
                       reduction != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE))
        return false;
    uint32_t border = get32(p + 68);
    if ((u == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || v == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
         w == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) &&
        border > VK_BORDER_COLOR_INT_OPAQUE_WHITE && border != VK_BORDER_COLOR_FLOAT_CUSTOM_EXT &&
        border != VK_BORDER_COLOR_INT_CUSTOM_EXT)
        return false;
    if ((border == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT || border == VK_BORDER_COLOR_INT_CUSTOM_EXT) && !(mask & SAMPLER_CUSTOM))
        return false;
    if (mask & SAMPLER_MAPPING)
        for (unsigned i = 25; i < 29; ++i)
            if (get32(p + i * 4) > VK_COMPONENT_SWIZZLE_A) return false;
    if (conversion && !valid_conversion(p + SAMPLER_BYTES)) return false;
    if (stats->samplers == SIZE_MAX || (conversion && stats->conversions == SIZE_MAX)) return false;
    ++stats->samplers;
    if (conversion) ++stats->conversions;
    return true;
}
static bool validate_recipe(const void *data, size_t size, struct recipe_stats *stats) {
    const uint8_t *p = data;
    if (!p || size < HEADER_BYTES || get32(p) != INTERFACE_MAGIC || get32(p + 4) != INTERFACE_VERSION || get64(p + 8) != size ||
        (get32(p + 24) & ~15u) || get32(p + 28) > 1)
        return false;
    uint32_t sets = get32(p + 16), pushes = get32(p + 20);
    size_t offset = HEADER_BYTES;
    if (!in_range(size, offset, (uint64_t)pushes * 12)) return false;
    VkShaderStageFlags push_stages = 0;
    for (uint32_t i = 0; i < pushes; ++i) {
        const uint8_t *push = p + offset + (size_t)i * 12;
        if (!get32(push) || !get32(push + 8) || ((get32(push + 4) | get32(push + 8)) & 3) ||
            get32(push + 4) > UINT32_MAX - get32(push + 8) || (push_stages & get32(push)))
            return false;
        push_stages |= get32(push);
    }
    offset += (size_t)pushes * 12;
    if ((uint64_t)sets * LAYOUT_BYTES > size - offset) return false;
    for (uint32_t i = 0; i < sets; ++i) {
        if (!in_range(size, offset, LAYOUT_BYTES)) return false;
        const uint8_t *layout = p + offset;
        uint64_t length = get64(layout);
        if (length < LAYOUT_BYTES || !in_range(size, offset, length) || get32(layout + 8) > 1 || get32(layout + 20) & ~3u ||
            get32(layout + 12) & ~0xf7u)
            return false;
        size_t end = offset + (size_t)length;
        uint32_t count = get32(layout + 16);
        offset += LAYOUT_BYTES;
        if ((uint64_t)count * BINDING_BYTES > end - offset) return false;
        for (uint32_t j = 0; j < count; ++j) {
            if (!in_range(end, offset, BINDING_BYTES)) return false;
            const uint8_t *binding = p + offset;
            const uint8_t *prior = layout + LAYOUT_BYTES;
            for (uint32_t k = 0; k < j; ++k) {
                if (!(get32(layout + 12) & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PER_STAGE_BIT_NV) && get32(prior) == get32(binding))
                    return false;
                prior = binding_view(prior).end;
            }
            uint32_t type = get32(binding + 4), n = get32(binding + 8), immutable = get32(binding + 20),
                     types = get32(binding + 24);
            if ((n && !valid_descriptor(type)) || (get32(binding + 16) & ~15u)) return false;
            if (get32(binding + 28) ||
                (immutable &&
                 (immutable != n || (type != VK_DESCRIPTOR_TYPE_SAMPLER && type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))) ||
                (types && type != VK_DESCRIPTOR_TYPE_MUTABLE_EXT))
                return false;
            if ((types && !(get32(layout + 20) & CHAIN_MUTABLE)) ||
                (get32(binding + 16) && !(get32(layout + 20) & CHAIN_BINDING_FLAGS)))
                return false;
            offset += BINDING_BYTES;
            if ((uint64_t)immutable * SAMPLER_BYTES > end - offset) return false;
            for (uint32_t k = 0; k < immutable; ++k) {
                if (!valid_sampler(p + offset, end - offset, stats)) return false;
                offset += (size_t)get64(p + offset);
            }
            if (!in_range(end, offset, (uint64_t)types * 4)) return false;
            for (uint32_t k = 0; k < types; ++k)
                if (!valid_descriptor(get32(p + offset + (size_t)k * 4))) return false;
            offset += (size_t)types * 4;
        }
        if (offset != end) return false;
    }
    return offset == size && stats->samplers <= SIZE_MAX / sizeof(VkSampler) &&
           stats->conversions <= SIZE_MAX / sizeof(VkSamplerYcbcrConversion);
}
VkResult vkrt_shader_interface_validate(const void *bytes, size_t size) {
    struct recipe_stats stats = {0};
    return validate_recipe(bytes, size, &stats) ? VK_SUCCESS : VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
}
/* Known capability feature mappings from the matching Vulkan registry. This
 * checks every original peer module, including capabilities unused by the
 * selected exported entry point. The private allowances match the backend's
 * actual device feature augmentation. Operation/storage-class restrictions
 * and other extension capabilities still require broader qualification; this
 * is not a complete SPIR-V validator. */
static bool core_capability_enabled(const struct interface_features *f, uint32_t cap, bool private_backend, bool private_atomics,
                                    uint32_t native_api) {
    switch (cap) {
        case 2:
            return f->core.geometryShader; /* Geometry */
        case 3:
            return f->core.tessellationShader;
        case 9:
            return f->v12.shaderFloat16 || f->amd_float16;
        case 10:
            return f->core.shaderFloat64;
        case 11:
            return f->core.shaderInt64 || private_backend;
        case 12:
            return f->v12.shaderBufferInt64Atomics || f->v12.shaderSharedInt64Atomics || f->image_int64_atomics || private_atomics;
        case 22:
            return f->core.shaderInt16 || private_backend;
        case 23: /* TessellationPointSize */
        case 24:
            return f->core.shaderTessellationAndGeometryPointSize;
        case 25:
            return f->core.shaderImageGatherExtended;
        case 27:
            return f->core.shaderStorageImageMultisample;
        case 28:
            return f->core.shaderUniformBufferArrayDynamicIndexing;
        case 29:
            return f->core.shaderSampledImageArrayDynamicIndexing;
        case 30:
            return f->core.shaderStorageBufferArrayDynamicIndexing || private_backend;
        case 31:
            return f->core.shaderStorageImageArrayDynamicIndexing;
        case 32:
            return f->core.shaderClipDistance;
        case 33:
            return f->core.shaderCullDistance;
        case 34: /* ImageCubeArray */
        case 45:
            return f->core.imageCubeArray;
        case 35: /* SampleRateShading */
        case 52:
            return f->core.sampleRateShading;
        case 39:
            return f->v12.shaderInt8 || private_backend;
        case 41:
            return f->core.shaderResourceResidency;
        case 42:
            return f->core.shaderResourceMinLod;
        case 48:
            return f->core.shaderStorageImageMultisample;
        case 55:
            return f->core.shaderStorageImageReadWithoutFormat || native_api >= VK_API_VERSION_1_3 || f->format_flags2;
        case 56:
            return f->core.shaderStorageImageWriteWithoutFormat || native_api >= VK_API_VERSION_1_3 || f->format_flags2;
        case 57:
            return f->core.multiViewport;
        case 69:
            return f->v12.shaderOutputLayer;
        case 70:
            return f->v12.shaderOutputViewportIndex;
        case 4427:
            return f->v11.shaderDrawParameters;
        case 4433:
            return f->v11.storageBuffer16BitAccess || private_backend;
        case 4434:
            return f->v11.uniformAndStorageBuffer16BitAccess;
        case 4435:
            return f->v11.storagePushConstant16;
        case 4436:
            return f->v11.storageInputOutput16;
        case 4439:
            return f->v11.multiview;
        case 4441:
            return f->v11.variablePointersStorageBuffer || private_backend;
        case 4442:
            return f->v11.variablePointers;
        case 4448:
            return f->v12.storageBuffer8BitAccess || private_backend;
        case 4449:
            return f->v12.uniformAndStorageBuffer8BitAccess;
        case 4450:
            return f->v12.storagePushConstant8;
        case 5302:
            return f->indexing.runtimeDescriptorArray;
        case 5303:
            return f->indexing.shaderInputAttachmentArrayDynamicIndexing;
        case 5304:
            return f->indexing.shaderUniformTexelBufferArrayDynamicIndexing;
        case 5305:
            return f->indexing.shaderStorageTexelBufferArrayDynamicIndexing;
        case 5306:
            return f->indexing.shaderUniformBufferArrayNonUniformIndexing;
        case 5307:
            return f->indexing.shaderSampledImageArrayNonUniformIndexing;
        case 5308:
            return f->indexing.shaderStorageBufferArrayNonUniformIndexing || private_backend;
        case 5309:
            return f->indexing.shaderStorageImageArrayNonUniformIndexing;
        case 5310:
            return f->indexing.shaderInputAttachmentArrayNonUniformIndexing;
        case 5311:
            return f->indexing.shaderUniformTexelBufferArrayNonUniformIndexing;
        case 5312:
            return f->indexing.shaderStorageTexelBufferArrayNonUniformIndexing;
        case 5345:
            return f->v12.vulkanMemoryModel || private_backend;
        case 5346:
            return f->v12.vulkanMemoryModelDeviceScope || private_backend;
        case 5347:
            return f->v12.bufferDeviceAddress || private_backend;
        case 5379:
            return f->v13.shaderDemoteToHelperInvocation;
        case 6016: /* DotProductInputAll, Input4x8Bit, Input4x8BitPacked, DotProduct */
        case 6017:
        case 6018:
        case 6019:
            return f->v13.shaderIntegerDotProduct;
        default:
            return true;
    }
}
static bool source_core_capabilities(const struct vkrt_device_data *dev, const struct vkrt_shader_interface_state *s,
                                     const VkShaderCreateInfoEXT *info) {
    const struct interface_features *f = &s->enabled;
    if (!info->pCode || info->codeSize < 20 || info->codeSize % 4) return false;
    const uint8_t *p = info->pCode;
    uint32_t magic;
    memcpy(&magic, p, 4);
    if (magic != 0x07230203) return false;
    size_t words = info->codeSize / 4;
    bool backend = dev->virtual_rt || dev->shader_object_backend;
    for (size_t i = 5; i < words;) {
        uint32_t instruction;
        memcpy(&instruction, p + i * 4, 4);
        uint32_t count = instruction >> 16, opcode = instruction & 0xffffu;
        if (!count || count > words - i) return false;
        if (opcode == 17) { /* OpCapability */
            if (count != 2) return false;
            uint32_t capability;
            memcpy(&capability, p + (i + 1) * 4, 4);
            if (!core_capability_enabled(f, capability, backend, dev->query_backend_enabled, s->native_api)) return false;
        } else if (opcode == 331) { /* OpExecutionModeId */
            if (count < 3) return false;
            uint32_t mode;
            memcpy(&mode, p + (i + 2) * 4, 4);
            if (mode == 38 && (count != 6 || !f->v13.maintenance4)) return false; /* LocalSizeId */
        }
        i += count;
    }
    return true;
}
VkResult vkrt_shader_interface_source_supported(struct vkrt_device_data *dev, const VkShaderCreateInfoEXT *info) {
    const struct vkrt_shader_interface_state *s = dev->shader_interfaces;
    if (!s || !info || info->codeType != VK_SHADER_CODE_TYPE_SPIRV_EXT) return VK_ERROR_FEATURE_NOT_PRESENT;
    const struct interface_features *f = &s->enabled;
    if (!source_core_capabilities(dev, s, info)) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkShaderStageFlags next = 0;
    switch (info->stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:
            next = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            break;
        case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
            if (!f->tessellation) return VK_ERROR_FEATURE_NOT_PRESENT;
            next = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
            break;
        case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
            if (!f->tessellation) return VK_ERROR_FEATURE_NOT_PRESENT;
            next = VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            break;
        case VK_SHADER_STAGE_GEOMETRY_BIT:
            if (!f->geometry) return VK_ERROR_FEATURE_NOT_PRESENT;
            next = VK_SHADER_STAGE_FRAGMENT_BIT;
            break;
        case VK_SHADER_STAGE_TASK_BIT_EXT:
            if (!f->task) return VK_ERROR_FEATURE_NOT_PRESENT;
            next = VK_SHADER_STAGE_MESH_BIT_EXT;
            break;
        case VK_SHADER_STAGE_MESH_BIT_EXT:
            if (!f->mesh) return VK_ERROR_FEATURE_NOT_PRESENT;
            next = VK_SHADER_STAGE_FRAGMENT_BIT;
            break;
        case VK_SHADER_STAGE_FRAGMENT_BIT:
        case VK_SHADER_STAGE_COMPUTE_BIT:
            break;
        default:
            return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if ((info->nextStage & ~next) ||
        ((info->nextStage & (VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)) &&
         !f->tessellation) ||
        ((info->nextStage & VK_SHADER_STAGE_GEOMETRY_BIT) && !f->geometry) ||
        ((info->nextStage & VK_SHADER_STAGE_MESH_BIT_EXT) && !f->mesh))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkShaderCreateFlagsEXT supported =
        VK_SHADER_CREATE_LINK_STAGE_BIT_EXT | VK_SHADER_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT |
        VK_SHADER_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT | VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT |
        VK_SHADER_CREATE_DISPATCH_BASE_BIT_EXT | VK_SHADER_CREATE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_EXT |
        VK_SHADER_CREATE_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT;
    if (info->flags & ~supported) return VK_ERROR_FEATURE_NOT_PRESENT;
    if (((info->flags & VK_SHADER_CREATE_LINK_STAGE_BIT_EXT) && info->stage == VK_SHADER_STAGE_COMPUTE_BIT) ||
        ((info->flags & VK_SHADER_CREATE_DISPATCH_BASE_BIT_EXT) && info->stage != VK_SHADER_STAGE_COMPUTE_BIT) ||
        ((info->flags & VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT) && info->stage != VK_SHADER_STAGE_MESH_BIT_EXT) ||
        ((info->flags & VK_SHADER_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT) && !f->subgroup_control) ||
        ((info->flags & VK_SHADER_CREATE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_EXT) &&
         (info->stage != VK_SHADER_STAGE_FRAGMENT_BIT || !f->shading_rate_attachment)) ||
        ((info->flags & VK_SHADER_CREATE_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT) &&
         (info->stage != VK_SHADER_STAGE_FRAGMENT_BIT || !f->density_map)))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    /* A full-subgroups compilation also constrains specialized LocalSize/X.
     * Until that execution-mode proof is decoded, fail this optional profile
     * even when computeFullSubgroups was enabled; do not forward a guessed CI. */
    if (info->flags & VK_SHADER_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT) return VK_ERROR_FEATURE_NOT_PRESENT;
    bool required = false;
    for (const VkBaseInStructure *p = info->pNext; p; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO || required ||
            !f->subgroup_control || !s->subgroup_known || (info->flags & VK_SHADER_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT_EXT))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        uint32_t size = ((const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *)p)->requiredSubgroupSize;
        if (!size || (size & (size - 1)) || size < s->subgroup.minSubgroupSize || size > s->subgroup.maxSubgroupSize ||
            !(s->subgroup.requiredSubgroupSizeStages & info->stage))
            return VK_ERROR_FEATURE_NOT_PRESENT;
        /* Workgroup stages also constrain specialized invocation counts by
         * requiredSize*maxComputeWorkgroupSubgroups. Until LocalSize/LocalSizeId
         * specialization is proven, retain the same explicit profile boundary. */
        if (info->stage == VK_SHADER_STAGE_COMPUTE_BIT || info->stage == VK_SHADER_STAGE_TASK_BIT_EXT ||
            info->stage == VK_SHADER_STAGE_MESH_BIT_EXT)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        required = true;
    }
    return VK_SUCCESS;
}
static bool update_after_bind_enabled(const struct interface_features *f, VkDescriptorType type) {
    switch (type) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            return f->indexing.descriptorBindingSampledImageUpdateAfterBind;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            return f->indexing.descriptorBindingStorageImageUpdateAfterBind;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            return f->indexing.descriptorBindingUniformBufferUpdateAfterBind;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            return f->indexing.descriptorBindingStorageBufferUpdateAfterBind;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            return f->indexing.descriptorBindingUniformTexelBufferUpdateAfterBind;
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return f->indexing.descriptorBindingStorageTexelBufferUpdateAfterBind;
        case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
            return f->inline_uab;
        default:
            return false;
    }
}
/* These are destination enabled bits, not merely physical-device support.
 * Unsupported construction profiles fail before any private native creation;
 * flags are never discarded to manufacture a different interface. */
static bool recipe_supported(const struct vkrt_shader_interface_state *s, const uint8_t *p) {
    const struct interface_features empty = {0};
    const struct interface_features *f = s ? &s->enabled : &empty;
    if (get32(p + 24) & ~f->robustness) return false;
    const uint8_t *layout = p + HEADER_BYTES + (size_t)get32(p + 20) * 12;
    for (uint32_t i = 0; i < get32(p + 16); ++i) {
        if (get32(layout + 12) & ~VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT) return false;
        const uint8_t *cursor = layout + LAYOUT_BYTES;
        bool translate = get32(layout + 8) != 0;
        for (uint32_t j = 0; j < get32(layout + 16); ++j) {
            struct binding_view b = binding_view(cursor);
            if (!get32(b.header + 8)) {
                cursor = b.end;
                continue; /* Unused bindings do not require their ignored type's features. */
            }
            VkDescriptorType type = descriptor_type(get32(b.header + 4), translate);
            uint32_t flags = get32(b.header + 16), types = get32(b.header + 24);
            if ((type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK && !f->inline_uniform) ||
                (type == VK_DESCRIPTOR_TYPE_MUTABLE_EXT && !f->mutable) ||
                ((flags & VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT) && !f->indexing.descriptorBindingPartiallyBound) ||
                ((flags & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) &&
                 !f->indexing.descriptorBindingVariableDescriptorCount) ||
                ((flags & VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT) &&
                 !f->indexing.descriptorBindingUpdateUnusedWhilePending))
                return false;
            if (flags & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT) {
                if (type != VK_DESCRIPTOR_TYPE_MUTABLE_EXT && !update_after_bind_enabled(f, type)) return false;
                for (uint32_t k = 0; k < types; ++k)
                    if (!update_after_bind_enabled(f, descriptor_type(get32(b.types + (size_t)k * 4), translate))) return false;
            }
            const uint8_t *sampler = b.samplers;
            for (uint32_t k = 0; k < get32(b.header + 20); ++k) {
                uint32_t mask = get32(sampler + 8), sampler_flags = get32(sampler + 12);
                float bias = bits_float(get32(sampler + 40));
                float anisotropy = bits_float(get32(sampler + 48));
                float max_bias = s ? s->max_lod_bias : 2.0f, max_anisotropy = s ? s->max_anisotropy : 16.0f;
                if (bias < -max_bias || bias > max_bias || (get32(sampler + 44) && anisotropy > max_anisotropy)) return false;
                /* Separate chroma reconstruction filters need a format-feature
                 * qualification path. Do not assume that optional format bit. */
                if ((mask & SAMPLER_CONVERSION) && (get32(sampler + 16) != get32(sampler + SAMPLER_BYTES + 36) ||
                                                    get32(sampler + 20) != get32(sampler + SAMPLER_BYTES + 36)))
                    return false;
                if ((get32(sampler + 44) && !f->anisotropy) || ((mask & SAMPLER_CONVERSION) && !f->ycbcr) ||
                    ((mask & SAMPLER_CUSTOM) && (!f->custom_border || (!get32(sampler + 96) && !f->custom_without_format))) ||
                    ((mask & SAMPLER_MAPPING) && !f->border_swizzle) ||
                    (sampler_flags & ~VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT) ||
                    ((sampler_flags & VK_SAMPLER_CREATE_NON_SEAMLESS_CUBE_MAP_BIT_EXT) && !f->non_seamless))
                    return false;
                if (!f->cubic && (get32(sampler + 16) == VK_FILTER_CUBIC_EXT || get32(sampler + 20) == VK_FILTER_CUBIC_EXT))
                    return false;
                if (!f->mirror_clamp && (get32(sampler + 28) == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ||
                                         get32(sampler + 32) == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ||
                                         get32(sampler + 36) == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE))
                    return false;
                if (mask & SAMPLER_REDUCTION) {
                    uint32_t mode = get32(sampler + 76);
                    if (mode > VK_SAMPLER_REDUCTION_MODE_MAX || (mode != VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE && !f->minmax))
                        return false;
                }
                sampler += (size_t)get64(sampler);
            }
            cursor = b.end;
        }
        layout += (size_t)get64(layout);
    }
    return true;
}
static PFN_vkVoidFunction native_proc(struct vkrt_device_data *dev, const char *core, const char *alias) {
    if (!dev->vtable.GetDeviceProcAddr) return NULL;
    PFN_vkVoidFunction proc = dev->vtable.GetDeviceProcAddr(dev->device, core);
    return !proc && alias ? dev->vtable.GetDeviceProcAddr(dev->device, alias) : proc;
}
static VkResult make_sampler(struct vkrt_device_data *dev, struct vkrt_shader_interface *owned, const uint8_t *p, VkSampler *out) {
    const VkAllocationCallbacks *allocator = owned_allocator(owned);
    VkSamplerCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.flags = get32(p + 12);
    info.magFilter = get32(p + 16);
    info.minFilter = get32(p + 20);
    info.mipmapMode = get32(p + 24);
    info.addressModeU = get32(p + 28);
    info.addressModeV = get32(p + 32);
    info.addressModeW = get32(p + 36);
    info.mipLodBias = bits_float(get32(p + 40));
    info.anisotropyEnable = get32(p + 44);
    info.maxAnisotropy = bits_float(get32(p + 48));
    info.compareEnable = get32(p + 52);
    info.compareOp = get32(p + 56);
    info.minLod = bits_float(get32(p + 60));
    info.maxLod = bits_float(get32(p + 64));
    info.borderColor = get32(p + 68);
    info.unnormalizedCoordinates = get32(p + 72);
    VkSamplerReductionModeCreateInfo reduction = {.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
                                                  .reductionMode = get32(p + 76)};
    VkSamplerCustomBorderColorCreateInfoEXT custom = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
                                                      .format = get32(p + 96)};
    for (unsigned i = 0; i < 4; ++i) custom.customBorderColor.uint32[i] = get32(p + (20 + i) * 4);
    VkSamplerBorderColorComponentMappingCreateInfoEXT mapping = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_BORDER_COLOR_COMPONENT_MAPPING_CREATE_INFO_EXT,
        .components = {get32(p + 100), get32(p + 104), get32(p + 108), get32(p + 112)},
        .srgb = get32(p + 116)};
    VkSamplerYcbcrConversionInfo conversion_info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
    uint32_t mask = get32(p + 8);
#define PREPEND(node, bit)         \
    if (mask & (bit)) {            \
        (node).pNext = info.pNext; \
        info.pNext = &(node);      \
    }
    PREPEND(reduction, SAMPLER_REDUCTION);
    PREPEND(custom, SAMPLER_CUSTOM);
    PREPEND(mapping, SAMPLER_MAPPING);
    if (mask & SAMPLER_CONVERSION) {
        const uint8_t *c = p + SAMPLER_BYTES;
        VkSamplerYcbcrConversionCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
                                                 .format = get32(c),
                                                 .ycbcrModel = get32(c + 4),
                                                 .ycbcrRange = get32(c + 8),
                                                 .components = {get32(c + 12), get32(c + 16), get32(c + 20), get32(c + 24)},
                                                 .xChromaOffset = get32(c + 28),
                                                 .yChromaOffset = get32(c + 32),
                                                 .chromaFilter = get32(c + 36),
                                                 .forceExplicitReconstruction = get32(c + 40)};
        PFN_vkCreateSamplerYcbcrConversion create =
            (void *)native_proc(dev, "vkCreateSamplerYcbcrConversion", "vkCreateSamplerYcbcrConversionKHR");
        if (!create) return VK_ERROR_FEATURE_NOT_PRESENT;
        VkResult result = create(dev->device, &ci, allocator, &conversion_info.conversion);
        if (result != VK_SUCCESS) return result;
        owned->conversions[owned->conversion_count++] = conversion_info.conversion;
    }
    PREPEND(conversion_info, SAMPLER_CONVERSION);
#undef PREPEND
    PFN_vkCreateSampler create = (void *)native_proc(dev, "vkCreateSampler", NULL);
    if (!create) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult result = create(dev->device, &info, allocator, out);
    if (result == VK_SUCCESS) owned->samplers[owned->sampler_count++] = *out;
    return result;
}
VkResult vkrt_shader_interface_materialize(struct vkrt_device_data *dev, const void *bytes, size_t size,
                                           struct vkrt_shader_interface **output) {
    *output = NULL;
    struct recipe_stats stats = {0};
    if (!validate_recipe(bytes, size, &stats)) return VK_ERROR_INCOMPATIBLE_SHADER_BINARY_EXT;
    const uint8_t *p = bytes;
    uint32_t sets = get32(p + 16), pushes = get32(p + 20);
    if (!recipe_supported(dev->shader_interfaces, p)) return VK_ERROR_FEATURE_NOT_PRESENT;
    if ((sets && (!dev->vtable.CreateDescriptorSetLayout || !dev->vtable.DestroyDescriptorSetLayout)) ||
        (stats.samplers && (!native_proc(dev, "vkCreateSampler", NULL) || !native_proc(dev, "vkDestroySampler", NULL))) ||
        (stats.conversions && (!native_proc(dev, "vkCreateSamplerYcbcrConversion", "vkCreateSamplerYcbcrConversionKHR") ||
                               !native_proc(dev, "vkDestroySamplerYcbcrConversion", "vkDestroySamplerYcbcrConversionKHR"))))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if ((uint64_t)sets * sizeof(VkDescriptorSetLayout) > SIZE_MAX || (uint64_t)pushes * sizeof(VkPushConstantRange) > SIZE_MAX)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    const VkAllocationCallbacks *a = device_allocator(dev);
    struct vkrt_shader_interface *owned = allocate(a, sizeof(*owned));
    if (!owned) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(owned, 0, sizeof(*owned));
    if (a) {
        owned->custom = true;
        owned->allocator = *a;
    }
    owned->layouts = allocate(a, (size_t)sets * sizeof(*owned->layouts));
    owned->pushes = allocate(a, (size_t)pushes * sizeof(*owned->pushes));
    owned->samplers = allocate(a, stats.samplers * sizeof(*owned->samplers));
    owned->conversions = allocate(a, stats.conversions * sizeof(*owned->conversions));
    VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
    if ((sets && !owned->layouts) || (pushes && !owned->pushes) || (stats.samplers && !owned->samplers) ||
        (stats.conversions && !owned->conversions))
        goto fail;
    size_t offset = HEADER_BYTES;
    for (uint32_t i = 0; i < pushes; ++i) {
        VkShaderStageFlags stages = get32(p + offset);
        /* Shader objects preserve core-stage ranges, strip virtual RT stages,
         * and omit RT-only ranges. This differs from descriptor-layout stage
         * lowering and exactly matches the source native shader create. */
        if (get32(p + 28) && stages != VK_SHADER_STAGE_ALL) stages &= ~VKRT_RTP_SHADER_STAGES;
        if (stages)
            owned->pushes[owned->push_count++] = (VkPushConstantRange){stages, get32(p + offset + 4), get32(p + offset + 8)};
        offset += 12;
    }
    for (uint32_t i = 0; i < sets; ++i) {
        const uint8_t *layout = p + offset;
        uint32_t count = get32(layout + 16);
        if ((uint64_t)count * sizeof(VkDescriptorSetLayoutBinding) > SIZE_MAX ||
            (uint64_t)count * sizeof(VkMutableDescriptorTypeListEXT) > SIZE_MAX) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto fail;
        }
        VkDescriptorSetLayoutBinding *bindings = allocate(a, (size_t)count * sizeof(*bindings));
        VkDescriptorBindingFlags *flags = allocate(a, (size_t)count * sizeof(*flags));
        VkMutableDescriptorTypeListEXT *lists = allocate(a, (size_t)count * sizeof(*lists));
        VkDescriptorType **type_storage = allocate(a, (size_t)count * sizeof(*type_storage));
        if (count && (!bindings || !flags || !lists || !type_storage)) {
            release(a, bindings);
            release(a, flags);
            release(a, lists);
            release(a, type_storage);
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto fail;
        }
        if (count) {
            memset(lists, 0, count * sizeof(*lists));
            memset(type_storage, 0, count * sizeof(*type_storage));
        }
        const uint8_t *cursor = layout + LAYOUT_BYTES;
        bool translate = get32(layout + 8) != 0;
        for (uint32_t j = 0; j < count; ++j) {
            struct binding_view b = binding_view(cursor);
            uint32_t immutable = get32(b.header + 20), types = get32(b.header + 24);
            bindings[j] = (VkDescriptorSetLayoutBinding){
                get32(b.header), descriptor_type(get32(b.header + 4), translate), get32(b.header + 8),
                translate ? vkrt_compute_shader_stages(get32(b.header + 12)) : get32(b.header + 12),
                immutable ? &owned->samplers[owned->sampler_count] : NULL};
            flags[j] = get32(b.header + 16);
            const uint8_t *sampler = b.samplers;
            for (uint32_t k = 0; k < immutable; ++k) {
                VkSampler made;
                result = make_sampler(dev, owned, sampler, &made);
                if (result != VK_SUCCESS) goto layout_fail;
                sampler += (size_t)get64(sampler);
            }
            if (types) {
                type_storage[j] = allocate(a, (size_t)types * sizeof(VkDescriptorType));
                if (!type_storage[j]) {
                    result = VK_ERROR_OUT_OF_HOST_MEMORY;
                    goto layout_fail;
                }
                lists[j].descriptorTypeCount = types;
                lists[j].pDescriptorTypes = type_storage[j];
                for (uint32_t k = 0; k < types; ++k)
                    type_storage[j][k] = descriptor_type(get32(b.types + (size_t)k * 4), translate);
            }
            cursor = b.end;
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfo flag_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount = count,
            .pBindingFlags = flags};
        VkMutableDescriptorTypeCreateInfoEXT mutable = {.sType = VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT,
                                                        .mutableDescriptorTypeListCount = count,
                                                        .pMutableDescriptorTypeLists = lists};
        VkDescriptorSetLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                .flags = get32(layout + 12),
                                                .bindingCount = count,
                                                .pBindings = bindings};
        if (get32(layout + 20) & CHAIN_BINDING_FLAGS) info.pNext = &flag_info;
        if (get32(layout + 20) & CHAIN_MUTABLE) {
            mutable.pNext = info.pNext;
            info.pNext = &mutable;
        }
        if (!dev->vtable.CreateDescriptorSetLayout)
            result = VK_ERROR_FEATURE_NOT_PRESENT;
        else
            result = dev->vtable.CreateDescriptorSetLayout(dev->device, &info, a, &owned->layouts[owned->set_count]);
        if (result == VK_SUCCESS) ++owned->set_count;
    layout_fail:
        for (uint32_t j = 0; j < count; ++j) release(a, type_storage[j]);
        release(a, bindings);
        release(a, flags);
        release(a, lists);
        release(a, type_storage);
        if (result != VK_SUCCESS) goto fail;
        offset += (size_t)get64(layout);
    }
    *output = owned;
    return VK_SUCCESS;
fail:
    vkrt_shader_interface_destroy(dev, owned);
    return result;
}
void vkrt_shader_interface_apply(const struct vkrt_shader_interface *owned, VkShaderCreateInfoEXT *info) {
    info->setLayoutCount = owned->set_count;
    info->pSetLayouts = owned->layouts;
    info->pushConstantRangeCount = owned->push_count;
    info->pPushConstantRanges = owned->pushes;
}
void vkrt_shader_interface_destroy(struct vkrt_device_data *dev, struct vkrt_shader_interface *owned) {
    if (!owned) return;
    const VkAllocationCallbacks *a = owned_allocator(owned);
    for (uint32_t i = 0; i < owned->set_count; ++i) dev->vtable.DestroyDescriptorSetLayout(dev->device, owned->layouts[i], a);
    PFN_vkDestroySampler destroy_sampler = (void *)native_proc(dev, "vkDestroySampler", NULL);
    for (size_t i = 0; i < owned->sampler_count; ++i) destroy_sampler(dev->device, owned->samplers[i], a);
    PFN_vkDestroySamplerYcbcrConversion destroy_conversion =
        (void *)native_proc(dev, "vkDestroySamplerYcbcrConversion", "vkDestroySamplerYcbcrConversionKHR");
    for (size_t i = 0; i < owned->conversion_count; ++i) destroy_conversion(dev->device, owned->conversions[i], a);
    release(a, owned->layouts);
    release(a, owned->pushes);
    release(a, owned->samplers);
    release(a, owned->conversions);
    release(a, owned);
}
