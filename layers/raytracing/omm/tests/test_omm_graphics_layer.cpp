// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
// Public loader regression. Layer selection/order is supplied by the runner.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <vulkan/vulkan.h>
static VkResult graphicsInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
static void graphicsDestroyInstance(VkInstance, const VkAllocationCallbacks*);
static VkResult graphicsDevice(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
#define vkCreateInstance graphicsInstance
#define vkDestroyInstance graphicsDestroyInstance
#define vkCreateDevice graphicsDevice
#include "native_fixture.h"
#undef vkCreateDevice
#undef vkDestroyInstance
#undef vkCreateInstance
#include <array>
#include <atomic>
#include <string>

namespace {
std::atomic<unsigned> validation_errors{0}, validation_warnings{0};
VkDebugUtilsMessengerEXT messenger{};
bool graphics_supported = false;
unsigned checks = 0, failures = 0;
void expect(bool value, const char* message) {
    ++checks;
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL graphics loader: %s\n", message);
    }
}
template <typename T>
T proc(VkDevice device, const char* name) {
    return reinterpret_cast<T>(vkGetDeviceProcAddr(device, name));
}
VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (!(type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)))
        return VK_FALSE;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++validation_errors;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++validation_warnings;
    std::fprintf(stderr, "Validation %s: %s\n", severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "Error" : "Warning",
                 data->pMessage);
    return VK_FALSE;
}
std::vector<uint32_t> read_shader(const char* path) {
    std::ifstream source(path, std::ios::binary | std::ios::ate);
    if (!source || source.tellg() <= 0 || source.tellg() % 4) {
        std::fprintf(stderr, "Cannot read SPIR-V: %s\n", path);
        std::exit(1);
    }
    std::vector<uint32_t> words(size_t(source.tellg()) / 4);
    source.seekg(0);
    if (!source.read(reinterpret_cast<char*>(words.data()), words.size() * 4)) std::exit(1);
    return words;
}
VkShaderModule module(Fixture& fixture, const std::vector<uint32_t>& words) {
    auto info = init<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    info.codeSize = words.size() * 4;
    info.pCode = words.data();
    VkShaderModule result{};
    CHECK(vkCreateShaderModule(fixture.device, &info, nullptr, &result));
    return result;
}
VkRenderPass render_pass(Fixture& fixture) {
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    auto info = init<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    VkRenderPass result{};
    CHECK(vkCreateRenderPass(fixture.device, &info, nullptr, &result));
    return result;
}
VkPipelineLayout layout(Fixture& fixture, VkDescriptorSetLayout descriptors) {
    auto info = init<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    info.setLayoutCount = 1;
    info.pSetLayouts = &descriptors;
    VkPipelineLayout result{};
    CHECK(vkCreatePipelineLayout(fixture.device, &info, nullptr, &result));
    return result;
}
void run(Fixture& fixture, const std::array<std::vector<uint32_t>, 4>& shaders,
         const std::array<VkAccelerationStructureKHR, 2>& scenes, bool query_vertex) {
    const VkShaderStageFlags query_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding bindings[] = {{0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, query_stages, nullptr},
                                               {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, query_stages, nullptr}};
    auto set_info = init<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    set_info.bindingCount = 2;
    set_info.pBindings = bindings;
    VkDescriptorSetLayout set_layout{};
    CHECK(vkCreateDescriptorSetLayout(fixture.device, &set_info, nullptr, &set_layout));
    const auto original_layout = layout(fixture, set_layout), recording_layout = layout(fixture, set_layout);
    const auto original_pass = render_pass(fixture), recording_pass = render_pass(fixture);
    const auto vertex = module(fixture, shaders[query_vertex ? 0 : 2]);
    const auto fragment = module(fixture, shaders[query_vertex ? 3 : 1]);
    VkPipelineShaderStageCreateInfo stages[] = {
        init<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO),
        init<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO)};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[0].pName = stages[1].pName = "main";
    const uint32_t specialization_value = 3;
    VkSpecializationMapEntry specialization_entry{0, 0, sizeof(specialization_value)};
    VkSpecializationInfo specialization{1, &specialization_entry, sizeof(specialization_value), &specialization_value};
    stages[query_vertex ? 0 : 1].pSpecializationInfo = &specialization;
    auto vertices = init<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    auto assembly = init<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // P is never drawn. Its static rectangle deliberately misses the 1x1 target,
    // making an illegal hidden P rebind destroy Q's pre-set dynamic rectangle.
    VkViewport static_viewport{16, 16, 1, 1, 0, 1};
    VkRect2D static_scissor{{16, 16}, {1, 1}};
    auto viewport = init<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    viewport.viewportCount = viewport.scissorCount = 1;
    viewport.pViewports = &static_viewport;
    viewport.pScissors = &static_scissor;
    auto raster = init<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1;
    auto samples = init<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    auto blend = init<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    auto dynamic = init<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    auto create = init<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    create.stageCount = 2;
    create.pStages = stages;
    create.pVertexInputState = &vertices;
    create.pInputAssemblyState = &assembly;
    create.pViewportState = &viewport;
    create.pRasterizationState = &raster;
    create.pMultisampleState = &samples;
    create.pColorBlendState = &blend;
    create.layout = original_layout;
    create.renderPass = original_pass;
    VkPipeline static_pipeline{}, dynamic_pipeline{};
    CHECK(vkCreateGraphicsPipelines(fixture.device, VK_NULL_HANDLE, 1, &create, nullptr, &static_pipeline));
    create.pDynamicState = &dynamic;
    CHECK(vkCreateGraphicsPipelines(fixture.device, VK_NULL_HANDLE, 1, &create, nullptr, &dynamic_pipeline));
    vkDestroyShaderModule(fixture.device, vertex, nullptr);
    vkDestroyShaderModule(fixture.device, fragment, nullptr);
    vkDestroyPipelineLayout(fixture.device, original_layout, nullptr);
    vkDestroyRenderPass(fixture.device, original_pass, nullptr);

    auto framebuffer_info = init<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
    framebuffer_info.renderPass = recording_pass;
    framebuffer_info.width = framebuffer_info.height = framebuffer_info.layers = 1;
    VkFramebuffer framebuffer{};
    CHECK(vkCreateFramebuffer(fixture.device, &framebuffer_info, nullptr, &framebuffer));
    VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 4},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}};
    auto pool_info = init<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    pool_info.maxSets = 4;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    VkDescriptorPool pool{};
    CHECK(vkCreateDescriptorPool(fixture.device, &pool_info, nullptr, &pool));
    VkDescriptorSetLayout set_layouts[] = {set_layout, set_layout, set_layout, set_layout};
    auto allocate = init<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    allocate.descriptorPool = pool;
    allocate.descriptorSetCount = 4;
    allocate.pSetLayouts = set_layouts;
    std::array<VkDescriptorSet, 4> sets{};
    CHECK(vkAllocateDescriptorSets(fixture.device, &allocate, sets.data()));
    std::array<Buffer, 4> outputs;
    constexpr uint32_t scene_indices[] = {0, 1, 1, 0};
    for (uint32_t i = 0; i < 4; ++i) {
        outputs[i] = fixture.buffer(4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        *static_cast<uint32_t*>(outputs[i].mapped) = 0xa5a5a5a5u;
        auto as_info =
            init<VkWriteDescriptorSetAccelerationStructureKHR>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR);
        as_info.accelerationStructureCount = 1;
        as_info.pAccelerationStructures = &scenes[scene_indices[i]];
        VkDescriptorBufferInfo output_info{outputs[i].handle, 0, 4};
        VkWriteDescriptorSet writes[] = {init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET),
                                         init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET)};
        writes[0].pNext = &as_info;
        writes[0].dstSet = writes[1].dstSet = sets[i];
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writes[0].descriptorCount = writes[1].descriptorCount = 1;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &output_info;
        vkUpdateDescriptorSets(fixture.device, 2, writes, 0, nullptr);
    }
    const VkDrawIndirectCommand indirect_arguments{3, 1, 0, 0};
    auto indirect = fixture.buffer(sizeof(indirect_arguments), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true);
    std::memcpy(indirect.mapped, &indirect_arguments, sizeof(indirect_arguments));
    const uint16_t index_values[] = {0, 1, 2};
    auto indices = fixture.buffer(sizeof(index_values), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
    std::memcpy(indices.mapped, index_values, sizeof(index_values));
    auto cb = fixture.begin();
    Fixture::barrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
                     VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                     VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT);
    Fixture::barrier(cb, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                     VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    auto begin = init<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
    begin.renderPass = recording_pass;
    begin.framebuffer = framebuffer;
    begin.renderArea.extent = {1, 1};
    vkCmdBeginRenderPass(cb, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, recording_layout, 0, 1, &sets[0], 0, nullptr);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, static_pipeline);
    const VkViewport dynamic_viewport{0, 0, 1, 1, 0, 1};
    const VkRect2D dynamic_scissor{{0, 0}, {1, 1}};
    vkCmdSetViewport(cb, 0, 1, &dynamic_viewport);
    vkCmdSetScissor(cb, 0, 1, &dynamic_scissor);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, recording_layout, 0, 1, &sets[1], 0, nullptr);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, dynamic_pipeline);
    vkCmdDraw(cb, 3, 1, 0, 0);
    // The descriptor change requires a new query context immediately before
    // the draw, while Q's viewport/scissor remain valid dynamic state.
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, recording_layout, 0, 1, &sets[0], 0, nullptr);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, recording_layout, 0, 1, &sets[2], 0, nullptr);
    vkCmdDrawIndirect(cb, indirect.handle, 0, 1, sizeof(VkDrawIndirectCommand));
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, recording_layout, 0, 1, &sets[3], 0, nullptr);
    vkCmdBindIndexBuffer(cb, indices.handle, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cb, 3, 1, 0, 0, 0);
    vkCmdEndRenderPass(cb);
    Fixture::barrier(cb, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    fixture.submit(cb);
    expect(*static_cast<const uint32_t*>(outputs[0].mapped) == 103,
           "descriptor scene change before direct draw preserves ordinary opaque query classification");
    expect(*static_cast<const uint32_t*>(outputs[1].mapped) == 1003,
           "static P -> dynamic setters -> descriptor switch -> dynamic Q preserves viewport/scissor and nonopaque candidate");
    expect(*static_cast<const uint32_t*>(outputs[2].mapped) == 1003,
           "indirect draw refreshes its own descriptor context without racing another output");
    expect(*static_cast<const uint32_t*>(outputs[3].mapped) == 103, "indexed draw refreshes its own opaque descriptor context");
    std::printf("Public graphics %s query: direct opaque=%u nonopaque=%u indirect=%u indexed=%u\n",
                query_vertex ? "vertex" : "fragment", *static_cast<const uint32_t*>(outputs[0].mapped),
                *static_cast<const uint32_t*>(outputs[1].mapped), *static_cast<const uint32_t*>(outputs[2].mapped),
                *static_cast<const uint32_t*>(outputs[3].mapped));
    vkDestroyDescriptorPool(fixture.device, pool, nullptr);
    vkDestroyPipeline(fixture.device, static_pipeline, nullptr);
    vkDestroyPipeline(fixture.device, dynamic_pipeline, nullptr);
    vkDestroyFramebuffer(fixture.device, framebuffer, nullptr);
    vkDestroyRenderPass(fixture.device, recording_pass, nullptr);
    vkDestroyPipelineLayout(fixture.device, recording_layout, nullptr);
    vkDestroyDescriptorSetLayout(fixture.device, set_layout, nullptr);
}
}  // namespace

