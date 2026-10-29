// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "state_compiler.h"
#include "typed_pipeline.h"
#include "typed_runtime.h"
#include "glsl_compiler.h"
#include "cts_raytmax_intersection_fixture.h"
#include <spirv-tools/libspirv.hpp>
#include <spirv/unified1/spirv.hpp>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace {
using namespace vel::rt;
void Require(bool value, const std::string &message) {
    if (!value) throw std::runtime_error(message);
}
void Check(VkResult result) { Require(result == VK_SUCCESS, "Vulkan result " + std::to_string(result)); }
#include "frame_test_gpu.h"

// The pointer regressions need real independent descriptor allocations and,
// optionally, divergent descriptor-array elements. Keep those requirements
// local to these cases instead of changing the other component GPU fixtures.
struct PointerGPU : GPU {
    struct AppBuffer {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        uint32_t *mapped = nullptr;
    };
    VkPhysicalDevice physical{};
    std::vector<AppBuffer> resources;
    void Allocate(uint32_t bytes, bool bda, VkBuffer &out_buffer, VkDeviceMemory &out_memory, uint32_t *&out_mapped) {
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = bytes;
        ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | (bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
        Check(vkCreateBuffer(device, &ci, nullptr, &out_buffer));
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, out_buffer, &requirements);
        VkPhysicalDeviceMemoryProperties properties;
        vkGetPhysicalDeviceMemoryProperties(physical, &properties);
        uint32_t type = 0;
        for (; type < properties.memoryTypeCount; ++type)
            if ((requirements.memoryTypeBits & (1u << type)) &&
                (properties.memoryTypes[type].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                break;
        Require(type < properties.memoryTypeCount, "No coherent pointer-fixture memory");
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.pNext = bda ? &flags : nullptr;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        Check(vkAllocateMemory(device, &allocation, nullptr, &out_memory));
        Check(vkBindBufferMemory(device, out_buffer, out_memory, 0));
        Check(vkMapMemory(device, out_memory, 0, bytes, 0, reinterpret_cast<void **>(&out_mapped)));
        std::memset(out_mapped, 0xcd, bytes);
    }
    bool InitPointer(const std::vector<uint32_t> &code, uint32_t bytes, uint32_t resource_words, uint32_t descriptor_count,
                     bool bda, uint32_t context_offset, bool independent_roots) {
        specialized_context = bda;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ic.pApplicationInfo = &app;
        const auto result = vkCreateInstance(&ic, nullptr, &instance);
        if (result == VK_ERROR_INCOMPATIBLE_DRIVER) return false;
        Check(result);
        uint32_t count = 0;
        Check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        if (!count) return false;
        std::vector<VkPhysicalDevice> devices(count);
        Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        physical = devices[0];
        VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 device_properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &subgroup};
        vkGetPhysicalDeviceProperties2(physical, &device_properties);
        const auto &properties = device_properties.properties;
        const auto descriptors_needed = 1 + 2 * descriptor_count;
        if (properties.apiVersion < VK_API_VERSION_1_3 ||
            properties.limits.maxPerStageDescriptorStorageBuffers < descriptors_needed ||
            properties.limits.maxDescriptorSetStorageBuffers < descriptors_needed ||
            properties.limits.maxPerStageResources < descriptors_needed)
            return false;
        // The descriptor-array qualification uses one 16-invocation workgroup.
        // Each selected root then has eight active invocations with different
        // descriptor indices, rather than one invocation per workgroup.
        if (descriptor_count > 1 &&
            (subgroup.subgroupSize < 16 || !(subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
             properties.limits.maxComputeWorkGroupSize[0] < 16 || properties.limits.maxComputeWorkGroupInvocations < 16))
            return false;
        VkPhysicalDeviceVariablePointersFeatures pointers{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES};
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &pointers};
        VkPhysicalDeviceFeatures2 available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12};
        vkGetPhysicalDeviceFeatures2(physical, &available);
        if (!available.features.shaderInt64 || !pointers.variablePointersStorageBuffer ||
            (independent_roots && !pointers.variablePointers) || (bda && !v12.bufferDeviceAddress))
            return false;
        if (descriptor_count > 1 &&
            (!available.features.shaderStorageBufferArrayDynamicIndexing || !v12.shaderStorageBufferArrayNonUniformIndexing))
            return false;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        uint32_t family = 0;
        while (family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
        if (family == count) return false;
        float priority = 1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        pointers = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES};
        pointers.variablePointersStorageBuffer = VK_TRUE;
        pointers.variablePointers = independent_roots;
        v12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &pointers};
        v12.bufferDeviceAddress = bda;
        v12.shaderStorageBufferArrayNonUniformIndexing = descriptor_count > 1;
        VkPhysicalDeviceFeatures enabled{};
        enabled.shaderInt64 = VK_TRUE;
        enabled.shaderStorageBufferArrayDynamicIndexing = descriptor_count > 1;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &v12};
        dc.queueCreateInfoCount = 1;
        dc.pQueueCreateInfos = &qi;
        dc.pEnabledFeatures = &enabled;
        Check(vkCreateDevice(physical, &dc, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
        Allocate(bytes, bda, buffer, memory, mapped);
        if (bda) {
            VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            info.buffer = buffer;
            address = vkGetBufferDeviceAddress(device, &info);
            Require(address != 0, "Pointer fixture has no frame BDA");
        }
        resources.resize(2 * descriptor_count);
        std::vector<VkDescriptorBufferInfo> infos{{buffer, 0, bytes}};
        for (auto &resource : resources) {
            Allocate(resource_words * 4, false, resource.buffer, resource.memory, resource.mapped);
            infos.push_back({resource.buffer, 0, resource_words * 4});
        }
        VkDescriptorSetLayoutBinding bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, descriptor_count, VK_SHADER_STAGE_COMPUTE_BIT},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, descriptor_count, VK_SHADER_STAGE_COMPUTE_BIT}};
        VkDescriptorSetLayoutCreateInfo sc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sc.bindingCount = 3;
        sc.pBindings = bindings;
        Check(vkCreateDescriptorSetLayout(device, &sc, nullptr, &set_layout));
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 + 2 * descriptor_count};
        VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pc.maxSets = 1;
        pc.poolSizeCount = 1;
        pc.pPoolSizes = &size;
        Check(vkCreateDescriptorPool(device, &pc, nullptr, &descriptors));
        VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        sa.descriptorPool = descriptors;
        sa.descriptorSetCount = 1;
        sa.pSetLayouts = &set_layout;
        Check(vkAllocateDescriptorSets(device, &sa, &set));
        VkWriteDescriptorSet writes[3]{};
        for (uint32_t binding = 0; binding < 3; ++binding) {
            auto &write = writes[binding];
            write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = set;
            write.dstBinding = binding;
            write.descriptorCount = binding ? descriptor_count : 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = infos.data() + (binding ? 1 + (binding - 1) * descriptor_count : 0);
        }
        vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lc.setLayoutCount = 1;
        lc.pSetLayouts = &set_layout;
        lc.pushConstantRangeCount = bda ? 0 : 1;
        lc.pPushConstantRanges = &push;
        Check(vkCreatePipelineLayout(device, &lc, nullptr, &layout));
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.codeSize = code.size() * 4;
        sm.pCode = code.data();
        Check(vkCreateShaderModule(device, &sm, nullptr, &shader));
        const auto context = address + context_offset;
        const uint32_t values[] = {uint32_t(context), uint32_t(context >> 32)};
        VkSpecializationMapEntry entries[] = {{0, 0, 4}, {1, 4, 4}};
        VkSpecializationInfo spec{2, entries, sizeof(values), values};
        VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cp.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cp.stage.module = shader;
        cp.stage.pName = "main";
        cp.stage.pSpecializationInfo = bda ? &spec : nullptr;
        cp.layout = layout;
        Check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp, nullptr, &pipeline));
        VkCommandPoolCreateInfo command_pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        command_pool.queueFamilyIndex = family;
        Check(vkCreateCommandPool(device, &command_pool, nullptr, &pool));
        return true;
    }
    ~PointerGPU() {
        if (!device) return;
        vkDeviceWaitIdle(device);
        for (auto &resource : resources) {
            if (resource.mapped) vkUnmapMemory(device, resource.memory);
            if (resource.buffer) vkDestroyBuffer(device, resource.buffer, nullptr);
            if (resource.memory) vkFreeMemory(device, resource.memory, nullptr);
        }
    }
};

// This is source SPIR-V, not a hand-written compute/state test. The compiler
// must transform its actual CFG, helper call, Phi edges, arithmetic and memory
// operations. The CPU oracle below implements the algorithm independently.
const char *kSource = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpExtension "SPV_KHR_ray_tracing"
%glsl = OpExtInstImport "GLSL.std.450"
OpMemoryModel Logical GLSL450
OpEntryPoint IntersectionKHR %main "main" %launch %config %output
OpName %local "local"
OpName %shadow "shadow"
OpName %untouched "untouched"
OpName %accepted "first_accepted"
OpDecorate %launch BuiltIn LaunchIdKHR
OpDecorate %launch UserSemantic "launch%123\"%456"
OpDecorate %config_type Block
OpMemberDecorate %config_type 0 Offset 0
OpDecorate %out_array ArrayStride 4
OpDecorate %out_type Block
OpMemberDecorate %out_type 0 Offset 0
OpDecorate %output DescriptorSet 0
OpDecorate %output Binding 1
%void = OpTypeVoid
%bool = OpTypeBool
%u32 = OpTypeInt 32 0
%f32 = OpTypeFloat 32
%v3u = OpTypeVector %u32 3
%input = OpTypePointer Input %v3u
%config_type = OpTypeStruct %u32
%config_ptr = OpTypePointer PushConstant %config_type
%config_word = OpTypePointer PushConstant %u32
%out_array = OpTypeRuntimeArray %u32
%out_type = OpTypeStruct %out_array
%out_ptr = OpTypePointer StorageBuffer %out_type
%out_word = OpTypePointer StorageBuffer %u32
%launch = OpVariable %input Input
%config = OpVariable %config_ptr PushConstant
%output = OpVariable %out_ptr StorageBuffer
%c0 = OpConstant %u32 0
%c1 = OpConstant %u32 1
%c2 = OpConstant %u32 2
%c3 = OpConstant %u32 3
%c5 = OpConstant %u32 5
%c7 = OpConstant %u32 7
%c11 = OpConstant %u32 11
%c13 = OpConstant %u32 13
%c17 = OpConstant %u32 17
%c23 = OpConstant %u32 23
%c31 = OpConstant %u32 31
%c64 = OpConstant %u32 64
%half = OpConstant %f32 0.5
%quarter = OpConstant %f32 1.25
%true = OpConstantTrue %bool
%array = OpTypeArray %u32 %c3
%payload = OpTypeStruct %v3u %array %bool %u32
%local_ptr = OpTypePointer Function %payload
%local_word = OpTypePointer Function %u32
%vec = OpConstantComposite %v3u %c17 %c23 %c31
%arr = OpConstantComposite %array %c5 %c7 %c11
%initial = OpConstantComposite %payload %vec %arr %true %c0
%entry_type = OpTypeFunction %void
%helper_type = OpTypeFunction %bool %f32 %u32
%main = OpFunction %void None %entry_type
%entry = OpLabel
%local = OpVariable %local_ptr Function
%shadow = OpVariable %local_ptr Function
%untouched = OpVariable %local_word Function
%launch_id = OpLoad %v3u %launch
%id = OpCompositeExtract %u32 %launch_id 0
%seed_ptr = OpAccessChain %out_word %output %c0 %id
%external_seed = OpLoad %u32 %seed_ptr
%index = OpUMod %u32 %id %c3
%alias = OpAccessChain %local_word %local %c1 %index
%limit_ptr = OpAccessChain %config_word %config %c0
%base_limit = OpLoad %u32 %limit_ptr
%limit = OpIAdd %u32 %base_limit %index
OpStore %local %initial
OpBranch %loop
%loop = OpLabel
%i = OpPhi %u32 %c0 %entry %next %continue
%sum = OpPhi %u32 %c0 %entry %next_sum %continue
%flip_a = OpPhi %u32 %c2 %entry %flip_b %continue
%flip_b = OpPhi %u32 %c3 %entry %flip_a %continue
%in_range = OpULessThan %bool %i %limit
OpLoopMerge %exit %continue None
OpBranchConditional %in_range %body %exit
%body = OpLabel
%snapshot = OpLoad %payload %local
%fi = OpConvertUToF %f32 %i
%distance = OpFAdd %f32 %fi %half
%kind = OpIAdd %u32 %i %c1
%accepted = OpReportIntersectionKHR %bool %distance %kind
OpStore %shadow %snapshot
%old = OpLoad %u32 %alias
%plus_i = OpIAdd %u32 %old %i
%bonus = OpSelect %u32 %accepted %c5 %c1
%updated = OpIAdd %u32 %plus_i %bonus
OpStore %alias %updated
OpSelectionMerge %choice None
OpBranchConditional %accepted %yes %no
%yes = OpLabel
%good = OpIAdd %u32 %sum %c3
OpBranch %choice
%no = OpLabel
%bad = OpIAdd %u32 %sum %c7
OpBranch %choice
%choice = OpLabel
%chosen = OpPhi %u32 %good %yes %bad %no
%low = OpBitwiseAnd %u32 %i %c1
%odd = OpIEqual %bool %low %c1
OpSelectionMerge %second_merge None
OpBranchConditional %odd %second %skip
%second = OpLabel
%distance2_raw = OpFAdd %f32 %fi %quarter
%negative_distance = OpFNegate %f32 %distance2_raw
%distance2 = OpExtInst %f32 %glsl FAbs %negative_distance
%kind2 = OpIAdd %u32 %i %c64
%accepted2 = OpFunctionCall %bool %helper %distance2 %kind2
%second_bonus = OpSelect %u32 %accepted2 %c11 %c13
OpBranch %second_merge
%skip = OpLabel
OpBranch %second_merge
%second_merge = OpLabel
%extra = OpPhi %u32 %second_bonus %second %c0 %skip
%late_first_bonus = OpSelect %u32 %accepted %c2 %c3
%with_value = OpIAdd %u32 %chosen %updated
%with_flip = OpIAdd %u32 %with_value %flip_a
%with_second = OpIAdd %u32 %with_flip %extra
%next_sum = OpIAdd %u32 %with_second %late_first_bonus
OpBranch %continue
%continue = OpLabel
%next = OpIAdd %u32 %i %c1
OpBranch %loop
%exit = OpLabel
%mode = OpUMod %u32 %id %c3
OpSelectionMerge %final None
OpSwitch %mode %case2 0 %case0 1 %case1
%case0 = OpLabel
%r0 = OpIAdd %u32 %sum %c17
OpBranch %final
%case1 = OpLabel
%r1 = OpIAdd %u32 %sum %c23
OpBranch %final
%case2 = OpLabel
%r2 = OpIAdd %u32 %sum %c31
OpBranch %final
%final = OpLabel
%final_sum = OpPhi %u32 %r0 %case0 %r1 %case1 %r2 %case2
%seed_added = OpIAdd %u32 %final_sum %external_seed
%result_ptr = OpAccessChain %local_word %local %c3
OpStore %result_ptr %seed_added
%external_ptr = OpAccessChain %out_word %output %c0 %id
OpStore %external_ptr %seed_added
OpReturn
OpFunctionEnd
%helper = OpFunction %bool None %helper_type
%argument_distance = OpFunctionParameter %f32
%argument_kind = OpFunctionParameter %u32
%helper_entry = OpLabel
%reported = OpReportIntersectionKHR %bool %argument_distance %argument_kind
OpReturnValue %reported
OpFunctionEnd
)spv";

