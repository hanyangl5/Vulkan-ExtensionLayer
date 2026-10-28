/* SPDX-License-Identifier: MIT
 * Public AS query replay. AS/scratch buffers are device local and never mapped.
 *
 * This fixture uses the software backend's documented serialization format:
 * size = queried BUILD capacity + align16(56 + 8 * retained instances).
 * Inputs with 1/3/5 non-null references give three distinct, independent
 * expected values. No queried value or GPU header is used as its own oracle.
 *
 * Every occurrence executes RESET -> BUILD -> WRITE -> COPY. Query reset's
 * scopes order earlier/later uses of the same index. ALL_COMMANDS barriers also
 * order AS/scratch/input/result-buffer hazards. Across submissions, timeline
 * waits are ALL_COMMANDS. These tests do not require narrower-wait scheduling.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <vulkan/vulkan.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>
static VkResult replay_create_instance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
static VkResult replay_create_device(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);
#define vkCreateInstance replay_create_instance
#define vkCreateDevice replay_create_device
#define VKRT_TEST_FIXTURE_ONLY
#include "00_single_tri/main.c"
#undef vkCreateInstance
#undef vkCreateDevice
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

enum { INPUT_COUNT = 5, QUERY_COUNT = 2, RESULT_STRIDE = 32, EPOCH_BYTES = QUERY_COUNT * RESULT_STRIDE, ARCHIVE_SLOTS = 9 };
static const uint64_t timeout_ns = UINT64_C(15000000000);
static const uint64_t gap = UINT64_C(0xcdcdcdcdcdcdcdcd);
static atomic_uint validation_errors;
static VkDebugUtilsMessengerEXT messenger;
static PFN_vkCmdBuildAccelerationStructuresKHR build_as;
static PFN_vkCmdWriteAccelerationStructuresPropertiesKHR write_as;
static PFN_vkCmdResetQueryPool reset_query;
static PFN_vkCmdCopyQueryPoolResults copy_query;
static PFN_vkGetQueryPoolResults get_query;
static PFN_vkQueueSubmit queue_submit;
static PFN_vkSignalSemaphore signal_semaphore;
static PFN_vkWaitSemaphores wait_semaphores;

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

static VkResult replay_create_instance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator, VkInstance *out) {
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

static VkResult replay_create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                     const VkAllocationCallbacks *allocator, VkDevice *out) {
    VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
                                                          .pNext = (void *)info->pNext,
                                                          .timelineSemaphore = VK_TRUE};
    VkDeviceCreateInfo local = *info;
    local.pNext = &timeline;
    return vkCreateDevice(physical, &local, allocator, out);
}

struct object {
    struct gpu_buf output, scratch;
    VkAccelerationStructureKHR handle;
    VkDeviceAddress public_address;
    VkDeviceSize required, capacity;
    VkAccelerationStructureBuildGeometryInfoKHR info;
    VkAccelerationStructureBuildRangeInfoKHR range;
};
struct scene {
    struct object blas, tlas;
    struct gpu_buf vertices, instances, results, archive, tail;
    VkAccelerationStructureGeometryKHR triangle, instance_geometry;
    VkQueryPool pools[QUERY_COUNT];
};
static const VkQueryType query_types[QUERY_COUNT] = {VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE_KHR,
                                                     VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR};

static void barrier(VkCommandBuffer command) {
    VkMemoryBarrier memory = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memory, 0, NULL, 0,
                         NULL);
}
static VkCommandBuffer allocate_command(VkCommandBufferLevel level) {
    VkCommandBufferAllocateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_pool, .level = level, .commandBufferCount = 1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(g_dev, &info, &command));
    return command;
}
static void begin_command(VkCommandBuffer command, VkCommandBufferLevel level, bool simultaneous) {
    VkCommandBufferInheritanceInfo inheritance = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
    VkCommandBufferBeginInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                     .flags = simultaneous ? VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT : 0,
                                     .pInheritanceInfo = level == VK_COMMAND_BUFFER_LEVEL_SECONDARY ? &inheritance : NULL};
    CHECK(vkBeginCommandBuffer(command, &info));
}
static VkFence create_fence(void) {
    VkFenceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(g_dev, &info, NULL, &fence));
    return fence;
}
static void wait_fence(VkFence fence) { CHECK(vkWaitForFences(g_dev, 1, &fence, VK_TRUE, timeout_ns)); }
static void submit_and_wait(uint32_t count, const VkCommandBuffer *commands) {
    VkFence fence = create_fence();
    VkSubmitInfo info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = count, .pCommandBuffers = commands};
    CHECK(queue_submit(g_queue, 1, &info, fence));
    wait_fence(fence);
    vkDestroyFence(g_dev, fence, NULL);
}

static void create_object(struct object *object, VkAccelerationStructureTypeKHR type,
                          const VkAccelerationStructureGeometryKHR *geometry, uint32_t count) {
    object->info =
        (VkAccelerationStructureBuildGeometryInfoKHR){.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                                                      .type = type,
                                                      .flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR,
                                                      .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                                                      .geometryCount = 1,
                                                      .pGeometries = geometry};
    object->range.primitiveCount = count;
    PFN_vkGetAccelerationStructureBuildSizesKHR sizes =
        (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetInstanceProcAddr(g_instance, "vkGetAccelerationStructureBuildSizesKHR");
    VkAccelerationStructureBuildSizesInfoKHR size = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    sizes(g_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &object->info, &count, &size);
    object->required = size.accelerationStructureSize;
    object->capacity = object->required + 4096; /* Reject a capacity-as-property implementation. */
    buf_create(object->capacity, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &object->output);
    buf_create(size.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &object->scratch);
    REQUIRE(!object->output.mapped && !object->scratch.mapped);
    VkAccelerationStructureCreateInfoKHR create = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                   .buffer = object->output.buf,
                                                   .size = object->capacity,
                                                   .type = type};
    CHECK(pCreateAS(g_dev, &create, NULL, &object->handle));
    object->info.dstAccelerationStructure = object->handle;
    object->info.scratchData.deviceAddress = (object->scratch.addr + 255) & ~UINT64_C(255);
    PFN_vkGetAccelerationStructureDeviceAddressKHR address =
        (PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureDeviceAddressKHR");
    VkAccelerationStructureDeviceAddressInfoKHR info = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                                                        .accelerationStructure = object->handle};
    object->public_address = address(g_dev, &info);
    REQUIRE(object->public_address != 0);
}
static void destroy_object(struct object *object) {
    pDestroyAS(g_dev, object->handle, NULL);
    buf_destroy(&object->scratch);
    buf_destroy(&object->output);
}

