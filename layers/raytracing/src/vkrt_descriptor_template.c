/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vkrt_descriptor_template.h"
#include "vkrt_replay_capture_generated.h"
#include "vkrt_layer.h"
#include "vkrt_rt_capabilities.h"
#include "vkrt_debug_objects.h"
#include <stddef.h>

union descriptor_extension {
    VkWriteDescriptorSetAccelerationStructureKHR acceleration;
    VkWriteDescriptorSetAccelerationStructureNV acceleration_nv;
    VkWriteDescriptorSetInlineUniformBlock inline_uniform;
};
struct decoded_template {
    VkWriteDescriptorSet *writes;
    union descriptor_extension *extensions;
    void **payloads;
    uint32_t count;
    VkPipelineBindPoint bind_point;
    struct descriptor_template *owner;
};
struct descriptor_template {
    struct descriptor_template *next;
    struct vkrt_device_data *dev;
    VkDescriptorUpdateTemplate handle;
    VkDescriptorUpdateTemplateType type;
    VkPipelineBindPoint bind_point;
    uint32_t count, readers;
    bool removed, custom;
    VkAllocationCallbacks allocator;
    vkrt_mutex_t decode_lock;
    bool decode_lock_initialized;
    struct decoded_template storage;
    VkDescriptorUpdateTemplateEntry entries[];
};
#ifdef _WIN32
static vkrt_mutex_t templates_lock = SRWLOCK_INIT;
#else
static vkrt_mutex_t templates_lock = PTHREAD_MUTEX_INITIALIZER;
#endif
static struct descriptor_template *templates;

static void *template_allocate(struct descriptor_template *record, size_t size) {
    void *memory = record->custom ? record->allocator.pfnAllocation(record->allocator.pUserData, size, _Alignof(max_align_t),
                                                                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT)
                                  : malloc(size);
    if (memory) memset(memory, 0, size);
    return memory;
}
static void template_memory_free(struct descriptor_template *record, void *memory) {
    if (!memory) return;
    if (record->custom)
        record->allocator.pfnFree(record->allocator.pUserData, memory);
    else
        free(memory);
}
static VkResult template_prepare_storage(struct descriptor_template *record);
static void template_free(struct descriptor_template *record) {
    if (record->storage.payloads)
        for (uint32_t i = 0; i < record->count; ++i) template_memory_free(record, record->storage.payloads[i]);
    template_memory_free(record, record->storage.payloads);
    template_memory_free(record, record->storage.extensions);
    template_memory_free(record, record->storage.writes);
#ifndef _WIN32
    if (record->decode_lock_initialized) pthread_mutex_destroy(&record->decode_lock);
#endif
    if (record->custom) {
        VkAllocationCallbacks a = record->allocator;
        a.pfnFree(a.pUserData, record);
    } else {
        free(record);
    }
}
static struct descriptor_template *template_acquire(struct vkrt_device_data *dev, VkDescriptorUpdateTemplate handle) {
    vkrt_mutex_lock(&templates_lock);
    struct descriptor_template *record = templates;
    while (record && (record->dev != dev || record->handle != handle)) record = record->next;
    if (record && record->readers != UINT32_MAX)
        ++record->readers;
    else
        record = NULL;
    vkrt_mutex_unlock(&templates_lock);
    return record;
}
static void template_release(struct descriptor_template *record) {
    vkrt_mutex_lock(&templates_lock);
    bool destroy = !--record->readers && record->removed;
    vkrt_mutex_unlock(&templates_lock);
    if (destroy) template_free(record);
}
static bool template_remove(struct vkrt_device_data *dev, VkDescriptorUpdateTemplate handle) {
    vkrt_mutex_lock(&templates_lock);
    struct descriptor_template **link = &templates;
    while (*link && ((*link)->dev != dev || (*link)->handle != handle)) link = &(*link)->next;
    struct descriptor_template *record = *link;
    bool destroy = false;
    if (record) {
        *link = record->next;
        record->removed = true;
        destroy = !record->readers;
    }
    vkrt_mutex_unlock(&templates_lock);
    if (record) vkrt_debug_object_forget(dev, VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE, VKRT_HKEY(handle));
    if (destroy) template_free(record);
    return record != NULL;
}
bool vkrt_descriptor_template_owns(struct vkrt_device_data *dev, uint64_t handle) {
    vkrt_mutex_lock(&templates_lock);
    const struct descriptor_template *record = templates;
    while (record && (record->dev != dev || VKRT_HKEY(record->handle) != handle)) record = record->next;
    bool found = record != NULL;
    vkrt_mutex_unlock(&templates_lock);
    return found;
}

