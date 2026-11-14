/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 * Public RTP execution with a >128-KiB live source array and nested callables.
 * Outputs use atomics so concurrent occurrences have no application data race.
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdatomic.h>
#include <stdbool.h>
#include <vulkan/vulkan.h>
static VkQueue other_queue;
static VkResult arena_instance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
static VkResult arena_device(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);
#define vkCreateInstance arena_instance
#define vkCreateDevice arena_device
#define VKRT_TEST_FIXTURE_ONLY
#include "gpu_harness.h"
#undef vkCreateInstance
#undef vkCreateDevice

enum { WIDTH = 3, HEIGHT = 3, DEPTH = 3, RAYS = WIDTH * HEIGHT * DEPTH, WORDS = RAYS * 4, CELLS = 32769 };
static atomic_uint validation_errors;
static VkDebugUtilsMessengerEXT messenger;
static PFN_vkCreateRayTracingPipelinesKHR create_rt;
static PFN_vkGetRayTracingShaderGroupHandlesKHR get_handles;
static PFN_vkGetRayTracingShaderGroupStackSizeKHR get_stack;
static PFN_vkCmdSetRayTracingPipelineStackSizeKHR set_stack;
static PFN_vkCmdTraceRaysKHR trace_rays;
static PFN_vkSignalSemaphore signal_timeline;
static PFN_vkQueueSubmit2 submit_two;
static bool sync2, no_opt;
static VkPipeline pipeline;
static VkPipelineLayout layout;
static VkDescriptorSet descriptor;
static VkStridedDeviceAddressRegionKHR raygen, callable;
static uint64_t exact_stack;
static struct gpu_buf output, sbt;
static VkDeviceSize output_offset;
static uint32_t expected[WORDS];

