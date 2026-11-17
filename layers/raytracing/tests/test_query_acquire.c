/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Public query COPY acquisition, with native and software RT controls. Values
 * are compared with a completed host property query on the same implementation;
 * no native/software size equality or private AS layout is assumed.
 *
 * The host releases each timeline gate unconditionally after submission. Only
 * final values, availability, padding, API results and validation are oracles;
 * no early query/signal observation is required for progress.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdbool.h>
#include <stdatomic.h>
#include <time.h>
#include <vulkan/vulkan.h>
static VkResult acquire_create_instance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
static VkResult acquire_create_device(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);
#define vkCreateInstance acquire_create_instance
#define vkCreateDevice acquire_create_device
#define VKRT_TEST_FIXTURE_ONLY
#include "gpu_harness.h"
#undef vkCreateInstance
#undef vkCreateDevice
#ifdef _WIN32
#include <windows.h>
#endif

enum { OUTPUT_BYTES = 80, OUTPUT_OFFSET = 16, OUTPUT_PAYLOAD = 16, MAX_WAITS = 3 };
static const VkQueryType property_type = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
static const uint64_t timeout_ns = UINT64_C(15000000000);
static bool second_queue_available, expect_rejection;
static unsigned accepted_cases, rejected_cases, skipped_cases;
static atomic_uint validation_errors;
static VkDebugUtilsMessengerEXT messenger;
static VkQueue producer_queue;
static PFN_vkQueueSubmit submit_legacy;
static PFN_vkQueueSubmit2 submit_two;
static PFN_vkCmdBuildAccelerationStructuresKHR build_as;
static PFN_vkCmdWriteAccelerationStructuresPropertiesKHR write_property;
static PFN_vkSignalSemaphore signal_timeline;

