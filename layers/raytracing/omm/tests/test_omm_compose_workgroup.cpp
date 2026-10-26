// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "../omm_shader_compose.h"
#include "native_fixture.h"
#include <spirv/unified1/spirv.hpp>
#include <string>

namespace {
void require(bool value, const std::string& message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        std::exit(1);
    }
}
std::vector<uint32_t> read(const char* path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    require(bool(stream), "Open SPIR-V file");
    const size_t bytes = size_t(stream.tellg());
    require(bytes && bytes % 4 == 0, "SPIR-V byte count");
    std::vector<uint32_t> words(bytes / 4);
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(words.data()), bytes);
    require(bool(stream), "Read SPIR-V bytes");
    return words;
}
}  // namespace
int main(int argc, char** argv) {
    require(argc == 3, "Expected application and production library SPIR-V paths");
    const auto application = read(argv[1]), library = read(argv[2]);
    uint32_t classify = 0;
    for (size_t offset = 5; offset < library.size(); offset += library[offset] >> 16)
        if (spv::Op(library[offset] & 0xffffu) == spv::OpName &&
            std::string(reinterpret_cast<const char*>(library.data() + offset + 2)).find("ommClassify(") == 0)
            classify = library[offset + 1];
    require(classify != 0, "Locate production classifier");
    vel::omm::ComposedQueryShader composed;
    std::string diagnostic;
    const auto status = vel::omm::compose_query_classifier(application, library, classify, &composed, &diagnostic);
    require(status == vel::omm::ShaderCompositionStatus::Success, "Production classifier composition: " + diagnostic);
    Fixture fixture;
    if (!fixture.open()) return 77;
    auto output = fixture.buffer(8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    std::memset(output.mapped, 0, 8);
    VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    auto setInfo = init<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    setInfo.bindingCount = 1;
    setInfo.pBindings = &binding;
    VkDescriptorSetLayout setLayout;
    CHECK(vkCreateDescriptorSetLayout(fixture.device, &setInfo, nullptr, &setLayout));
    auto layoutInfo = init<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(fixture.device, &layoutInfo, nullptr, &layout));
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    auto poolInfo = init<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(fixture.device, &poolInfo, nullptr, &pool));
    auto allocate = init<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    allocate.descriptorPool = pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &setLayout;
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(fixture.device, &allocate, &set));
    VkDescriptorBufferInfo bufferInfo{output.handle, 0, 8};
    auto write = init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    write.dstSet = set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(fixture.device, 1, &write, 0, nullptr);
    auto shaderInfo = init<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    shaderInfo.codeSize = composed.words.size() * 4;
    shaderInfo.pCode = composed.words.data();
    VkShaderModule shader;
    CHECK(vkCreateShaderModule(fixture.device, &shaderInfo, nullptr, &shader));
    auto pipelineInfo = init<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    pipelineInfo.layout = layout;
    pipelineInfo.stage = init<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shader;
    pipelineInfo.stage.pName = "main";
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(fixture.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
    auto command = fixture.begin();
    Fixture::barrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(command, 1, 1, 1);
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                     VK_ACCESS_HOST_READ_BIT);
    fixture.submit(command);
    const auto* result = static_cast<uint32_t*>(output.mapped);
    require(result[0] == 8 && result[1] == 28, "Helper must preserve application's 4x2x1 native GPU workgroup");
    vkDestroyPipeline(fixture.device, pipeline, nullptr);
    vkDestroyShaderModule(fixture.device, shader, nullptr);
    vkDestroyDescriptorPool(fixture.device, pool, nullptr);
    vkDestroyPipelineLayout(fixture.device, layout, nullptr);
    vkDestroyDescriptorSetLayout(fixture.device, setLayout, nullptr);
    std::puts("Production classifier composition: actual GPU 8 invocations, local-index sum 28; application workgroup preserved");
}
