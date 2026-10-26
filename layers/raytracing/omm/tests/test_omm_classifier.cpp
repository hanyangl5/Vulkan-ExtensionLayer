// Copyright 2022-2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0

#include "native_fixture.h"
#include "../omm_metadata.h"
#include "../omm_opacity.h"
#include "../omm_query_rewrite.h"
#include "../omm_shader_compose.h"
#include "../omm_storage.h"
#include <array>
#include <string>

using namespace vel::omm;

namespace {
void require(bool condition, const std::string& text) {
    if (!condition) {
        std::fprintf(stderr, "FAIL classifier: %s\n", text.c_str());
        std::exit(1);
    }
}
std::vector<uint32_t> read_shader(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(bool(file), std::string("open ") + path);
    const auto bytes = file.tellg();
    require(bytes > 0 && bytes % 4 == 0, "shader byte count");
    std::vector<uint32_t> words(static_cast<size_t>(bytes) / 4);
    file.seekg(0);
    require(bool(file.read(reinterpret_cast<char*>(words.data()), bytes)), "shader read");
    return words;
}
VkPipeline make_pipeline(Fixture& fixture, VkPipelineLayout layout, const std::vector<uint32_t>& words) {
    auto info = init<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    info.codeSize = words.size() * 4;
    info.pCode = words.data();
    VkShaderModule module;
    CHECK(vkCreateShaderModule(fixture.device, &info, nullptr, &module));
    auto pipeline_info = init<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    pipeline_info.layout = layout;
    pipeline_info.stage = init<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = module;
    pipeline_info.stage.pName = "main";
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(fixture.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline));
    vkDestroyShaderModule(fixture.device, module, nullptr);
    return pipeline;
}
void release(Fixture& fixture, Buffer value) {
    const auto found = std::find_if(fixture.buffers.begin(), fixture.buffers.end(),
                                    [&](const Buffer& buffer) { return buffer.handle == value.handle; });
    require(found != fixture.buffers.end(), "buffer bookkeeping");
    if (value.mapped) vkUnmapMemory(fixture.device, value.memory);
    vkDestroyBuffer(fixture.device, value.handle, nullptr);
    vkFreeMemory(fixture.device, value.memory, nullptr);
    fixture.buffers.erase(found);
}
// Independent inverse oracle from Apache-2.0 Khronos CTS at revision
// d18e4c68543103371715845cd914c71e5859963e. The shader only receives the resulting
// barycentrics, not the known index or expected opacity decision.
std::array<float, 2> centroid(uint32_t index) {
    uint32_t d = index;
    d = ((d >> 1) & 0x22222222u) | ((d << 1) & 0x44444444u) | (d & 0x99999999u);
    d = ((d >> 2) & 0x0c0c0c0cu) | ((d << 2) & 0x30303030u) | (d & 0xc3c3c3c3u);
    d = ((d >> 4) & 0x00f000f0u) | ((d << 4) & 0x0f000f00u) | (d & 0xf00ff00fu);
    d = ((d >> 8) & 0x0000ff00u) | ((d << 8) & 0x00ff0000u) | (d & 0xff0000ffu);
    uint32_t f = (d & 0xffffu) | ((d << 16) & ~d);
    f ^= (f >> 1) & 0x7fff7fffu;
    f ^= (f >> 2) & 0x3fff3fffu;
    f ^= (f >> 4) & 0x0fff0fffu;
    f ^= (f >> 8) & 0x00ff00ffu;
    const uint32_t t = (f ^ d) >> 16;
    const uint32_t iu = ((f & ~t) | (d & ~t) | (~d & ~f & t)) & 3u;
    const uint32_t iv = ((f >> 16) ^ d) & 3u;
    const uint32_t iw = ((~f & ~t) | (d & ~t) | (~d & f & t)) & 3u;
    const float fraction = ((iu ^ iv ^ iw) & 1) ? 1.f / 3.f : 2.f / 3.f;
    return {(static_cast<float>(iu) + fraction) * 0.25f, (static_cast<float>(iv) + fraction) * 0.25f};
}
struct MicromapPush {
    uint64_t triangles, data, output, status, triangles_bytes, data_bytes, output_bytes, count, stride;
    uint64_t triangles_accessible, data_accessible, phase;
};
static_assert(sizeof(MicromapPush) == 96);
struct QueryOutput {
    uint32_t flags, index, mode, reserved;
    float u, v;
    uint32_t visible, committed, instance, logical_flags, candidate_flags, aabb_opaque;
    float t;
};
static_assert(sizeof(QueryOutput) == 52 && offsetof(QueryOutput, visible) == 24);
}  // namespace

