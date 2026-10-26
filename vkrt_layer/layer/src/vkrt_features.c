/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_chain.h"
#include "vkrt_rt_capabilities.h"

/* The AS/RQ objects and shaders are always layer-owned. A native RTP pipeline
 * cannot consume their virtual handles/representation; mixed execution needs
 * the shared backend, not forwarding an apparently supported feature bit. */
static VkResult virtual_feature(const VkBaseInStructure *p, bool *remove, bool *uses_virtual_rt) {
    size_t offset, bytes;
    *remove = vkrt_unsupported_rt_feature_range(p->sType, &offset, &bytes);
    if (*remove) {
        for (size_t i = 0; i < bytes; i += sizeof(VkBool32)) {
            VkBool32 enabled;
            memcpy(&enabled, (const char *)p + offset + i, sizeof(enabled));
            if (enabled) return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR)
        *uses_virtual_rt |= ((const VkPhysicalDeviceAccelerationStructureFeaturesKHR *)p)->accelerationStructure != VK_FALSE;
    if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR) {
        *uses_virtual_rt |= ((const VkPhysicalDeviceRayQueryFeaturesKHR *)p)->rayQuery != VK_FALSE;
        *remove = true;
    }
    return VK_SUCCESS;
}

static bool augmented_feature(VkStructureType type) {
    switch (type) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
            return true;
        default:
            return false;
    }
}

bool vkrt_device_uses_virtual_rt(const VkDeviceCreateInfo *info) {
    for (uint32_t i = 0; info->ppEnabledExtensionNames && i < info->enabledExtensionCount; ++i)
        if (!strcmp(info->ppEnabledExtensionNames[i], VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) ||
            !strcmp(info->ppEnabledExtensionNames[i], VK_KHR_RAY_QUERY_EXTENSION_NAME))
            return true;
    for (const VkBaseInStructure *p = info->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR &&
            ((const VkPhysicalDeviceAccelerationStructureFeaturesKHR *)p)->accelerationStructure)
            return true;
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR &&
            ((const VkPhysicalDeviceRayQueryFeaturesKHR *)p)->rayQuery)
            return true;
    }
    return false;
}

/* Ordinary devices and the CPU-only deferred dependency must not inherit the
 * GPU backend's prerequisites. Only remove consumed feature nodes/names. */
static VkResult create_without_rt(PFN_vkCreateDevice create, VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                  const VkAllocationCallbacks *allocator, VkDevice *device) {
    const VkBaseInStructure *last = NULL;
    bool uses_rt = false;
    for (const VkBaseInStructure *p = info->pNext; p; p = p->pNext) {
        bool remove;
        VkResult result = virtual_feature(p, &remove, &uses_rt);
        if (result != VK_SUCCESS) return result;
        if (remove) last = p;
    }
    const VkBaseInStructure *borrowed = last ? last->pNext : info->pNext;
    VkBaseOutStructure *head = NULL, **tail = &head;
    VkResult result = VK_SUCCESS;
    for (const VkBaseInStructure *p = info->pNext; p != borrowed; p = p->pNext) {
        bool remove;
        result = virtual_feature(p, &remove, &uses_rt);
        if (result != VK_SUCCESS) goto done;
        if (remove) continue;
        size_t size = vkrt_chain_size(p->sType);
        if (!size) {
            result = VK_ERROR_EXTENSION_NOT_PRESENT;
            goto done;
        }
        VkBaseOutStructure *copy = malloc(size);
        if (!copy) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto done;
        }
        memcpy(copy, p, size);
        copy->pNext = NULL;
        *tail = copy;
        tail = &copy->pNext;
    }
    *tail = (VkBaseOutStructure *)borrowed;
    const char **extensions = NULL;
    if (info->enabledExtensionCount) {
        extensions = malloc((size_t)info->enabledExtensionCount * sizeof(*extensions));
        if (!extensions) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto done;
        }
    }
    VkDeviceCreateInfo local = *info;
    local.pNext = head;
    local.ppEnabledExtensionNames = extensions;
    local.enabledExtensionCount = 0;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        if (strcmp(info->ppEnabledExtensionNames[i], VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME))
            extensions[local.enabledExtensionCount++] = info->ppEnabledExtensionNames[i];
    result = create(physical, &local, allocator, device);
    free(extensions);