std::vector<uint32_t> Source(const std::string &text = kSource) {
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    std::string error;
    tools.SetMessageConsumer(
        [&](spv_message_level_t, const char *, const spv_position_t &, const char *message) { error += message; });
    std::vector<uint32_t> binary;
    bool success = tools.Assemble(text, &binary);
    Require(success, "Source assembly: " + error);
    success = tools.Validate(binary);
    Require(success, "Source validation: " + error);
    return binary;
}
void Replace(std::string &source, const std::string &before, const std::string &after) {
    auto offset = source.find(before);
    Require(offset != std::string::npos, "Regression source fragment missing");
    source.replace(offset, before.size(), after);
}
void LargeDescriptorResourceTypes() {
    // A full native storage-buffer budget is a descriptor declaration, not a
    // million-element value to spill into each invocation's continuation frame.
    const char *source = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpCapability StorageBufferArrayDynamicIndexing
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint RayGenerationKHR %main "main" %scenes %buffers %payload
OpDecorate %scenes DescriptorSet 31
OpDecorate %scenes Binding 0
OpDecorate %buffers DescriptorSet 31
OpDecorate %buffers Binding 1
OpDecorate %block Block
OpMemberDecorate %block 0 Offset 0
OpDecorate %payload Location 0
%void = OpTypeVoid
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%vec3 = OpTypeVector %float 3
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%mask = OpConstant %uint 255
%sixteen = OpConstant %uint 16
%last_scene = OpConstant %uint 15
%count = OpConstant %uint 1048544
%last = OpConstant %uint 1048543
%fzero = OpConstant %float 0
%fone = OpConstant %float 1
%origin = OpConstantComposite %vec3 %fzero %fzero %fzero
%direction = OpConstantComposite %vec3 %fzero %fzero %fone
%as = OpTypeAccelerationStructureKHR
%as_array = OpTypeArray %as %sixteen
%as_array_ptr = OpTypePointer UniformConstant %as_array
%as_ptr = OpTypePointer UniformConstant %as
%block = OpTypeStruct %uint
%blocks = OpTypeArray %block %count
%blocks_ptr = OpTypePointer StorageBuffer %blocks
%word_ptr = OpTypePointer StorageBuffer %uint
%payload_ptr = OpTypePointer RayPayloadKHR %uint
%scenes = OpVariable %as_array_ptr UniformConstant
%buffers = OpVariable %blocks_ptr StorageBuffer
%payload = OpVariable %payload_ptr RayPayloadKHR
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%entry = OpLabel
%scene_ptr = OpAccessChain %as_ptr %scenes %last_scene
%scene = OpLoad %as %scene_ptr
OpStore %payload %one
OpTraceRayKHR %scene %one %mask %zero %zero %zero %origin %fzero %direction %fone %payload
%word = OpAccessChain %word_ptr %buffers %last %zero
%result = OpLoad %uint %payload
OpStore %word %result
OpReturn
OpFunctionEnd
)spv";
    auto program = CompileTypedContinuationSteps(Source(source), "main", spv::ExecutionModelRayGenerationKHR);
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    Require(tools.Validate(program.spirv), "Large resource declaration failed generated-module validation");
    Require(program.spirv.size() < 15000, "Resource descriptor array was expanded into per-element code");
    for (const auto &type : program.frames.types)
        Require(type.size < 4096, "Resource descriptor array was included in continuation value layouts");
    std::map<uint32_t, uint32_t> constants;
    bool full_array = false;
    for (size_t p = 5; p < program.spirv.size(); p += program.spirv[p] >> 16) {
        const auto op = program.spirv[p] & 65535;
        if (op == spv::OpConstant && (program.spirv[p] >> 16) == 4) constants[program.spirv[p + 2]] = program.spirv[p + 3];
        if (op == spv::OpTypeArray) full_array |= constants[program.spirv[p + 3]] == 1048544;
    }
    Require(full_array && program.acceleration_structure_descriptors.size() == 1,
            "Full SSBO array or AS descriptor interface was discarded");
}
void ResourceValueTypes() {
    // SPIR-V1.4 permits a source Function pointer to the same explicitly laid
    // out structure used by a buffer. The newer emitted module must split
    // logical values from resource layouts without weakening those layouts.
    const char *source = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint RayGenerationKHR %main "main" %buffer %callable
OpMemberDecorate %pair 0 Offset 0
OpMemberDecorate %pair 1 Offset 8
OpDecorate %array ArrayStride 16
OpMemberDecorate %block 0 Offset 0
OpMemberDecorate %block 1 Offset 16
OpDecorate %block Block
OpDecorate %buffer DescriptorSet 0
OpDecorate %buffer Binding 1
OpDecorate %callable Location 0
%void = OpTypeVoid
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%two = OpConstant %uint 2
%pair = OpTypeStruct %uint %float
%array = OpTypeArray %pair %two
%block = OpTypeStruct %pair %array
%buffer_ptr = OpTypePointer StorageBuffer %block
%pair_ptr = OpTypePointer StorageBuffer %pair
%array_ptr = OpTypePointer StorageBuffer %array
%local_ptr = OpTypePointer Function %pair
%callable_ptr = OpTypePointer CallableDataKHR %uint
%buffer = OpVariable %buffer_ptr StorageBuffer
%callable = OpVariable %callable_ptr CallableDataKHR
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%entry = OpLabel
%local = OpVariable %local_ptr Function
%p = OpAccessChain %pair_ptr %buffer %zero
%a = OpAccessChain %array_ptr %buffer %one
%before = OpLoad %pair %p Volatile
%before_array = OpLoad %array %a Volatile
OpExecuteCallableKHR %zero %callable
%after = OpLoad %pair %p Volatile
OpStore %local %before
OpStore %p %after Volatile
OpStore %a %before_array Volatile
OpReturn
OpFunctionEnd
)spv";
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    std::string error;
    tools.SetMessageConsumer(
        [&](spv_message_level_t, const char *, const spv_position_t &, const char *message) { error += message; });
    std::vector<uint32_t> words;
    Require(tools.Assemble(source, &words), "Resource source assembly: " + error);
    words[1] = 0x00010400;
    Require(tools.Validate(words), "Resource source validation: " + error);
    auto program = CompileTypedContinuationSteps(words, "main", spv::ExecutionModelRayGenerationKHR);
    Require(tools.Validate(program.spirv), "Resource value/layout split validation: " + error);
    uint32_t logical_copies = 0, volatile_loads = 0;
    for (size_t p = 5; p < program.spirv.size(); p += program.spirv[p] >> 16) {
        const auto op = program.spirv[p] & 65535;
        logical_copies += op == spv::OpCopyLogical;
        volatile_loads +=
            op == spv::OpLoad && (program.spirv[p] >> 16) >= 5 && (program.spirv[p + 4] & spv::MemoryAccessVolatileMask);
    }
    Require(logical_copies >= 5 && volatile_loads == 3, "Aggregate conversion lost or duplicated a volatile source read");
}
void SourcePrivateFrameBounds() {
    const char *source = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpCapability Int64
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint IntersectionKHR %main "main" %large
%void = OpTypeVoid
%bool = OpTypeBool
%u32 = OpTypeInt 32 0
%u64 = OpTypeInt 64 0
%f32 = OpTypeFloat 32
%zero = OpConstant %u32 0
%wide_zero = OpConstant %u64 0
%one = OpConstant %f32 1
%count = OpConstant %u32 1073741824
%array = OpTypeArray %u64 %count
%array_ptr = OpTypePointer Private %array
%word_ptr = OpTypePointer Private %u64
%large = OpVariable %array_ptr Private
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%entry = OpLabel
%element = OpAccessChain %word_ptr %large %zero
%reported = OpReportIntersectionKHR %bool %one %zero
OpStore %element %wide_zero
OpReturn
OpFunctionEnd
)spv";
    bool rejected = false;
    try {
        CompileTypedContinuationSteps(Source(source), "main", spv::ExecutionModelIntersectionKHR);
    } catch (const std::exception &e) {
        rejected = std::string(e.what()).find("module-Private storage exceeds 32-bit") != std::string::npos;
    }
    Require(rejected, "Oversized module-Private array was emitted or its frame offset truncated");
}
void OpaqueResourceBoundaries() {
    const std::string image = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint RayGenerationKHR %main "main" %image
OpDecorate %image DescriptorSet 0
OpDecorate %image Binding 1
%void = OpTypeVoid
%bool = OpTypeBool
%u32 = OpTypeInt 32 0
%i32 = OpTypeInt 32 1
%f32 = OpTypeFloat 32
%v3i = OpTypeVector %i32 3
%v4i = OpTypeVector %i32 4
%image_type = OpTypeImage %i32 3D 0 0 0 2 R32i
%image_pointer = OpTypePointer UniformConstant %image_type
%image = OpVariable %image_pointer UniformConstant
%fn = OpTypeFunction %void
%zero = OpConstant %i32 0
%one = OpConstant %i32 1
%uzero = OpConstant %u32 0
%fone = OpConstant %f32 1
%coord = OpConstantComposite %v3i %zero %zero %zero
%color = OpConstantComposite %v4i %one %zero %zero %one
%main = OpFunction %void None %fn
%entry = OpLabel
%loaded = OpLoad %image_type %image
OpImageWrite %loaded %coord %color SignExtend
OpReturn
OpFunctionEnd
)spv";
    auto check = [&](const std::string &source, uint32_t model) {
        auto program = CompileTypedContinuationSteps(Source(source), "main", model);
        uint32_t writes = 0, images = 0;
        for (size_t p = 5; p < program.spirv.size(); p += program.spirv[p] >> 16) {
            auto op = program.spirv[p] & 65535;
            writes += op == spv::OpImageWrite;
            images += op == spv::OpTypeImage;
        }
        Require(writes == 1 && images == 1, "Storage image source operation/interface lost");
    };
    check(image, spv::ExecutionModelRayGenerationKHR);
    std::string late = image;
    Replace(late, "RayGenerationKHR %main", "IntersectionKHR %main");
    Replace(late, "%loaded = OpLoad", "%reported = OpReportIntersectionKHR %bool %fone %uzero\n%loaded = OpLoad");
    check(late, spv::ExecutionModelIntersectionKHR);
    std::string crossing = image;
    Replace(crossing, "OpImageWrite", "OpBranch %next\n%next = OpLabel\nOpImageWrite");
    check(crossing, spv::ExecutionModelRayGenerationKHR);

    std::string atomic = image;
    Replace(atomic, "%image_pointer = OpTypePointer", "%texel_pointer = OpTypePointer Image %i32\n%image_pointer = OpTypePointer");
    Replace(atomic, "%uzero = OpConstant",
            "%device = OpConstant %u32 1\n%image_acq_rel = OpConstant %u32 2056\n"
            "%image_release = OpConstant %u32 2052\n%uzero = OpConstant");
    Replace(atomic, "%loaded = OpLoad %image_type %image\nOpImageWrite %loaded %coord %color SignExtend",
            "%texel = OpImageTexelPointer %texel_pointer %image %coord %uzero\n"
            "%prior = OpAtomicSMin %i32 %texel %device %image_acq_rel %one\n"
            "OpAtomicStore %texel %device %image_release %prior\nOpMemoryBarrier %device %image_acq_rel");
    auto atomic_check = [&](const std::string &source, uint32_t model, uint32_t expected_texels = 1) {
        auto program = CompileTypedContinuationSteps(Source(source), "main", model);
        std::map<uint32_t, uint32_t> constants;
        uint32_t texels = 0, minima = 0, stores = 0, barriers = 0;
        for (size_t p = 5; p < program.spirv.size(); p += program.spirv[p] >> 16) {
            const auto op = program.spirv[p] & 65535;
            if (op == spv::OpConstant && program.spirv[p] >> 16 == 4) constants[program.spirv[p + 2]] = program.spirv[p + 3];
            texels += op == spv::OpImageTexelPointer;
            if (op == spv::OpAtomicSMin) {
                ++minima;
                Require(constants.at(program.spirv[p + 4]) == 1 && constants.at(program.spirv[p + 5]) == 2056,
                        "Image atomic scope or acquire/release/image memory semantics changed");
            }
            if (op == spv::OpAtomicStore) {
                ++stores;
                Require(constants.at(program.spirv[p + 2]) == 1 && constants.at(program.spirv[p + 3]) == 2052,
                        "Image atomic store scope or release/image semantics changed");
            }
            if (op == spv::OpMemoryBarrier) {
                ++barriers;
                Require(constants.at(program.spirv[p + 1]) == 1 && constants.at(program.spirv[p + 2]) == 2056,
                        "Source memory barrier semantics changed");
            }
        }
        Require(texels == expected_texels && minima == 1 && stores == 1 && barriers == 1,
                "Image texel pointer/atomic side effects duplicated or lost");
        if (expected_texels == 2 && !program.frames.functions.at(0).suspensions.empty()) {
            uint32_t atomic_value = 0;
            for (size_t p = 5; p < program.source.size(); p += program.source[p] >> 16)
                if ((program.source[p] & 65535) == spv::OpAtomicSMin) atomic_value = program.source[p + 2];
            const auto &saved = program.frames.functions.at(0).suspensions.at(0).saved_values;
            Require(std::find(saved.begin(), saved.end(), atomic_value) != saved.end(),
                    "Atomic result lost across suspension or atomic side effect scheduled for replay");
        }
    };
    atomic_check(atomic, spv::ExecutionModelRayGenerationKHR);
    std::string late_atomic = atomic;
    Replace(late_atomic, "RayGenerationKHR %main", "IntersectionKHR %main");
    Replace(late_atomic, "%texel = OpImageTexelPointer",
            "%reported = OpReportIntersectionKHR %bool %fone %uzero\n%texel = OpImageTexelPointer");
    atomic_check(late_atomic, spv::ExecutionModelIntersectionKHR);
    std::string live_atomic = atomic;
    Replace(live_atomic, "RayGenerationKHR %main", "IntersectionKHR %main");
    Replace(live_atomic, "OpAtomicStore %texel",
            "%reported = OpReportIntersectionKHR %bool %fone %uzero\n"
            "%later_texel = OpImageTexelPointer %texel_pointer %image %coord %uzero\nOpAtomicStore %later_texel");
    atomic_check(live_atomic, spv::ExecutionModelIntersectionKHR, 2);
    std::string live_texel = atomic;
    Replace(live_texel, "RayGenerationKHR %main", "IntersectionKHR %main");
    Replace(live_texel, "OpAtomicStore %texel", "%reported = OpReportIntersectionKHR %bool %fone %uzero\nOpAtomicStore %texel");
    atomic_check(live_texel, spv::ExecutionModelIntersectionKHR, 2);
    // A selector is source data, not a rematerializable load. Both a texel
    // pointer and its copy must snapshot that data before its backing changes.
    std::string dynamic_texel = live_texel;
    Replace(dynamic_texel, "%fn = OpTypeFunction", "%local_pointer = OpTypePointer Function %i32\n%fn = OpTypeFunction");
    Replace(dynamic_texel, "%entry = OpLabel",
            "%entry = OpLabel\n%selector_storage = OpVariable %local_pointer Function %one\n"
            "%selector = OpLoad %i32 %selector_storage\n"
            "%dynamic_coord = OpCompositeConstruct %v3i %selector %selector %selector");
    Replace(dynamic_texel, "%image %coord %uzero", "%image %dynamic_coord %uzero");
    Replace(dynamic_texel, "%prior = OpAtomicSMin",
            "%saved_texel = OpCopyObject %texel_pointer %texel\nOpStore %selector_storage %zero\n%prior = OpAtomicSMin");
    Replace(dynamic_texel, "OpAtomicStore %texel", "OpAtomicStore %saved_texel");
    atomic_check(dynamic_texel, spv::ExecutionModelIntersectionKHR, 2);
    auto dynamic_program = CompileTypedContinuationSteps(Source(dynamic_texel), "main", spv::ExecutionModelIntersectionKHR);
    Require(dynamic_program.resource_snapshot_fields == 2 && dynamic_program.resource_snapshot_bytes == 24,
            "Dynamic texel coordinates/copy have no independent typed snapshots");
    std::string dynamic_descriptor = dynamic_texel;
    Replace(dynamic_descriptor, "OpCapability RayTracingKHR",
            "OpCapability RayTracingKHR\nOpCapability StorageImageArrayDynamicIndexing");
    Replace(dynamic_descriptor, "%image_pointer = OpTypePointer UniformConstant %image_type",
            "%descriptor_count = OpConstant %u32 2\n%image_array = OpTypeArray %image_type %descriptor_count\n"
            "%array_pointer = OpTypePointer UniformConstant %image_array\n"
            "%image_pointer = OpTypePointer UniformConstant %image_type");
    Replace(dynamic_descriptor, "%image = OpVariable %image_pointer", "%image = OpVariable %array_pointer");
    Replace(dynamic_descriptor, "%texel = OpImageTexelPointer",
            "%selected_image = OpAccessChain %image_pointer %image %selector\n%texel = OpImageTexelPointer");
    Replace(dynamic_descriptor, "%texel_pointer %image %dynamic_coord", "%texel_pointer %selected_image %dynamic_coord");
    atomic_check(dynamic_descriptor, spv::ExecutionModelIntersectionKHR, 2);
    auto selected_program = CompileTypedContinuationSteps(Source(dynamic_descriptor), "main", spv::ExecutionModelIntersectionKHR);
    Require(selected_program.resource_snapshot_fields == 5 && selected_program.resource_snapshot_bytes == 36,
            "Descriptor index was not captured independently through texel/copy provenance");
    auto dynamic_source = Source(dynamic_descriptor);
    auto physical_program =
        CompileTypedPipelineStage(dynamic_source, "main", VK_SHADER_STAGE_INTERSECTION_BIT_KHR, nullptr, "snapshot_bda");
    Require(physical_program.resource_snapshot_fields == 5 && physical_program.resource_snapshot_bytes == 36 &&
                physical_program.system_header_bytes == VEL_RT_SYSTEM_BYTES,
            "BDA resource snapshots lost the typed selectors or system prefix");
    std::string crossing_pointer = atomic;
    Replace(crossing_pointer, "%prior = OpAtomicSMin", "OpBranch %next\n%next = OpLabel\n%prior = OpAtomicSMin");
    atomic_check(crossing_pointer, spv::ExecutionModelRayGenerationKHR, 2);
}
void ExactCtsImageAtomicStage() {
    spvtools::SpirvTools source_tools(SPV_ENV_VULKAN_1_3);
    std::vector<uint32_t> source;
    Require(source_tools.Assemble(kCtsRayTMaxIntersectionSource, &source, SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS),
            "Exact CTS intersection source assembly failed");
    source[1] = 0x00010400;
    Require(source_tools.Validate(source), "Exact CTS SPIR-V1.4 intersection source failed validation");
    auto physical = CompileTypedPipelineStage(source, "main", VK_SHADER_STAGE_INTERSECTION_BIT_KHR, nullptr, "cts_is_bda");
    for (const auto *program : {&physical}) {
        spvtools::SpirvTools library_tools(SPV_ENV_UNIVERSAL_1_5);
        Require(library_tools.Validate(program->spirv), "Exact CTS image atomic stage library failed validation");
        uint32_t pointers = 0, atomics = 0;
        for (size_t p = 5; p < program->spirv.size(); p += program->spirv[p] >> 16) {
            const auto op = program->spirv[p] & 65535;
            pointers += op == spv::OpImageTexelPointer;
            atomics += op == spv::OpAtomicSMin;
        }
        Require(pointers == 1 && atomics == 1 && program->frames.functions.at(0).suspensions.size() == 1,
                "Exact CTS image atomic/report source shape changed");
        Require(std::any_of(program->builtin_variables.begin(), program->builtin_variables.end(),
                            [](const auto &v) { return v.builtin == spv::BuiltInRayTmaxKHR; }),
                "Exact CTS RayTmax system backing was lost");
    }
}
void BoundariesAndSourcePreservation() {
    constexpr uint32_t tail = kTypedContinuationControlWords * 4u + 3u;
    auto bounds = PlanTypedContinuationFrameBounds(UINT32_MAX - tail);
    Require(bounds.stride_words == UINT32_MAX / 4 && bounds.max_invocations == 4, "Near-4GiB frame arithmetic changed");
    for (auto size : {uint64_t(UINT32_MAX) - tail + 1, uint64_t(UINT32_MAX), UINT64_MAX}) {
        bool rejected = false;
        try {
            PlanTypedContinuationFrameBounds(size);
        } catch (const std::exception &error) {
            rejected = std::string(error.what()).find("32-bit byte indexing") != std::string::npos;
        }
        Require(rejected, "Frame/control byte offset overflow accepted");
    }
    std::string grouped(kSource);
    Replace(grouped, "OpDecorate %output DescriptorSet 0\nOpDecorate %output Binding 1",
            "%resource_decorations = OpDecorationGroup\nOpDecorate %resource_decorations DescriptorSet 0\n"
            "OpDecorate %resource_decorations Binding 1\nOpGroupDecorate %resource_decorations %output");
    CompileTypedContinuationSteps(Source(grouped), "main", spv::ExecutionModelIntersectionKHR);
    std::string switch64(kSource);
    Replace(switch64, "OpCapability RayTracingKHR", "OpCapability RayTracingKHR\nOpCapability Int64");
    Replace(switch64, "%f32 = OpTypeFloat 32", "%f32 = OpTypeFloat 32\n%u64 = OpTypeInt 64 0");
    Replace(switch64, "%mode = OpUMod %u32 %id %c3", "%mode32 = OpUMod %u32 %id %c3\n%mode = OpUConvert %u64 %mode32");
    Replace(switch64, "OpSwitch %mode %case2 0 %case0 1 %case1",
            "OpSwitch %mode %case2 4294967296 %case0 18446744073709551615 %case1");
    CompileTypedContinuationSteps(Source(switch64), "main", spv::ExecutionModelIntersectionKHR);
    std::string pointer_copy(kSource);
    Replace(pointer_copy, "OpCapability RayTracingKHR", "OpCapability RayTracingKHR\nOpCapability ShaderNonUniform");
    Replace(pointer_copy, "OpDecorate %launch BuiltIn", "OpDecorate %seed_ptr NonUniform\nOpDecorate %launch BuiltIn");
    Replace(pointer_copy, "%seed_ptr = OpAccessChain %out_word %output %c0 %id",
            "%seed_base = OpAccessChain %out_word %output %c0 %id\n%seed_ptr = OpCopyObject %out_word %seed_base");
    auto copied = CompileTypedContinuationSteps(Source(pointer_copy), "main", spv::ExecutionModelIntersectionKHR);
    std::set<uint32_t> decorated, loaded_pointers;
    for (size_t p = 5; p < copied.spirv.size(); p += copied.spirv[p] >> 16) {
        auto opcode = copied.spirv[p] & 0xffff;
        if (opcode == spv::OpDecorate && copied.spirv[p + 2] == spv::DecorationNonUniform) decorated.insert(copied.spirv[p + 1]);
        if (opcode == spv::OpLoad) loaded_pointers.insert(copied.spirv[p + 3]);
    }
    Require(std::any_of(loaded_pointers.begin(), loaded_pointers.end(), [&](uint32_t id) { return decorated.count(id) != 0; }),
            "Load through a copied resource pointer lost its sole NonUniform decoration");

    std::string modes(kSource);
    Replace(modes, "OpCapability RayTracingKHR", "OpCapability RayTracingKHR\nOpCapability SignedZeroInfNanPreserve");
    Replace(modes, "OpExtension \"SPV_KHR_ray_tracing\"",
            "OpExtension \"SPV_KHR_ray_tracing\"\nOpExtension \"SPV_KHR_float_controls\"");
    Replace(modes, "OpName %local",
            "OpExecutionMode %main SignedZeroInfNanPreserve 32\nOpDecorate %distance NoContraction\nOpName %local");
    // Decorations belong after names, so insert NoContraction in that section.
    Replace(modes, "OpDecorate %distance NoContraction\n", "");
    Replace(modes, "OpDecorate %launch BuiltIn", "OpDecorate %distance NoContraction\nOpDecorate %launch BuiltIn");
    auto controls = CompileTypedContinuationSteps(Source(modes), "main", spv::ExecutionModelIntersectionKHR);
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    std::string disassembly;
    Require(tools.Disassemble(controls.spirv, &disassembly), "FP-control disassembly failed");
    Require(disassembly.find("SignedZeroInfNanPreserve 32") != std::string::npos &&
                disassembly.find("NoContraction") != std::string::npos,
            "Source FP execution mode/operation decoration was discarded");

    std::string multiple(kSource);
    Replace(multiple, "OpName %local", "OpEntryPoint RayGenerationKHR %other \"other\" %other_private\nOpName %local");
    Replace(multiple, "%launch = OpVariable",
            "%private_type = OpTypePointer Private %u32\n%other_private = OpVariable %private_type Private\n%launch = OpVariable");
    multiple +=
        "\n%other = OpFunction %void None %entry_type\n%other_label = OpLabel\n%unreachable_private_read = OpLoad %u32 "
        "%other_private\nOpReturn\nOpFunctionEnd\n";
    auto selected = CompileTypedContinuationSteps(Source(multiple), "main", spv::ExecutionModelIntersectionKHR);
    uint32_t entry_count = 0;
    for (size_t p = 5; p < selected.source.size(); p += selected.source[p] >> 16)
        if ((selected.source[p] & 0xffff) == spv::OpEntryPoint) ++entry_count;
    Require(entry_count == 1, "Unselected source entry point survived selection");

    auto reject = [&](const std::string &source, const char *reason) {
        try {
            CompileTypedContinuationSteps(Source(source), "main", spv::ExecutionModelIntersectionKHR);
        } catch (const std::exception &error) {
            Require(std::string(error.what()).find(reason) != std::string::npos,
                    "Wrong explicit rejection: " + std::string(error.what()));
            return;
        }
        throw std::runtime_error("Unsupported source accepted");
    };
    std::string collision(kSource);
    Replace(collision, "OpDecorate %output Binding 1", "OpDecorate %output Binding 0");
    reject(collision, "frame descriptor binding collides");
    std::string pointer(kSource);
    Replace(pointer, "OpCapability RayTracingKHR", "OpCapability RayTracingKHR\nOpCapability VariablePointersStorageBuffer");
    Replace(pointer, "%seed_ptr = OpAccessChain %out_word %output %c0 %id",
            "%seed_base = OpAccessChain %out_word %output %c0 %id\n%seed_ptr = OpSelect %out_word %true %seed_base %seed_base");
    auto pointer_program = CompileTypedContinuationSteps(Source(pointer), "main", spv::ExecutionModelIntersectionKHR);
    Require(pointer_program.pointer_snapshot_values && pointer_program.pointer_snapshot_bytes,
            "StorageBuffer pointer selection has no owned identity snapshots");
    std::string memory_model(kSource);
    Replace(memory_model, "OpCapability RayTracingKHR", "OpCapability RayTracingKHR\nOpCapability VulkanMemoryModel");
    Replace(memory_model, "OpExtension \"SPV_KHR_ray_tracing\"",
            "OpExtension \"SPV_KHR_ray_tracing\"\nOpExtension \"SPV_KHR_vulkan_memory_model\"");
    Replace(memory_model, "OpMemoryModel Logical GLSL450", "OpMemoryModel Logical Vulkan");
    auto memory_program = CompileTypedContinuationSteps(Source(memory_model), "main", spv::ExecutionModelIntersectionKHR);
    Require(tools.Disassemble(memory_program.spirv, &disassembly), "Vulkan memory-model disassembly failed");
    Require(disassembly.find("OpMemoryModel Logical Vulkan") != std::string::npos,
            "Vulkan source memory model changed during state lowering");
}
const char *kPointerSource = R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpCapability VariablePointersStorageBuffer
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint IntersectionKHR %main "main" %launch %output
OpDecorate %launch BuiltIn LaunchIdKHR
OpDecorate %array ArrayStride 4
OpDecorate %block Block
OpMemberDecorate %block 0 Offset 0
OpMemberDecorate %block 1 Offset 128
OpDecorate %output DescriptorSet 0
OpDecorate %output Binding 1
%void = OpTypeVoid
%bool = OpTypeBool
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%v3u = OpTypeVector %uint 3
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%two = OpConstant %uint 2
%three = OpConstant %uint 3
%seven = OpConstant %uint 7
%eleven = OpConstant %uint 11
%count = OpConstant %uint 32
%half = OpConstant %float 0.5
%array = OpTypeArray %uint %count
%block = OpTypeStruct %array %array
%block_ptr = OpTypePointer StorageBuffer %block
%word_ptr = OpTypePointer StorageBuffer %uint
%input_ptr = OpTypePointer Input %v3u
%output = OpVariable %block_ptr StorageBuffer
%launch = OpVariable %input_ptr Input
%null = OpConstantNull %word_ptr
%true = OpConstantTrue %bool
%entry_type = OpTypeFunction %void
%helper_type = OpTypeFunction %void %word_ptr %uint %float %uint
%main = OpFunction %void None %entry_type
%entry = OpLabel
%launch_value = OpLoad %v3u %launch
%id = OpCompositeExtract %uint %launch_value 0
%a = OpAccessChain %word_ptr %output %zero %id
%b = OpAccessChain %word_ptr %output %one %id
%nonnull = OpSelect %word_ptr %true %a %null
%parity = OpBitwiseAnd %uint %id %one
%even = OpIEqual %bool %parity %zero
OpBranch %loop
%loop = OpLabel
%round = OpPhi %uint %zero %entry %next %continue
%pa = OpPhi %word_ptr %nonnull %entry %pb %continue
%pb = OpPhi %word_ptr %b %entry %pa %continue
%keep_going = OpULessThan %bool %round %three
OpLoopMerge %done %continue None
OpBranchConditional %keep_going %body %done
%body = OpLabel
%selected = OpSelect %word_ptr %even %pa %pb
%round_float = OpConvertUToF %float %round
%distance = OpFAdd %float %round_float %half
%kind = OpIAdd %uint %id %round
%delta = OpIAdd %uint %round %one
%called = OpFunctionCall %void %helper %selected %delta %distance %kind
OpBranch %continue
%continue = OpLabel
%next = OpIAdd %uint %round %one
OpBranch %loop
%done = OpLabel
OpReturn
OpFunctionEnd
%helper = OpFunction %void None %helper_type
%target = OpFunctionParameter %word_ptr
%increment = OpFunctionParameter %uint
%t = OpFunctionParameter %float
%hit_kind = OpFunctionParameter %uint
%helper_entry = OpLabel
%old = OpLoad %uint %target
%accepted = OpReportIntersectionKHR %bool %t %hit_kind
%bonus = OpSelect %uint %accepted %seven %eleven
%sum = OpIAdd %uint %old %increment
%value = OpIAdd %uint %sum %bonus
OpStore %target %value
OpReturn
OpFunctionEnd
)spv";
bool LogicalPointerSnapshots(bool execute, bool array_pointers = false) {
    std::string source_text(kPointerSource);
    if (array_pointers) {
        Replace(source_text, "%word_ptr = OpTypePointer",
                "%array_ptr = OpTypePointer StorageBuffer %array\n%word_ptr = OpTypePointer");
        Replace(source_text, "%a = OpAccessChain %word_ptr %output %zero %id", "%a = OpAccessChain %array_ptr %output %zero");
        Replace(source_text, "%b = OpAccessChain %word_ptr %output %one %id", "%b = OpAccessChain %array_ptr %output %one");
        Replace(source_text, "%null = OpConstantNull %word_ptr", "%null = OpConstantNull %array_ptr");
        Replace(source_text, "%nonnull = OpSelect %word_ptr", "%nonnull = OpSelect %array_ptr");
        Replace(source_text, "%pa = OpPhi %word_ptr", "%pa = OpPhi %array_ptr");
        Replace(source_text, "%pb = OpPhi %word_ptr", "%pb = OpPhi %array_ptr");
        Replace(source_text, "%selected = OpSelect %word_ptr", "%selected = OpSelect %array_ptr");
        Replace(source_text, "%called = OpFunctionCall",
                "%element = OpAccessChain %word_ptr %selected %id\n%called = OpFunctionCall");
        Replace(source_text, "%helper %selected %delta", "%helper %element %delta");
    }
    const auto source = Source(source_text);
    auto program = CompileTypedContinuationSteps(source, "main", spv::ExecutionModelIntersectionKHR);
    Require(program.pointer_snapshot_values >= 5 && program.pointer_snapshot_bytes >= uint64_t(program.pointer_snapshot_values) * 4,
            "Pointer Phi/Select/helper arguments lack persistent typed identity snapshots");
    auto physical = CompileTypedPipelineStage(source, "main", VK_SHADER_STAGE_INTERSECTION_BIT_KHR, nullptr, "logical_pointer_bda");
    Require(physical.pointer_snapshot_values == program.pointer_snapshot_values &&
                physical.pointer_snapshot_bytes == program.pointer_snapshot_bytes,
            "Logical pointer identity ABI differs between SSBO and BDA frames");
    std::string wide_source(kPointerSource);
    Replace(wide_source, "OpCapability Shader", "OpCapability Shader\nOpCapability Int64");
    Replace(wide_source, "%float = OpTypeFloat 32", "%wide = OpTypeInt 64 1\n%float = OpTypeFloat 32");
    Replace(wide_source, "%a = OpAccessChain", "%wide_id = OpSConvert %wide %id\n%a = OpAccessChain");
    Replace(wide_source, "%output %zero %id", "%output %zero %wide_id");
    Replace(wide_source, "%output %one %id", "%output %one %wide_id");
    const auto wide = CompileTypedContinuationSteps(Source(wide_source), "main", spv::ExecutionModelIntersectionKHR);
    if (!array_pointers)
        Require(wide.pointer_snapshot_bytes > program.pointer_snapshot_bytes,
                "64-bit pointer selectors were truncated instead of retaining both words");
    std::string nonuniform_source(source_text);
    Replace(nonuniform_source, "OpCapability Shader", "OpCapability Shader\nOpCapability ShaderNonUniform");
    Replace(nonuniform_source, "OpDecorate %launch BuiltIn", "OpDecorate %a NonUniform\nOpDecorate %launch BuiltIn");
    const auto nonuniform = CompileTypedContinuationSteps(Source(nonuniform_source), "main", spv::ExecutionModelIntersectionKHR);
    std::set<uint32_t> decorated, pointer_types;
    for (size_t p = 5; p < nonuniform.spirv.size(); p += nonuniform.spirv[p] >> 16) {
        const auto op = nonuniform.spirv[p] & 0xffff;
        if (op == spv::OpTypePointer) pointer_types.insert(nonuniform.spirv[p + 1]);
        if (op == spv::OpDecorate && nonuniform.spirv[p + 2] == spv::DecorationNonUniform)
            decorated.insert(nonuniform.spirv[p + 1]);
    }
    bool pointer_phi = false, access_nonuniform = false;
    for (size_t p = 5; p < nonuniform.spirv.size(); p += nonuniform.spirv[p] >> 16) {
        const auto op = nonuniform.spirv[p] & 0xffff;
        if (op == spv::OpPhi && pointer_types.count(nonuniform.spirv[p + 1])) pointer_phi = true;
        if (op == spv::OpAccessChain && pointer_types.count(nonuniform.spirv[p + 1]) && decorated.count(nonuniform.spirv[p + 2]))
            access_nonuniform = true;
    }
    Require(!pointer_phi && access_nonuniform, "Routed access lost NonUniform or still merges opaque native pointers");
    if (!execute) return true;
    constexpr uint32_t count = 32;
    const auto frame_bytes = program.frame_stride_words * count * 4;
    const auto output_offset = (frame_bytes + 255u) & ~255u;
    GPU gpu;
    std::cout << "Creating logical " << (array_pointers ? "array" : "element") << " pointer GPU pipeline: " << program.spirv.size()
              << " words, frame " << program.frame_stride_bytes << " bytes\n"
              << std::flush;
    if (!gpu.Init(program.spirv, output_offset + count * 8, output_offset, false, frame_bytes, VK_WHOLE_SIZE, true)) {
        std::cout << "Logical-pointer GPU fixture skipped: no suitable Vulkan device with VariablePointersStorageBuffer\n";
        return false;
    }
    for (uint32_t activation = 0; activation < 2; ++activation) {
        for (uint32_t id = 0; id < count; ++id) {
            std::fill_n(gpu.mapped + id * program.frame_stride_words + program.control_word, 7, 0);
            gpu.mapped[output_offset / 4 + id] = 100 + id;
            gpu.mapped[output_offset / 4 + count + id] = 200 + id;
        }
        std::vector<uint32_t> reports(count);
        for (uint32_t round = 0; round < 4; ++round) {
            gpu.Dispatch(0, 0, count);
            for (uint32_t id = 0; id < count; ++id) {
                auto *control = gpu.mapped + id * program.frame_stride_words + program.control_word;
                if (round == 3) {
                    Require(control[0] == UINT32_MAX && reports[id] == 3, "Pointer loop did not terminate after three reports");
                    continue;
                }
                float distance;
                std::memcpy(&distance, control + 4, 4);
                Require(control[1] == 2 && control[2] == 0 && distance == float(round) + 0.5f && control[5] == id + round,
                        "Pointer helper lost source request order/arguments");
                control[3] = (id + round) % 3 != 0;
                control[2] = 1;
                ++reports[id];
            }
        }
        for (uint32_t id = 0; id < count; ++id) {
            uint32_t expected[2] = {100 + id, 200 + id};
            for (uint32_t round = 0; round < 3; ++round) {
                const auto which = (id & 1) ^ (round & 1);
                expected[which] += round + 1 + ((id + round) % 3 != 0 ? 7 : 11);
            }
            Require(gpu.mapped[output_offset / 4 + id] == expected[0] && gpu.mapped[output_offset / 4 + count + id] == expected[1],
                    "Pointer Phi swaps/selection/helper alias changed source writes across suspension");
        }
    }
    std::cout << "Logical StorageBuffer " << (array_pointers ? "array" : "element")
              << " pointers executed: 32 invocations, parallel loop Phi swaps, Select/null, "
                 "inlined pointer helper, independent root/index snapshots and repeated suspend/resume\n";
    return true;
}
std::string MutablePointerSource(bool module_private, bool stride_views = false) {
    std::string source(kPointerSource);
    Replace(source, "OpDecorate %array ArrayStride 4", "OpDecorate %word_ptr ArrayStride 4\nOpDecorate %array ArrayStride 4");
    Replace(source, "OpMemberDecorate %block 1 Offset 128", "OpMemberDecorate %block 1 Offset 512");
    Replace(source, "%count = OpConstant %uint 32", "%count = OpConstant %uint 128");
    Replace(source, "%seven = OpConstant",
            "%four = OpConstant %uint 4\n%negative_one = OpConstant %uint 4294967295\n"
            "%memory_count = OpConstant %uint 8193\n%memory_base = OpConstant %uint 8170\n%seven = OpConstant");
    Replace(source, "%entry_type = OpTypeFunction",
            "%pointer_array = OpTypeArray %word_ptr %memory_count\n"
            "%table_type = OpTypePointer Function %pointer_array\n%slot_type = OpTypePointer Function %word_ptr\n"
            "%round_slot_type = OpTypePointer Function %uint\n%record_type = OpTypeStruct %word_ptr %uint\n"
            "%record_pointer = OpTypePointer Function %record_type\n"
            "%entry_type = OpTypeFunction");
    Replace(source, "%entry = OpLabel",
            "%entry = OpLabel\n%table = OpVariable %table_type Function\n"
            "%other = OpVariable %slot_type Function %null\n"
            "%record = OpVariable %record_pointer Function\n%record_backup = OpVariable %record_pointer Function");
    Replace(source, "%a = OpAccessChain %word_ptr %output %zero %id",
            "%base_index = OpIMul %uint %id %four\n%a = OpAccessChain %word_ptr %output %zero %base_index");
    Replace(source, "%b = OpAccessChain %word_ptr %output %one %id",
            "%b = OpAccessChain %word_ptr %output %one %base_index\n"
            "%slot_index = OpIAdd %uint %memory_base %id\n%alias = OpAccessChain %slot_type %table %slot_index\n"
            "%alias_copy = OpCopyObject %slot_type %alias");
    Replace(source, "%pa = OpPhi %word_ptr %nonnull %entry %pb %continue",
            "%pa = OpPhi %word_ptr %nonnull %entry %advance_a %continue");
    Replace(source, "%pb = OpPhi %word_ptr %b %entry %pa %continue", "%pb = OpPhi %word_ptr %b %entry %advance_b %continue");
    Replace(source, "%round_float = OpConvertUToF",
            "OpSelectionMerge %stored None\nOpBranchConditional %even %store_yes %store_no\n"
            "%store_yes = OpLabel\nOpStore %alias_copy %selected\nOpCopyMemory %other %alias\n"
            "%yes_pointer = OpLoad %word_ptr %other\nOpBranch %stored\n"
            "%store_no = OpLabel\nOpStore %other %selected\nOpCopyMemory %alias %other\n"
            "%no_pointer = OpLoad %word_ptr %alias_copy\nOpBranch %stored\n"
            "%stored = OpLabel\n%loaded = OpPhi %word_ptr %yes_pointer %store_yes %no_pointer %store_no\n"
            "%record_slot = OpAccessChain %slot_type %record %zero\n%round_slot = OpAccessChain %round_slot_type %record %one\n"
            "OpStore %record_slot %loaded\nOpStore %round_slot %round\nOpCopyMemory %record_backup %record\n"
            "%record_copy = OpLoad %record_type %record\n%from_record = OpLoad %word_ptr %record_slot\n"
            "%round_float = OpConvertUToF");
    Replace(source, "%helper %selected %delta", "%helper %from_record %delta");
    Replace(source, "OpBranch %continue\n%continue = OpLabel",
            "%saved_round = OpCompositeExtract %uint %record_copy 1\n"
            "%same_round = OpIEqual %bool %saved_round %round\n"
            "OpStore %record %record_copy\nOpStore %record_slot %null\nOpStore %round_slot %memory_count\n"
            "OpCopyMemory %record %record_backup\n%late_pointer = OpLoad %word_ptr %record_slot\n"
            "%late_round = OpLoad %uint %round_slot\n%memory_round = OpIEqual %bool %late_round %round\n"
            "%same_pointer = OpPtrEqual %bool %late_pointer %loaded\n"
            "%copy_correct = OpLogicalAnd %bool %memory_round %same_pointer\n"
            "%correct = OpLogicalAnd %bool %same_round %copy_correct\n"
            "OpSelectionMerge %checked None\nOpBranchConditional %correct %checked %wrong\n"
            "%wrong = OpLabel\nOpStore %a %memory_count\nOpBranch %checked\n%checked = OpLabel\n"
            "OpBranch %continue\n%continue = OpLabel\n"
            "%forward_a = OpPtrAccessChain %word_ptr %pa %one\n"
            "%back_a = OpPtrAccessChain %word_ptr %forward_a %negative_one\n"
            "%advance_a = OpPtrAccessChain %word_ptr %back_a %one\n"
            "%advance_b = OpPtrAccessChain %word_ptr %pb %one\n");
    if (module_private) {
        Replace(source, "%launch %output", "%launch %output %table %other %record %record_backup");
        Replace(source, "%table_type = OpTypePointer Function", "%table_type = OpTypePointer Private");
        Replace(source, "%slot_type = OpTypePointer Function", "%slot_type = OpTypePointer Private");
        Replace(source, "%round_slot_type = OpTypePointer Function", "%round_slot_type = OpTypePointer Private");
        Replace(source, "%record_pointer = OpTypePointer Function", "%record_pointer = OpTypePointer Private");
        Replace(source, "%table = OpVariable %table_type Function\n", "");
        Replace(source, "%other = OpVariable %slot_type Function %null\n", "");
        Replace(source, "%record = OpVariable %record_pointer Function\n", "");
        Replace(source, "%record_backup = OpVariable %record_pointer Function\n", "");
        Replace(source, "%entry_type = OpTypeFunction",
                "%table = OpVariable %table_type Private\n%other = OpVariable %slot_type Private %null\n"
                "%record = OpVariable %record_pointer Private\n%record_backup = OpVariable %record_pointer Private\n"
                "%entry_type = OpTypeFunction");
    }
    if (stride_views) {
        // ArrayStride is optional on ordinary access-chain results. The two
        // real arrays below have different strides; both can use the same
        // unadorned pointer type. Restore each actual array's stride before
        // arithmetic, including after pointer-valued memory loads.
        for (size_t offset = 0; (offset = source.find("%word_ptr", offset)) != std::string::npos;) {
            source.replace(offset, 9, "%unstrided_word");
            offset += 15;
        }
        Replace(source, "OpDecorate %unstrided_word ArrayStride 4", "OpDecorate %word_ptr ArrayStride 4");
        Replace(source, "OpDecorate %array ArrayStride 4",
                "OpDecorate %array ArrayStride 4\nOpDecorate %array8 ArrayStride 8\nOpDecorate %word_ptr8 ArrayStride 8");
        Replace(source, "%block = OpTypeStruct %array %array",
                "%array8 = OpTypeArray %uint %count\n%block = OpTypeStruct %array %array8");
        Replace(source, "%unstrided_word = OpTypePointer StorageBuffer %uint",
                "%unstrided_word = OpTypePointer StorageBuffer %uint\n%word_ptr = OpTypePointer StorageBuffer %uint\n"
                "%word_ptr8 = OpTypePointer StorageBuffer %uint");
        Replace(source, "OpCapability Shader", "OpCapability Shader\nOpCapability Int64\nOpCapability ShaderNonUniform");
        Replace(source, "OpDecorate %array ArrayStride 4", "OpDecorate %a_drop NonUniform\nOpDecorate %array ArrayStride 4");
        Replace(source, "%float = OpTypeFloat 32",
                "%float = OpTypeFloat 32\n%uint64 = OpTypeInt 64 0\n"
                "%one64 = OpConstant %uint64 1\n%minus_one64 = OpConstant %uint64 18446744073709551615");
        Replace(source, "%a = OpAccessChain %unstrided_word %output %zero %base_index",
                "%a_origin = OpAccessChain %word_ptr %output %zero %base_index\n"
                "%a_drop = OpAccessChain %unstrided_word %a_origin\n%a = OpAccessChain %unstrided_word %a_drop");
        Replace(source, "%yes_pointer = OpLoad %unstrided_word %other",
                "%yes_raw = OpLoad %unstrided_word %other\n%yes_typed = OpAccessChain %word_ptr %yes_raw\n"
                "%yes_pointer = OpAccessChain %unstrided_word %yes_typed");
        Replace(source, "%no_pointer = OpLoad %unstrided_word %alias_copy",
                "%no_raw = OpLoad %unstrided_word %alias_copy\n%no_typed = OpAccessChain %word_ptr8 %no_raw\n"
                "%no_pointer = OpAccessChain %unstrided_word %no_typed");
        Replace(source, "%forward_a = OpPtrAccessChain %unstrided_word %pa %one",
                "%pa_typed = OpAccessChain %word_ptr %pa\n"
                "%forward_a = OpPtrAccessChain %unstrided_word %pa_typed %one64");
        Replace(source, "%back_a = OpPtrAccessChain %unstrided_word %forward_a %negative_one",
                "%forward_typed = OpAccessChain %word_ptr %forward_a\n"
                "%back_a = OpPtrAccessChain %unstrided_word %forward_typed %minus_one64");
        Replace(source, "%advance_a = OpPtrAccessChain %unstrided_word %back_a %one",
                "%back_typed = OpAccessChain %word_ptr %back_a\n"
                "%advance_a = OpPtrAccessChain %unstrided_word %back_typed %one64");
        Replace(source, "%advance_b = OpPtrAccessChain %unstrided_word %pb %one",
                "%pb_typed = OpAccessChain %word_ptr8 %pb\n"
                "%advance_b = OpPtrAccessChain %unstrided_word %pb_typed %one");
    }
    return source;
}
std::string QualifiedPointerSource(bool module_private, bool descriptor_array, bool independent_roots) {
    auto source = MutablePointerSource(module_private, true);
    if (independent_roots) {
        Replace(source, "OpCapability VariablePointersStorageBuffer", "OpCapability VariablePointers");
        Replace(source, "%launch %output", "%launch %output %output_b");
        Replace(source, "OpDecorate %output Binding 1",
                "OpDecorate %output Binding 1\nOpDecorate %output_b DescriptorSet 0\nOpDecorate %output_b Binding 2");
        Replace(source, "%output = OpVariable %block_ptr StorageBuffer",
                "%output = OpVariable %block_ptr StorageBuffer\n%output_b = OpVariable %block_ptr StorageBuffer");
        Replace(source, "%b = OpAccessChain %unstrided_word %output %one", "%b = OpAccessChain %unstrided_word %output_b %one");
    }
    Replace(source, "OpStore %alias_copy %selected", "OpStore %alias_copy %selected Volatile|Aligned|Nontemporal 1");
    Replace(source, "OpCopyMemory %other %alias", "OpCopyMemory %other %alias Volatile|Aligned 1 Nontemporal|Aligned 1");
    Replace(source, "OpCopyMemory %alias %other", "OpCopyMemory %alias %other Nontemporal|Aligned 1 Volatile|Aligned 1");
    Replace(source, "OpLoad %unstrided_word %other", "OpLoad %unstrided_word %other Volatile|Aligned 1");
    Replace(source, "OpLoad %unstrided_word %alias_copy", "OpLoad %unstrided_word %alias_copy Nontemporal|Aligned 1");
    Replace(source, "OpCopyMemory %record_backup %record", "OpCopyMemory %record_backup %record Volatile|Aligned|Nontemporal 1");
    Replace(source, "OpLoad %record_type %record", "OpLoad %record_type %record Volatile|Aligned|Nontemporal 1");
    Replace(source, "OpStore %record %record_copy", "OpStore %record %record_copy Volatile|Aligned|Nontemporal 1");
    Replace(source, "OpCopyMemory %record %record_backup", "OpCopyMemory %record %record_backup Aligned 1 Volatile|Nontemporal");
    Replace(source, "OpStore %round_slot %round", "OpStore %round_slot %round Aligned|Nontemporal 4");
    Replace(source, "%late_round = OpLoad %uint %round_slot", "%late_round = OpLoad %uint %round_slot Volatile|Aligned 4");
    Replace(source, "%old = OpLoad %uint %target", "%old = OpLoad %uint %target Volatile|Aligned|Nontemporal 4");
    Replace(source, "OpStore %target %value", "OpStore %target %value Volatile|Aligned|Nontemporal 4");
    if (module_private)
        Replace(source, "%entry_type = OpTypeFunction",
                "%helper_word_pointer = OpTypePointer Function %uint\n%entry_type = OpTypeFunction");
    Replace(source, "%helper_entry = OpLabel",
            "%helper_entry = OpLabel\n%helper_word = OpVariable " +
                std::string(module_private ? "%helper_word_pointer" : "%round_slot_type") + " Function");
    Replace(source, "%old = OpLoad %uint %target Volatile|Aligned|Nontemporal 4",
            "OpCopyMemory %helper_word %target Volatile|Aligned 4 Nontemporal|Aligned 4\n"
            "%old = OpLoad %uint %helper_word Volatile|Aligned 4");
    Replace(source, "OpStore %target %value Volatile|Aligned|Nontemporal 4",
            "OpStore %helper_word %value Nontemporal|Aligned 4\n"
            "OpCopyMemory %target %helper_word Volatile|Aligned 4 Nontemporal|Aligned 4");
    if (descriptor_array) {
        Require(independent_roots, "Descriptor-array matrix requires the independent-root fixture");
        Replace(source, "OpCapability ShaderNonUniform",
                "OpCapability ShaderNonUniform\nOpCapability StorageBufferArrayNonUniformIndexing");
        Replace(source, "OpDecorate %a_drop NonUniform",
                "OpDecorate %a_drop NonUniform\nOpDecorate %descriptor_a NonUniform\nOpDecorate %descriptor_b NonUniform");
        Replace(source, "%block_ptr = OpTypePointer StorageBuffer %block",
                "%descriptor_count = OpConstant %uint 3\n%descriptor_blocks = OpTypeArray %block %descriptor_count\n"
                "%block_ptr = OpTypePointer StorageBuffer %descriptor_blocks");
        if (module_private)
            Replace(source, "%entry_type = OpTypeFunction",
                    "%selector_pointer = OpTypePointer Function %uint\n%entry_type = OpTypeFunction");
        Replace(source, "%entry = OpLabel",
                "%entry = OpLabel\n%selector_slot = OpVariable " +
                    std::string(module_private ? "%selector_pointer" : "%round_slot_type") + " Function");
        // The selector's backing changes after the pointers are captured. The
        // identity must retain the original descriptor index through copies,
        // aliases and reports, rather than reload that changed backing later.
        Replace(source, "%base_index = OpIMul",
                "%initial_selector = OpUMod %uint %id %descriptor_count\nOpStore %selector_slot %initial_selector\n"
                "%descriptor_a = OpLoad %uint %selector_slot\n%id_next = OpIAdd %uint %id %one\n"
                "%descriptor_b = OpUMod %uint %id_next %descriptor_count\n%base_index = OpIMul");
        Replace(source, "%a_origin = OpAccessChain %word_ptr %output %zero",
                "%a_origin = OpAccessChain %word_ptr %output %descriptor_a %zero");
        Replace(source, "%b = OpAccessChain %unstrided_word %output_b %one",
                "%b = OpAccessChain %unstrided_word %output_b %descriptor_b %one");
        Replace(source, "%slot_index = OpIAdd",
                "%changed_selector = OpIAdd %uint %descriptor_a %one\nOpStore %selector_slot %changed_selector\n"
                "%slot_index = OpIAdd");
    }
    return source;
}