int main(int argc, char** argv) {
    std::array<std::string, 4> paths;
    if (argc == 2) {
        const std::string directory = argv[1];
        paths = {directory + "/classifier_query.comp.spv", directory + "/classifier_library.comp.spv",
                 directory + "/omm_build.comp.spv", directory + "/omm_metadata.comp.spv"};
    } else if (argc == 5) {
        for (size_t i = 0; i < paths.size(); ++i) paths[i] = argv[i + 1];
    } else {
        return 1;
    }
    const auto application = read_shader(paths[0].c_str());
    const auto library = read_shader(paths[1].c_str());
    uint32_t classifier = 0;
    for (size_t offset = 5; offset < library.size(); offset += library[offset] >> 16)
        if ((library[offset] & 0xffffu) == 5u &&
            std::string(reinterpret_cast<const char*>(library.data() + offset + 2)).find("ommClassify(") == 0)
            classifier = library[offset + 1];
    require(classifier != 0, "library preserves named classifier closure (--keep-uncalled)");
    ComposedQueryShader composed;
    std::string diagnostic;
    require(compose_query_classifier(application, library, classifier, &composed, &diagnostic) == ShaderCompositionStatus::Success,
            "production classifier composition: " + diagnostic);
    Fixture fixture;
    if (!fixture.open()) return 77;
    const auto bda_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const auto as_usage =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    constexpr uint32_t count = 8;
    auto cells = fixture.buffer((count + 1) * sizeof(MetadataRootCell), bda_usage, true);
    auto* root = static_cast<MetadataRootCell*>(cells.mapped);
    std::memset(cells.mapped, 0, (count + 1) * sizeof(MetadataRootCell));
    const auto scene_address = cells.address + count * sizeof(MetadataRootCell);

    // The state stream is packed data, not precomputed candidate decisions.
    // GPU build produces the canonical object; GPU metadata build independently
    // normalizes original-width indices and copies the object's active bytes.
    const uint32_t triangle_words[] = {0, (2u << 16) | 2u, 4, (1u << 16) | 2u};
    const uint8_t state_bytes[] = {0xe4, 0xe4, 0xe4, 0xe4, 0xa5, 0xa5, 0xcc, 0xcc};
    auto triangle_input = fixture.buffer(sizeof(triangle_words), bda_usage, true);
    auto state_input = fixture.buffer(sizeof(state_bytes), bda_usage, true);
    std::memcpy(triangle_input.mapped, triangle_words, sizeof(triangle_words));
    std::memcpy(state_input.mapped, state_bytes, sizeof(state_bytes));
    const StorageUsage usages[] = {{1, 2, StorageFormat::FourState}, {1, 2, StorageFormat::TwoState}};
    vel::omm::StorageLayout canonical_plan;
    require(storage_plan(2, usages, nullptr, &canonical_plan) == StorageResult::Success, "canonical sizing");
    const auto canonical_capacity = canonical_plan.size + 64;
    auto canonical = fixture.buffer(canonical_capacity, bda_usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    auto indices = fixture.buffer(count * 4, bda_usage, true);
    std::memset(indices.mapped, 0xa5, count * 4);
    for (uint32_t i = 0; i < count; ++i) {
        const uint16_t raw = i >= 4 ? static_cast<uint16_t>(UINT16_MAX - (i - 4)) : 0;
        std::memcpy(static_cast<uint8_t*>(indices.mapped) + i * 4 + 2, &raw, sizeof(raw));
    }
    std::array<Buffer, count> bodies;
    std::array<MetadataGeometrySource, count> jobs{};
    for (uint32_t i = 0; i < count; ++i) {
        const bool associated = i == 0 || i >= 3;
        const uint64_t snapshot_capacity = i == 0 || i == 3 ? canonical_capacity : 0;
        const MetadataGeometryPlan plan{i == 2 ? kMetadataAabbs : kMetadataTriangles, 1, 1, associated, snapshot_capacity};
        uint64_t body_bytes;
        require(metadata_plan_blas(&plan, 1, &jobs[i].geometry, 1, &body_bytes) == MetadataResult::Success, "metadata sizing");
        bodies[i] = fixture.buffer(body_bytes + 64, bda_usage, false);
        root[i] = {kMetadataRootMagic, kMetadataVersion, kMetadataBlas, kMetadataReady, bodies[i].address, body_bytes + 64, 7,
                   {0, 0, 0}};
        if (associated) {
            jobs[i].indices_address = indices.address + i * 4;
            jobs[i].indices_accessible_bytes = 4;
            jobs[i].indices_offset = 2;
            jobs[i].indices_bytes = 2;
            jobs[i].index_stride = 2;
            jobs[i].index_width = 16;
            jobs[i].base_triangle = i >= 4 ? UINT32_MAX : (i == 3 ? 1 : 0);
        }
        if (snapshot_capacity) {
            jobs[i].snapshot_address = canonical.address;
            jobs[i].source_snapshot_capacity = canonical_capacity;
        }
    }
    auto job_input = fixture.buffer(sizeof(jobs), bda_usage, true);
    std::memcpy(job_input.mapped, jobs.data(), sizeof(jobs));
    auto statuses = fixture.buffer((count + 1) * 4, bda_usage, true);
    std::memset(statuses.mapped, 0xff, (count + 1) * 4);
    VkPushConstantRange private_range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(MicromapPush)};
    auto private_info = init<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    private_info.pushConstantRangeCount = 1;
    private_info.pPushConstantRanges = &private_range;
    VkPipelineLayout private_layout;
    CHECK(vkCreatePipelineLayout(fixture.device, &private_info, nullptr, &private_layout));
    const auto build_pipeline = make_pipeline(fixture, private_layout, read_shader(paths[2].c_str()));
    const auto metadata_pipeline = make_pipeline(fixture, private_layout, read_shader(paths[3].c_str()));
    auto command = fixture.begin();
    vkCmdFillBuffer(command, canonical.handle, 0, canonical_capacity, 0xcdcdcdcd);
    Fixture::barrier(command, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    MicromapPush build{triangle_input.address,
                       state_input.address,
                       canonical.address,
                       statuses.address,
                       sizeof(triangle_words),
                       6,
                       canonical_plan.size,
                       2,
                       8,
                       sizeof(triangle_words),
                       sizeof(state_bytes),
                       0};
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, build_pipeline);
    vkCmdPushConstants(command, private_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(build), &build);
    vkCmdDispatch(command, 1, 1, 1);
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    build.phase = 1;
    vkCmdPushConstants(command, private_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(build), &build);
    vkCmdDispatch(command, 128, 1, 1);
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, metadata_pipeline);
    for (uint32_t i = 0; i < count; ++i) {
        MetadataBuildPush push{job_input.address + i * sizeof(MetadataGeometrySource),
                               bodies[i].address,
                               root[i].body_capacity,
                               statuses.address + (i + 1) * 4,
                               1,
                               7,
                               0,
                               0x80,
                               0};
        vkCmdPushConstants(command, private_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(command, 1, 1, 1);
        Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        push.phase = 1;
        vkCmdPushConstants(command, private_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(command, 128, 1, 1);
        Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT);
    }
    fixture.submit(command);
    for (uint32_t i = 0; i <= count; ++i)
        require(static_cast<const uint32_t*>(statuses.mapped)[i] == 0, "GPU producer status " + std::to_string(i));
    // Only completion statuses were read. No GPU state/association/decision is
    // read back or reconstructed on the CPU before the shader consumes it.
    release(fixture, triangle_input);
    release(fixture, state_input);
    release(fixture, indices);
    release(fixture, canonical);
    release(fixture, job_input);
    release(fixture, statuses);

    auto vertices = fixture.buffer(9 * sizeof(float), as_usage, true);
    const float triangle[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::memcpy(vertices.mapped, triangle, sizeof(triangle));
    auto boxes = fixture.buffer(sizeof(VkAabbPositionsKHR), as_usage, true);
    *static_cast<VkAabbPositionsKHR*>(boxes.mapped) = {3, 0, 0, 4, 1, 0.5};
    std::array<VkAccelerationStructureGeometryKHR, count> geometries;
    std::array<Acceleration, count> native;
    for (uint32_t i = 0; i < count; ++i) {
        auto& geometry = geometries[i];
        geometry = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
        geometry.geometryType = i == 2 ? VK_GEOMETRY_TYPE_AABBS_KHR : VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        if (i == 2) {
            geometry.geometry.aabbs =
                init<VkAccelerationStructureGeometryAabbsDataKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR);
            geometry.geometry.aabbs.data.deviceAddress = boxes.address;
            geometry.geometry.aabbs.stride = sizeof(VkAabbPositionsKHR);
        } else {
            auto& data = geometry.geometry.triangles;
            data = init<VkAccelerationStructureGeometryTrianglesDataKHR>(
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR);
            data.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            data.vertexData.deviceAddress = vertices.address;
            data.vertexStride = 12;
            data.maxVertex = 2;
            data.indexType = VK_INDEX_TYPE_NONE_KHR;
        }
        native[i] = fixture.acceleration(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geometry);
    }
    auto instance_input = fixture.buffer(count * sizeof(VkAccelerationStructureInstanceKHR), as_usage, true);
    auto* instance_values = static_cast<VkAccelerationStructureInstanceKHR*>(instance_input.mapped);
    std::array<MetadataInstance, count> metadata_instances;
    for (uint32_t i = 0; i < count; ++i) {
        instance_values[i] = {};
        for (uint32_t axis = 0; axis < 3; ++axis) instance_values[i].transform.matrix[axis][axis] = 1;
        if (i == 1) instance_values[i].transform.matrix[2][3] = 1;
        if (i == 3) instance_values[i].transform.matrix[0][3] = 5;
        if (i >= 4) instance_values[i].transform.matrix[0][3] = 7.f + 2.f * static_cast<float>(i - 4);
        instance_values[i].mask = 255;
        instance_values[i].instanceCustomIndex = 77;
        instance_values[i].accelerationStructureReference = native[i].address;
        metadata_instances[i] = {cells.address + i * sizeof(MetadataRootCell), native[i].address, 0, kMetadataInstanceActive, 0};
    }
    auto scene_geometry = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
    scene_geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    scene_geometry.geometry.instances =
        init<VkAccelerationStructureGeometryInstancesDataKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR);
    scene_geometry.geometry.instances.data.deviceAddress = instance_input.address;
    auto tlas = fixture.acceleration(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, scene_geometry, count);
    std::vector<uint8_t> tlas_bytes;
    require(metadata_pack_tlas(metadata_instances.data(), count, 0, 100, &tlas_bytes) == MetadataResult::Success,
            "TLAS metadata pack");
    auto tlas_body = fixture.buffer(tlas_bytes.size() + 64, bda_usage, true);
    std::memcpy(tlas_body.mapped, tlas_bytes.data(), tlas_bytes.size());
    root[count] = {kMetadataRootMagic, kMetadataVersion,       kMetadataTlas, kMetadataReady,
                   tlas_body.address,  tlas_bytes.size() + 64, 100,           {0, 0, 0}};
    command = fixture.begin();
    Fixture::barrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
                     VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    for (const auto& structure : native) {
        const auto* range = &structure.range;
        fixture.buildAS(command, 1, &structure.build, &range);
    }
    Fixture::barrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                     VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                     VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    const auto* scene_range = &tlas.range;
    fixture.buildAS(command, 1, &tlas.build, &scene_range);
    Fixture::barrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                     VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    fixture.submit(command);

    auto output = fixture.buffer(sizeof(QueryOutput), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    auto descriptor_info = init<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    descriptor_info.bindingCount = 2;
    descriptor_info.pBindings = bindings;
    VkDescriptorSetLayout descriptor_layout;
    CHECK(vkCreateDescriptorSetLayout(fixture.device, &descriptor_info, nullptr, &descriptor_layout));
    auto layout_info = init<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &descriptor_layout;
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(fixture.device, &layout_info, nullptr, &layout));
    const VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
                                               {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    auto pool_info = init<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(fixture.device, &pool_info, nullptr, &pool));
    auto allocate = init<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    allocate.descriptorPool = pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &descriptor_layout;
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(fixture.device, &allocate, &set));
    auto as_write =
        init<VkWriteDescriptorSetAccelerationStructureKHR>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR);
    as_write.accelerationStructureCount = 1;
    as_write.pAccelerationStructures = &tlas.handle;
    VkDescriptorBufferInfo buffer_info{output.handle, 0, sizeof(QueryOutput)};
    VkWriteDescriptorSet writes[] = {init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET),
                                     init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET)};
    writes[0].pNext = &as_write;
    writes[0].dstSet = set;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(fixture.device, 2, writes, 0, nullptr);
    auto rewrite = [&](uint64_t context) {
        QueryConsumerOptions options;
        options.classify_function = composed.classifier_function;
        options.use_constant_metadata = true;
        options.constant_metadata_low = static_cast<uint32_t>(context);
        options.constant_metadata_high = static_cast<uint32_t>(context >> 32);
        QueryStateModule transformed;
        require(rewrite_query_consumers(composed.words, options, &transformed, &diagnostic) == QueryRewriteStatus::Success,
                "automatic production query consumer: " + diagnostic);
        return make_pipeline(fixture, layout, transformed.words);
    };
    const auto query_pipeline = rewrite(scene_address);
    const auto invalid_pipeline = rewrite(0);
    uint32_t dispatches = 0;
    uint64_t generation = 100;
    auto exercise = [&](uint32_t mode, uint32_t micro, uint32_t flags, uint32_t instance_flags, bool invalid_context = false) {
        const auto point = centroid(micro);
        uint32_t selected = mode & 1 ? 2 : (mode & 8 ? 4 + ((mode >> 4) & 3) : (mode & 4 ? 3 : 0));
        const State state = selected == 3   ? static_cast<State>((0xa5u >> (micro % 8)) & 1u)
                            : selected >= 4 ? static_cast<State>(selected - 4)
                                            : static_cast<State>(micro % 4);
        const bool associated = selected == 0 || selected >= 3;
        Decision decision;
        require(resolve_opacity({associated, state, flags, instance_flags, 1}, &decision) == Status::Success,
                "CPU semantic oracle");
        if (!(mode & (1u | 2u | 4u | 8u)) && decision == Decision::Ignore) {
            selected = 1;
            require(resolve_opacity({false, State::Opaque, flags, instance_flags, 1}, &decision) == Status::Success,
                    "ordinary far oracle");
        }
        if (invalid_context) decision = Decision::Ignore;
        const uint32_t hit = decision == Decision::Ignore ? 0 : (selected == 2 ? 2 : 1);
        const uint32_t visible = decision == Decision::NonOpaque || (selected == 2 && hit) ? 1 : 0;
        auto* header = static_cast<MetadataBodyHeader*>(tlas_body.mapped);
        auto* instances_meta =
            reinterpret_cast<MetadataInstance*>(static_cast<uint8_t*>(tlas_body.mapped) + header->records_offset);
        for (uint32_t i = 0; i < count; ++i) {
            instances_meta[i].original_flags = instance_flags;
            instance_values[i].flags = static_cast<VkGeometryInstanceFlagsKHR>(instance_flags & ~0x30u);
        }
        header->generation = ++generation;
        root[count].generation = generation;
        for (uint32_t index = 0; index < 2; ++index) {
            QueryOutput initial{};
            initial.flags = flags;
            initial.index = index;
            initial.mode = mode;
            initial.u = point[0];
            initial.v = point[1];
            std::memcpy(output.mapped, &initial, sizeof(initial));
            command = fixture.begin();
            Fixture::barrier(
                command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
                VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            fixture.buildAS(command, 1, &tlas.build, &scene_range);
            Fixture::barrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, invalid_context ? invalid_pipeline : query_pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            vkCmdDispatch(command, 1, 1, 1);
            Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             VK_ACCESS_HOST_READ_BIT);
            fixture.submit(command);
            const auto result = *static_cast<const QueryOutput*>(output.mapped);
            const uint32_t aabb = selected == 2 && hit ? uint32_t(decision == Decision::Opaque) : 9;
            const float expected_t = hit ? (selected == 1 ? 2.f : 1.f) : -1.f;
            if (result.committed != hit || result.visible != visible || result.instance != (hit ? selected : UINT32_MAX) ||
                result.logical_flags != flags || result.candidate_flags != (visible ? flags : UINT32_MAX) ||
                result.aabb_opaque != aabb || std::fabs(result.t - expected_t) > 0.00001f) {
                std::fprintf(stderr,
                             "mode=%u micro=%u flags=%u instanceFlags=%u index=%u: "
                             "hit/id/visible/flags/candidate/aabb/t=%u/%u/%u/%u/%u/%u/%.5f\n",
                             mode, micro, flags, instance_flags, index, result.committed, result.instance, result.visible,
                             result.logical_flags, result.candidate_flags, result.aabb_opaque, result.t);
                require(false, "production packed classifier native result");
            }
            ++dispatches;
        }
    };
    const uint32_t ray_flags[] = {0, 1, 2, 64, 128};
    const uint32_t instance_flags[] = {0, 4, 8, 16, 20, 24, 32, 36, 40, 48};
    for (uint32_t micro = 0; micro < 16; ++micro)
        for (auto ray : ray_flags)
            for (uint32_t force_two = 0; force_two < 2; ++force_two)
                for (auto instance : instance_flags) exercise(2, micro, ray | (force_two ? 1024u : 0u), instance);
    for (uint32_t micro = 0; micro < 16; ++micro)
        for (auto ray : ray_flags)
            for (uint32_t instance : {0u, 16u, 32u}) exercise(6, micro, ray, instance);
    for (uint32_t special = 0; special < 4; ++special)
        for (auto ray : ray_flags)
            for (uint32_t force_two = 0; force_two < 2; ++force_two)
                for (auto instance : instance_flags) exercise(10u | (special << 4), 0, ray | (force_two ? 1024u : 0u), instance);
    for (auto ray : ray_flags) {
        exercise(1, 0, ray, 0);
        exercise(0, 0, ray, 0);
    }
    exercise(2, 1, 4, 0);
    exercise(2, 2, 4, 0);
    exercise(2, 1, 0, 0, true);
    vkDestroyPipeline(fixture.device, invalid_pipeline, nullptr);
    vkDestroyPipeline(fixture.device, query_pipeline, nullptr);
    vkDestroyPipeline(fixture.device, metadata_pipeline, nullptr);
    vkDestroyPipeline(fixture.device, build_pipeline, nullptr);
    vkDestroyPipelineLayout(fixture.device, private_layout, nullptr);
    vkDestroyDescriptorPool(fixture.device, pool, nullptr);
    vkDestroyPipelineLayout(fixture.device, layout, nullptr);
    vkDestroyDescriptorSetLayout(fixture.device, descriptor_layout, nullptr);
    std::printf(
        "Production packed classifier: %u native traces PASS (GPU build/snapshot, dead inputs, 2/4state, specials, ordinary/AABB, "
        "arrays/flags)\n",
        dispatches);
    return 0;
}
