/* SPDX-License-Identifier: MIT
 * Safe GPU-only AS property/formatting component. Public query-pool host
 * publication is deliberately not represented by a coherent-memory poll.
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdbool.h>
#include "bvh4_abi.h"
#include "vkrt_queries.h"
#include "vkrt_as_serialization.h"
static VkResult component_create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                        const VkAllocationCallbacks *allocator, VkDevice *device);
#define vkCreateDevice component_create_device
#define VKRT_TEST_FIXTURE_ONLY
#include "00_single_tri/main.c"
#undef vkCreateDevice

static VkResult component_create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                        const VkAllocationCallbacks *allocator, VkDevice *device) {
    /* These component shaders are application-visible native compute modules,
     * so their Int64 requirement must be visible to validation ABOVE the
     * emulation layer as well as privately enabled in the GPU backend. */
    VkDeviceCreateInfo local = *info;
    VkPhysicalDeviceFeatures features = info->pEnabledFeatures ? *info->pEnabledFeatures : (VkPhysicalDeviceFeatures){0};
    features.shaderInt64 = VK_TRUE;
    local.pEnabledFeatures = &features;
    return vkCreateDevice(physical, &local, allocator, device);
}

_Static_assert(sizeof(struct vkrt_as_query_args) == 32, "GPU property push ABI");
_Static_assert(sizeof(struct vkrt_as_query_value) == 16, "GPU property slot ABI");
_Static_assert(sizeof(struct vkrt_query_format_args) == 56, "GPU formatter push ABI");

#define REQUIRE(condition)                                          \
    do {                                                            \
        if (!(condition)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #condition); \
            ++g_fail;                                               \
        }                                                           \
    } while (0)
enum { SLOT_COUNT = 12, FORMAT_COUNT = 12, FORMAT_BYTES = 1024 };
static const uint64_t high_value = UINT64_C(0xfedcba9876543210);
static PFN_vkCmdBuildAccelerationStructuresKHR build;

struct built {
    struct gpu_buf output, scratch;
    VkAccelerationStructureKHR handle;
    VkDeviceAddress public_address;
    VkAccelerationStructureBuildGeometryInfoKHR info;
    VkAccelerationStructureBuildRangeInfoKHR range;
    VkDeviceSize required, capacity;
};

static void prepare(struct built *out, VkAccelerationStructureTypeKHR type, const VkAccelerationStructureGeometryKHR *geometry,
                    uint32_t count) {
    memset(out, 0, sizeof(*out));
    out->info =
        (VkAccelerationStructureBuildGeometryInfoKHR){.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                                                      .type = type,
                                                      .flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR,
                                                      .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                                                      .geometryCount = 1,
                                                      .pGeometries = geometry};
    out->range.primitiveCount = count;
    PFN_vkGetAccelerationStructureBuildSizesKHR sizes =
        (void *)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureBuildSizesKHR");
    VkAccelerationStructureBuildSizesInfoKHR size = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    sizes(g_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &out->info, &count, &size);
    out->required = size.accelerationStructureSize;
    out->capacity = out->required + 4096;
    VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    buf_create(out->capacity + 512,
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, host,
               &out->output);
    memset(out->output.mapped, 0xa5, (size_t)out->output.size);
    VkAccelerationStructureCreateInfoKHR create = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                   .buffer = out->output.buf,
                                                   .offset = 256,
                                                   .size = out->capacity,
                                                   .type = type};
    CHECK(pCreateAS(g_dev, &create, NULL, &out->handle));
    VkAccelerationStructureDeviceAddressInfoKHR address = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, .accelerationStructure = out->handle};
    PFN_vkGetAccelerationStructureDeviceAddressKHR get =
        (void *)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureDeviceAddressKHR");
    out->public_address = get(g_dev, &address);
    buf_create(size.buildScratchSize + 512, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, host,
               &out->scratch);
    out->info.scratchData.deviceAddress = (out->scratch.addr + 255) & ~UINT64_C(255);
    out->info.dstAccelerationStructure = out->handle;
}

static void destroy_built(struct built *object) {
    pDestroyAS(g_dev, object->handle, NULL);
    buf_destroy(&object->scratch);
    buf_destroy(&object->output);
}

