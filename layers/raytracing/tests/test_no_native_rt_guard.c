/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 * Independent host controls for the same loader-facing boundary used on GPU.
 */
#include "no_native_rt_guard_layer.c"

static unsigned failures, native_instances, native_devices, native_modules, native_buffers, native_barriers, native_submits;
static unsigned native_dispatches, native_base_dispatches, native_indirect_dispatches;
#define REQUIRE(x)                                          \
    do {                                                    \
        if (!(x)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); \
            ++failures;                                     \
        }                                                   \
    } while (0)
struct fake_handle {
    void *dispatch;
};
static int instance_dispatch, device_dispatch;
static struct fake_handle native_instance = {&instance_dispatch}, native_physical = {&instance_dispatch};
static struct fake_handle native_device = {&device_dispatch}, native_command = {&device_dispatch},
                          native_queue = {&device_dispatch};
static void environment(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (*value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

static VKAPI_ATTR void VKAPI_CALL driver_unknown(void) { REQUIRE(0); }
static VKAPI_ATTR VkResult VKAPI_CALL driver_CreateInstance(const VkInstanceCreateInfo *info,
                                                            const VkAllocationCallbacks *allocator, VkInstance *instance) {
    (void)allocator;
    REQUIRE(info->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    ++native_instances;
    *instance = (VkInstance)&native_instance;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL driver_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator) {
    (void)allocator;
    REQUIRE(instance == (VkInstance)&native_instance);
}
static VKAPI_ATTR VkResult VKAPI_CALL driver_CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                                          const VkAllocationCallbacks *allocator, VkDevice *device) {
    (void)allocator;
    REQUIRE(physical == (VkPhysicalDevice)&native_physical);
    REQUIRE(!guard_feature_chain(info->pNext));
    ++native_devices;
    *device = (VkDevice)&native_device;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL driver_DestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator) {
    (void)allocator;
    REQUIRE(device == (VkDevice)&native_device);
}
static VKAPI_ATTR VkResult VKAPI_CALL driver_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *info,
                                                                const VkAllocationCallbacks *allocator, VkShaderModule *module) {
    (void)allocator;
    REQUIRE(device == (VkDevice)&native_device && info->codeSize > 20);
    ++native_modules;
    *module = (VkShaderModule)(uintptr_t)19;
    return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL driver_CreateBuffer(VkDevice device, const VkBufferCreateInfo *info,
                                                          const VkAllocationCallbacks *allocator, VkBuffer *buffer) {
    (void)allocator;
    REQUIRE(device == (VkDevice)&native_device && info->usage == VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    ++native_buffers;
    *buffer = (VkBuffer)(uintptr_t)23;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL driver_CmdPipelineBarrier2(VkCommandBuffer command, const VkDependencyInfo *info) {
    REQUIRE(command == (VkCommandBuffer)&native_command && !guard_dependency(info));
    ++native_barriers;
}
static VKAPI_ATTR VkResult VKAPI_CALL driver_QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *infos,
                                                          VkFence fence) {
    REQUIRE(queue == (VkQueue)&native_queue && count == 1 && infos->sType == VK_STRUCTURE_TYPE_SUBMIT_INFO_2 && !fence);
    ++native_submits;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL driver_Features2(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 *features) {
    REQUIRE(physical == (VkPhysicalDevice)&native_physical);
    features->features.shaderInt64 = VK_TRUE;
    for (VkBaseOutStructure *p = features->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR)
            ((VkPhysicalDeviceAccelerationStructureFeaturesKHR *)p)->accelerationStructure = VK_TRUE;
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR)
            ((VkPhysicalDeviceRayQueryFeaturesKHR *)p)->rayQuery = VK_TRUE;
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR)
            ((VkPhysicalDeviceRayTracingPipelineFeaturesKHR *)p)->rayTracingPipeline = VK_TRUE;
    }
}
static VKAPI_ATTR void VKAPI_CALL driver_Properties2(VkPhysicalDevice physical, VkPhysicalDeviceProperties2 *properties) {
    REQUIRE(physical == (VkPhysicalDevice)&native_physical);
    properties->properties.vendorID = 123;
    properties->properties.limits.maxComputeWorkGroupCount[0] = 17;
    properties->properties.limits.maxComputeWorkGroupCount[1] = 5;
    properties->properties.limits.maxComputeWorkGroupCount[2] = 11;
    for (VkBaseOutStructure *p = properties->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR)
            ((VkPhysicalDeviceRayTracingPipelinePropertiesKHR *)p)->maxRayRecursionDepth = 31;
}
static VKAPI_ATTR void VKAPI_CALL driver_Properties(VkPhysicalDevice physical, VkPhysicalDeviceProperties *properties) {
    VkPhysicalDeviceProperties2 local = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    driver_Properties2(physical, &local);
    *properties = local.properties;
}
static VKAPI_ATTR void VKAPI_CALL driver_FormatProperties(VkPhysicalDevice physical, VkFormat format, VkFormatProperties *out) {
    REQUIRE(physical == (VkPhysicalDevice)&native_physical && format == VK_FORMAT_R32G32B32_SFLOAT);
    *out =
        (VkFormatProperties){VK_FORMAT_FEATURE_BLIT_SRC_BIT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
                             VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT | VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR};
}
static VKAPI_ATTR void VKAPI_CALL driver_FormatProperties2(VkPhysicalDevice physical, VkFormat format, VkFormatProperties2 *out) {
    driver_FormatProperties(physical, format, &out->formatProperties);
    for (VkBaseOutStructure *p = out->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
            VkFormatProperties3 *flags = (void *)p;
            flags->linearTilingFeatures = out->formatProperties.linearTilingFeatures;
            flags->optimalTilingFeatures = out->formatProperties.optimalTilingFeatures;
            flags->bufferFeatures = out->formatProperties.bufferFeatures | (UINT64_C(1) << 42);
        }
}
static VKAPI_ATTR void VKAPI_CALL driver_CmdDispatch(VkCommandBuffer command, uint32_t x, uint32_t y, uint32_t z) {
    REQUIRE(command == (VkCommandBuffer)&native_command && x <= 17 && y <= 5 && z <= 11);
    ++native_dispatches;
}
static VKAPI_ATTR void VKAPI_CALL driver_CmdDispatchBase(VkCommandBuffer command, uint32_t bx, uint32_t by, uint32_t bz, uint32_t x,
                                                         uint32_t y, uint32_t z) {
    REQUIRE(command == (VkCommandBuffer)&native_command && (uint64_t)bx + x <= 17 && (uint64_t)by + y <= 5 &&
            (uint64_t)bz + z <= 11);
    ++native_base_dispatches;
}
static VKAPI_ATTR void VKAPI_CALL driver_CmdDispatchIndirect(VkCommandBuffer command, VkBuffer buffer, VkDeviceSize offset) {
    (void)buffer;
    (void)offset;
    REQUIRE(command == (VkCommandBuffer)&native_command);
    ++native_indirect_dispatches;
}
static VKAPI_ATTR VkResult VKAPI_CALL driver_Extensions(VkPhysicalDevice physical, const char *layer, uint32_t *count,
                                                        VkExtensionProperties *properties) {
    REQUIRE(physical == (VkPhysicalDevice)&native_physical && !layer);
    const char *names[] = {"VK_KHR_acceleration_structure", "VK_KHR_ray_query", "VK_KHR_ray_tracing_pipeline", "VK_KHR_swapchain"};
    uint32_t capacity = *count;
    *count = properties && capacity < 4 ? capacity : 4;
    if (!properties) return VK_SUCCESS;
    for (uint32_t i = 0; i < *count; ++i) {
        memset(&properties[i], 0, sizeof(properties[i]));
        strcpy(properties[i].extensionName, names[i]);
        properties[i].specVersion = i + 1;
    }
    return capacity < 4 ? VK_INCOMPLETE : VK_SUCCESS;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL driver_GDPA(VkDevice device, const char *name) {
    REQUIRE(device == (VkDevice)&native_device);
#define MOCK(n) \
    if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)driver_##n
    MOCK(DestroyDevice);
    MOCK(CreateShaderModule);
    MOCK(CreateBuffer);
    MOCK(CmdPipelineBarrier2);
    MOCK(QueueSubmit2);
    MOCK(CmdDispatch);
    MOCK(CmdDispatchBase);
    MOCK(CmdDispatchIndirect);
#undef MOCK
    return (PFN_vkVoidFunction)driver_unknown;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL driver_GIPA(VkInstance instance, const char *name) {
    (void)instance;
#define MOCK(n) \
    if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)driver_##n
    MOCK(CreateInstance);
    MOCK(DestroyInstance);
    MOCK(CreateDevice);
#undef MOCK
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2")) return (PFN_vkVoidFunction)driver_Features2;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2")) return (PFN_vkVoidFunction)driver_Properties2;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties")) return (PFN_vkVoidFunction)driver_Properties;
    if (!strcmp(name, "vkGetPhysicalDeviceFormatProperties")) return (PFN_vkVoidFunction)driver_FormatProperties;
    if (!strcmp(name, "vkGetPhysicalDeviceFormatProperties2") || !strcmp(name, "vkGetPhysicalDeviceFormatProperties2KHR"))
        return (PFN_vkVoidFunction)driver_FormatProperties2;
    if (!strcmp(name, "vkEnumerateDeviceExtensionProperties")) return (PFN_vkVoidFunction)driver_Extensions;
    return (PFN_vkVoidFunction)driver_unknown;
}