static PFN_vkVoidFunction native_proc(struct vkrt_device_data *dev, const char *core, const char *alias) {
    /* An application below the promoted core version may have enabled only
     * the extension spelling. Prefer that legal alias when it is available. */
    PFN_vkVoidFunction proc = dev->vtable.GetDeviceProcAddr(dev->device, alias);
    return proc ? proc : dev->vtable.GetDeviceProcAddr(dev->device, core);
}
static VKAPI_ATTR VkResult VKAPI_CALL CreateDescriptorUpdateTemplate(VkDevice device,
                                                                     const VkDescriptorUpdateTemplateCreateInfo *info,
                                                                     const VkAllocationCallbacks *allocator,
                                                                     VkDescriptorUpdateTemplate *out) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev || !info || !out) return VK_ERROR_INITIALIZATION_FAILED;
    if (!dev->virtual_rt) {
        PFN_vkCreateDescriptorUpdateTemplate create =
            (void *)native_proc(dev, "vkCreateDescriptorUpdateTemplate", "vkCreateDescriptorUpdateTemplateKHR");
        return create ? create(device, info, allocator, out) : VK_ERROR_FEATURE_NOT_PRESENT;
    }
    *out = VK_NULL_HANDLE;
    if (info->pNext || info->flags ||
        (info->templateType != VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET &&
         info->templateType != VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR) ||
        (info->descriptorUpdateEntryCount && !info->pDescriptorUpdateEntries))
        return VK_ERROR_FEATURE_NOT_PRESENT;
#if SIZE_MAX <= UINT32_MAX
    if (info->descriptorUpdateEntryCount >
        (SIZE_MAX - offsetof(struct descriptor_template, entries)) / sizeof(VkDescriptorUpdateTemplateEntry))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
#endif
    size_t size = offsetof(struct descriptor_template, entries) +
                  (size_t)info->descriptorUpdateEntryCount * sizeof(VkDescriptorUpdateTemplateEntry);
    struct descriptor_template *record =
        allocator ? allocator->pfnAllocation(allocator->pUserData, size, _Alignof(max_align_t), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT)
                  : malloc(size);
    if (!record) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memset(record, 0, size);
    record->dev = dev;
    record->handle = (VkDescriptorUpdateTemplate)(uintptr_t)record;
    record->type = info->templateType;
    record->bind_point = info->pipelineBindPoint;
    record->count = info->descriptorUpdateEntryCount;
    if (allocator) {
        record->custom = true;
        record->allocator = *allocator;
    }
    if (record->count) memcpy(record->entries, info->pDescriptorUpdateEntries, record->count * sizeof(*record->entries));
    VkResult result = template_prepare_storage(record);
    if (result != VK_SUCCESS) {
        template_free(record);
        return result;
    }
#ifdef _WIN32
    vkrt_mutex_init(&record->decode_lock);
#else
    if (pthread_mutex_init(&record->decode_lock, NULL)) {
        template_free(record);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
#endif
    record->decode_lock_initialized = true;
    vkrt_mutex_lock(&templates_lock);
    record->next = templates;
    templates = record;
    vkrt_mutex_unlock(&templates_lock);
    *out = record->handle;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL DestroyDescriptorUpdateTemplate(VkDevice device, VkDescriptorUpdateTemplate handle,
                                                                  const VkAllocationCallbacks *allocator) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev || !handle || template_remove(dev, handle)) return;
    PFN_vkDestroyDescriptorUpdateTemplate destroy =
        (void *)native_proc(dev, "vkDestroyDescriptorUpdateTemplate", "vkDestroyDescriptorUpdateTemplateKHR");
    if (destroy) destroy(device, handle, allocator);
}

static void decoded_finish(struct decoded_template *decoded) {
    if (decoded->owner) {
        vkrt_mutex_unlock(&decoded->owner->decode_lock);
        template_release(decoded->owner);
    }
    *decoded = (struct decoded_template){0};
}
static size_t descriptor_size(VkDescriptorType type) {
    switch (type) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            return sizeof(VkDescriptorImageInfo);
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            return sizeof(VkDescriptorBufferInfo);
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return sizeof(VkBufferView);
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
            return sizeof(VkAccelerationStructureKHR);
        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV:
            return sizeof(VkAccelerationStructureNV);
        case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
            return 1;
        default:
            return 0;
    }
}
/* Template application is a void operation. Reserve all decode storage at
 * creation, where allocation failures can be returned, and serialize borrowing
 * it until the ordinary update/push or owning journal copy has consumed it. */
