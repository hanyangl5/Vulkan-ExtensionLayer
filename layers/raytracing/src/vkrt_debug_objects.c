/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vkrt_layer.h"
#include "vkrt_debug_objects.h"
#include <stddef.h>

struct private_value {
    struct private_value *next;
    VkPrivateDataSlot slot;
    uint64_t data;
};
struct object_tag {
    struct object_tag *next;
    uint64_t name;
    size_t size;
    unsigned char bytes[];
};
struct object_annotation {
    struct object_annotation *next;
    VkObjectType type;
    uint64_t handle;
    char *name;
    struct object_tag *tags;
    struct private_value *private_data;
};
/* Every owned shader module uses its public identity here, including modules
 * backed by a native shader. Native module values can alias each other or a
 * raw RT token, so annotation/private-data sidecars must not unwrap or merge
 * them. The typed ownership callback covers both kinds; no token reaches ICD
 * debug/private-data calls. */
struct debug_objects {
    struct debug_objects *next;
    struct vkrt_device_data *dev;
    vkrt_debug_object_owns owns;
    bool custom;
    VkAllocationCallbacks allocator;
    struct object_annotation *objects;
};
#ifdef _WIN32
static vkrt_mutex_t debug_lock = SRWLOCK_INIT;
#else
static vkrt_mutex_t debug_lock = PTHREAD_MUTEX_INITIALIZER;
#endif
static struct debug_objects *devices;

