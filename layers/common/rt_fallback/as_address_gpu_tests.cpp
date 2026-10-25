// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void Check(VkResult result) { Require(result == VK_SUCCESS, "Vulkan operation failed"); }
std::atomic_uint validation_errors;
VKAPI_ATTR VkBool32 VKAPI_CALL Message(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                       const VkDebugUtilsMessengerCallbackDataEXT* message, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++validation_errors;
        std::cerr << message->pMessage << '\n';
    }
    return VK_FALSE;
}

struct GPU {
    VkInstance instance{};
    VkDebugUtilsMessengerEXT messenger{};
    VkDevice device{};
    VkQueue queue{};
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkCommandPool pool{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    uint8_t* mapped = nullptr;
    uint64_t address = 0, cursor = 0;
    static constexpr uint64_t bytes = 4 * 1024 * 1024;
    ~GPU() {
        if (device) {
            vkDeviceWaitIdle(device);
            if (mapped) vkUnmapMemory(device, memory);
            vkDestroyPipeline(device, pipeline, nullptr);
            vkDestroyPipelineLayout(device, layout, nullptr);
            vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyBuffer(device, buffer, nullptr);
            vkFreeMemory(device, memory, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (messenger) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            destroy(instance, messenger, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
    bool Init(const char* path, bool validation) {
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.apiVersion = VK_API_VERSION_1_2;
        const char* layer = "VK_LAYER_KHRONOS_validation";
        const char* extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &application;
        if (validation) {
            create.enabledLayerCount = create.enabledExtensionCount = 1;
            create.ppEnabledLayerNames = &layer;
            create.ppEnabledExtensionNames = &extension;
        }
        auto result = vkCreateInstance(&create, nullptr, &instance);
        if (result == VK_ERROR_INCOMPATIBLE_DRIVER) return false;
        Check(result);
        if (validation) {
            auto make = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            Require(make != nullptr, "Validation messenger unavailable");
            VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
            info.pfnUserCallback = Message;
            Check(make(instance, &info, nullptr, &messenger));
        }
        uint32_t count = 0, family = 0;
        Check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        VkPhysicalDevice physical{};
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_2) continue;
            VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12};
            vkGetPhysicalDeviceFeatures2(candidate, &features);
            if (!v12.bufferDeviceAddress || !v12.scalarBlockLayout || !features.features.shaderInt64) continue;
            uint32_t queue_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, nullptr);
            std::vector<VkQueueFamilyProperties> queues(queue_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queue_count, queues.data());
            for (family = 0; family < queue_count && !(queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT); ++family) {
            }
            if (family == queue_count) continue;
            physical = candidate;
            std::cout << "AS resolver component device: " << properties.deviceName << '\n';
            break;
        }
        if (!physical) return false;
        float priority = 1;
        VkDeviceQueueCreateInfo queues{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queues.queueFamilyIndex = family;
        queues.queueCount = 1;
        queues.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan12Features enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        enabled.bufferDeviceAddress = enabled.scalarBlockLayout = VK_TRUE;
        VkPhysicalDeviceFeatures features{};
        features.shaderInt64 = VK_TRUE;
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enabled};
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queues;
        device_info.pEnabledFeatures = &features;
        // Ordinary compute and BDA only: no native RT feature or descriptor.
        Check(vkCreateDevice(physical, &device_info, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = bytes;
        buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        Check(vkCreateBuffer(device, &buffer_info, nullptr, &buffer));
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        VkPhysicalDeviceMemoryProperties properties;
        vkGetPhysicalDeviceMemoryProperties(physical, &properties);
        uint32_t type = 0;
        for (; type < properties.memoryTypeCount; ++type)
            if ((requirements.memoryTypeBits & (1u << type)) &&
                (properties.memoryTypes[type].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                break;
        if (type == properties.memoryTypeCount) return false;
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        Check(vkAllocateMemory(device, &allocation, nullptr, &memory));
        Check(vkBindBufferMemory(device, buffer, memory, 0));
        void* mapping = nullptr;
        Check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapping));
        mapped = static_cast<uint8_t*>(mapping);
        std::memset(mapped, 0, size_t(bytes));
        VkBufferDeviceAddressInfo bda{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        bda.buffer = buffer;
        address = vkGetBufferDeviceAddress(device, &bda);
        Require(address != 0, "Arena BDA unavailable");
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.queueFamilyIndex = family;
        Check(vkCreateCommandPool(device, &pool_info, nullptr, &pool));
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &range;
        Check(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout));
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        Require(file.good(), "Resolver shader unavailable");
        auto length = file.tellg();
        Require(length > 0 && length % 4 == 0, "Resolver shader size");
        std::vector<uint32_t> code(size_t(length) / 4);
        file.seekg(0);
        Require(bool(file.read(reinterpret_cast<char*>(code.data()), length)), "Resolver shader read");
        VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_info.codeSize = size_t(length);
        module_info.pCode = code.data();
        VkShaderModule module;
        Check(vkCreateShaderModule(device, &module_info, nullptr, &module));
        VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_info.layout = layout;
        pipeline_info.stage = VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_info.stage.module = module;
        pipeline_info.stage.pName = "main";
        result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        Check(result);
        return true;
    }
    uint64_t Allocate(uint64_t size, uint64_t alignment = 8) {
        uint64_t begin = (address + cursor + alignment - 1) & ~(alignment - 1);
        Require(begin >= address && begin - address <= bytes && size <= bytes - (begin - address), "Test arena exhausted");
        cursor = begin - address + size;
        return begin;
    }
    template <class T>
    void Write(uint64_t pointer, const T& value) {
        std::memcpy(mapped + (pointer - address), &value, sizeof(value));
    }
    template <class T>
    T Read(uint64_t pointer) const {
        T result;
        std::memcpy(&result, mapped + (pointer - address), sizeof(result));
        return result;
    }
    void Dispatch(uint64_t queries, uint64_t results, uint32_t count) {
        struct Args {
            uint64_t queries, results;
            uint32_t count, reserved;
        } args{queries, results, count, 0};
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer command;
        Check(vkAllocateCommandBuffers(device, &allocate, &command));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        Check(vkBeginCommandBuffer(command, &begin));
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(args), &args);
        vkCmdDispatch(command, (count + 63) / 64, 1, 1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr,
                             0, nullptr);
        Check(vkEndCommandBuffer(command));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        Check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        Check(vkQueueWaitIdle(queue));
        vkFreeCommandBuffers(device, pool, 1, &command);
    }
};

struct Header {
    uint32_t magic = 0x52534156, version = 1, stride = 3, radix = 8, levels = 7, shift = 8, reserved0[2]{};
    uint64_t root = 0, reserved1[3]{};
};
static_assert(sizeof(Header) == 64 && offsetof(Header, root) == 32);
struct Query {
    uint64_t context, value;
};
struct Result {
    uint64_t physical;
    uint32_t marker, reserved;
};
struct Radix {
    GPU& gpu;
    Header header;
    uint64_t context;
    std::map<uint64_t, uint64_t> oracle;
    explicit Radix(GPU& value) : gpu(value), context(gpu.Allocate(sizeof(Header))) {
        header.root = gpu.Allocate(2048);
        gpu.Write(context, header);
    }
    void Add(uint64_t value, uint64_t physical) {
        Require(value && !(value & 255), "Fixture public alignment");
        auto known = oracle.emplace(value, physical);
        Require(known.second || known.first->second == physical, "Fixture alias changed its physical root");
        uint64_t page = header.root, key = value >> 8;
        for (uint32_t level = 7; level--;) {
            uint64_t slot = (key >> (level * 8)) & 255;
            uint64_t entry = page + slot * 8;
            auto existing = gpu.Read<uint64_t>(entry);
            if (!level) {
                if (!existing)
                    gpu.Write(entry, physical);
                else
                    Require(existing == physical, "Fixture attempted a live mapping rewrite");
            } else {
                if (!existing) {
                    existing = gpu.Allocate(2048);
                    gpu.Write(entry, existing);
                }
                page = existing;
            }
        }
    }
    uint64_t Clone(const Header& copy) {
        auto target = gpu.Allocate(sizeof(Header));
        gpu.Write(target, copy);
        return target;
    }
};
}  // namespace

int main(int argc, char** argv) {
    try {
        Require(argc == 2 || (argc == 3 && std::strcmp(argv[2], "--validation") == 0),
                "Usage: resolver_test shader.spv [--validation]");
        GPU gpu;
        if (!gpu.Init(argv[1], argc == 3)) return 77;
        Radix map(gpu);
        auto physical16 = gpu.Allocate(272, 256) + 16;
        auto physical256 = gpu.Allocate(16, 256);
        auto physical4 = gpu.Allocate(32, 16) + 4;
        Require((physical16 & 255) == 16 && (physical4 & 15) == 4, "Physical/public alignment distinction not exercised");
        gpu.Write(physical16, uint32_t(0x12345678));
        gpu.Write(physical256, uint32_t(0x89abcdef));
        gpu.Write(physical4, uint32_t(0x55667788));
        map.Add(0x0102030405060700, physical16);
        map.Add(0xff02030405060700, physical256);
        map.Add(0xffffffffffffff00, physical4);
        map.Add(0x100, physical256);
        map.Add(0x0102030405060700, physical16);  // Two alias handles, one address.
        map.Add(0x0102030405060800, physical16);  // Different keys may alias storage.
        auto verify = [&](const std::vector<Query>& queries) {
            auto input = gpu.Allocate(queries.size() * sizeof(Query));
            auto output = gpu.Allocate((queries.size() + 1) * sizeof(Result));
            for (size_t i = 0; i < queries.size(); ++i) gpu.Write(input + i * sizeof(Query), queries[i]);
            const Result guard{0xdeadbeef01234567, 0x10203040, 0x55667788};
            gpu.Write(output + queries.size() * sizeof(Result), guard);
            gpu.Dispatch(input, output, uint32_t(queries.size()));
            for (size_t i = 0; i < queries.size(); ++i) {
                uint64_t expected = 0;
                auto found = map.oracle.find(queries[i].value);
                if (queries[i].context == map.context && found != map.oracle.end()) expected = found->second;
                auto result = gpu.Read<Result>(output + i * sizeof(Result));
                Require(result.physical == expected, "GPU resolver returned wrong physical header");
                Require(result.marker == (expected ? gpu.Read<uint32_t>(expected) : 0),
                        "Resolved BDA did not reach original header");
                Require(result.reserved == 0, "GPU resolver result record incomplete");
            }
            auto after = gpu.Read<Result>(output + queries.size() * sizeof(Result));
            Require(std::memcmp(&after, &guard, sizeof(guard)) == 0, "Padded dispatch exceeded query count");
        };
        std::vector<Query> queries;
        for (auto [value, physical] : map.oracle) {
            (void)physical;
            queries.push_back({map.context, value});
        }
        queries.push_back({map.context, 0});
        queries.push_back({map.context, 0x0102030405060701});
        queries.push_back({map.context, 0x0102030405060900});  // Missing leaf.
        queries.push_back({map.context, 0x0202030405060700});  // Missing root branch.
        queries.push_back({0, 0x100});
        queries.push_back({map.context + 1, 0x100});
        for (uint32_t field = 0; field < 6; ++field) {
            auto bad = map.header;
            auto member = reinterpret_cast<uint8_t*>(&bad) + field * 4;
            uint32_t bits;
            std::memcpy(&bits, member, sizeof(bits));
            bits ^= 1;
            std::memcpy(member, &bits, sizeof(bits));
            queries.push_back({map.Clone(bad), 0x100});
        }
        auto no_root = map.header;
        no_root.root = 0;
        queries.push_back({map.Clone(no_root), 0x100});
        auto bad_root = map.header;
        bad_root.root += 2;
        queries.push_back({map.Clone(bad_root), 0x100});
        verify(queries);
        // Append a sibling sharing six directories, then retain every earlier
        // lookup and test the new leaf. No dense allocation follows key size.
        map.Add(0x0102030405060900, physical4);
        queries.push_back({map.context, 0x0102030405060900});
        verify(queries);
        Require(gpu.cursor < 128 * 1024, "Sparse high-address mapping became dense");
        Require(validation_errors == 0, "Khronos validation error");
        std::cout << "GPU AS resolver PASS: full56-bit keys, sparse pages, aliases, invalid headers/misses, physical4/16/256 "
                     "alignment, append stability\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