static VkResult template_prepare_storage(struct descriptor_template *record) {
    struct decoded_template *storage = &record->storage;
    storage->count = record->count;
    storage->bind_point = record->bind_point;
    if (!record->count) return VK_SUCCESS;
    if ((uint64_t)record->count * sizeof(*storage->writes) > SIZE_MAX ||
        (uint64_t)record->count * sizeof(*storage->extensions) > SIZE_MAX ||
        (uint64_t)record->count * sizeof(*storage->payloads) > SIZE_MAX)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    storage->writes = template_allocate(record, (size_t)record->count * sizeof(*storage->writes));
    storage->extensions = template_allocate(record, (size_t)record->count * sizeof(*storage->extensions));
    storage->payloads = template_allocate(record, (size_t)record->count * sizeof(*storage->payloads));
    if (!storage->writes || !storage->extensions || !storage->payloads) return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (uint32_t i = 0; i < record->count; ++i) {
        size_t size = descriptor_size(record->entries[i].descriptorType);
        if (!size) return VK_ERROR_FEATURE_NOT_PRESENT;
        if ((uint64_t)record->entries[i].descriptorCount * size > SIZE_MAX) return VK_ERROR_OUT_OF_HOST_MEMORY;
        if (!record->entries[i].descriptorCount) continue;
        storage->payloads[i] = template_allocate(record, (size_t)record->entries[i].descriptorCount * size);
        if (!storage->payloads[i]) return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return VK_SUCCESS;
}
static VkResult decode_template(struct vkrt_device_data *dev, VkDescriptorUpdateTemplate handle, const void *data, bool push,
                                VkDescriptorSet destination, struct decoded_template *decoded) {
    *decoded = (struct decoded_template){0};
    struct descriptor_template *record = template_acquire(dev, handle);
    if (!record) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult result = VK_SUCCESS;
    if (record->type !=
        (push ? VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR : VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET)) {
        result = VK_ERROR_INITIALIZATION_FAILED;
        goto done;
    }
    vkrt_mutex_lock(&record->decode_lock);
    *decoded = record->storage;
    decoded->owner = record;
    if (!record->count) goto done;
    for (uint32_t i = 0; i < record->count; ++i) {
        const VkDescriptorUpdateTemplateEntry *entry = &record->entries[i];
        size_t size = descriptor_size(entry->descriptorType);
        if (!size || !data || entry->offset > SIZE_MAX || entry->stride > SIZE_MAX) {
            result = VK_ERROR_FEATURE_NOT_PRESENT;
            goto done;
        }
        bool inline_uniform = entry->descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK;
        size_t step = inline_uniform ? 1 : (size_t)entry->stride;
        size_t end = (size_t)entry->offset;
        if (entry->descriptorCount && ((step && (size_t)(entry->descriptorCount - 1) > (SIZE_MAX - end) / step) ||
                                       size > SIZE_MAX - end - (size_t)(entry->descriptorCount - 1) * step ||
                                       (size_t)entry->descriptorCount > SIZE_MAX / size)) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto done;
        }
        if (entry->descriptorCount) end += (size_t)(entry->descriptorCount - 1) * step + size;
        if (end > UINTPTR_MAX - (uintptr_t)data) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto done;
        }
        if (entry->descriptorCount) {
            for (uint32_t j = 0; j < entry->descriptorCount; ++j)
                memcpy((uint8_t *)decoded->payloads[i] + (size_t)j * size,
                       (const uint8_t *)data + (size_t)entry->offset + (size_t)j * step, size);
        }
        VkWriteDescriptorSet *write = &decoded->writes[i];
        *write = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                        .dstSet = destination,
                                        .dstBinding = entry->dstBinding,
                                        .dstArrayElement = entry->dstArrayElement,
                                        .descriptorCount = entry->descriptorCount,
                                        .descriptorType = entry->descriptorType};
        union descriptor_extension *extension = &decoded->extensions[i];
        switch (entry->descriptorType) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                write->pImageInfo = decoded->payloads[i];
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                write->pBufferInfo = decoded->payloads[i];
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                write->pTexelBufferView = decoded->payloads[i];
                break;
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                extension->acceleration = (VkWriteDescriptorSetAccelerationStructureKHR){
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                    .accelerationStructureCount = entry->descriptorCount,
                    .pAccelerationStructures = decoded->payloads[i]};
                write->pNext = &extension->acceleration;
                break;
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV:
                extension->acceleration_nv = (VkWriteDescriptorSetAccelerationStructureNV){
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_NV,
                    .accelerationStructureCount = entry->descriptorCount,
                    .pAccelerationStructures = decoded->payloads[i]};
                write->pNext = &extension->acceleration_nv;
                break;
            case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
                extension->inline_uniform =
                    (VkWriteDescriptorSetInlineUniformBlock){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK,
                                                             .dataSize = entry->descriptorCount,
                                                             .pData = decoded->payloads[i]};
                write->pNext = &extension->inline_uniform;
                break;
            default:
                break;
        }
    }
