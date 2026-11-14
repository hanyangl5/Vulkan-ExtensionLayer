/* Copyright (c) 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: MIT
 * Shared shader-object fixture helpers (device admission, AS preparation and
 * command submission). Ray-query specific pipelines live in their own tests.
 */
#ifndef VKRT_SHADER_OBJECT_HARNESS_H
#define VKRT_SHADER_OBJECT_HARNESS_H
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <vulkan/vulkan.h>
static VkResult make_device(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);
static VkResult make_instance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
#define vkCreateDevice make_device
#define vkCreateInstance make_instance
#define VKRT_TEST_FIXTURE_ONLY
#include "gpu_harness.h"
#undef vkCreateDevice
#undef vkCreateInstance

static VkResult make_instance(const VkInstanceCreateInfo *source, const VkAllocationCallbacks *allocator, VkInstance *out) {
    VkApplicationInfo application = *source->pApplicationInfo;
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo local = *source;
    local.pApplicationInfo = &application;
    return vkCreateInstance(&local, allocator, out);
}
static VkResult make_device(VkPhysicalDevice physical, const VkDeviceCreateInfo *source, const VkAllocationCallbacks *allocator,
                            VkDevice *out) {
    VkPhysicalDeviceShaderObjectFeaturesEXT objects = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingFeatures rendering = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
                                                          .pNext = &objects};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, .pNext = &rendering};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &rt};
    vkGetPhysicalDeviceFeatures2(physical, &features);
    if (!objects.shaderObject || !rendering.dynamicRendering || !rt.rayTracingPipeline || !features.features.shaderInt64) exit(77);
    objects.pNext = (void *)source->pNext;
    const char **extensions = calloc((size_t)source->enabledExtensionCount + 2, sizeof(*extensions));
    if (!extensions) return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(extensions, source->ppEnabledExtensionNames, source->enabledExtensionCount * sizeof(*extensions));
    VkDeviceCreateInfo local = *source;
    VkPhysicalDeviceFeatures core = source->pEnabledFeatures ? *source->pEnabledFeatures : (VkPhysicalDeviceFeatures){0};
    core.shaderInt64 = VK_TRUE;
    local.pEnabledFeatures = &core;
    local.pNext = &rt;
    local.ppEnabledExtensionNames = extensions;
    extensions[local.enabledExtensionCount++] = VK_EXT_SHADER_OBJECT_EXTENSION_NAME;
    extensions[local.enabledExtensionCount++] = VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME;
    VkResult result = vkCreateDevice(physical, &local, allocator, out);
    free(extensions);
    return result;
}
struct acceleration {
    struct gpu_buf storage, scratch;
    VkAccelerationStructureKHR handle;
    VkDeviceAddress address;
    VkAccelerationStructureBuildGeometryInfoKHR info;
    VkAccelerationStructureBuildRangeInfoKHR range;
};
static void prepare(struct acceleration *as, VkAccelerationStructureTypeKHR type,
                    const VkAccelerationStructureGeometryKHR *geometry) {
    memset(as, 0, sizeof(*as));
    as->range.primitiveCount = 1;
    as->info =
        (VkAccelerationStructureBuildGeometryInfoKHR){.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                                                      .type = type,
                                                      .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                                                      .geometryCount = 1,
                                                      .pGeometries = geometry};
    VkAccelerationStructureBuildSizesInfoKHR size = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    PFN_vkGetAccelerationStructureBuildSizesKHR sizes =
        (void *)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureBuildSizesKHR");
    sizes(g_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &as->info, &as->range.primitiveCount, &size);
    buf_create(size.accelerationStructureSize,
               VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &as->storage);
    VkAccelerationStructureCreateInfoKHR create = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                   .buffer = as->storage.buf,
                                                   .size = size.accelerationStructureSize,
                                                   .type = type};
    CHECK(pCreateAS(g_dev, &create, NULL, &as->handle));
    VkAccelerationStructureDeviceAddressInfoKHR address = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, .accelerationStructure = as->handle};
    PFN_vkGetAccelerationStructureDeviceAddressKHR get =
        (void *)vkGetDeviceProcAddr(g_dev, "vkGetAccelerationStructureDeviceAddressKHR");
    as->address = get(g_dev, &address);
    buf_create(size.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &as->scratch);
    as->info.dstAccelerationStructure = as->handle;
    as->info.scratchData.deviceAddress = (as->scratch.addr + 255) & ~UINT64_C(255);
}
static void memory_barrier(VkCommandBuffer command, VkPipelineStageFlags source, VkAccessFlags src,
                           VkPipelineStageFlags destination, VkAccessFlags dst) {
    VkMemoryBarrier memory = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst};
    vkCmdPipelineBarrier(command, source, destination, 0, 1, &memory, 0, NULL, 0, NULL);
}
static VkCommandBuffer begin(void) {
    VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                            .commandPool = g_pool,
                                            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                            .commandBufferCount = 1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(g_dev, &allocate, &command));
    VkCommandBufferBeginInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(command, &info));
    return command;
}
static void submit(VkCommandBuffer command) {
    CHECK(vkEndCommandBuffer(command));
    VkSubmitInfo info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    CHECK(vkQueueSubmit(g_queue, 1, &info, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(g_queue));
    vkFreeCommandBuffers(g_dev, g_pool, 1, &command);
}

#endif /* VKRT_SHADER_OBJECT_HARNESS_H */