static uint64_t expected_value(const struct scene *scene, unsigned query, uint32_t active_instances) {
    if (query == 1) return scene->tlas.required;
    /* The wire policy has one relocation pointer per retained instance. */
    return scene->tlas.required + ((UINT64_C(56) + 8 * active_instances + 15) & ~UINT64_C(15));
}
static void instance_data(const struct scene *scene, uint32_t active, VkAccelerationStructureInstanceKHR input[INPUT_COUNT]) {
    for (uint32_t i = 0; i < INPUT_COUNT; ++i)
        input[i] =
            (VkAccelerationStructureInstanceKHR){.transform.matrix = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}},
                                                 .instanceCustomIndex = i,
                                                 .mask = 255,
                                                 .accelerationStructureReference = i < active ? scene->blas.public_address : 0};
}
static void inline_control(VkCommandBuffer command, const struct scene *scene, int archive_slot, int active) {
    barrier(command);
    if (archive_slot >= 0) {
        VkBufferCopy region = {0, (VkDeviceSize)archive_slot * EPOCH_BYTES, EPOCH_BYTES};
        vkCmdCopyBuffer(command, scene->results.buf, scene->archive.buf, 1, &region);
        barrier(command);
    }
    if (active >= 0) {
        VkAccelerationStructureInstanceKHR input[INPUT_COUNT];
        instance_data(scene, (uint32_t)active, input);
        /* Values are command inputs; a replay implementation must own them. */
        vkCmdUpdateBuffer(command, scene->instances.buf, 0, sizeof(input), input);
        memset(input, 0xa7, sizeof(input));
        barrier(command);
    }
    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                            .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
}
static VkCommandBuffer record_control(const struct scene *scene, int slot, int active) {
    VkCommandBuffer command = allocate_command(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    begin_command(command, VK_COMMAND_BUFFER_LEVEL_PRIMARY, false);
    inline_control(command, scene, slot, active);
    CHECK(vkEndCommandBuffer(command));
    return command;
}
static VkCommandBuffer record_queries(const struct scene *scene, VkCommandBufferLevel level) {
    VkCommandBuffer command = allocate_command(level);
    begin_command(command, level, true);
    barrier(command);
    for (unsigned i = 0; i < QUERY_COUNT; ++i) reset_query(command, scene->pools[i], 0, 1);
    vkCmdFillBuffer(command, scene->results.buf, 0, EPOCH_BYTES, 0xcdcdcdcd);
    barrier(command);
    const VkAccelerationStructureBuildRangeInfoKHR *range = &scene->tlas.range;
    build_as(command, 1, &scene->tlas.info, &range);
    barrier(command);
    for (unsigned i = 0; i < QUERY_COUNT; ++i) {
        write_as(command, 1, &scene->tlas.handle, query_types[i], scene->pools[i], 0);
        copy_query(command, scene->pools[i], 0, 1, scene->results.buf, i * RESULT_STRIDE, RESULT_STRIDE,
                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT);
    }
    barrier(command);
    CHECK(vkEndCommandBuffer(command));
    return command;
}

static void check_archive(const struct scene *scene, unsigned slot, uint32_t active) {
    for (unsigned query = 0; query < QUERY_COUNT; ++query) {
        uint64_t actual[4];
        memcpy(actual, (const uint8_t *)scene->archive.mapped + slot * EPOCH_BYTES + query * RESULT_STRIDE, sizeof(actual));
        uint64_t expected = expected_value(scene, query, active);
        if (actual[0] != expected || !actual[1] || actual[2] != gap || actual[3] != gap) {
            fprintf(stderr, "FAIL archive%u query%u active=%u actual=%llu available=%llu gaps=%llx/%llx expected=%llu\n", slot,
                    query, active, (unsigned long long)actual[0], (unsigned long long)actual[1], (unsigned long long)actual[2],
                    (unsigned long long)actual[3], (unsigned long long)expected);
            ++g_fail;
        }
    }
}
static void check_host_values(const struct scene *scene, uint32_t active) {
    for (unsigned query = 0; query < QUERY_COUNT; ++query) {
        uint64_t actual[2] = {0};
        CHECK(get_query(g_dev, scene->pools[query], 0, 1, sizeof(actual), actual, sizeof(actual),
                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT));
        REQUIRE(actual[1] && actual[0] == expected_value(scene, query, active));
    }
}

static void serial_reuse(const struct scene *scene, VkCommandBuffer query) {
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t active = i ? 3 : 1;
        VkCommandBuffer commands[] = {record_control(scene, -1, active), query, record_control(scene, (int)i, -1)};
        submit_and_wait(ARRAY_SIZE(commands), commands);
        check_archive(scene, i, active);
        check_host_values(scene, active);
        vkFreeCommandBuffers(g_dev, g_pool, 1, &commands[0]);
        vkFreeCommandBuffers(g_dev, g_pool, 1, &commands[2]);
    }
    puts("query replay: serial recording reuse with independent GPU inputs passed");
}
static void repeated_primary(const struct scene *scene, VkCommandBuffer query) {
    VkCommandBuffer control[] = {record_control(scene, -1, 1), record_control(scene, 2, 3), record_control(scene, 3, 5),
                                 record_control(scene, 4, -1)};
    VkCommandBuffer commands[] = {control[0], query, control[1], query, control[2], query, control[3]};
    submit_and_wait(ARRAY_SIZE(commands), commands);
    check_archive(scene, 2, 1);
    check_archive(scene, 3, 3);
    check_archive(scene, 4, 5);
    check_host_values(scene, 5);
    vkFreeCommandBuffers(g_dev, g_pool, ARRAY_SIZE(control), control);
    puts("query replay: one VkSubmitInfo executes the same primary three times with values 1/3/5 passed");
}
static void repeated_secondary(const struct scene *scene) {
    VkCommandBuffer child = record_queries(scene, VK_COMMAND_BUFFER_LEVEL_SECONDARY);
    VkCommandBuffer parent = allocate_command(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    begin_command(parent, VK_COMMAND_BUFFER_LEVEL_PRIMARY, false);
    inline_control(parent, scene, -1, 1);
    vkCmdExecuteCommands(parent, 1, &child);
    inline_control(parent, scene, 5, 5);
    vkCmdExecuteCommands(parent, 1, &child);
    inline_control(parent, scene, 6, -1);
    CHECK(vkEndCommandBuffer(parent));
    submit_and_wait(1, &parent);
    check_archive(scene, 5, 1);
    check_archive(scene, 6, 5);
    check_host_values(scene, 5);
    vkFreeCommandBuffers(g_dev, g_pool, 1, &parent);
    vkFreeCommandBuffers(g_dev, g_pool, 1, &child);
    puts("query replay: one primary executes the same simultaneous secondary twice with values 1/5 passed");
}

static void sleep_briefly(void) {
#ifdef _WIN32
    Sleep(5);
#else
    struct timespec interval = {0, 5000000};
    nanosleep(&interval, NULL);
#endif
}
static uint64_t milliseconds(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
#endif
}
static bool wait_atomic(const atomic_bool *done, uint64_t timeout_ms) {
    const uint64_t start = milliseconds();
    do {
        if (atomic_load(done)) return true;
        sleep_briefly();
    } while (milliseconds() - start < timeout_ms);
    return atomic_load(done);
}
struct submit_job {
    VkSubmitInfo info;
    VkFence fence;
    VkResult result;
    atomic_bool done;
};
struct read_job {
    const struct scene *scene;
    uint64_t values[QUERY_COUNT][2];
    VkResult result;
    atomic_bool done;
};
static void submit_work(struct submit_job *job) {
    job->result = queue_submit(g_queue, 1, &job->info, job->fence);
    atomic_store(&job->done, true);
}
static void read_work(struct read_job *job) {
    job->result = VK_SUCCESS;
    for (unsigned i = 0; i < QUERY_COUNT; ++i) {
        VkResult result =
            get_query(g_dev, job->scene->pools[i], 0, 1, sizeof(job->values[i]), job->values[i], sizeof(job->values[i]),
                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (result != VK_SUCCESS) job->result = result;
    }
    atomic_store(&job->done, true);
}
#ifdef _WIN32
typedef HANDLE thread_handle;
static DWORD WINAPI submit_thread(void *job) {
    submit_work(job);
    return 0;
}
static DWORD WINAPI read_thread(void *job) {
    read_work(job);
    return 0;
}
static thread_handle start_thread(bool read, void *job) {
    HANDLE thread = CreateThread(NULL, 0, read ? read_thread : submit_thread, job, 0, NULL);
    if (!thread) {
        fputs("Cannot create test worker\n", stderr);
        exit(1);
    }
    return thread;
}
static void join_thread(thread_handle thread) {
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
}
#else
typedef pthread_t thread_handle;
static void *submit_thread(void *job) {
    submit_work(job);
    return NULL;
}
static void *read_thread(void *job) {
    read_work(job);
    return NULL;
}
static thread_handle start_thread(bool read, void *job) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, read ? read_thread : submit_thread, job)) {
        fputs("Cannot create test worker\n", stderr);
        exit(1);
    }
    return thread;
}
static void join_thread(thread_handle thread) { pthread_join(thread, NULL); }
#endif
static VkSemaphore create_timeline(void) {
    VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
    VkSemaphore semaphore;
    CHECK(vkCreateSemaphore(g_dev, &info, NULL, &semaphore));
    return semaphore;
}
static void signal_value(VkSemaphore semaphore, uint64_t value) {
    VkSemaphoreSignalInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = semaphore, .value = value};
    CHECK(signal_semaphore(g_dev, &info));
}