static void barrier(VkCommandBuffer command, VkPipelineStageFlags source, VkAccessFlags source_access,
                    VkPipelineStageFlags destination, VkAccessFlags destination_access) {
    VkMemoryBarrier memory = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = source_access, .dstAccessMask = destination_access};
    vkCmdPipelineBarrier(command, source, destination, 0, 1, &memory, 0, NULL, 0, NULL);
}

static VkPipeline pipeline(const char *path, VkPipelineLayout layout) {
    size_t count;
    uint32_t *words = read_spv(path, &count);
    VkShaderModuleCreateInfo module_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = count * 4, .pCode = words};
    VkShaderModule module;
    CHECK(vkCreateShaderModule(g_dev, &module_info, NULL, &module));
    free(words);
    VkComputePipelineCreateInfo create = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                    .module = module,
                                                    .pName = "main"},
                                          .layout = layout};
    VkPipeline result;
    CHECK(vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &create, NULL, &result));
    vkDestroyShaderModule(g_dev, module, NULL);
    return result;
}

static void query(VkCommandBuffer command, VkPipelineLayout layout, const struct built *object, VkQueryType type,
                  const struct gpu_buf *slots, uint32_t slot, bool short_capacity, bool null_source) {
    struct vkrt_as_query_args args = {null_source ? 0 : object->output.addr + 256, short_capacity ? 127 : object->capacity,
                                      slots->addr + slot * 16, type, 0};
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(args), &args);
    vkCmdDispatch(command, 1, 1, 1);
}

static struct vkrt_query_format_args format_args(unsigned variant, const struct gpu_buf *slots, const struct gpu_buf *formatted) {
    struct vkrt_query_format_args args = {slots->addr,
                                          slots->size,
                                          formatted->addr + variant * FORMAT_BYTES,
                                          FORMAT_BYTES,
                                          24,
                                          0,
                                          7,
                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_WAIT_BIT,
                                          0};
    switch (variant) {
        case 0:
            break;
        case 1:
            args.flags = VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_PARTIAL_BIT;
            args.count = 9;
            args.stride = 16;
            break;
        case 2:
            args.flags = VK_QUERY_RESULT_64_BIT;
            args.first_query = 5;
            args.count = 4;
            args.stride = 16;
            break;
        case 3:
            args.flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT | VK_QUERY_RESULT_PARTIAL_BIT;
            args.first_query = 7;
            args.count = 2;
            break;
        case 4:
            args.flags = 0;
            args.first_query = 8;
            args.count = 1;
            args.stride = 0;
            break;
        case 5:
            args.flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
            args.count = 1;
            args.stride = 0;
            break;
        case 6:
            args.output_capacity = 7;
            args.count = 2;
            break;
        case 7:
            /* Aligned, so this reaches the multiplication-overflow guard
             * rather than being rejected by the alignment check first. */
            args.stride = UINT64_MAX - 7;
            args.count = 3;
            break;
        case 8:
            args.first_query = UINT32_MAX;
            args.count = 2;
            break;
        case 9:
            args.output += 4;
            break;
        case 10:
            args.stride = 8;
            args.count = 2;
            break;
        case 11:
            args.count = 0;
            break;
    }
    return args;
}

