/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-only downstream boundary. In execution-only mode discovery retains
 * driver truth so unmodified CTS selects existing cases. Masked discovery is
 * a separate mode and does not turn the upper layer's unqualified discovery
 * path into support. A run is evidence only with zero reject records and a
 * completed instance summary; return codes alone are insufficient.
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "no_native_rt_guard_policy.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vk_layer.h>
#ifdef _WIN32
#include <windows.h>
#define GUARD_EXPORT /* Exported by no_native_rt_guard.def. */
static SRWLOCK guard_mutex = SRWLOCK_INIT;
static void guard_lock(void) { AcquireSRWLockExclusive(&guard_mutex); }
static void guard_unlock(void) { ReleaseSRWLockExclusive(&guard_mutex); }
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#define GUARD_EXPORT __attribute__((visibility("default")))
static pthread_mutex_t guard_mutex = PTHREAD_MUTEX_INITIALIZER;
static void guard_lock(void) { pthread_mutex_lock(&guard_mutex); }
static void guard_unlock(void) { pthread_mutex_unlock(&guard_mutex); }
#endif

struct guard_dispatch {
    void *key;
    VkInstance instance;
    VkDevice device;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_GetPhysicalDeviceProcAddr gpdpa;
    PFN_vkGetDeviceProcAddr gdpa;
    uint32_t dispatch_limits[3];
    struct guard_dispatch *next;
};
static struct guard_dispatch *guard_instances, *guard_devices;
static uint64_t guard_checked_calls, guard_rejected_calls, guard_device_creates, guard_shader_definitions;
static uint64_t guard_dispatches, guard_multidimensional_dispatches;
static uint32_t guard_dispatch_max[3], guard_dispatch_limits[3];
static int guard_force_dispatch_limits;
static int guard_mask_discovery;
static int guard_configured;
static const char *guard_log_path;
static uint64_t guard_pid;
static char guard_load_id[96];

/* This intentionally creates a nonconforming test profile. Never infer a
 * Vulkan mandatory-limit proof from these small, explicitly audited limits. */
static int guard_parse_dispatch_limits(const char *text, uint32_t limits[3]) {
    if (!text) return 0;
    uint32_t parsed[3];
    for (uint32_t axis = 0; axis < 3; ++axis) {
        uint32_t value = 0;
        if (*text < '0' || *text > '9') return 0;
        do {
            uint32_t digit = (uint32_t)(*text++ - '0');
            if (value > (UINT32_MAX - digit) / 10) return 0;
            value = value * 10 + digit;
        } while (*text >= '0' && *text <= '9');
        if (!value || (axis < 2 ? *text != ',' : *text != 0)) return 0;
        parsed[axis] = value;
        if (axis < 2) ++text;
    }
    memcpy(limits, parsed, sizeof(parsed));
    return 1;
}

static void guard_clamp_dispatch_limits(VkPhysicalDeviceProperties *properties) {
    if (!guard_force_dispatch_limits) return;
    for (uint32_t axis = 0; axis < 3; ++axis)
        if (properties->limits.maxComputeWorkGroupCount[axis] > guard_dispatch_limits[axis])
            properties->limits.maxComputeWorkGroupCount[axis] = guard_dispatch_limits[axis];
}

/* The loader can unload and reload this DLL between discovery and execution
 * in one process. Counters reset on reload; record the process and a per-load
 * monotonic timestamp/address identity so the auditor can prove each segment
 * separately rather than accepting concatenated process runs. */
static int guard_load_identity(void) {
#ifdef _WIN32
    LARGE_INTEGER ticks;
    if (!QueryPerformanceCounter(&ticks)) return 0;
    guard_pid = GetCurrentProcessId();
    snprintf(guard_load_id, sizeof(guard_load_id), "qpc-%016" PRIx64 "-%" PRIxPTR, (uint64_t)ticks.QuadPart,
             (uintptr_t)&guard_configured);
#else
    struct timespec stamp;
    if (clock_gettime(CLOCK_MONOTONIC, &stamp)) return 0;
    guard_pid = (uint64_t)getpid();
    snprintf(guard_load_id, sizeof(guard_load_id), "mono-%016" PRIx64 "-%08lx-%" PRIxPTR, (uint64_t)stamp.tv_sec,
             (unsigned long)stamp.tv_nsec, (uintptr_t)&guard_configured);
#endif
    return 1;
}

static void guard_json_string(FILE *file, const char *value) {
    fputc('"', file);
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\')
            fprintf(file, "\\%c", *p);
        else if (*p < 32)
            fprintf(file, "\\u%04x", *p);
        else
            fputc(*p, file);
    }
    fputc('"', file);
}

/* Caller holds the mutex. Reopening permits a process-local append-only log
 * without keeping inherited descriptors or depending on normal process exit. */
static int guard_record(const char *event, const char *kind, const char *detail) {
    if (!guard_log_path) return 0;
    FILE *file = fopen(guard_log_path, "ab");
    if (!file) return 0;
    fprintf(file, "{\"schema_version\":2,\"pid\":%" PRIu64 ",\"load_id\":", guard_pid);
    guard_json_string(file, guard_load_id);
    fprintf(file, ",\"event\":");
    guard_json_string(file, event);
    fprintf(file, ",\"kind\":");
    guard_json_string(file, kind);
    fprintf(file, ",\"detail\":");
    guard_json_string(file, detail);
    fprintf(file,
            ",\"mode\":\"%s\",\"checked\":%" PRIu64 ",\"rejected\":%" PRIu64 ",\"devices\":%" PRIu64
            ",\"shader_definitions\":%" PRIu64,
            guard_force_dispatch_limits ? "forced-dispatch-limits"
            : guard_mask_discovery      ? "mask-discovery"
                                        : "execution-only",
            guard_checked_calls, guard_rejected_calls, guard_device_creates, guard_shader_definitions);
    if (guard_force_dispatch_limits)
        fprintf(file,
                ",\"dispatch_limits\":[%u,%u,%u],\"dispatches\":%" PRIu64 ",\"multidimensional_dispatches\":%" PRIu64
                ",\"dispatch_max\":[%u,%u,%u]",
                guard_dispatch_limits[0], guard_dispatch_limits[1], guard_dispatch_limits[2], guard_dispatches,
                guard_multidimensional_dispatches, guard_dispatch_max[0], guard_dispatch_max[1], guard_dispatch_max[2]);
    fprintf(file, "}\n");
    int ok = !ferror(file);
    if (fclose(file)) ok = 0;
    return ok;
}