static void overlapping_submissions(const struct scene *scene, VkCommandBuffer query) {
    VkCommandBuffer setup = record_control(scene, -1, 1);
    submit_and_wait(1, &setup);
    vkFreeCommandBuffers(g_dev, g_pool, 1, &setup);
    VkCommandBuffer archive_a = record_control(scene, 7, -1), change_b = record_control(scene, -1, 3);
    VkCommandBuffer archive_b = record_control(scene, 8, -1);
    VkCommandBuffer tail = allocate_command(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    begin_command(tail, VK_COMMAND_BUFFER_LEVEL_PRIMARY, false);
    vkCmdFillBuffer(tail, scene->tail.buf, 0, 4, 0x12345678);
    CHECK(vkEndCommandBuffer(tail));
    VkSemaphore gate = create_timeline(), prefix_done = create_timeline();
    VkFence fence_a = create_fence(), fence_b = create_fence();
    const uint64_t one = 1, two = 2;
    const VkPipelineStageFlags all = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo first_timeline = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                                    .waitSemaphoreValueCount = 1,
                                                    .pWaitSemaphoreValues = &one,
                                                    .signalSemaphoreValueCount = 1,
                                                    .pSignalSemaphoreValues = &one};
    VkTimelineSemaphoreSubmitInfo tail_timeline = {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = 1, .pWaitSemaphoreValues = &two};
    VkCommandBuffer first_commands[] = {query, archive_a};
    VkSubmitInfo first[] = {{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .pNext = &first_timeline,
                             .waitSemaphoreCount = 1,
                             .pWaitSemaphores = &gate,
                             .pWaitDstStageMask = &all,
                             .commandBufferCount = ARRAY_SIZE(first_commands),
                             .pCommandBuffers = first_commands,
                             .signalSemaphoreCount = 1,
                             .pSignalSemaphores = &prefix_done},
                            {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .pNext = &tail_timeline,
                             .waitSemaphoreCount = 1,
                             .pWaitSemaphores = &gate,
                             .pWaitDstStageMask = &all,
                             .commandBufferCount = 1,
                             .pCommandBuffers = &tail}};
    CHECK(queue_submit(g_queue, ARRAY_SIZE(first), first, fence_a));
    VkSemaphore second_waits[] = {prefix_done, gate};
    uint64_t second_values[] = {1, 2};
    VkPipelineStageFlags second_stages[] = {all, all};
    VkTimelineSemaphoreSubmitInfo second_timeline = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                                     .waitSemaphoreValueCount = ARRAY_SIZE(second_waits),
                                                     .pWaitSemaphoreValues = second_values};
    VkCommandBuffer second_commands[] = {change_b, query, archive_b};
    struct submit_job submit = {.info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                         .pNext = &second_timeline,
                                         .waitSemaphoreCount = ARRAY_SIZE(second_waits),
                                         .pWaitSemaphores = second_waits,
                                         .pWaitDstStageMask = second_stages,
                                         .commandBufferCount = ARRAY_SIZE(second_commands),
                                         .pCommandBuffers = second_commands},
                                .fence = fence_b};
    thread_handle submitting = start_thread(false, &submit);
    /* The first occurrence has not run. Preparing B must not wait for A's
     * private query marker or for A's whole-submission retirement fence. */
    const bool returned_before_a = wait_atomic(&submit.done, 10000);
    REQUIRE(returned_before_a);
    signal_value(gate, 1);
    VkSemaphoreWaitInfo ready = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &prefix_done, .pValues = &one};
    CHECK(wait_semaphores(g_dev, &ready, timeout_ns));
    if (!wait_atomic(&submit.done, 1000)) {
        /* Rescue a whole-submit-fence deadlock and still report failure. */
        fprintf(stderr, "FAIL Submit B still waits for A after prefix completion\n");
        ++g_fail;
        signal_value(gate, 2);
        if (!wait_atomic(&submit.done, 15000)) exit(1);
        join_thread(submitting);
        CHECK(submit.result);
        wait_fence(fence_a);
        wait_fence(fence_b);
        goto completed;
    }
    join_thread(submitting);
    CHECK(submit.result);
    REQUIRE(vkGetFenceStatus(g_dev, fence_a) == VK_NOT_READY);
    REQUIRE(vkGetFenceStatus(g_dev, fence_b) == VK_NOT_READY);
    struct read_job read = {.scene = scene};
    thread_handle reading = start_thread(true, &read);
    const bool prefix_available = wait_atomic(&read.done, 10000);
    REQUIRE(prefix_available);
    REQUIRE(vkGetFenceStatus(g_dev, fence_a) == VK_NOT_READY);
    REQUIRE(vkGetFenceStatus(g_dev, fence_b) == VK_NOT_READY);
    /* The pending B reset has not executed. Host WAIT must still acquire A,
     * even though B has its own pending marker/payload and A's tail is blocked. */
    if (prefix_available) {
        REQUIRE(read.result == VK_SUCCESS);
        for (unsigned i = 0; i < QUERY_COUNT; ++i) REQUIRE(read.values[i][1] && read.values[i][0] == expected_value(scene, i, 1));
    }
    signal_value(gate, 2);
    wait_fence(fence_a);
    wait_fence(fence_b);
    if (!wait_atomic(&read.done, 15000)) exit(1);
    join_thread(reading);
