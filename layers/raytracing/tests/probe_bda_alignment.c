/* Copyright 2026 The Khronos Group Inc.
 * SPDX-License-Identifier: MIT
 * Native ordinary-buffer qualification probe, not an AS conformance test.
 */
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x)                                \
    do {                                        \
        VkResult r = (x);                       \
        if (r) {                                \
            fprintf(stderr, "%s: %d\n", #x, r); \
            return 1;                           \
        }                                       \
    } while (0)
int main(void) {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance;
    CHECK(vkCreateInstance(&ici, 0, &instance));
    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &count, 0));
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    CHECK(vkEnumeratePhysicalDevices(instance, &count, devices));
    for (uint32_t p = 0; p < count; ++p) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[p], &props);
        printf("DEVICE name=%s vendor=0x%x device=0x%x api=%u.%u.%u driver=0x%x minStorageBufferOffsetAlignment=%llu\n",
               props.deviceName, props.vendorID, props.deviceID, VK_API_VERSION_MAJOR(props.apiVersion),
               VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion,
               (unsigned long long)props.limits.minStorageBufferOffsetAlignment);
        uint32_t fc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[p], &fc, 0);
        VkQueueFamilyProperties *fp = calloc(fc, sizeof(*fp));
        vkGetPhysicalDeviceQueueFamilyProperties(devices[p], &fc, fp);
        uint32_t family = 0;
        for (; family < fc && !(fp[family].queueFlags & VK_QUEUE_COMPUTE_BIT); ++family) {
        }
        free(fp);
        if (family == fc) continue;
        float priority = 1;
        VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                     .queueFamilyIndex = family,
                                     .queueCount = 1,
                                     .pQueuePriorities = &priority};
        VkPhysicalDeviceBufferDeviceAddressFeatures bda = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, .bufferDeviceAddress = 1};
        VkDeviceCreateInfo dci = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &bda, .queueCreateInfoCount = 1, .pQueueCreateInfos = &q};
        VkDevice device;
        CHECK(vkCreateDevice(devices[p], &dci, 0, &device));
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(devices[p], &mp);
        const uint64_t sizes[] = {256, 4096, 65536};
        for (uint32_t usage = 0; usage < 2; ++usage)
            for (uint32_t s = 0; s < 3; ++s) {
                VkBufferCreateInfo bi = {
                    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                    .size = sizes[s],
                    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                             (usage ? (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT) : 0)};
                VkBuffer buffers[2];
                CHECK(vkCreateBuffer(device, &bi, 0, &buffers[0]));
                CHECK(vkCreateBuffer(device, &bi, 0, &buffers[1]));
                VkMemoryDedicatedRequirements dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
                VkMemoryRequirements2 req = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedicated};
                VkBufferMemoryRequirementsInfo2 ri = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
                                                      .buffer = buffers[0]};
                vkGetBufferMemoryRequirements2(device, &ri, &req);
                VkDeviceSize offset = req.memoryRequirements.alignment;
                printf("REQ usage=0x%x bufferSize=%llu size=%llu alignment=%llu dedicatedRequired=%u dedicatedPreferred=%u\n",
                       bi.usage, (unsigned long long)bi.size, (unsigned long long)req.memoryRequirements.size,
                       (unsigned long long)offset, dedicated.requiresDedicatedAllocation, dedicated.prefersDedicatedAllocation);
                if (dedicated.requiresDedicatedAllocation) {
                    vkDestroyBuffer(device, buffers[1], 0);
                    vkDestroyBuffer(device, buffers[0], 0);
                    continue;
                }
                uint32_t type = 0;
                for (; type < mp.memoryTypeCount && !(req.memoryRequirements.memoryTypeBits & (1u << type)); ++type) {
                };
                VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                                   .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
                VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                           .pNext = &flags,
                                           .allocationSize = req.memoryRequirements.size + offset,
                                           .memoryTypeIndex = type};
                VkDeviceMemory memory;
                CHECK(vkAllocateMemory(device, &ai, 0, &memory));
                CHECK(vkBindBufferMemory(device, buffers[0], memory, 0));
                CHECK(vkBindBufferMemory(device, buffers[1], memory, offset));
                VkBufferDeviceAddressInfo a = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffers[0]},
                                          b = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buffers[1]};
                VkDeviceAddress av = vkGetBufferDeviceAddress(device, &a), bv = vkGetBufferDeviceAddress(device, &b);
                printf("BDA bind0=0x%llx mod128=%llu mod256=%llu bind%llu=0x%llx mod128=%llu mod256=%llu delta=%llu\n",
                       (unsigned long long)av, (unsigned long long)(av % 128), (unsigned long long)(av % 256),
                       (unsigned long long)offset, (unsigned long long)bv, (unsigned long long)(bv % 128),
                       (unsigned long long)(bv % 256), (unsigned long long)(bv - av));
                vkDestroyBuffer(device, buffers[1], 0);
                vkDestroyBuffer(device, buffers[0], 0);
                vkFreeMemory(device, memory, 0);
            }
        vkDestroyDevice(device, 0);
    }
    free(devices);
    vkDestroyInstance(instance, 0);
    return 0;
}