static int guard_configure(void) {
    guard_lock();
    if (!guard_configured) {
        if (!guard_load_identity()) {
            guard_unlock();
            return 0;
        }
        guard_log_path = getenv("VKRT_NO_RT_GUARD_LOG");
        guard_force_dispatch_limits = 0;
        const char *mask = getenv("VKRT_NO_RT_GUARD_MASK_DISCOVERY");
        guard_mask_discovery = mask && !strcmp(mask, "1");
        const char *dispatch = getenv("VKRT_NO_RT_GUARD_DISPATCH_LIMITS");
        if (dispatch && *dispatch) {
            if (guard_mask_discovery || !guard_parse_dispatch_limits(dispatch, guard_dispatch_limits)) {
                guard_unlock();
                fprintf(
                    stderr,
                    "VKRT no-native-RT guard dispatch limits require three positive decimal uint32 values and no discovery mask\n");
                return 0;
            }
            guard_force_dispatch_limits = 1;
        }
        guard_configured = guard_record("start", "configuration", "native-RT rejection boundary");
    }
    int ok = guard_configured;
    guard_unlock();
    if (!ok) fprintf(stderr, "VKRT no-native-RT guard requires a writable VKRT_NO_RT_GUARD_LOG\n");
    return ok;
}

static void guard_reject(const char *kind, const char *detail) {
    guard_lock();
    ++guard_rejected_calls;
    int ok = guard_record("reject", kind, detail);
    guard_unlock();
    fprintf(stderr, "VKRT no-native-RT guard REJECT: %s: %s\n", kind, detail);
    if (!ok) _Exit(87);
}

static void guard_accept(void) {
    guard_lock();
    ++guard_checked_calls;
    guard_unlock();
}

static void guard_fatal(const char *kind, const char *detail) {
    guard_reject(kind, detail);
    _Exit(86);
}

static void *guard_key(const void *handle) { return handle ? *(void *const *)handle : NULL; }

static struct guard_dispatch guard_find(const void *handle, int device) {
    void *key = guard_key(handle);
    struct guard_dispatch copy = {0};
    guard_lock();
    for (struct guard_dispatch *p = device ? guard_devices : guard_instances; p; p = p->next)
        if (p->key == key) {
            copy = *p;
            break;
        }
    guard_unlock();
    return copy;
}

static void guard_insert(struct guard_dispatch *data, int device) {
    guard_lock();
    struct guard_dispatch **head = device ? &guard_devices : &guard_instances;
    data->next = *head;
    *head = data;
    guard_unlock();
}

static void guard_remove(const void *handle, int device) {
    guard_lock();
    struct guard_dispatch **head = device ? &guard_devices : &guard_instances;
    for (; *head; head = &(*head)->next)
        if ((device && (*head)->device == (VkDevice)handle) || (!device && (*head)->instance == (VkInstance)handle)) {
            struct guard_dispatch *old = *head;
            *head = old->next;
            free(old);
            break;
        }
    guard_unlock();
}

static PFN_vkVoidFunction guard_device_proc(const void *handle, const char *name) {
    struct guard_dispatch data = guard_find(handle, 1);
    return data.gdpa ? data.gdpa(data.device, name) : NULL;
}

#define GUARD_NEXT(type, handle, name) ((PFN_vk##type)guard_device_proc((const void *)(handle), "vk" name))
#define GUARD_CHECK_RESULT(condition, kind, name) \
    do {                                          \
        if (condition) {                          \
            guard_reject(kind, name);             \
            return VK_ERROR_FEATURE_NOT_PRESENT;  \
        }                                         \
        guard_accept();                           \
    } while (0)
#define GUARD_CHECK_VOID(condition, kind, name) \
    do {                                        \
        if (condition) guard_fatal(kind, name); \
        guard_accept();                         \
    } while (0)

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL guard_GetInstanceProcAddr(VkInstance instance, const char *name);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL guard_GetDeviceProcAddr(VkDevice device, const char *name);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL guard_GetPhysicalDeviceProcAddr(VkInstance instance, const char *name);

static VKAPI_ATTR VkResult VKAPI_CALL guard_CreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator,
                                                           VkInstance *instance) {
    if (!guard_configure()) return VK_ERROR_INITIALIZATION_FAILED;
    VkLayerInstanceCreateInfo *link = NULL;
    for (const VkBaseInStructure *p = info->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
            ((const VkLayerInstanceCreateInfo *)p)->function == VK_LAYER_LINK_INFO)
            link = (void *)p;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_GetPhysicalDeviceProcAddr gpdpa = link->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    struct guard_dispatch *data = calloc(1, sizeof(*data));
    if (!data) return VK_ERROR_OUT_OF_HOST_MEMORY;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult result = create(info, allocator, instance);
    if (result != VK_SUCCESS) {
        free(data);
        return result;
    }
    *data = (struct guard_dispatch){.key = guard_key(*instance), .instance = *instance, .gipa = gipa, .gpdpa = gpdpa};
    guard_insert(data, 0);
    guard_lock();
    int ok = guard_record("instance", "created", "vkCreateInstance");
    guard_unlock();
    if (!ok) _Exit(87);
    return result;
}