static void verify(const struct built *blas, const struct built *tlas, unsigned execution, const struct gpu_buf *slots,
                   const struct gpu_buf *formatted, const struct gpu_buf *destination) {
    VelRtBvh4Header b, t;
    memcpy(&b, (uint8_t *)blas->output.mapped + 256, sizeof(b));
    memcpy(&t, (uint8_t *)tlas->output.mapped + 256, sizeof(t));
    REQUIRE(b.magic == VEL_RT_BVH4_MAGIC && b.version == VEL_RT_BVH4_ABI_VERSION && b.flags == 0);
    REQUIRE(t.magic == VEL_RT_BVH4_MAGIC && t.version == VEL_RT_BVH4_ABI_VERSION && t.flags == 0);
    REQUIRE(b.total_size == blas->required && b.total_size < blas->capacity && b.triangle_count == 2);
    REQUIRE(t.total_size == tlas->required && t.total_size < tlas->capacity && t.instance_count == 2 - execution);
    uint64_t values[9] = {b.total_size,
                          vkrt_as_serialization_size(b.total_size, 0),
                          b.total_size,
                          t.total_size,
                          vkrt_as_serialization_size(t.total_size, t.instance_count),
                          t.instance_count,
                          t.total_size,
                          0,
                          high_value};
    const struct vkrt_as_query_value *gpu_values = slots->mapped;
    for (unsigned i = 0; i < 7; ++i) REQUIRE(gpu_values[i].valid == 1 && gpu_values[i].value == values[i]);
    REQUIRE(!gpu_values[7].valid && gpu_values[8].valid == 1 && gpu_values[8].value == high_value);
    REQUIRE(!gpu_values[9].valid && !gpu_values[10].valid && !gpu_values[11].valid);
    for (unsigned variant = 0; variant < FORMAT_COUNT; ++variant) {
        uint8_t expected[FORMAT_BYTES];
        memset(expected, 0xa5, sizeof(expected));
        struct vkrt_query_format_args args = format_args(variant, slots, formatted);
        if (variant < 6) {
            unsigned width = args.flags & VK_QUERY_RESULT_64_BIT ? 8 : 4;
            for (uint32_t q = 0; q < args.count; ++q) {
                uint32_t index = args.first_query + q;
                bool available = index != 7;
                uint64_t value = available ? values[index] : 0;
                if (available || (args.flags & VK_QUERY_RESULT_PARTIAL_BIT)) memcpy(expected + q * args.stride, &value, width);
                if (args.flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
                    uint64_t availability = available;
                    memcpy(expected + q * args.stride + width, &availability, width);
                }
            }
        }
        const uint8_t *actual = (const uint8_t *)destination->mapped + variant * FORMAT_BYTES;
        if (memcmp(actual, expected, sizeof(expected))) {
            fprintf(stderr, "FAIL GPU formatter variant%u execution%u\n", variant, execution);
            ++g_fail;
        }
    }
    for (unsigned n = 0; n < 256; ++n) {
        REQUIRE(((const uint8_t *)destination->mapped)[FORMAT_COUNT * FORMAT_BYTES + n] == 0xa5);
        REQUIRE(((const uint8_t *)blas->output.mapped)[n] == 0xa5);
        REQUIRE(((const uint8_t *)tlas->output.mapped)[n] == 0xa5);
        REQUIRE(((const uint8_t *)blas->output.mapped)[256 + blas->capacity + n] == 0xa5);
        REQUIRE(((const uint8_t *)tlas->output.mapped)[256 + tlas->capacity + n] == 0xa5);
    }
    printf("GPU-built BVH4 property values +12 formatter layouts +native transfer copy, resubmit%u: %s\n", execution,
           g_fail ? "FAIL" : "PASS");
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s property.spv format.spv\n", argv[0]);
        return 1;
    }
    const char *layers = getenv("VK_INSTANCE_LAYERS");
    if (!layers || !strstr(layers, "VK_LAYER_VKRT_SOFTWARE_RAY_QUERY")) {
        fprintf(stderr, "This component requires the canonical software BVH4 producer layer\n");
        return 1;
    }
    init_vulkan();
    build = (void *)vkGetDeviceProcAddr(g_dev, "vkCmdBuildAccelerationStructuresKHR");
    REQUIRE(build != NULL);
    if (!build) return 1;
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    struct gpu_buf vertices, instances, slots, formatted, destination;
    buf_create(9 * 3 * sizeof(float),
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               host, &vertices);
    float triangle[27] = {-1, -1, 0, 1, -1, 0, 0, 1, 0, -1, -1, 1, 1, -1, 1, 0, 1, 1, NAN, 0, 0, 0, 0, 0, 0, 0, 0};
    memcpy(vertices.mapped, triangle, sizeof(triangle));
    VkAccelerationStructureGeometryKHR geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                               .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                               .vertexData.deviceAddress = vertices.addr,
                               .vertexStride = 12,
                               .maxVertex = 8,
                               .indexType = VK_INDEX_TYPE_NONE_KHR}};
    struct built blas, tlas;
    prepare(&blas, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &geometry, 3);
    buf_create(3 * sizeof(VkAccelerationStructureInstanceKHR),
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               host, &instances);
    VkAccelerationStructureInstanceKHR *instance = instances.mapped;
    for (unsigned i = 0; i < 3; ++i)
        instance[i] = (VkAccelerationStructureInstanceKHR){.transform = {.matrix = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}},
                                                           .mask = i == 1 ? 0 : 0xff,
                                                           .accelerationStructureReference = blas.public_address};
    VkAccelerationStructureGeometryKHR instance_geometry = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                               .data.deviceAddress = instances.addr}};
    prepare(&tlas, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &instance_geometry, 3);
    buf_create(SLOT_COUNT * 16,
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               host, &slots);
    buf_create(FORMAT_COUNT * FORMAT_BYTES + 256,
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               host, &formatted);
    buf_create(formatted.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host, &destination);
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(g_dev, &layout_info, NULL, &layout));
    VkPipeline property = pipeline(argv[1], layout), formatter = pipeline(argv[2], layout);
    VkCommandBufferAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                              .commandPool = g_pool,
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                              .commandBufferCount = 1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(g_dev, &allocation, &command));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(command, &begin));
    barrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    const VkAccelerationStructureBuildRangeInfoKHR *range = &blas.range;
    build(command, 1, &blas.info, &range);
    barrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    range = &tlas.range;
    build(command, 1, &tlas.info, &range);
    barrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    vkCmdFillBuffer(command, slots.buf, 0, slots.size, 0);
    vkCmdFillBuffer(command, formatted.buf, 0, formatted.size, 0xa5a5a5a5);
    barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    const struct vkrt_as_query_value high = {UINT64_C(0xfedcba9876543210), 1, 0};
    vkCmdUpdateBuffer(command, slots.buf, 8 * 16, sizeof(high), &high);
    barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, property);
    query(command, layout, &blas, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, &slots, 0, false, false);
    query(command, layout, &blas, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE_KHR, &slots, 1, false, false);
    query(command, layout, &blas, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SIZE_KHR, &slots, 2, false, false);
    query(command, layout, &tlas, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, &slots, 3, false, false);
    query(command, layout, &tlas, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_SIZE_KHR, &slots, 4, false, false);
    query(command, layout, &tlas, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_BOTTOM_LEVEL_POINTERS_KHR, &slots, 5, false,
          false);
    query(command, layout, &tlas, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SIZE_KHR, &slots, 6, false, false);
    query(command, layout, &blas, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, &slots, 9, true, false);
    query(command, layout, &blas, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, &slots, 10, false, true);
    query(command, layout, &blas, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_SERIALIZATION_BOTTOM_LEVEL_POINTERS_KHR, &slots, 11, false,
          false);
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, formatter);
    for (unsigned variant = 0; variant < FORMAT_COUNT; ++variant) {
        struct vkrt_query_format_args args = format_args(variant, &slots, &formatted);
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(args), &args);
        vkCmdDispatch(command, 1, 1, 1);
    }
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy region = {0, 0, formatted.size};
    vkCmdCopyBuffer(command, formatted.buf, destination.buf, 1, &region);
    barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    CHECK(vkEndCommandBuffer(command));
    for (unsigned execution = 0; execution < 3; ++execution) {
        if (execution == 1) instance[2].mask = 0;
        if (execution == 2) instance[0].mask = 0;
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
        CHECK(vkQueueSubmit(g_queue, 1, &submit, VK_NULL_HANDLE));
        CHECK(vkQueueWaitIdle(g_queue));
        verify(&blas, &tlas, execution, &slots, &formatted, &destination);
    }
    vkFreeCommandBuffers(g_dev, g_pool, 1, &command);
    vkDestroyPipeline(g_dev, property, NULL);
    vkDestroyPipeline(g_dev, formatter, NULL);
    vkDestroyPipelineLayout(g_dev, layout, NULL);
    buf_destroy(&destination);
    buf_destroy(&formatted);
    buf_destroy(&slots);
    destroy_built(&tlas);
    destroy_built(&blas);
    buf_destroy(&instances);
    buf_destroy(&vertices);
    vkDestroyCommandPool(g_dev, g_pool, NULL);
    vkDestroyDevice(g_dev, NULL);
    vkDestroyInstance(g_instance, NULL);
    return g_fail ? 1 : 0;
}