static void policy_controls(void) {
    uint32_t limits[3];
    REQUIRE(guard_parse_dispatch_limits("9,7,3", limits) && limits[0] == 9 && limits[1] == 7 && limits[2] == 3);
    REQUIRE(guard_parse_dispatch_limits("4294967295,17,1", limits) && limits[0] == UINT32_MAX && limits[1] == 17 && limits[2] == 1);
    const char *invalid_limits[] = {NULL,     "",       "0,2,2",          "2,0,2",  "2,2,0",  "2,2",   "2,2,2,2",
                                    "+2,2,2", "-2,2,2", "4294967296,2,2", "2,2,2x", "2,2, 2", "2,2,2 "};
    for (unsigned i = 0; i < sizeof(invalid_limits) / sizeof(invalid_limits[0]); ++i) {
        limits[0] = 101;
        limits[1] = 102;
        limits[2] = 103;
        REQUIRE(!guard_parse_dispatch_limits(invalid_limits[i], limits));
        REQUIRE(limits[0] == 101 && limits[1] == 102 && limits[2] == 103);
    }
    REQUIRE(guard_extension("VK_KHR_ray_query"));
    REQUIRE(guard_extension("VK_VENDOR_ray_tracing_future"));
    REQUIRE(!guard_extension("VK_KHR_deferred_host_operations"));
    REQUIRE(!guard_extension("VK_EXT_descriptor_buffer"));
    REQUIRE(guard_stages(VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_COPY_BIT_KHR));
    REQUIRE(guard_stages(VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR));
    REQUIRE(!guard_stages(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT));
    REQUIRE(guard_access(VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR));
    REQUIRE(!guard_access(VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT));
    REQUIRE(guard_shader_stages(VK_SHADER_STAGE_INTERSECTION_BIT_KHR));
    REQUIRE(!guard_shader_stages(VK_SHADER_STAGE_ALL));
    REQUIRE(!guard_shader_stages(VK_SHADER_STAGE_ALL_GRAPHICS));
    REQUIRE(guard_descriptor(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR));
    REQUIRE(!guard_descriptor(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER));
    REQUIRE(guard_query(VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SIZE_KHR));
    REQUIRE(!guard_query(VK_QUERY_TYPE_TIMESTAMP));

    /* Literal instruction/capability numbers deliberately do not use the
     * generated rejection tables as their expected-result oracle. */
    uint32_t module[] = {0x07230203, 0x00010600, 0, 5, 0, (2u << 16) | 17, 1};
    REQUIRE(!guard_spirv(module, sizeof(module)));
    module[6] = 4472; /* RayQueryKHR */
    REQUIRE(!strcmp(guard_spirv(module, sizeof(module)), "native_rt_capability"));
    module[5] = (1u << 16) | 4473; /* RayQueryInitializeKHR without its capability */
    REQUIRE(!strcmp(guard_spirv(module, 6 * sizeof(uint32_t)), "native_rt_opcode"));
    module[5] = (1u << 16) | 5337; /* Original NV OpTraceNV, also without capability. */
    REQUIRE(!strcmp(guard_spirv(module, 6 * sizeof(uint32_t)), "native_rt_opcode"));
    module[5] = (2u << 16) | 60000;
    REQUIRE(!strcmp(guard_spirv(module, sizeof(module)), "unknown_spirv_opcode"));
    module[5] = (9u << 16) | 17;
    REQUIRE(!strcmp(guard_spirv(module, sizeof(module)), "malformed_spirv"));
    REQUIRE(guard_spirv(NULL, 0));

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, .rayTracingPipeline = VK_TRUE};
    VkPhysicalDeviceRayQueryFeaturesKHR rq = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, .pNext = &rt, .rayQuery = VK_TRUE};
    VkPhysicalDeviceVulkan12Features core = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &rq, .bufferDeviceAddress = VK_TRUE};
    REQUIRE(guard_feature_chain(&core));
    guard_mask_features(&core);
    REQUIRE(!guard_feature_chain(&core) && core.bufferDeviceAddress && !rq.rayQuery && !rt.rayTracingPipeline);
    REQUIRE(core.pNext == &rq && rq.pNext == &rt);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: test_no_native_rt_guard LOG_PATH [CONTROL_MODE]\n");
        return 2;
    }
    remove(argv[1]);
    const char *mode = argc > 2 ? argv[2] : "";
    const int forced = !strncmp(mode, "--forced-", 9);
    const int invalid_config = !strcmp(mode, "--invalid-dispatch-config-control") || !strcmp(mode, "--forced-mask-config-control");
    environment("VKRT_NO_RT_GUARD_LOG", argv[1]);
    environment("VKRT_NO_RT_GUARD_DISPATCH_LIMITS", !strcmp(mode, "--invalid-dispatch-config-control") ? "2,0,2"
                                                    : forced                                           ? "9,7,3"
                                                                                                       : "");
    environment("VKRT_NO_RT_GUARD_MASK_DISCOVERY", !strcmp(mode, "--forced-mask-config-control") ? "1" : "");
    policy_controls();
    VkNegotiateLayerInterface negotiation = {.sType = LAYER_NEGOTIATE_INTERFACE_STRUCT, .loaderLayerInterfaceVersion = 2};
    REQUIRE(vkNegotiateLoaderLayerInterfaceVersion(&negotiation) == VK_SUCCESS);
    REQUIRE(negotiation.pfnGetInstanceProcAddr && negotiation.pfnGetDeviceProcAddr && negotiation.pfnGetPhysicalDeviceProcAddr);
    VkLayerInstanceLink il = {.pfnNextGetInstanceProcAddr = driver_GIPA};
    VkLayerInstanceCreateInfo ic = {
        .sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, .function = VK_LAYER_LINK_INFO, .u.pLayerInfo = &il};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pNext = &ic};
    VkInstance instance;
    PFN_vkCreateInstance create_instance =
        (PFN_vkCreateInstance)negotiation.pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (invalid_config) {
        REQUIRE(create_instance(&ii, NULL, &instance) == VK_ERROR_INITIALIZATION_FAILED && !native_instances);
        return failures ? 1 : 0;
    }
    REQUIRE(create_instance(&ii, NULL, &instance) == VK_SUCCESS && !ic.u.pLayerInfo);
    VkLayerDeviceLink dl = {.pfnNextGetInstanceProcAddr = driver_GIPA, .pfnNextGetDeviceProcAddr = driver_GDPA};
    VkLayerDeviceCreateInfo dc = {
        .sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, .function = VK_LAYER_LINK_INFO, .u.pLayerInfo = &dl};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &dc};
    VkDevice device;
    PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)negotiation.pfnGetInstanceProcAddr(instance, "vkCreateDevice");
    REQUIRE(create_device((VkPhysicalDevice)&native_physical, &di, NULL, &device) == VK_SUCCESS && native_devices == 1 &&
            !dc.u.pLayerInfo);

    if (!strcmp(mode, "--fatal-control")) {
        PFN_vkCmdTraceRaysKHR trace = (PFN_vkCmdTraceRaysKHR)negotiation.pfnGetDeviceProcAddr(device, "vkCmdTraceRaysKHR");
        trace((VkCommandBuffer)&native_command, NULL, NULL, NULL, NULL, 1, 1, 1);
        return 3; /* Unreachable: forbidden void calls must terminate. */
    }

    if (forced) {
        VkPhysicalDeviceProperties legacy;
        ((PFN_vkGetPhysicalDeviceProperties)negotiation.pfnGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties"))(
            (VkPhysicalDevice)&native_physical, &legacy);
        REQUIRE(legacy.vendorID == 123 && legacy.limits.maxComputeWorkGroupCount[0] == 9 &&
                legacy.limits.maxComputeWorkGroupCount[1] == 5 && legacy.limits.maxComputeWorkGroupCount[2] == 3);
        const char *property_names[] = {"vkGetPhysicalDeviceProperties2", "vkGetPhysicalDeviceProperties2KHR"};
        for (unsigned i = 0; i < 2; ++i) {
            VkPhysicalDeviceRayTracingPipelinePropertiesKHR rt_properties = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
            VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                                      .pNext = &rt_properties};
            ((PFN_vkGetPhysicalDeviceProperties2)negotiation.pfnGetInstanceProcAddr(instance, property_names[i]))(
                (VkPhysicalDevice)&native_physical, &properties);
            REQUIRE(properties.properties.vendorID == legacy.vendorID && properties.pNext == &rt_properties &&
                    properties.properties.limits.maxComputeWorkGroupCount[0] == 9 &&
                    properties.properties.limits.maxComputeWorkGroupCount[1] == 5 &&
                    properties.properties.limits.maxComputeWorkGroupCount[2] == 3 && rt_properties.maxRayRecursionDepth == 31);
        }
        PFN_vkCmdDispatch dispatch = (PFN_vkCmdDispatch)negotiation.pfnGetDeviceProcAddr(device, "vkCmdDispatch");
        PFN_vkCmdDispatchBase base = (PFN_vkCmdDispatchBase)negotiation.pfnGetDeviceProcAddr(device, "vkCmdDispatchBaseKHR");
        if (!strcmp(mode, "--forced-dispatch-fatal-control")) {
            dispatch((VkCommandBuffer)&native_command, 10, 1, 1);
            return 3;
        }
        if (!strcmp(mode, "--forced-native-limit-fatal-control")) {
            dispatch((VkCommandBuffer)&native_command, 1, 6, 1);
            return 3;
        }
        if (!strcmp(mode, "--forced-base-fatal-control")) {
            base((VkCommandBuffer)&native_command, UINT32_MAX, 0, 0, 1, 1, 1);
            return 3;
        }
        if (!strcmp(mode, "--forced-indirect-fatal-control")) {
            ((PFN_vkCmdDispatchIndirect)negotiation.pfnGetDeviceProcAddr(device, "vkCmdDispatchIndirect"))(
                (VkCommandBuffer)&native_command, (VkBuffer)(uintptr_t)1, 0);
            return 3;
        }
        if (strcmp(mode, "--forced-no-dispatch-control")) {
            dispatch((VkCommandBuffer)&native_command, 8, 5, 3);
            base((VkCommandBuffer)&native_command, 1, 1, 1, 8, 4, 2);
            REQUIRE(native_dispatches == 1 && native_base_dispatches == 1 && !native_indirect_dispatches && guard_dispatches == 2 &&
                    guard_multidimensional_dispatches == 2 && guard_dispatch_max[0] == 8 && guard_dispatch_max[1] == 5 &&
                    guard_dispatch_max[2] == 3);
        }
    }

    if (!strcmp(mode, "--positive-control") || forced) {
        uint32_t plain[] = {0x07230203, 0x00010600, 0, 5, 0, (2u << 16) | 17, 1};
        VkShaderModuleCreateInfo plain_info = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(plain), .pCode = plain};
        VkShaderModule plain_module;
        REQUIRE(((PFN_vkCreateShaderModule)negotiation.pfnGetDeviceProcAddr(device, "vkCreateShaderModule"))(
                    device, &plain_info, NULL, &plain_module) == VK_SUCCESS);
        REQUIRE(native_modules == 1 && guard_rejected_calls == 0);
        ((PFN_vkDestroyDevice)negotiation.pfnGetDeviceProcAddr(device, "vkDestroyDevice"))(device, NULL);
        ((PFN_vkDestroyInstance)negotiation.pfnGetInstanceProcAddr(instance, "vkDestroyInstance"))(instance, NULL);
        return failures ? 1 : 0;
    }

    uint64_t rejected = guard_rejected_calls;
    const char *extension = "VK_KHR_ray_query";
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = &extension;
    REQUIRE(create_device((VkPhysicalDevice)&native_physical, &di, NULL, &device) == VK_ERROR_FEATURE_NOT_PRESENT &&
            native_devices == 1);
    di.enabledExtensionCount = 0;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, .accelerationStructure = VK_TRUE};
    di.pNext = &as;
    REQUIRE(create_device((VkPhysicalDevice)&native_physical, &di, NULL, &device) == VK_ERROR_FEATURE_NOT_PRESENT &&
            native_devices == 1);
    di.pNext = &dc;

    uint32_t module[] = {0x07230203, 0x00010600, 0, 5, 0, (2u << 16) | 17, 1};
    VkShaderModuleCreateInfo si = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(module), .pCode = module};
    VkShaderModule shader;
    PFN_vkCreateShaderModule create_shader =
        (PFN_vkCreateShaderModule)negotiation.pfnGetDeviceProcAddr(device, "vkCreateShaderModule");
    REQUIRE(create_shader(device, &si, NULL, &shader) == VK_SUCCESS && native_modules == 1);
    module[6] = 4479; /* RayTracingKHR */
    REQUIRE(create_shader(device, &si, NULL, &shader) == VK_ERROR_FEATURE_NOT_PRESENT && native_modules == 1);
    module[6] = 1;
    module[5] = (1u << 16) | 4445; /* TraceRayKHR with no capability */
    si.codeSize = 6 * sizeof(uint32_t);
    REQUIRE(create_shader(device, &si, NULL, &shader) == VK_ERROR_FEATURE_NOT_PRESENT && native_modules == 1);
    PFN_vkCreateShadersEXT create_shaders = (PFN_vkCreateShadersEXT)negotiation.pfnGetDeviceProcAddr(device, "vkCreateShadersEXT");
    VkShaderCreateInfoEXT binary = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                    .codeType = VK_SHADER_CODE_TYPE_BINARY_EXT};
    VkShaderEXT object;
    REQUIRE(create_shaders(device, 1, &binary, NULL, &object) == VK_ERROR_FEATURE_NOT_PRESENT);

    PFN_vkCreateComputePipelines create_compute =
        (PFN_vkCreateComputePipelines)negotiation.pfnGetDeviceProcAddr(device, "vkCreateComputePipelines");
    VkComputePipelineCreateInfo pipeline = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = &si, .stage = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkPipeline created_pipeline;
    REQUIRE(create_compute(device, VK_NULL_HANDLE, 1, &pipeline, NULL, &created_pipeline) == VK_ERROR_FEATURE_NOT_PRESENT);
    VkPipelineShaderStageModuleIdentifierCreateInfoEXT identifier = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT, .identifierSize = 1};
    pipeline.stage.pNext = &identifier;
    REQUIRE(create_compute(device, VK_NULL_HANDLE, 1, &pipeline, NULL, &created_pipeline) == VK_ERROR_FEATURE_NOT_PRESENT);
    pipeline.stage.pNext = NULL;
    VkPipelineBinaryInfoKHR binary_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR, .binaryCount = 1};
    pipeline.pNext = &binary_info;
    REQUIRE(create_compute(device, VK_NULL_HANDLE, 1, &pipeline, NULL, &created_pipeline) == VK_ERROR_FEATURE_NOT_PRESENT);

    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer buffer;
    PFN_vkCreateBuffer create_buffer = (PFN_vkCreateBuffer)negotiation.pfnGetDeviceProcAddr(device, "vkCreateBuffer");
    REQUIRE(create_buffer(device, &bi, NULL, &buffer) == VK_SUCCESS && native_buffers == 1);
    bi.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR;
    REQUIRE(create_buffer(device, &bi, NULL, &buffer) == VK_ERROR_FEATURE_NOT_PRESENT && native_buffers == 1);
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkBufferUsageFlags2CreateInfo usage = {.sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO,
                                           .usage = VK_BUFFER_USAGE_2_SHADER_BINDING_TABLE_BIT_KHR};
    bi.pNext = &usage;
    REQUIRE(create_buffer(device, &bi, NULL, &buffer) == VK_ERROR_FEATURE_NOT_PRESENT && native_buffers == 1);

    PFN_vkCreateAccelerationStructureKHR create_as =
        (PFN_vkCreateAccelerationStructureKHR)negotiation.pfnGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR");
    VkAccelerationStructureKHR structure;
    REQUIRE(create_as(device, NULL, NULL, &structure) == VK_ERROR_FEATURE_NOT_PRESENT);
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                .queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR};
    VkQueryPool pool;
    REQUIRE(((PFN_vkCreateQueryPool)negotiation.pfnGetDeviceProcAddr(device, "vkCreateQueryPool"))(device, &qi, NULL, &pool) ==
            VK_ERROR_FEATURE_NOT_PRESENT);

    VkMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT};
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    ((PFN_vkCmdPipelineBarrier2)negotiation.pfnGetDeviceProcAddr(device, "vkCmdPipelineBarrier2KHR"))(
        (VkCommandBuffer)&native_command, &dependency);
    REQUIRE(native_barriers == 1);
    VkSemaphoreSubmitInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                       .stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT};
    VkSubmitInfo2 submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = 1, .pWaitSemaphoreInfos = &semaphore};
    PFN_vkQueueSubmit2 queue_submit = (PFN_vkQueueSubmit2)negotiation.pfnGetDeviceProcAddr(device, "vkQueueSubmit2KHR");
    REQUIRE(queue_submit((VkQueue)&native_queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS && native_submits == 1);
    semaphore.stageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    REQUIRE(queue_submit((VkQueue)&native_queue, 1, &submit, VK_NULL_HANDLE) == VK_ERROR_FEATURE_NOT_PRESENT &&
            native_submits == 1);
    REQUIRE(guard_rejected_calls - rejected == 13);

    VkPhysicalDeviceRayQueryFeaturesKHR rq = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &rq};
    PFN_vkGetPhysicalDeviceFeatures2 features2 =
        (PFN_vkGetPhysicalDeviceFeatures2)negotiation.pfnGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2KHR");
    features2((VkPhysicalDevice)&native_physical, &features);
    REQUIRE(rq.rayQuery && features.features.shaderInt64);
    const char *native_rt_names[] = {"vkBuildAccelerationStructuresKHR", "vkGetRayTracingCaptureReplayShaderGroupHandlesKHR",
                                     "vkCmdTraceRaysKHR", "vkCmdTraceRaysIndirectKHR"};
    for (unsigned i = 0; i < sizeof(native_rt_names) / sizeof(native_rt_names[0]); ++i)
        REQUIRE(negotiation.pfnGetDeviceProcAddr(device, native_rt_names[i]) != NULL);
    PFN_vkGetPhysicalDeviceFormatProperties formats =
        (void *)negotiation.pfnGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFormatProperties");
    VkFormatProperties format;
    formats((VkPhysicalDevice)&native_physical, VK_FORMAT_R32G32B32_SFLOAT, &format);
    REQUIRE(format.bufferFeatures & VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR);
    guard_mask_discovery = 1;
    features2((VkPhysicalDevice)&native_physical, &features);
    REQUIRE(!rq.rayQuery && features.features.shaderInt64);
    for (unsigned i = 0; i < sizeof(native_rt_names) / sizeof(native_rt_names[0]); ++i) {
        REQUIRE(negotiation.pfnGetDeviceProcAddr(device, native_rt_names[i]) == NULL);
        REQUIRE(negotiation.pfnGetInstanceProcAddr(instance, native_rt_names[i]) == NULL);
    }
    formats((VkPhysicalDevice)&native_physical, VK_FORMAT_R32G32B32_SFLOAT, &format);
    REQUIRE(format.bufferFeatures == VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT &&
            format.linearTilingFeatures == VK_FORMAT_FEATURE_BLIT_SRC_BIT &&
            format.optimalTilingFeatures == VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    const char *format_names[] = {"vkGetPhysicalDeviceFormatProperties2", "vkGetPhysicalDeviceFormatProperties2KHR"};
    for (unsigned i = 0; i < 2; ++i) {
        VkFormatProperties3 flags = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 format2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &flags};
        ((PFN_vkGetPhysicalDeviceFormatProperties2)negotiation.pfnGetInstanceProcAddr(instance, format_names[i]))(
            (VkPhysicalDevice)&native_physical, VK_FORMAT_R32G32B32_SFLOAT, &format2);
        REQUIRE(!memcmp(&format, &format2.formatProperties, sizeof(format)) && format2.pNext == &flags &&
                flags.bufferFeatures == (VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT | (UINT64_C(1) << 42)));
    }
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR properties_rt = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &properties_rt};
    ((PFN_vkGetPhysicalDeviceProperties2)negotiation.pfnGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"))(
        (VkPhysicalDevice)&native_physical, &properties);
    REQUIRE(properties.properties.vendorID == 123 && !properties_rt.maxRayRecursionDepth && properties.pNext == &properties_rt);
    PFN_vkEnumerateDeviceExtensionProperties enumerate =
        (PFN_vkEnumerateDeviceExtensionProperties)negotiation.pfnGetInstanceProcAddr(instance,
                                                                                     "vkEnumerateDeviceExtensionProperties");
    uint32_t count = 0;
    REQUIRE(enumerate((VkPhysicalDevice)&native_physical, NULL, &count, NULL) == VK_SUCCESS && count == 1);
    VkExtensionProperties visible;
    REQUIRE(enumerate((VkPhysicalDevice)&native_physical, NULL, &count, &visible) == VK_SUCCESS && count == 1 &&
            !strcmp(visible.extensionName, "VK_KHR_swapchain") && visible.specVersion == 4);
    count = 0;
    REQUIRE(enumerate((VkPhysicalDevice)&native_physical, NULL, &count, &visible) == VK_INCOMPLETE && count == 0);

    ((PFN_vkDestroyDevice)negotiation.pfnGetDeviceProcAddr(device, "vkDestroyDevice"))(device, NULL);
    ((PFN_vkDestroyInstance)negotiation.pfnGetInstanceProcAddr(instance, "vkDestroyInstance"))(instance, NULL);
    REQUIRE(!guard_devices && !guard_instances);
    printf("Native-RT guard host controls: %s (%u failures; %u accepted devices, %u shader modules; %" PRIu64
           " rejected attacks)\n",
           failures ? "FAIL" : "PASS", failures, native_devices, native_modules, guard_rejected_calls);
    return failures ? 1 : 0;
}