static VKAPI_ATTR void VKAPI_CALL guard_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator) {
    struct guard_dispatch data = guard_find(instance, 0);
    PFN_vkDestroyInstance destroy = data.gipa ? (PFN_vkDestroyInstance)data.gipa(instance, "vkDestroyInstance") : NULL;
    if (destroy) destroy(instance, allocator);
    guard_remove(instance, 0);
    guard_lock();
    int ok = guard_record("summary", "instance_destroyed", "vkDestroyInstance");
    guard_unlock();
    if (!ok) _Exit(87);
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                                         const VkAllocationCallbacks *allocator, VkDevice *device) {
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
        GUARD_CHECK_RESULT(guard_extension(info->ppEnabledExtensionNames[i]), "native_rt_device_extension",
                           info->ppEnabledExtensionNames[i]);
    GUARD_CHECK_RESULT(guard_feature_chain(info->pNext), "native_rt_device_feature", "vkCreateDevice");
    VkLayerDeviceCreateInfo *link = NULL;
    for (const VkBaseInStructure *p = info->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            ((const VkLayerDeviceCreateInfo *)p)->function == VK_LAYER_LINK_INFO)
            link = (void *)p;
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    struct guard_dispatch parent = guard_find(physical, 0);
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(parent.instance, "vkCreateDevice");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    struct guard_dispatch *data = calloc(1, sizeof(*data));
    if (!data) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t dispatch_limits[3] = {0};
    if (guard_force_dispatch_limits) {
        PFN_vkGetPhysicalDeviceProperties properties_proc =
            parent.gipa ? (PFN_vkGetPhysicalDeviceProperties)parent.gipa(parent.instance, "vkGetPhysicalDeviceProperties") : NULL;
        if (!properties_proc) {
            free(data);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        VkPhysicalDeviceProperties properties = {0};
        properties_proc(physical, &properties);
        guard_clamp_dispatch_limits(&properties);
        for (uint32_t axis = 0; axis < 3; ++axis) {
            dispatch_limits[axis] = properties.limits.maxComputeWorkGroupCount[axis];
            if (!dispatch_limits[axis]) {
                free(data);
                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }
    }
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult result = create(physical, info, allocator, device);
    if (result != VK_SUCCESS) {
        free(data);
        return result;
    }
    *data = (struct guard_dispatch){.key = guard_key(*device),
                                    .device = *device,
                                    .gdpa = gdpa,
                                    .dispatch_limits = {dispatch_limits[0], dispatch_limits[1], dispatch_limits[2]}};
    guard_insert(data, 1);
    guard_lock();
    ++guard_device_creates;
    int ok = guard_record("device", "created_without_native_rt", "vkCreateDevice");
    guard_unlock();
    if (!ok) _Exit(87);
    return result;
}

static VKAPI_ATTR void VKAPI_CALL guard_DestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator) {
    PFN_vkDestroyDevice destroy = GUARD_NEXT(DestroyDevice, device, "DestroyDevice");
    if (destroy) destroy(device, allocator);
    guard_remove(device, 1);
}

static VKAPI_ATTR void VKAPI_CALL guard_GetPhysicalDeviceFeatures2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *features) {
    struct guard_dispatch data = guard_find(physical, 0);
    PFN_vkGetPhysicalDeviceFeatures2 next =
        data.gipa ? (PFN_vkGetPhysicalDeviceFeatures2)data.gipa(data.instance, "vkGetPhysicalDeviceFeatures2") : NULL;
    if (!next && data.gipa) next = (PFN_vkGetPhysicalDeviceFeatures2)data.gipa(data.instance, "vkGetPhysicalDeviceFeatures2KHR");
    if (next) next(physical, features);
    if (guard_mask_discovery) guard_mask_features(features->pNext);
}

static VKAPI_ATTR void VKAPI_CALL guard_GetPhysicalDeviceProperties2(VkPhysicalDevice physical,
                                                                     VkPhysicalDeviceProperties2 *properties) {
    struct guard_dispatch data = guard_find(physical, 0);
    PFN_vkGetPhysicalDeviceProperties2 next =
        data.gipa ? (PFN_vkGetPhysicalDeviceProperties2)data.gipa(data.instance, "vkGetPhysicalDeviceProperties2") : NULL;
    if (!next && data.gipa)
        next = (PFN_vkGetPhysicalDeviceProperties2)data.gipa(data.instance, "vkGetPhysicalDeviceProperties2KHR");
    if (next) next(physical, properties);
    guard_clamp_dispatch_limits(&properties->properties);
    if (guard_mask_discovery) guard_mask_properties(properties->pNext);
}

static VKAPI_ATTR void VKAPI_CALL guard_GetPhysicalDeviceProperties(VkPhysicalDevice physical,
                                                                    VkPhysicalDeviceProperties *properties) {
    struct guard_dispatch data = guard_find(physical, 0);
    PFN_vkGetPhysicalDeviceProperties next =
        data.gipa ? (PFN_vkGetPhysicalDeviceProperties)data.gipa(data.instance, "vkGetPhysicalDeviceProperties") : NULL;
    if (next) next(physical, properties);
    guard_clamp_dispatch_limits(properties);
}

static void guard_check_dispatch(VkCommandBuffer command, const uint32_t base[3], const uint32_t groups[3], const char *name) {
    if (!guard_force_dispatch_limits) return;
    struct guard_dispatch data = guard_find(command, 1);
    GUARD_CHECK_VOID(!data.device, "dispatch_limits", name);
    for (uint32_t axis = 0; axis < 3; ++axis)
        GUARD_CHECK_VOID(base[axis] > data.dispatch_limits[axis] || groups[axis] > data.dispatch_limits[axis] - base[axis],
                         "dispatch_limits", name);
    guard_lock();
    ++guard_dispatches;
    guard_multidimensional_dispatches += groups[1] > 1 || groups[2] > 1;
    for (uint32_t axis = 0; axis < 3; ++axis)
        if (guard_dispatch_max[axis] < groups[axis]) guard_dispatch_max[axis] = groups[axis];
    guard_unlock();
}

static VKAPI_ATTR void VKAPI_CALL guard_CmdDispatch(VkCommandBuffer command, uint32_t x, uint32_t y, uint32_t z) {
    const uint32_t base[3] = {0}, groups[3] = {x, y, z};
    guard_check_dispatch(command, base, groups, "vkCmdDispatch");
    GUARD_NEXT(CmdDispatch, command, "CmdDispatch")(command, x, y, z);
}

static VKAPI_ATTR void VKAPI_CALL guard_CmdDispatchBase(VkCommandBuffer command, uint32_t bx, uint32_t by, uint32_t bz, uint32_t x,
                                                        uint32_t y, uint32_t z) {
    const uint32_t base[3] = {bx, by, bz}, groups[3] = {x, y, z};
    guard_check_dispatch(command, base, groups, "vkCmdDispatchBase");
    PFN_vkCmdDispatchBase next = GUARD_NEXT(CmdDispatchBase, command, "CmdDispatchBase");
    if (!next) next = GUARD_NEXT(CmdDispatchBase, command, "CmdDispatchBaseKHR");
    next(command, bx, by, bz, x, y, z);
}

