/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0 AND MIT
 *
 * Public shader hooks moved from MIT-licensed vkrt_layer.c.
 */
#include "vkrt_layer.h"
#include "vkrt_rtp.h"

VKAPI_ATTR VkResult VKAPI_CALL vkrt_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo,
                                                       const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule) {
    *pShaderModule = VK_NULL_HANDLE;
    struct vkrt_device_data *data = VKRT_FIND(struct vkrt_device_data, device);
    if (!data) return VK_ERROR_INITIALIZATION_FAILED;

    if (!data->virtual_rt && !data->rtp) return data->vtable.CreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule);
    if (data->virtual_rt) {
        bool handled = false;
        VkResult rtp_result = vkrt_rtp_create_shader(data, pCreateInfo, pAllocator, pShaderModule, &handled);
        if (handled || rtp_result != VK_SUCCESS) return rtp_result;
        /* Classification only: ray query and RT pipeline constructs are
         * rejected here instead of being lowered into the application's
         * graphics/compute module. */
        VkShaderModuleCreateInfo checked = *pCreateInfo;
        bool uses_rq = false, uses_rtp = false;
        VkResult result = vkrt_rewrite_shader_module(data, pCreateInfo, &checked, &uses_rq, &uses_rtp);
        if (result != VK_SUCCESS) return result;
    }

    VkShaderModule native = VK_NULL_HANDLE;
    VkResult result = data->vtable.CreateShaderModule(device, pCreateInfo, pAllocator, &native);
    if (result != VK_SUCCESS) return result;

    result = vkrt_rtp_wrap_native_shader(data, native, pAllocator, pShaderModule);
    if (result != VK_SUCCESS) return result;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkrt_DestroyShaderModule(VkDevice device, VkShaderModule shaderModule,
                                                    const VkAllocationCallbacks *pAllocator) {
    struct vkrt_device_data *data = VKRT_FIND(struct vkrt_device_data, device);
    if (!data) return;

    if (vkrt_rtp_destroy_shader(data, shaderModule, pAllocator)) return;
    data->vtable.DestroyShaderModule(device, shaderModule, pAllocator);
}