#define REQUIRE(test)                                               \
    do {                                                            \
        if (!(test)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); \
            ++g_fail;                                               \
        }                                                           \
    } while (0)
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                    VkDebugUtilsMessageTypeFlagsEXT type,
                                                    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)type;
    (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        atomic_fetch_add(&validation_errors, 1);
        fprintf(stderr, "VALIDATION: %s\n", data->pMessage);
    }
    return VK_FALSE;
}
static VkResult arena_instance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *a, VkInstance *out) {
    const char *extensions[] = {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkDebugUtilsMessengerCreateInfoEXT debug = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = info->pNext,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
        .pfnUserCallback = debug_message};
    VkInstanceCreateInfo local = *info;
    local.pNext = &debug;
    local.enabledExtensionCount = ARRAY_SIZE(extensions);
    local.ppEnabledExtensionNames = extensions;
    VkResult r = vkCreateInstance(&local, a, out);
    if (r == VK_SUCCESS) {
        debug.pNext = NULL;
        PFN_vkCreateDebugUtilsMessengerEXT create = (void *)vkGetInstanceProcAddr(*out, "vkCreateDebugUtilsMessengerEXT");
        if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
        r = create(*out, &debug, a, &messenger);
    }
    return r;
}
static VkResult arena_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info, const VkAllocationCallbacks *a,
                             VkDevice *out) {
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR supported = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
                                                          .pNext = &supported};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &timeline};
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!supported.rayTracingPipeline || !timeline.timelineSemaphore) {
        fputs("SKIP public RTP arena test requires advertised RTP and timeline semaphore support\n", stderr);
        exit(77);
    }
    supported = (VkPhysicalDeviceRayTracingPipelineFeaturesKHR){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
        .pNext = (void *)info->pNext,
        .rayTracingPipeline = VK_TRUE};
    timeline = (VkPhysicalDeviceTimelineSemaphoreFeatures){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .pNext = &supported, .timelineSemaphore = VK_TRUE};
    VkDeviceCreateInfo local = *info;
    local.pNext = &timeline;
    const char **extensions = malloc((info->enabledExtensionCount + 1) * sizeof(*extensions));
    if (!extensions) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(extensions, info->ppEnabledExtensionNames, info->enabledExtensionCount * sizeof(*extensions));
    extensions[local.enabledExtensionCount++] = VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME;
    local.ppEnabledExtensionNames = extensions;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    if (!families) {
        free(extensions);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    VkDeviceQueueCreateInfo queue = info->pQueueCreateInfos[0];
    const float priorities[] = {1, 1};
    bool two = queue.queueFamilyIndex < count && families[queue.queueFamilyIndex].queueCount >= 2;
    free(families);
    if (two) {
        queue.queueCount = 2;
        queue.pQueuePriorities = priorities;
        local.pQueueCreateInfos = &queue;
    }
    VkResult r = vkCreateDevice(physical, &local, a, out);
    if (r == VK_SUCCESS && two) vkGetDeviceQueue(*out, queue.queueFamilyIndex, 1, &other_queue);
    free(extensions);
    return r;
}
static void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags source, VkAccessFlags read, VkPipelineStageFlags destination,
                           VkAccessFlags write) {
    VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, read, write};
    vkCmdPipelineBarrier(cmd, source, destination, 0, 1, &b, 0, NULL, 0, NULL);
}
static VkCommandBuffer allocate_command(VkCommandBufferLevel level) {
    VkCommandBufferAllocateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_pool, .level = level, .commandBufferCount = 1};
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(g_dev, &info, &cmd));
    return cmd;
}
static void begin_command(VkCommandBuffer cmd, VkCommandBufferLevel level) {
    VkCommandBufferInheritanceInfo inheritance = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
    VkCommandBufferBeginInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                     .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
                                     .pInheritanceInfo = level == VK_COMMAND_BUFFER_LEVEL_SECONDARY ? &inheritance : NULL};
    CHECK(vkBeginCommandBuffer(cmd, &info));
}
static VkCommandBuffer record_trace(uint32_t seed, VkCommandBufferLevel level, bool larger_stack) {
    VkCommandBuffer cmd = allocate_command(level);
    begin_command(cmd, level);
    memory_barrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, layout, 0, 1, &descriptor, 0, NULL);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, sizeof(seed), &seed);
    set_stack(cmd, (uint32_t)exact_stack + (larger_stack ? 64 : 0));
    VkStridedDeviceAddressRegionKHR empty = {0};
    trace_rays(cmd, &raygen, &empty, &empty, &callable, WIDTH, HEIGHT, DEPTH);
    memory_barrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                   VK_ACCESS_HOST_READ_BIT);
    CHECK(vkEndCommandBuffer(cmd));
    return cmd;
}
static VkCommandBuffer record_parent(VkCommandBuffer child, uint32_t count) {
    VkCommandBuffer cmd = allocate_command(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    begin_command(cmd, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    VkCommandBuffer children[] = {child, child};
    vkCmdExecuteCommands(cmd, count, children);
    CHECK(vkEndCommandBuffer(cmd));
    return cmd;
}
static VkFence create_fence(void) {
    VkFenceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(g_dev, &info, NULL, &fence));
    return fence;
}
static void wait_fence(VkFence fence) { CHECK(vkWaitForFences(g_dev, 1, &fence, VK_TRUE, UINT64_C(30000000000))); }
static VkResult submit(VkQueue queue, uint32_t count, const VkCommandBuffer *commands, VkSemaphore gate, VkFence fence) {
    if (sync2) {
        VkCommandBufferSubmitInfo *c = calloc(count, sizeof(*c));
        if (!c && count) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i = 0; i < count; ++i)
            c[i] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = commands[i]};
        VkSemaphoreSubmitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                      .semaphore = gate,
                                      .value = 1,
                                      .stageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR};
        VkSubmitInfo2 info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                              .waitSemaphoreInfoCount = gate ? 1 : 0,
                              .pWaitSemaphoreInfos = &wait,
                              .commandBufferInfoCount = count,
                              .pCommandBufferInfos = c};
        VkResult r = submit_two(queue, 1, &info, fence);
        free(c);
        return r;
    }
    const uint64_t value = 1;
    VkTimelineSemaphoreSubmitInfo timeline = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &value};
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
    VkSubmitInfo info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                         .pNext = gate ? &timeline : NULL,
                         .waitSemaphoreCount = gate ? 1 : 0,
                         .pWaitSemaphores = &gate,
                         .pWaitDstStageMask = &stage,
                         .commandBufferCount = count,
                         .pCommandBuffers = commands};
    return vkQueueSubmit(queue, 1, &info, fence);
}
static void add_expected(uint32_t seed, uint32_t occurrences) {
    for (uint32_t id = 0; id < RAYS; ++id) {
        uint32_t digest = 2166136261u;
        for (uint32_t n = 0; n < CELLS; ++n) {
            const uint32_t value = (seed * 1664525u + id * 1013904223u) ^ (n * 2246822519u);
            digest = (digest ^ value) * 16777619u;
        }
        expected[id * 4] += digest * occurrences;
        expected[id * 4 + 1] += ((seed ^ (id * 747796405u)) * 33u + id * 17u + 0x13579u + 7u) * occurrences;
        expected[id * 4 + 2] += occurrences;
        expected[id * 4 + 3] += (seed + id) * occurrences;
    }
}
static void reset_output(void) {
    memset(output.mapped, 0xa5, (size_t)output.size);
    memset((uint8_t *)output.mapped + output_offset, 0, sizeof(expected));
    memset(expected, 0, sizeof(expected));
}
static void check_output(const char *name) {
    const uint32_t *actual = (const uint32_t *)((const uint8_t *)output.mapped + output_offset);
    for (uint32_t n = 0; n < WORDS; ++n)
        if (actual[n] != expected[n]) {
            fprintf(stderr, "FAIL %s word %u: got 0x%x expected 0x%x\n", name, n, actual[n], expected[n]);
            ++g_fail;
            break;
        }
    for (VkDeviceSize n = 0; n < output.size; ++n)
        if ((n < output_offset || n >= output_offset + sizeof(expected)) && ((const uint8_t *)output.mapped)[n] != 0xa5) {
            fprintf(stderr, "FAIL %s padding at byte %llu\n", name, (unsigned long long)n);
            ++g_fail;
            break;
        }
    printf("RTP arenas: %s checked 27 three-dimensional launches, nested callable values, full-array digest and padding\n", name);
}
static void batch_case(VkCommandBuffer command, uint32_t seed, uint32_t count, uint32_t traces_per_command, const char *name) {
    reset_output();
    VkCommandBuffer commands[] = {command, command, command};
    VkFence fence = create_fence();
    CHECK(submit(g_queue, count, commands, VK_NULL_HANDLE, fence));
    wait_fence(fence);
    add_expected(seed, count * traces_per_command);
    check_output(name);
    vkDestroyFence(g_dev, fence, NULL);
}
static void pending_pair(VkCommandBuffer a, VkCommandBuffer b, uint32_t seed_a, uint32_t seed_b, const char *name) {
    reset_output();
    VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
    VkSemaphore gate;
    CHECK(vkCreateSemaphore(g_dev, &si, NULL, &gate));
    VkFence fences[] = {create_fence(), create_fence()};
    CHECK(submit(g_queue, 1, &a, gate, fences[0]));
    CHECK(submit(other_queue ? other_queue : g_queue, 1, &b, gate, fences[1]));
    REQUIRE(vkGetFenceStatus(g_dev, fences[0]) == VK_NOT_READY && vkGetFenceStatus(g_dev, fences[1]) == VK_NOT_READY);
    VkSemaphoreSignalInfo signal = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = gate, .value = 1};
    CHECK(signal_timeline(g_dev, &signal));
    for (unsigned i = 0; i < 2; ++i) wait_fence(fences[i]);
    add_expected(seed_a, 1);
    add_expected(seed_b, 1);
    check_output(name);
    for (unsigned i = 0; i < 2; ++i) vkDestroyFence(g_dev, fences[i], NULL);
    vkDestroySemaphore(g_dev, gate, NULL);
}
static VkShaderModule load_module(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END)) {
        fprintf(stderr, "Cannot open shader %s\n", path);
        exit(1);
    }
    const long length = ftell(file);
    if (length <= 0 || length % 4 || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        exit(1);
    }
    uint32_t *words = malloc((size_t)length);
    if (!words) {
        fclose(file);
        exit(1);
    }
    if (fread(words, 1, (size_t)length, file) != (size_t)length) {
        free(words);
        fclose(file);
        exit(1);
    }
    fclose(file);
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)length, .pCode = words};
    VkShaderModule shader;
    CHECK(vkCreateShaderModule(g_dev, &info, NULL, &shader));
    free(words);
    return shader;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fputs("Usage: test_rtp_arenas raygen.spv callable.spv [--submit2] [--disable-optimization]\n", stderr);
        return 2;
    }
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--submit2"))
            sync2 = true;
        else if (!strcmp(argv[i], "--disable-optimization"))
            no_opt = true;
        else
            return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    init_vulkan();
    create_rt = (void *)vkGetDeviceProcAddr(g_dev, "vkCreateRayTracingPipelinesKHR");
    get_handles = (void *)vkGetDeviceProcAddr(g_dev, "vkGetRayTracingShaderGroupHandlesKHR");
    get_stack = (void *)vkGetDeviceProcAddr(g_dev, "vkGetRayTracingShaderGroupStackSizeKHR");
    set_stack = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdSetRayTracingPipelineStackSizeKHR");
    trace_rays = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdTraceRaysKHR");
    signal_timeline = (void *)vkGetDeviceProcAddr(g_dev, "vkSignalSemaphore");
    submit_two = (void *)vkGetDeviceProcAddr(g_dev, "vkQueueSubmit2KHR");
    REQUIRE(create_rt && get_handles && get_stack && set_stack && trace_rays && signal_timeline && (!sync2 || submit_two));
    if (g_fail) return 1;
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(g_pdev, &properties);
    output_offset = properties.limits.minStorageBufferOffsetAlignment;
    if (output_offset < 64) output_offset = 64;
    buf_create(output_offset + sizeof(expected) + 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &output);
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, NULL};
    VkDescriptorSetLayoutCreateInfo dli = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(g_dev, &dli, NULL, &dsl));
    VkPushConstantRange push = {VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, 4};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &dsl,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    CHECK(vkCreatePipelineLayout(g_dev, &li, NULL, &layout));
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pi = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(g_dev, &pi, NULL, &pool));
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = pool,
                                      .descriptorSetCount = 1,
                                      .pSetLayouts = &dsl};
    CHECK(vkAllocateDescriptorSets(g_dev, &ai, &descriptor));
    VkDescriptorBufferInfo bi = {output.buf, output_offset, sizeof(expected)};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = descriptor,
                                  .dstBinding = 0,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .pBufferInfo = &bi};
    vkUpdateDescriptorSets(g_dev, 1, &write, 0, NULL);
    VkShaderModule modules[] = {load_module(argv[1]), load_module(argv[2])};
    VkPipelineShaderStageCreateInfo stages[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
                                                  .module = modules[0],
                                                  .pName = "main"},
                                                 {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = VK_SHADER_STAGE_CALLABLE_BIT_KHR,
                                                  .module = modules[1],
                                                  .pName = "main"}};
    VkRayTracingShaderGroupCreateInfoKHR groups[2];
    for (unsigned i = 0; i < 2; ++i)
        groups[i] = (VkRayTracingShaderGroupCreateInfoKHR){.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                                                           .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
                                                           .generalShader = i,
                                                           .closestHitShader = VK_SHADER_UNUSED_KHR,
                                                           .anyHitShader = VK_SHADER_UNUSED_KHR,
                                                           .intersectionShader = VK_SHADER_UNUSED_KHR};
    VkDynamicState dynamic = VK_DYNAMIC_STATE_RAY_TRACING_PIPELINE_STACK_SIZE_KHR;
    VkPipelineDynamicStateCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 1, .pDynamicStates = &dynamic};
    VkRayTracingPipelineCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
                                            .flags = no_opt ? VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT : 0,
                                            .stageCount = 2,
                                            .pStages = stages,
                                            .groupCount = 2,
                                            .pGroups = groups,
                                            .maxPipelineRayRecursionDepth = 1,
                                            .pDynamicState = &di,
                                            .layout = layout};
    CHECK(create_rt(g_dev, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &ci, NULL, &pipeline));
    for (unsigned i = 0; i < 2; ++i) vkDestroyShaderModule(g_dev, modules[i], NULL);
    const uint64_t rg_stack = get_stack(g_dev, pipeline, 0, VK_SHADER_GROUP_SHADER_GENERAL_KHR);
    const uint64_t call_stack = get_stack(g_dev, pipeline, 1, VK_SHADER_GROUP_SHADER_GENERAL_KHR);
    REQUIRE(call_stack <= UINT64_MAX / 2);
    REQUIRE(rg_stack <= UINT64_MAX - 2 * call_stack);
    exact_stack = rg_stack + 2 * call_stack;
    REQUIRE(exact_stack && exact_stack <= UINT32_MAX - 64);
    const char *layers = getenv("VK_INSTANCE_LAYERS");
    if (layers && strstr(layers, "VK_LAYER_VKRT_SOFTWARE_RAY_QUERY")) REQUIRE(rg_stack >= CELLS * sizeof(uint32_t));
    printf("RTP arenas: source array=%u bytes, public stack equation=%llu (raygen=%llu callable=%llu), optimization=%s\n",
           CELLS * 4, (unsigned long long)exact_stack, (unsigned long long)rg_stack, (unsigned long long)call_stack,
           no_opt ? "disabled" : "enabled");
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rt = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &rt};
    vkGetPhysicalDeviceProperties2(g_pdev, &props);
    REQUIRE(rt.shaderGroupHandleSize && rt.shaderGroupHandleSize <= 256 && rt.shaderGroupHandleAlignment &&
            rt.shaderGroupBaseAlignment);
    if (g_fail) return 1;
    uint64_t stride = rt.shaderGroupHandleSize;
    const uint64_t alignment =
        rt.shaderGroupBaseAlignment > rt.shaderGroupHandleAlignment ? rt.shaderGroupBaseAlignment : rt.shaderGroupHandleAlignment;
    stride = (stride + alignment - 1) / alignment * alignment;
    REQUIRE(stride <= rt.maxShaderGroupStride);
    if (g_fail) return 1;
    buf_create(stride * 2 + alignment, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &sbt);
    /* Exercise an aligned, nonzero suballocation even when the base is aligned. */
    const uint64_t sbt_offset = alignment - sbt.addr % alignment;
    uint8_t handles[512];
    CHECK(get_handles(g_dev, pipeline, 0, 2, rt.shaderGroupHandleSize * 2, handles));
    for (unsigned i = 0; i < 2; ++i)
        memcpy((uint8_t *)sbt.mapped + sbt_offset + i * stride, handles + i * rt.shaderGroupHandleSize, rt.shaderGroupHandleSize);
    raygen = (VkStridedDeviceAddressRegionKHR){sbt.addr + sbt_offset, stride, stride};
    callable = (VkStridedDeviceAddressRegionKHR){sbt.addr + sbt_offset + stride, stride, stride};
    VkCommandBuffer direct = record_trace(7, VK_COMMAND_BUFFER_LEVEL_PRIMARY, false);
    batch_case(direct, 7, 1, 1, "first execution");
    batch_case(direct, 7, 1, 1, "serial recording reuse");
    batch_case(direct, 7, 3, 1, "same primary three occurrences in one submit");
    pending_pair(direct, direct, 7, 7, "same simultaneous primary pending twice");
    VkCommandBuffer child = record_trace(13, VK_COMMAND_BUFFER_LEVEL_SECONDARY, false);
    VkCommandBuffer parent = record_parent(child, 2);
    batch_case(parent, 13, 1, 2, "same secondary twice inside one primary");
    /* Each execution of parent performs two child occurrences. */
    VkCommandBuffer one_child[2] = {record_parent(child, 1), record_parent(child, 1)};
    pending_pair(one_child[0], one_child[1], 13, 13, "two pending parents share one simultaneous secondary");
    VkCommandBuffer different[] = {record_trace(17, VK_COMMAND_BUFFER_LEVEL_PRIMARY, false),
                                   record_trace(19, VK_COMMAND_BUFFER_LEVEL_PRIMARY, true)};
    pending_pair(different[0], different[1], 17, 19, "different pending contexts and stack budgets");
    vkFreeCommandBuffers(g_dev, g_pool, 1, &direct);
    vkFreeCommandBuffers(g_dev, g_pool, 1, &parent);
    vkFreeCommandBuffers(g_dev, g_pool, 2, one_child);
    vkFreeCommandBuffers(g_dev, g_pool, 2, different);
    vkFreeCommandBuffers(g_dev, g_pool, 1, &child);
    vkDestroyPipeline(g_dev, pipeline, NULL);
    vkDestroyDescriptorPool(g_dev, pool, NULL);
    vkDestroyPipelineLayout(g_dev, layout, NULL);
    vkDestroyDescriptorSetLayout(g_dev, dsl, NULL);
    buf_destroy(&sbt);
    buf_destroy(&output);
    vkDestroyCommandPool(g_dev, g_pool, NULL);
    vkDestroyDevice(g_dev, NULL);
    PFN_vkDestroyDebugUtilsMessengerEXT destroy = (void *)vkGetInstanceProcAddr(g_instance, "vkDestroyDebugUtilsMessengerEXT");
    destroy(g_instance, messenger, NULL);
    vkDestroyInstance(g_instance, NULL);
    printf("Public RTP arenas: %s (%d failures, %u validation errors, submit=%s, queues=%u)\n",
           g_fail || atomic_load(&validation_errors) ? "FAIL" : "PASS", g_fail, atomic_load(&validation_errors),
           sync2 ? "Submit2" : "Submit", other_queue ? 2u : 1u);
    return g_fail || atomic_load(&validation_errors) ? 1 : 0;
}
