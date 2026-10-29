// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "native_fixture.h"
#include "../omm_consumers.h"
#include "../omm_metadata.h"
#include "../omm_acceleration.h"
#include <atomic>
#include <cstdlib>
#include <new>
#include <set>
#include <string>
#include <vulkan/utility/vk_struct_helper.hpp>

// Public pipeline admission must remain usable under sustained host OOM. In
// particular, a read-only shader predicate must not copy an entire module.
static std::atomic<bool> deny_allocations{false};
static std::atomic<long long> fail_new_after{-1};
static std::atomic<unsigned> denied_allocations{0};
void* operator new(std::size_t bytes) {
    const auto remaining = fail_new_after.load();
    if (deny_allocations.load() || (remaining >= 0 && fail_new_after.fetch_sub(1) == 0)) {
        ++denied_allocations;
        throw std::bad_alloc();
    }
    if (void* value = std::malloc(bytes ? bytes : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

using namespace vel::omm;
namespace {
unsigned assertions = 0, created = 0;
unsigned compile_required_requests = 0;
bool simulate_compile_required = false;
enum class BatchProbe { None, FailedBase, FailedChild, Success };
BatchProbe batch_probe = BatchProbe::None;
unsigned batch_probe_calls = 0;
bool batch_flags2 = false;
VkPipeline batch_parent{};
std::set<VkPipeline> destroyed;
PFN_vkCreateComputePipelines native_create{};
PFN_vkDestroyPipeline native_destroy{};
void require(bool condition, const char* why) {
    ++assertions;
    if (!condition) {
        std::fprintf(stderr, "FAIL consumer backend: %s\n", why);
        std::exit(1);
    }
}
VKAPI_ATTR VkResult VKAPI_CALL spy_create(VkDevice device, VkPipelineCache cache, uint32_t count,
                                          const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* allocator,
                                          VkPipeline* pipelines) {
    require(count == 1, "backend native pipeline calls are independent compilations");
    auto info = infos[0];
    const auto* feedback = vku::FindStructInPNextChain<VkPipelineCreationFeedbackCreateInfo>(info.pNext);
    const auto* flags2 = vku::FindStructInPNextChain<VkPipelineCreateFlags2CreateInfo>(info.pNext);
    const auto effective_flags = flags2 ? flags2->flags : VkPipelineCreateFlags2(info.flags);
    const unsigned batch_call = batch_probe == BatchProbe::None ? 0 : batch_probe_calls++;
    if (batch_probe != BatchProbe::None) {
        require(bool(flags2) == batch_flags2, "batch preserves the selected effective flag representation");
        if (!batch_call && batch_probe == BatchProbe::FailedBase) {
            pipelines[0] = VK_NULL_HANDLE;
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        if (batch_call == 1) {
            if (batch_probe == BatchProbe::FailedBase)
                require(!(effective_flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) && !info.basePipelineHandle &&
                            info.basePipelineIndex == -1,
                        "failed batch parent leads to a valid independent native child compilation");
            else
                require((effective_flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) && info.basePipelineHandle == batch_parent &&
                            info.basePipelineIndex == -1,
                        "successful batch parent is preserved as the child's native base handle");
            if (batch_probe == BatchProbe::FailedChild) {
                require((effective_flags & VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT) != 0,
                        "child retains its effective early-return flag");
                pipelines[0] = VK_NULL_HANDLE;
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            }
        }
    }
    if (simulate_compile_required) {
        require(flags2 && (effective_flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT),
                "public effective flags2 reaches downstream unchanged");
        ++compile_required_requests;
        pipelines[0] = VK_NULL_HANDLE;
        return VK_PIPELINE_COMPILE_REQUIRED;
    }
    if (feedback)
        require((effective_flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT) != 0,
                "public compile-required flag preserved through flags2");
    else
        require((effective_flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT) == 0,
                "private flags2 removes compile-required behavior");
    // Deliberately emulate feedback output without requiring the optional
    // feedback extension on the fixture's otherwise ordinary Vulkan device.
    if (feedback || flags2) {
        info.pNext = nullptr;
        info.flags = VkPipelineCreateFlags(effective_flags &
                                           ~VkPipelineCreateFlags2(VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT));
    }
    VkResult result = native_create(device, cache, count, &info, allocator, pipelines);
    if (result == VK_SUCCESS) {
        ++created;
        if (batch_probe != BatchProbe::None && batch_call == 0) batch_parent = pipelines[0];
        destroyed.erase(pipelines[0]);  // Drivers may recycle destroyed handles.
        if (feedback) {
            require((info.flags & VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT) != 0, "public pipeline flags preserved");
            if (feedback->pPipelineCreationFeedback)
                *feedback->pPipelineCreationFeedback = {VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT, 0x1234};
            if (feedback->pipelineStageCreationFeedbackCount)
                feedback->pPipelineStageCreationFeedbacks[0] = {VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT, 0x5678};
        }
    }
    return result;
}
VKAPI_ATTR void VKAPI_CALL spy_destroy(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* allocator) {
    require(destroyed.insert(pipeline).second, "native pipeline destroyed exactly once per live creation");
    native_destroy(device, pipeline, allocator);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL consumer_proc(VkDevice device, const char* name) {
    if (!std::strcmp(name, "vkCreateComputePipelines")) return reinterpret_cast<PFN_vkVoidFunction>(spy_create);
    if (!std::strcmp(name, "vkDestroyPipeline")) return reinterpret_cast<PFN_vkVoidFunction>(spy_destroy);
    return vkGetDeviceProcAddr(device, name);
}
struct IdentityMap {
    std::vector<std::pair<VkAccelerationStructureKHR, DescriptorASIdentity>> values;
};
VkResult resolve(void* user, VkAccelerationStructureKHR handle, DescriptorASIdentity* output) {
    for (const auto& value : static_cast<IdentityMap*>(user)->values)
        if (value.first == handle) {
            *output = value.second;
            return VK_SUCCESS;
        }
    return VK_ERROR_VALIDATION_FAILED_EXT;
}
std::vector<uint32_t> read_shader(const char* path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(bool(input) && input.tellg() > 0 && input.tellg() % 4 == 0, "shader file");
    std::vector<uint32_t> code(size_t(input.tellg()) / 4);
    input.seekg(0);
    require(bool(input.read(reinterpret_cast<char*>(code.data()), code.size() * 4)), "shader read");
    return code;
}
const char application[] = R"GLSL(#version 460
#extension GL_EXT_ray_query : require
layout(local_size_x=1) in;
layout(constant_id=0) const uint originalSpecConstant=3u;
layout(set=0,binding=0) uniform accelerationStructureEXT scene;
layout(set=0,binding=1,std430) buffer ApplicationOutput { uint value; } result;
layout(set=7,binding=0) uniform sampler unusedOtherSet;
void main() {
    rayQueryEXT query;
    rayQueryInitializeEXT(query,scene,0u,255u,vec3(0.25,0.25,-1.0),0.0,vec3(0,0,1),2.0);
    uint visible=0u;
    while(rayQueryProceedEXT(query)) ++visible;
    result.value=originalSpecConstant+100u*rayQueryGetIntersectionTypeEXT(query,true)+1000u*visible;
}
)GLSL";
Buffer upload(Fixture& fixture, const void* data, size_t bytes) {
    auto buffer = fixture.buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    std::memcpy(buffer.mapped, data, bytes);
    return buffer;
}
Buffer root_for(Fixture& fixture, const std::vector<uint8_t>& bytes, uint32_t kind) {
    auto body = upload(fixture, bytes.data(), bytes.size());
    MetadataRootCell root{kMetadataRootMagic, kMetadataVersion, kind, kMetadataReady, body.address, bytes.size(), 1, {0, 0, 0}};
    return upload(fixture, &root, sizeof(root));
}
VkDescriptorSetLayout set_layout(Fixture& fixture, DescriptorBackend* descriptors, ConsumerBackend* consumers, uint32_t binding,
                                 uint32_t count, bool as_output = false) {
    VkDescriptorSetLayoutBinding entries[] = {
        {binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, count, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    auto info = init<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    info.bindingCount = as_output ? 2 : 1;
    info.pBindings = entries;
    VkDescriptorSetLayout result{};
    CHECK(vkCreateDescriptorSetLayout(fixture.device, &info, nullptr, &result));
    CHECK(descriptor_track_layout(descriptors, result, &info));
    CHECK(consumer_track_set_layout(consumers, result, &info));
    return result;
}
VkPipelineLayout pipeline_layout(Fixture& fixture, ConsumerBackend* consumers, const std::vector<VkDescriptorSetLayout>& sets,
                                 uint32_t push_bytes = 0) {
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
    auto info = init<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    info.setLayoutCount = uint32_t(sets.size());
    info.pSetLayouts = sets.data();
    info.pushConstantRangeCount = push_bytes ? 1 : 0;
    info.pPushConstantRanges = push_bytes ? &range : nullptr;
    VkPipelineLayout result{};
    CHECK(vkCreatePipelineLayout(fixture.device, &info, nullptr, &result));
    CHECK(consumer_track_layout(consumers, result, &info));
    return result;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[1], "--write-shader")) {
        std::ofstream output(argv[2]);
        output << application;
        return output ? 0 : 1;
    }
    if (argc != 3) {
        std::fprintf(stderr, "usage: test_omm_consumers library.spv application.spv | --write-shader application.comp\n");
        return 1;
    }
    const auto library = read_shader(argv[1]), app = read_shader(argv[2]);
    Fixture fixture;
    if (!fixture.open()) return 77;
    native_create = reinterpret_cast<PFN_vkCreateComputePipelines>(vkGetDeviceProcAddr(fixture.device, "vkCreateComputePipelines"));
    native_destroy = reinterpret_cast<PFN_vkDestroyPipeline>(vkGetDeviceProcAddr(fixture.device, "vkDestroyPipeline"));
    IdentityMap identities;
    DescriptorBackend* descriptors{};
    CHECK(descriptor_backend_create({fixture.physical, fixture.device, vkGetDeviceProcAddr, nullptr, resolve, &identities},
                                    &descriptors));
    ConsumerBackend* consumers{};
    CHECK(consumer_backend_create({fixture.device, consumer_proc, descriptors, nullptr, library.data(), library.size() * 4},
                                  &consumers));

    const auto a = set_layout(fixture, descriptors, consumers, 1, 1, true);
    const auto b = set_layout(fixture, descriptors, consumers, 3, 1);
    const auto c = set_layout(fixture, descriptors, consumers, 3, 2);
    const auto abb = pipeline_layout(fixture, consumers, {a, b, b});
    const auto acb = pipeline_layout(fixture, consumers, {a, c, b});
    const auto acb_push = pipeline_layout(fixture, consumers, {a, c, b}, 4);
    ConsumerBindingState compatibility;
    VkDescriptorSet fake[] = {reinterpret_cast<VkDescriptorSet>(uintptr_t(0x12340000)),
                              reinterpret_cast<VkDescriptorSet>(uintptr_t(0x12350000)),
                              reinterpret_cast<VkDescriptorSet>(uintptr_t(0x12360000))};
    CHECK(consumer_bind_sets(consumers, compatibility, abb, 0, 3, fake));
    const auto replacement = reinterpret_cast<VkDescriptorSet>(uintptr_t(0x12370000));
    CHECK(consumer_bind_sets(consumers, compatibility, acb, 1, 1, &replacement));
    require(compatibility.sets[0] == fake[0], "[A,B] to [A,C] preserves compatible lower set0");
    require(compatibility.sets[1] == replacement && !compatibility.sets[2], "incompatible previous set1 disturbs higher set2");
    require(bool(compatibility.set_layouts[2]), "disturbed undefined binding retains layout identity");
    CHECK(consumer_bind_sets(consumers, compatibility, abb, 0, 1, fake));
    require(compatibility.sets[1] == replacement, "compatible previous set0 preserves higher set1");
    CHECK(consumer_bind_sets(consumers, compatibility, acb_push, 0, 1, fake));
    require(!compatibility.sets[1] && !compatibility.sets[2], "incompatible push ranges disturb higher sets");
    compatibility.reset();
    for (long long fault = 0; fault < 2; ++fault) {
        ConsumerBindingState allocation_failure;
        CHECK(consumer_bind_sets(consumers, allocation_failure, abb, 0, 1, fake));
        const auto old_layout = allocation_failure.set_layouts[0];
        fail_new_after = fault;
        const auto result = consumer_bind_sets(consumers, allocation_failure, abb, 0, 3, fake);
        fail_new_after = -1;
        require(result == VK_ERROR_OUT_OF_HOST_MEMORY, "each bound-set array growth can fail with OOM");
        require(allocation_failure.sets.size() == 1 && allocation_failure.set_layouts.size() == 1 &&
                    allocation_failure.sets[0] == fake[0] && allocation_failure.set_layouts[0] == old_layout,
                "failed bound-set growth preserves both logical sizes and the original binding");
        CHECK(consumer_bind_sets(consumers, allocation_failure, abb, 0, 3, fake));
        require(allocation_failure.sets.size() == 3 && allocation_failure.set_layouts.size() == 3 &&
                    allocation_failure.sets[2] == fake[2],
                "bound-set growth retries successfully after OOM");
    }
    ConsumerBindingState inconsistent;
    inconsistent.sets.push_back(fake[0]);
    VkPipeline rejected{};
    require(consumer_pipeline(consumers, inconsistent, VK_NULL_HANDLE, &rejected) == VK_ERROR_INITIALIZATION_FAILED,
            "pipeline admission defensively rejects mismatched binding arrays before indexing");

    // Real native scene plus BVH-independent ordinary-geometry metadata. Both
    // TLASes use one BLAS. Instance force-nonopaque distinguishes their behavior.
    constexpr auto input_usage =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    auto vertices = fixture.buffer(36, input_usage, true);
    const float triangle[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::memcpy(vertices.mapped, triangle, sizeof(triangle));
    auto geometry = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    auto& triangles = geometry.geometry.triangles;
    triangles =
        init<VkAccelerationStructureGeometryTrianglesDataKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR);
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = vertices.address;
    triangles.vertexStride = 12;
    triangles.maxVertex = 2;
    triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    auto blas = fixture.acceleration(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geometry);
    MetadataGeometryInput metadata{};
    metadata.plan = {kMetadataTriangles, VK_GEOMETRY_OPAQUE_BIT_KHR, 1, false, 0};
    std::vector<uint8_t> packed;
    require(metadata_pack_blas(&metadata, 1, 0, 1, &packed) == MetadataResult::Success, "ordinary BLAS metadata");
    auto blas_root = root_for(fixture, packed, kMetadataBlas);
    VkAccelerationStructureGeometryKHR instance_geometry[2]{};
    Acceleration scenes[2];
    for (unsigned i = 0; i < 2; ++i) {
        auto instance_buffer = fixture.buffer(sizeof(VkAccelerationStructureInstanceKHR), input_usage, true);
        auto& instance = *static_cast<VkAccelerationStructureInstanceKHR*>(instance_buffer.mapped);
        instance = {};
        for (unsigned axis = 0; axis < 3; ++axis) instance.transform.matrix[axis][axis] = 1;
        instance.mask = 255;
        instance.accelerationStructureReference = blas.address;
        instance.flags = i ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR : 0;
        instance_geometry[i] = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
        instance_geometry[i].geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        instance_geometry[i].geometry.instances = init<VkAccelerationStructureGeometryInstancesDataKHR>(
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR);
        instance_geometry[i].geometry.instances.data.deviceAddress = instance_buffer.address;
        scenes[i] = fixture.acceleration(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, instance_geometry[i]);
        MetadataInstance logical{blas_root.address, blas.address, uint32_t(instance.flags), kMetadataInstanceActive, 0};
        require(metadata_pack_tlas(&logical, 1, 0, 1, &packed) == MetadataResult::Success, "TLAS metadata");
        auto scene_root = root_for(fixture, packed, kMetadataTlas);
        const AccelerationIdentityCell payload{kAccelerationIdentityMagic, 1, 1, 0, scenes[i].address, scene_root.address};
        const auto cell = upload(fixture, &payload, sizeof(payload));
        identities.values.push_back({scenes[i].handle, {scenes[i].address, scene_root.address, {}, cell.address}});
    }
    auto build_command = fixture.begin();
    const auto* blas_range = &blas.range;
    fixture.buildAS(build_command, 1, &blas.build, &blas_range);
    Fixture::barrier(build_command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                     VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                     VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    for (auto& scene : scenes) {
        const auto* range = &scene.range;
        fixture.buildAS(build_command, 1, &scene.build, &range);
    }
    Fixture::barrier(build_command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                     VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    fixture.submit(build_command);
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    auto pool_info = init<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    pool_info.maxSets = 2;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = sizes;
    VkDescriptorPool pool{};
    CHECK(vkCreateDescriptorPool(fixture.device, &pool_info, nullptr, &pool));
    CHECK(descriptor_track_pool(descriptors, pool, &pool_info));
    VkDescriptorSetLayout layouts[] = {a, a};
    auto allocate = init<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    allocate.descriptorPool = pool;
    allocate.descriptorSetCount = 2;
    allocate.pSetLayouts = layouts;
    VkDescriptorSet sets[2]{};
    CHECK(vkAllocateDescriptorSets(fixture.device, &allocate, sets));
    CHECK(descriptor_allocate_sets(descriptors, &allocate, sets));
    Buffer outputs[2];
    for (unsigned i = 0; i < 2; ++i) {
        outputs[i] = fixture.buffer(4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        VkDescriptorBufferInfo buffer{outputs[i].handle, 0, 4};
        auto acceleration =
            init<VkWriteDescriptorSetAccelerationStructureKHR>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR);
        acceleration.accelerationStructureCount = 1;
        acceleration.pAccelerationStructures = &scenes[i].handle;
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
        writes[1].pBufferInfo = &buffer;
        CHECK(descriptor_update(descriptors, 2, writes, 0, nullptr));
        vkUpdateDescriptorSets(fixture.device, 2, writes, 0, nullptr);
    }
    const auto layout = pipeline_layout(fixture, consumers, {a});
    auto shader_info = init<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    shader_info.codeSize = app.size() * 4;
    shader_info.pCode = app.data();
    VkShaderModule module{};
    CHECK(consumer_create_shader(consumers, &shader_info, nullptr, &module));
    auto plain_shader_info = shader_info;
    plain_shader_info.codeSize = library.size() * 4;
    plain_shader_info.pCode = library.data();
    VkShaderModule plain_module{};
    CHECK(consumer_create_shader(consumers, &plain_shader_info, nullptr, &plain_module));
    auto query_stage = init<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    query_stage.module = module;
    auto plain_stage = query_stage;
    plain_stage.module = plain_module;
    auto inline_query_stage = query_stage;
    inline_query_stage.module = VK_NULL_HANDLE;
    inline_query_stage.pNext = &shader_info;
    auto inline_plain_stage = inline_query_stage;
    inline_plain_stage.pNext = &plain_shader_info;
    auto unknown_stage = inline_query_stage;
    unknown_stage.pNext = nullptr;
    static_assert(noexcept(consumer_shader_uses_query(consumers, query_stage)));
    denied_allocations = 0;
    deny_allocations = true;
    const bool tracked_query = consumer_shader_uses_query(consumers, query_stage);
    const bool tracked_plain = consumer_shader_uses_query(consumers, plain_stage);
    const bool inline_query = consumer_shader_uses_query(consumers, inline_query_stage);
    const bool inline_plain = consumer_shader_uses_query(consumers, inline_plain_stage);
    const bool unknown_query = consumer_shader_uses_query(consumers, unknown_stage);
    deny_allocations = false;
    require(tracked_query, "query admission inspects retained original module under sustained OOM");
    require(!tracked_plain, "ordinary retained module passes admission under sustained OOM");
    require(inline_query, "query admission inspects inline source under sustained OOM");
    require(!inline_plain, "ordinary inline module passes admission under sustained OOM");
    require(unknown_query, "unknown identifier/binary source stays conservatively guarded under sustained OOM");
    require(denied_allocations.load() == 0, "shader admission attempts no C++ allocation");
    consumer_destroy_shader(consumers, plain_module, nullptr);
    VkPipelineCreationFeedback feedback{}, stage_feedback{};
    auto feedback_info = init<VkPipelineCreationFeedbackCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO);
    feedback_info.pPipelineCreationFeedback = &feedback;
    feedback_info.pipelineStageCreationFeedbackCount = 1;
    feedback_info.pPipelineStageCreationFeedbacks = &stage_feedback;
    auto flags2_info = init<VkPipelineCreateFlags2CreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO);
    flags2_info.flags = VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT | VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    feedback_info.pNext = &flags2_info;
    uint32_t spec_value = 37;
    VkSpecializationMapEntry spec_entry{0, 0, 4};
    VkSpecializationInfo specialization{1, &spec_entry, 4, &spec_value};
    auto create = init<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    create.pNext = &feedback_info;
    create.flags = VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
    create.layout = layout;
    create.basePipelineIndex = -1;
    create.stage = init<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    create.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    create.stage.module = module;
    create.stage.pName = "main";
    create.stage.pSpecializationInfo = &specialization;
    VkPipeline logical{};
    CHECK(consumer_create_compute(consumers, VK_NULL_HANDLE, 1, &create, nullptr, &logical));
    require(feedback.duration == 0x1234 && stage_feedback.duration == 0x5678 &&
                feedback.flags == VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT,
            "public feedback copied back from downstream owned chain");
    auto early_info = flags2_info;
    early_info.flags |= VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT;
    VkComputePipelineCreateInfo early[] = {create, create};
    for (auto& info : early) {
        info.pNext = &early_info;
        info.flags = 0;
    }
    VkPipeline failed[2]{};
    simulate_compile_required = true;
    require(consumer_create_compute(consumers, VK_NULL_HANDLE, 2, early, nullptr, failed) == VK_PIPELINE_COMPILE_REQUIRED,
            "public compile-required result preserved");
    simulate_compile_required = false;
    require(compile_required_requests == 1 && !failed[0] && !failed[1], "effective flags2 early-return stops later batch entries");
    for (bool use_flags2 : {false, true}) {
        for (auto probe : {BatchProbe::FailedBase, BatchProbe::FailedChild, BatchProbe::Success}) {
            VkComputePipelineCreateInfo batch[] = {create, create, create};
            VkPipelineCreateFlags2CreateInfo flags[3]{};
            VkPipelineCreationFeedback values[3]{}, stages[3]{};
            VkPipelineCreationFeedbackCreateInfo feedbacks[3]{};
            for (unsigned i = 0; i < 3; ++i) {
                auto effective = VkPipelineCreateFlags2(VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT);
                if (i == 1) effective |= VK_PIPELINE_CREATE_DERIVATIVE_BIT;
                if (i == 1 && probe == BatchProbe::FailedChild) effective |= VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT;
                if (use_flags2) effective |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
                flags[i] = init<VkPipelineCreateFlags2CreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO);
                flags[i].flags = effective;
                feedbacks[i] = init<VkPipelineCreationFeedbackCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO);
                feedbacks[i].pNext = &flags[i];
                feedbacks[i].pPipelineCreationFeedback = &values[i];
                feedbacks[i].pipelineStageCreationFeedbackCount = 1;
                feedbacks[i].pPipelineStageCreationFeedbacks = &stages[i];
                batch[i].pNext = use_flags2 ? &feedbacks[i] : nullptr;
                batch[i].flags = use_flags2 ? 0 : VkPipelineCreateFlags(effective);
                batch[i].basePipelineHandle = VK_NULL_HANDLE;
                batch[i].basePipelineIndex = i == 1 ? 0 : -1;
            }
            batch_probe = probe;
            batch_probe_calls = 0;
            batch_flags2 = use_flags2;
            batch_parent = VK_NULL_HANDLE;
            VkPipeline pipelines[3]{};
            const auto result = consumer_create_compute(consumers, VK_NULL_HANDLE, 3, batch, nullptr, pipelines);
            batch_probe = BatchProbe::None;
            require(result == (probe == BatchProbe::Success ? VK_SUCCESS : VK_ERROR_OUT_OF_DEVICE_MEMORY),
                    "batch returns an actual pipeline failure while preserving successful entries");
            if (probe == BatchProbe::FailedBase)
                require(batch_probe_calls == 3 && !pipelines[0] && pipelines[1] && pipelines[2],
                        "failed parent does not prevent successful child and unrelated pipeline attempts");
            else if (probe == BatchProbe::FailedChild)
                require(batch_probe_calls == 2 && pipelines[0] && !pipelines[1] && !pipelines[2],
                        "failed child with early-return suppresses only later pipeline attempts");
            else
                require(batch_probe_calls == 3 && pipelines[0] && pipelines[1] && pipelines[2],
                        "successful derivative batch creates every requested pipeline");
            require(batch[1].basePipelineIndex == 0 && !batch[1].basePipelineHandle &&
                        ((use_flags2 ? flags[1].flags : batch[1].flags) & VK_PIPELINE_CREATE_DERIVATIVE_BIT),
                    "batch translation preserves original parent index and effective derivative flag");
            if (use_flags2 && probe != BatchProbe::FailedChild)
                require(values[1].duration == 0x1234 && stages[1].duration == 0x5678,
                        "owned independent-child feedback propagates to the original public output pointers");
            for (auto pipeline : pipelines)
                if (pipeline) consumer_destroy_pipeline(consumers, pipeline, nullptr);
        }
    }
    consumer_destroy_shader(consumers, module, nullptr);  // No live original module at deferred variant creation.
    // Mutating original specialization storage must not change future variants.
    spec_value = 99;
    ConsumerBindingState states[2];
    VkPipeline variants[2]{};
    VkCommandBuffer commands[] = {fixture.begin(), fixture.begin()};
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(consumer_bind_sets(consumers, states[i], layout, 0, 1, &sets[i]));
        CHECK(consumer_pipeline(consumers, states[i], logical, &variants[i]));
        VkPipeline repeated{};
        const unsigned before = created;
        CHECK(consumer_pipeline(consumers, states[i], logical, &repeated));
        require(repeated == variants[i] && created == before && states[i].retained.size() == 1,
                "live variant cache reuses pipeline once per CB");
        vkCmdBindPipeline(commands[i], VK_PIPELINE_BIND_POINT_COMPUTE, variants[i]);
        vkCmdBindDescriptorSets(commands[i], VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &sets[i], 0, nullptr);
        vkCmdDispatch(commands[i], 1, 1, 1);
        Fixture::barrier(commands[i], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         VK_ACCESS_HOST_READ_BIT);
        CHECK(vkEndCommandBuffer(commands[i]));
    }
    require(variants[0] != variants[1], "independent bound scenes produce independent immutable variants");
    auto submit = init<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    submit.commandBufferCount = 2;
    submit.pCommandBuffers = commands;
    CHECK(vkQueueSubmit(fixture.queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(fixture.queue));
    require(*static_cast<uint32_t*>(outputs[0].mapped) == 137, "opaque scene autocommits with original SpecId0 specialization");
    require(*static_cast<uint32_t*>(outputs[1].mapped) == 1037, "other CB keeps force-nonopaque scene and original specialization");
    vkFreeCommandBuffers(fixture.device, fixture.pool, 2, commands);
    states[0].reset();
    require(destroyed.count(variants[0]) && !destroyed.count(variants[1]),
            "CB reset retires only its own variant, no pipeline strong cache");
    for (unsigned i = 0; i < 8; ++i) {
        ConsumerBindingState churn;
        CHECK(consumer_bind_sets(consumers, churn, layout, 0, 1, &sets[0]));
        VkPipeline temporary{};
        CHECK(consumer_pipeline(consumers, churn, logical, &temporary));
        require(!destroyed.count(temporary), "new churn variant alive");
        churn.reset();
        require(destroyed.count(temporary), "expired variant retires while public pipeline stays alive");
    }
    consumer_destroy_pipeline(consumers, logical, nullptr);
    require(!destroyed.count(variants[1]), "recorded variant retains resources after public pipeline destruction");
    states[1].reset();
    require(destroyed.count(variants[1]), "last CB releases retained variant");
    for (auto value : {layout, abb, acb, acb_push}) {
        consumer_untrack_layout(consumers, value);
        vkDestroyPipelineLayout(fixture.device, value, nullptr);
    }
    for (auto value : {a, b, c}) {
        consumer_untrack_set_layout(consumers, value);
        descriptor_untrack_layout(descriptors, value);
        vkDestroyDescriptorSetLayout(fixture.device, value, nullptr);
    }
    consumer_backend_destroy(consumers);
    descriptor_untrack_pool(descriptors, pool);
    vkDestroyDescriptorPool(fixture.device, pool, nullptr);
    descriptor_backend_destroy(descriptors);
    std::printf(
        "OMM consumer backend: %u checks, %u native pipeline creations, two native traversals, "
        "lifecycle/cache/feedback/spec/layout regressions\n",
        assertions, created);
    return 0;
}
