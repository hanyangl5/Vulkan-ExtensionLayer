/* SPDX-License-Identifier: MIT
 * Public query epochs and native host acquisition. AS storage is never mapped.
 */
#include <vulkan/vulkan.h>
#include <stdatomic.h>
static VkResult query_create_device(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);
static VkResult query_create_instance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
#define vkCreateDevice query_create_device
#define vkCreateInstance query_create_instance
#define main query_unused_gpu_build_main
#include "test_gpu_build.c"
#undef main
#undef vkCreateDevice
#undef vkCreateInstance
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

static bool cross_queue_mode, second_queue_available;
static atomic_uint validation_errors;
static VkDebugUtilsMessengerEXT messenger;
static VKAPI_ATTR VkBool32 VKAPI_CALL query_debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                  VkDebugUtilsMessageTypeFlagsEXT type,
                                                  const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)type;
    (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++validation_errors;
        fprintf(stderr, "VALIDATION: %s\n", data->pMessage);
    }
    return VK_FALSE;
}
static VkResult query_create_instance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator, VkInstance *out) {
    const char *extensions[] = {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkDebugUtilsMessengerCreateInfoEXT debug = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .pNext = info->pNext,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
        .pfnUserCallback = query_debug};
    VkInstanceCreateInfo local = *info;
    local.pNext = &debug;
    local.enabledExtensionCount = ARRAY_SIZE(extensions);
    local.ppEnabledExtensionNames = extensions;
    VkResult result = vkCreateInstance(&local, allocator, out);
    if (result == VK_SUCCESS) {
        PFN_vkCreateDebugUtilsMessengerEXT create_messenger =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(*out, "vkCreateDebugUtilsMessengerEXT");
        debug.pNext = NULL;
        result = create_messenger(*out, &debug, NULL, &messenger);
    }
    return result;
}
static VkResult query_create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                    const VkAllocationCallbacks *allocator, VkDevice *device) {
    VkPhysicalDeviceHostQueryResetFeatures reset = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES,
                                                    .pNext = (void *)info->pNext,
                                                    .hostQueryReset = VK_TRUE};
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, .pNext = &reset, .timelineSemaphore = VK_TRUE};
    VkDeviceCreateInfo local = *info;
    local.pNext = &timeline;
    VkDeviceQueueCreateInfo queue;
    const float priorities[] = {1, 1};
    if (cross_queue_mode && info->queueCreateInfoCount == 1) {
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
        VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
        if (!families) return VK_ERROR_OUT_OF_HOST_MEMORY;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
        queue = info->pQueueCreateInfos[0];
        second_queue_available = queue.queueFamilyIndex < count && families[queue.queueFamilyIndex].queueCount >= 2;
        free(families);
        if (second_queue_available) {
            queue.queueCount = 2;
            queue.pQueuePriorities = priorities;
            local.pQueueCreateInfos = &queue;
        }
    }
    return vkCreateDevice(physical, &local, allocator, device);
}
static VkQueryPool pools[3];
static const VkQueryType types[] = {VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                                    VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE_KHR,
                                    VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR};
