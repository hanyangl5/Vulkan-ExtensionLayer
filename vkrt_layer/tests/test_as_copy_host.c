/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_as_copy.h"

struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
#define H(type, value) ((type)(uintptr_t)(value))
#define CHECK(x)                                            \
    do {                                                    \
        if (!(x)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); \
            ++failures;                                     \
        }                                                   \
    } while (0)
static unsigned failures, barriers, binds, pushes, dispatches, addresses;
static const VkDevice device = H(VkDevice, 1);
static const VkCommandBuffer command = H(VkCommandBuffer, 2);
static const VkAccelerationStructureKHR src_handle = H(VkAccelerationStructureKHR, 3),
                                        dst_handle = H(VkAccelerationStructureKHR, 4);
static VkDeviceAddress source_address = UINT64_C(0x100000000010), destination_address = UINT64_C(0x200000000010);
static struct vkrt_as_copy_args observed;

VkDeviceAddress vkrt_as_backing_address(struct vkrt_device_data *dev, VkAccelerationStructureKHR handle) {
    CHECK(dev->device == device && (handle == src_handle || handle == dst_handle));
    ++addresses;
    return handle == src_handle ? source_address : destination_address;
}
static VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                          VkDependencyFlags flags, uint32_t nm, const VkMemoryBarrier *memory, uint32_t nb,
                                          const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im) {
    CHECK(cmd == command && src == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && dst == src && !flags && nm == 1 && !nb && !ni);
    CHECK(memory->srcAccessMask == VK_ACCESS_MEMORY_WRITE_BIT &&
          memory->dstAccessMask == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT));
    ++barriers;
}
static VKAPI_ATTR void VKAPI_CALL bind(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline) {
    CHECK(cmd == command && point == VK_PIPELINE_BIND_POINT_COMPUTE && pipeline == H(VkPipeline, binds ? 33 : 22));
    ++binds;
}
static VKAPI_ATTR void VKAPI_CALL push(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset,
                                       uint32_t size, const void *values) {
    CHECK(cmd == command);
    if (!pushes) {
        CHECK(layout == H(VkPipelineLayout, 21) && stages == VK_SHADER_STAGE_COMPUTE_BIT && !offset && size == 40);
        memcpy(&observed, values, sizeof(observed));
    } else {
        CHECK(layout == H(VkPipelineLayout, pushes == 1 ? 51 : 52));
        CHECK(stages ==
              (pushes == 1 ? (VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT) : VK_SHADER_STAGE_FRAGMENT_BIT));
        CHECK(offset == (pushes == 1 ? 0u : 16u) && size == 4 && *(const uint32_t *)values == (pushes == 1 ? 71u : 72u));
    }
    ++pushes;
}
static VKAPI_ATTR void VKAPI_CALL dispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z) {
    CHECK(cmd == command && x == 3 && y == 1 && z == 1 && addresses == 2 && barriers == 1 && binds == 1 && pushes == 1);
    ++dispatches;
}
static struct vkrt_device_data dev = {
    .device = H(VkDevice, 1),
    .copy_pipeline = H(VkPipeline, 22),
    .copy_pipeline_layout = H(VkPipelineLayout, 21),
    .max_build_dispatch_x = 3,
    .vtable = {.CmdBindPipeline = bind, .CmdPushConstants = push, .CmdDispatch = dispatch, .CmdPipelineBarrier = barrier}};
static struct vkrt_virtual_as source = {
    .owner = H(VkDevice, 1), .size = UINT64_C(1) << 40, .type = VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR};
static struct vkrt_virtual_as destination = {
    .owner = H(VkDevice, 1), .size = UINT64_C(1) << 39, .type = VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR};
static struct vkrt_cmd_buffer_data cb = {
    .cmd = H(VkCommandBuffer, 2), .device_data = &dev, .bound_compute_pipeline = H(VkPipeline, 33)};
