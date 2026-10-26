// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "native_fixture.h"
#include "../omm_query_rewrite.h"
#include <spirv-tools/libspirv.hpp>
#include <string>

static void require(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
static std::vector<uint32_t> read(const char* filename) {
    std::ifstream source(filename, std::ios::binary | std::ios::ate);
    require(bool(source), "open source SPIR-V");
    const auto bytes = source.tellg();
    require(bytes > 0 && bytes % 4 == 0, "aligned SPIR-V");
    std::vector<uint32_t> words(size_t(bytes) / 4);
    source.seekg(0);
    require(bool(source.read(reinterpret_cast<char*>(words.data()), bytes)), "read SPIR-V");
    return words;
}
int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) return 1;
    auto source = read(argv[1]);
    const auto saved = source;
    vel::omm::QueryStateModule modules[2];
    for (unsigned mode = 0; mode < 2; ++mode) {
        std::string message;
        auto status = vel::omm::prepare_query_state(source, mode != 0, &modules[mode], &message);
        if (status != vel::omm::QueryRewriteStatus::Success) std::fprintf(stderr, "%s\n", message.c_str());
        require(status == vel::omm::QueryRewriteStatus::Success, "rewrite valid query arrays and helper parameters");
        require(source == saved, "source immutability");
        require(!modules[mode].state.empty() && modules[mode].initializations && modules[mode].flag_getters,
                "query state bindings and operations");
        auto malformed = source;
        malformed[0] = 0;
        const auto previous = modules[mode].words;
        require(
            vel::omm::prepare_query_state(malformed, mode != 0, &modules[mode]) == vel::omm::QueryRewriteStatus::InvalidModule &&
                modules[mode].words == previous,
            "invalid source leaves output unchanged");
    }
    spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
    std::vector<uint32_t> unrelated;
    require(tools.Assemble("OpCapability Shader\nOpMemoryModel Logical GLSL450\nOpEntryPoint GLCompute %main \"main\"\n"
                           "OpExecutionMode %main LocalSize 1 1 1\n%void = OpTypeVoid\n%fn = OpTypeFunction %void\n"
                           "%main = OpFunction %void None %fn\n%label = OpLabel\nOpReturn\nOpFunctionEnd\n",
                           &unrelated),
            "assemble unrelated shader");
    vel::omm::QueryStateModule unchanged;
    require(vel::omm::prepare_query_state(unrelated, true, &unchanged) == vel::omm::QueryRewriteStatus::Success &&
                unchanged.words == unrelated && unchanged.state.empty(),
            "unrelated module unchanged");
    std::puts("Query state SPIR-V validation, arrays, helper parameters, immutability: PASS");
    if (argc == 2) return 0;
    Fixture fixture;
    if (!fixture.open()) return 77;
    auto input = fixture.buffer(
        16, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    std::memset(input.mapped, 0, 16);
    auto geometry = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances =
        init<VkAccelerationStructureGeometryInstancesDataKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR);
    geometry.geometry.instances.data.deviceAddress = input.address;
    auto tlas = fixture.acceleration(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, geometry, 0);
    auto command = fixture.begin();
    const auto* range = &tlas.range;
    fixture.buildAS(command, 1, &tlas.build, &range);
    Fixture::barrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                     VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    fixture.submit(command);
    auto output = fixture.buffer(7 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    auto descriptorInfo = init<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    descriptorInfo.bindingCount = 2;
    descriptorInfo.pBindings = bindings;
    VkDescriptorSetLayout descriptorLayout;
    CHECK(vkCreateDescriptorSetLayout(fixture.device, &descriptorInfo, nullptr, &descriptorLayout));
    auto layoutInfo = init<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorLayout;
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(fixture.device, &layoutInfo, nullptr, &layout));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    auto poolInfo = init<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(fixture.device, &poolInfo, nullptr, &pool));
    auto allocate = init<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    allocate.descriptorPool = pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &descriptorLayout;
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(fixture.device, &allocate, &set));
    auto asWrite =
        init<VkWriteDescriptorSetAccelerationStructureKHR>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR);
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &tlas.handle;
    VkDescriptorBufferInfo bufferInfo = {output.handle, 0, 7 * sizeof(uint32_t)};
    VkWriteDescriptorSet writes[] = {init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET),
                                     init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET)};
    for (auto& write : writes) {
        write.dstSet = set;
        write.descriptorCount = 1;
    }
    writes[0].pNext = &asWrite;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(fixture.device, 2, writes, 0, nullptr);
    const uint32_t expected[] = {1, 128, 1, 64, 2, 128};
    for (auto& transformed : modules) {
        auto create = init<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        create.codeSize = transformed.words.size() * 4;
        create.pCode = transformed.words.data();
        VkShaderModule module;
        CHECK(vkCreateShaderModule(fixture.device, &create, nullptr, &module));
        auto pipelineInfo = init<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
        pipelineInfo.layout = layout;
        pipelineInfo.stage = init<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module;
        pipelineInfo.stage.pName = "main";
        VkPipeline pipeline;
        CHECK(vkCreateComputePipelines(fixture.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
        for (uint32_t seed = 0; seed < 2; ++seed) {
            std::memset(output.mapped, 0xff, 7 * sizeof(uint32_t));
            *static_cast<uint32_t*>(output.mapped) = seed;
            command = fixture.begin();
            Fixture::barrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            vkCmdDispatch(command, 1, 1, 1);
            Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             VK_ACCESS_HOST_READ_BIT);
            fixture.submit(command);
            require(!std::memcmp(static_cast<uint32_t*>(output.mapped) + 1, expected, sizeof(expected)),
                    "native GPU preserves independent logical flags after helper reinitialization");
        }
        vkDestroyPipeline(fixture.device, pipeline, nullptr);
        vkDestroyShaderModule(fixture.device, module, nullptr);
    }
    vkDestroyDescriptorPool(fixture.device, pool, nullptr);
    vkDestroyPipelineLayout(fixture.device, layout, nullptr);
    vkDestroyDescriptorSetLayout(fixture.device, descriptorLayout, nullptr);
    std::puts("Native GPU query arrays/helpers/reinitialization in both flag modes: PASS");
    return 0;
}