static void *debug_allocate(struct debug_objects *state, size_t size) {
    return state->custom ? state->allocator.pfnAllocation(state->allocator.pUserData, size, _Alignof(max_align_t),
                                                          VK_SYSTEM_ALLOCATION_SCOPE_DEVICE)
                         : malloc(size);
}
static void debug_free(struct debug_objects *state, void *memory) {
    if (!memory) return;
    if (state->custom)
        state->allocator.pfnFree(state->allocator.pUserData, memory);
    else
        free(memory);
}
/* Caller holds debug_lock. */
static struct debug_objects *find_device(struct vkrt_device_data *dev) {
    struct debug_objects *state = devices;
    while (state && state->dev != dev) state = state->next;
    return state;
}
static struct object_annotation **find_object(struct debug_objects *state, VkObjectType type, uint64_t handle) {
    struct object_annotation **link = &state->objects;
    while (*link && ((*link)->type != type || (*link)->handle != handle)) link = &(*link)->next;
    return link;
}
static void free_object(struct debug_objects *state, struct object_annotation *object) {
    while (object->private_data) {
        struct private_value *value = object->private_data;
        object->private_data = value->next;
        debug_free(state, value);
    }
    while (object->tags) {
        struct object_tag *tag = object->tags;
        object->tags = tag->next;
        debug_free(state, tag);
    }
    debug_free(state, object->name);
    debug_free(state, object);
}
VkResult vkrt_debug_objects_init(struct vkrt_device_data *dev, const VkAllocationCallbacks *allocator,
                                 vkrt_debug_object_owns owns) {
    if (!dev || !owns) return VK_ERROR_INITIALIZATION_FAILED;
    if (!allocator && dev->queue_families_have_allocator) allocator = &dev->queue_families_allocator;
    if (!allocator && dev->instance_data && dev->instance_data->has_allocator) allocator = &dev->instance_data->allocator;
    struct debug_objects seed = {.dev = dev, .owns = owns};
    if (allocator) {
        seed.custom = true;
        seed.allocator = *allocator;
    }
    vkrt_mutex_lock(&debug_lock);
    if (find_device(dev)) {
        vkrt_mutex_unlock(&debug_lock);
        return VK_SUCCESS;
    }
    struct debug_objects *state = debug_allocate(&seed, sizeof(*state));
    if (!state) {
        vkrt_mutex_unlock(&debug_lock);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *state = seed;
    state->next = devices;
    devices = state;
    vkrt_mutex_unlock(&debug_lock);
    return VK_SUCCESS;
}
void vkrt_debug_object_forget(struct vkrt_device_data *dev, VkObjectType type, uint64_t handle) {
    vkrt_mutex_lock(&debug_lock);
    struct debug_objects *state = find_device(dev);
    if (state) {
        struct object_annotation **link = find_object(state, type, handle);
        if (*link) {
            struct object_annotation *object = *link;
            *link = object->next;
            free_object(state, object);
        }
    }
    vkrt_mutex_unlock(&debug_lock);
}
void vkrt_debug_objects_finish(struct vkrt_device_data *dev) {
    vkrt_mutex_lock(&debug_lock);
    struct debug_objects **link = &devices;
    while (*link && (*link)->dev != dev) link = &(*link)->next;
    struct debug_objects *state = *link;
    if (state) {
        *link = state->next;
        while (state->objects) {
            struct object_annotation *object = state->objects;
            state->objects = object->next;
            free_object(state, object);
        }
        debug_free(state, state);
    }
    vkrt_mutex_unlock(&debug_lock);
}
static VkResult store_name(struct vkrt_device_data *dev, VkObjectType type, uint64_t handle, const char *name, bool *handled) {
    *handled = false;
    vkrt_mutex_lock(&debug_lock);
    struct debug_objects *state = find_device(dev);
    if (!state || !state->owns(dev, type, handle)) {
        vkrt_mutex_unlock(&debug_lock);
        return VK_SUCCESS;
    }
    *handled = true;
    struct object_annotation **link = find_object(state, type, handle);
    if (!name || !name[0]) {
        if (*link) {
            debug_free(state, (*link)->name);
            (*link)->name = NULL;
            if (!(*link)->tags && !(*link)->private_data) {
                struct object_annotation *old = *link;
                *link = old->next;
                free_object(state, old);
            }
        }
        vkrt_mutex_unlock(&debug_lock);
        return VK_SUCCESS;
    }
    size_t size = strlen(name) + 1;
    char *copy = debug_allocate(state, size);
    if (!copy) {
        vkrt_mutex_unlock(&debug_lock);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    memcpy(copy, name, size);
    if (!*link) {
        struct object_annotation *object = debug_allocate(state, sizeof(*object));
        if (!object) {
            debug_free(state, copy);
            vkrt_mutex_unlock(&debug_lock);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        *object = (struct object_annotation){.type = type, .handle = handle};
        *link = object;
    }
    debug_free(state, (*link)->name);
    (*link)->name = copy;
    vkrt_mutex_unlock(&debug_lock);
    return VK_SUCCESS;
}
static VkResult store_tag(struct vkrt_device_data *dev, VkObjectType type, uint64_t handle, uint64_t name, size_t size,
                          const void *bytes, bool *handled) {
    *handled = false;
    vkrt_mutex_lock(&debug_lock);
    struct debug_objects *state = find_device(dev);
    if (!state || !state->owns(dev, type, handle)) {
        vkrt_mutex_unlock(&debug_lock);
        return VK_SUCCESS;
    }
    *handled = true;
    if (size > SIZE_MAX - offsetof(struct object_tag, bytes)) {
        vkrt_mutex_unlock(&debug_lock);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (!size || !bytes) {
        vkrt_mutex_unlock(&debug_lock);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct object_tag *tag = debug_allocate(state, offsetof(struct object_tag, bytes) + size);
    if (!tag) {
        vkrt_mutex_unlock(&debug_lock);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *tag = (struct object_tag){.name = name, .size = size};
    memcpy(tag->bytes, bytes, size);
    struct object_annotation **link = find_object(state, type, handle);
    if (!*link) {
        struct object_annotation *object = debug_allocate(state, sizeof(*object));
        if (!object) {
            debug_free(state, tag);
            vkrt_mutex_unlock(&debug_lock);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        *object = (struct object_annotation){.type = type, .handle = handle};
        *link = object;
    }
    struct object_tag **previous = &(*link)->tags;
    while (*previous && (*previous)->name != name) previous = &(*previous)->next;
    struct object_tag *old = *previous;
    tag->next = old ? old->next : NULL;
    *previous = tag;
    debug_free(state, old);
    vkrt_mutex_unlock(&debug_lock);
    return VK_SUCCESS;
}
static PFN_vkVoidFunction native_proc(struct vkrt_device_data *dev, const char *name) {
    return dev->vtable.GetDeviceProcAddr ? dev->vtable.GetDeviceProcAddr(dev->device, name) : NULL;
}
static PFN_vkVoidFunction private_proc(struct vkrt_device_data *dev, const char *core, const char *extension) {
    /* Applications below Vulkan1.3 can enable the EXT spelling. The native
     * dispatch decides which alias is legal; slot objects are never wrapped. */
    PFN_vkVoidFunction call = native_proc(dev, extension);
    return call ? call : native_proc(dev, core);
}
static VkResult store_private(struct vkrt_device_data *dev, VkObjectType type, uint64_t handle, VkPrivateDataSlot slot,
                              uint64_t data, bool *handled) {
    *handled = false;
    vkrt_mutex_lock(&debug_lock);
    struct debug_objects *state = find_device(dev);
    if (!state || !state->owns(dev, type, handle)) {
        vkrt_mutex_unlock(&debug_lock);
        return VK_SUCCESS;
    }
    *handled = true;
    struct object_annotation **link = find_object(state, type, handle);
    if (*link) {
        struct private_value **value = &(*link)->private_data;
        while (*value && (*value)->slot != slot) value = &(*value)->next;
        if (*value) {
            if (data)
                (*value)->data = data;
            else {
                struct private_value *old = *value;
                *value = old->next;
                debug_free(state, old);
                if (!(*link)->name && !(*link)->tags && !(*link)->private_data) {
                    struct object_annotation *object = *link;
                    *link = object->next;
                    free_object(state, object);
                }
            }
            vkrt_mutex_unlock(&debug_lock);
            return VK_SUCCESS;
        }
    }
    // A zero value is the specified initial state and needs no sidecar row.
    if (!data) {
        vkrt_mutex_unlock(&debug_lock);
        return VK_SUCCESS;
    }
    struct private_value *value = debug_allocate(state, sizeof(*value));
    if (!value) {
        vkrt_mutex_unlock(&debug_lock);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    *value = (struct private_value){.slot = slot, .data = data};
    if (!*link) {
        struct object_annotation *object = debug_allocate(state, sizeof(*object));
        if (!object) {
            debug_free(state, value);
            vkrt_mutex_unlock(&debug_lock);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        *object = (struct object_annotation){.type = type, .handle = handle};
        *link = object;
    }
    value->next = (*link)->private_data;
    (*link)->private_data = value;
    vkrt_mutex_unlock(&debug_lock);
    return VK_SUCCESS;
}
static bool read_private(struct vkrt_device_data *dev, VkObjectType type, uint64_t handle, VkPrivateDataSlot slot, uint64_t *data) {
    vkrt_mutex_lock(&debug_lock);
    struct debug_objects *state = find_device(dev);
    if (!state || !state->owns(dev, type, handle)) {
        vkrt_mutex_unlock(&debug_lock);
        return false;
    }
    *data = 0;
    const struct object_annotation *object = *find_object(state, type, handle);
    if (object)
        for (const struct private_value *value = object->private_data; value; value = value->next)
            if (value->slot == slot) {
                *data = value->data;
                break;
            }
    vkrt_mutex_unlock(&debug_lock);
    return true;
}
static void clear_private_slot(struct vkrt_device_data *dev, VkPrivateDataSlot slot) {
    vkrt_mutex_lock(&debug_lock);
    struct debug_objects *state = find_device(dev);
    if (state) {
        struct object_annotation **link = &state->objects;
        while (*link) {
            struct object_annotation *object = *link;
            struct private_value **value = &object->private_data;
            while (*value) {
                if ((*value)->slot == slot) {
                    struct private_value *old = *value;
                    *value = old->next;
                    debug_free(state, old);
                } else
                    value = &(*value)->next;
            }
            if (!object->name && !object->tags && !object->private_data) {
                *link = object->next;
                free_object(state, object);
            } else
                link = &object->next;
        }
    }
    vkrt_mutex_unlock(&debug_lock);
}
static VKAPI_ATTR VkResult VKAPI_CALL SetPrivateData(VkDevice device, VkObjectType type, uint64_t handle, VkPrivateDataSlot slot,
                                                     uint64_t data) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev) return VK_ERROR_INITIALIZATION_FAILED;
    bool handled;
    VkResult result = store_private(dev, type, handle, slot, data, &handled);
    if (handled) return result;
    PFN_vkSetPrivateData call = (void *)private_proc(dev, "vkSetPrivateData", "vkSetPrivateDataEXT");
    return call ? call(device, type, handle, slot, data) : VK_ERROR_EXTENSION_NOT_PRESENT;
}
static VKAPI_ATTR void VKAPI_CALL GetPrivateData(VkDevice device, VkObjectType type, uint64_t handle, VkPrivateDataSlot slot,
                                                 uint64_t *data) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev || !data) return;
    if (read_private(dev, type, handle, slot, data)) return;
    PFN_vkGetPrivateData call = (void *)private_proc(dev, "vkGetPrivateData", "vkGetPrivateDataEXT");
    if (call)
        call(device, type, handle, slot, data);
    else
        *data = 0;
}
static VKAPI_ATTR void VKAPI_CALL DestroyPrivateDataSlot(VkDevice device, VkPrivateDataSlot slot,
                                                         const VkAllocationCallbacks *allocator) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev) return;
    // Do this before the native destructor can make the slot's numeric value
    // available for reuse, including reentrant allocation in that destructor.
    clear_private_slot(dev, slot);
    PFN_vkDestroyPrivateDataSlot call = (void *)private_proc(dev, "vkDestroyPrivateDataSlot", "vkDestroyPrivateDataSlotEXT");
    if (call) call(device, slot, allocator);
}
static VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectNameEXT(VkDevice device, const VkDebugUtilsObjectNameInfoEXT *info) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev || !info) return VK_ERROR_INITIALIZATION_FAILED;
    bool handled;
    VkResult result = store_name(dev, info->objectType, info->objectHandle, info->pObjectName, &handled);
    if (handled) return result;
    PFN_vkSetDebugUtilsObjectNameEXT call = (void *)native_proc(dev, "vkSetDebugUtilsObjectNameEXT");
    return call ? call(device, info) : VK_ERROR_EXTENSION_NOT_PRESENT;
}
static VKAPI_ATTR VkResult VKAPI_CALL SetDebugUtilsObjectTagEXT(VkDevice device, const VkDebugUtilsObjectTagInfoEXT *info) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev || !info) return VK_ERROR_INITIALIZATION_FAILED;
    bool handled;
    VkResult result = store_tag(dev, info->objectType, info->objectHandle, info->tagName, info->tagSize, info->pTag, &handled);
    if (handled) return result;
    PFN_vkSetDebugUtilsObjectTagEXT call = (void *)native_proc(dev, "vkSetDebugUtilsObjectTagEXT");
    return call ? call(device, info) : VK_ERROR_EXTENSION_NOT_PRESENT;
}
static VkObjectType marker_type(VkDebugReportObjectTypeEXT type) {
    switch (type) {
        case VK_DEBUG_REPORT_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR_EXT:
            return VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR;
        case VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_EXT:
            return VK_OBJECT_TYPE_PIPELINE;
        case VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT:
            return VK_OBJECT_TYPE_SHADER_MODULE;
        case VK_DEBUG_REPORT_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_EXT:
            return VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE;
        case VK_DEBUG_REPORT_OBJECT_TYPE_QUERY_POOL_EXT:
            return VK_OBJECT_TYPE_QUERY_POOL;
        default:
            return VK_OBJECT_TYPE_UNKNOWN;
    }
}
static VKAPI_ATTR VkResult VKAPI_CALL DebugMarkerSetObjectNameEXT(VkDevice device, const VkDebugMarkerObjectNameInfoEXT *info) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev || !info) return VK_ERROR_INITIALIZATION_FAILED;
    bool handled;
    VkResult result = store_name(dev, marker_type(info->objectType), info->object, info->pObjectName, &handled);
    if (handled) return result;
    PFN_vkDebugMarkerSetObjectNameEXT call = (void *)native_proc(dev, "vkDebugMarkerSetObjectNameEXT");
    return call ? call(device, info) : VK_ERROR_EXTENSION_NOT_PRESENT;
}
static VKAPI_ATTR VkResult VKAPI_CALL DebugMarkerSetObjectTagEXT(VkDevice device, const VkDebugMarkerObjectTagInfoEXT *info) {
    struct vkrt_device_data *dev = VKRT_FIND(struct vkrt_device_data, device);
    if (!dev || !info) return VK_ERROR_INITIALIZATION_FAILED;
    bool handled;
    VkResult result =
        store_tag(dev, marker_type(info->objectType), info->object, info->tagName, info->tagSize, info->pTag, &handled);
    if (handled) return result;
    PFN_vkDebugMarkerSetObjectTagEXT call = (void *)native_proc(dev, "vkDebugMarkerSetObjectTagEXT");
    return call ? call(device, info) : VK_ERROR_EXTENSION_NOT_PRESENT;
}
PFN_vkVoidFunction vkrt_debug_objects_proc(const char *name) {
#define ENTRY(command) \
    if (!strcmp(name, "vk" #command)) return (PFN_vkVoidFunction)command
    ENTRY(SetDebugUtilsObjectNameEXT);
    ENTRY(SetDebugUtilsObjectTagEXT);
    ENTRY(DebugMarkerSetObjectNameEXT);
    ENTRY(DebugMarkerSetObjectTagEXT);
    ENTRY(SetPrivateData);
    ENTRY(GetPrivateData);
    ENTRY(DestroyPrivateDataSlot);
    if (!strcmp(name, "vkSetPrivateDataEXT")) return (PFN_vkVoidFunction)SetPrivateData;
    if (!strcmp(name, "vkGetPrivateDataEXT")) return (PFN_vkVoidFunction)GetPrivateData;
    if (!strcmp(name, "vkDestroyPrivateDataSlotEXT")) return (PFN_vkVoidFunction)DestroyPrivateDataSlot;
#undef ENTRY
    return NULL;
}