void CheckQualifiedPointerCode(const std::vector<uint32_t> &words, TypedFrameTransport transport) {
    std::map<uint32_t, uint32_t> storage, pointer_storage;
    uint32_t qualified_loads = 0, qualified_stores = 0, nonuniform_memory = 0;
    std::set<uint32_t> nonuniform;
    for (size_t p = 5; p < words.size(); p += words[p] >> 16) {
        const auto op = words[p] & 65535;
        if (op == spv::OpTypePointer) storage[words[p + 1]] = words[p + 2];
        if (op == spv::OpDecorate && words[p + 2] == spv::DecorationNonUniform) nonuniform.insert(words[p + 1]);
    }
    for (size_t p = 5; p < words.size(); p += words[p] >> 16) {
        const auto op = words[p] & 65535, size = words[p] >> 16;
        if ((op == spv::OpVariable || op == spv::OpAccessChain || op == spv::OpInBoundsAccessChain || op == spv::OpPtrAccessChain ||
             op == spv::OpConvertUToPtr || op == spv::OpCopyObject) &&
            storage.count(words[p + 1]))
            pointer_storage[words[p + 2]] = storage.at(words[p + 1]);
        if (op == spv::OpPhi || op == spv::OpSelect)
            Require(!storage.count(words[p + 1]) || storage.at(words[p + 1]) != spv::StorageClassStorageBuffer,
                    "Qualified pointer code merged native logical pointer values");
        if (op != spv::OpLoad && op != spv::OpStore) continue;
        const auto pointer = words[p + (op == spv::OpLoad ? 3 : 1)];
        if (nonuniform.count(pointer)) ++nonuniform_memory;
        const auto mask_word = op == spv::OpLoad ? 4u : 3u;
        if (size <= mask_word) continue;
        const auto mask = words[p + mask_word];
        if (!(mask & (spv::MemoryAccessVolatileMask | spv::MemoryAccessNontemporalMask))) continue;
        const auto actual_storage = pointer_storage.at(pointer);
        Require(actual_storage != spv::StorageClassFunction, "Source qualifiers leaked onto compiler Function scratch");
        const auto frame_storage = transport == TypedFrameTransport::PhysicalAddress ? spv::StorageClassPhysicalStorageBuffer
                                                                                     : spv::StorageClassStorageBuffer;
        if (actual_storage == frame_storage) {
            if (op == spv::OpLoad)
                ++qualified_loads;
            else
                ++qualified_stores;
            if (frame_storage == spv::StorageClassPhysicalStorageBuffer) {
                Require((mask & spv::MemoryAccessAlignedMask) && words[p + mask_word + 1] == 4,
                        "Relocated BDA memory inherited an unproved source alignment");
            }
        }
    }
    Require(qualified_loads >= 2 && qualified_stores >= 2 && nonuniform_memory,
            "Qualified typed identity accesses or actual NonUniform consumption are absent");
}