done:
    while (head && head != (const VkBaseOutStructure *)borrowed) {
        VkBaseOutStructure *next = head->pNext;
        free(head);
        head = next;
    }
    return result;
}

VkResult vkrt_create_device_with_features(struct vkrt_instance_data *inst,
   VkPhysicalDevice physical, PFN_vkCreateDevice create,
   const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDevice *device)
{
    if (!inst || !ci || !create || !device || (ci->enabledExtensionCount && !ci->ppEnabledExtensionNames))
        return VK_ERROR_INITIALIZATION_FAILED;
    bool uses_virtual_rt = vkrt_device_uses_virtual_rt(ci);
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) {
        const char *name = ci->ppEnabledExtensionNames[i];
        if (vkrt_incompatible_device_extension(name)) return VK_ERROR_FEATURE_NOT_PRESENT;
        uses_virtual_rt |=
            !strcmp(name, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) || !strcmp(name, VK_KHR_RAY_QUERY_EXTENSION_NAME);
    }
    /* Clone through the last node that must change. An opaque unmodified suffix
     * can be borrowed intact; a prefix whose size is unknown cannot be spliced
     * safely and is rejected rather than mutating/truncating caller storage. */
    const VkBaseInStructure *last_change = NULL;
    for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext) {
        bool remove;
        VkResult check = virtual_feature(p, &remove, &uses_virtual_rt);
        if (check != VK_SUCCESS) return check;
        if (remove || augmented_feature(p->sType)) last_change = p;
    }
    if (!uses_virtual_rt) return create_without_rt(create, physical, ci, alloc, device);
    const VkBaseInStructure *borrowed = last_change ? last_change->pNext : ci->pNext;
    VkPhysicalDeviceBufferDeviceAddressFeatures bda = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDeviceVulkanMemoryModelFeatures memory = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES,
                                                        .pNext = &bda};
    VkPhysicalDeviceScalarBlockLayoutFeatures scalar = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES,
                                                        .pNext = &memory};
    VkPhysicalDeviceShaderFloat16Int8Features int8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,
                                                      .pNext = &scalar};
    VkPhysicalDevice8BitStorageFeatures storage8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES,
                                                    .pNext = &int8};
    VkPhysicalDevice16BitStorageFeatures storage16 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,
                                                      .pNext = &storage8};
    VkPhysicalDeviceDescriptorIndexingFeatures descriptor = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, .pNext = &storage16};
    VkPhysicalDeviceVariablePointersFeatures pointers = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES,
                                                         .pNext = &descriptor};
    VkPhysicalDeviceFeatures2 supported = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &pointers};
    if (!inst->vtable.GetPhysicalDeviceFeatures2 || !inst->vtable.GetPhysicalDeviceProperties) return VK_ERROR_FEATURE_NOT_PRESENT;
    inst->vtable.GetPhysicalDeviceFeatures2(physical, &supported);
    if (!supported.features.shaderInt64 || !supported.features.shaderInt16 || !storage16.storageBuffer16BitAccess ||
        !storage8.storageBuffer8BitAccess || !int8.shaderInt8 || !scalar.scalarBlockLayout || !bda.bufferDeviceAddress ||
        !memory.vulkanMemoryModel || !memory.vulkanMemoryModelDeviceScope) {
        VKRT_LOG_ERR("GPU BVH requires BDA, scalar layout, int8/16/64, buffer8/16 and Vulkan device-scope memory model");
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    /* Base AS/RQ permits dynamically uniform and NonUniform-decorated AS
     * descriptor-array indexes. Lowering changes their descriptor type to
     * SSBO. Descriptor-pointer helper arguments additionally need variable
     * storage-buffer pointers after that change of storage class. */
    if (uses_virtual_rt && (!supported.features.shaderStorageBufferArrayDynamicIndexing ||
                            !descriptor.shaderStorageBufferArrayNonUniformIndexing || !pointers.variablePointersStorageBuffer)) {
        VKRT_LOG_ERR("AS value lowering requires storage-buffer dynamic/nonuniform indexing and variable pointers");
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

   VkBaseOutStructure *head = NULL, **tail = &head;
   VkResult result = VK_SUCCESS;
   for (const VkBaseInStructure *p = ci->pNext; p != borrowed; p = p->pNext) {
       bool remove;
       result = virtual_feature(p, &remove, &uses_virtual_rt);
       if (result != VK_SUCCESS) goto done;
       if (remove) continue;
       size_t size = vkrt_chain_size(p->sType);
       if (!size) {
           result = VK_ERROR_EXTENSION_NOT_PRESENT;
           goto done;
       }
       VkBaseOutStructure *copy = malloc(size);
       if (!copy) {
           result = VK_ERROR_OUT_OF_HOST_MEMORY;
           goto done;
       }
       memcpy(copy, p, size);
       copy->pNext = NULL;
       *tail = copy;
       tail = &copy->pNext;
   }
   *tail = (VkBaseOutStructure *)borrowed;

   VkPhysicalDeviceFeatures core = ci->pEnabledFeatures ? *ci->pEnabledFeatures : (VkPhysicalDeviceFeatures){0};
   VkPhysicalDeviceFeatures *features = &core;
   bool has_features2 = false;
   bool has16 = false, has8 = false, has_int8 = false, has_scalar = false, has_memory = false, has_bda = false;
   bool has_descriptor = false;
   bool has_pointers = false;
   for (VkBaseOutStructure *p = head; p != (VkBaseOutStructure *)borrowed; p = p->pNext) {
       switch (p->sType) {
           case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
               features = &((VkPhysicalDeviceFeatures2 *)p)->features;
               has_features2 = true;
               break;
           case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
               ((VkPhysicalDeviceVulkan11Features *)p)->storageBuffer16BitAccess = VK_TRUE;
               if (uses_virtual_rt) ((VkPhysicalDeviceVulkan11Features *)p)->variablePointersStorageBuffer = VK_TRUE;
               has16 = true;
               has_pointers = true;
               break;
           case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
               VkPhysicalDeviceVulkan12Features *f = (void *)p;
               f->storageBuffer8BitAccess = f->shaderInt8 = f->scalarBlockLayout = VK_TRUE;
               f->vulkanMemoryModel = f->vulkanMemoryModelDeviceScope = f->bufferDeviceAddress = VK_TRUE;
               if (uses_virtual_rt) f->shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
               has8 = has_int8 = has_scalar = has_memory = has_bda = true;
               has_descriptor = true;
               break;
           }
           case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:
               if (uses_virtual_rt)
                   ((VkPhysicalDeviceDescriptorIndexingFeatures *)p)->shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
               has_descriptor = true;
               break;
           case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
               if (uses_virtual_rt) ((VkPhysicalDeviceVariablePointersFeatures *)p)->variablePointersStorageBuffer = VK_TRUE;
               has_pointers = true;
               break;
           case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
               ((VkPhysicalDevice16BitStorageFeatures *)p)->storageBuffer16BitAccess = VK_TRUE;
               has16 = true;
               break;
           case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES:
               ((VkPhysicalDevice8BitStorageFeatures *)p)->storageBuffer8BitAccess = VK_TRUE;
               has8 = true;
               break;
           case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:
               ((VkPhysicalDeviceShaderFloat16Int8Features *)p)->shaderInt8 = VK_TRUE;
               has_int8 = true;
               break;
           case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES:
               ((VkPhysicalDeviceScalarBlockLayoutFeatures *)p)->scalarBlockLayout = VK_TRUE;
               has_scalar = true;
               break;
           case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES:
               ((VkPhysicalDeviceVulkanMemoryModelFeatures *)p)->vulkanMemoryModel = VK_TRUE;
               ((VkPhysicalDeviceVulkanMemoryModelFeatures *)p)->vulkanMemoryModelDeviceScope = VK_TRUE;
               has_memory = true;
               break;
           case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES:
               ((VkPhysicalDeviceBufferDeviceAddressFeatures *)p)->bufferDeviceAddress = VK_TRUE;
               has_bda = true;
               break;
           default:
               break;
       }
   }
   features->shaderInt64 = features->shaderInt16 = VK_TRUE;
   if (uses_virtual_rt) features->shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
   /* Query results must not accidentally enable unrelated optional features. */
   storage16 = (VkPhysicalDevice16BitStorageFeatures){.sType = storage16.sType, .storageBuffer16BitAccess = VK_TRUE};
   storage8 = (VkPhysicalDevice8BitStorageFeatures){.sType = storage8.sType, .storageBuffer8BitAccess = VK_TRUE};
   int8 = (VkPhysicalDeviceShaderFloat16Int8Features){.sType = int8.sType, .shaderInt8 = VK_TRUE};
   scalar = (VkPhysicalDeviceScalarBlockLayoutFeatures){.sType = scalar.sType, .scalarBlockLayout = VK_TRUE};
   memory = (VkPhysicalDeviceVulkanMemoryModelFeatures){.sType = memory.sType, .vulkanMemoryModel = VK_TRUE, .vulkanMemoryModelDeviceScope = VK_TRUE};
   bda = (VkPhysicalDeviceBufferDeviceAddressFeatures){.sType = bda.sType, .bufferDeviceAddress = VK_TRUE};
   descriptor = (VkPhysicalDeviceDescriptorIndexingFeatures){.sType = descriptor.sType,
                                                             .shaderStorageBufferArrayNonUniformIndexing = VK_TRUE};
   pointers = (VkPhysicalDeviceVariablePointersFeatures){.sType = pointers.sType, .variablePointersStorageBuffer = VK_TRUE};
   VkDeviceCreateInfo local = *ci;
   local.pNext = head;
   local.pEnabledFeatures = has_features2 ? NULL : &core;
#define PREPEND(node, present) if (!(present)) { node.pNext = (void *)local.pNext; local.pNext = &node; }
   PREPEND(storage16, has16);
   PREPEND(storage8, has8);
   PREPEND(int8, has_int8);
   PREPEND(scalar, has_scalar);
   PREPEND(memory, has_memory);
   PREPEND(bda, has_bda);
   PREPEND(descriptor, has_descriptor || !uses_virtual_rt);
   PREPEND(pointers, has_pointers || !uses_virtual_rt);
#undef PREPEND
   /* Extension aliases also cover applications that requested Vulkan 1.0/1.1. */
   const char *required[] = {VK_KHR_16BIT_STORAGE_EXTENSION_NAME,       VK_KHR_8BIT_STORAGE_EXTENSION_NAME,
                             VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME, VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME,
                             VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
                             VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME,   VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME};
   size_t required_count = VKRT_ARRAY_SIZE(required) - (uses_virtual_rt ? 0 : 2);
   if (ci->enabledExtensionCount > UINT32_MAX - required_count ||
       (uint64_t)ci->enabledExtensionCount + required_count > SIZE_MAX / sizeof(const char *)) {
       result = VK_ERROR_OUT_OF_HOST_MEMORY;
       goto done;
   }
   const char **extensions = malloc(((size_t)ci->enabledExtensionCount + required_count) * sizeof(*extensions));
   if (!extensions) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
   local.enabledExtensionCount = 0;
   for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) {
       const char *name = ci->ppEnabledExtensionNames[i];
       if (!strcmp(name, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) || !strcmp(name, VK_KHR_RAY_QUERY_EXTENSION_NAME) ||
           !strcmp(name, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME))
           continue;
       extensions[local.enabledExtensionCount++] = name;
   }
   VkPhysicalDeviceProperties properties;
   inst->vtable.GetPhysicalDeviceProperties(physical, &properties);
   for (size_t i = 0; i < required_count; ++i) {
       /* Do not require extension aliases when the application uses core
        * features: implementations may expose core 1.2 without those names. */
       uint32_t promoted = i == 0 || i == 6 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_2;
       if (inst->api_version >= promoted && properties.apiVersion >= promoted) continue;
       bool found = false;
       for (uint32_t j = 0; j < local.enabledExtensionCount; ++j) found |= strcmp(extensions[j], required[i]) == 0;
       if (!found) extensions[local.enabledExtensionCount++] = required[i];
   }
   local.ppEnabledExtensionNames = extensions;
   result = create(physical, &local, alloc, device);
   free(extensions);
done:
    while (head && head != (VkBaseOutStructure *)borrowed) {
        VkBaseOutStructure *next = head->pNext;
        free(head);
        head = next;
    }
   return result;
}