static VKAPI_ATTR void VKAPI_CALL guard_CmdDispatchIndirect(VkCommandBuffer command, VkBuffer buffer, VkDeviceSize offset) {
    if (guard_force_dispatch_limits) guard_fatal("uninspectable_dispatch", "vkCmdDispatchIndirect");
    GUARD_NEXT(CmdDispatchIndirect, command, "CmdDispatchIndirect")(command, buffer, offset);
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_EnumerateDeviceExtensionProperties(VkPhysicalDevice physical, const char *layer,
                                                                               uint32_t *count, VkExtensionProperties *properties) {
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    if (layer && !strcmp(layer, "VK_LAYER_VKRT_NO_NATIVE_RT_GUARD")) {
        *count = 0;
        return VK_SUCCESS;
    }
    struct guard_dispatch data = guard_find(physical, 0);
    PFN_vkEnumerateDeviceExtensionProperties next =
        data.gipa ? (PFN_vkEnumerateDeviceExtensionProperties)data.gipa(data.instance, "vkEnumerateDeviceExtensionProperties")
                  : NULL;
    if (!next) return VK_ERROR_INITIALIZATION_FAILED;
    if (!guard_mask_discovery || layer) return next(physical, layer, count, properties);
    /* Retry a changing native enumeration without publishing a partial mask. */
    for (unsigned retry = 0; retry < 8; ++retry) {
        uint32_t native_count = 0;
        VkResult result = next(physical, NULL, &native_count, NULL);
        if (result != VK_SUCCESS) return result;
        VkExtensionProperties *all = native_count ? calloc(native_count, sizeof(*all)) : NULL;
        if (native_count && !all) return VK_ERROR_OUT_OF_HOST_MEMORY;
        result = next(physical, NULL, &native_count, all);
        if (result == VK_INCOMPLETE) {
            free(all);
            continue;
        }
        if (result != VK_SUCCESS) {
            free(all);
            return result;
        }
        uint32_t visible = 0, written = 0, capacity = properties ? *count : 0;
        for (uint32_t i = 0; i < native_count; ++i)
            if (!guard_extension(all[i].extensionName)) {
                if (properties && written < capacity) properties[written++] = all[i];
                ++visible;
            }
        free(all);
        *count = properties ? written : visible;
        return properties && written < visible ? VK_INCOMPLETE : VK_SUCCESS;
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *info,
                                                               const VkAllocationCallbacks *allocator, VkShaderModule *module) {
    const char *reason = guard_spirv(info->pCode, info->codeSize);
    GUARD_CHECK_RESULT(reason != NULL, reason ? reason : "spirv", "vkCreateShaderModule");
    VkResult result = GUARD_NEXT(CreateShaderModule, device, "CreateShaderModule")(device, info, allocator, module);
    if (result == VK_SUCCESS) {
        guard_lock();
        ++guard_shader_definitions;
        guard_unlock();
    }
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_CreateShadersEXT(VkDevice device, uint32_t count, const VkShaderCreateInfoEXT *infos,
                                                             const VkAllocationCallbacks *allocator, VkShaderEXT *shaders) {
    for (uint32_t i = 0; i < count; ++i) {
        GUARD_CHECK_RESULT(guard_shader_stages(infos[i].stage | infos[i].nextStage), "native_rt_shader_stage",
                           "vkCreateShadersEXT");
        GUARD_CHECK_RESULT(infos[i].codeType != VK_SHADER_CODE_TYPE_SPIRV_EXT, "uninspectable_shader_binary", "vkCreateShadersEXT");
        const char *reason = guard_spirv(infos[i].pCode, infos[i].codeSize);
        GUARD_CHECK_RESULT(reason != NULL, reason ? reason : "spirv", "vkCreateShadersEXT");
    }
    VkResult result = GUARD_NEXT(CreateShadersEXT, device, "CreateShadersEXT")(device, count, infos, allocator, shaders);
    if (result == VK_SUCCESS) {
        guard_lock();
        guard_shader_definitions += count;
        guard_unlock();
    }
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_CreateBuffer(VkDevice device, const VkBufferCreateInfo *info,
                                                         const VkAllocationCallbacks *allocator, VkBuffer *buffer) {
    GUARD_CHECK_RESULT(guard_buffer_info(info), "native_rt_buffer_usage", "vkCreateBuffer");
    return GUARD_NEXT(CreateBuffer, device, "CreateBuffer")(device, info, allocator, buffer);
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_CreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo *info,
                                                            const VkAllocationCallbacks *allocator, VkQueryPool *pool) {
    GUARD_CHECK_RESULT(guard_query(info->queryType), "native_rt_query", "vkCreateQueryPool");
    return GUARD_NEXT(CreateQueryPool, device, "CreateQueryPool")(device, info, allocator, pool);
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_CreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *info,
                                                                      const VkAllocationCallbacks *allocator,
                                                                      VkDescriptorSetLayout *layout) {
    for (uint32_t i = 0; i < info->bindingCount; ++i)
        GUARD_CHECK_RESULT(
            guard_descriptor(info->pBindings[i].descriptorType) || guard_shader_stages(info->pBindings[i].stageFlags),
            "native_rt_descriptor_layout", "vkCreateDescriptorSetLayout");
    return GUARD_NEXT(CreateDescriptorSetLayout, device, "CreateDescriptorSetLayout")(device, info, allocator, layout);
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_CreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo *info,
                                                                 const VkAllocationCallbacks *allocator, VkDescriptorPool *pool) {
    for (uint32_t i = 0; i < info->poolSizeCount; ++i)
        GUARD_CHECK_RESULT(guard_descriptor(info->pPoolSizes[i].type), "native_rt_descriptor_pool", "vkCreateDescriptorPool");
    return GUARD_NEXT(CreateDescriptorPool, device, "CreateDescriptorPool")(device, info, allocator, pool);
}

static VKAPI_ATTR void VKAPI_CALL guard_UpdateDescriptorSets(VkDevice device, uint32_t count, const VkWriteDescriptorSet *writes,
                                                             uint32_t copy_count, const VkCopyDescriptorSet *copies) {
    for (uint32_t i = 0; i < count; ++i)
        GUARD_CHECK_VOID(guard_descriptor(writes[i].descriptorType), "native_rt_descriptor_write", "vkUpdateDescriptorSets");
    GUARD_NEXT(UpdateDescriptorSets, device, "UpdateDescriptorSets")(device, count, writes, copy_count, copies);
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_CreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *info,
                                                                 const VkAllocationCallbacks *allocator, VkPipelineLayout *layout) {
    for (uint32_t i = 0; i < info->pushConstantRangeCount; ++i)
        GUARD_CHECK_RESULT(guard_shader_stages(info->pPushConstantRanges[i].stageFlags), "native_rt_push_stage",
                           "vkCreatePipelineLayout");
    return GUARD_NEXT(CreatePipelineLayout, device, "CreatePipelineLayout")(device, info, allocator, layout);
}

static VKAPI_ATTR void VKAPI_CALL guard_CmdBindPipeline(VkCommandBuffer command, VkPipelineBindPoint point, VkPipeline pipeline) {
    GUARD_CHECK_VOID(point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, "native_rt_bind_point", "vkCmdBindPipeline");
    GUARD_NEXT(CmdBindPipeline, command, "CmdBindPipeline")(command, point, pipeline);
}

static VKAPI_ATTR void VKAPI_CALL guard_CmdPipelineBarrier(VkCommandBuffer command, VkPipelineStageFlags source,
                                                           VkPipelineStageFlags destination, VkDependencyFlags flags,
                                                           uint32_t memory_count, const VkMemoryBarrier *memory,
                                                           uint32_t buffer_count, const VkBufferMemoryBarrier *buffers,
                                                           uint32_t image_count, const VkImageMemoryBarrier *images) {
    GUARD_CHECK_VOID(guard_stages(source | destination), "native_rt_barrier_stage", "vkCmdPipelineBarrier");
    for (uint32_t i = 0; i < memory_count; ++i)
        GUARD_CHECK_VOID(guard_access(memory[i].srcAccessMask | memory[i].dstAccessMask), "native_rt_barrier_access",
                         "vkCmdPipelineBarrier");
    for (uint32_t i = 0; i < buffer_count; ++i)
        GUARD_CHECK_VOID(guard_access(buffers[i].srcAccessMask | buffers[i].dstAccessMask), "native_rt_barrier_access",
                         "vkCmdPipelineBarrier");
    for (uint32_t i = 0; i < image_count; ++i)
        GUARD_CHECK_VOID(guard_access(images[i].srcAccessMask | images[i].dstAccessMask), "native_rt_barrier_access",
                         "vkCmdPipelineBarrier");
    GUARD_NEXT(CmdPipelineBarrier, command, "CmdPipelineBarrier")(command, source, destination, flags, memory_count, memory,
                                                                  buffer_count, buffers, image_count, images);
}

static VKAPI_ATTR void VKAPI_CALL guard_CmdPipelineBarrier2(VkCommandBuffer command, const VkDependencyInfo *info) {
    GUARD_CHECK_VOID(guard_dependency(info), "native_rt_barrier2", "vkCmdPipelineBarrier2");
    PFN_vkCmdPipelineBarrier2 next = GUARD_NEXT(CmdPipelineBarrier2, command, "CmdPipelineBarrier2");
    if (!next) next = GUARD_NEXT(CmdPipelineBarrier2, command, "CmdPipelineBarrier2KHR");
    next(command, info);
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_QueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *infos, VkFence fence) {
    for (uint32_t i = 0; i < count; ++i)
        for (uint32_t j = 0; j < infos[i].waitSemaphoreCount; ++j)
            GUARD_CHECK_RESULT(guard_stages(infos[i].pWaitDstStageMask[j]), "native_rt_submit_stage", "vkQueueSubmit");
    return GUARD_NEXT(QueueSubmit, queue, "QueueSubmit")(queue, count, infos, fence);
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *infos, VkFence fence) {
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t j = 0; j < infos[i].waitSemaphoreInfoCount; ++j)
            GUARD_CHECK_RESULT(guard_stages(infos[i].pWaitSemaphoreInfos[j].stageMask), "native_rt_submit_stage", "vkQueueSubmit2");
        for (uint32_t j = 0; j < infos[i].signalSemaphoreInfoCount; ++j)
            GUARD_CHECK_RESULT(guard_stages(infos[i].pSignalSemaphoreInfos[j].stageMask), "native_rt_submit_stage",
                               "vkQueueSubmit2");
    }
    PFN_vkQueueSubmit2 next = GUARD_NEXT(QueueSubmit2, queue, "QueueSubmit2");
    if (!next) next = GUARD_NEXT(QueueSubmit2, queue, "QueueSubmit2KHR");
    return next(queue, count, infos, fence);
}

static VKAPI_ATTR void VKAPI_CALL guard_CmdSetEvent(VkCommandBuffer command, VkEvent event, VkPipelineStageFlags stage) {
    GUARD_CHECK_VOID(guard_stages(stage), "native_rt_event_stage", "vkCmdSetEvent");
    GUARD_NEXT(CmdSetEvent, command, "CmdSetEvent")(command, event, stage);
}

static VKAPI_ATTR void VKAPI_CALL guard_CmdResetEvent(VkCommandBuffer command, VkEvent event, VkPipelineStageFlags stage) {
    GUARD_CHECK_VOID(guard_stages(stage), "native_rt_event_stage", "vkCmdResetEvent");
    GUARD_NEXT(CmdResetEvent, command, "CmdResetEvent")(command, event, stage);
}

static VKAPI_ATTR void VKAPI_CALL guard_CmdWaitEvents(VkCommandBuffer command, uint32_t count, const VkEvent *events,
                                                      VkPipelineStageFlags source, VkPipelineStageFlags destination,
                                                      uint32_t memory_count, const VkMemoryBarrier *memory, uint32_t buffer_count,
                                                      const VkBufferMemoryBarrier *buffers, uint32_t image_count,
                                                      const VkImageMemoryBarrier *images) {
    GUARD_CHECK_VOID(guard_stages(source | destination), "native_rt_event_stage", "vkCmdWaitEvents");
    for (uint32_t i = 0; i < memory_count; ++i)
        GUARD_CHECK_VOID(guard_access(memory[i].srcAccessMask | memory[i].dstAccessMask), "native_rt_event_access",
                         "vkCmdWaitEvents");
    for (uint32_t i = 0; i < buffer_count; ++i)
        GUARD_CHECK_VOID(guard_access(buffers[i].srcAccessMask | buffers[i].dstAccessMask), "native_rt_event_access",
                         "vkCmdWaitEvents");
    for (uint32_t i = 0; i < image_count; ++i)
        GUARD_CHECK_VOID(guard_access(images[i].srcAccessMask | images[i].dstAccessMask), "native_rt_event_access",
                         "vkCmdWaitEvents");
    GUARD_NEXT(CmdWaitEvents, command, "CmdWaitEvents")(command, count, events, source, destination, memory_count, memory,
                                                        buffer_count, buffers, image_count, images);
}

static VKAPI_ATTR void VKAPI_CALL guard_CmdSetEvent2(VkCommandBuffer command, VkEvent event, const VkDependencyInfo *info) {
    GUARD_CHECK_VOID(guard_dependency(info), "native_rt_event_dependency", "vkCmdSetEvent2");
    PFN_vkCmdSetEvent2 next = GUARD_NEXT(CmdSetEvent2, command, "CmdSetEvent2");
    if (!next) next = GUARD_NEXT(CmdSetEvent2, command, "CmdSetEvent2KHR");
    next(command, event, info);
}

static VKAPI_ATTR void VKAPI_CALL guard_CmdResetEvent2(VkCommandBuffer command, VkEvent event, VkPipelineStageFlags2 stage) {
    GUARD_CHECK_VOID(guard_stages(stage), "native_rt_event_stage", "vkCmdResetEvent2");
    PFN_vkCmdResetEvent2 next = GUARD_NEXT(CmdResetEvent2, command, "CmdResetEvent2");
    if (!next) next = GUARD_NEXT(CmdResetEvent2, command, "CmdResetEvent2KHR");
    next(command, event, stage);
}

static VKAPI_ATTR void VKAPI_CALL guard_CmdWaitEvents2(VkCommandBuffer command, uint32_t count, const VkEvent *events,
                                                       const VkDependencyInfo *infos) {
    for (uint32_t i = 0; i < count; ++i)
        GUARD_CHECK_VOID(guard_dependency(&infos[i]), "native_rt_event_dependency", "vkCmdWaitEvents2");
    PFN_vkCmdWaitEvents2 next = GUARD_NEXT(CmdWaitEvents2, command, "CmdWaitEvents2");
    if (!next) next = GUARD_NEXT(CmdWaitEvents2, command, "CmdWaitEvents2KHR");
    next(command, count, events, infos);
}

static VKAPI_ATTR void VKAPI_CALL guard_CmdWriteTimestamp(VkCommandBuffer command, VkPipelineStageFlagBits stage, VkQueryPool pool,
                                                          uint32_t query) {
    GUARD_CHECK_VOID(guard_stages(stage), "native_rt_timestamp_stage", "vkCmdWriteTimestamp");
    GUARD_NEXT(CmdWriteTimestamp, command, "CmdWriteTimestamp")(command, stage, pool, query);
}

static VKAPI_ATTR void VKAPI_CALL guard_CmdWriteTimestamp2(VkCommandBuffer command, VkPipelineStageFlags2 stage, VkQueryPool pool,
                                                           uint32_t query) {
    GUARD_CHECK_VOID(guard_stages(stage), "native_rt_timestamp_stage", "vkCmdWriteTimestamp2");
    PFN_vkCmdWriteTimestamp2 next = GUARD_NEXT(CmdWriteTimestamp2, command, "CmdWriteTimestamp2");
    if (!next) next = GUARD_NEXT(CmdWriteTimestamp2, command, "CmdWriteTimestamp2KHR");
    next(command, stage, pool, query);
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *info,
                                                             const VkAllocationCallbacks *allocator, VkRenderPass *pass) {
    for (uint32_t i = 0; i < info->dependencyCount; ++i) {
        const VkSubpassDependency *d = &info->pDependencies[i];
        GUARD_CHECK_RESULT(guard_stages(d->srcStageMask | d->dstStageMask) || guard_access(d->srcAccessMask | d->dstAccessMask),
                           "native_rt_render_pass_dependency", "vkCreateRenderPass");
    }
    return GUARD_NEXT(CreateRenderPass, device, "CreateRenderPass")(device, info, allocator, pass);
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_CreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2 *info,
                                                              const VkAllocationCallbacks *allocator, VkRenderPass *pass) {
    for (uint32_t i = 0; i < info->dependencyCount; ++i) {
        const VkSubpassDependency2 *d = &info->pDependencies[i];
        GUARD_CHECK_RESULT(guard_stages(d->srcStageMask | d->dstStageMask) || guard_access(d->srcAccessMask | d->dstAccessMask),
                           "native_rt_render_pass_dependency", "vkCreateRenderPass2");
        for (const VkBaseInStructure *p = d->pNext; p; p = p->pNext)
            if (p->sType == VK_STRUCTURE_TYPE_MEMORY_BARRIER_2) {
                const VkMemoryBarrier2 *m = (const void *)p;
                GUARD_CHECK_RESULT(
                    guard_stages(m->srcStageMask | m->dstStageMask) || guard_access(m->srcAccessMask | m->dstAccessMask),
                    "native_rt_render_pass_dependency", "vkCreateRenderPass2");
            }
    }
    PFN_vkCreateRenderPass2 next = GUARD_NEXT(CreateRenderPass2, device, "CreateRenderPass2");
    if (!next) next = GUARD_NEXT(CreateRenderPass2, device, "CreateRenderPass2KHR");
    return next(device, info, allocator, pass);
}

static VKAPI_ATTR void VKAPI_CALL guard_GetDeviceBufferMemoryRequirements(VkDevice device,
                                                                          const VkDeviceBufferMemoryRequirements *info,
                                                                          VkMemoryRequirements2 *requirements) {
    GUARD_CHECK_VOID(guard_buffer_info(info->pCreateInfo), "native_rt_buffer_requirement", "vkGetDeviceBufferMemoryRequirements");
    PFN_vkGetDeviceBufferMemoryRequirements next =
        GUARD_NEXT(GetDeviceBufferMemoryRequirements, device, "GetDeviceBufferMemoryRequirements");
    if (!next) next = GUARD_NEXT(GetDeviceBufferMemoryRequirements, device, "GetDeviceBufferMemoryRequirementsKHR");
    next(device, info, requirements);
}

static VKAPI_ATTR void VKAPI_CALL guard_GetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice physical,
                                                                                  const VkPhysicalDeviceExternalBufferInfo *info,
                                                                                  VkExternalBufferProperties *properties) {
    GUARD_CHECK_VOID(guard_buffer_usage(info->usage), "native_rt_external_buffer_usage",
                     "vkGetPhysicalDeviceExternalBufferProperties");
    struct guard_dispatch data = guard_find(physical, 0);
    PFN_vkGetPhysicalDeviceExternalBufferProperties next = data.gipa
                                                               ? (PFN_vkGetPhysicalDeviceExternalBufferProperties)data.gipa(
                                                                     data.instance, "vkGetPhysicalDeviceExternalBufferProperties")
                                                               : NULL;
    if (!next && data.gipa)
        next = (PFN_vkGetPhysicalDeviceExternalBufferProperties)data.gipa(data.instance,
                                                                          "vkGetPhysicalDeviceExternalBufferPropertiesKHR");
    next(physical, info, properties);
}

static VKAPI_ATTR void VKAPI_CALL guard_CmdBindDescriptorSets(VkCommandBuffer command, VkPipelineBindPoint point,
                                                              VkPipelineLayout layout, uint32_t first, uint32_t count,
                                                              const VkDescriptorSet *sets, uint32_t dynamic_count,
                                                              const uint32_t *offsets) {
    GUARD_CHECK_VOID(point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, "native_rt_descriptor_bind_point", "vkCmdBindDescriptorSets");
    GUARD_NEXT(CmdBindDescriptorSets, command, "CmdBindDescriptorSets")(command, point, layout, first, count, sets, dynamic_count,
                                                                        offsets);
}

static VKAPI_ATTR void VKAPI_CALL guard_CmdBindDescriptorSets2(VkCommandBuffer command, const VkBindDescriptorSetsInfo *info) {
    GUARD_CHECK_VOID(guard_shader_stages(info->stageFlags), "native_rt_descriptor_stages", "vkCmdBindDescriptorSets2");
    PFN_vkCmdBindDescriptorSets2 next = GUARD_NEXT(CmdBindDescriptorSets2, command, "CmdBindDescriptorSets2");
    if (!next) next = GUARD_NEXT(CmdBindDescriptorSets2, command, "CmdBindDescriptorSets2KHR");
    next(command, info);
}

static VKAPI_ATTR void VKAPI_CALL guard_CmdPushConstants(VkCommandBuffer command, VkPipelineLayout layout,
                                                         VkShaderStageFlags stages, uint32_t offset, uint32_t size,
                                                         const void *values) {
    GUARD_CHECK_VOID(guard_shader_stages(stages), "native_rt_push_stages", "vkCmdPushConstants");
    GUARD_NEXT(CmdPushConstants, command, "CmdPushConstants")(command, layout, stages, offset, size, values);
}

static VKAPI_ATTR void VKAPI_CALL guard_CmdPushConstants2(VkCommandBuffer command, const VkPushConstantsInfo *info) {
    GUARD_CHECK_VOID(guard_shader_stages(info->stageFlags), "native_rt_push_stages", "vkCmdPushConstants2");
    PFN_vkCmdPushConstants2 next = GUARD_NEXT(CmdPushConstants2, command, "CmdPushConstants2");
    if (!next) next = GUARD_NEXT(CmdPushConstants2, command, "CmdPushConstants2KHR");
    next(command, info);
}

static const char *guard_pipeline_binary(const void *chain) {
    for (const VkBaseInStructure *p = chain; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR && ((const VkPipelineBinaryInfoKHR *)p)->binaryCount)
            return "uninspectable_pipeline_binary";
    return NULL;
}

static const char *guard_stage_code(const VkPipelineShaderStageCreateInfo *stage, uint64_t *inline_definitions) {
    if (guard_shader_stages(stage->stage)) return "native_rt_pipeline_stage";
    for (const VkBaseInStructure *p = stage->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT &&
            ((const VkPipelineShaderStageModuleIdentifierCreateInfoEXT *)p)->identifierSize)
            return "uninspectable_shader_identifier";
        if (p->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
            const VkShaderModuleCreateInfo *module = (const void *)p;
            const char *reason = guard_spirv(module->pCode, module->codeSize);
            if (reason) return reason;
            ++*inline_definitions;
        }
    }
    return NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                                    const VkGraphicsPipelineCreateInfo *infos,
                                                                    const VkAllocationCallbacks *allocator, VkPipeline *pipelines) {
    uint64_t inline_definitions = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const char *reason = guard_pipeline_binary(infos[i].pNext);
        GUARD_CHECK_RESULT(reason != NULL, reason ? reason : "pipeline", "vkCreateGraphicsPipelines");
        for (uint32_t j = 0; j < infos[i].stageCount; ++j) {
            reason = guard_stage_code(&infos[i].pStages[j], &inline_definitions);
            GUARD_CHECK_RESULT(reason != NULL, reason ? reason : "stage", "vkCreateGraphicsPipelines");
        }
    }
    VkResult result =
        GUARD_NEXT(CreateGraphicsPipelines, device, "CreateGraphicsPipelines")(device, cache, count, infos, allocator, pipelines);
    if (result == VK_SUCCESS) {
        guard_lock();
        guard_shader_definitions += inline_definitions;
        guard_unlock();
    }
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                                                   const VkComputePipelineCreateInfo *infos,
                                                                   const VkAllocationCallbacks *allocator, VkPipeline *pipelines) {
    uint64_t inline_definitions = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const char *reason = guard_pipeline_binary(infos[i].pNext);
        GUARD_CHECK_RESULT(reason != NULL, reason ? reason : "pipeline", "vkCreateComputePipelines");
        reason = guard_stage_code(&infos[i].stage, &inline_definitions);
        GUARD_CHECK_RESULT(reason != NULL, reason ? reason : "stage", "vkCreateComputePipelines");
    }
    VkResult result =
        GUARD_NEXT(CreateComputePipelines, device, "CreateComputePipelines")(device, cache, count, infos, allocator, pipelines);
    if (result == VK_SUCCESS) {
        guard_lock();
        guard_shader_definitions += inline_definitions;
        guard_unlock();
    }
    return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL guard_CreateDescriptorUpdateTemplate(VkDevice device,
                                                                           const VkDescriptorUpdateTemplateCreateInfo *info,
                                                                           const VkAllocationCallbacks *allocator,
                                                                           VkDescriptorUpdateTemplate *result) {
    GUARD_CHECK_RESULT(info->pipelineBindPoint == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR &&
                           info->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR,
                       "native_rt_update_template_bind_point", "vkCreateDescriptorUpdateTemplate");
    for (uint32_t i = 0; i < info->descriptorUpdateEntryCount; ++i)
        GUARD_CHECK_RESULT(guard_descriptor(info->pDescriptorUpdateEntries[i].descriptorType), "native_rt_update_template_type",
                           "vkCreateDescriptorUpdateTemplate");
    PFN_vkCreateDescriptorUpdateTemplate next =
        GUARD_NEXT(CreateDescriptorUpdateTemplate, device, "CreateDescriptorUpdateTemplate");
    if (!next) next = GUARD_NEXT(CreateDescriptorUpdateTemplate, device, "CreateDescriptorUpdateTemplateKHR");
    return next(device, info, allocator, result);
}