static VkResult graphicsInstance(const VkInstanceCreateInfo* original, const VkAllocationCallbacks* allocator, VkInstance* output) {
    auto info = *original;
    std::vector<const char*> extensions;
    for (uint32_t i = 0; i < original->enabledExtensionCount; ++i) extensions.push_back(original->ppEnabledExtensionNames[i]);
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    auto debug = init<VkDebugUtilsMessengerCreateInfoEXT>(VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
    debug.pNext = original->pNext;
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = debug_callback;
    info.pNext = &debug;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    const auto result = vkCreateInstance(&info, allocator, output);
    if (result == VK_SUCCESS) {
        const auto create =
            reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(*output, "vkCreateDebugUtilsMessengerEXT"));
        debug.pNext = nullptr;
        if (create) CHECK(create(*output, &debug, nullptr, &messenger));
    }
    return result;
}
static void graphicsDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
    if (messenger) {
        const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(instance, messenger, nullptr);
        messenger = VK_NULL_HANDLE;
    }
    vkDestroyInstance(instance, allocator);
}
static VkResult graphicsDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* original,
                               const VkAllocationCallbacks* allocator, VkDevice* output) {
    auto info = *original;
    std::vector<const char*> extensions;
    for (uint32_t i = 0; i < original->enabledExtensionCount; ++i) extensions.push_back(original->ppEnabledExtensionNames[i]);
    extensions.push_back(VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME);
    // NativeFixture requests Vulkan 1.2, so OMM's synchronization2 dependency
    // must be named even though this fixture only records legacy barriers.
    extensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    auto micromap =
        init<VkPhysicalDeviceOpacityMicromapFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT);
    micromap.micromap = VK_TRUE;
    micromap.pNext = const_cast<void*>(original->pNext);  // Preserve the fixture's native RQ/AS/BDA chain.
    info.pNext = &micromap;
    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures(physical, &supported);
    VkPhysicalDeviceFeatures features = original->pEnabledFeatures ? *original->pEnabledFeatures : VkPhysicalDeviceFeatures{};
    features.vertexPipelineStoresAndAtomics = supported.vertexPipelineStoresAndAtomics;
    features.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics;
    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, queues.data());
    graphics_supported = features.vertexPipelineStoresAndAtomics && features.fragmentStoresAndAtomics &&
                         original->queueCreateInfoCount && original->pQueueCreateInfos[0].queueFamilyIndex < queue_count &&
                         (queues[original->pQueueCreateInfos[0].queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT);
    info.pEnabledFeatures = &features;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    return vkCreateDevice(physical, &info, allocator, output);
}

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: test_omm_graphics_layer query.vert.spv query.frag.spv plain.vert.spv plain.frag.spv\n");
        return 1;
    }
    const auto* selection = std::getenv("VK_VEL_OMM_EXPERIMENTAL_SOFTWARE");
    if (!selection || std::strcmp(selection, "1")) {
        std::puts("SKIP graphics loader: explicit software OMM selector and external layer configuration are required");
        return 77;
    }
    const std::array<std::vector<uint32_t>, 4> shaders{read_shader(argv[1]), read_shader(argv[2]), read_shader(argv[3]),
                                                       read_shader(argv[4])};
    {
        Fixture fixture;
        if (!fixture.open()) {
            std::puts("SKIP graphics loader: native RQ/AS/BDA/Int64 device unavailable");
            return 77;
        }
        if (!graphics_supported) {
            std::puts("SKIP graphics loader: graphics queue or vertex/fragment storage writes unavailable");
            return 77;
        }
        constexpr VkBufferUsageFlags input_usage =
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        const float positions[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        auto vertex_buffer = fixture.buffer(sizeof(positions), input_usage, true);
        std::memcpy(vertex_buffer.mapped, positions, sizeof(positions));
        std::array<VkAccelerationStructureGeometryKHR, 2> geometries, instances_geometries;
        std::array<Acceleration, 2> blases, tlases;
        for (uint32_t i = 0; i < 2; ++i) {
            auto& geometry = geometries[i];
            geometry = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
            geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            geometry.flags = i == 0 ? VK_GEOMETRY_OPAQUE_BIT_KHR : VkGeometryFlagsKHR(0);
            geometry.geometry.triangles = init<VkAccelerationStructureGeometryTrianglesDataKHR>(
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR);
            geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            geometry.geometry.triangles.vertexData.deviceAddress = vertex_buffer.address;
            geometry.geometry.triangles.vertexStride = 12;
            geometry.geometry.triangles.maxVertex = 2;
            geometry.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
            blases[i] = fixture.acceleration(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geometry);
            VkAccelerationStructureInstanceKHR instance{};
            instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1;
            instance.mask = 255;
            instance.accelerationStructureReference = blases[i].address;
            auto instance_buffer = fixture.buffer(sizeof(instance), input_usage, true);
            std::memcpy(instance_buffer.mapped, &instance, sizeof(instance));
            auto& instances = instances_geometries[i];
            instances = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
            instances.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
            instances.geometry.instances = init<VkAccelerationStructureGeometryInstancesDataKHR>(
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR);
            instances.geometry.instances.data.deviceAddress = instance_buffer.address;
            tlases[i] = fixture.acceleration(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, instances);
        }
        auto build = fixture.begin();
        Fixture::barrier(build, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_SHADER_READ_BIT);
        for (const auto& blas : blases) {
            const auto* range = &blas.range;
            fixture.buildAS(build, 1, &blas.build, &range);
        }
        Fixture::barrier(build, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        for (const auto& tlas : tlases) {
            const auto* range = &tlas.range;
            fixture.buildAS(build, 1, &tlas.build, &range);
        }
        fixture.submit(build);
        const std::array<VkAccelerationStructureKHR, 2> scenes{tlases[0].handle, tlases[1].handle};
        run(fixture, shaders, scenes, true);
        run(fixture, shaders, scenes, false);
    }
    std::printf("Public graphics loader: %u checks, %u failures, %u validation errors, %u warnings\n", checks, failures,
                validation_errors.load(), validation_warnings.load());
    return failures || validation_errors || validation_warnings ? 1 : 0;
}
