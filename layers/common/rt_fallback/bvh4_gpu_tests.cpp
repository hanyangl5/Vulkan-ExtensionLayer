// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "bvh4_gpu.h"
#include "mesa_ir_test_fixture.h"
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
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
    VkPhysicalDeviceMemoryProperties memory{};
    uint32_t queue_family = 0;

    ~Context() {
        if (device) {
            for (auto p : pipeline) vkDestroyPipeline(device, p, nullptr);
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
    bool Init(const char* path, bool validation) {
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
            VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &f12;
            vkGetPhysicalDeviceFeatures2(candidate, &features);
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_2 || !f12.bufferDeviceAddress || !f12.scalarBlockLayout ||
                !features.features.shaderInt64 || properties.limits.maxPushConstantsSize < sizeof(GpuEncodeArgs))
                continue;
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
        VkPhysicalDeviceFeatures features{};
        features.shaderInt64 = VK_TRUE;
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

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "usage: vel_rt_bvh4_gpu_tests <bvh4_encode.spv>");
        Context ctx;
        if (!ctx.Init(argv[1], std::getenv("VEL_RT_BVH4_VALIDATION") != nullptr)) {
            std::cout << "SKIP: Vulkan 1.2 compute/BDA/scalar/int64 device unavailable\n";
            return 77;
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
        Check(validation_errors == 0, "Vulkan validation errors");
        std::cout << "BVH4 GPU encode: empty, 28 shuffled trees, deep tree, Mesa adapter input, chunks, bounds, parents and guards "
                     "passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
