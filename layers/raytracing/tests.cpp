// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "compiler.h"
#include "continuation_runtime.h"
#include "runtime.h"
#include "runtime_test_shader.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

static void Require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
static void Check(VkResult r) {
    if (r != VK_SUCCESS) throw std::runtime_error("VkResult=" + std::to_string(r));
}
static const char *kRaygen = R"(
#version 460
#extension GL_EXT_ray_tracing : require
layout(set=0,binding=0) uniform accelerationStructureEXT scene;
layout(set=0,binding=1,std430) buffer Output { vec4 pixels[]; } outputData;
layout(push_constant) uniform Params { uint mask; } params;
layout(location=0) rayPayloadEXT vec4 payload;
void main() {
    vec2 p=(vec2(gl_LaunchIDEXT.xy)+0.5)/vec2(gl_LaunchSizeEXT.xy);
    vec3 o=vec3(p.x*7.0-2.0,p.y*4.0-2.0,-2.0);
    traceRayEXT(scene,gl_RayFlagsOpaqueEXT,params.mask,0,1,0,o,0.01,vec3(0,0,1),100.0,0);
    outputData.pixels[gl_LaunchIDEXT.y*gl_LaunchSizeEXT.x+gl_LaunchIDEXT.x]=payload;
}
)";
static const char *kHit = R"(
#version 460
#extension GL_EXT_ray_tracing : require
layout(location=0) rayPayloadInEXT vec4 payload;
hitAttributeEXT vec2 bary;
void main() { payload=vec4(1.0-bary.x-bary.y,bary.x,bary.y,1.0); }
)";
static const char *kMiss = R"(
#version 460
#extension GL_EXT_ray_tracing : require
layout(location=0) rayPayloadInEXT vec4 payload;
void main() { payload=vec4(0,0,0,0); }
)";
struct Buffer {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    void *mapped{};
    VkDeviceAddress address{};
};
struct Harness {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{};
    VkCommandPool pool{};
    VkCommandBuffer cb{};
    VkPhysicalDeviceMemoryProperties memory{};
    std::vector<Buffer> buffers;
    template <class F>
    F Proc(const char *n) {
        auto f = vkGetDeviceProcAddr(device, n);
        Require(f != nullptr, n);
        return reinterpret_cast<F>(f);
    }
    void Init() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_3;
        const char *layers[] = {"VK_LAYER_HANYANG_ray_tracing_fallback", "VK_LAYER_KHRONOS_validation"};
        VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ic.pApplicationInfo = &app;
        ic.enabledLayerCount = std::getenv("VEL_RT_TEST_VALIDATION") ? 2 : 1;
        ic.ppEnabledLayerNames = layers;
        Check(vkCreateInstance(&ic, nullptr, &instance));
        uint32_t n = 0;
        Check(vkEnumeratePhysicalDevices(instance, &n, nullptr));
        Require(n > 0, "No Vulkan physical device");
        std::vector<VkPhysicalDevice> physicals(n);
        Check(vkEnumeratePhysicalDevices(instance, &n, physicals.data()));
        physical = physicals[0];
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        as.pNext = &rt;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &as};
        vkGetPhysicalDeviceFeatures2(physical, &features);
        Require(as.accelerationStructure && rt.rayTracingPipeline, "Layer did not expose RT features");
        as.accelerationStructure = rt.rayTracingPipeline = VK_TRUE;
        VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &as};
        v13.synchronization2 = VK_TRUE;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, nullptr);
        std::vector<VkQueueFamilyProperties> qp(n);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, qp.data());
        while (family < n && !(qp[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) family++;
        Require(family < n, "No compute queue");
        float priority = 1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        const char *extensions[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
                                    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &v13};
        dc.queueCreateInfoCount = 1;
        dc.pQueueCreateInfos = &qi;
        dc.enabledExtensionCount = 3;
        dc.ppEnabledExtensionNames = extensions;
        Check(vkCreateDevice(physical, &dc, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pc.queueFamilyIndex = family;
        pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        Check(vkCreateCommandPool(device, &pc, nullptr, &pool));
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = pool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        Check(vkAllocateCommandBuffers(device, &ca, &cb));
    }
    Buffer Make(size_t size, VkBufferUsageFlags usage, const void *data = nullptr) {
        Buffer b;
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = size;
        ci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        Check(vkCreateBuffer(device, &ci, nullptr, &b.buffer));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, b.buffer, &req);
        uint32_t type = 0;
        for (; type < memory.memoryTypeCount; type++)
            if ((req.memoryTypeBits & (1u << type)) &&
                (memory.memoryTypes[type].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                break;
        Require(type < memory.memoryTypeCount, "No coherent memory");
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        Check(vkAllocateMemory(device, &ai, nullptr, &b.memory));
        Check(vkBindBufferMemory(device, b.buffer, b.memory, 0));
        Check(vkMapMemory(device, b.memory, 0, size, 0, &b.mapped));
        std::memset(b.mapped, 0, size);
        if (data) std::memcpy(b.mapped, data, size);
        VkBufferDeviceAddressInfo bi{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        bi.buffer = b.buffer;
        b.address = vkGetBufferDeviceAddress(device, &bi);
        buffers.push_back(b);
        return b;
    }
    VkShaderModule Module(const std::vector<uint32_t> &code) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size() * 4;
        ci.pCode = code.data();
        VkShaderModule m;
        Check(vkCreateShaderModule(device, &ci, nullptr, &m));
        return m;
    }
    void Barrier(VkPipelineStageFlags src, VkAccessFlags sa, VkPipelineStageFlags dst, VkAccessFlags da) {
        VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        m.srcAccessMask = sa;
        m.dstAccessMask = da;
        vkCmdPipelineBarrier(cb, src, dst, 0, 1, &m, 0, nullptr, 0, nullptr);
    }
    void Begin() {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        Check(vkBeginCommandBuffer(cb, &bi));
    }
    void Submit(bool sync2 = false) {
        Check(vkEndCommandBuffer(cb));
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore semaphore;
        Check(vkCreateSemaphore(device, &sci, nullptr, &semaphore));
        VkSubmitInfo producer{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        producer.signalSemaphoreCount = 1;
        producer.pSignalSemaphores = &semaphore;
        Check(vkQueueSubmit(queue, 1, &producer, VK_NULL_HANDLE));
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &semaphore;
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        if (sync2) {
            VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            wait.semaphore = semaphore;
            wait.stageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
            VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
            command.commandBuffer = cb;
            VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            submit.waitSemaphoreInfoCount = 1;
            submit.pWaitSemaphoreInfos = &wait;
            submit.commandBufferInfoCount = 1;
            submit.pCommandBufferInfos = &command;
            Check(vkQueueSubmit2(queue, 1, &submit, VK_NULL_HANDLE));
        } else
            Check(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
        Check(vkQueueWaitIdle(queue));
        vkDestroySemaphore(device, semaphore, nullptr);
    }
    ~Harness() {
        if (device) {
            vkDeviceWaitIdle(device);
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            for (auto b : buffers) {
                if (b.mapped) vkUnmapMemory(device, b.memory);
                vkDestroyBuffer(device, b.buffer, nullptr);
                vkFreeMemory(device, b.memory, nullptr);
            }
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
};

static void CheckContinuationRuntime(Harness &t) {
    auto code = vel::rt::CompileGLSL(
        std::string("#version 460\n") + vel::rt::kMemory + vel::rt::kContinuationRuntime + vel::rt::kRuntimeTestShader,
        VK_SHADER_STAGE_COMPUTE_BIT);
    auto module = t.Module(code);
    auto output = t.Make(32 * 16 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VkDeviceAddress)};
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.pushConstantRangeCount = 1;
    ci.pPushConstantRanges = &push;
    VkPipelineLayout layout;
    Check(vkCreatePipelineLayout(t.device, &ci, nullptr, &layout));
    VkComputePipelineCreateInfo pc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pc.layout = layout;
    pc.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pc.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.stage.module = module;
    pc.stage.pName = "main";
    VkPipeline pipeline;
    Check(vkCreateComputePipelines(t.device, VK_NULL_HANDLE, 1, &pc, nullptr, &pipeline));
    t.Begin();
    vkCmdBindPipeline(t.cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdPushConstants(t.cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(output.address), &output.address);
    vkCmdDispatch(t.cb, 1, 1, 1);
    t.Barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
              VK_ACCESS_HOST_READ_BIT);
    t.Submit();
    const auto *values = static_cast<const uint32_t *>(output.mapped);
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
            uint32_t expected = x < 7 && y < 3 ? 65535u : 0u;
            uint32_t actual = values[(y * 8 + x) * 16];
            if (actual != expected)
                throw std::runtime_error("Continuation runtime mismatch at " + std::to_string(x) + "," + std::to_string(y) + ": " +
                                         std::to_string(actual) + "/" + std::to_string(expected));
        }
    }
    vkDestroyPipeline(t.device, pipeline, nullptr);
    vkDestroyPipelineLayout(t.device, layout, nullptr);
    vkDestroyShaderModule(t.device, module, nullptr);
    std::puts("Continuation runtime: stack, pending/committed hit attributes, nested ray save/restore and scheduler passed");
}

int main(int argc, char **argv) {
    try {
        std::string variant = argc > 1 ? argv[1] : "u16";
        bool empty = variant == "empty";
        std::vector<vel::rt::Stage> stages = {
            {VK_SHADER_STAGE_RAYGEN_BIT_KHR, vel::rt::CompileGLSL(kRaygen, VK_SHADER_STAGE_RAYGEN_BIT_KHR)},
            {VK_SHADER_STAGE_MISS_BIT_KHR, vel::rt::CompileGLSL(kMiss, VK_SHADER_STAGE_MISS_BIT_KHR)},
            {VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, vel::rt::CompileGLSL(kHit, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR)}};
        std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups(3);
        for (uint32_t i = 0; i < 3; i++) {
            groups[i] = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR};
            groups[i].type =
                i == 2 ? VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR : VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
            groups[i].generalShader = i < 2 ? i : VK_SHADER_UNUSED_KHR;
            groups[i].closestHitShader = i == 2 ? 2 : VK_SHADER_UNUSED_KHR;
            groups[i].anyHitShader = groups[i].intersectionShader = VK_SHADER_UNUSED_KHR;
        }
        auto linked = vel::rt::Link(stages, groups, 1);
        Require(!linked.spirv.empty(), "Compiler produced no code");
        for (size_t i = 5; i < linked.spirv.size(); i += linked.spirv[i] >> 16) {
            uint32_t op = linked.spirv[i] & 65535;
            Require(op != 4445 && op != 5337, "Native RT instruction survived lowering");
        }
        Require(vel::rt::AccelerationStructureSize(0, false) >= 48, "Empty AS storage");
        // Release SPIR-V commonly has no OpName instructions. IDs must still be
        // namespaced during linking.
        auto stripped = stages;
        for (auto &s : stripped) {
            std::vector<uint32_t> code(s.spirv.begin(), s.spirv.begin() + 5);
            for (size_t i = 5; i < s.spirv.size();) {
                uint32_t n = s.spirv[i] >> 16, op = s.spirv[i] & 65535;
                if (op != 5 && op != 6) code.insert(code.end(), s.spirv.begin() + i, s.spirv.begin() + i + n);
                i += n;
            }
            s.spirv = std::move(code);
        }
        Require(!vel::rt::Link(stripped, groups, 1).spirv.empty(), "Debug-stripped SPIR-V failed");
        bool rejected = false;
        auto bad = stages;
        bad[0].stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
        try {
            vel::rt::Link(bad, groups, 1);
        } catch (...) {
            rejected = true;
        }
        Require(rejected, "Unsupported stage was accepted");
        std::puts("Compiler and rejection checks passed");
        Harness t;
        t.Init();
        if (variant == "runtime") {
            CheckContinuationRuntime(t);
            return 0;
        }
#define PROC(name) auto name = t.Proc<PFN_vk##name>("vk" #name)
        PROC(GetAccelerationStructureBuildSizesKHR);
        PROC(CreateAccelerationStructureKHR);
        PROC(DestroyAccelerationStructureKHR);
        PROC(GetAccelerationStructureDeviceAddressKHR);
        PROC(CmdBuildAccelerationStructuresKHR);
        PROC(CreateRayTracingPipelinesKHR);
        PROC(GetRayTracingShaderGroupHandlesKHR);
        PROC(CmdTraceRaysKHR);
        PROC(CreateDeferredOperationKHR);
        PROC(DestroyDeferredOperationKHR);
        PROC(GetDeferredOperationMaxConcurrencyKHR);
        PROC(GetDeferredOperationResultKHR);
        PROC(DeferredOperationJoinKHR);
        PROC(GetRayTracingShaderGroupStackSizeKHR);
        PROC(CmdSetRayTracingPipelineStackSizeKHR);
#undef PROC
        // Three triangles in one BLAS, instanced twice. Geometry order is
        // deliberately unsorted.
        const float vertices[] = {1, -1, 0, 3, -1, 0, 2, 1, 0, -1, -1, 0, 1, -1, 0, 0, 1, 0, 100, -1, 0, 102, -1, 0, 101, 1, 0};
        const uint16_t indices[] = {65535, 0, 1, 2, 3, 4, 5, 6, 7, 8};
        const uint32_t indices32[] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
        auto vb = t.Make(sizeof(vertices), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, vertices);
        auto ib = variant == "u32"
                      ? t.Make(sizeof(indices32), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, indices32)
                      : t.Make(sizeof(indices), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, indices);
        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto &tri = geometry.geometry.triangles;
        tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tri.vertexData.deviceAddress = vb.address;
        tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        tri.vertexStride = 12;
        tri.maxVertex = 8;
        tri.indexData.deviceAddress = ib.address;
        tri.indexType = VK_INDEX_TYPE_UINT16;
        if (variant == "u32") tri.indexType = VK_INDEX_TYPE_UINT32;
        if (variant == "nonindexed") {
            tri.indexType = VK_INDEX_TYPE_NONE_KHR;
            tri.indexData.deviceAddress = 0;
        }
        VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build.geometryCount = 1;
        build.pGeometries = &geometry;
        uint32_t prims = empty ? 0 : 3;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        GetAccelerationStructureBuildSizesKHR(t.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &prims, &sizes);
        auto blasBuffer = t.Make(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
        auto scratch = t.Make(sizes.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        VkAccelerationStructureCreateInfoKHR ac{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        ac.buffer = blasBuffer.buffer;
        ac.size = sizes.accelerationStructureSize;
        ac.type = build.type;
        VkAccelerationStructureKHR blas;
        Check(CreateAccelerationStructureKHR(t.device, &ac, nullptr, &blas));
        build.dstAccelerationStructure = blas;
        build.scratchData.deviceAddress = scratch.address;
        VkAccelerationStructureBuildRangeInfoKHR range{prims, 0, 0, 0};
        if (variant == "u16" || empty) range.primitiveOffset = 2;
        auto rangePtr = &range;
        t.Begin();
        CmdBuildAccelerationStructuresKHR(t.cb, 1, &build, &rangePtr);
        t.Barrier(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                  VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        VkAccelerationStructureDeviceAddressInfoKHR ba{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        ba.accelerationStructure = blas;
        std::array<VkAccelerationStructureInstanceKHR, 2> inst{};
        for (uint32_t i = 0; i < 2; i++) {
            inst[i].transform.matrix[0][0] = inst[i].transform.matrix[1][1] = inst[i].transform.matrix[2][2] = 1;
            if (i == 1) {
                inst[i].transform.matrix[0][0] = 1.5f;
                inst[i].transform.matrix[1][1] = 0.75f;
            }
            inst[i].transform.matrix[0][3] = float(i) * 2.0f;
            inst[i].mask = 1u << i;
            inst[i].instanceCustomIndex = 10 + i;
            inst[i].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            inst[i].accelerationStructureReference = GetAccelerationStructureDeviceAddressKHR(t.device, &ba);
        }
        auto instances = t.Make(sizeof(inst), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, inst.data());
        VkAccelerationStructureGeometryKHR ig{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        ig.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        ig.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        ig.geometry.instances.data.deviceAddress = instances.address;
        auto tb = build;
        tb.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        tb.pGeometries = &ig;
        prims = 2;
        range.primitiveCount = 2;
        range.primitiveOffset = 0;
        GetAccelerationStructureBuildSizesKHR(t.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tb, &prims, &sizes);
        auto tlasBuffer = t.Make(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
        ac.buffer = tlasBuffer.buffer;
        ac.size = sizes.accelerationStructureSize;
        ac.type = tb.type;
        VkAccelerationStructureKHR tlas;
        Check(CreateAccelerationStructureKHR(t.device, &ac, nullptr, &tlas));
        tb.dstAccelerationStructure = tlas;
        CmdBuildAccelerationStructuresKHR(t.cb, 1, &tb, &rangePtr);
        t.Barrier(VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                  VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        constexpr uint32_t w = 13, h = 9;
        auto output = t.Make(w * h * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        VkDescriptorSetLayoutBinding bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr}};
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount = 2;
        sl.pBindings = bindings;
        VkDescriptorSetLayout setLayout;
        Check(vkCreateDescriptorSetLayout(t.device, &sl, nullptr, &setLayout));
        VkPushConstantRange push{VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, 4};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &setLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        VkPipelineLayout layout;
        Check(vkCreatePipelineLayout(t.device, &pl, nullptr, &layout));
        std::array<VkShaderModule, 3> modules;
        std::array<VkPipelineShaderStageCreateInfo, 3> ss{};
        for (uint32_t i = 0; i < 3; i++) {
            modules[i] = t.Module(stages[i].spirv);
            ss[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            ss[i].stage = stages[i].stage;
            ss[i].module = modules[i];
            ss[i].pName = "main";
        }
        VkRayTracingPipelineCreateInfoKHR pci{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
        pci.stageCount = 3;
        pci.pStages = ss.data();
        pci.groupCount = 3;
        pci.pGroups = groups.data();
        pci.maxPipelineRayRecursionDepth = 1;
        pci.layout = layout;
        VkPipeline rejectedPipeline{};
        pci.maxPipelineRayRecursionDepth = 2;
        Require(CreateRayTracingPipelinesKHR(t.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pci, nullptr, &rejectedPipeline) ==
                    VK_ERROR_FEATURE_NOT_PRESENT,
                "Unsupported recursion accepted");
        Require(rejectedPipeline == VK_NULL_HANDLE, "Rejected pipeline leaked a handle");
        pci.maxPipelineRayRecursionDepth = 1;
        VkDeferredOperationKHR deferred;
        Check(CreateDeferredOperationKHR(t.device, nullptr, &deferred));
        VkPipeline pipeline;
        Require(CreateRayTracingPipelinesKHR(t.device, deferred, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline) ==
                    VK_OPERATION_NOT_DEFERRED_KHR,
                "Synchronous deferred operation returned wrong status");
        Require(GetDeferredOperationMaxConcurrencyKHR(t.device, deferred) == 0, "Unexpected deferred concurrency");
        Check(GetDeferredOperationResultKHR(t.device, deferred));
        Check(DeferredOperationJoinKHR(t.device, deferred));
        DestroyDeferredOperationKHR(t.device, deferred, nullptr);
        Require(GetRayTracingShaderGroupStackSizeKHR(t.device, pipeline, 2, VK_SHADER_GROUP_SHADER_CLOSEST_HIT_KHR) > 0,
                "Stack size missing");
        for (auto m : modules) vkDestroyShaderModule(t.device, m, nullptr);
        std::array<uint8_t, 96> handles;
        Check(GetRayTracingShaderGroupHandlesKHR(t.device, pipeline, 0, 3, handles.size(), handles.data()));
        auto sbt = t.Make(256, VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR);
        // Guarantee the reported 64-byte shader-group base alignment even if the
        // buffer's BDA is less aligned.
        uint64_t sbtOffset = (64 - sbt.address % 64) % 64;
        for (uint32_t i = 0; i < 3; i++)
            std::memcpy(static_cast<uint8_t *>(sbt.mapped) + sbtOffset + i * 64, handles.data() + i * 32, 32);
        VkStridedDeviceAddressRegionKHR rg{sbt.address + sbtOffset, 32, 32}, ms{sbt.address + sbtOffset + 64, 32, 32},
            hs{sbt.address + sbtOffset + 128, 32, 32}, cs{};
        VkDescriptorPoolSize ps[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = 1;
        dp.poolSizeCount = 2;
        dp.pPoolSizes = ps;
        VkDescriptorPool descriptorPool;
        Check(vkCreateDescriptorPool(t.device, &dp, nullptr, &descriptorPool));
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = descriptorPool;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &setLayout;
        VkDescriptorSet set;
        Check(vkAllocateDescriptorSets(t.device, &da, &set));
        VkWriteDescriptorSetAccelerationStructureKHR wa{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        wa.accelerationStructureCount = 1;
        wa.pAccelerationStructures = &tlas;
        VkDescriptorBufferInfo ob{output.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &wa, set, 0, 0, 1,
                                           VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, nullptr, nullptr, nullptr},
                                          {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1,
                                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &ob, nullptr}};
        vkUpdateDescriptorSets(t.device, 2, writes, 0, nullptr);
        const char *computeSource = R"(
#version 460
layout(local_size_x=1) in;
layout(set=0,binding=1,std430) buffer Output { vec4 pixels[]; } outData;
layout(push_constant) uniform Params { uint marker; } params;
void main() { outData.pixels[116]=vec4(float(params.marker),0,0,0); }
)";
        auto computeModule = t.Module(vel::rt::CompileGLSL(computeSource, VK_SHADER_STAGE_COMPUTE_BIT));
        VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cp.layout = layout;
        cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    nullptr,
                    0,
                    VK_SHADER_STAGE_COMPUTE_BIT,
                    computeModule,
                    "main",
                    nullptr};
        VkPipeline computePipeline;
        Check(vkCreateComputePipelines(t.device, VK_NULL_HANDLE, 1, &cp, nullptr, &computePipeline));
        vkDestroyShaderModule(t.device, computeModule, nullptr);
        for (uint32_t mask : {1u, 2u, 3u, 0u}) {
            if (mask != 1) {
                Check(vkResetCommandPool(t.device, t.pool, 0));
                t.Begin();
            }
            uint32_t marker = 73;
            vkCmdBindPipeline(t.cb, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
            vkCmdBindDescriptorSets(t.cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(t.cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &marker);
            CmdSetRayTracingPipelineStackSizeKHR(t.cb, 1024);
            vkCmdBindPipeline(t.cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);
            vkCmdBindDescriptorSets(t.cb, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, layout, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(t.cb, layout, VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, 4, &mask);
            CmdTraceRaysKHR(t.cb, &rg, &ms, &hs, &cs, w, h, 1);
            t.Barrier(VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
            // No rebind: the layer must restore the application's compute pipeline,
            // descriptor sets and push constants.
            vkCmdDispatch(t.cb, 1, 1, 1);
            if (mask == 3) {
                VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
                barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.memoryBarrierCount = 1;
                dependency.pMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(t.cb, &dependency);
            } else
                t.Barrier(VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          VK_ACCESS_HOST_READ_BIT);
            t.Submit(mask == 3);
            uint32_t hits = 0;
            auto pixels = static_cast<float *>(output.mapped);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) {
                    float px = (float(x) + 0.5f) / w * 7 - 2, py = (float(y) + 0.5f) / h * 4 - 2;
                    bool inside = false;
                    for (uint32_t ii = 0; ii < 2; ii++)
                        if (mask & (1u << ii))
                            for (float cx : {0.0f, 2.0f}) {
                                float lx = (px - float(ii) * 2) / (ii == 1 ? 1.5f : 1.0f), ly = py / (ii == 1 ? 0.75f : 1.0f);
                                float u = (lx - cx + 1 - (ly + 1) * 0.5f) * 0.5f, v = (ly + 1) * 0.5f;
                                if (u >= 0 && v >= 0 && u + v <= 1) inside = true;
                            }
                    if (empty) inside = false;
                    auto p = pixels + (y * w + x) * 4;
                    Require(std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]), "Non-finite payload");
                    if (std::abs(p[3] - (inside ? 1.f : 0.f)) > 0.001f) {
                        std::fprintf(stderr,
                                     "Mismatch mask=%u pixel=(%u,%u) origin=(%f,%f) "
                                     "alpha=%f expected=%d\n",
                                     mask, x, y, px, py, p[3], inside);
                        throw std::runtime_error(
                            "GPU traversal differs from independent "
                            "CPU triangle reference");
                    }
                    if (inside) {
                        hits++;
                        Require(std::abs(p[0] + p[1] + p[2] - 1) < 0.001f, "Invalid barycentrics");
                    }
                }
            Require(pixels[(w * h - 1) * 4] == 73, "Compute pipeline or push constant state was not restored");
            std::printf("GPU trace mask=%u: %u hits, %u misses verified\n", mask, hits, w * h - hits);
        }
        vkDestroyPipeline(t.device, computePipeline, nullptr);
        vkDestroyPipeline(t.device, pipeline, nullptr);
        vkDestroyDescriptorPool(t.device, descriptorPool, nullptr);
        vkDestroyPipelineLayout(t.device, layout, nullptr);
        vkDestroyDescriptorSetLayout(t.device, setLayout, nullptr);
        DestroyAccelerationStructureKHR(t.device, tlas, nullptr);
        DestroyAccelerationStructureKHR(t.device, blas, nullptr);
        std::puts("All fallback tests passed");
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAILED: %s\n", e.what());
        return 1;
    }
}