void CopyMemoryMaskSemantics() {
    const std::map<std::string, std::pair<uint32_t, uint32_t>> masks = {{"", {0, 0}},
                                                                        {"Aligned 4", {0, 0}},
                                                                        {"Volatile|Aligned 4", {1, 1}},
                                                                        {"Nontemporal|Aligned 4", {4, 4}},
                                                                        {"Volatile|Aligned 4 Nontemporal|Aligned 4", {1, 4}},
                                                                        {"Nontemporal|Aligned 4 Volatile|Aligned 4", {4, 1}},
                                                                        {"Volatile|Aligned|Nontemporal 4", {5, 5}}};
    for (const auto &[operands, expected] : masks) {
        const auto source = Source(R"spv(
OpCapability Shader
OpCapability RayTracingKHR
OpExtension "SPV_KHR_ray_tracing"
OpMemoryModel Logical GLSL450
OpEntryPoint RayGenerationKHR %main "main"
%void = OpTypeVoid
%uint = OpTypeInt 32 0
%one = OpConstant %uint 1
%pointer = OpTypePointer Function %uint
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%entry = OpLabel
%a = OpVariable %pointer Function %one
%b = OpVariable %pointer Function
OpCopyMemory %b %a )spv" + operands +
                                   "\nOpReturn\nOpFunctionEnd\n");
        for (const auto transport : {TypedFrameTransport::StorageBuffer, TypedFrameTransport::PhysicalAddress}) {
            TypedContinuationCompileOptions options;
            options.transport = transport;
            const auto program = CompileTypedContinuationSteps(source, "main", spv::ExecutionModelRayGenerationKHR, options);
            const auto &words = program.spirv;
            std::map<uint32_t, uint32_t> pointer_types, pointers, read_flags, write_flags;
            const auto storage = transport == TypedFrameTransport::PhysicalAddress ? spv::StorageClassPhysicalStorageBuffer
                                                                                   : spv::StorageClassStorageBuffer;
            for (size_t p = 5; p < words.size(); p += words[p] >> 16)
                if ((words[p] & 65535) == spv::OpTypePointer) pointer_types[words[p + 1]] = words[p + 2];
            uint32_t function = 0;
            for (size_t p = 5; p < words.size(); p += words[p] >> 16) {
                const auto op = words[p] & 65535, size = words[p] >> 16;
                if (op == spv::OpFunction) function = words[p + 2];
                if ((op == spv::OpAccessChain || op == spv::OpConvertUToPtr) && pointer_types.count(words[p + 1]))
                    pointers[words[p + 2]] = pointer_types.at(words[p + 1]);
                if (op == spv::OpLoad && pointers.count(words[p + 3]) && pointers.at(words[p + 3]) == storage)
                    read_flags[function] = size > 4 ? words[p + 4] & 5u : 0;
                if (op == spv::OpStore && pointers.count(words[p + 1]) && pointers.at(words[p + 1]) == storage)
                    write_flags[function] = size > 3 ? words[p + 3] & 5u : 0;
            }
            // Match the complete value dependency, not codec names: the
            // source-qualified load's result must be the value passed to the
            // target-qualified store. One CopyMemory mask applies to both;
            // two masks apply to target then source, respectively.
            std::map<uint32_t, uint32_t> loaded;
            bool copied = false;
            for (size_t p = 5; p < words.size(); p += words[p] >> 16) {
                if ((words[p] & 65535) != spv::OpFunctionCall) continue;
                const auto callee = words[p + 3];
                if (read_flags.count(callee)) loaded[words[p + 2]] = read_flags.at(callee);
                if ((words[p] >> 16) == 7 && write_flags.count(callee) && loaded.count(words[p + 6]))
                    copied |= write_flags.at(callee) == expected.first && loaded.at(words[p + 6]) == expected.second;
            }
            Require(copied, "CopyMemory lost source/target mask semantics: " + operands);
        }
    }
}

