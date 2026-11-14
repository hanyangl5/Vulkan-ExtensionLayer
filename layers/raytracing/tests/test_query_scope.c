/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Public native/software control for an occlusion query around independent AS
 * query operations, all outside a render pass. No drawing occurs: occlusion
 * must be zero. AS COPY is compared with an independent, completed host read on
 * the same implementation, without assuming identical native/software sizes.
 */
#include <stdbool.h>
#include <stdatomic.h>
#include <vulkan/vulkan.h>
static VkResult scope_create_instance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
static VkResult scope_create_device(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);
#define vkCreateInstance scope_create_instance
#define vkCreateDevice scope_create_device
#define VKRT_TEST_FIXTURE_ONLY
#include "gpu_harness.h"
#undef vkCreateInstance
#undef vkCreateDevice

enum { OUTPUT_BYTES = 80, OUTPUT_OFFSET = 16, OUTPUT_PAYLOAD = 16 };
static const uint64_t timeout_ns = UINT64_C(15000000000);
static atomic_uint validation_errors;
static VkDebugUtilsMessengerEXT messenger;
static PFN_vkCmdBuildAccelerationStructuresKHR build_as;
static PFN_vkCmdWriteAccelerationStructuresPropertiesKHR write_property;
static unsigned failures, accepted, rejected;
static bool expect_rejection;
#define REQUIRE(condition)                                          \
    do {                                                            \
        if (!(condition)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #condition); \
            ++failures;                                             \
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
static VkResult scope_create_instance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator, VkInstance *out) {
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
static VkResult scope_create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                    const VkAllocationCallbacks *allocator, VkDevice *out) {
    if (info->queueCreateInfoCount != 1) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    if (!families) return VK_ERROR_OUT_OF_HOST_MEMORY;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i)
        if (families[i].queueCount && (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                                          (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            family = i;
            break;
        }
    free(families);
    if (family == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkDeviceQueueCreateInfo queue = info->pQueueCreateInfos[0];
    queue.queueFamilyIndex = family;
    VkDeviceCreateInfo local = *info;
    local.pQueueCreateInfos = &queue;
    g_qfamily = family;
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
static void submit_complete(VkCommandBuffer command) {
    VkFenceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(g_dev, &info, NULL, &fence));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    CHECK(vkQueueSubmit(g_queue, 1, &submit, fence));
    CHECK(vkWaitForFences(g_dev, 1, &fence, VK_TRUE, timeout_ns));
    vkDestroyFence(g_dev, fence, NULL);
}
struct scope_scene {
    struct gpu_buf vertices, storage, scratch;
    VkAccelerationStructureKHR as;
};
static void create_scene(struct scope_scene *scene) {
    const float vertices[] = {-1, -1, 0, 1, -1, 0, 0, 1, 0};
    buf_create(sizeof(vertices),
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &scene->vertices);
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
    REQUIRE(get_sizes);
    if (!get_sizes) exit(1);
    get_sizes(g_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &count, &sizes);
    VkPhysicalDeviceAccelerationStructurePropertiesKHR properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 physical = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &properties};
    vkGetPhysicalDeviceProperties2(g_pdev, &physical);
    const uint64_t alignment = properties.minAccelerationStructureScratchOffsetAlignment;
    REQUIRE(alignment && !(alignment & (alignment - 1)));
    if (!alignment) exit(1);
    buf_create(sizes.accelerationStructureSize,
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &scene->storage);
    buf_create(sizes.buildScratchSize + alignment - 1,
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
               &scene->scratch);
    VkAccelerationStructureCreateInfoKHR create = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                   .buffer = scene->storage.buf,
                                                   .size = sizes.accelerationStructureSize,
                                                   .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR};
    CHECK(pCreateAS(g_dev, &create, NULL, &scene->as));
    build.dstAccelerationStructure = scene->as;
    build.scratchData.deviceAddress = (scene->scratch.addr + alignment - 1) & ~(alignment - 1);
    VkAccelerationStructureBuildRangeInfoKHR range = {.primitiveCount = 1};
    const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
    VkCommandBuffer command = begin_command();
    build_as(command, 1, &build, &ranges);
    VkMemoryBarrier ready = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                             .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                             .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &ready, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(command));
    submit_complete(command);
    vkFreeCommandBuffers(g_dev, g_pool, 1, &command);
}
static void query_scope(const struct scope_scene *scene, unsigned kind) {
    static const char *names[] = {"reset", "property", "copy", "reset-property-copy"};
    const VkQueryType type = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
    VkQueryPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = type, .queryCount = 1};
    VkQueryPool as_pool, occlusion;
    CHECK(vkCreateQueryPool(g_dev, &info, NULL, &as_pool));
    info.queryType = VK_QUERY_TYPE_OCCLUSION;
    CHECK(vkCreateQueryPool(g_dev, &info, NULL, &occlusion));
    struct gpu_buf output;
    buf_create(OUTPUT_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &output);
    VkCommandBuffer command = begin_command();
    vkCmdResetQueryPool(command, occlusion, 0, 1);
    /* Begin/EndQuery both permit outside-render-pass recording. These are
     * separate pools: the AS queries themselves are never active. */
    if (kind == 0 || kind == 3) vkCmdBeginQuery(command, occlusion, 0, 0);
    vkCmdResetQueryPool(command, as_pool, 0, 1);
    if (kind == 0) vkCmdEndQuery(command, occlusion, 0);
    if (kind == 1) vkCmdBeginQuery(command, occlusion, 0, 0);
    write_property(command, 1, &scene->as, type, as_pool, 0);
    if (kind == 1) vkCmdEndQuery(command, occlusion, 0);
    if (kind == 2) vkCmdBeginQuery(command, occlusion, 0, 0);
    vkCmdCopyQueryPoolResults(command, as_pool, 0, 1, output.buf, OUTPUT_OFFSET, 32,
                              VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (kind == 2 || kind == 3) vkCmdEndQuery(command, occlusion, 0);
    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                            .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    VkResult end = vkEndCommandBuffer(command);
    if (end == VK_ERROR_FEATURE_NOT_PRESENT && expect_rejection) {
        ++rejected;
        printf("query scope: %s expected old blanket-guard rejection\n", names[kind]);
    } else {
        CHECK(end);
        REQUIRE(!expect_rejection);
        for (unsigned occurrence = 0; occurrence < 2; ++occurrence) {
            memset(output.mapped, 0xcd, OUTPUT_BYTES);
            submit_complete(command);
            uint64_t native_result[2] = {UINT64_MAX, 0}, reference[2] = {0, 0}, copied[2] = {0, 0};
            const VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
            CHECK(
                vkGetQueryPoolResults(g_dev, occlusion, 0, 1, sizeof(native_result), native_result, sizeof(native_result), flags));
            CHECK(vkGetQueryPoolResults(g_dev, as_pool, 0, 1, sizeof(reference), reference, sizeof(reference), flags));
            memcpy(copied, (const uint8_t *)output.mapped + OUTPUT_OFFSET, sizeof(copied));
            REQUIRE(native_result[0] == 0 && native_result[1] != 0);
            REQUIRE(reference[0] != 0 && reference[1] != 0 && copied[0] == reference[0] && copied[1] != 0);
            for (unsigned i = 0; i < OUTPUT_BYTES; ++i)
                if (i < OUTPUT_OFFSET || i >= OUTPUT_OFFSET + OUTPUT_PAYLOAD) REQUIRE(((const uint8_t *)output.mapped)[i] == 0xcd);
            ++accepted;
            printf("query scope: %s occurrence %u O=0, AS value/availability/padding PASS\n", names[kind], occurrence);
        }
    }
    vkFreeCommandBuffers(g_dev, g_pool, 1, &command);
    vkDestroyQueryPool(g_dev, as_pool, NULL);
    vkDestroyQueryPool(g_dev, occlusion, NULL);
    buf_destroy(&output);
}
int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--expect-rejection"))
            expect_rejection = true;
        else {
            fprintf(stderr, "usage: %s [--expect-rejection]\n", argv[0]);
            return 2;
        }
    init_vulkan();
    build_as = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetDeviceProcAddr(g_dev, "vkCmdBuildAccelerationStructuresKHR");
    write_property = (PFN_vkCmdWriteAccelerationStructuresPropertiesKHR)vkGetDeviceProcAddr(
        g_dev, "vkCmdWriteAccelerationStructuresPropertiesKHR");
    REQUIRE(build_as && write_property);
    if (!build_as || !write_property) return 1;
    struct scope_scene scene = {0};
    create_scene(&scene);
    for (unsigned kind = 0; kind < 4; ++kind) query_scope(&scene, kind);
    pDestroyAS(g_dev, scene.as, NULL);
    buf_destroy(&scene.scratch);
    buf_destroy(&scene.storage);
    buf_destroy(&scene.vertices);
    vkDestroyCommandPool(g_dev, g_pool, NULL);
    vkDestroyDevice(g_dev, NULL);
    PFN_vkDestroyDebugUtilsMessengerEXT destroy =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_instance, "vkDestroyDebugUtilsMessengerEXT");
    if (messenger && destroy) destroy(g_instance, messenger, NULL);
    vkDestroyInstance(g_instance, NULL);
    REQUIRE(validation_errors == 0);
    REQUIRE(expect_rejection ? rejected == 4 && accepted == 0 : accepted == 8 && rejected == 0);
    printf("query scope: %u accepted occurrences, %u expected rejections, %u validation errors, %u failures\n", accepted, rejected,
           (unsigned)validation_errors, failures);
    return failures ? 1 : 0;
}
