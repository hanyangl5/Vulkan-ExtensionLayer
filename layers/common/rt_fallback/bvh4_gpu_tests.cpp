// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "bvh4_gpu.h"
#include "bvh4_mesa_gpu.h"
#include "mesa_ir_test_fixture.h"
#include <vulkan/vulkan.h>
#include "../../../vkrt_layer/layer/bvh/bvh_ir_builder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

using namespace vel::rt::bvh4;
static void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static void VkCheck(VkResult result) { Check(result == VK_SUCCESS, "Vulkan operation failed"); }
static uint32_t validation_errors = 0;
static VKAPI_ATTR VkBool32 VKAPI_CALL DebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                                   const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++validation_errors;
        std::cerr << data->pMessage << '\n';
    }
    return VK_FALSE;
}

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
    uint64_t address = 0;
};
struct Context {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline[2]{};
    VkPipelineLayout mesa_layout = VK_NULL_HANDLE;
    VkPipeline mesa_pipeline[3]{};
    VkPipelineLayout builder_layout = VK_NULL_HANDLE;
    VkPipeline builder_pipeline[5]{};
    VkPhysicalDeviceMemoryProperties memory{};
    uint32_t queue_family = 0;

    ~Context() {
        if (device) {
            for (auto p : pipeline) vkDestroyPipeline(device, p, nullptr);
            for (auto p : mesa_pipeline) vkDestroyPipeline(device, p, nullptr);
            for (auto p : builder_pipeline) vkDestroyPipeline(device, p, nullptr);
            vkDestroyPipelineLayout(device, builder_layout, nullptr);
            vkDestroyPipelineLayout(device, mesa_layout, nullptr);
            vkDestroyPipelineLayout(device, layout, nullptr);
            vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (messenger) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            destroy(instance, messenger, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
    bool Init(const char* path, bool validation, bool builder = false) {
        const char* layer_name = "VK_LAYER_KHRONOS_validation";
        const char* extension_name = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        ci.enabledLayerCount = validation ? 1 : 0;
        ci.ppEnabledLayerNames = &layer_name;
        ci.enabledExtensionCount = validation ? 1 : 0;
        ci.ppEnabledExtensionNames = &extension_name;
        VkCheck(vkCreateInstance(&ci, nullptr, &instance));
        if (validation) {
            VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
            debug.pfnUserCallback = DebugMessage;
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            Check(create != nullptr, "debug utils unavailable");
            VkCheck(create(instance, &debug, nullptr, &messenger));
        }
        uint32_t count = 0;
        VkCheck(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> physicals(count);
        VkCheck(vkEnumeratePhysicalDevices(instance, &count, physicals.data()));
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        for (auto candidate : physicals) {
            VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
            VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            f12.pNext = &f11;
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &f12;
            vkGetPhysicalDeviceFeatures2(candidate, &features);
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_2 || !f12.bufferDeviceAddress || !f12.scalarBlockLayout ||
                !features.features.shaderInt64 || properties.limits.maxPushConstantsSize < sizeof(MesaGpuEncodeArgs))
                continue;
            if (builder && (!features.features.shaderInt16 || !f11.storageBuffer16BitAccess || !f12.storageBuffer8BitAccess ||
                            !f12.shaderInt8 || !f12.vulkanMemoryModel || !f12.vulkanMemoryModelDeviceScope ||
                            properties.limits.maxPushConstantsSize < 128))
                continue;
            if (builder) {
                VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
                VkPhysicalDeviceFloatControlsProperties controls{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
                subgroup.pNext = &controls;
                VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
                properties2.pNext = &subgroup;
                vkGetPhysicalDeviceProperties2(candidate, &properties2);
                if (!(subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
                    !(subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT) ||
                    !controls.shaderSignedZeroInfNanPreserveFloat32)
                    continue;
            }
            uint32_t families_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(families_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families_count, families.data());
            for (uint32_t i = 0; i < families_count; ++i) {
                if (!(families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
                physical = candidate;
                queue_family = i;
                std::cout << "BVH4 component device: " << properties.deviceName << '\n';
                break;
            }
            if (physical) break;
        }
        if (!physical) return false;
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        float priority = 1;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queue_family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        f12.bufferDeviceAddress = VK_TRUE;
        f12.scalarBlockLayout = VK_TRUE;
        VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        if (builder) {
            f11.storageBuffer16BitAccess = VK_TRUE;
            f12.pNext = &f11;
            f12.storageBuffer8BitAccess = f12.shaderInt8 = VK_TRUE;
            f12.vulkanMemoryModel = f12.vulkanMemoryModelDeviceScope = VK_TRUE;
        }
        VkPhysicalDeviceFeatures features{};
        features.shaderInt64 = VK_TRUE;
        features.shaderInt16 = builder ? VK_TRUE : VK_FALSE;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &f12;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.pEnabledFeatures = &features;
        // No RT extensions, features, AS descriptors or fallback layers.
        VkCheck(vkCreateDevice(physical, &dci, nullptr, &device));
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = queue_family;
        VkCheck(vkCreateCommandPool(device, &pci, nullptr, &pool));
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GpuEncodeArgs)};
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges = &range;
        VkCheck(vkCreatePipelineLayout(device, &lci, nullptr, &layout));
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        Check(file.good(), "shader file unavailable");
        const auto bytes = file.tellg();
        Check(bytes > 0 && bytes % 4 == 0, "invalid shader size");
        std::vector<uint32_t> code(size_t(bytes) / 4);
        file.seekg(0);
        Check(bool(file.read(reinterpret_cast<char*>(code.data()), bytes)), "shader read failed");
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = size_t(bytes);
        smci.pCode = code.data();
        VkShaderModule module;
        VkCheck(vkCreateShaderModule(device, &smci, nullptr, &module));
        for (uint32_t phase = 0; phase < 2; ++phase) {
            VkSpecializationMapEntry entry{0, 0, sizeof(phase)};
            VkSpecializationInfo specialization{1, &entry, sizeof(phase), &phase};
            VkComputePipelineCreateInfo p{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            p.layout = layout;
            p.stage = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            p.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            p.stage.module = module;
            p.stage.pName = "main";
            p.stage.pSpecializationInfo = &specialization;
            VkCheck(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &p, nullptr, &pipeline[phase]));
        }
        vkDestroyShaderModule(device, module, nullptr);
        return true;
    }
    void InitMesa(const char* path) {
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(MesaGpuEncodeArgs)};
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges = &range;
        VkCheck(vkCreatePipelineLayout(device, &lci, nullptr, &mesa_layout));
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        Check(file.good(), "Mesa encoder shader unavailable");
        const auto bytes = file.tellg();
        Check(bytes > 0 && bytes % 4 == 0, "Mesa encoder shader size");
        std::vector<uint32_t> code(size_t(bytes) / 4);
        file.seekg(0);
        Check(bool(file.read(reinterpret_cast<char*>(code.data()), bytes)), "Mesa encoder shader read");
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = size_t(bytes);
        smci.pCode = code.data();
        VkShaderModule module;
        VkCheck(vkCreateShaderModule(device, &smci, nullptr, &module));
        for (uint32_t phase = 0; phase < 3; ++phase) {
            VkSpecializationMapEntry entry{0, 0, sizeof(phase)};
            VkSpecializationInfo specialization{1, &entry, sizeof(phase), &phase};
            VkComputePipelineCreateInfo p{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            p.layout = mesa_layout;
            p.stage = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            p.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            p.stage.module = module;
            p.stage.pName = "main";
            p.stage.pSpecializationInfo = &specialization;
            VkCheck(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &p, nullptr, &mesa_pipeline[phase]));
        }
        vkDestroyShaderModule(device, module, nullptr);
    }
    void InitBuilder(const char* directory) {
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges = &range;
        VkCheck(vkCreatePipelineLayout(device, &lci, nullptr, &builder_layout));
        const char* names[] = {"vkrt_init", "leaf", "morton_sort", "lbvh_main", "lbvh_generate_ir"};
        uint32_t values[] = {64, 0, 0, 0, 128, 2};
        VkSpecializationMapEntry entries[6];
        for (uint32_t i = 0; i < 6; ++i) entries[i] = {i, i * 4, 4};
        VkSpecializationInfo specialization{6, entries, sizeof(values), values};
        for (uint32_t pass = 0; pass < 5; ++pass) {
            std::ifstream file(std::string(directory) + "/" + names[pass] + ".spv", std::ios::binary | std::ios::ate);
            Check(file.good(), "Mesa builder shader unavailable");
            const auto bytes = file.tellg();
            Check(bytes > 0 && bytes % 4 == 0, "Mesa builder shader size");
            std::vector<uint32_t> code(size_t(bytes) / 4);
            file.seekg(0);
            Check(bool(file.read(reinterpret_cast<char*>(code.data()), bytes)), "Mesa builder shader read");
            VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            smci.codeSize = size_t(bytes);
            smci.pCode = code.data();
            VkShaderModule module;
            VkCheck(vkCreateShaderModule(device, &smci, nullptr, &module));
            VkComputePipelineCreateInfo p{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            p.layout = builder_layout;
            p.stage = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            p.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            p.stage.module = module;
            p.stage.pName = "main";
            p.stage.pSpecializationInfo = &specialization;
            VkCheck(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &p, nullptr, &builder_pipeline[pass]));
            vkDestroyShaderModule(device, module, nullptr);
        }
    }
    void RecordBuilder(VkCommandBuffer cmd, const VkAccelerationStructureBuildGeometryInfoKHR& info,
                       const VkAccelerationStructureBuildRangeInfoKHR* ranges, uint64_t output, uint64_t scratch) {
        struct Emit {
            Context* context;
            VkCommandBuffer command;
        } emit{this, cmd};
        auto callback = [](void* user, uint32_t pass, const void* data, uint32_t size, uint32_t groups) {
            const auto& e = *static_cast<Emit*>(user);
            vkCmdBindPipeline(e.command, VK_PIPELINE_BIND_POINT_COMPUTE, e.context->builder_pipeline[pass]);
            vkCmdPushConstants(e.command, e.context->builder_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, size, data);
            vkCmdDispatch(e.command, groups, 1, 1);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(e.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                 &barrier, 0, nullptr, 0, nullptr);
        };
        VkCheck(bvh_ir_record_build(&info, ranges, output, scratch, 0, callback, &emit));
    }
    Buffer Allocate(uint64_t bytes) {
        Buffer b;
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = bytes + 127;
        ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VkCheck(vkCreateBuffer(device, &ci, nullptr, &b.handle));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, b.handle, &req);
        uint32_t type = 0;
        const auto required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (; type < memory.memoryTypeCount; ++type)
            if ((req.memoryTypeBits & (1u << type)) && (memory.memoryTypes[type].propertyFlags & required) == required) break;
        Check(type < memory.memoryTypeCount, "test requires host-coherent memory");
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = &flags;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        VkCheck(vkAllocateMemory(device, &ai, nullptr, &b.memory));
        VkCheck(vkBindBufferMemory(device, b.handle, b.memory, 0));
        void* mapped;
        VkCheck(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &mapped));
        b.mapped = static_cast<uint8_t*>(mapped);
        VkBufferDeviceAddressInfo address{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        address.buffer = b.handle;
        b.address = vkGetBufferDeviceAddress(device, &address);
        // The ABI requires 128-byte AS alignment. Vulkan does not promise that
        // for every buffer address, so align a subrange of the larger buffer.
        const uint64_t padding = (128 - b.address % 128) % 128;
        b.address += padding;
        b.mapped += padding;
        std::memset(b.mapped, 0xa5, size_t(bytes));
        return b;
    }
    void Free(Buffer b) {
        vkUnmapMemory(device, b.memory);
        vkDestroyBuffer(device, b.handle, nullptr);
        vkFreeMemory(device, b.memory, nullptr);
    }
    void EncodeGpu(const Buffer& source, const Buffer& mapping, const Buffer& output, uint32_t count, uint32_t root) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd;
        VkCheck(vkAllocateCommandBuffers(device, &ai, &cmd));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VkCheck(vkBeginCommandBuffer(cmd, &begin));
        for (uint32_t phase = 0; phase < 2; ++phase) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline[phase]);
            // Deliberately force small chunks, including a partial final group.
            for (uint64_t first = 0; first < count; first += 128) {
                GpuEncodeArgs args{source.address, mapping.address + 128, output.address + 128, first, count, root};
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(args), &args);
                vkCmdDispatch(cmd, uint32_t((std::min(uint64_t(count) - first, uint64_t(128)) + 63) / 64), 1, 1);
            }
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = phase ? VK_ACCESS_HOST_READ_BIT : VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 phase ? VK_PIPELINE_STAGE_HOST_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0,
                                 nullptr, 0, nullptr);
        }
        VkCheck(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence;
        VkCheck(vkCreateFence(device, &fci, nullptr, &fence));
        VkCheck(vkQueueSubmit(queue, 1, &submit, fence));
        VkCheck(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, pool, 1, &cmd);
    }
    void EncodeMesa(MesaGpuEncodeArgs args, const std::function<void(VkCommandBuffer)>& producer = {}) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd;
        VkCheck(vkAllocateCommandBuffers(device, &ai, &cmd));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VkCheck(vkBeginCommandBuffer(cmd, &begin));
        if (producer) producer(cmd);
        for (uint32_t phase = 0; phase < 3; ++phase) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mesa_pipeline[phase]);
            for (uint64_t first = 0; first < std::max(args.capacity, 1u); first += 128) {
                args.first = first;
                vkCmdPushConstants(cmd, mesa_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(args), &args);
                vkCmdDispatch(cmd, uint32_t((std::min(uint64_t(std::max(args.capacity, 1u)) - first, uint64_t(128)) + 63) / 64), 1,
                              1);
            }
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = phase == 2 ? VK_ACCESS_HOST_READ_BIT : VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 phase == 2 ? VK_PIPELINE_STAGE_HOST_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0,
                                 nullptr, 0, nullptr);
        }
        VkCheck(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence;
        VkCheck(vkCreateFence(device, &fci, nullptr, &fence));
        VkCheck(vkQueueSubmit(queue, 1, &submit, fence));
        VkCheck(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
        vkDestroyFence(device, fence, nullptr);
        vkFreeCommandBuffers(device, pool, 1, &cmd);
    }
};

