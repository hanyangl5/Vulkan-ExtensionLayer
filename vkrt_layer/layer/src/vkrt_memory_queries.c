/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_buffer_usage.h"
#include "vkrt_memory_queries.h"

VKAPI_ATTR void VKAPI_CALL vkrt_GetDeviceBufferMemoryRequirements(VkDevice device, const VkDeviceBufferMemoryRequirements *info,
                                                                  VkMemoryRequirements2 *requirements) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev || !dev->vtable.GetDeviceBufferMemoryRequirements) {
        requirements->memoryRequirements = (VkMemoryRequirements){0};
        VKRT_LOG_ERR("GetDeviceBufferMemoryRequirements: native entry point is unavailable");
        return;
    }
    if (!dev->as_registry) {
        dev->vtable.GetDeviceBufferMemoryRequirements(device, info, requirements);
        return;
    }

    struct vkrt_buffer_create_info buffer;
    VkResult result = vkrt_buffer_create_info_translate(&buffer, info->pCreateInfo);
    if (result == VK_SUCCESS) {
        VkDeviceBufferMemoryRequirements local = *info;
        local.pCreateInfo = &buffer.info;
        dev->vtable.GetDeviceBufferMemoryRequirements(device, &local, requirements);
    } else {
        /* Known valid chains have allocation-free stack storage. An unsupported
         * future prefix cannot be copied safely: do not send native-RT bits to
         * the ICD, and preserve the caller's extension-chain links. */
        requirements->memoryRequirements = (VkMemoryRequirements){0};
        VKRT_LOG_ERR("GetDeviceBufferMemoryRequirements: buffer translation failed (%d)", result);
    }
}

VKAPI_ATTR void VKAPI_CALL vkrt_GetBufferMemoryRequirements2(VkDevice device, const VkBufferMemoryRequirementsInfo2 *info,
                                                             VkMemoryRequirements2 *requirements) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (dev && dev->vtable.GetBufferMemoryRequirements2) /* The real handle was created with the translated usage already. */
        dev->vtable.GetBufferMemoryRequirements2(device, info, requirements);
    else {
        requirements->memoryRequirements = (VkMemoryRequirements){0};
        VKRT_LOG_ERR("GetBufferMemoryRequirements2: native entry point is unavailable");
    }
}

VKAPI_ATTR void VKAPI_CALL vkrt_GetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice physical,
                                                                          const VkPhysicalDeviceExternalBufferInfo *info,
                                                                          VkExternalBufferProperties *properties) {
    struct vkrt_instance_data *instance = VKRT_FIND(struct vkrt_instance_data, physical);
    if (!instance || !instance->vtable.GetPhysicalDeviceExternalBufferProperties) {
        properties->externalMemoryProperties = (VkExternalMemoryProperties){0};
        VKRT_LOG_ERR("GetPhysicalDeviceExternalBufferProperties: native entry point is unavailable");
        return;
    }
    const VkBufferUsageFlags2CreateInfoKHR *usage2 = vkrt_buffer_usage_flags2(info->pNext);
    if (!(info->usage & vkrt_buffer_rt_usage_mask()) && (!usage2 || !(usage2->usage & vkrt_buffer_rt_usage_mask()))) {
        instance->vtable.GetPhysicalDeviceExternalBufferProperties(physical, info, properties);
        return;
    }

    /* This query precedes logical-device creation. Explicit virtual RT usage
     * identifies buffers which CreateBuffer will lower; ordinary external
     * buffers retain their native usage on both ordinary and RT devices. */
    VkPhysicalDeviceExternalBufferInfo local = *info;
    local.usage = (VkBufferUsageFlags)vkrt_buffer_usage_translate(info->usage);
    struct vkrt_buffer_usage_chain chain;
    VkResult result = vkrt_buffer_usage_chain_translate(&chain, info->pNext);
    if (result == VK_SUCCESS) {
        local.pNext = chain.pNext;
        instance->vtable.GetPhysicalDeviceExternalBufferProperties(physical, &local, properties);
    } else {
        properties->externalMemoryProperties = (VkExternalMemoryProperties){0};
        VKRT_LOG_ERR("GetPhysicalDeviceExternalBufferProperties: buffer translation failed (%d)", result);
    }
}

PFN_vkVoidFunction vkrt_memory_query_proc(const char *name) {
#define ENTRY(api) \
    if (!strcmp(name, "vk" #api) || !strcmp(name, "vk" #api "KHR")) return (PFN_vkVoidFunction)vkrt_##api
    ENTRY(GetDeviceBufferMemoryRequirements);
    ENTRY(GetBufferMemoryRequirements2);
    ENTRY(GetPhysicalDeviceExternalBufferProperties);
#undef ENTRY
    return NULL;
}
