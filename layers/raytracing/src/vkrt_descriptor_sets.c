/* SPDX-License-Identifier: MIT
 * Public descriptor-set layout object ownership and native consumers.
 */
#include "vkrt_layer.h"
#include "vkrt_descriptor_layout.h"
#include "vkrt_shader_interface.h"
#include "vkrt_rtp.h"

static VKAPI_ATTR VkResult VKAPI_CALL vkrt_CreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *info,
                                                                     const VkAllocationCallbacks *allocator,
                                                                     VkDescriptorSetLayout *out) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev || !info || !dev->vtable.CreateDescriptorSetLayout) return VK_ERROR_INITIALIZATION_FAILED;
    VkDescriptorSetLayoutCreateInfo native;
    VkDescriptorSetLayoutBinding *bindings;
    VkResult result = vkrt_prepare_descriptor_layout(dev->virtual_rt, info, &native, &bindings);
    if (result != VK_SUCCESS) return result;
    vkrt_shader_interface_lock(dev);
    VkDescriptorSetLayout native_layout = VK_NULL_HANDLE, public_layout = VK_NULL_HANDLE;
    result = dev->vtable.CreateDescriptorSetLayout(device, dev->virtual_rt ? &native : info, allocator, &native_layout);
    free(bindings);
    if (result == VK_SUCCESS) {
        result = vkrt_rtp_wrap_descriptor_layout(dev, native_layout, info, allocator, &public_layout);
        if (result == VK_SUCCESS) {
            result = vkrt_shader_interface_track_layout_locked(dev, public_layout, info);
            if (result != VK_SUCCESS && !vkrt_rtp_destroy_descriptor_layout(dev, public_layout, allocator) && !dev->rtp)
                dev->vtable.DestroyDescriptorSetLayout(device, public_layout, allocator);
        }
    }
    if (result == VK_SUCCESS) *out = public_layout;
    vkrt_shader_interface_unlock(dev);
    return result;
}

static VKAPI_ATTR void VKAPI_CALL vkrt_DestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout layout,
                                                                  const VkAllocationCallbacks *allocator) {
    struct vkrt_device_data *data = VKRT_FIND(struct vkrt_device_data, device);
    if (!data) return;
    vkrt_shader_interface_lock(data);
    vkrt_shader_interface_forget_layout_locked(data, layout);
    if (!vkrt_rtp_destroy_descriptor_layout(data, layout, allocator) && !data->rtp && layout)
        data->vtable.DestroyDescriptorSetLayout(device, layout, allocator);
    vkrt_shader_interface_unlock(data);
}

static PFN_vkVoidFunction native_proc(struct vkrt_device_data *dev, const char *name) {
    return dev && dev->vtable.GetDeviceProcAddr ? dev->vtable.GetDeviceProcAddr(dev->device, name) : NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL AllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *info,
                                                             VkDescriptorSet *sets) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    PFN_vkAllocateDescriptorSets call = (PFN_vkAllocateDescriptorSets)native_proc(dev, "vkAllocateDescriptorSets");
    if (!dev || !call) return VK_ERROR_INITIALIZATION_FAILED;
    if (!dev->rtp) return call(device, info, sets);
    if (!info || !sets) return VK_ERROR_INITIALIZATION_FAILED;
    if ((uint64_t)info->descriptorSetCount * sizeof(*sets) > SIZE_MAX) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(sets, 0, (size_t)info->descriptorSetCount * sizeof(*sets));
    if ((uint64_t)info->descriptorSetCount * sizeof(VkDescriptorSetLayout) > SIZE_MAX) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkDescriptorSetLayout *layouts = info->descriptorSetCount ? malloc((size_t)info->descriptorSetCount * sizeof(*layouts)) : NULL;
    if (info->descriptorSetCount && !layouts) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < info->descriptorSetCount; ++i) {
        layouts[i] = vkrt_rtp_native_descriptor_layout(dev, info->pSetLayouts[i]);
        if (!layouts[i]) {
            free(layouts);
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    VkDescriptorSetAllocateInfo native = *info;
    native.pSetLayouts = layouts;
    VkResult result = call(device, &native, sets);
    free(layouts);
    return result;
}

static VKAPI_ATTR void VKAPI_CALL GetDescriptorSetLayoutSizeEXT(VkDevice device, VkDescriptorSetLayout layout, VkDeviceSize *size) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    PFN_vkGetDescriptorSetLayoutSizeEXT call =
        (PFN_vkGetDescriptorSetLayoutSizeEXT)native_proc(dev, "vkGetDescriptorSetLayoutSizeEXT");
    VkDescriptorSetLayout native = vkrt_rtp_native_descriptor_layout(dev, layout);
    if (call && native)
        call(device, native, size);
    else if (size)
        *size = 0;
}

