// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#pragma once
// Native Vulkan fixture shared by the experimental device mechanism tests.
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#define CHECK(call)                                                   \
    do {                                                              \
        VkResult status = (call);                                     \
        if (status != VK_SUCCESS) {                                   \
            std::fprintf(stderr, "%s: VkResult %d\n", #call, status); \
            std::exit(1);                                             \
        }                                                             \
    } while (0)
template <typename T>
static T init(VkStructureType type) {
    return T{type};
}

struct Buffer {
    VkBuffer handle;
    VkDeviceMemory memory;
    VkDeviceAddress address;
    void* mapped;
};
struct Acceleration {
    VkAccelerationStructureKHR handle;
    VkDeviceAddress address;
    VkAccelerationStructureBuildGeometryInfoKHR build;
    VkAccelerationStructureBuildRangeInfoKHR range;
};
struct Fixture {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family = UINT32_MAX;
    VkCommandPool pool{};
    PFN_vkCreateAccelerationStructureKHR createAS{};
    PFN_vkDestroyAccelerationStructureKHR destroyAS{};
    PFN_vkGetAccelerationStructureBuildSizesKHR sizesAS{};
    PFN_vkGetAccelerationStructureDeviceAddressKHR addressAS{};
    PFN_vkCmdBuildAccelerationStructuresKHR buildAS{};
    VkDeviceSize scratchAlignment{};
    std::vector<Buffer> buffers;
    std::vector<VkAccelerationStructureKHR> structures;

    bool open(bool rayPipeline = false) {
        auto application = init<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        application.apiVersion = VK_API_VERSION_1_2;
        auto create = init<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        create.pApplicationInfo = &application;
        CHECK(vkCreateInstance(&create, nullptr, &instance));
        uint32_t count = 0;
        CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties candidateProperties;
            vkGetPhysicalDeviceProperties(candidate, &candidateProperties);
            if (candidateProperties.apiVersion < VK_API_VERSION_1_2) continue;
            auto rq = init<VkPhysicalDeviceRayQueryFeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR);
            auto rtp = init<VkPhysicalDeviceRayTracingPipelineFeaturesKHR>(
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR);
            auto as = init<VkPhysicalDeviceAccelerationStructureFeaturesKHR>(
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR);
            auto bda =
                init<VkPhysicalDeviceBufferDeviceAddressFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES);
            auto features = init<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
            features.pNext = &rq;
            rq.pNext = &rtp;
            rtp.pNext = &as;
            as.pNext = &bda;
            vkGetPhysicalDeviceFeatures2(candidate, &features);
            if ((!rayPipeline && !rq.rayQuery) || (rayPipeline && !rtp.rayTracingPipeline) || !as.accelerationStructure ||
                !bda.bufferDeviceAddress || !features.features.shaderInt64)
                continue;
            uint32_t extensionCount = 0;
            CHECK(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr));
            std::vector<VkExtensionProperties> extensions(extensionCount);
            CHECK(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, extensions.data()));
            bool hasAS = false, hasRQ = false, hasRTP = false, hasDeferred = false;
            for (const auto& extension : extensions) {
                hasAS |= !std::strcmp(extension.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
                hasRQ |= !std::strcmp(extension.extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME);
                hasRTP |= !std::strcmp(extension.extensionName, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
                hasDeferred |= !std::strcmp(extension.extensionName, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
            }
            if (!hasAS || (!rayPipeline && !hasRQ) || (rayPipeline && !hasRTP) || !hasDeferred) continue;
            uint32_t queueCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
            std::vector<VkQueueFamilyProperties> queues(queueCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queues.data());
            for (uint32_t q = 0; q < queueCount; ++q) {
                if ((queues[q].queueFlags & VK_QUEUE_COMPUTE_BIT) && queues[q].queueCount) {
                    physical = candidate;
                    family = q;
                    break;
                }
            }
            if (physical) break;
        }
        if (!physical) return false;
        auto properties = init<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
        auto asProperties = init<VkPhysicalDeviceAccelerationStructurePropertiesKHR>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR);
        properties.pNext = &asProperties;
        vkGetPhysicalDeviceProperties2(physical, &properties);
        std::printf("Native %s candidate proof: %s\n", rayPipeline ? "RTP" : "RQ", properties.properties.deviceName);
        scratchAlignment = asProperties.minAccelerationStructureScratchOffsetAlignment;
        float priority = 1.f;
        auto queueInfo = init<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        auto bda =
            init<VkPhysicalDeviceBufferDeviceAddressFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES);
        bda.bufferDeviceAddress = VK_TRUE;
        auto as = init<VkPhysicalDeviceAccelerationStructureFeaturesKHR>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR);
        as.accelerationStructure = VK_TRUE;
        as.pNext = &bda;
        auto rq = init<VkPhysicalDeviceRayQueryFeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR);
        rq.rayQuery = VK_TRUE;
        rq.pNext = &as;
        auto rtp = init<VkPhysicalDeviceRayTracingPipelineFeaturesKHR>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR);
        rtp.rayTracingPipeline = VK_TRUE;
        rtp.pNext = &as;
        VkPhysicalDeviceFeatures core{};
        core.shaderInt64 = VK_TRUE;
        const char* extensions[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                    rayPipeline ? VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME : VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
        auto deviceInfo = init<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        deviceInfo.pNext = rayPipeline ? static_cast<void*>(&rtp) : static_cast<void*>(&rq);
        deviceInfo.pEnabledFeatures = &core;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 3;
        deviceInfo.ppEnabledExtensionNames = extensions;
        CHECK(vkCreateDevice(physical, &deviceInfo, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
#define LOAD(member, function) member = reinterpret_cast<PFN_##function>(vkGetDeviceProcAddr(device, #function))
        LOAD(createAS, vkCreateAccelerationStructureKHR);
        LOAD(destroyAS, vkDestroyAccelerationStructureKHR);
        LOAD(sizesAS, vkGetAccelerationStructureBuildSizesKHR);
        LOAD(addressAS, vkGetAccelerationStructureDeviceAddressKHR);
        LOAD(buildAS, vkCmdBuildAccelerationStructuresKHR);
#undef LOAD
        if (!createAS || !destroyAS || !sizesAS || !addressAS || !buildAS) std::exit(1);
        auto poolInfo = init<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        poolInfo.queueFamilyIndex = family;
        CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &pool));
        return true;
    }
    Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
        Buffer value{};
        auto create = init<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        create.size = size;
        create.usage = usage;
        CHECK(vkCreateBuffer(device, &create, nullptr, &value.handle));
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, value.handle, &requirements);
        VkPhysicalDeviceMemoryProperties properties;
        vkGetPhysicalDeviceMemoryProperties(physical, &properties);
        VkMemoryPropertyFlags wanted = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0;
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
                type = i;
                break;
            }
        if (type == UINT32_MAX) std::exit(1);
        auto flags = init<VkMemoryAllocateFlagsInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO);
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        auto allocate = init<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        allocate.pNext = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ? &flags : nullptr;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        CHECK(vkAllocateMemory(device, &allocate, nullptr, &value.memory));
        CHECK(vkBindBufferMemory(device, value.handle, value.memory, 0));
        if (host) CHECK(vkMapMemory(device, value.memory, 0, VK_WHOLE_SIZE, 0, &value.mapped));
        if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
            auto address = init<VkBufferDeviceAddressInfo>(VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO);
            address.buffer = value.handle;
            value.address = vkGetBufferDeviceAddress(device, &address);
        }
        buffers.push_back(value);
        return value;
    }
    Acceleration acceleration(VkAccelerationStructureTypeKHR type, const VkAccelerationStructureGeometryKHR& geometry,
                              uint32_t count = 1) {
        Acceleration value{};
        value.build =
            init<VkAccelerationStructureBuildGeometryInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR);
        value.build.type = type;
        value.build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        value.build.geometryCount = 1;
        value.build.pGeometries = &geometry;
        value.range.primitiveCount = count;
        auto sizes = init<VkAccelerationStructureBuildSizesInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR);
        sizesAS(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &value.build, &count, &sizes);
        auto storage =
            buffer(sizes.accelerationStructureSize,
                   VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
        auto create = init<VkAccelerationStructureCreateInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR);
        create.type = type;
        create.buffer = storage.handle;
        create.size = sizes.accelerationStructureSize;
        CHECK(createAS(device, &create, nullptr, &value.handle));
        structures.push_back(value.handle);
        auto address =
            init<VkAccelerationStructureDeviceAddressInfoKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR);
        address.accelerationStructure = value.handle;
        value.address = addressAS(device, &address);
        auto scratch = buffer(sizes.buildScratchSize + scratchAlignment - 1,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
        value.build.dstAccelerationStructure = value.handle;
        value.build.scratchData.deviceAddress = (scratch.address + scratchAlignment - 1) & ~(scratchAlignment - 1);
        return value;
    }
    VkCommandBuffer begin() {
        auto allocate = init<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        allocate.commandPool = pool;
        allocate.commandBufferCount = 1;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        VkCommandBuffer command;
        CHECK(vkAllocateCommandBuffers(device, &allocate, &command));
        auto start = init<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        CHECK(vkBeginCommandBuffer(command, &start));
        return command;
    }
    static void barrier(VkCommandBuffer command, VkPipelineStageFlags source, VkAccessFlags src, VkPipelineStageFlags destination,
                        VkAccessFlags dst) {
        auto memory = init<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        memory.srcAccessMask = src;
        memory.dstAccessMask = dst;
        vkCmdPipelineBarrier(command, source, destination, 0, 1, &memory, 0, nullptr, 0, nullptr);
    }
    void submit(VkCommandBuffer command) {
        CHECK(vkEndCommandBuffer(command));
        auto submit = init<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        VkFence fence;
        auto create = init<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        CHECK(vkCreateFence(device, &create, nullptr, &fence));
        CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, pool, 1, &command);
    }
    ~Fixture() {
        if (device) {
            for (auto as : structures) destroyAS(device, as, nullptr);
            for (auto value : buffers) {
                if (value.mapped) vkUnmapMemory(device, value.memory);
                vkDestroyBuffer(device, value.handle, nullptr);
                vkFreeMemory(device, value.memory, nullptr);
            }
            vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
};
