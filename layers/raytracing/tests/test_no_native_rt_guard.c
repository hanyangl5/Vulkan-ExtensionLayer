/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 * Independent host controls for the same loader-facing boundary used on GPU.
 */
#include "no_native_rt_guard_layer.c"

static unsigned failures, native_devices, native_modules, native_buffers, native_barriers, native_submits;
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

static VKAPI_ATTR void VKAPI_CALL driver_unknown(void) { REQUIRE(0); }
static VKAPI_ATTR VkResult VKAPI_CALL driver_CreateInstance(const VkInstanceCreateInfo *info,
                                                            const VkAllocationCallbacks *allocator, VkInstance *instance) {
    (void)allocator;
    REQUIRE(info->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
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
    for (VkBaseOutStructure *p = properties->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR)
            ((VkPhysicalDeviceRayTracingPipelinePropertiesKHR *)p)->maxRayRecursionDepth = 31;
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
    if (!strcmp(name, "vkEnumerateDeviceExtensionProperties")) return (PFN_vkVoidFunction)driver_Extensions;
    return (PFN_vkVoidFunction)driver_unknown;
}

static void policy_controls(void) {
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
        fprintf(stderr, "usage: test_no_native_rt_guard LOG_PATH [--fatal-control]\n");
        return 2;
    }
    remove(argv[1]);
#ifdef _WIN32
    _putenv_s("VKRT_NO_RT_GUARD_LOG", argv[1]);
#else
    setenv("VKRT_NO_RT_GUARD_LOG", argv[1], 1);
#endif
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
    REQUIRE(create_instance(&ii, NULL, &instance) == VK_SUCCESS && !ic.u.pLayerInfo);
    VkLayerDeviceLink dl = {.pfnNextGetInstanceProcAddr = driver_GIPA, .pfnNextGetDeviceProcAddr = driver_GDPA};
    VkLayerDeviceCreateInfo dc = {
        .sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, .function = VK_LAYER_LINK_INFO, .u.pLayerInfo = &dl};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &dc};
    VkDevice device;
    PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)negotiation.pfnGetInstanceProcAddr(instance, "vkCreateDevice");
    REQUIRE(create_device((VkPhysicalDevice)&native_physical, &di, NULL, &device) == VK_SUCCESS && native_devices == 1 &&
            !dc.u.pLayerInfo);

    if (argc > 2 && !strcmp(argv[2], "--fatal-control")) {
        PFN_vkCmdTraceRaysKHR trace = (PFN_vkCmdTraceRaysKHR)negotiation.pfnGetDeviceProcAddr(device, "vkCmdTraceRaysKHR");
        trace((VkCommandBuffer)&native_command, NULL, NULL, NULL, NULL, 1, 1, 1);
        return 3; /* Unreachable: forbidden void calls must terminate. */
    }

    if (argc > 2 && !strcmp(argv[2], "--positive-control")) {
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
    guard_mask_discovery = 1;
    features2((VkPhysicalDevice)&native_physical, &features);
    REQUIRE(!rq.rayQuery && features.features.shaderInt64);
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