static VKAPI_ATTR void VKAPI_CALL GetDescriptorSetLayoutBindingOffsetEXT(VkDevice device, VkDescriptorSetLayout layout,
                                                                         uint32_t binding, VkDeviceSize *offset) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    PFN_vkGetDescriptorSetLayoutBindingOffsetEXT call =
        (PFN_vkGetDescriptorSetLayoutBindingOffsetEXT)native_proc(dev, "vkGetDescriptorSetLayoutBindingOffsetEXT");
    VkDescriptorSetLayout native = vkrt_rtp_native_descriptor_layout(dev, layout);
    if (call && native)
        call(device, native, binding, offset);
    else if (offset)
        *offset = 0;
}

static VKAPI_ATTR void VKAPI_CALL GetDescriptorSetLayoutHostMappingInfoVALVE(VkDevice device,
                                                                             const VkDescriptorSetBindingReferenceVALVE *reference,
                                                                             VkDescriptorSetLayoutHostMappingInfoVALVE *mapping) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    PFN_vkGetDescriptorSetLayoutHostMappingInfoVALVE call =
        (PFN_vkGetDescriptorSetLayoutHostMappingInfoVALVE)native_proc(dev, "vkGetDescriptorSetLayoutHostMappingInfoVALVE");
    if (dev && call && reference) {
        if (!dev->rtp) {
            call(device, reference, mapping);
            return;
        }
        VkDescriptorSetBindingReferenceVALVE native = *reference;
        native.descriptorSetLayout = vkrt_rtp_native_descriptor_layout(dev, reference->descriptorSetLayout);
        if (native.descriptorSetLayout) {
            call(device, &native, mapping);
            return;
        }
    }
    if (mapping) {
        mapping->descriptorOffset = 0;
        mapping->descriptorSize = 0;
    }
}

PFN_vkVoidFunction vkrt_descriptor_sets_proc(const char *name) {
    if (!strcmp(name, "vkCreateDescriptorSetLayout")) return (PFN_vkVoidFunction)vkrt_CreateDescriptorSetLayout;
    if (!strcmp(name, "vkDestroyDescriptorSetLayout")) return (PFN_vkVoidFunction)vkrt_DestroyDescriptorSetLayout;
    if (!strcmp(name, "vkAllocateDescriptorSets")) return (PFN_vkVoidFunction)AllocateDescriptorSets;
    if (!strcmp(name, "vkGetDescriptorSetLayoutSizeEXT")) return (PFN_vkVoidFunction)GetDescriptorSetLayoutSizeEXT;
    if (!strcmp(name, "vkGetDescriptorSetLayoutBindingOffsetEXT"))
        return (PFN_vkVoidFunction)GetDescriptorSetLayoutBindingOffsetEXT;
    if (!strcmp(name, "vkGetDescriptorSetLayoutHostMappingInfoVALVE"))
        return (PFN_vkVoidFunction)GetDescriptorSetLayoutHostMappingInfoVALVE;
    return NULL;
}