static PFN_vkCmdWriteAccelerationStructuresPropertiesKHR write_properties;
static uint64_t allowed[2][3];
static atomic_bool stop_reader;
static atomic_uint reader_errors, reader_calls;
static void read_loop(void) {
    while (!atomic_load(&stop_reader)) {
        for (unsigned i = 0; i < 3; ++i) {
            uint64_t result[2] = {0};
            VkResult status = vkGetQueryPoolResults(g_dev, pools[i], 0, 1, sizeof(result), result, 16,
                                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (status != VK_SUCCESS && status != VK_NOT_READY) atomic_fetch_add(&reader_errors, 1);
            if (result[1] && result[0] != allowed[0][i] && result[0] != allowed[1][i]) atomic_fetch_add(&reader_errors, 1);
        }
        atomic_fetch_add(&reader_calls, 1);
    }
}
#ifdef _WIN32
static DWORD WINAPI reader_thread(void *unused) {
    (void)unused;
    read_loop();
    return 0;
}
#else
static void *reader_thread(void *unused) {
    (void)unused;
    read_loop();
    return NULL;
}
#endif
static void query_record(VkCommandBuffer command, struct built_as *as, uint32_t primitives, struct gpu_buf *copy) {
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(command, &begin));
    if (copy) vkCmdFillBuffer(command, copy->buf, 0, copy->size, 0xcdcdcdcd);
    for (unsigned i = 0; i < 3; ++i) vkCmdResetQueryPool(command, pools[i], 0, 1);
    VkAccelerationStructureBuildRangeInfoKHR range = {.primitiveCount = primitives};
    const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
    pBuild(command, 1, &as->info, &ranges);
    VkMemoryBarrier dependency = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                                  VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &dependency, 0, NULL, 0, NULL);
    for (unsigned i = 0; i < 3; ++i) {
        write_properties(command, 1, &as->handle, types[i], pools[i], 0);
        if (copy)
            vkCmdCopyQueryPoolResults(command, pools[i], 0, 1, copy->buf, i * 32, 32,
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT);
    }
    VkMemoryBarrier host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(command));
}
static void get_values(uint64_t values[3]) {
    for (unsigned i = 0; i < 3; ++i)
        CHECK(vkGetQueryPoolResults(g_dev, pools[i], 0, 1, 8, &values[i], 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
}
static void check_copy(const struct gpu_buf *copy, const uint64_t values[3]) {
    for (unsigned i = 0; i < 3; ++i) {
        const uint64_t *data = (const void *)((const uint8_t *)copy->mapped + i * 32);
        expect_u("GPU copied property", data[0] == values[i], 1);
        expect_u("GPU copied availability", data[1] != 0, 1);
        expect_u("GPU copy preserves gap1", data[2] == UINT64_C(0xcdcdcdcdcdcdcdcd), 1);
        expect_u("GPU copy preserves gap2", data[3] == UINT64_C(0xcdcdcdcdcdcdcdcd), 1);
    }
}
/* Queue0's producer waits for a host timeline signal. Queue1's copy waits
 * for that producer at TRANSFER. Native Vulkan must copy the fresh properties;
 * the bounded software backend must reject before submitting its CS formatter. */
static void cross_queue_test(struct built_as *as, struct gpu_buf *copy, VkCommandBuffer commands[2]) {
    VkQueue consumer;
    vkGetDeviceQueue(g_dev, g_qfamily, 1, &consumer);
    query_record(commands[0], as, 1, NULL);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(commands[1], &begin));
    vkCmdFillBuffer(commands[1], copy->buf, 0, copy->size, 0xcdcdcdcd);
    for (unsigned i = 0; i < 3; ++i)
        vkCmdCopyQueryPoolResults(commands[1], pools[i], 0, 1, copy->buf, i * 32, 32,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT);
    VkMemoryBarrier host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(commands[1], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(commands[1]));
    VkSemaphoreTypeCreateInfo type = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo create = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type, 0};
    VkSemaphore gate, ready;
    CHECK(vkCreateSemaphore(g_dev, &create, NULL, &gate));
    CHECK(vkCreateSemaphore(g_dev, &create, NULL, &ready));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(g_dev, &fence_info, NULL, &fence));
    const uint64_t value = 1;
    VkTimelineSemaphoreSubmitInfo timeline = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                              .waitSemaphoreValueCount = 1,
                                              .pWaitSemaphoreValues = &value,
                                              .signalSemaphoreValueCount = 1,
                                              .pSignalSemaphoreValues = &value};
    VkPipelineStageFlags all = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo producer = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .pNext = &timeline,
                             .waitSemaphoreCount = 1,
                             .pWaitSemaphores = &gate,
                             .pWaitDstStageMask = &all,
                             .commandBufferCount = 1,
                             .pCommandBuffers = &commands[0],
                             .signalSemaphoreCount = 1,
                             .pSignalSemaphores = &ready};
    CHECK(vkQueueSubmit(g_queue, 1, &producer, fence));
    VkTimelineSemaphoreSubmitInfo consumer_timeline = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &value};
    VkSubmitInfo consume = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .pNext = &consumer_timeline,
                            .waitSemaphoreCount = 1,
                            .pWaitSemaphores = &ready,
                            .pWaitDstStageMask = &transfer,
                            .commandBufferCount = 1,
                            .pCommandBuffers = &commands[1]};
    VkResult result = vkQueueSubmit(consumer, 1, &consume, VK_NULL_HANDLE);
    expect_u("cross-queue TRANSFER wait qualification", result == (g_software ? VK_ERROR_FEATURE_NOT_PRESENT : VK_SUCCESS), 1);
    VkSemaphoreSignalInfo signal = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, gate, 1};
    PFN_vkSignalSemaphore signal_semaphore = (void *)vkGetDeviceProcAddr(g_dev, "vkSignalSemaphore");
    CHECK(signal_semaphore(g_dev, &signal));
    CHECK(vkWaitForFences(g_dev, 1, &fence, VK_TRUE, UINT64_MAX));
    if (g_software) {
        /* VVL records a rejected native submit before seeing the layer's
         * bounded error. An idle acquire retires that validation bookkeeping. */
        CHECK(vkQueueWaitIdle(consumer));
        consume.pWaitDstStageMask = &all;
        CHECK(vkQueueSubmit(consumer, 1, &consume, VK_NULL_HANDLE));
    }
    CHECK(vkQueueWaitIdle(consumer));
    get_values(allowed[0]);
    check_copy(copy, allowed[0]);

    /* A previous submission's narrower wait remains a qualification boundary
     * even when that semaphore happens to be ready. Queue idle clears it. */
    timeline.signalSemaphoreValueCount = 0;
    VkSubmitInfo history = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .pNext = &timeline,
                            .waitSemaphoreCount = 1,
                            .pWaitSemaphores = &gate,
                            .pWaitDstStageMask = &transfer};
    CHECK(vkQueueSubmit(consumer, 1, &history, VK_NULL_HANDLE));
    consume.waitSemaphoreCount = 0;
    consumer_timeline.waitSemaphoreValueCount = 0;
    result = vkQueueSubmit(consumer, 1, &consume, VK_NULL_HANDLE);
    expect_u("prior-call narrow wait history", result == (g_software ? VK_ERROR_FEATURE_NOT_PRESENT : VK_SUCCESS), 1);
    CHECK(vkQueueWaitIdle(consumer));
    CHECK(vkQueueSubmit(consumer, 1, &consume, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(consumer));
    check_copy(copy, allowed[0]);

    PFN_vkQueueSubmit2 submit2 = (void *)vkGetDeviceProcAddr(g_dev, "vkQueueSubmit2KHR");
    VkSemaphoreSubmitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                  .semaphore = gate,
                                  .value = 1,
                                  .stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT};
    VkSubmitInfo2 history2 = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = 1, .pWaitSemaphoreInfos = &wait};
    CHECK(submit2(consumer, 1, &history2, VK_NULL_HANDLE));
    VkCommandBufferSubmitInfo command = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = commands[1]};
    VkSubmitInfo2 consume2 = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &command};
    result = submit2(consumer, 1, &consume2, VK_NULL_HANDLE);
    expect_u("submit2 narrow wait history", result == (g_software ? VK_ERROR_FEATURE_NOT_PRESENT : VK_SUCCESS), 1);
    CHECK(vkDeviceWaitIdle(g_dev));
    CHECK(submit2(consumer, 1, &consume2, VK_NULL_HANDLE));
    CHECK(vkDeviceWaitIdle(g_dev));
    check_copy(copy, allowed[0]);
    vkDestroyFence(g_dev, fence, NULL);
    vkDestroySemaphore(g_dev, ready, NULL);
    vkDestroySemaphore(g_dev, gate, NULL);
}
int main(int argc, char **argv) {
    cross_queue_mode = argc == 2 && !strcmp(argv[1], "--cross-queue");
    const char *layers = getenv("VK_INSTANCE_LAYERS");
    g_software = layers && strstr(layers, "VK_LAYER_VKRT_SOFTWARE_RAY_QUERY");
    init_vulkan();
    if (cross_queue_mode && !second_queue_available) {
        fprintf(stderr, "SKIP cross-queue query fixture requires two compute queues in one family\n");
        vkDestroyCommandPool(g_dev, g_pool, NULL);
        vkDestroyDevice(g_dev, NULL);
        PFN_vkDestroyDebugUtilsMessengerEXT destroy = (void *)vkGetInstanceProcAddr(g_instance, "vkDestroyDebugUtilsMessengerEXT");
        destroy(g_instance, messenger, NULL);
        vkDestroyInstance(g_instance, NULL);
        return 77;
    }
    pBuild = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(g_dev, "vkCmdBuildAccelerationStructuresKHR");
    write_properties = (PFN_vkCmdWriteAccelerationStructuresPropertiesKHR)vkGetDeviceProcAddr(
        g_dev, "vkCmdWriteAccelerationStructuresPropertiesKHR");
    for (unsigned i = 0; i < 3; ++i) {
        VkQueryPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = types[i], .queryCount = 1};
        CHECK(vkCreateQueryPool(g_dev, &info, NULL, &pools[i]));
    }
    struct gpu_buf vertices, copy, tail_buffer;
    buf_create(4 * 9 * sizeof(float),
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &vertices);
    for (unsigned i = 0; i < 4; ++i) {
        const float triangle[9] = {-1, -1, 0, 1, -1, 0, 0, 1, 0};
        memcpy((float *)vertices.mapped + i * 9, triangle, sizeof(triangle));
    }
    VkAccelerationStructureGeometryKHR geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                               .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                               .vertexData.deviceAddress = vertices.addr,
                               .vertexStride = 12,
                               .maxVertex = 11,
                               .indexType = VK_INDEX_TYPE_NONE_KHR}};
    struct built_as as = {0};
    as.info =
        (VkAccelerationStructureBuildGeometryInfoKHR){.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                                                      .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                                      .flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR,
                                                      .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                                                      .geometryCount = 1,
                                                      .pGeometries = &geometry};
    uint32_t max_count = 4;
    VkAccelerationStructureBuildSizesInfoKHR sizes = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    PFN_vkGetAccelerationStructureBuildSizesKHR get_sizes =
        (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureBuildSizesKHR");
    get_sizes(g_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &as.info, &max_count, &sizes);
    as.size = sizes.accelerationStructureSize;
    buf_create(as.size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &as.output);
    buf_create(sizes.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &as.scratch);
    VkAccelerationStructureCreateInfoKHR create = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                   .buffer = as.output.buf,
                                                   .size = as.size,
                                                   .type = as.info.type};
    CHECK(pCreateAS(g_dev, &create, NULL, &as.handle));
    as.info.dstAccelerationStructure = as.handle;
    as.info.scratchData.deviceAddress = (as.scratch.addr + 255) & ~UINT64_C(255);
    buf_create(128, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               &copy);
    buf_create(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &tail_buffer);
    VkCommandBuffer commands[2];
    VkCommandBufferAllocateInfo allocate = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_pool,
                                            VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
    CHECK(vkAllocateCommandBuffers(g_dev, &allocate, commands));
    if (cross_queue_mode) {
        cross_queue_test(&as, &copy, commands);
        vkFreeCommandBuffers(g_dev, g_pool, 2, commands);
        goto cleanup;
    }
    query_record(commands[0], &as, 1, &copy);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(commands[1], &begin));
    vkCmdFillBuffer(commands[1], tail_buffer.buf, 0, 16, 0);
    CHECK(vkEndCommandBuffer(commands[1]));
    VkSemaphoreTypeCreateInfo timeline_type = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo semaphore_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &timeline_type, 0};
    VkSemaphore semaphore;
    CHECK(vkCreateSemaphore(g_dev, &semaphore_info, NULL, &semaphore));
    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(g_dev, &fence_info, NULL, &fence));
    uint64_t wait_value = 1;
    VkTimelineSemaphoreSubmitInfo timeline = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 1, &wait_value, 0, NULL};
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submits[2] = {{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &commands[0]},
                               {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                .pNext = &timeline,
                                .waitSemaphoreCount = 1,
                                .pWaitSemaphores = &semaphore,
                                .pWaitDstStageMask = &wait_stage,
                                .commandBufferCount = 1,
                                .pCommandBuffers = &commands[1]}};
    CHECK(vkQueueSubmit(g_queue, 2, submits, fence));
    /* The second batch is blocked. This WAIT must acquire the first batch's
     * query prefix without waiting for the submit fence or later batch. */
    get_values(allowed[0]);
    VkSemaphoreSignalInfo signal = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, semaphore, 1};
    PFN_vkSignalSemaphore signal_semaphore = (PFN_vkSignalSemaphore)vkGetDeviceProcAddr(g_dev, "vkSignalSemaphore");
    CHECK(signal_semaphore(g_dev, &signal));
    CHECK(vkWaitForFences(g_dev, 1, &fence, VK_TRUE, UINT64_MAX));
    check_copy(&copy, allowed[0]);
    memcpy(allowed[1], allowed[0], sizeof(allowed[1]));
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, reader_thread, NULL, 0, NULL);
#else
    pthread_t thread;
    pthread_create(&thread, NULL, reader_thread, NULL);