static Bounds Unite(Bounds a, const Bounds& b) {
    for (size_t i = 0; i < 3; ++i) {
        a.min[i] = std::min(a.min[i], b.min[i]);
        a.max[i] = std::max(a.max[i], b.max[i]);
    }
    return a;
}
static bool Contains(const float* lower, const float* upper, const Bounds& b) {
    for (size_t i = 0; i < 3; ++i)
        if (!std::isfinite(lower[i]) || !std::isfinite(upper[i]) || lower[i] > upper[i] || lower[i] > b.min[i] ||
            upper[i] < b.max[i])
            return false;
    return true;
}
static std::vector<BuildNode> Tree(uint32_t leaves, uint32_t seed, bool deep, uint32_t& root) {
    std::mt19937 random(seed);
    std::vector<BuildNode> tree;
    std::vector<uint32_t> roots;
    uint32_t counters[3]{};
    for (uint32_t i = 0; i < leaves; ++i) {
        float x = float(int(random() % 400) - 200);
        uint32_t kind = i % 3;
        roots.push_back(uint32_t(tree.size()));
        tree.push_back({{{x, x - 1, x + 2}, {x + 3, x + 4, x + 5}},
                        VEL_RT_BVH4_INVALID_ID,
                        VEL_RT_BVH4_INVALID_ID,
                        MakeId(kind + 1, counters[kind]++)});
    }
    while (roots.size() > 1) {
        size_t a = deep ? roots.size() - 1 : random() % roots.size();
        uint32_t left = roots[a];
        roots.erase(roots.begin() + std::ptrdiff_t(a));
        size_t b = deep ? roots.size() - 1 : random() % roots.size();
        uint32_t right = roots[b];
        roots.erase(roots.begin() + std::ptrdiff_t(b));
        roots.push_back(uint32_t(tree.size()));
        tree.push_back({Unite(tree[left].bounds, tree[right].bounds), left, right});
    }
    root = roots.empty() ? VEL_RT_BVH4_INVALID_ID : roots[0];
    // Exercise arbitrary node order instead of assuming parents follow children.
    std::vector<uint32_t> order(tree.size()), inverse(tree.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = uint32_t(i);
    std::shuffle(order.begin(), order.end(), random);
    for (size_t i = 0; i < order.size(); ++i) inverse[order[i]] = uint32_t(i);
    auto original = tree;
    for (size_t i = 0; i < order.size(); ++i) {
        tree[i] = original[order[i]];
        if (tree[i].leaf_id == VEL_RT_BVH4_INVALID_ID) {
            tree[i].left = inverse[tree[i].left];
            tree[i].right = inverse[tree[i].right];
        }
    }
    if (!tree.empty()) root = inverse[root];
    return tree;
}

static void Run(Context& ctx, const std::vector<BuildNode>& tree, uint32_t root) {
    // CPU validation supplies the input contract, but its collapsed topology is
    // never used as the GPU output oracle. Check GPU topology from leaf bounds.
    const auto reference = Encode(tree, root);
    const auto counts = reference.counts;
    const auto leaves = counts.triangles + counts.aabbs + counts.instances;
    const auto internals = tree.size() - size_t(leaves);
    const auto layout = PlanLayout(internals, counts);
    auto source = ctx.Allocate(std::max(size_t(4), tree.size() * sizeof(BuildNode)));
    auto mapping = ctx.Allocate(tree.size() * 4 + 256);
    auto output = ctx.Allocate(layout.size + 256);
    Check((source.address % 4) == 0 && (mapping.address % 4) == 0 && (output.address % 128) == 0, "BDA alignment");
    if (!tree.empty()) std::memcpy(source.mapped, tree.data(), tree.size() * sizeof(BuildNode));
    VelRtBvh4Header header{};
    header.magic = VEL_RT_BVH4_MAGIC;
    header.version = VEL_RT_BVH4_ABI_VERSION;
    header.root_id = VEL_RT_BVH4_INVALID_ID;
    header.triangle_count = uint32_t(counts.triangles);
    header.aabb_count = uint32_t(counts.aabbs);
    header.instance_count = uint32_t(counts.instances);
    header.node_offset = layout.nodes;
    header.triangle_offset = layout.triangles;
    header.aabb_offset = layout.aabbs;
    header.instance_offset = layout.instances;
    header.leaf_parent_offset = layout.leaf_parents;
    header.total_size = layout.size;
    std::memcpy(output.mapped + 128, &header, sizeof(header));
    ctx.EncodeGpu(source, mapping, output, uint32_t(tree.size()), root);
    const auto* bytes = output.mapped + 128;
    std::memcpy(&header, bytes, sizeof(header));
    Check(header.node_count == internals, "GPU internal count");
    Check(header.triangle_count == counts.triangles && header.aabb_count == counts.aabbs &&
              header.instance_count == counts.instances && header.node_offset == layout.nodes &&
              header.triangle_offset == layout.triangles && header.aabb_offset == layout.aabbs &&
              header.instance_offset == layout.instances && header.leaf_parent_offset == layout.leaf_parents,
          "GPU region or count corruption");
    Check(header.magic == VEL_RT_BVH4_MAGIC && header.version == VEL_RT_BVH4_ABI_VERSION && header.total_size == layout.size,
          "GPU header corruption");
    for (size_t i = 0; i < 128; ++i) {
        Check(output.mapped[i] == 0xa5 && output.mapped[size_t(layout.size) + 128 + i] == 0xa5, "AS guard overwritten");
        Check(mapping.mapped[i] == 0xa5 && mapping.mapped[tree.size() * 4 + 128 + i] == 0xa5, "mapping guard overwritten");
    }
    for (uint64_t i = layout.triangles; i < layout.leaf_parents; ++i) Check(bytes[size_t(i)] == 0xa5, "leaf record overwritten");
    std::unordered_map<uint32_t, Bounds> expected, computed;
    for (const auto& node : tree)
        if (node.leaf_id != VEL_RT_BVH4_INVALID_ID) expected[node.leaf_id] = node.bounds;
    struct Task {
        uint32_t id, parent;
        bool finish;
    };
    std::vector<Task> stack;
    if (tree.empty())
        Check(header.root_id == VEL_RT_BVH4_INVALID_ID, "empty GPU root");
    else
        stack.push_back({header.root_id, VEL_RT_BVH4_INVALID_ID, false});
    std::set<uint32_t> visited;
    size_t internal_seen = 0, leaf_seen = 0;
    while (!stack.empty()) {
        auto task = stack.back();
        stack.pop_back();
        uint32_t kind = task.id >> 30, index = task.id & VEL_RT_BVH4_INDEX_MASK;
        if (kind) {
            Check(visited.insert(task.id).second && expected.count(task.id) == 1, "GPU leaf coverage/duplicate");
            uint64_t slot = index + (kind > 1 ? counts.triangles : 0) + (kind > 2 ? counts.aabbs : 0);
            uint32_t parent;
            std::memcpy(&parent, bytes + layout.leaf_parents + slot * 4, 4);
            Check(parent == task.parent, "GPU leaf parent");
            computed[task.id] = expected.at(task.id);
            ++leaf_seen;
            continue;
        }
        Check(index < header.node_count, "GPU child index");
        VelRtBvh4Node node;
        std::memcpy(&node, bytes + layout.nodes + uint64_t(index) * sizeof(node), sizeof(node));
        if (task.finish) {
            Bounds combined = computed.at(node.child_id[0]);
            for (uint32_t i = 0; i < node.child_count; ++i) {
                const auto b = computed.at(node.child_id[i]);
                Check(Contains(node.bounds_min[i], node.bounds_max[i], b), "GPU child bounds");
                combined = Unite(combined, b);
            }
            computed[task.id] = combined;
            continue;
        }
        Check(visited.insert(task.id).second, "GPU internal cycle/duplicate");
        Check(node.parent_id == task.parent && node.child_count >= 2 && node.child_count <= 4, "GPU parent/arity");
        for (uint32_t i = node.child_count; i < 4; ++i) Check(node.child_id[i] == VEL_RT_BVH4_INVALID_ID, "GPU unused child");
        stack.push_back({task.id, task.parent, true});
        for (uint32_t i = 0; i < node.child_count; ++i) stack.push_back({node.child_id[i], task.id, false});
        ++internal_seen;
    }
    Check(internal_seen == internals && leaf_seen == leaves, "GPU unreachable nodes or missing leaves");
    if (!tree.empty()) Check(Contains(header.bounds_min, header.bounds_max, computed.at(header.root_id)), "GPU root bounds");
    ctx.Free(output);
    ctx.Free(mapping);
    ctx.Free(source);
}

static uint64_t RecordKey(uint32_t geometry, uint32_t primitive) { return (uint64_t(geometry) << 32) | primitive; }
static void RunMesa(Context& ctx, const mesa_test::Fixture& fixture, const std::vector<uint32_t>& flags,
                    const std::vector<uint64_t>& runtime_blas = {}, uint32_t expected_error = 0,
                    const std::function<void(VkCommandBuffer, uint64_t)>& producer = {}) {
    const auto plan = PlanMesaGpuLayout(fixture.capacity, fixture.type);
    const uint64_t mapping_bytes = MesaGpuMappingWords(fixture.capacity) * 4;
    auto source = ctx.Allocate(std::max(size_t(4), fixture.bytes.size()));
    auto mapping = ctx.Allocate(mapping_bytes + 256);
    auto output = ctx.Allocate(plan.size + 256);
    auto flag_buffer = ctx.Allocate(std::max(size_t(4), flags.size() * 4));
    auto blas_buffer = ctx.Allocate(std::max(size_t(8), runtime_blas.size() * 8));
    auto status = ctx.Allocate(260);
    if (!producer && !fixture.bytes.empty()) std::memcpy(source.mapped, fixture.bytes.data(), fixture.bytes.size());
    if (!flags.empty()) std::memcpy(flag_buffer.mapped, flags.data(), flags.size() * 4);
    if (!runtime_blas.empty()) {
        Check(runtime_blas.size() == fixture.capacity, "runtime BLAS table capacity");
        std::memcpy(blas_buffer.mapped, runtime_blas.data(), runtime_blas.size() * 8);
    }
    std::memset(status.mapped + 128, 0, 4);
    VelRtBvh4Header header{};
    header.magic = VEL_RT_BVH4_MAGIC;
    header.version = VEL_RT_BVH4_ABI_VERSION;
    header.type = fixture.type == 2 ? 0 : 1;
    header.node_offset = plan.nodes;
    header.triangle_offset = plan.triangles;
    header.aabb_offset = plan.aabbs;
    header.instance_offset = plan.instances;
    header.leaf_parent_offset = plan.leaf_parents;
    header.total_size = plan.size;
    std::memcpy(output.mapped + 128, &header, sizeof(header));
    MesaGpuEncodeArgs args{source.address,
                           fixture.bytes.size(),
                           output.address + 128,
                           mapping.address + 128,
                           flags.empty() ? 0 : flag_buffer.address,
                           runtime_blas.empty() ? 0 : blas_buffer.address,
                           status.address + 128,
                           0,
                           fixture.capacity,
                           fixture.type,
                           uint32_t(flags.size()),
                           0};
    ctx.EncodeMesa(args, producer
                             ? std::function<void(VkCommandBuffer)>([&](VkCommandBuffer cmd) { producer(cmd, source.address); })
                             : std::function<void(VkCommandBuffer)>());
    uint32_t error;
    std::memcpy(&error, status.mapped + 128, 4);
    if (expected_error ? (error & expected_error) == 0 : error != 0)
        std::cerr << "Mesa GPU fixture type=" << fixture.type << " capacity=" << fixture.capacity << " active=" << fixture.active
                  << " status=" << error << " expected=" << expected_error << '\n';
    Check(expected_error ? (error & expected_error) != 0 : error == 0, "Mesa GPU status mismatch");
    Check(producer || fixture.bytes.empty() || std::memcmp(source.mapped, fixture.bytes.data(), fixture.bytes.size()) == 0,
          "Mesa source input mutated");
    for (size_t i = 0; i < 128; ++i) {
        Check(output.mapped[i] == 0xa5 && output.mapped[size_t(plan.size) + 128 + i] == 0xa5, "Mesa AS guard overwritten");
        Check(mapping.mapped[i] == 0xa5 && mapping.mapped[size_t(mapping_bytes) + 128 + i] == 0xa5, "Mesa map guard overwritten");
        Check(status.mapped[i] == 0xa5 && status.mapped[132 + i] == 0xa5, "Mesa status guard overwritten");
    }
    if (!expected_error) {
        // Independent host adapter is only the test oracle. No adapted tree,
        // active count, record or map is ever uploaded to the GPU encoder.
        const auto oracle = AdaptMesaContainerV2(fixture.Span(), flags, [&](uint64_t source_address) {
            for (size_t slot = 0; slot < runtime_blas.size(); ++slot) {
                uint64_t address = 0;
                std::memcpy(&address, fixture.bytes.data() + fixture.Leaf(uint32_t(slot)) + 24, 8);
                if (address == source_address) return runtime_blas[slot];
            }
            return uint64_t(0);
        });
        std::memcpy(&header, output.mapped + 128, sizeof(header));
        Check(header.magic == VEL_RT_BVH4_MAGIC && header.version == VEL_RT_BVH4_ABI_VERSION && header.total_size == plan.size &&
                  header.node_offset == plan.nodes && header.triangle_offset == plan.triangles &&
                  header.aabb_offset == plan.aabbs && header.instance_offset == plan.instances &&
                  header.leaf_parent_offset == plan.leaf_parents,
              "Mesa output layout corruption");
        Check(header.node_count == (fixture.active > 1 ? fixture.active - 1 : 0) &&
                  header.triangle_count == oracle.triangles.size() && header.aabb_count == oracle.aabbs.size() &&
                  header.instance_count == oracle.instances.size(),
              "Mesa GPU compact counts");
        struct Expected {
            Bounds bounds;
            std::vector<uint8_t> record;
        };
        std::unordered_map<uint64_t, Expected> expected;
        for (const auto& input : oracle.tree) {
            if (input.leaf_id == VEL_RT_BVH4_INVALID_ID) continue;
            const uint32_t index = input.leaf_id & VEL_RT_BVH4_INDEX_MASK;
            const uint8_t* record;
            size_t size;
            uint64_t key;
            if (fixture.type == 0) {
                const auto& r = oracle.triangles[index];
                record = reinterpret_cast<const uint8_t*>(&r);
                size = sizeof(r);
                key = RecordKey(r.geometry_id, r.primitive_id);
            } else if (fixture.type == 1) {
                const auto& r = oracle.aabbs[index];
                record = reinterpret_cast<const uint8_t*>(&r);
                size = sizeof(r);
                key = RecordKey(r.geometry_id, r.primitive_id);
            } else {
                const auto& r = oracle.instances[index];
                record = reinterpret_cast<const uint8_t*>(&r);
                size = sizeof(r);
                key = r.instance_id;
            }
            Check(expected.emplace(key, Expected{input.bounds, std::vector<uint8_t>(record, record + size)}).second,
                  "test has duplicate application IDs");
        }
        const auto* bytes = output.mapped + 128;
        struct Task {
            uint32_t id, parent;
            bool finish;
        };
        std::vector<Task> stack;
        if (fixture.active)
            stack.push_back({header.root_id, VEL_RT_BVH4_INVALID_ID, false});
        else
            Check(header.root_id == VEL_RT_BVH4_INVALID_ID, "Mesa empty root");
        std::set<uint32_t> visited;
        std::set<uint64_t> record_ids;
        std::unordered_map<uint32_t, Bounds> computed;
        size_t internals = 0, leaves = 0;
        while (!stack.empty()) {
            const auto task = stack.back();
            stack.pop_back();
            const uint32_t kind = task.id >> 30, index = task.id & VEL_RT_BVH4_INDEX_MASK;
            if (kind) {
                Check(kind == fixture.type + 1 && index < fixture.active && visited.insert(task.id).second,
                      "Mesa dense leaf ID/type/duplicate");
                const uint64_t offset = fixture.type == 0   ? header.triangle_offset
                                        : fixture.type == 1 ? header.aabb_offset
                                                            : header.instance_offset;
                const auto* record = bytes + offset + uint64_t(index) * (fixture.type == 2 ? 128 : 48);
                uint32_t primitive, geometry = 0;
                std::memcpy(&primitive, record + (fixture.type == 0 ? 36 : fixture.type == 1 ? 24 : 112), 4);
                if (fixture.type < 2) std::memcpy(&geometry, record + (fixture.type == 0 ? 40 : 28), 4);
                const auto key = fixture.type < 2 ? RecordKey(geometry, primitive) : uint64_t(primitive);
                Check(expected.count(key) && record_ids.insert(key).second, "Mesa original IDs/record coverage");
                const auto& e = expected.at(key);
                if (fixture.type < 2)
                    Check(std::memcmp(record, e.record.data(), 48) == 0, "Mesa vertex/AABB/full-flag record mismatch");
                else {
                    Check(
                        std::memcmp(record, e.record.data(), 64) == 0 && std::memcmp(record + 112, e.record.data() + 112, 16) == 0,
                        "Mesa instance address/flags/transform/ID mismatch");
                    for (size_t component = 0; component < 12; ++component) {
                        float a, b;
                        std::memcpy(&a, record + 64 + component * 4, 4);
                        std::memcpy(&b, e.record.data() + 64 + component * 4, 4);
                        Check(std::isfinite(a) && std::abs(a - b) <= 1e-5f * std::max(1.0f, std::abs(b)),
                              "Mesa inverse transform mismatch");
                    }
                }
                uint32_t parent;
                std::memcpy(&parent, bytes + header.leaf_parent_offset + uint64_t(index) * 4, 4);
                Check(parent == task.parent, "Mesa leaf parent mismatch");
                computed[task.id] = e.bounds;
                ++leaves;
                continue;
            }
            Check(index < header.node_count, "Mesa internal ID");
            VelRtBvh4Node node;
            std::memcpy(&node, bytes + header.node_offset + uint64_t(index) * sizeof(node), sizeof(node));
            if (task.finish) {
                Bounds bounds = computed.at(node.child_id[0]);
                for (uint32_t child = 0; child < node.child_count; ++child) {
                    const auto& b = computed.at(node.child_id[child]);
                    Check(Contains(node.bounds_min[child], node.bounds_max[child], b), "Mesa conservative child bounds");
                    bounds = Unite(bounds, b);
                }
                computed[task.id] = bounds;
                continue;
            }
            Check(visited.insert(task.id).second && node.child_count == 2 && node.parent_id == task.parent,
                  "Mesa graph arity/parent/cycle");
            Check(node.child_id[2] == VEL_RT_BVH4_INVALID_ID && node.child_id[3] == VEL_RT_BVH4_INVALID_ID,
                  "Mesa unused child IDs");
            stack.push_back({task.id, task.parent, true});
            stack.push_back({node.child_id[0], task.id, false});
            stack.push_back({node.child_id[1], task.id, false});
            ++internals;
        }
        Check(internals == header.node_count && leaves == fixture.active && record_ids.size() == expected.size(),
              "Mesa hierarchy/record coverage");
        if (fixture.active) Check(Contains(header.bounds_min, header.bounds_max, computed.at(header.root_id)), "Mesa root bounds");
    }
    ctx.Free(status);
    ctx.Free(blas_buffer);
    ctx.Free(flag_buffer);
    ctx.Free(output);
    ctx.Free(mapping);
    ctx.Free(source);
}

static void MesaCases(Context& ctx) {
    for (uint32_t type = 0; type < 3; ++type)
        for (uint32_t capacity : {0u, 7u}) {
            mesa_test::Fixture empty(capacity, 0, type);
            empty.Empty();
            RunMesa(ctx, empty, {});
        }
    const auto sparse = mesa_test::SparseTriangles();
    const auto flags = mesa_test::SparseTriangleFlags();
    RunMesa(ctx, sparse, flags);
    mesa_test::Fixture singleton(4, 1, 0);
    singleton.Triangle(3, -2, 510, 0x80000000u);
    singleton.InternalBox(0, singleton.LeafId(3), 0xffffffffu, {{-2, 0, 0}, {-1, 1, 1}});
    RunMesa(ctx, singleton, {1});
    constexpr uint32_t count = 4096;
    mesa_test::Fixture deep(count * 2, count, 1);
    for (uint32_t i = 0; i < count; ++i) deep.Aabb(i * 2, {{float(i), 0, 0}, {float(i + 1), 1, 1}}, 100000 - i, i % 2);
    deep.InternalBox(0, deep.LeafId(0), deep.LeafId(2), {{0, 0, 0}, {2, 1, 1}});
    for (uint32_t i = 1; i < count - 1; ++i)
        deep.InternalBox(i, deep.InternalId(i - 1), deep.LeafId((i + 1) * 2), {{0, 0, 0}, {float(i + 2), 1, 1}});
    RunMesa(ctx, deep, {0, 2});
    const float transform[12] = {0, -2, 0, 4, 4, 1, 0, -8, 0, 0, 0.5f, 2};
    mesa_test::Fixture instances(6, 3, 2);
    std::vector<uint64_t> blas(6);
    for (uint32_t i = 0; i < 3; ++i) {
        instances.Instance(i * 2, 0x123456780000ULL + uint64_t(i) * 0x10000, 313 + i, transform);
        blas[i * 2] = 0xfedcba980000ULL + uint64_t(i) * 0x10000;
    }
    instances.InternalBox(0, instances.LeafId(0), instances.LeafId(2), {{-8, -8, -8}, {8, 8, 8}});
    instances.InternalBox(1, instances.InternalId(0), instances.LeafId(4), {{-8, -8, -8}, {8, 8, 8}});
    RunMesa(ctx, instances, {}, blas);
    auto bad = sparse;
    mesa_test::U32(bad.bytes, 112, 3);
    RunMesa(ctx, bad, flags, {}, MesaGpuMetadata);
    bad = sparse;
    mesa_test::U32(bad.bytes, bad.Internal(1) + 24, 0xfffffffdu);
    RunMesa(ctx, bad, flags, {}, MesaGpuReference);
    bad = sparse;
    mesa_test::U32(bad.bytes, bad.Internal(1) + 28, bad.LeafId(0));
    RunMesa(ctx, bad, flags, {}, MesaGpuDuplicate);
    bad = sparse;
    bad.bytes.resize(128);
    RunMesa(ctx, bad, flags, {}, MesaGpuMetadata);
    RunMesa(ctx, sparse, {0, 2, 2}, {}, MesaGpuGeometryFlags);
    RunMesa(ctx, instances, {}, {}, MesaGpuBlasAddress);
    blas[0] = 0;
    RunMesa(ctx, instances, {}, blas, MesaGpuBlasAddress);
    blas[0] = 0xfedcba980000ULL;
    mesa_test::F32(instances.bytes, instances.Leaf(0) + 40, std::numeric_limits<float>::infinity());
    RunMesa(ctx, instances, {}, blas, MesaGpuTransform);
}

static void MesaBuilderCases(Context& ctx) {
    for (uint32_t type = 0; type < 2; ++type)
        for (uint32_t capacity : {0u, 1u, 321u, 1025u}) {
            uint32_t live = 0;
            for (uint32_t i = 0; i < capacity; ++i)
                if (i % 7 != 6) ++live;
            mesa_test::Fixture fixture(capacity, live, type);
            const uint32_t components = type == 0 ? 9 : 6;
            auto vertices = ctx.Allocate(std::max(uint64_t(4), uint64_t(capacity) * components * 4));
            const uint32_t geometries_count = capacity > 1 ? 2 : 1;
            const uint32_t split = capacity / geometries_count;
            std::vector<uint32_t> flags(geometries_count, 2);
            flags[0] = 3;
            std::vector<uint32_t> live_slots;
            for (uint32_t i = 0; i < capacity; ++i) {
                float x = float((i * 37) % 991);
                const uint32_t geometry = geometries_count == 2 && i >= split ? 1 : 0;
                const uint32_t primitive = i - (geometry ? split : 0);
                float* data = reinterpret_cast<float*>(vertices.mapped) + size_t(i) * components;
                if (type == 0) {
                    const float triangle[] = {x, 0, 0, x + 1, 0, 0, x, 1, 1};
                    std::memcpy(data, triangle, sizeof(triangle));
                } else {
                    const float aabb[] = {x, -1, -2, x + 1, 1, 2};
                    std::memcpy(data, aabb, sizeof(aabb));
                }
                if (i % 7 == 6) {
                    data[0] = std::numeric_limits<float>::quiet_NaN();
                    continue;
                }
                const uint32_t packed = geometry | ((flags[geometry] & 1) ? 0x80000000u : 0);
                if (type == 0)
                    fixture.Triangle(i, x, primitive, packed);
                else
                    fixture.Aabb(i, {{x, -1, -2}, {x + 1, 1, 2}}, primitive, packed);
                live_slots.push_back(i);
            }
            auto bounds = [&](uint32_t slot) {
                float x = float((slot * 37) % 991);
                return type == 0 ? Bounds{{x, 0, 0}, {x + 1, 1, 1}} : Bounds{{x, -1, -2}, {x + 1, 1, 2}};
            };
            if (!live)
                fixture.Empty();
            else if (live == 1)
                fixture.InternalBox(0, fixture.LeafId(live_slots[0]), 0xffffffffu, bounds(live_slots[0]));
            else {
                Bounds aggregate = Unite(bounds(live_slots[0]), bounds(live_slots[1]));
                fixture.InternalBox(0, fixture.LeafId(live_slots[0]), fixture.LeafId(live_slots[1]), aggregate);
                for (uint32_t box = 1; box < live - 1; ++box) {
                    aggregate = Unite(aggregate, bounds(live_slots[box + 1]));
                    fixture.InternalBox(box, fixture.InternalId(box - 1), fixture.LeafId(live_slots[box + 1]), aggregate);
                }
            }
            VkAccelerationStructureGeometryKHR geometries[2]{};
            VkAccelerationStructureBuildRangeInfoKHR ranges[2]{};
            for (uint32_t geometry = 0; geometry < geometries_count; ++geometry) {
                const uint32_t first = geometry ? split : 0;
                const uint32_t count = geometry + 1 == geometries_count ? capacity - first : split;
                geometries[geometry] = VkAccelerationStructureGeometryKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
                geometries[geometry].geometryType = VkGeometryTypeKHR(type);
                geometries[geometry].flags = flags[geometry];
                if (type == 0) {
                    auto& triangle = geometries[geometry].geometry.triangles;
                    triangle = VkAccelerationStructureGeometryTrianglesDataKHR{
                        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
                    triangle.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
                    triangle.vertexStride = 12;
                    triangle.vertexData.deviceAddress = vertices.address + uint64_t(first) * components * 4;
                    triangle.maxVertex = count ? count * 3 - 1 : 0;
                    triangle.indexType = VK_INDEX_TYPE_NONE_KHR;
                } else {
                    auto& aabb = geometries[geometry].geometry.aabbs;
                    aabb = VkAccelerationStructureGeometryAabbsDataKHR{
                        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR};
                    aabb.data.deviceAddress = vertices.address + uint64_t(first) * components * 4;
                    aabb.stride = 24;
                }
                ranges[geometry].primitiveCount = count;
            }
            VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            info.geometryCount = geometries_count;
            info.pGeometries = geometries;
            const auto layout = bvh_ir_build_layout(&info, nullptr, ranges);
            auto scratch = ctx.Allocate(layout.scratch_size);
            fixture.bytes.resize(std::max(fixture.bytes.size(), size_t(layout.as_size)), 0xcd);
            RunMesa(ctx, fixture, flags, {}, 0,
                    [&](VkCommandBuffer cmd, uint64_t output) { ctx.RecordBuilder(cmd, info, ranges, output, scratch.address); });
            ctx.Free(scratch);
            ctx.Free(vertices);
        }
}

int main(int argc, char** argv) {
    try {
        const bool builder = argc == 5 && std::strcmp(argv[3], "--mesa-builder") == 0;
        Check(argc == 3 || builder,
              "usage: vel_rt_bvh4_gpu_tests <bvh4_encode.spv> <bvh4_mesa_encode.spv> [--mesa-builder <shader-dir>]");
        Context ctx;
        if (!ctx.Init(argv[1], std::getenv("VEL_RT_BVH4_VALIDATION") != nullptr, builder)) {
            std::cout
                << (builder ? "SKIP: Mesa builder compute/int8/int16/storage/memory-model/float-control prerequisites unavailable\n"
                            : "SKIP: Vulkan 1.2 compute/BDA/scalar/int64 device unavailable\n");
            return 77;
        }
        ctx.InitMesa(argv[2]);
        if (builder) {
            ctx.InitBuilder(argv[4]);
            MesaBuilderCases(ctx);
            Check(validation_errors == 0, "Vulkan validation errors");
            std::cout << "Mesa GPU BUILD -> shared BVH4: 8 triangle/AABB scenes, sparse and multigeometry inputs, "
                         "same-command-buffer encoding passed\n";
            return 0;
        }
        Run(ctx, {}, VEL_RT_BVH4_INVALID_ID);
        for (uint32_t count : {1u, 2u, 3u, 4u, 17u, 129u, 1025u}) {
            for (uint32_t seed = 0; seed < 4; ++seed) {
                uint32_t root;
                auto tree = Tree(count, seed, false, root);
                Run(ctx, tree, root);
            }
        }
        uint32_t root;
        auto deep = Tree(8192, 23, true, root);
        Run(ctx, deep, root);
        const auto mesa_fixture = mesa_test::SparseTriangles();
        const auto adapted = AdaptMesaContainerV2(mesa_fixture.Span(), mesa_test::SparseTriangleFlags());
        Run(ctx, adapted.tree, adapted.root);
        MesaCases(ctx);
        Check(validation_errors == 0, "Vulkan validation errors");
        std::cout << "BVH4 GPU encode: empty, 28 shuffled trees, deep tree, Mesa adapter input, chunks, bounds, parents and guards "
                     "passed; direct Mesa GPU triangle/AABB/TLAS records, sparse/empty/deep graphs and rejection checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