#define REQUIRE(condition)                                          \
    do {                                                            \
        if (!(condition)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #condition); \
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
static VkResult acquire_create_instance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator, VkInstance *out) {
    const char *extensions[] = {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkDebugUtilsMessengerCreateInfoEXT debug = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = info->pNext,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
        .pfnUserCallback = debug_message};
    VkInstanceCreateInfo local = *info;
    local.pNext = &debug;
    local.enabledExtensionCount = ARRAY_SIZE(extensions);
    local.ppEnabledExtensionNames = extensions;
    VkResult result = vkCreateInstance(&local, allocator, out);
    if (result == VK_SUCCESS) {
        PFN_vkCreateDebugUtilsMessengerEXT create =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(*out, "vkCreateDebugUtilsMessengerEXT");
        if (!create) return VK_ERROR_EXTENSION_NOT_PRESENT;
        debug.pNext = NULL;
        result = create(*out, &debug, NULL, &messenger);
    }
    return result;
}
static VkResult acquire_create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                      const VkAllocationCallbacks *allocator, VkDevice *out) {
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
                                                          .pNext = (void *)info->pNext,
                                                          .timelineSemaphore = VK_TRUE};
    VkDeviceCreateInfo local = *info;
    local.pNext = &timeline;
    if (info->queueCreateInfoCount != 1) return VK_ERROR_INITIALIZATION_FAILED;
    VkDeviceQueueCreateInfo queue = info->pQueueCreateInfos[0];
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    if (!families) return VK_ERROR_OUT_OF_HOST_MEMORY;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    second_queue_available = queue.queueFamilyIndex < count && families[queue.queueFamilyIndex].queueCount >= 2;
    free(families);
    const float priorities[] = {1, 1};
    if (second_queue_available) {
        queue.queueCount = 2;
        queue.pQueuePriorities = priorities;
        local.pQueueCreateInfos = &queue;
    }
    return vkCreateDevice(physical, &local, allocator, out);
}
static VkCommandBuffer begin_command(void) {
    VkCommandBufferAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                              .commandPool = g_pool,
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                              .commandBufferCount = 1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(g_dev, &allocation, &command));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(command, &begin));
    return command;
}
static VkFence create_fence(void) {
    VkFenceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(g_dev, &info, NULL, &fence));
    return fence;
}
static void wait_fence(VkFence fence) { CHECK(vkWaitForFences(g_dev, 1, &fence, VK_TRUE, timeout_ns)); }
static VkSemaphore create_semaphore(bool binary) {
    VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = binary ? NULL : &type};
    VkSemaphore semaphore;
    CHECK(vkCreateSemaphore(g_dev, &info, NULL, &semaphore));
    return semaphore;
}
struct semaphore_op {
    VkSemaphore semaphore;
    uint64_t value;
    VkPipelineStageFlags2 stages;
};
static VkResult submit(VkQueue queue, bool sync2, VkCommandBuffer command, uint32_t wait_count, const struct semaphore_op *waits,
                       const struct semaphore_op *signal, VkFence fence) {
    if (wait_count > MAX_WAITS) return VK_ERROR_INITIALIZATION_FAILED;
    if (sync2) {
        VkSemaphoreSubmitInfo w[MAX_WAITS], s = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        for (uint32_t i = 0; i < wait_count; ++i)
            w[i] = (VkSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                           .semaphore = waits[i].semaphore,
                                           .value = waits[i].value,
                                           .stageMask = waits[i].stages};
        if (signal) {
            s.semaphore = signal->semaphore;
            s.value = signal->value;
            s.stageMask = signal->stages;
        }
        VkCommandBufferSubmitInfo c = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = command};
        VkSubmitInfo2 info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                              .waitSemaphoreInfoCount = wait_count,
                              .pWaitSemaphoreInfos = wait_count ? w : NULL,
                              .commandBufferInfoCount = command ? 1 : 0,
                              .pCommandBufferInfos = command ? &c : NULL,
                              .signalSemaphoreInfoCount = signal ? 1 : 0,
                              .pSignalSemaphoreInfos = signal ? &s : NULL};
        return submit_two(queue, 1, &info, fence);
    }
    VkSemaphore semaphores[MAX_WAITS];
    VkPipelineStageFlags stages[MAX_WAITS];
    uint64_t values[MAX_WAITS];
    for (uint32_t i = 0; i < wait_count; ++i) {
        semaphores[i] = waits[i].semaphore;
        stages[i] = (VkPipelineStageFlags)waits[i].stages;
        values[i] = waits[i].value;
    }
    VkTimelineSemaphoreSubmitInfo timeline = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                              .waitSemaphoreValueCount = wait_count,
                                              .pWaitSemaphoreValues = wait_count ? values : NULL,
                                              .signalSemaphoreValueCount = signal ? 1 : 0,
                                              .pSignalSemaphoreValues = signal ? &signal->value : NULL};
    VkSubmitInfo info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                         .pNext = &timeline,
                         .waitSemaphoreCount = wait_count,
                         .pWaitSemaphores = wait_count ? semaphores : NULL,
                         .pWaitDstStageMask = wait_count ? stages : NULL,
                         .commandBufferCount = command ? 1 : 0,
                         .pCommandBuffers = command ? &command : NULL,
                         .signalSemaphoreCount = signal ? 1 : 0,
                         .pSignalSemaphores = signal ? &signal->semaphore : NULL};
    return submit_legacy(queue, 1, &info, fence);
}
static void release_gate(VkSemaphore gate) {
    /* This delay only increases the chance of exposing a missing acquire in a
     * negative control. Gate release never depends on observing GPU progress. */
#ifdef _WIN32
    Sleep(25);
#else
    struct timespec pause = {0, 25000000};
    nanosleep(&pause, NULL);
#endif
    VkSemaphoreSignalInfo signal = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = gate, .value = 1};
    CHECK(signal_timeline(g_dev, &signal));
}
struct acquire_scene {
    struct gpu_buf vertices, storage, scratch;
    VkAccelerationStructureKHR as;
    VkSemaphore build_done;
};
static void create_scene(struct acquire_scene *scene) {
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const float vertices[] = {-1, -1, 0, 1, -1, 0, 0, 1, 0};
    buf_create(sizeof(vertices),
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               host, &scene->vertices);
    memcpy(scene->vertices.mapped, vertices, sizeof(vertices));
    VkAccelerationStructureGeometryKHR geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                               .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                               .vertexData.deviceAddress = scene->vertices.addr,
                               .vertexStride = 12,
                               .maxVertex = 2,
                               .indexType = VK_INDEX_TYPE_NONE_KHR}};
    VkAccelerationStructureBuildGeometryInfoKHR build = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                                                         .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                                         .flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR,
                                                         .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                                                         .geometryCount = 1,
                                                         .pGeometries = &geometry};
    const uint32_t count = 1;
    VkAccelerationStructureBuildSizesInfoKHR sizes = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    PFN_vkGetAccelerationStructureBuildSizesKHR get_sizes =
        (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureBuildSizesKHR");
    if (!get_sizes) {
        fputs("Missing vkGetAccelerationStructureBuildSizesKHR\n", stderr);
        exit(1);
    }
    get_sizes(g_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &count, &sizes);
    buf_create(sizes.accelerationStructureSize,
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &scene->storage);
    buf_create(sizes.buildScratchSize + 255, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &scene->scratch);
    REQUIRE(!scene->storage.mapped && !scene->scratch.mapped);
    VkAccelerationStructureCreateInfoKHR create = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                   .buffer = scene->storage.buf,
                                                   .size = sizes.accelerationStructureSize,
                                                   .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR};
    CHECK(pCreateAS(g_dev, &create, NULL, &scene->as));
    build.dstAccelerationStructure = scene->as;
    build.scratchData.deviceAddress = (scene->scratch.addr + 255) & ~UINT64_C(255);
    VkAccelerationStructureBuildRangeInfoKHR range = {.primitiveCount = 1};
    const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
    VkCommandBuffer command = begin_command();
    build_as(command, 1, &build, &ranges);
    CHECK(vkEndCommandBuffer(command));
    scene->build_done = create_semaphore(false);
    const struct semaphore_op signal = {scene->build_done, 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkFence fence = create_fence();
    CHECK(submit(g_queue, false, command, 0, NULL, &signal, fence));
    wait_fence(fence);
    vkFreeCommandBuffers(g_dev, g_pool, 1, &command);
    vkDestroyFence(g_dev, fence, NULL);
}
struct query_case {
    VkQueryPool pool;
    VkSemaphore reset_done;
    struct gpu_buf output;
};
static void prepare_query(struct query_case *test, bool sync2) {
    VkQueryPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = property_type, .queryCount = 1};
    CHECK(vkCreateQueryPool(g_dev, &info, NULL, &test->pool));
    buf_create(OUTPUT_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &test->output);
    memset(test->output.mapped, 0xcd, OUTPUT_BYTES);
    VkCommandBuffer command = begin_command();
    vkCmdResetQueryPool(command, test->pool, 0, 1);
    CHECK(vkEndCommandBuffer(command));
    test->reset_done = create_semaphore(false);
    const struct semaphore_op signal = {test->reset_done, 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkFence fence = create_fence();
    CHECK(submit(g_queue, sync2, command, 0, NULL, &signal, fence));
    wait_fence(fence);
    uint64_t initial[] = {UINT64_MAX, UINT64_MAX};
    VkResult result = vkGetQueryPoolResults(g_dev, test->pool, 0, 1, sizeof(initial), initial, sizeof(initial),
                                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    REQUIRE(result == VK_NOT_READY && initial[1] == 0);
    vkFreeCommandBuffers(g_dev, g_pool, 1, &command);
    vkDestroyFence(g_dev, fence, NULL);
}
static VkCommandBuffer record_property(const struct acquire_scene *scene, const struct query_case *test) {
    VkCommandBuffer command = begin_command();
    write_property(command, 1, &scene->as, property_type, test->pool, 0);
    CHECK(vkEndCommandBuffer(command));
    return command;
}
static VkCommandBuffer record_copy(const struct query_case *test) {
    VkCommandBuffer command = begin_command();
    /* No preceding fill, broad application barrier, or WAIT_BIT can hide an
     * absent private COPY-stage acquire before the formatter reads Q. */
    vkCmdCopyQueryPoolResults(command, test->pool, 0, 1, test->output.buf, OUTPUT_OFFSET, 32,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                            .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(command));
    return command;
}
static uint64_t reference_value(const struct query_case *test) {
    uint64_t values[] = {0, 0};
    CHECK(vkGetQueryPoolResults(g_dev, test->pool, 0, 1, sizeof(values), values, sizeof(values),
                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT));
    REQUIRE(values[0] != 0 && values[1] != 0);
    return values[0];
}
static void check_output(const struct query_case *test, uint64_t reference, bool copied) {
    const uint8_t *bytes = test->output.mapped;
    if (copied) {
        uint64_t values[2];
        memcpy(values, bytes + OUTPUT_OFFSET, sizeof(values));
        /* Vulkan permits any nonzero availability representation. */
        if (values[0] != reference || values[1] == 0) {
            fprintf(stderr, "FAIL copied value=%llu availability=%llu reference=%llu\n", (unsigned long long)values[0],
                    (unsigned long long)values[1], (unsigned long long)reference);
            ++g_fail;
        }
    }
    for (unsigned i = 0; i < OUTPUT_BYTES; ++i)
        if (!copied || i < OUTPUT_OFFSET || i >= OUTPUT_OFFSET + OUTPUT_PAYLOAD) REQUIRE(bytes[i] == 0xcd);
}
static bool consume_result(VkResult result, VkFence fence, const struct query_case *test, uint64_t reference,
                           bool completed_binary_history, const char *name) {
    if (result == VK_SUCCESS) {
        wait_fence(fence);
        check_output(test, reference, true);
        ++accepted_cases;
        if (expect_rejection && !completed_binary_history) {
            fprintf(stderr, "FAIL %s: expected the baseline guard rejection, received SUCCESS\n", name);
            ++g_fail;
        }
        printf("query acquire: %s accepted with final value/availability/padding checked%s\n", name,
               expect_rejection && completed_binary_history ? " (completed binary prefix can retire before admission)" : "");
        return true;
    }
    if (result != VK_ERROR_FEATURE_NOT_PRESENT) CHECK(result);
    check_output(test, reference, false);
    if (expect_rejection)
        ++rejected_cases;
    else {
        fprintf(stderr, "FAIL %s: legal query COPY rejected with FEATURE_NOT_PRESENT\n", name);
        ++g_fail;
    }
    printf("query acquire: %s %s baseline guard rejection\n", name, expect_rejection ? "expected" : "unexpected");
    return false;
}
static void finish_query(struct query_case *test) {
    vkDestroyQueryPool(g_dev, test->pool, NULL);
    vkDestroySemaphore(g_dev, test->reset_done, NULL);
    buf_destroy(&test->output);
}
static void single_queue_case(const struct acquire_scene *scene, bool sync2, bool binary, bool history) {
    struct query_case test = {0};
    prepare_query(&test, sync2);
    VkSemaphore gate = create_semaphore(binary);
    VkCommandBuffer producer = record_property(scene, &test), copy = record_copy(&test);
    const struct semaphore_op ready[] = {{scene->build_done, 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                         {test.reset_done, 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    const struct semaphore_op binary_signal = {gate, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkFence producer_fence = create_fence(), copy_fence = create_fence(), wait_only_fence = VK_NULL_HANDLE;
    CHECK(submit(g_queue, sync2, producer, ARRAY_SIZE(ready), ready, binary ? &binary_signal : NULL, producer_fence));
    wait_fence(producer_fence);
    const uint64_t reference = reference_value(&test);
    const struct semaphore_op wait = {gate, binary ? 0 : 1, VK_PIPELINE_STAGE_2_TRANSFER_BIT};
    if (history) {
        wait_only_fence = create_fence();
        CHECK(submit(g_queue, sync2, VK_NULL_HANDLE, 1, &wait, NULL, wait_only_fence));
    }
    VkResult result = submit(g_queue, sync2, copy, history ? 0 : 1, history ? NULL : &wait, NULL, copy_fence);
    if (!binary) release_gate(gate); /* Unconditional: no GPU observation controls this signal. */
    if (history) wait_fence(wait_only_fence);
    char name[96];
    snprintf(name, sizeof(name), "%s %s %s", sync2 ? "submit2" : "legacy", binary ? "completed-binary" : "timeline",
             history ? "historical-wait" : "same-batch-wait");
    bool accepted = consume_result(result, copy_fence, &test, reference, binary && history, name);
    if (binary && !history && !accepted) {
        /* Rejection never consumed B. Drain its one legitimate wait before
         * destroying the signaled semaphore; do not issue a second consumer. */
        VkFence drain = create_fence();
        CHECK(submit(g_queue, sync2, VK_NULL_HANDLE, 1, &wait, NULL, drain));
        wait_fence(drain);
        vkDestroyFence(g_dev, drain, NULL);
    }
    vkFreeCommandBuffers(g_dev, g_pool, 1, &producer);
    vkFreeCommandBuffers(g_dev, g_pool, 1, &copy);
    vkDestroyFence(g_dev, producer_fence, NULL);
    vkDestroyFence(g_dev, copy_fence, NULL);
    if (wait_only_fence) vkDestroyFence(g_dev, wait_only_fence, NULL);
    vkDestroySemaphore(g_dev, gate, NULL);
    finish_query(&test);
}
static void cross_queue_case(const struct acquire_scene *scene, bool sync2) {
    struct query_case test = {0};
    prepare_query(&test, sync2);
    VkSemaphore host_gate = create_semaphore(false), published = create_semaphore(false);
    VkCommandBuffer producer = record_property(scene, &test), copy = record_copy(&test);
    const struct semaphore_op waits[] = {{scene->build_done, 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                         {test.reset_done, 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                         {host_gate, 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    const struct semaphore_op signal = {published, 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    const struct semaphore_op acquire = {published, 1, VK_PIPELINE_STAGE_2_TRANSFER_BIT};
    VkFence producer_fence = create_fence(), copy_fence = create_fence();
    /* B is timeline: a binary producer depending on a future host H signal
     * would violate the prior-signal-chain requirements (03238/03873). */
    CHECK(submit(producer_queue, sync2, producer, ARRAY_SIZE(waits), waits, &signal, producer_fence));
    VkResult result = submit(g_queue, sync2, copy, 1, &acquire, NULL, copy_fence);
    release_gate(host_gate); /* Always releases P, even when the old software guard rejected C. */
    wait_fence(producer_fence);
    const uint64_t reference = reference_value(&test);
    consume_result(result, copy_fence, &test, reference, false,
                   sync2 ? "submit2 cross-queue H/B timelines" : "legacy cross-queue H/B timelines");
    vkFreeCommandBuffers(g_dev, g_pool, 1, &producer);
    vkFreeCommandBuffers(g_dev, g_pool, 1, &copy);
    vkDestroyFence(g_dev, producer_fence, NULL);
    vkDestroyFence(g_dev, copy_fence, NULL);
    vkDestroySemaphore(g_dev, published, NULL);
    vkDestroySemaphore(g_dev, host_gate, NULL);
    finish_query(&test);
}
static void destroy_device(void) {
    vkDestroyCommandPool(g_dev, g_pool, NULL);
    vkDestroyDevice(g_dev, NULL);
    PFN_vkDestroyDebugUtilsMessengerEXT destroy =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_instance, "vkDestroyDebugUtilsMessengerEXT");
    destroy(g_instance, messenger, NULL);
    vkDestroyInstance(g_instance, NULL);
}
int main(int argc, char **argv) {
    bool legacy = true, sync2 = true, single = true, cross = true;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--legacy-only"))
            sync2 = false;
        else if (!strcmp(argv[i], "--submit2-only"))
            legacy = false;
        else if (!strcmp(argv[i], "--single-queue-only"))
            cross = false;
        else if (!strcmp(argv[i], "--cross-queue-only"))
            single = false;
        else if (!strcmp(argv[i], "--expect-rejection"))
            expect_rejection = true;
        else {
            fprintf(stderr,
                    "usage: %s [--legacy-only|--submit2-only] [--single-queue-only|--cross-queue-only] "
                    "[--expect-rejection]\n",
                    argv[0]);
            return 2;
        }
    }
    if ((!legacy && !sync2) || (!single && !cross)) return 2;
    const char *layers = getenv("VK_INSTANCE_LAYERS");
    if (expect_rejection && layers) {
        const char *software = strstr(layers, "VK_LAYER_PA_RTP"),
                   *validation = strstr(layers, "VK_LAYER_KHRONOS_validation");
        if (software && validation && validation < software) {
            fputs("Expected-rejection controls require software above VVL; outer VVL retains rejected submits as pending.\n",
                  stderr);
            return 2;
        }
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    init_vulkan();
    submit_legacy = (PFN_vkQueueSubmit)vkGetDeviceProcAddr(g_dev, "vkQueueSubmit");
    submit_two = (PFN_vkQueueSubmit2)vkGetDeviceProcAddr(g_dev, "vkQueueSubmit2KHR");
    build_as = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(g_dev, "vkCmdBuildAccelerationStructuresKHR");
    write_property = (PFN_vkCmdWriteAccelerationStructuresPropertiesKHR)vkGetDeviceProcAddr(
        g_dev, "vkCmdWriteAccelerationStructuresPropertiesKHR");
    signal_timeline = (PFN_vkSignalSemaphore)vkGetDeviceProcAddr(g_dev, "vkSignalSemaphore");
    REQUIRE(submit_legacy && (!sync2 || submit_two) && build_as && write_property && signal_timeline);
    if (g_fail) {
        destroy_device();
        return 1;
    }
    if (second_queue_available) vkGetDeviceQueue(g_dev, g_qfamily, 1, &producer_queue);
    if (cross && !second_queue_available) {
        puts("SKIP cross-queue acquire controls: selected family has fewer than two queues");
        skipped_cases = (legacy ? 1 : 0) + (sync2 ? 1 : 0);
        if (!single) {
            destroy_device();
            return 77;
        }
    }
    struct acquire_scene scene = {0};
    create_scene(&scene);
    for (unsigned mode = 0; mode < 2; ++mode) {
        if ((mode == 0 && !legacy) || (mode == 1 && !sync2)) continue;
        if (single)
            for (unsigned test = 0; test < 4; ++test) single_queue_case(&scene, mode != 0, (test & 2) != 0, (test & 1) != 0);
        if (cross && second_queue_available) cross_queue_case(&scene, mode != 0);
    }
    vkDestroySemaphore(g_dev, scene.build_done, NULL);
    pDestroyAS(g_dev, scene.as, NULL);
    buf_destroy(&scene.scratch);
    buf_destroy(&scene.storage);
    buf_destroy(&scene.vertices);
    destroy_device();
    printf("Public query acquire: %s (%u accepted, %u expected rejections, %u skipped, %d failures, %u validation errors)\n",
           g_fail || atomic_load(&validation_errors) ? "FAIL" : "PASS", accepted_cases, rejected_cases, skipped_cases, g_fail,
           atomic_load(&validation_errors));
    return g_fail || atomic_load(&validation_errors) ? 1 : 0;
}