done:
    if (result != VK_SUCCESS) {
        if (decoded->owner)
            decoded_finish(decoded);
        else
            template_release(record);
    }
    return result;
}

static VkShaderStageFlags bind_point_stages(VkPipelineBindPoint point) {
    return point == VK_PIPELINE_BIND_POINT_COMPUTE           ? VK_SHADER_STAGE_COMPUTE_BIT
           : point == VK_PIPELINE_BIND_POINT_GRAPHICS        ? VK_SHADER_STAGE_ALL_GRAPHICS
           : point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR ? VKRT_RTP_SHADER_STAGES
                                                             : 0;
}
VkResult vkrt_descriptor_template_capture(struct vkrt_replay_context *context, const VkPushDescriptorSetWithTemplateInfo *info) {
    struct vkrt_cmd_buffer_data *cb = context ? context->user : NULL;
    if (!cb || !info) return VK_ERROR_INITIALIZATION_FAILED;
    struct decoded_template decoded;
    VkResult result = decode_template(cb->device_data, info->descriptorUpdateTemplate, info->pData, true, VK_NULL_HANDLE, &decoded);
    if (result != VK_SUCCESS) return result;
    VkPipelineLayout capture_layout = info->layout, temporary_layout = VK_NULL_HANDLE;
    PFN_vkDestroyPipelineLayout destroy = NULL;
    if (info->pNext) {
        const VkBaseInStructure *next = info->pNext;
        /* Maintenance6 has one inline layout definition. An existing layout
         * ignores it; do not clone ignored descriptor-layout pointers. */
        if (next->sType != VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO || next->pNext)
            result = VK_ERROR_FEATURE_NOT_PRESENT;
        else if (!capture_layout) {
            PFN_vkCreatePipelineLayout create = (void *)vkrt_device_direct_proc(cb->device_data, "vkCreatePipelineLayout");
            destroy = (void *)vkrt_device_direct_proc(cb->device_data, "vkDestroyPipelineLayout");
            /* The integration must retain a semantic layout identity before
             * its temporary public native handle is destroyed below. */
            if (!create || !destroy || !context->normalize_pipeline_layout)
                result = VK_ERROR_FEATURE_NOT_PRESENT;
            else
                result = create(cb->device_data->device, (const VkPipelineLayoutCreateInfo *)next, NULL, &temporary_layout);
            if (result == VK_SUCCESS) capture_layout = temporary_layout;
        }
    }
    if (result == VK_SUCCESS) {
        PFN_vkrt_capture_CmdPushDescriptorSetKHR capture = (void *)vkrt_replay_capture_proc("vkCmdPushDescriptorSetKHR");
        result = capture(context, decoded.bind_point, capture_layout, info->set, decoded.count, decoded.writes);
    }
    if (temporary_layout) destroy(cb->device_data->device, temporary_layout, NULL);
    decoded_finish(&decoded);
    return result;
}
static VKAPI_ATTR void VKAPI_CALL CmdPushDescriptorSetWithTemplate2(VkCommandBuffer command,
                                                                    const VkPushDescriptorSetWithTemplateInfo *info) {
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, command);
    if (!cb || cb->record_result != VK_SUCCESS) return;
    struct vkrt_device_data *dev = cb->device_data;
    if (!dev->virtual_rt) {
        PFN_vkCmdPushDescriptorSetWithTemplate2 proc =
            (void *)native_proc(dev, "vkCmdPushDescriptorSetWithTemplate2", "vkCmdPushDescriptorSetWithTemplate2KHR");
        if (proc) proc(command, info);
        return;
    }
    struct decoded_template decoded;
    VkResult result = decode_template(dev, info->descriptorUpdateTemplate, info->pData, true, VK_NULL_HANDLE, &decoded);
    if (result != VK_SUCCESS) {
        cb->record_result = result;
        return;
    }
    if (info->pNext) {
        const VkPushDescriptorSetInfo push = {.sType = VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_INFO,
                                              .pNext = info->pNext,
                                              .stageFlags = bind_point_stages(decoded.bind_point),
                                              .layout = info->layout,
                                              .set = info->set,
                                              .descriptorWriteCount = decoded.count,
                                              .pDescriptorWrites = decoded.writes};
        PFN_vkCmdPushDescriptorSet2KHR proc = (void *)vkrt_device_direct_proc(dev, "vkCmdPushDescriptorSet2KHR");
        if (proc)
            proc(command, &push);
        else
            cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
    } else {
        PFN_vkCmdPushDescriptorSetKHR proc = (void *)vkrt_device_direct_proc(dev, "vkCmdPushDescriptorSetKHR");
        if (proc)
            proc(command, decoded.bind_point, info->layout, info->set, decoded.count, decoded.writes);
        else
            cb->record_result = VK_ERROR_FEATURE_NOT_PRESENT;
    }
    decoded_finish(&decoded);
}
static VKAPI_ATTR void VKAPI_CALL CmdPushDescriptorSetWithTemplate(VkCommandBuffer command, VkDescriptorUpdateTemplate update,
                                                                   VkPipelineLayout layout, uint32_t set, const void *data) {
    const VkPushDescriptorSetWithTemplateInfo info = {.sType = VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_WITH_TEMPLATE_INFO,
                                                      .descriptorUpdateTemplate = update,
                                                      .layout = layout,
                                                      .set = set,
                                                      .pData = data};
    struct vkrt_cmd_buffer_data *cb = VKRT_FIND(struct vkrt_cmd_buffer_data, command);
    if (cb && !cb->device_data->virtual_rt) {
        PFN_vkCmdPushDescriptorSetWithTemplate proc =
            (void *)native_proc(cb->device_data, "vkCmdPushDescriptorSetWithTemplate", "vkCmdPushDescriptorSetWithTemplateKHR");
        if (proc) proc(command, update, layout, set, data);
    } else {
        CmdPushDescriptorSetWithTemplate2(command, &info);
    }
}
static VKAPI_ATTR void VKAPI_CALL UpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set,
                                                                  VkDescriptorUpdateTemplate update, const void *data) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev) return;
    if (!dev->virtual_rt) {
        PFN_vkUpdateDescriptorSetWithTemplate proc =
            (void *)native_proc(dev, "vkUpdateDescriptorSetWithTemplate", "vkUpdateDescriptorSetWithTemplateKHR");
        if (proc) proc(device, set, update, data);
        return;
    }
    struct decoded_template decoded;
    VkResult result = decode_template(dev, update, data, false, set, &decoded);
    if (result != VK_SUCCESS) {
        VKRT_LOG_ERR("descriptor update template decode failed: %d", result);
        return;
    }
    PFN_vkUpdateDescriptorSets proc = (void *)vkrt_device_direct_proc(dev, "vkUpdateDescriptorSets");
    if (proc) proc(device, decoded.count, decoded.writes, 0, NULL);
    decoded_finish(&decoded);
}
void vkrt_descriptor_template_finish(struct vkrt_device_data *dev) {
    for (;;) {
        vkrt_mutex_lock(&templates_lock);
        struct descriptor_template *record = templates;
        while (record && record->dev != dev) record = record->next;
        VkDescriptorUpdateTemplate handle = record ? record->handle : VK_NULL_HANDLE;
        vkrt_mutex_unlock(&templates_lock);
        if (!handle) break;
        template_remove(dev, handle);
    }
}
PFN_vkVoidFunction vkrt_descriptor_template_proc(const char *name) {
#define ENTRY(command) \
    if (!strcmp(name, "vk" #command) || !strcmp(name, "vk" #command "KHR")) return (PFN_vkVoidFunction)command
    ENTRY(CreateDescriptorUpdateTemplate);
    ENTRY(DestroyDescriptorUpdateTemplate);
    ENTRY(UpdateDescriptorSetWithTemplate);
    ENTRY(CmdPushDescriptorSetWithTemplate);
    ENTRY(CmdPushDescriptorSetWithTemplate2);
#undef ENTRY
    return NULL;
}