static VKAPI_ATTR void VKAPI_CALL guard_CmdPushDescriptorSet(VkCommandBuffer command, VkPipelineBindPoint point,
                                                             VkPipelineLayout layout, uint32_t set, uint32_t count,
                                                             const VkWriteDescriptorSet *writes) {
    GUARD_CHECK_VOID(point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, "native_rt_push_descriptor_bind_point",
                     "vkCmdPushDescriptorSet");
    for (uint32_t i = 0; i < count; ++i)
        GUARD_CHECK_VOID(guard_descriptor(writes[i].descriptorType), "native_rt_push_descriptor_type", "vkCmdPushDescriptorSet");
    PFN_vkCmdPushDescriptorSet next = GUARD_NEXT(CmdPushDescriptorSet, command, "CmdPushDescriptorSet");
    if (!next) next = GUARD_NEXT(CmdPushDescriptorSet, command, "CmdPushDescriptorSetKHR");
    next(command, point, layout, set, count, writes);
}

static VKAPI_ATTR void VKAPI_CALL guard_GetDescriptorEXT(VkDevice device, const VkDescriptorGetInfoEXT *info, size_t size,
                                                         void *descriptor) {
    GUARD_CHECK_VOID(guard_descriptor(info->type), "native_rt_descriptor_buffer_type", "vkGetDescriptorEXT");
    GUARD_NEXT(GetDescriptorEXT, device, "GetDescriptorEXT")(device, info, size, descriptor);
}