completed:
    check_archive(scene, 7, 1);
    check_archive(scene, 8, 3);
    check_host_values(scene, 3);
    vkDestroyFence(g_dev, fence_b, NULL);
    vkDestroyFence(g_dev, fence_a, NULL);
    vkDestroySemaphore(g_dev, prefix_done, NULL);
    vkDestroySemaphore(g_dev, gate, NULL);
    VkCommandBuffer commands[] = {archive_a, change_b, archive_b, tail};
    vkFreeCommandBuffers(g_dev, g_pool, ARRAY_SIZE(commands), commands);
    puts("query replay: overlapping submissions, independent execution copies, prefix GetResults before suffix wait passed");
}

int main(void) {
    const char *layers = getenv("VK_INSTANCE_LAYERS");
    if (!layers || !strstr(layers, "VK_LAYER_VKRT_SOFTWARE_RAY_QUERY")) {
        fputs("SKIP query replay exact property oracle requires the canonical software layer\n", stderr);
        return 77;
    }
    init_vulkan();
    build_as = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(g_dev, "vkCmdBuildAccelerationStructuresKHR");
    write_as = (PFN_vkCmdWriteAccelerationStructuresPropertiesKHR)vkGetInstanceProcAddr(
        g_instance, "vkCmdWriteAccelerationStructuresPropertiesKHR");
    reset_query = (PFN_vkCmdResetQueryPool)vkGetDeviceProcAddr(g_dev, "vkCmdResetQueryPool");
    copy_query = (PFN_vkCmdCopyQueryPoolResults)vkGetInstanceProcAddr(g_instance, "vkCmdCopyQueryPoolResults");
    get_query = (PFN_vkGetQueryPoolResults)vkGetDeviceProcAddr(g_dev, "vkGetQueryPoolResults");
    queue_submit = (PFN_vkQueueSubmit)vkGetDeviceProcAddr(g_dev, "vkQueueSubmit");
    signal_semaphore = (PFN_vkSignalSemaphore)vkGetDeviceProcAddr(g_dev, "vkSignalSemaphore");
    wait_semaphores = (PFN_vkWaitSemaphores)vkGetDeviceProcAddr(g_dev, "vkWaitSemaphores");
    REQUIRE(build_as && write_as && reset_query && copy_query && get_query && queue_submit && signal_semaphore && wait_semaphores);
    if (g_fail) return 1;
    struct scene scene = {0};
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    buf_create(9 * sizeof(float),
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               host, &scene.vertices);
    const float triangle[9] = {-1, -1, 0, 1, -1, 0, 0, 1, 0};
    memcpy(scene.vertices.mapped, triangle, sizeof(triangle));
    scene.triangle = (VkAccelerationStructureGeometryKHR){
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                               .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                               .vertexData.deviceAddress = scene.vertices.addr,
                               .vertexStride = 12,
                               .maxVertex = 2,
                               .indexType = VK_INDEX_TYPE_NONE_KHR}};
    create_object(&scene.blas, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &scene.triangle, 1);
    buf_create(INPUT_COUNT * sizeof(VkAccelerationStructureInstanceKHR),
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &scene.instances);
    scene.instance_geometry = (VkAccelerationStructureGeometryKHR){
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                               .data.deviceAddress = scene.instances.addr}};
    create_object(&scene.tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &scene.instance_geometry, INPUT_COUNT);
    REQUIRE(expected_value(&scene, 0, 1) != expected_value(&scene, 0, 3) &&
            expected_value(&scene, 0, 3) != expected_value(&scene, 0, 5));
    buf_create(EPOCH_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &scene.results);
    buf_create(ARCHIVE_SLOTS * EPOCH_BYTES + 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host, &scene.archive);
    memset(scene.archive.mapped, 0xa5, (size_t)scene.archive.size);
    buf_create(4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &scene.tail);
    PFN_vkCreateQueryPool create_query = (PFN_vkCreateQueryPool)vkGetDeviceProcAddr(g_dev, "vkCreateQueryPool");
    for (unsigned i = 0; i < QUERY_COUNT; ++i) {
        VkQueryPoolCreateInfo info = {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = query_types[i], .queryCount = 1};
        CHECK(create_query(g_dev, &info, NULL, &scene.pools[i]));
    }
    VkCommandBuffer setup = allocate_command(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    begin_command(setup, VK_COMMAND_BUFFER_LEVEL_PRIMARY, false);
    const VkAccelerationStructureBuildRangeInfoKHR *range = &scene.blas.range;
    build_as(setup, 1, &scene.blas.info, &range);
    barrier(setup);
    CHECK(vkEndCommandBuffer(setup));
    submit_and_wait(1, &setup);
    vkFreeCommandBuffers(g_dev, g_pool, 1, &setup);
    VkCommandBuffer query = record_queries(&scene, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    serial_reuse(&scene, query);
    repeated_primary(&scene, query);
    repeated_secondary(&scene);
    overlapping_submissions(&scene, query);
    for (unsigned i = ARCHIVE_SLOTS * EPOCH_BYTES; i < scene.archive.size; ++i)
        REQUIRE(((const uint8_t *)scene.archive.mapped)[i] == 0xa5);
    vkFreeCommandBuffers(g_dev, g_pool, 1, &query);
    check_host_values(&scene, 3); /* Public values survive source recording destruction. */
    for (unsigned i = 0; i < QUERY_COUNT; ++i) vkDestroyQueryPool(g_dev, scene.pools[i], NULL);
    buf_destroy(&scene.tail);
    buf_destroy(&scene.archive);
    buf_destroy(&scene.results);
    destroy_object(&scene.tlas);
    destroy_object(&scene.blas);
    buf_destroy(&scene.instances);
    buf_destroy(&scene.vertices);
    vkDestroyCommandPool(g_dev, g_pool, NULL);
    vkDestroyDevice(g_dev, NULL);
    PFN_vkDestroyDebugUtilsMessengerEXT destroy =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_instance, "vkDestroyDebugUtilsMessengerEXT");
    destroy(g_instance, messenger, NULL);
    vkDestroyInstance(g_instance, NULL);
    printf("Public query replay: %s (%d failures, %u validation errors)\n",
           g_fail || atomic_load(&validation_errors) ? "FAIL" : "PASS", g_fail, atomic_load(&validation_errors));
    return g_fail || atomic_load(&validation_errors) ? 1 : 0;
}
