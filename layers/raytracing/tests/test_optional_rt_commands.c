/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "vkrt_layer.h"

struct vkrt_global vkrt_global = VKRT_GLOBAL_INITIALIZER;
#define H(type, value) ((type)(uintptr_t)(value))
#define CHECK(x)                                                 \
    do {                                                         \
        if (!(x)) {                                              \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
            ++failures;                                          \
        }                                                        \
    } while (0)
static unsigned failures, native_queries, native_calls;
static const VkDevice device = H(VkDevice, 1);
static const VkCommandBuffer command = H(VkCommandBuffer, 2);
static bool native_available;
static VKAPI_ATTR VkResult VKAPI_CALL native_handles(VkDevice d, VkPipeline pipeline, uint32_t first, uint32_t count, size_t bytes,
                                                     void *data) {
    CHECK(d == device && pipeline == H(VkPipeline, 3) && first == 4 && count == 5 && bytes == 8);
    ++native_calls;
    memset(data, 0x5a, bytes);
    return VK_INCOMPLETE;
}
static VKAPI_ATTR void VKAPI_CALL native_indirect(VkCommandBuffer cb, const VkStridedDeviceAddressRegionKHR *rg,
                                                  const VkStridedDeviceAddressRegionKHR *miss,
                                                  const VkStridedDeviceAddressRegionKHR *hit,
                                                  const VkStridedDeviceAddressRegionKHR *callable, VkDeviceAddress address) {
    CHECK(cb == command && rg->deviceAddress == 16 && miss == rg && hit == rg && callable == rg && address == 64);
    ++native_calls;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL native_proc(VkDevice d, const char *name) {
    CHECK(d == device);
    ++native_queries;
    if (!native_available) return NULL;
    if (!strcmp(name, "vkGetRayTracingCaptureReplayShaderGroupHandlesKHR")) return (PFN_vkVoidFunction)native_handles;
    if (!strcmp(name, "vkCmdTraceRaysIndirectKHR")) return (PFN_vkVoidFunction)native_indirect;
    return NULL;
}
int main(void) {
    struct vkrt_device_data dev = {.device = device, .virtual_rt = true};
    dev.vtable.GetDeviceProcAddr = native_proc;
    struct vkrt_cmd_buffer_data cb = {.cmd = command, .device_data = &dev, .record_result = VK_SUCCESS};
    vkrt_map_object(VKRT_HKEY(device), &dev);
    vkrt_map_object(VKRT_HKEY(command), &cb);
    const char *names[] = {"vkBuildAccelerationStructuresKHR",
                           "vkCopyAccelerationStructureKHR",
                           "vkCopyAccelerationStructureToMemoryKHR",
                           "vkCopyMemoryToAccelerationStructureKHR",
                           "vkWriteAccelerationStructuresPropertiesKHR",
                           "vkGetRayTracingCaptureReplayShaderGroupHandlesKHR",
                           "vkCmdTraceRaysIndirectKHR"};
    for (unsigned i = 0; i < VKRT_ARRAY_SIZE(names); ++i) CHECK(vkrt_optional_rt_proc(names[i]) != NULL);
    CHECK(vkrt_optional_rt_proc("vkUnknownCommand") == NULL);
    PFN_vkBuildAccelerationStructuresKHR build = (void *)vkrt_optional_rt_proc(names[0]);
    PFN_vkCopyAccelerationStructureKHR clone = (void *)vkrt_optional_rt_proc(names[1]);
    PFN_vkCopyAccelerationStructureToMemoryKHR serialize = (void *)vkrt_optional_rt_proc(names[2]);
    PFN_vkCopyMemoryToAccelerationStructureKHR deserialize = (void *)vkrt_optional_rt_proc(names[3]);
    PFN_vkWriteAccelerationStructuresPropertiesKHR write = (void *)vkrt_optional_rt_proc(names[4]);
    PFN_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR handles = (void *)vkrt_optional_rt_proc(names[5]);
    PFN_vkCmdTraceRaysIndirectKHR indirect = (void *)vkrt_optional_rt_proc(names[6]);
    uint64_t output = UINT64_C(0x123456789abcdef0);
    const VkAccelerationStructureKHR object = H(VkAccelerationStructureKHR, 6);
    const VkStridedDeviceAddressRegionKHR region = {16, 32, 32};
    for (unsigned available = 0; available < 2; ++available) {
        native_available = available != 0;
        CHECK(build(device, VK_NULL_HANDLE, 1, NULL, NULL) == VK_ERROR_FEATURE_NOT_PRESENT);
        CHECK(clone(device, VK_NULL_HANDLE, NULL) == VK_ERROR_FEATURE_NOT_PRESENT);
        CHECK(serialize(device, VK_NULL_HANDLE, NULL) == VK_ERROR_FEATURE_NOT_PRESENT);
        CHECK(deserialize(device, VK_NULL_HANDLE, NULL) == VK_ERROR_FEATURE_NOT_PRESENT);
        CHECK(write(device, 1, &object, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, sizeof(output), &output, 8) ==
              VK_ERROR_FEATURE_NOT_PRESENT);
        CHECK(handles(device, H(VkPipeline, 3), 4, 5, sizeof(output), &output) == VK_ERROR_FEATURE_NOT_PRESENT);
        CHECK(output == UINT64_C(0x123456789abcdef0));
        cb.record_result = VK_SUCCESS;
        indirect(command, &region, &region, &region, &region, 64);
        CHECK(cb.record_result == VK_ERROR_FEATURE_NOT_PRESENT);
        cb.record_result = VK_ERROR_OUT_OF_HOST_MEMORY;
        indirect(command, &region, &region, &region, &region, 64);
        CHECK(cb.record_result == VK_ERROR_OUT_OF_HOST_MEMORY);
        CHECK(native_queries == 0 && native_calls == 0);
    }
    /* Ordinary native devices retain dispatch and output behavior. */
    dev.virtual_rt = false;
    native_available = true;
    CHECK(handles(device, H(VkPipeline, 3), 4, 5, sizeof(output), &output) == VK_INCOMPLETE);
    CHECK(output == UINT64_C(0x5a5a5a5a5a5a5a5a));
    cb.record_result = VK_SUCCESS;
    indirect(command, &region, &region, &region, &region, 64);
    CHECK(native_calls == 2 && native_queries == 2 && cb.record_result == VK_SUCCESS);
    native_available = false;
    CHECK(build(device, VK_NULL_HANDLE, 1, NULL, NULL) == VK_ERROR_FEATURE_NOT_PRESENT);
    CHECK(native_queries == 3 && native_calls == 2);
    vkrt_unmap_object(VKRT_HKEY(command));
    vkrt_unmap_object(VKRT_HKEY(device));
    printf("Disabled optional RT entry points: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