template <class T>
void PointerWrite(uint32_t *mapped, uint64_t offset, T value) {
    std::memcpy(reinterpret_cast<uint8_t *>(mapped) + offset, &value, sizeof(value));
}

bool QualifiedPointerMemory(bool execute, bool module_private, bool descriptor_array, TypedFrameTransport transport,
                            bool independent_roots = true) {
    constexpr uint32_t count = 16, resource_words = 384;
    const auto input = Source(QualifiedPointerSource(module_private, descriptor_array, independent_roots));
    TypedContinuationCompileOptions options;
    options.transport = transport;
    auto program = CompileTypedContinuationSteps(input, "main", spv::ExecutionModelIntersectionKHR, options);
    auto binary = program.spirv;
    if (descriptor_array) {
        bool changed = false;
        for (size_t p = 5; p < binary.size(); p += binary[p] >> 16)
            if ((binary[p] & 65535) == spv::OpExecutionMode && binary[p + 2] == spv::ExecutionModeLocalSize) {
                binary[p + 3] = count;
                changed = true;
            }
        Require(changed, "Descriptor-array fixture has no compute LocalSize");
    }
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
    Require(tools.Validate(binary), "Actual qualified pointer GPU module failed validation");
    CheckQualifiedPointerCode(binary, transport);
    Require(program.frames.options.logical_pointer_words >= (descriptor_array ? 5u : 4u),
            "Pointer memory did not retain complete numeric descriptor/index identity");
    if (!execute) return true;
    const bool bda = transport == TypedFrameTransport::PhysicalAddress;
    const auto frame_bytes = count * program.frame_stride_words * 4;
    const auto context = (frame_bytes + 255u) & ~255u;
    PointerGPU gpu;
    const char *name = bda ? "BDA" : "SSBO";
    std::cout << "Creating qualified " << name << " pointer memory: " << (module_private ? "Private" : "Function")
              << ", source roots=" << (independent_roots ? 2 : 1) << ", descriptors per root=" << (descriptor_array ? 3 : 1) << ", "
              << binary.size() << " SPIR-V words\n"
              << std::flush;
    const auto descriptors = descriptor_array ? 3u : 1u;
    if (!gpu.InitPointer(binary, context + 64, resource_words, descriptors, bda, context, independent_roots)) {
        std::cout << "Qualified pointer fixture skipped: Vulkan1.3/descriptor budget/shaderInt64/variablePointersStorageBuffer"
                  << (independent_roots ? "/variablePointers" : "") << (bda ? "/bufferDeviceAddress" : "")
                  << (descriptor_array ? "/storage-buffer dynamic and NonUniform descriptor-array indexing/16-lane compute subgroup"
                                       : "")
                  << " unavailable\n";
        return false;
    }
    const auto workgroups = descriptor_array ? 1u : count;
    for (uint32_t activation = 0; activation < 2; ++activation) {
        for (uint32_t root = 0; root < gpu.resources.size(); ++root)
            for (uint32_t word = 0; word < resource_words; ++word) gpu.resources[root].mapped[word] = 1000 + root * 10000 + word;
        if (bda) {
            PointerWrite(gpu.mapped, context + VEL_RT_CTX_FRAMES_ADDRESS, gpu.address);
            PointerWrite(gpu.mapped, context + VEL_RT_CTX_FRAME_STRIDE, program.frame_stride_bytes);
            PointerWrite(gpu.mapped, context + VEL_RT_CTX_FRAME_COUNT, uint64_t(count));
            PointerWrite(gpu.mapped, context + VEL_RT_CTX_FRAME_OFFSET, uint64_t(0));
            PointerWrite(gpu.mapped, context + VEL_RT_CTX_FRAME_CAPACITY, uint64_t(frame_bytes));
        }
        for (uint32_t id = 0; id < count; ++id) {
            auto *frame = gpu.mapped + id * program.frame_stride_words;
            std::fill_n(frame + program.control_word, kTypedContinuationControlWords, 0);
            if (bda) {
                frame[VEL_RT_SYS_LAUNCH_ID / 4] = id;
                frame[VEL_RT_SYS_LAUNCH_ID / 4 + 1] = frame[VEL_RT_SYS_LAUNCH_ID / 4 + 2] = 0;
            }
        }
        for (uint32_t round = 0; round < 4; ++round) {
            gpu.Dispatch(0, 0, workgroups);
            for (uint32_t id = 0; id < count; ++id) {
                auto *control = gpu.mapped + id * program.frame_stride_words + program.control_word;
                if (round == 3) {
                    Require(control[0] == UINT32_MAX, "Qualified pointer memory did not finish");
                    continue;
                }
                float distance;
                std::memcpy(&distance, control + 4, 4);
                Require(control[1] == 2 && control[2] == 0 && distance == float(round) + .5f && control[5] == id + round,
                        "Qualified pointer report order/arguments changed");
                const auto descriptor = descriptor_array ? ((id & 1) ? (id + 1) % 3 : id % 3) : 0;
                const auto root = independent_roots ? (id & 1) * descriptors + descriptor : 0;
                const auto element = id * 4 + round;
                const auto word = (id & 1) ? 128 + element * 2 : element;
                // Change the resource after its pre-report copy/read.
                // Resumption must retain the old value, not replay a late
                // native load from this now-poisoned word.
                gpu.resources[root].mapped[word] = 0xabcd0000u + id * 4 + round;
                control[3] = (id + round) % 3 != 0;
                control[2] = 1;
            }
        }
        for (uint32_t root = 0; root < gpu.resources.size(); ++root) {
            std::vector<uint32_t> expected(resource_words);
            for (uint32_t word = 0; word < resource_words; ++word) expected[word] = 1000 + root * 10000 + word;
            for (uint32_t id = 0; id < count; ++id) {
                const auto descriptor = descriptor_array ? ((id & 1) ? (id + 1) % 3 : id % 3) : 0;
                if (root != (independent_roots ? (id & 1) * descriptors + descriptor : 0)) continue;
                for (uint32_t round = 0; round < 3u; ++round) {
                    const auto element = id * 4 + round;
                    const auto word = (id & 1) ? 128 + element * 2 : element;
                    expected[word] += round + 1 + ((id + round) % 3 != 0 ? 7 : 11);
                }
            }
            Require(std::equal(expected.begin(), expected.end(), gpu.resources[root].mapped),
                    std::string(name) + " qualified pointer identity/memory flags/descriptor snapshot corrupted root " +
                        std::to_string(root));
        }
    }
    std::cout << "Qualified " << name << " pointer memory executed with " << (independent_roots ? gpu.resources.size() : 1)
              << " source VkBuffers, all " << gpu.resources.size() * resource_words
              << " words checked including padding; Volatile/Aligned/Nontemporal and CopyMemory preserved\n";
    return true;
}
bool QualifiedPointerMatrix(bool execute, bool transport_only = false, bool enhanced_only = false) {
    if (!enhanced_only)
        for (bool module_private : {false, true})
            if (!QualifiedPointerMemory(execute, module_private, false, TypedFrameTransport::PhysicalAddress, false)) return false;
    if (transport_only) return true;
    for (const auto transport : {TypedFrameTransport::StorageBuffer, TypedFrameTransport::PhysicalAddress})
        if (!QualifiedPointerMemory(execute, false, false, transport)) return false;
    for (const auto transport : {TypedFrameTransport::StorageBuffer, TypedFrameTransport::PhysicalAddress})
        if (!QualifiedPointerMemory(execute, false, true, transport)) return false;
    return true;
}
bool MutablePointerMemory(bool execute, bool module_private, bool stride_views = false) {
    // SDK357's source validator misses this pointer decoration mismatch. A
    // zero advance still names an element of the original stride4 array; its
    // result cannot claim that array has stride8. Keep the explicit compiler
    // diagnostic and finite route planning for this invalid source layout.
    std::string stride_cycle(kPointerSource);
    Replace(stride_cycle, "OpDecorate %array ArrayStride 4",
            "OpDecorate %word_ptr ArrayStride 4\nOpDecorate %stride_view_type ArrayStride 8\nOpDecorate %array ArrayStride 4");
    Replace(stride_cycle, "%word_ptr = OpTypePointer StorageBuffer %uint",
            "%word_ptr = OpTypePointer StorageBuffer %uint\n%stride_view_type = OpTypePointer StorageBuffer %uint");
    Replace(stride_cycle, "%pa = OpPhi %word_ptr %nonnull %entry %pb %continue",
            "%pa = OpPhi %word_ptr %nonnull %entry %stride_back %continue");
    Replace(stride_cycle, "%continue = OpLabel",
            "%continue = OpLabel\n%stride_view = OpPtrAccessChain %stride_view_type %pa %zero\n"
            "%stride_back = OpPtrAccessChain %word_ptr %stride_view %zero");
    bool stride_rejected = false;
    try {
        CompileTypedContinuationSteps(Source(stride_cycle), "main", spv::ExecutionModelIntersectionKHR);
    } catch (const std::exception &error) {
        Require(std::string(error.what()).find("ArrayStride changes require a matching array origin") != std::string::npos,
                "Stride-changing pointer view rejected for an unrelated reason: " + std::string(error.what()));
        stride_rejected = true;
    }
    Require(stride_rejected, "Stride-changing pointer cycle entered unbounded route planning");
    std::string indirect_stride_cycle(stride_cycle);
    Replace(indirect_stride_cycle, "%stride_view_type = OpTypePointer StorageBuffer %uint",
            "%stride_view_type = OpTypePointer StorageBuffer %uint\n%bare_view_type = OpTypePointer StorageBuffer %uint");
    Replace(indirect_stride_cycle,
            "%stride_view = OpPtrAccessChain %stride_view_type %pa %zero\n"
            "%stride_back = OpPtrAccessChain %word_ptr %stride_view %zero",
            "%bare_a = OpAccessChain %bare_view_type %pa\n%stride_view = OpAccessChain %stride_view_type %bare_a\n"
            "%bare_b = OpAccessChain %bare_view_type %stride_view\n%stride_back = OpAccessChain %word_ptr %bare_b");
    stride_rejected = false;
    try {
        CompileTypedContinuationSteps(Source(indirect_stride_cycle), "main", spv::ExecutionModelIntersectionKHR);
    } catch (const std::exception &error) {
        Require(std::string(error.what()).find("ArrayStride disagrees with its originating array") != std::string::npos,
                "An unadorned view hid the original array layout: " + std::string(error.what()));
        stride_rejected = true;
    }
    Require(stride_rejected, "Unadorned intermediate view accepted a mismatching restored array stride");
    if (!module_private && !stride_views) {
        auto volatile_copy = MutablePointerSource(false);
        Replace(volatile_copy, "OpCopyMemory %other %alias", "OpCopyMemory %other %alias Volatile|Aligned|Nontemporal 1");
        CompileTypedContinuationSteps(Source(volatile_copy), "main", spv::ExecutionModelIntersectionKHR);
    }
    const auto words = Source(MutablePointerSource(module_private, stride_views));
    const auto program = CompileTypedContinuationSteps(words, "main", spv::ExecutionModelIntersectionKHR);
    Require(program.frames.options.logical_pointer_words >= 4 && program.frame_stride_bytes > 8193 * 16,
            "Pointer memory/arithmetic lacks checked selector identity backing");
    if (stride_views) {
        std::set<uint32_t> decorated;
        for (size_t p = 5; p < program.spirv.size(); p += program.spirv[p] >> 16)
            if ((program.spirv[p] & 65535) == spv::OpDecorate && program.spirv[p + 2] == spv::DecorationNonUniform)
                decorated.insert(program.spirv[p + 1]);
        bool nonuniform_load = false;
        for (size_t p = 5; p < program.spirv.size(); p += program.spirv[p] >> 16) {
            const auto op = program.spirv[p] & 65535;
            if (op == spv::OpLoad && decorated.count(program.spirv[p + 3])) nonuniform_load = true;
        }
        Require(nonuniform_load, "Stride-view recovery lost NonUniform on the actual memory access");
    }
    const auto physical =
        CompileTypedPipelineStage(words, "main", VK_SHADER_STAGE_INTERSECTION_BIT_KHR, nullptr, "mutable_pointer_bda");
    Require(
        physical.pointer_snapshot_bytes == program.pointer_snapshot_bytes && physical.system_header_bytes == VEL_RT_SYSTEM_BYTES,
        "Mutable pointer memory lost typed BDA identity backing");
    if (!execute) return true;
    constexpr uint32_t count = 16, array_size = 128;
    const auto output_words = array_size * (stride_views ? 3u : 2u);
    const auto frame_bytes = program.frame_stride_words * count * 4;
    const auto output_offset = (frame_bytes + 255u) & ~255u;
    GPU gpu;
    std::cout << "Creating mutable logical " << (module_private ? "Private" : "Function") << (stride_views ? " stride views" : "")
              << " pointer GPU pipeline: " << program.spirv.size() << " words, frame " << program.frame_stride_bytes << " bytes\n"
              << std::flush;
    if (!gpu.Init(program.spirv, output_offset + output_words * 4, output_offset, true, frame_bytes, VK_WHOLE_SIZE, true)) {
        std::cout << "Mutable-pointer GPU fixture skipped: required scalar/variable-pointer features unavailable\n";
        return false;
    }
    for (uint32_t activation = 0; activation < 2; ++activation) {
        for (uint32_t id = 0; id < count; ++id)
            std::fill_n(gpu.mapped + id * program.frame_stride_words + program.control_word, 7, 0);
        for (uint32_t n = 0; n < output_words; ++n) gpu.mapped[output_offset / 4 + n] = 1000 + n;
        for (uint32_t round = 0; round < 4; ++round) {
            gpu.Dispatch(0, 0, count);
            for (uint32_t id = 0; id < count; ++id) {
                auto *control = gpu.mapped + id * program.frame_stride_words + program.control_word;
                if (round == 3) {
                    Require(control[0] == UINT32_MAX, "Mutable pointer loop did not terminate");
                    continue;
                }
                float distance;
                std::memcpy(&distance, control + 4, 4);
                Require(control[1] == 2 && control[2] == 0 && distance == float(round) + 0.5f && control[5] == id + round,
                        "Mutable pointer helper lost report order/arguments");
                control[3] = (id + round) % 3 != 0;
                control[2] = 1;
            }
        }
        std::vector<uint32_t> expected(output_words);
        for (uint32_t n = 0; n < expected.size(); ++n) expected[n] = 1000 + n;
        for (uint32_t id = 0; id < count; ++id)
            for (uint32_t round = 0; round < 3; ++round) {
                const auto element = id * 4 + round;
                const auto index = (id & 1) * array_size + element * (stride_views && (id & 1) ? 2 : 1);
                expected[index] += round + 1 + ((id + round) % 3 != 0 ? 7 : 11);
            }
        Require(std::equal(expected.begin(), expected.end(), gpu.mapped + output_offset / 4),
                "Pointer memory alias/aggregate or signed PtrAccessChain changed independent expected writes");
    }
    std::cout << "Mutable pointer " << (module_private ? "Private" : "Function")
              << (stride_views ? " actual stride4/8 through unadorned pointer views" : "")
              << " storage executed: 8193-cell array, dynamic indices above4096, aliases/branch stores, "
                 "pointer/aggregate CopyMemory across suspension, signed loop-carried PtrAccessChain\n";
    return true;
}
struct Event {
    float distance;
    uint32_t kind;
};
struct Oracle {
    std::vector<Event> events;
    uint32_t array[3]{5, 7, 11}, shadow[3]{5, 7, 11}, result = 0;
    Oracle(uint32_t id, uint32_t base_limit) {
        uint32_t a = 2, b = 3;
        auto index = id % 3;
        for (uint32_t i = 0; i < base_limit + index; ++i) {
            events.push_back({float(i) + 0.5f, i + 1});
            std::copy(std::begin(array), std::end(array), std::begin(shadow));
            bool first = (id + i + 1) % 3 != 0;
            array[index] += i + (first ? 5 : 1);
            result += (first ? 3 : 7) + array[index] + a;
            result += first ? 2 : 3;
            if (i & 1) {
                events.push_back({float(i) + 1.25f, i + 64});
                result += (id + i + 64) % 3 != 0 ? 11 : 13;
            }
            std::swap(a, b);
        }
        result += index == 0 ? 17 : index == 1 ? 23 : 31;
        result += id + 100;
    }
};
void VerifyPayload(const uint32_t *value, const uint32_t *array, uint32_t result) {
    Require(value[0] == 17 && value[1] == 23 && value[2] == 31, "Aggregate vector changed");
    Require(std::equal(array, array + 3, value + 3), "Aggregate/local alias state mismatch");
    Require(value[6] == 1 && value[7] == result, "Aggregate bool/result mismatch");
}
#include "state_physical_pointer_tests.h"
}  // namespace

