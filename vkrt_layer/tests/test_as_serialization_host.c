/* SPDX-License-Identifier: MIT */
#include "vkrt_layer.h"
#include "vkrt_as_serialization.h"

struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
#define H(type, n) ((type)(uintptr_t)(n))
#define EXPECT(x)                                           \
    do {                                                    \
        if (!(x)) {                                         \
            fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); \
            ++failures;                                     \
        }                                                   \
    } while (0)
static unsigned failures, fail_step, allocations, frees, pipeline_calls, destroyed_pipelines, destroyed_layouts, destroyed_modules;
static unsigned barriers, binds, pushes, dispatches, publications, expected_operation;
static VkDeviceAddress physical = UINT64_C(0x123400000010);
static struct vkrt_serialization_args observed;
static VKAPI_ATTR void *VKAPI_CALL allocate(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    EXPECT(user == H(void *, 9) && alignment >= 8 && scope == VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
    ++allocations;
    return fail_step == 1 ? NULL : malloc(size);
}
static VKAPI_ATTR void VKAPI_CALL deallocate(void *user, void *ptr) {
    EXPECT(user == H(void *, 9) && ptr);
    ++frees;
    free(ptr);
}
static const VkAllocationCallbacks allocator = {.pUserData = H(void *, 9), .pfnAllocation = allocate, .pfnFree = deallocate};
VkDeviceAddress vkrt_as_registry_address(struct vkrt_device_data *dev) {
    EXPECT(dev->device == H(VkDevice, 1));
    return UINT64_C(0x456700001000);
}
VkDeviceAddress vkrt_as_backing_address(struct vkrt_device_data *dev, VkAccelerationStructureKHR as) {
    EXPECT(dev->device == H(VkDevice, 1) && as == H(VkAccelerationStructureKHR, 3));
    ++publications;
    return physical;
}
static VKAPI_ATTR void VKAPI_CALL properties(VkPhysicalDevice device, VkPhysicalDeviceProperties2 *out) {
    EXPECT(device == H(VkPhysicalDevice, 4));
    VkPhysicalDeviceIDProperties *ids = out->pNext;
    EXPECT(ids->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES);
    for (unsigned i = 0; i < 16; ++i) {
        ids->driverUUID[i] = (uint8_t)(i + 1);
        ids->deviceUUID[i] = (uint8_t)(0x80 + i);
    }
}
static VKAPI_ATTR VkResult VKAPI_CALL create_layout(VkDevice d, const VkPipelineLayoutCreateInfo *info,
                                                    const VkAllocationCallbacks *a, VkPipelineLayout *out) {
    EXPECT(d == H(VkDevice, 1) && a && a->pUserData == allocator.pUserData && !info->setLayoutCount &&
           info->pushConstantRangeCount == 1);
    EXPECT(info->pPushConstantRanges[0].size == 64 && info->pPushConstantRanges[0].stageFlags == VK_SHADER_STAGE_COMPUTE_BIT);
    if (fail_step == 2) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = H(VkPipelineLayout, 20);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_layout(VkDevice d, VkPipelineLayout layout, const VkAllocationCallbacks *a) {
    EXPECT(d == H(VkDevice, 1) && layout == H(VkPipelineLayout, 20) && a && a->pUserData == allocator.pUserData);
    ++destroyed_layouts;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_module(VkDevice d, const VkShaderModuleCreateInfo *info,
                                                    const VkAllocationCallbacks *a, VkShaderModule *out) {
    EXPECT(d == H(VkDevice, 1) && a && info->pCode[0] == 0x07230203u);
    if (fail_step == 3) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *out = H(VkShaderModule, 21);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_module(VkDevice d, VkShaderModule module, const VkAllocationCallbacks *a) {
    EXPECT(d == H(VkDevice, 1) && module == H(VkShaderModule, 21) && a);
    ++destroyed_modules;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_pipeline(VkDevice d, VkPipelineCache cache, uint32_t count,
                                                      const VkComputePipelineCreateInfo *info, const VkAllocationCallbacks *a,
                                                      VkPipeline *out) {
    EXPECT(d == H(VkDevice, 1) && !cache && count == 1 && info->layout == H(VkPipelineLayout, 20) && a);
    const VkSpecializationInfo *spec = info->stage.pSpecializationInfo;
    uint32_t phase;
    uint64_t context;
    memcpy(&phase, spec->pData, 4);
    memcpy(&context, (const uint8_t *)spec->pData + 8, 8);
    EXPECT(phase == pipeline_calls && context == UINT64_C(0x456700001000) && spec->mapEntryCount == 2);
    unsigned n = pipeline_calls++;
    if ((fail_step == 4 && n == 0) || (fail_step == 5 && n == 1)) {
        *out = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    *out = H(VkPipeline, 30 + n);
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_pipeline(VkDevice d, VkPipeline p, const VkAllocationCallbacks *a) {
    EXPECT(d == H(VkDevice, 1) && (p == H(VkPipeline, 30) || p == H(VkPipeline, 31)) && a);
    ++destroyed_pipelines;
}
static VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                          VkDependencyFlags dep, uint32_t count, const VkMemoryBarrier *memory, uint32_t buffers,
                                          const VkBufferMemoryBarrier *b, uint32_t images, const VkImageMemoryBarrier *im) {
    EXPECT(cb == H(VkCommandBuffer, 2) && src == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && src == dst && !dep && count == 1 &&
           !buffers && !images);
    EXPECT(memory->srcAccessMask == VK_ACCESS_MEMORY_WRITE_BIT &&
           memory->dstAccessMask == (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT));
    ++barriers;
}
static VKAPI_ATTR void VKAPI_CALL bind(VkCommandBuffer cb, VkPipelineBindPoint point, VkPipeline pipeline) {
    EXPECT(cb == H(VkCommandBuffer, 2) && point == VK_PIPELINE_BIND_POINT_COMPUTE &&
           pipeline == H(VkPipeline, binds ? 90 : 30 + expected_operation));
    ++binds;
}
static VKAPI_ATTR void VKAPI_CALL push(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset,
                                       uint32_t size, const void *data) {
    EXPECT(cb == H(VkCommandBuffer, 2) && layout == H(VkPipelineLayout, 20) && stages == VK_SHADER_STAGE_COMPUTE_BIT && !offset &&
           size == 64);
    memcpy(&observed, data, sizeof(observed));
    ++pushes;
}
static VKAPI_ATTR void VKAPI_CALL dispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    EXPECT(cb == H(VkCommandBuffer, 2) && x == 3 && y == 1 && z == 1 && publications == 1 && barriers == 1 && binds == 1 &&
           pushes == 1);
    ++dispatches;
}
static struct vkrt_instance_data instance = {.vtable = {.GetPhysicalDeviceProperties2 = properties}};
static struct vkrt_device_data device = {.device = H(VkDevice, 1),
                                         .physical_device = H(VkPhysicalDevice, 4),
                                         .instance_data = &instance,
                                         .max_build_dispatch_x = 3,
                                         .vtable = {.CreatePipelineLayout = create_layout,
                                                    .DestroyPipelineLayout = destroy_layout,
                                                    .CreateShaderModule = create_module,
                                                    .DestroyShaderModule = destroy_module,
                                                    .CreateComputePipelines = create_pipeline,
                                                    .DestroyPipeline = destroy_pipeline,
                                                    .CmdPipelineBarrier = barrier,
                                                    .CmdBindPipeline = bind,
                                                    .CmdPushConstants = push,
                                                    .CmdDispatch = dispatch}};
static struct vkrt_virtual_as as = {
    .owner = H(VkDevice, 1), .size = UINT64_C(1) << 40, .type = VK_ACCELERATION_STRUCTURE_TYPE_GENERIC_KHR};
static struct vkrt_cmd_buffer_data cb = {
    .cmd = H(VkCommandBuffer, 2), .device_data = &device, .bound_compute_pipeline = H(VkPipeline, 90)};
static void reset_commands(void) {
    barriers = binds = pushes = dispatches = publications = 0;
    cb.record_result = VK_SUCCESS;
}
static void no_commands(VkResult result) { EXPECT(cb.record_result == result && !barriers && !binds && !pushes && !dispatches); }
int main(void) {
    EXPECT(vkrt_as_serialization_payload(0) == 64 && vkrt_as_serialization_payload(3) == 80);
    EXPECT(vkrt_as_serialization_size(UINT64_MAX, 0) == 0 &&
           vkrt_as_serialization_size(UINT64_C(1) << 40, UINT32_MAX) > (UINT64_C(1) << 40));
    for (fail_step = 1; fail_step < 6; ++fail_step) {
        allocations = frees = pipeline_calls = destroyed_pipelines = destroyed_layouts = destroyed_modules = 0;
        EXPECT(vkrt_as_serialization_init(&device, &allocator) ==
               (fail_step == 1 ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_OUT_OF_DEVICE_MEMORY));
        EXPECT(!device.serialization && allocations == 1 && frees == (fail_step != 1));
        EXPECT(destroyed_layouts == (fail_step >= 3) && destroyed_modules == (fail_step >= 4) &&
               destroyed_pipelines == (fail_step == 5));
        vkrt_as_serialization_finish(&device);
    }
    fail_step = 0;
    pipeline_calls = 0;
    allocations = frees = destroyed_pipelines = destroyed_layouts = destroyed_modules = 0;
    EXPECT(vkrt_as_serialization_init(&device, &allocator) == VK_SUCCESS);
    vkrt_map_object(VKRT_HKEY(cb.cmd), &cb);
    vkrt_map_object(VKRT_HKEY(H(VkAccelerationStructureKHR, 3)), &as);
    VkCopyAccelerationStructureToMemoryInfoKHR serialize = {
        .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_TO_MEMORY_INFO_KHR,
        .src = H(VkAccelerationStructureKHR, 3),
        .dst.deviceAddress = UINT64_C(0xabcd00000100),
        .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_SERIALIZE_KHR};
    VkCopyMemoryToAccelerationStructureInfoKHR deserialize = {
        .sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_ACCELERATION_STRUCTURE_INFO_KHR,
        .src.deviceAddress = serialize.dst.deviceAddress,
        .dst = serialize.src,
        .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_DESERIALIZE_KHR};
    for (expected_operation = 0; expected_operation < 2; ++expected_operation) {
        reset_commands();
        if (expected_operation)
            vkrt_cmd_deserialize_as(&device, cb.cmd, &deserialize);
        else
            vkrt_cmd_serialize_as(&device, cb.cmd, &serialize);
        EXPECT(cb.record_result == VK_SUCCESS && barriers == 2 && binds == 2 && pushes == 1 && dispatches == 1);
        EXPECT(observed.as_capacity == as.size && observed.destination_type == 2 && !observed.reserved);
        EXPECT(observed.source == (expected_operation ? serialize.dst.deviceAddress : physical));
        EXPECT(observed.destination == (expected_operation ? physical : serialize.dst.deviceAddress));
    }
    uint8_t version[32];
    memcpy(version, observed.version_data, 32);
    for (unsigned i = 0; i < 16; ++i) EXPECT(version[i] == i + 1);
    VkAccelerationStructureVersionInfoKHR info = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_VERSION_INFO_KHR,
                                                  .pVersionData = version};
    VkAccelerationStructureCompatibilityKHR compatibility;
    vkrt_as_serialization_compatibility(&device, &info, &compatibility);
    EXPECT(compatibility == VK_ACCELERATION_STRUCTURE_COMPATIBILITY_COMPATIBLE_KHR);
    for (unsigned i = 0; i < 32; ++i) {
        version[i] ^= 1;
        vkrt_as_serialization_compatibility(&device, &info, &compatibility);
        version[i] ^= 1;
        EXPECT(compatibility == VK_ACCELERATION_STRUCTURE_COMPATIBILITY_INCOMPATIBLE_KHR);
    }
    reset_commands();
    serialize.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR;
    vkrt_cmd_serialize_as(&device, cb.cmd, &serialize);
    no_commands(VK_ERROR_FEATURE_NOT_PRESENT);
    EXPECT(!publications);
    serialize.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_SERIALIZE_KHR;
    reset_commands();
    serialize.dst.deviceAddress += 16;
    vkrt_cmd_serialize_as(&device, cb.cmd, &serialize);
    no_commands(VK_ERROR_INITIALIZATION_FAILED);
    EXPECT(!publications);
    serialize.dst.deviceAddress -= 16;
    reset_commands();
    physical = 0;
    vkrt_cmd_deserialize_as(&device, cb.cmd, &deserialize);
    no_commands(VK_ERROR_INITIALIZATION_FAILED);
    physical = UINT64_C(0x123400000010);
    reset_commands();
    as.size = 127;
    vkrt_cmd_deserialize_as(&device, cb.cmd, &deserialize);
    no_commands(VK_ERROR_OUT_OF_DEVICE_MEMORY);
    as.size = UINT64_C(1) << 40;
    reset_commands();
    cb.record_result = VK_ERROR_OUT_OF_HOST_MEMORY;
    vkrt_cmd_serialize_as(&device, cb.cmd, &serialize);
    no_commands(VK_ERROR_OUT_OF_HOST_MEMORY);
    EXPECT(!publications);
    vkrt_as_serialization_finish(&device);
    EXPECT(!device.serialization && allocations == 1 && frees == 1 && destroyed_pipelines == 2 && destroyed_layouts == 1 &&
           destroyed_modules == 1);
    vkrt_as_serialization_compatibility(&device, &info, &compatibility);
    EXPECT(compatibility == VK_ACCELERATION_STRUCTURE_COMPATIBILITY_INCOMPATIBLE_KHR);
    vkrt_unmap_object(VKRT_HKEY(cb.cmd));
    vkrt_unmap_object(VKRT_HKEY(H(VkAccelerationStructureKHR, 3)));
    vkrt_map_destroy(&vkrt_global.objects);
    printf("AS serialization host boundary: %u failures\n", failures);
    return failures != 0;
}
