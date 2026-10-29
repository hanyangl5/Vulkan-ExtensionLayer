// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "native_fixture.h"
#include "../omm_consumers.h"
#include "../omm_metadata.h"
#include "../omm_storage.h"
#include "../omm_acceleration.h"
#include <array>
#include <map>
#include <string>
#include <vulkan/utility/vk_struct_helper.hpp>

using namespace vel::omm;
namespace {
unsigned checks = 0, compilations = 0;
constexpr uint32_t kPipelineCount = 4, kSceneCount = 2, kCaseCount = kPipelineCount * kSceneCount;
std::map<VkRenderPass, unsigned> private_passes;
enum class Probe { None, FailedBase, FailedChild, Success, CompileRequired };
Probe probe = Probe::None;
unsigned probe_calls = 0;
bool probe_flags2 = false;
VkPipeline probe_parent{};
void require(bool condition, const char* why) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL graphics consumer: %s\n", why);
        std::exit(1);
    }
}
template <typename T>
T proc(VkDevice device, const char* name) {
    return reinterpret_cast<T>(vkGetDeviceProcAddr(device, name));
}
VKAPI_ATTR VkResult VKAPI_CALL create_graphics(VkDevice device, VkPipelineCache cache, uint32_t count,
                                               const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator,
                                               VkPipeline* out) {
    require(count == 1, "independent graphics compilations");
    auto info = infos[0];
    require(!info.renderPass || private_passes.count(info.renderPass) != 0,
            "legacy public and deferred query pipelines use a live private render pass");
    const auto* flags2 = vku::FindStructInPNextChain<VkPipelineCreateFlags2CreateInfo>(info.pNext);
    const auto flags = flags2 ? flags2->flags : VkPipelineCreateFlags2(info.flags);
    const auto* feedback = vku::FindStructInPNextChain<VkPipelineCreationFeedbackCreateInfo>(info.pNext);
    const auto* rendering = vku::FindStructInPNextChain<VkPipelineRenderingCreateInfo>(info.pNext);
    if (rendering)
        require(!rendering->colorAttachmentCount && !rendering->pColorAttachmentFormats,
                "legacy pipeline snapshot never dereferences ignored dynamic-rendering formats");
    if (!info.renderPass) require(rendering != nullptr, "dynamic-rendering snapshot survives native compilation");
    if (info.pRasterizationState->rasterizerDiscardEnable)
        require(info.stageCount == 1 && info.pStages[0].stage == VK_SHADER_STAGE_VERTEX_BIT && !info.pViewportState &&
                    !info.pMultisampleState && !info.pDepthStencilState && !info.pColorBlendState,
                "single-stage static-discard snapshot never reads ignored state pointers");
    if (probe != Probe::None) {
        const auto index = probe_calls++;
        require(bool(flags2) == probe_flags2, "effective graphics flag representation retained");
        if (probe == Probe::CompileRequired) {
            require((flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT) != 0, "compile-required flag retained");
            *out = VK_NULL_HANDLE;
            return VK_PIPELINE_COMPILE_REQUIRED;
        }
        if (index == 0 && probe == Probe::FailedBase) {
            *out = VK_NULL_HANDLE;
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        if (index == 1) {
            require(info.basePipelineIndex == -1, "batch child index resolved before native create");
            if (probe == Probe::FailedBase)
                require(!info.basePipelineHandle && !(flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT),
                        "failed parent uses independent child");
            else
                require(info.basePipelineHandle == probe_parent && (flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT),
                        "successful parent remains child's native base");
            if (probe == Probe::FailedChild) {
                *out = VK_NULL_HANDLE;
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            }
        }
    }
    // Emulate optional flags2/feedback using a core 1.2 device, while checking
    // the production backend's actual downstream arguments above.
    auto native_rendering = init<VkPipelineRenderingCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
    if (rendering) {
        native_rendering = *rendering;
        native_rendering.pNext = nullptr;
    }
    if (flags2 || feedback || (rendering && info.renderPass)) {
        info.pNext = !info.renderPass && rendering ? &native_rendering : nullptr;
        info.flags =
            VkPipelineCreateFlags(flags & ~VkPipelineCreateFlags2(VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT));
    } else
        require(!(flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) && !info.basePipelineHandle,
                "deferred compilation removes dead derivative handles");
    const auto status = vkCreateGraphicsPipelines(device, cache, count, &info, allocator, out);
    if (status == VK_SUCCESS) {
        ++compilations;
        if (probe != Probe::None && probe_calls == 1) probe_parent = *out;
        if (feedback) {
            *feedback->pPipelineCreationFeedback = {VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT, 123};
            for (uint32_t i = 0; i < feedback->pipelineStageCreationFeedbackCount; ++i)
                feedback->pPipelineStageCreationFeedbacks[i] = {VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT, 456 + i};
        }
    }
    return status;
}
VKAPI_ATTR VkResult VKAPI_CALL create_pass(VkDevice device, const VkRenderPassCreateInfo* info,
                                           const VkAllocationCallbacks* allocator, VkRenderPass* out) {
    require(!vku::FindStructInPNextChain<VkRenderPassCreationFeedbackCreateInfoEXT>(info->pNext), "private RP1 strips feedback");
    const auto result = vkCreateRenderPass(device, info, allocator, out);
    if (result == VK_SUCCESS) ++private_passes[*out];
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL create_pass2(VkDevice device, const VkRenderPassCreateInfo2* info,
                                            const VkAllocationCallbacks* allocator, VkRenderPass* out) {
    require(!vku::FindStructInPNextChain<VkRenderPassCreationFeedbackCreateInfoEXT>(info->pNext), "private RP2 strips feedback");
    for (uint32_t i = 0; i < info->subpassCount; ++i)
        require(!vku::FindStructInPNextChain<VkRenderPassSubpassFeedbackCreateInfoEXT>(info->pSubpasses[i].pNext),
                "private subpasses strip feedback");
    const auto result = proc<PFN_vkCreateRenderPass2>(device, "vkCreateRenderPass2")(device, info, allocator, out);
    if (result == VK_SUCCESS) ++private_passes[*out];
    return result;
}
VKAPI_ATTR void VKAPI_CALL destroy_pass(VkDevice device, VkRenderPass pass, const VkAllocationCallbacks* allocator) {
    const auto found = private_passes.find(pass);
    require(found != private_passes.end(), "private render pass destroyed once per live creation");
    if (--found->second == 0) private_passes.erase(found);
    vkDestroyRenderPass(device, pass, allocator);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL consumer_proc(VkDevice device, const char* name) {
    if (!std::strcmp(name, "vkCreateGraphicsPipelines")) return reinterpret_cast<PFN_vkVoidFunction>(create_graphics);
    if (!std::strcmp(name, "vkCreateRenderPass")) return reinterpret_cast<PFN_vkVoidFunction>(create_pass);
    if (!std::strcmp(name, "vkCreateRenderPass2")) return reinterpret_cast<PFN_vkVoidFunction>(create_pass2);
    if (!std::strcmp(name, "vkDestroyRenderPass")) return reinterpret_cast<PFN_vkVoidFunction>(destroy_pass);
    return vkGetDeviceProcAddr(device, name);
}
const char query_body[] = R"GLSL(
layout(set=0,binding=0) uniform accelerationStructureEXT scene;
layout(set=0,binding=1,std430) buffer ApplicationOutput { uint value; } result;
layout(constant_id=0) const uint originalSpecialization=3u;
void trace_scene() {
    rayQueryEXT query;
    rayQueryInitializeEXT(query,scene,0u,255u,vec3(0.25,0.25,-1.0),0.0,vec3(0,0,1),2.0);
    uint candidates=0u;
    while(rayQueryProceedEXT(query)) ++candidates;
    result.value=originalSpecialization+100u*rayQueryGetIntersectionTypeEXT(query,true)+1000u*candidates;
}
)GLSL";
const char position_body[] = R"GLSL(
void position_vertex() {
    const vec2 positions[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));
    gl_Position=vec4(positions[gl_VertexIndex],0,1);
}
)GLSL";
std::string shader(bool query, bool vertex) {
    std::string source = "#version 460\n";
    if (query) source += "#extension GL_EXT_ray_query : require\n";
    if (query) source += query_body;
    if (vertex) source += position_body;
    source += "void main(){";
    if (vertex) source += "position_vertex();";
    if (query) source += vertex ? "if(gl_VertexIndex==0)trace_scene();" : "trace_scene();";
    return source + "}\n";
}
std::vector<uint32_t> read_shader(const char* path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(bool(input) && input.tellg() > 0 && input.tellg() % 4 == 0, "read shader file");
    std::vector<uint32_t> result(size_t(input.tellg()) / 4);
    input.seekg(0);
    require(bool(input.read(reinterpret_cast<char*>(result.data()), result.size() * 4)), "read shader words");
    return result;
}
bool open_graphics(Fixture& fixture) {
    if (!fixture.open()) return false;
    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures(fixture.physical, &supported);
    uint32_t count;
    vkGetPhysicalDeviceQueueFamilyProperties(fixture.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(fixture.physical, &count, queues.data());
    if (!supported.vertexPipelineStoresAndAtomics || !supported.fragmentStoresAndAtomics ||
        !(queues[fixture.family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
        return false;
    uint32_t extension_count = 0;
    CHECK(vkEnumerateDeviceExtensionProperties(fixture.physical, nullptr, &extension_count, nullptr));
    std::vector<VkExtensionProperties> extensions_supported(extension_count);
    CHECK(vkEnumerateDeviceExtensionProperties(fixture.physical, nullptr, &extension_count, extensions_supported.data()));
    bool dynamic_extension = false;
    for (const auto& extension : extensions_supported)
        dynamic_extension |= !std::strcmp(extension.extensionName, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    if (!dynamic_extension) return false;
    auto dynamic_rendering =
        init<VkPhysicalDeviceDynamicRenderingFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES);
    auto queried = init<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    queried.pNext = &dynamic_rendering;
    vkGetPhysicalDeviceFeatures2(fixture.physical, &queried);
    if (!dynamic_rendering.dynamicRendering) return false;
    vkDestroyCommandPool(fixture.device, fixture.pool, nullptr);
    fixture.pool = VK_NULL_HANDLE;
    vkDestroyDevice(fixture.device, nullptr);
    fixture.device = VK_NULL_HANDLE;
    auto bda = init<VkPhysicalDeviceBufferDeviceAddressFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES);
    bda.bufferDeviceAddress = VK_TRUE;
    auto as = init<VkPhysicalDeviceAccelerationStructureFeaturesKHR>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR);
    as.accelerationStructure = VK_TRUE;
    as.pNext = &bda;
    auto rq = init<VkPhysicalDeviceRayQueryFeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR);
    rq.rayQuery = VK_TRUE;
    rq.pNext = &as;
    dynamic_rendering.pNext = &rq;
    VkPhysicalDeviceFeatures core{};
    core.shaderInt64 = core.vertexPipelineStoresAndAtomics = core.fragmentStoresAndAtomics = VK_TRUE;
    const float priority = 1;
    auto queue = init<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
    queue.queueFamilyIndex = fixture.family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    const char* extensions[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME};
    auto device = init<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    device.pNext = &dynamic_rendering;
    device.pEnabledFeatures = &core;
    device.queueCreateInfoCount = 1;
    device.pQueueCreateInfos = &queue;
    device.enabledExtensionCount = 4;
    device.ppEnabledExtensionNames = extensions;
    CHECK(vkCreateDevice(fixture.physical, &device, nullptr, &fixture.device));
    vkGetDeviceQueue(fixture.device, fixture.family, 0, &fixture.queue);
    fixture.createAS = proc<PFN_vkCreateAccelerationStructureKHR>(fixture.device, "vkCreateAccelerationStructureKHR");
    fixture.destroyAS = proc<PFN_vkDestroyAccelerationStructureKHR>(fixture.device, "vkDestroyAccelerationStructureKHR");
    fixture.sizesAS = proc<PFN_vkGetAccelerationStructureBuildSizesKHR>(fixture.device, "vkGetAccelerationStructureBuildSizesKHR");
    fixture.addressAS =
        proc<PFN_vkGetAccelerationStructureDeviceAddressKHR>(fixture.device, "vkGetAccelerationStructureDeviceAddressKHR");
    fixture.buildAS = proc<PFN_vkCmdBuildAccelerationStructuresKHR>(fixture.device, "vkCmdBuildAccelerationStructuresKHR");
    auto pool = init<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    pool.queueFamilyIndex = fixture.family;
    CHECK(vkCreateCommandPool(fixture.device, &pool, nullptr, &fixture.pool));
    return true;
}
struct Identities {
    std::vector<std::pair<VkAccelerationStructureKHR, DescriptorASIdentity>> values;
};
VkResult resolve(void* user, VkAccelerationStructureKHR handle, DescriptorASIdentity* output) {
    for (const auto& entry : static_cast<Identities*>(user)->values)
        if (entry.first == handle) {
            *output = entry.second;
            return VK_SUCCESS;
        }
    return VK_ERROR_VALIDATION_FAILED_EXT;
}
Buffer upload(Fixture& fixture, const void* data, size_t size) {
    auto value = fixture.buffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    std::memcpy(value.mapped, data, size);
    return value;
}
Buffer root_for(Fixture& fixture, const std::vector<uint8_t>& bytes, uint32_t kind) {
    const auto body = upload(fixture, bytes.data(), bytes.size());
    const MetadataRootCell root{kMetadataRootMagic, kMetadataVersion, kind, kMetadataReady,
                                body.address,       bytes.size(),     1,    {0, 0, 0}};
    return upload(fixture, &root, sizeof(root));
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[1], "--write-shaders")) {
        const char* names[] = {"query.vert", "query.frag", "plain.vert", "plain.frag"};
        for (unsigned i = 0; i < 4; ++i) {
            std::ofstream output(std::string(argv[2]) + "/" + names[i]);
            output << shader(i < 2, i % 2 == 0);
            require(bool(output), "write fixture shader");
        }
        return 0;
    }
    if (argc != 6) {
        std::fprintf(
            stderr, "usage: test_omm_graphics_consumers library.spv query.vert.spv query.frag.spv plain.vert.spv plain.frag.spv\n");
        return 1;
    }
    const auto library = read_shader(argv[1]);
    const std::array<std::vector<uint32_t>, 4> sources = {read_shader(argv[2]), read_shader(argv[3]), read_shader(argv[4]),
                                                          read_shader(argv[5])};
    Fixture fixture;
    if (!open_graphics(fixture)) return 77;
    Identities identities;
    DescriptorBackend* descriptors{};
    CHECK(descriptor_backend_create({fixture.physical, fixture.device, vkGetDeviceProcAddr, nullptr, resolve, &identities},
                                    &descriptors));
    ConsumerBackend* consumers{};
    CHECK(consumer_backend_create({fixture.device, consumer_proc, descriptors, nullptr, library.data(), library.size() * 4},
                                  &consumers));

    constexpr auto input_usage =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const float triangle[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    auto vertices = fixture.buffer(sizeof(triangle), input_usage, true);
    std::memcpy(vertices.mapped, triangle, sizeof(triangle));
    auto geometry = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.geometry.triangles =
        init<VkAccelerationStructureGeometryTrianglesDataKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR);
    auto& triangles = geometry.geometry.triangles;
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = vertices.address;
    triangles.vertexStride = 12;
    triangles.maxVertex = 2;
    triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    const auto blas = fixture.acceleration(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geometry);
    std::array<Acceleration, 2> scenes;
    std::array<VkAccelerationStructureGeometryKHR, 2> instance_geometries{};
    std::vector<uint8_t> packed;
    for (unsigned i = 0; i < 2; ++i) {
        // Uniform level-1 FOUR_STATE texels, not precomputed query decisions.
        // Native AS geometry stays nonopaque; instrumentation must auto-commit
        // the opaque map and hide every transparent-map candidate.
        VkMicromapTriangleEXT record{0, 1, VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT};
        const uint8_t states = i == 0 ? 0x55 : 0;
        std::vector<uint8_t> snapshot;
        require(storage_build({reinterpret_cast<const uint8_t*>(&record), sizeof(record), 1, sizeof(record), &states, 1},
                              &snapshot) == StorageResult::Success,
                "build packed micromap snapshot");
        MetadataGeometryInput metadata{};
        metadata.plan = {kMetadataTriangles, 0, 1, true, snapshot.size()};
        metadata.snapshot = snapshot.data();
        metadata.snapshot_size = snapshot.size();
        require(metadata_pack_blas(&metadata, 1, 0, 1, &packed) == MetadataResult::Success, "pack micromap BLAS metadata");
        const auto blas_root = root_for(fixture, packed, kMetadataBlas);
        auto instances = fixture.buffer(sizeof(VkAccelerationStructureInstanceKHR), input_usage, true);
        auto& instance = *static_cast<VkAccelerationStructureInstanceKHR*>(instances.mapped);
        instance = {};
        for (unsigned axis = 0; axis < 3; ++axis) instance.transform.matrix[axis][axis] = 1;
        instance.mask = 255;
        instance.accelerationStructureReference = blas.address;
        auto& instance_geometry = instance_geometries[i];
        instance_geometry = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
        instance_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        instance_geometry.geometry.instances = init<VkAccelerationStructureGeometryInstancesDataKHR>(
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR);
        instance_geometry.geometry.instances.data.deviceAddress = instances.address;
        scenes[i] = fixture.acceleration(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, instance_geometry);
        const MetadataInstance logical{blas_root.address, blas.address, 0, kMetadataInstanceActive, 0};
        require(metadata_pack_tlas(&logical, 1, 0, 1, &packed) == MetadataResult::Success, "pack scene metadata");
        const auto root = root_for(fixture, packed, kMetadataTlas);
        const AccelerationIdentityCell identity{kAccelerationIdentityMagic, 1, 1, 0, scenes[i].address, root.address};
        const auto cell = upload(fixture, &identity, sizeof(identity));
        identities.values.push_back({scenes[i].handle, {scenes[i].address, root.address, {}, cell.address}});
    }
    auto build = fixture.begin();
    const auto* blas_range = &blas.range;
    fixture.buildAS(build, 1, &blas.build, &blas_range);
    Fixture::barrier(build, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                     VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    for (const auto& scene : scenes) {
        const auto* range = &scene.range;
        fixture.buildAS(build, 1, &scene.build, &range);
    }
    Fixture::barrier(build, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                     VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    fixture.submit(build);

    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    auto set_info = init<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    set_info.bindingCount = 2;
    set_info.pBindings = bindings;
    VkDescriptorSetLayout set_layout{};
    CHECK(vkCreateDescriptorSetLayout(fixture.device, &set_info, nullptr, &set_layout));
    CHECK(descriptor_track_layout(descriptors, set_layout, &set_info));
    CHECK(consumer_track_set_layout(consumers, set_layout, &set_info));
    auto layout_info = init<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;
    VkPipelineLayout original_layout{}, record_layout{};
    CHECK(vkCreatePipelineLayout(fixture.device, &layout_info, nullptr, &original_layout));
    CHECK(consumer_track_layout(consumers, original_layout, &layout_info));
    CHECK(vkCreatePipelineLayout(fixture.device, &layout_info, nullptr, &record_layout));
    CHECK(consumer_track_layout(consumers, record_layout, &layout_info));
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kCaseCount},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kCaseCount}};
    auto pool_info = init<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    pool_info.maxSets = kCaseCount;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = sizes;
    VkDescriptorPool pool{};
    CHECK(vkCreateDescriptorPool(fixture.device, &pool_info, nullptr, &pool));
    CHECK(descriptor_track_pool(descriptors, pool, &pool_info));
    std::array<VkDescriptorSetLayout, kCaseCount> layouts;
    layouts.fill(set_layout);
    auto allocation = init<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    allocation.descriptorPool = pool;
    allocation.descriptorSetCount = kCaseCount;
    allocation.pSetLayouts = layouts.data();
    VkDescriptorSet sets[kCaseCount]{};
    CHECK(vkAllocateDescriptorSets(fixture.device, &allocation, sets));
    CHECK(descriptor_allocate_sets(descriptors, &allocation, sets));
    Buffer outputs[kCaseCount];
    for (unsigned i = 0; i < kCaseCount; ++i) {
        outputs[i] = fixture.buffer(4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        *static_cast<uint32_t*>(outputs[i].mapped) = UINT32_MAX;
        const VkDescriptorBufferInfo output{outputs[i].handle, 0, 4};
        auto acceleration =
            init<VkWriteDescriptorSetAccelerationStructureKHR>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR);
        acceleration.accelerationStructureCount = 1;
        acceleration.pAccelerationStructures = &scenes[i % 2].handle;
        VkWriteDescriptorSet writes[] = {init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET),
                                         init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET)};
        for (auto& write : writes) {
            write.dstSet = sets[i];
            write.descriptorCount = 1;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writes[0].pNext = &acceleration;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &output;
        CHECK(descriptor_update(descriptors, 2, writes, 0, nullptr));
        vkUpdateDescriptorSets(fixture.device, 2, writes, 0, nullptr);
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    auto pass_info = init<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    VkRenderPass passes[2]{}, record_pass{};
    CHECK(vkCreateRenderPass(fixture.device, &pass_info, nullptr, &passes[0]));
    CHECK(consumer_track_render_pass(consumers, passes[0], &pass_info));
    auto subpass2 = init<VkSubpassDescription2>(VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2);
    subpass2.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    auto pass2_info = init<VkRenderPassCreateInfo2>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2);
    pass2_info.subpassCount = 1;
    pass2_info.pSubpasses = &subpass2;
    CHECK(proc<PFN_vkCreateRenderPass2>(fixture.device, "vkCreateRenderPass2")(fixture.device, &pass2_info, nullptr, &passes[1]));
    VkRenderPassCreationFeedbackInfoEXT render_feedback{123};
    VkRenderPassSubpassFeedbackInfoEXT subpass_feedback{};
    subpass_feedback.postMergeIndex = 456;
    auto render_feedback_info =
        init<VkRenderPassCreationFeedbackCreateInfoEXT>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATION_FEEDBACK_CREATE_INFO_EXT);
    render_feedback_info.pRenderPassFeedback = &render_feedback;
    auto subpass_feedback_info =
        init<VkRenderPassSubpassFeedbackCreateInfoEXT>(VK_STRUCTURE_TYPE_RENDER_PASS_SUBPASS_FEEDBACK_CREATE_INFO_EXT);
    subpass_feedback_info.pSubpassFeedback = &subpass_feedback;
    // Feedback admission/removal is a mock transaction; the extension is not
    // enabled on the underlying core fixture device.
    pass2_info.pNext = &render_feedback_info;
    subpass2.pNext = &subpass_feedback_info;
    CHECK(consumer_track_render_pass2(consumers, passes[1], &pass2_info));
    require(render_feedback.postMergeSubpassCount == 123 && subpass_feedback.postMergeIndex == 456,
            "private render-pass cloning preserves both caller feedback destinations");
    CHECK(vkCreateRenderPass(fixture.device, &pass_info, nullptr, &record_pass));
    auto framebuffer_info = init<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
    framebuffer_info.renderPass = record_pass;
    framebuffer_info.width = framebuffer_info.height = framebuffer_info.layers = 1;
    VkFramebuffer framebuffer{};
    CHECK(vkCreateFramebuffer(fixture.device, &framebuffer_info, nullptr, &framebuffer));

    VkShaderModule modules[4]{};
    for (unsigned i = 0; i < 4; ++i) {
        auto module_info = init<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        module_info.codeSize = sources[i].size() * 4;
        module_info.pCode = sources[i].data();
        CHECK(consumer_create_shader(consumers, &module_info, nullptr, &modules[i]));
    }
    auto vertex = init<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    auto assembly = init<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    const VkViewport viewport{0, 0, 1, 1, 0, 1};
    const VkRect2D scissor{{0, 0}, {1, 1}};
    auto viewport_info = init<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    viewport_info.viewportCount = viewport_info.scissorCount = 1;
    viewport_info.pViewports = &viewport;
    viewport_info.pScissors = &scissor;
    auto raster = init<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1;
    auto sample = init<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    sample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    auto discard = raster;
    discard.rasterizerDiscardEnable = VK_TRUE;
    uint32_t specialization_value = 37;
    const VkSpecializationMapEntry map_entry{0, 0, 4};
    const VkSpecializationInfo specialization{1, &map_entry, 4, &specialization_value};
    VkGraphicsPipelineCreateInfo create[kPipelineCount]{};
    VkPipelineShaderStageCreateInfo stages[kPipelineCount][2]{};
    VkPipeline logical[kPipelineCount]{};
    auto ignored_rendering = init<VkPipelineRenderingCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
    ignored_rendering.colorAttachmentCount = 123;
    ignored_rendering.pColorAttachmentFormats = reinterpret_cast<const VkFormat*>(uintptr_t(1));
    auto dynamic_rendering = init<VkPipelineRenderingCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
    for (unsigned i = 0; i < kPipelineCount; ++i) {
        const bool query_vertex = i == 0 || i == 2;
        for (unsigned j = 0; j < 2; ++j) {
            stages[i][j] = init<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
            stages[i][j].stage = j ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            stages[i][j].module = modules[query_vertex ? (j == 0 ? 0 : 3) : (j == 0 ? 2 : 1)];
            stages[i][j].pName = "main";
            if (query_vertex ? j == 0 : j == 1) stages[i][j].pSpecializationInfo = &specialization;
        }
        create[i] = init<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
        auto& info = create[i];
        info.pNext = i == 3 ? &dynamic_rendering : &ignored_rendering;
        info.stageCount = i == 2 ? 1 : 2;
        info.pStages = stages[i];
        info.pVertexInputState = &vertex;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = i == 2 ? reinterpret_cast<const VkPipelineViewportStateCreateInfo*>(uintptr_t(1)) : &viewport_info;
        info.pRasterizationState = i == 2 ? &discard : &raster;
        info.pMultisampleState = i == 2 ? reinterpret_cast<const VkPipelineMultisampleStateCreateInfo*>(uintptr_t(1)) : &sample;
        // No color/depth attachments: these pointers are explicitly ignored.
        info.pDepthStencilState = reinterpret_cast<const VkPipelineDepthStencilStateCreateInfo*>(uintptr_t(1));
        info.pColorBlendState = reinterpret_cast<const VkPipelineColorBlendStateCreateInfo*>(uintptr_t(1));
        info.pTessellationState = reinterpret_cast<const VkPipelineTessellationStateCreateInfo*>(uintptr_t(1));
        info.layout = original_layout;
        info.renderPass = i == 3 ? VK_NULL_HANDLE : passes[i == 1 ? 1 : 0];
        info.basePipelineIndex = -1;
        CHECK(consumer_create_graphics(consumers, VK_NULL_HANDLE, 1, &info, nullptr, &logical[i]));
    }
    for (bool flags2_path : {false, true}) {
        for (auto mode : {Probe::FailedBase, Probe::FailedChild, Probe::Success, Probe::CompileRequired}) {
            VkGraphicsPipelineCreateInfo batch[] = {create[0], create[0], create[0]};
            VkPipelineCreateFlags2CreateInfo flags[3]{};
            VkPipelineCreationFeedback values[3]{}, stage_values[3][2]{};
            VkPipelineCreationFeedbackCreateInfo feedback[3]{};
            for (unsigned i = 0; i < 3; ++i) {
                auto effective = VkPipelineCreateFlags2(VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT);
                if (i == 1) effective |= VK_PIPELINE_CREATE_DERIVATIVE_BIT;
                if ((i == 1 && mode == Probe::FailedChild) || mode == Probe::CompileRequired)
                    effective |= VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT;
                if (mode == Probe::CompileRequired) effective |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
                flags[i] = init<VkPipelineCreateFlags2CreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO);
                flags[i].flags = effective;
                feedback[i] = init<VkPipelineCreationFeedbackCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO);
                feedback[i].pNext = flags2_path ? &flags[i] : nullptr;
                feedback[i].pPipelineCreationFeedback = &values[i];
                feedback[i].pipelineStageCreationFeedbackCount = 2;
                feedback[i].pPipelineStageCreationFeedbacks = stage_values[i];
                batch[i].pNext = &feedback[i];
                batch[i].flags = flags2_path ? 0 : VkPipelineCreateFlags(effective);
                batch[i].basePipelineIndex = i == 1 ? 0 : -1;
            }
            probe = mode;
            probe_calls = 0;
            probe_flags2 = flags2_path;
            VkPipeline pipelines[3]{};
            const auto result = consumer_create_graphics(consumers, VK_NULL_HANDLE, 3, batch, nullptr, pipelines);
            probe = Probe::None;
            const auto expected = mode == Probe::Success           ? VK_SUCCESS
                                  : mode == Probe::CompileRequired ? VK_PIPELINE_COMPILE_REQUIRED
                                                                   : VK_ERROR_OUT_OF_DEVICE_MEMORY;
            require(result == expected, "graphics batch preserves native result");
            require(probe_calls == (mode == Probe::CompileRequired ? 1
                                    : mode == Probe::FailedChild   ? 2
                                                                   : 3),
                    "effective early-return behavior");
            if (mode == Probe::FailedBase)
                require(!pipelines[0] && pipelines[1] && pipelines[2], "failed parent permits successful child");
            if (mode == Probe::Success || mode == Probe::FailedBase)
                require(values[1].duration == 123 && stage_values[1][0].duration == 456 && stage_values[1][1].duration == 457,
                        "all stage feedback copied to caller, including independent child");
            require(batch[1].basePipelineIndex == 0 &&
                        ((flags2_path ? flags[1].flags : batch[1].flags) & VK_PIPELINE_CREATE_DERIVATIVE_BIT),
                    "caller derivative flags and base index unchanged");
            for (auto pipeline : pipelines)
                if (pipeline) consumer_destroy_pipeline(consumers, pipeline, nullptr);
        }
    }
    auto library_info = init<VkPipelineLibraryCreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR);
    library_info.libraryCount = 1;
    library_info.pLibraries = logical;
    auto hidden = create[0];
    hidden.pNext = &library_info;
    hidden.stageCount = 0;
    hidden.pStages = nullptr;
    VkPipeline rejected{};
    require(consumer_create_graphics(consumers, VK_NULL_HANDLE, 1, &hidden, nullptr, &rejected) == VK_ERROR_FEATURE_NOT_PRESENT &&
                !rejected,
            "linked library cannot hide query stages behind empty final pStages");
    VkPipelineShaderStageCreateInfo ordinary_stages[] = {stages[0][0], stages[0][1]};
    ordinary_stages[0].module = modules[2];
    ordinary_stages[0].pSpecializationInfo = nullptr;
    VkGraphicsShaderGroupCreateInfoNV groups[2]{};
    for (auto& group : groups) {
        group = init<VkGraphicsShaderGroupCreateInfoNV>(VK_STRUCTURE_TYPE_GRAPHICS_SHADER_GROUP_CREATE_INFO_NV);
        group.stageCount = 2;
        group.pVertexInputState = &vertex;
    }
    groups[0].pStages = ordinary_stages;
    groups[1].pStages = stages[0];
    auto group_info =
        init<VkGraphicsPipelineShaderGroupsCreateInfoNV>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_SHADER_GROUPS_CREATE_INFO_NV);
    group_info.groupCount = 2;
    group_info.pGroups = groups;
    hidden = create[0];
    hidden.pNext = &group_info;
    hidden.pStages = ordinary_stages;
    const auto before_groups = compilations;
    require(consumer_create_graphics(consumers, VK_NULL_HANDLE, 1, &hidden, nullptr, &rejected) == VK_ERROR_FEATURE_NOT_PRESENT &&
                !rejected && compilations == before_groups,
            "NV groups cannot hide query stages behind ordinary parent stages");
    group_info.groupCount = 1;
    group_info.pipelineCount = 1;
    group_info.pPipelines = logical;
    require(consumer_create_graphics(consumers, VK_NULL_HANDLE, 1, &hidden, nullptr, &rejected) == VK_ERROR_FEATURE_NOT_PRESENT &&
                !rejected && compilations == before_groups,
            "NV referenced pipelines cannot hide query stages or their original module lifetime");
    for (auto module : modules) consumer_destroy_shader(consumers, module, nullptr);
    consumer_untrack_layout(consumers, original_layout);
    vkDestroyPipelineLayout(fixture.device, original_layout, nullptr);
    for (auto pass : passes) {
        consumer_untrack_render_pass(consumers, pass);
        vkDestroyRenderPass(fixture.device, pass, nullptr);
    }
    consumer_untrack_set_layout(consumers, set_layout);
    descriptor_untrack_layout(descriptors, set_layout);
    vkDestroyDescriptorSetLayout(fixture.device, set_layout, nullptr);
    specialization_value = 99;
    require(!private_passes.empty(), "public object destruction preserves private render pass lifetime");
    ConsumerBindingState states[kCaseCount];
    VkPipeline variants[kCaseCount]{};
    VkCommandBuffer commands[kCaseCount]{};
    const auto begin_rendering = proc<PFN_vkCmdBeginRenderingKHR>(fixture.device, "vkCmdBeginRenderingKHR");
    const auto end_rendering = proc<PFN_vkCmdEndRenderingKHR>(fixture.device, "vkCmdEndRenderingKHR");
    require(begin_rendering && end_rendering, "enabled dynamic rendering provides KHR entry points");
    for (unsigned i = 0; i < kCaseCount; ++i) {
        const auto pipeline_index = i / kSceneCount;
        CHECK(consumer_bind_sets(consumers, states[i], record_layout, 0, 1, &sets[i]));
        CHECK(consumer_pipeline(consumers, states[i], logical[pipeline_index], &variants[i]));
        VkPipeline repeated{};
        const auto before = compilations;
        CHECK(consumer_pipeline(consumers, states[i], logical[pipeline_index], &repeated));
        require(repeated == variants[i] && compilations == before && states[i].retained.size() == 1,
                "graphics variant cache preserves CB ownership");
        commands[i] = fixture.begin();
        if (pipeline_index == 3) {
            auto begin = init<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
            begin.renderArea = scissor;
            begin.layerCount = 1;
            begin_rendering(commands[i], &begin);
        } else {
            auto begin = init<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
            begin.renderPass = record_pass;
            begin.framebuffer = framebuffer;
            begin.renderArea = scissor;
            vkCmdBeginRenderPass(commands[i], &begin, VK_SUBPASS_CONTENTS_INLINE);
        }
        vkCmdBindPipeline(commands[i], VK_PIPELINE_BIND_POINT_GRAPHICS, variants[i]);
        states[i].native_pipeline = variants[i];
        vkCmdBindDescriptorSets(commands[i], VK_PIPELINE_BIND_POINT_GRAPHICS, record_layout, 0, 1, &sets[i], 0, nullptr);
        vkCmdDraw(commands[i], 3, 1, 0, 0);
        if (pipeline_index == 3)
            end_rendering(commands[i]);
        else
            vkCmdEndRenderPass(commands[i]);
        Fixture::barrier(commands[i], VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        CHECK(vkEndCommandBuffer(commands[i]));
    }
    for (unsigned i = 0; i < kCaseCount; i += kSceneCount)
        require(variants[i] != variants[i + 1], "two scenes retain independent graphics variants");
    for (auto pipeline : logical) consumer_destroy_pipeline(consumers, pipeline, nullptr);
    require(!private_passes.empty(), "recorded variants retain render passes after public pipelines die");
    auto submit = init<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    submit.commandBufferCount = kCaseCount;
    submit.pCommandBuffers = commands;
    CHECK(vkQueueSubmit(fixture.queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(fixture.queue));
    const char* descriptions[] = {"vertex OMM opaque/transparent map and original specialization",
                                  "fragment OMM opaque/transparent map and original specialization",
                                  "single-stage discard vertex opaque/transparent map after ignored-pointer snapshot",
                                  "dynamic-rendering fragment OMM two scenes and original specialization"};
    for (unsigned i = 0; i < kCaseCount; ++i)
        require(*static_cast<const uint32_t*>(outputs[i].mapped) == (i % 2 == 0 ? 137u : 37u), descriptions[i / kSceneCount]);
    vkFreeCommandBuffers(fixture.device, fixture.pool, kCaseCount, commands);
    for (auto& state : states) {
        state.reset();
        require(!state.native_pipeline && !state.logical_pipeline, "binding reset clears both pipeline identities");
    }
    require(private_passes.empty(), "last executable CB reference releases private RP clones");
    consumer_untrack_layout(consumers, record_layout);
    vkDestroyPipelineLayout(fixture.device, record_layout, nullptr);
    consumer_backend_destroy(consumers);
    descriptor_untrack_pool(descriptors, pool);
    vkDestroyDescriptorPool(fixture.device, pool, nullptr);
    descriptor_backend_destroy(descriptors);
    vkDestroyFramebuffer(fixture.device, framebuffer, nullptr);
    vkDestroyRenderPass(fixture.device, record_pass, nullptr);
    std::printf(
        "OMM graphics consumer: %u checks, %u native pipelines; vertex+fragment packed OMM, discard+dynamic rendering, "
        "two scenes, dead shader/layout/RP, feedback/batch/cache/NV groups admission\n",
        checks, compilations);
    return 0;
}