#include "no_native_rt_guard_native.inc"

static PFN_vkVoidFunction guard_proc(const char *name) {
    if (!name) return NULL;
#define ENTRY(n) \
    if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)guard_##n
    ENTRY(GetInstanceProcAddr);
    ENTRY(GetDeviceProcAddr);
    ENTRY(CreateInstance);
    ENTRY(DestroyInstance);
    ENTRY(CreateDevice);
    ENTRY(DestroyDevice);
    ENTRY(GetPhysicalDeviceFeatures2);
    ENTRY(GetPhysicalDeviceProperties2);
    ENTRY(GetPhysicalDeviceProperties);
    ENTRY(EnumerateDeviceExtensionProperties);
    ENTRY(CreateShaderModule);
    ENTRY(CreateShadersEXT);
    ENTRY(CreateBuffer);
    ENTRY(CreateQueryPool);
    ENTRY(CreateDescriptorSetLayout);
    ENTRY(CreateDescriptorPool);
    ENTRY(UpdateDescriptorSets);
    ENTRY(CreatePipelineLayout);
    ENTRY(CmdBindPipeline);
    ENTRY(CmdPipelineBarrier);
    ENTRY(CmdPipelineBarrier2);
    ENTRY(CmdDispatch);
    ENTRY(CmdDispatchBase);
    ENTRY(CmdDispatchIndirect);
    ENTRY(QueueSubmit);
    ENTRY(QueueSubmit2);
    ENTRY(CmdSetEvent);
    ENTRY(CmdResetEvent);
    ENTRY(CmdWaitEvents);
    ENTRY(CmdSetEvent2);
    ENTRY(CmdResetEvent2);
    ENTRY(CmdWaitEvents2);
    ENTRY(CmdWriteTimestamp);
    ENTRY(CmdWriteTimestamp2);
    ENTRY(CreateRenderPass);
    ENTRY(CreateRenderPass2);
    ENTRY(GetDeviceBufferMemoryRequirements);
    ENTRY(GetPhysicalDeviceExternalBufferProperties);
    ENTRY(CmdBindDescriptorSets);
    ENTRY(CmdBindDescriptorSets2);
    ENTRY(CmdPushConstants);
    ENTRY(CmdPushConstants2);
    ENTRY(CreateGraphicsPipelines);
    ENTRY(CreateComputePipelines);
    ENTRY(CreateDescriptorUpdateTemplate);
    ENTRY(CmdPushDescriptorSet);
    ENTRY(GetDescriptorEXT);