int main(int argc, char **argv) {
    try {
        const bool compile_only = argc == 2 && std::string(argv[1]) == "--compile-only";
        const bool pointer_only = argc == 2 && std::string(argv[1]) == "--pointer-only";
        const bool qualified_only = argc == 2 && std::string(argv[1]) == "--qualified-pointer-only";
        const bool transport_only = argc == 2 && std::string(argv[1]) == "--transport-pointer-only";
        Require(
            argc == 1 || compile_only || pointer_only || qualified_only || transport_only,
            "Usage: vel_rt_state_compiler_tests [--compile-only|--pointer-only|--qualified-pointer-only|--transport-pointer-only]");
        if (pointer_only || qualified_only || transport_only) {
            CopyMemoryMaskSemantics();
            if (pointer_only) {
                for (bool array_pointers : {false, true})
                    if (!LogicalPointerSnapshots(true, array_pointers)) return 77;
                for (bool module_private : {false, true})
                    if (!MutablePointerMemory(true, module_private)) return 77;
                for (bool module_private : {false, true})
                    if (!MutablePointerMemory(true, module_private, true)) return 77;
            }
            if (!QualifiedPointerMatrix(true, !qualified_only, qualified_only)) return 77;
            return 0;
        }
        BoundariesAndSourcePreservation();
        PhysicalSourceSemantics();
        LargeDescriptorResourceTypes();
        ResourceValueTypes();
        SourcePrivateFrameBounds();
        OpaqueResourceBoundaries();
        ExactCtsImageAtomicStage();
        for (bool array_pointers : {false, true})
            if (!LogicalPointerSnapshots(!compile_only, array_pointers)) return 77;
        for (bool module_private : {false, true})
            if (!MutablePointerMemory(!compile_only, module_private)) return 77;
        for (bool module_private : {false, true})
            if (!MutablePointerMemory(!compile_only, module_private, true)) return 77;
        CopyMemoryMaskSemantics();
        if (!QualifiedPointerMatrix(!compile_only, !compile_only)) return 77;
        if (compile_only) {
            std::cout << "Typed source and logical pointer compile/validation checks passed; GPU execution not requested\n";
            return 0;
        }
        for (bool module_private : {false, true}) {
            std::string source = kSource;
            if (module_private) {
                Replace(source, "%launch %config %output", "%launch %config %output %local %untouched");
                Replace(source, "%entry_type = OpTypeFunction",
                        "%private_ptr = OpTypePointer Private %payload\n%private_word = OpTypePointer Private %u32\n"
                        "%local = OpVariable %private_ptr Private %initial\n%untouched = OpVariable %private_word Private\n"
                        "%entry_type = OpTypeFunction");
                Replace(source, "%local = OpVariable %local_ptr Function\n", "");
                Replace(source, "%untouched = OpVariable %local_word Function\n", "");
                Replace(source, "%alias = OpAccessChain %local_word", "%alias = OpAccessChain %private_word");
                Replace(source, "%result_ptr = OpAccessChain %local_word", "%result_ptr = OpAccessChain %private_word");
                Replace(source, "OpStore %local %initial\n", "");
                Replace(source, "OpStore %external_ptr %seed_added\n",
                        "OpStore %external_ptr %seed_added\n%uninitialized_value = OpLoad %u32 %untouched\n");
                for (const char *initializer : {"OpUndef", "OpConstantNull"}) {
                    std::string undefined = source;
                    Replace(undefined, "%local = OpVariable",
                            std::string("%private_initializer = ") + initializer + " %payload\n%local = OpVariable");
                    if (std::string(initializer) == "OpUndef")
                        Replace(undefined, "%launch_id = OpLoad", "OpStore %local %private_initializer\n%launch_id = OpLoad");
                    else
                        Replace(undefined, "%private_ptr Private %initial", "%private_ptr Private %private_initializer");
                    auto lowered = CompileTypedContinuationSteps(Source(undefined), "main", spv::ExecutionModelIntersectionKHR);
                    Require(lowered.private_variables.size() == 2,
                            "Private null initializer/undefined composite store backing lost");
                }
            }
            auto program = CompileTypedContinuationSteps(Source(source), "main", spv::ExecutionModelIntersectionKHR);
            Require(program.private_variables.size() == (module_private ? 2u : 0u), "Module-Private backing metadata missing");
            if (module_private) {
                auto physical = CompileTypedPipelineStage(Source(source), "main", VK_SHADER_STAGE_INTERSECTION_BIT_KHR, nullptr,
                                                          "private_globals_bda");
                Require(physical.private_variables.size() == 2 && physical.system_header_bytes == VEL_RT_SYSTEM_BYTES,
                        "Source module-Private storage lost its BDA backing");
            }
            std::string text;
            spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
            Require(tools.Disassemble(program.spirv, &text), "Generated module disassembly failed");
            Require(text.find("launch%123\\\"%456") != std::string::npos,
                    "SPIR-V literal percent IDs/escaped quote were rewritten");
            Require(program.state_count > 12, "Source CFG was not split into block/resume states");
            const auto &frame = program.frames.functions.at(0);
            Require(frame.suspensions.size() == 2, "Reachable helper report was not lowered");
            std::map<std::string, uint32_t> names;
            for (size_t p = 5; p < program.source.size(); p += program.source[p] >> 16)
                if ((program.source[p] & 0xffff) == spv::OpName)
                    names[reinterpret_cast<const char *>(&program.source[p + 2])] = program.source[p + 1];
            const auto first_result = names.at("first_accepted");
            Require(std::find(frame.suspensions[0].saved_values.begin(), frame.suspensions[0].saved_values.end(), first_result) ==
                            frame.suspensions[0].saved_values.end() &&
                        std::find(frame.suspensions[1].saved_values.begin(), frame.suspensions[1].saved_values.end(),
                                  first_result) != frame.suspensions[1].saved_values.end(),
                    "Suspension bool was saved before creation or not preserved at a later pause");
            auto offset = [&](const char *name) -> uint64_t {
                for (const auto &s : frame.slots)
                    if (s.value == names.at(name)) return s.offset;
                for (const auto &s : program.private_variables)
                    if (s.variable == names.at(name)) return s.offset;
                throw std::runtime_error("Missing source variable backing");
            };
            constexpr uint32_t count = 32, base_limit = 7;
            std::vector<Oracle> expected;
            for (uint32_t i = 0; i < count; ++i) expected.emplace_back(i, base_limit);
            GPU gpu;
            auto frame_bytes = program.frame_stride_words * count * 4;
            auto output_offset = (frame_bytes + 255u) & ~255u;
            if (!gpu.Init(program.spirv, output_offset + count * 4, output_offset, false, frame_bytes)) {
                std::cout << "Generated source CFG SPIR-V validated; Vulkan execution unavailable\n";
                return 77;
            }
            std::vector<uint32_t> complete;
            // Reuse the same backing for a second logical activation. The initialized
            // aggregate must reset at entry, while all resumed states preserve it.
            for (uint32_t activation = 0; activation < 2; ++activation) {
                for (uint32_t i = 0; i < count; ++i)
                    std::fill_n(gpu.mapped + i * program.frame_stride_words + program.control_word, 7, 0);
                for (uint32_t i = 0; i < count; ++i) gpu.mapped[output_offset / 4 + i] = i + 100;
                std::vector<uint32_t> event_index(count, 0);
                gpu.Dispatch(base_limit, 0, count);
                // A second dispatch without completing the first request must not
                // speculate through the report, mutate source locals, or expose bool.
                std::vector<uint32_t> waiting(gpu.mapped, gpu.mapped + output_offset / 4 + count);
                gpu.Dispatch(base_limit, 0, count);
                Require(std::equal(waiting.begin(), waiting.end(), gpu.mapped),
                        "Source resumed before suspension result was ready");
                // The source SSBO load has already occurred. Its typed SSA value must
                // survive pauses; rematerializing that load would observe this poison.
                for (uint32_t i = 0; i < count; ++i) gpu.mapped[output_offset / 4 + i] = 0xdeadbeef;
                bool finished = false;
                for (uint32_t round = 0; round < 128; ++round) {
                    finished = true;
                    for (uint32_t id = 0; id < count; ++id) {
                        auto control = gpu.mapped + id * program.frame_stride_words + program.control_word;
                        if (control[0] == UINT32_MAX) continue;
                        finished = false;
                        Require(control[1] == 2 && control[2] == 0, "Invalid generated request state");
                        Require(event_index[id] < expected[id].events.size(), "Source emitted excess suspension");
                        const auto &event = expected[id].events[event_index[id]++];
                        float distance;
                        std::memcpy(&distance, control + 4, 4);
                        Require(distance == event.distance && control[5] == event.kind, "Source ALU/request arguments changed");
                        Require(control[6] == frame.suspensions[0].word_offset || control[6] == frame.suspensions[1].word_offset,
                                "Request lost source-site identity");
                        control[3] = (id + event.kind) % 3 != 0;
                        control[2] = 1;
                    }
                    if (finished) break;
                    gpu.Dispatch(base_limit, 0, count);
                }
                Require(finished, "Generated source state machine did not terminate");
                for (uint32_t id = 0; id < count; ++id) {
                    auto words = gpu.mapped + id * program.frame_stride_words;
                    Require(event_index[id] == expected[id].events.size(), "Source skipped a suspension");
                    VerifyPayload(words + offset("local") / 4, expected[id].array, expected[id].result);
                    VerifyPayload(words + offset("shadow") / 4, expected[id].shadow, 0);
                    Require(gpu.mapped[output_offset / 4 + id] == expected[id].result, "Source SSBO binding/typed store changed");
                    Require(words[offset("untouched") / 4] == 0xcdcdcdcd, "Source uninitialized local was speculatively copied");
                }
                complete.assign(gpu.mapped, gpu.mapped + output_offset / 4 + count);
                gpu.Dispatch(base_limit + 1000, 0, count + 5);
                Require(std::equal(complete.begin(), complete.end(), gpu.mapped), "Source terminal state executed again");
            }
            // An undersized frame descriptor must also skip invocation zero before
            // touching its PC, local storage, or ordinary source output resource.
            VkDescriptorBufferInfo tiny{gpu.buffer, 0, (program.frame_stride_words - 1) * 4};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = gpu.set;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &tiny;
            vkUpdateDescriptorSets(gpu.device, 1, &write, 0, nullptr);
            std::fill_n(gpu.mapped + program.control_word, 7, 0);
            complete.assign(gpu.mapped, gpu.mapped + output_offset / 4 + count);
            gpu.Dispatch(base_limit, 0, count + 5);
            Require(std::equal(complete.begin(), complete.end(), gpu.mapped),
                    "Undersized frame descriptor bypassed kernel bounds guard");
            std::cout
                << "Compiled source CFG " << (module_private ? "module-Private" : "Function")
                << " storage validated and executed: 32 invocations, dynamic loops/branches, parallel Phi swaps, "
                   "inlined suspending helper, typed aggregates, persistent aliases, repeated real suspend/resume and late bool "
                   "results\n";
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