struct saved_push {
    struct vkrt_saved_push *next;
    VkPipelineLayout layout;
    VkShaderStageFlags stages;
    uint32_t offset, size, bytes[1];
};
static struct saved_push second = {
    .layout = H(VkPipelineLayout, 52), .stages = VK_SHADER_STAGE_FRAGMENT_BIT, .offset = 16, .size = 4, .bytes = {72}};
static struct saved_push first = {.next = (struct vkrt_saved_push *)&second,
                                  .layout = H(VkPipelineLayout, 51),
                                  .stages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                  .size = 4,
                                  .bytes = {71}};
static void reset(void) {
    barriers = binds = pushes = dispatches = addresses = 0;
    cb.record_result = VK_SUCCESS;
}
static void no_commands(VkResult expected) { CHECK(cb.record_result == expected && !barriers && !binds && !pushes && !dispatches); }

static unsigned init_failure, layouts_destroyed, modules_destroyed, pipelines_destroyed;
static VKAPI_ATTR VkResult VKAPI_CALL create_layout(VkDevice d, const VkPipelineLayoutCreateInfo *info,
                                                    const VkAllocationCallbacks *allocator, VkPipelineLayout *out) {
    CHECK(d == device && !allocator && info->pushConstantRangeCount == 1 && !info->setLayoutCount);
    CHECK(info->pPushConstantRanges[0].stageFlags == VK_SHADER_STAGE_COMPUTE_BIT &&
          info->pPushConstantRanges[0].size == sizeof(struct vkrt_as_copy_args));
    if (init_failure == 1) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = H(VkPipelineLayout, 21);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_layout(VkDevice d, VkPipelineLayout layout, const VkAllocationCallbacks *allocator) {
    CHECK(d == device && layout == H(VkPipelineLayout, 21) && !allocator);
    ++layouts_destroyed;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_module(VkDevice d, const VkShaderModuleCreateInfo *info,
                                                    const VkAllocationCallbacks *allocator, VkShaderModule *out) {
    CHECK(d == device && !allocator && info->codeSize > 20 && info->pCode[0] == 0x07230203u);
    if (init_failure == 2) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = H(VkShaderModule, 23);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_module(VkDevice d, VkShaderModule module, const VkAllocationCallbacks *allocator) {
    CHECK(d == device && module == H(VkShaderModule, 23) && !allocator);
    ++modules_destroyed;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_pipeline(VkDevice d, VkPipelineCache cache, uint32_t count,
                                                      const VkComputePipelineCreateInfo *info,
                                                      const VkAllocationCallbacks *allocator, VkPipeline *out) {
    CHECK(d == device && !cache && count == 1 && !allocator && info->layout == H(VkPipelineLayout, 21));
    CHECK(info->stage.module == H(VkShaderModule, 23) && info->stage.stage == VK_SHADER_STAGE_COMPUTE_BIT);
    *out = init_failure == 3 ? VK_NULL_HANDLE : H(VkPipeline, 22);
    return init_failure == 3 ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_pipeline(VkDevice d, VkPipeline pipeline, const VkAllocationCallbacks *allocator) {
    CHECK(d == device && pipeline == H(VkPipeline, 22) && !allocator);
    ++pipelines_destroyed;
}
static void check_init_cleanup(void) {
    for (init_failure = 0; init_failure < 4; ++init_failure) {
        struct vkrt_device_data candidate = {.device = H(VkDevice, 1),
                                             .vtable = {.CreatePipelineLayout = create_layout,
                                                        .DestroyPipelineLayout = destroy_layout,
                                                        .CreateShaderModule = create_module,
                                                        .DestroyShaderModule = destroy_module,
                                                        .CreateComputePipelines = create_pipeline,
                                                        .DestroyPipeline = destroy_pipeline}};
        layouts_destroyed = modules_destroyed = pipelines_destroyed = 0;
        VkResult result = vkrt_as_copy_init(&candidate);
        CHECK(result == (init_failure == 1 ? VK_ERROR_OUT_OF_HOST_MEMORY
                         : init_failure    ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                                           : VK_SUCCESS));
        if (!init_failure) {
            CHECK(candidate.copy_pipeline == H(VkPipeline, 22) && candidate.copy_pipeline_layout == H(VkPipelineLayout, 21));
            vkrt_as_copy_finish(&candidate);
        }
        CHECK(!candidate.copy_pipeline && !candidate.copy_pipeline_layout);
        CHECK(layouts_destroyed == (init_failure != 1) && modules_destroyed == (init_failure == 0 || init_failure == 3) &&
              pipelines_destroyed == (init_failure == 0));
        vkrt_as_copy_finish(&candidate); /* Successful and partial cleanup are idempotent. */
        CHECK(layouts_destroyed == (init_failure != 1));
    }
}

int main(void) {
    check_init_cleanup();
    vkrt_map_object(VKRT_HKEY(command), &cb);
    vkrt_map_object(VKRT_HKEY(src_handle), &source);
    vkrt_map_object(VKRT_HKEY(dst_handle), &destination);
    cb.push_head = (struct vkrt_saved_push *)&first;
    cb.push_tail = (struct vkrt_saved_push *)&second;
    VkCopyAccelerationStructureInfoKHR info = {.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
                                               .src = src_handle,
                                               .dst = dst_handle,
                                               .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR};
    for (unsigned compact = 0; compact < 2; ++compact) {
        reset();
        info.mode = compact ? VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR : VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR;
        vkrt_cmd_copy_as(&dev, command, &info);
        CHECK(cb.record_result == VK_SUCCESS && barriers == 2 && binds == 2 && pushes == 3 && dispatches == 1);
        CHECK(observed.source == source_address && observed.destination == destination_address);
        CHECK(observed.source_capacity == source.size && observed.destination_capacity == destination.size &&
              observed.destination_type == 2 && !observed.reserved);
    }
    reset();
    info.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_SERIALIZE_KHR;
    vkrt_cmd_copy_as(&dev, command, &info);
    no_commands(VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(!addresses);
    info.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR;
    reset();
    destination.owner = H(VkDevice, 99);
    vkrt_cmd_copy_as(&dev, command, &info);
    no_commands(VK_ERROR_INITIALIZATION_FAILED);
    CHECK(!addresses);
    destination.owner = device;
    reset();
    destination.size = 127;
    vkrt_cmd_copy_as(&dev, command, &info);
    no_commands(VK_ERROR_OUT_OF_DEVICE_MEMORY);
    CHECK(!addresses);
    destination.size = UINT64_C(1) << 39;
    reset();
    dev.vtable.CmdDispatch = NULL;
    vkrt_cmd_copy_as(&dev, command, &info);
    no_commands(VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(!addresses);
    dev.vtable.CmdDispatch = dispatch;
    reset();
    destination_address = source_address;
    vkrt_cmd_copy_as(&dev, command, &info);
    no_commands(VK_ERROR_INITIALIZATION_FAILED);
    destination_address = UINT64_C(0x200000000010);
    reset();
    source_address = UINT64_MAX - 16;
    vkrt_cmd_copy_as(&dev, command, &info);
    no_commands(VK_ERROR_INITIALIZATION_FAILED);
    source_address = UINT64_MAX - 15;
    reset();
    vkrt_cmd_copy_as(&dev, command, &info);
    no_commands(VK_ERROR_OUT_OF_DEVICE_MEMORY);
    source_address = UINT64_C(0x100000000010);
    reset();
    cb.record_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    vkrt_cmd_copy_as(&dev, command, &info);
    no_commands(VK_ERROR_OUT_OF_HOST_MEMORY);
    CHECK(!addresses);
    vkrt_unmap_object(VKRT_HKEY(command));
    vkrt_unmap_object(VKRT_HKEY(src_handle));
    vkrt_unmap_object(VKRT_HKEY(dst_handle));
    vkrt_map_destroy(&vkrt_global.objects);
    printf("AS copy command boundary: %u failures\n", failures);
    return failures != 0;
}