#endif
    for (unsigned i = 0; i < 64; ++i) {
        CHECK(vkResetFences(g_dev, 1, &fence));
        CHECK(vkQueueSubmit(g_queue, 1, submits, fence));
        CHECK(vkWaitForFences(g_dev, 1, &fence, VK_TRUE, UINT64_MAX));
    }
    atomic_store(&stop_reader, true);
#ifdef _WIN32
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
#else
    pthread_join(thread, NULL);
#endif
    expect_u("concurrent host publication", atomic_load(&reader_errors), 0);
    expect_u("concurrent reader ran", atomic_load(&reader_calls) != 0, 1);
    CHECK(vkResetCommandBuffer(commands[0], 0));
    get_values(allowed[1]);
    for (unsigned i = 0; i < 3; ++i) expect_u("result survives CB reset", allowed[1][i] == allowed[0][i], 1);
    query_record(commands[0], &as, 4, &copy);
    PFN_vkResetQueryPool reset_query = (PFN_vkResetQueryPool)vkGetDeviceProcAddr(g_dev, "vkResetQueryPool");
    for (unsigned i = 0; i < 3; ++i) reset_query(g_dev, pools[i], 0, 1);
    CHECK(vkResetFences(g_dev, 1, &fence));
    CHECK(vkQueueSubmit(g_queue, 1, submits, fence));
    get_values(allowed[1]);
    CHECK(vkWaitForFences(g_dev, 1, &fence, VK_TRUE, UINT64_MAX));
    expect_u("GPU property changes with rebuilt data", allowed[1][0] > allowed[0][0], 1);
    check_copy(&copy, allowed[1]);
    vkFreeCommandBuffers(g_dev, g_pool, 1, &commands[0]);
    uint64_t preserved[3];
    get_values(preserved);
    for (unsigned i = 0; i < 3; ++i) expect_u("result survives CB free", preserved[i] == allowed[1][i], 1);
    CHECK(vkResetCommandPool(g_dev, g_pool, 0));
    get_values(preserved);
    for (unsigned i = 0; i < 3; ++i) expect_u("result survives pool reset", preserved[i] == allowed[1][i], 1);
    allocate.commandBufferCount = 1;
    CHECK(vkAllocateCommandBuffers(g_dev, &allocate, commands));
    begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    CHECK(vkBeginCommandBuffer(commands[0], &begin));
    vkCmdResetQueryPool(commands[0], pools[0], 0, 1);
    VkResult simultaneous = vkEndCommandBuffer(commands[0]);
    expect_u("simultaneous query recording fails explicitly", simultaneous == VK_ERROR_FEATURE_NOT_PRESENT, 1);
    vkDestroyFence(g_dev, fence, NULL);
    vkDestroySemaphore(g_dev, semaphore, NULL);
cleanup:
    for (unsigned i = 0; i < 3; ++i) vkDestroyQueryPool(g_dev, pools[i], NULL);
    destroy_as(&as);
    buf_destroy(&vertices);
    buf_destroy(&copy);
    buf_destroy(&tail_buffer);
    vkDestroyCommandPool(g_dev, g_pool, NULL);
    vkDestroyDevice(g_dev, NULL);
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_instance, "vkDestroyDebugUtilsMessengerEXT");
    destroy_messenger(g_instance, messenger, NULL);
    vkDestroyInstance(g_instance, NULL);
    printf("Public AS queries: %s (%d failures, %u validation errors, %u concurrent reads)\n",
           g_fail || atomic_load(&validation_errors) ? "FAIL" : "PASS", g_fail, atomic_load(&validation_errors),
           atomic_load(&reader_calls));
    return g_fail || atomic_load(&validation_errors) ? 1 : 0;
}