#undef ENTRY
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2KHR")) return (PFN_vkVoidFunction)guard_GetPhysicalDeviceFeatures2;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2KHR")) return (PFN_vkVoidFunction)guard_GetPhysicalDeviceProperties2;
    if (!strcmp(name, "vkCmdDispatchBaseKHR")) return (PFN_vkVoidFunction)guard_CmdDispatchBase;
    if (!strcmp(name, "vkCmdPipelineBarrier2KHR")) return (PFN_vkVoidFunction)guard_CmdPipelineBarrier2;
    if (!strcmp(name, "vkQueueSubmit2KHR")) return (PFN_vkVoidFunction)guard_QueueSubmit2;
    if (!strcmp(name, "vkCmdSetEvent2KHR")) return (PFN_vkVoidFunction)guard_CmdSetEvent2;
    if (!strcmp(name, "vkCmdResetEvent2KHR")) return (PFN_vkVoidFunction)guard_CmdResetEvent2;
    if (!strcmp(name, "vkCmdWaitEvents2KHR")) return (PFN_vkVoidFunction)guard_CmdWaitEvents2;
    if (!strcmp(name, "vkCmdWriteTimestamp2KHR")) return (PFN_vkVoidFunction)guard_CmdWriteTimestamp2;
    if (!strcmp(name, "vkCreateRenderPass2KHR")) return (PFN_vkVoidFunction)guard_CreateRenderPass2;
    if (!strcmp(name, "vkGetDeviceBufferMemoryRequirementsKHR")) return (PFN_vkVoidFunction)guard_GetDeviceBufferMemoryRequirements;
    if (!strcmp(name, "vkGetPhysicalDeviceExternalBufferPropertiesKHR"))
        return (PFN_vkVoidFunction)guard_GetPhysicalDeviceExternalBufferProperties;
    if (!strcmp(name, "vkCmdBindDescriptorSets2KHR")) return (PFN_vkVoidFunction)guard_CmdBindDescriptorSets2;
    if (!strcmp(name, "vkCmdPushConstants2KHR")) return (PFN_vkVoidFunction)guard_CmdPushConstants2;
    if (!strcmp(name, "vkCreateDescriptorUpdateTemplateKHR")) return (PFN_vkVoidFunction)guard_CreateDescriptorUpdateTemplate;
    if (!strcmp(name, "vkCmdPushDescriptorSetKHR")) return (PFN_vkVoidFunction)guard_CmdPushDescriptorSet;
    return guard_native_proc(name);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL guard_GetInstanceProcAddr(VkInstance instance, const char *name) {
    PFN_vkVoidFunction intercept = guard_proc(name);
    if (intercept) return intercept;
    struct guard_dispatch data = guard_find(instance, 0);
    return data.gipa ? data.gipa(instance, name) : NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL guard_GetDeviceProcAddr(VkDevice device, const char *name) {
    PFN_vkVoidFunction intercept = guard_proc(name);
    if (intercept) return intercept;
    return guard_device_proc(device, name);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL guard_GetPhysicalDeviceProcAddr(VkInstance instance, const char *name) {
    PFN_vkVoidFunction intercept = guard_proc(name);
    if (intercept) return intercept;
    struct guard_dispatch data = guard_find(instance, 0);
    return data.gpdpa ? data.gpdpa(instance, name) : (data.gipa ? data.gipa(instance, name) : NULL);
}

GUARD_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *version) {
    if (!version || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || version->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    version->loaderLayerInterfaceVersion = 2;
    version->pfnGetInstanceProcAddr = guard_GetInstanceProcAddr;
    version->pfnGetDeviceProcAddr = guard_GetDeviceProcAddr;
    version->pfnGetPhysicalDeviceProcAddr = guard_GetPhysicalDeviceProcAddr;
    return VK_SUCCESS;
}
