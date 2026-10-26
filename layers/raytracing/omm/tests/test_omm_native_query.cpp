// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "native_fixture.h"
#include "../omm_opacity.h"
#include "../omm_storage.h"

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    Fixture fixture;
    if (!fixture.open()) {
        std::puts("SKIP: native AS/RQ/BDA/shaderInt64 prerequisites unavailable");
        return 77;
    }
    const auto inputUsage =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    auto vertices = fixture.buffer(9 * sizeof(float), inputUsage, true);
    const float triangle[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::memcpy(vertices.mapped, triangle, sizeof(triangle));
    VkAccelerationStructureGeometryKHR geometries[4]{};
    for (uint32_t i = 0; i < 2; ++i) {
        geometries[i] = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
        geometries[i].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometries[i].flags = i == 0 ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
        auto& data = geometries[i].geometry.triangles;
        data = init<VkAccelerationStructureGeometryTrianglesDataKHR>(
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR);
        data.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        data.vertexData.deviceAddress = vertices.address;
        data.vertexStride = 12;
        data.maxVertex = 2;
        data.indexType = VK_INDEX_TYPE_NONE_KHR;
    }
    auto aabb = fixture.buffer(sizeof(VkAabbPositionsKHR), inputUsage, true);
    *static_cast<VkAabbPositionsKHR*>(aabb.mapped) = {3, 0, 0, 4, 1, 0.5};
    geometries[2] = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
    geometries[2].geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
    geometries[2].flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometries[2].geometry.aabbs =
        init<VkAccelerationStructureGeometryAabbsDataKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR);
    geometries[2].geometry.aabbs.data.deviceAddress = aabb.address;
    geometries[2].geometry.aabbs.stride = sizeof(VkAabbPositionsKHR);
    Acceleration blas[3];
    for (uint32_t i = 0; i < 3; ++i) blas[i] = fixture.acceleration(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geometries[i]);
    auto instances = fixture.buffer(3 * sizeof(VkAccelerationStructureInstanceKHR), inputUsage, true);
    auto instanceData = static_cast<VkAccelerationStructureInstanceKHR*>(instances.mapped);
    for (uint32_t i = 0; i < 3; ++i) {
        instanceData[i] = {};
        for (uint32_t axis = 0; axis < 3; ++axis) instanceData[i].transform.matrix[axis][axis] = 1;
        instanceData[i].transform.matrix[2][3] = i == 1 ? 1.f : 0.f;
        instanceData[i].mask = 255;
        instanceData[i].instanceCustomIndex = 77;  // Deliberately not a unique metadata key.
        instanceData[i].accelerationStructureReference = blas[i].address;
    }
    geometries[3] = init<VkAccelerationStructureGeometryKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR);
    geometries[3].geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometries[3].geometry.instances =
        init<VkAccelerationStructureGeometryInstancesDataKHR>(VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR);
    geometries[3].geometry.instances.data.deviceAddress = instances.address;
    auto tlas = fixture.acceleration(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, geometries[3], 3);
    auto command = fixture.begin();
    Fixture::barrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
                     VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    for (auto& structure : blas) {
        const auto* range = &structure.range;
        fixture.buildAS(command, 1, &structure.build, &range);
    }
    Fixture::barrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                     VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                     VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    const auto* range = &tlas.range;
    fixture.buildAS(command, 1, &tlas.build, &range);
    Fixture::barrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                     VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    fixture.submit(command);

    auto metadata = fixture.buffer(48, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    const uint32_t records[] = {1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0};
    std::memcpy(metadata.mapped, records, sizeof(records));
    struct Result {
        uint32_t flags, nearState, farState, mode, hit, instanceId, visible, automatic;
        float hitT;
        uint32_t opaqueAabb, nativeFlags, customIndex;
        float u, v;
        uint32_t lookupStatus, microtriangle;
    };
    static_assert(sizeof(Result) == 64);
    auto output = fixture.buffer(sizeof(Result), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    uint8_t triangleRecords[] = {0, 0, 0, 0, 1, 0, 2, 0, 1, 0, 0, 0, 1, 0, 2, 0};
    uint8_t packedStates[] = {0xe4, 0x55};
    vel::omm::StorageBuildInput buildInput{triangleRecords, sizeof(triangleRecords), 2, 8, packedStates, sizeof(packedStates)};
    std::vector<uint8_t> blob;
    if (vel::omm::storage_build(buildInput, &blob) != vel::omm::StorageResult::Success) return 1;
    auto blobBuffer =
        fixture.buffer(blob.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    std::memcpy(blobBuffer.mapped, blob.data(), blob.size());
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    auto descriptorInfo = init<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    descriptorInfo.bindingCount = 2;
    descriptorInfo.pBindings = bindings;
    VkDescriptorSetLayout descriptorLayout;
    CHECK(vkCreateDescriptorSetLayout(fixture.device, &descriptorInfo, nullptr, &descriptorLayout));
    auto pipelineLayoutInfo = init<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorLayout;
    VkPipelineLayout pipelineLayout;
    CHECK(vkCreatePipelineLayout(fixture.device, &pipelineLayoutInfo, nullptr, &pipelineLayout));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    auto poolInfo = init<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    VkDescriptorPool descriptorPool;
    CHECK(vkCreateDescriptorPool(fixture.device, &poolInfo, nullptr, &descriptorPool));
    auto allocateSet = init<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    allocateSet.descriptorPool = descriptorPool;
    allocateSet.descriptorSetCount = 1;
    allocateSet.pSetLayouts = &descriptorLayout;
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(fixture.device, &allocateSet, &set));
    auto asWrite =
        init<VkWriteDescriptorSetAccelerationStructureKHR>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR);
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &tlas.handle;
    VkDescriptorBufferInfo outputInfo = {output.handle, 0, sizeof(Result)};
    VkWriteDescriptorSet writes[2] = {init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET),
                                      init<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET)};
    for (auto& write : writes) {
        write.dstSet = set;
        write.descriptorCount = 1;
    }
    writes[0].pNext = &asWrite;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[1].dstBinding = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &outputInfo;
    vkUpdateDescriptorSets(fixture.device, 2, writes, 0, nullptr);
    std::ifstream source(argv[1], std::ios::binary | std::ios::ate);
    if (!source) return 1;
    const auto byteCount = source.tellg();
    source.seekg(0);
    if (byteCount <= 0 || byteCount % 4) return 1;
    std::vector<uint32_t> words(static_cast<size_t>(byteCount) / 4);
    if (!source.read(reinterpret_cast<char*>(words.data()), byteCount)) return 1;
    auto moduleInfo = init<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    moduleInfo.codeSize = byteCount;
    moduleInfo.pCode = words.data();
    VkShaderModule module;
    CHECK(vkCreateShaderModule(fixture.device, &moduleInfo, nullptr, &module));
    const uint64_t addresses[] = {metadata.address, tlas.address, blobBuffer.address, blob.size()};
    VkSpecializationMapEntry entries[] = {{0, 0, 8}, {1, 8, 8}, {2, 16, 8}, {3, 24, 8}};
    VkSpecializationInfo specialization = {4, entries, sizeof(addresses), addresses};
    auto pipelineInfo = init<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    pipelineInfo.stage = init<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.stage.pSpecializationInfo = &specialization;
    pipelineInfo.layout = pipelineLayout;
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(fixture.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
    struct Case {
        const char* name;
        uint32_t flags, nearState, mode, hit, instance, visible, aabb;
        float t;
    };
    const Case cases[] = {
        {"transparent skips; opaque auto confirms", 0, 0, 0, 1, 1, 0, 0, 2},
        {"force opaque cannot revive transparent", 1, 0, 0, 1, 1, 0, 0, 2},
        {"opaque culling after OMM", 64, 0, 0, 0, UINT32_MAX, 0, 0, -1},
        {"nonopaque culling after OMM", 128, 0, 0, 1, 1, 0, 0, 2},
        {"unknown transparent stays candidate", 0, 2, 1, 1, 0, 1, 0, 1},
        {"force2 transparent ignores", 1024, 2, 1, 0, UINT32_MAX, 0, 0, -1},
        {"force2 opaque auto confirms", 1024, 3, 1, 1, 0, 0, 0, 1},
        {"ordinary opaque geometry preserved", 0, 4, 1, 1, 0, 0, 0, 1},
        {"ordinary forced nonopaque exposed", 2, 4, 1, 1, 0, 1, 0, 1},
        {"opaque AABB still application candidate", 0, 0, 2, 2, 2, 1, 1, 1},
        {"AABB forced nonopaque getter", 2, 0, 2, 2, 2, 1, 0, 1},
        {"opaque AABB culling", 64, 0, 2, 0, UINT32_MAX, 0, 0, -1},
        {"first hit through autoconfirm", 4, 0, 0, 1, 1, 0, 0, 2},
    };
    unsigned failures = 0;
    for (uint32_t integerSource = 0; integerSource < 2; ++integerSource) {
        for (const auto& test : cases) {
            Result initial{};
            initial.flags = test.flags;
            initial.nearState = test.nearState;
            initial.farState = 1;
            initial.mode = test.mode | (integerSource ? 16u : 0u);
            std::memcpy(output.mapped, &initial, sizeof(initial));
            command = fixture.begin();
            Fixture::barrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
            vkCmdDispatch(command, 1, 1, 1);
            Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             VK_ACCESS_HOST_READ_BIT);
            fixture.submit(command);
            Result result = *static_cast<Result*>(output.mapped);
            const auto nativeFlags = (test.flags & ~(1u | 2u | 64u | 128u | 1024u)) | 2u;
            const uint32_t expectedAutomatic = test.hit == 1 && !test.visible ? 1u : 0u;
            bool pass = result.hit == test.hit && result.instanceId == test.instance && result.visible == test.visible &&
                        result.automatic == expectedAutomatic && result.opaqueAabb == test.aabb &&
                        result.nativeFlags == nativeFlags && std::fabs(result.hitT - test.t) < 0.00001f &&
                        (!test.hit || result.customIndex == 77);
            if (!pass) ++failures;
            std::printf("%s: %s AS %s (hit %u instance %u visible %u automatic %u AABBopaque %u t %.2f)\n", pass ? "PASS" : "FAIL",
                        integerSource ? "integer" : "descriptor", test.name, result.hit, result.instanceId, result.visible,
                        result.automatic, result.opaqueAabb, result.hitT);
        }
    }
    // Differentially compare the reusable CPU/GPU core through real native
    // candidate delivery. tmax excludes the farther triangle so traversal
    // ordering cannot change candidate/auto-confirm counts in this matrix.
    const uint32_t rayFlags[] = {0, 1, 2, 64, 128};
    const uint32_t instanceFlags[] = {0, 4, 8, 16, 20, 24, 32, 36, 40, 48};
    unsigned comparisons = 0;
    for (uint32_t sourceKind = 0; sourceKind < 2; ++sourceKind)
        for (uint32_t state = 0; state <= 4; ++state)
            for (auto ray : rayFlags)
                for (uint32_t force2 = 0; force2 < 2; ++force2)
                    for (auto instance : instanceFlags) {
                        Result initial{};
                        initial.flags = ray | (force2 ? 1024u : 0u);
                        initial.nearState = state;
                        initial.farState = 1;
                        initial.mode = 1 | (sourceKind ? 16u : 0u);
                        static_cast<uint32_t*>(metadata.mapped)[1] = instance;
                        std::memcpy(output.mapped, &initial, sizeof(initial));
                        vel::omm::OpacityInput input;
                        input.has_micromap = state != 4;
                        input.state = static_cast<vel::omm::State>(state);
                        input.ray_flags = initial.flags;
                        input.instance_flags = instance;
                        input.geometry_flags = 1;
                        vel::omm::Decision decision;
                        if (vel::omm::resolve_opacity(input, &decision) != vel::omm::Status::Success) return 1;
                        command = fixture.begin();
                        Fixture::barrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
                        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
                        vkCmdDispatch(command, 1, 1, 1);
                        Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                         VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
                        fixture.submit(command);
                        const auto result = *static_cast<const Result*>(output.mapped);
                        bool ignore = decision == vel::omm::Decision::Ignore;
                        bool opaque = decision == vel::omm::Decision::Opaque;
                        bool pass = result.hit == (ignore ? 0u : 1u) && result.instanceId == (ignore ? UINT32_MAX : 0u) &&
                                    result.visible == uint32_t(!ignore && !opaque) && result.automatic == uint32_t(opaque) &&
                                    result.nativeFlags == 2 && result.hitT == (ignore ? -1.f : 1.f);
                        ++comparisons;
                        if (!pass) {
                            ++failures;
                            std::fprintf(stderr, "CPU/GPU mismatch source%u state%u ray%u instance%u decision%u\n", sourceKind,
                                         state, initial.flags, instance, unsigned(decision));
                        }
                    }
    std::printf("CPU/GPU opacity with real native candidates: %u comparisons\n", comparisons);
    unsigned packedComparisons = 0;
    static_cast<uint32_t*>(metadata.mapped)[1] = 0;
    const float samples[][2] = {{0.1f, 0.1f}, {0.8f, 0.1f}, {0.1f, 0.8f}, {0.4f, 0.4f}};
    const uint32_t packedFlags[] = {0, 1, 2, 64, 128, 1024};
    for (uint32_t format = 1; format <= 2; ++format) {
        triangleRecords[6] = triangleRecords[14] = uint8_t(format);
        packedStates[0] = format == 1 ? 0x0a : 0xe4;
        packedStates[1] = format == 1 ? 0x0f : 0x55;
        if (vel::omm::storage_build(buildInput, &blob) != vel::omm::StorageResult::Success) return 1;
        std::memcpy(blobBuffer.mapped, blob.data(), blob.size());
        for (uint32_t sourceKind = 0; sourceKind < 2; ++sourceKind)
            for (const auto& sample : samples)
                for (auto ray : packedFlags) {
                    uint32_t index = 0;
                    vel::omm::State state;
                    if (vel::omm::barycentric_index(sample[0], sample[1], 1, &index) != vel::omm::Status::Success ||
                        vel::omm::decode_state(static_cast<vel::omm::Format>(format), 1, packedStates, 1, index, &state) !=
                            vel::omm::Status::Success)
                        return 1;
                    vel::omm::OpacityInput opacity{true, state, ray, 0, 1};
                    vel::omm::Decision decision;
                    if (vel::omm::resolve_opacity(opacity, &decision) != vel::omm::Status::Success) return 1;
                    Result initial{};
                    initial.flags = ray;
                    initial.mode = 33u | (sourceKind ? 16u : 0u);
                    initial.u = sample[0];
                    initial.v = sample[1];
                    initial.lookupStatus = 0xffffffffu;
                    initial.microtriangle = 0xffffffffu;
                    std::memcpy(output.mapped, &initial, sizeof(initial));
                    command = fixture.begin();
                    Fixture::barrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
                    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
                    vkCmdDispatch(command, 1, 1, 1);
                    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                     VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
                    fixture.submit(command);
                    const auto result = *static_cast<const Result*>(output.mapped);
                    bool ignore = decision == vel::omm::Decision::Ignore;
                    bool opaque = decision == vel::omm::Decision::Opaque;
                    bool pass = result.lookupStatus == 0 && result.microtriangle == index && result.hit == (ignore ? 0u : 1u) &&
                                result.visible == uint32_t(!ignore && !opaque) && result.automatic == uint32_t(opaque);
                    ++packedComparisons;
                    if (!pass) {
                        ++failures;
                        std::fprintf(stderr, "Packed/native mismatch format%u source%u cell%u ray%u status%u micro%u\n", format,
                                     sourceKind, index, ray, result.lookupStatus, result.microtriangle);
                    }
                }
    }
    std::printf("Packed micromap lookup at native barycentric intersections: %u comparisons\n", packedComparisons);
    vkDestroyPipeline(fixture.device, pipeline, nullptr);
    vkDestroyShaderModule(fixture.device, module, nullptr);
    vkDestroyDescriptorPool(fixture.device, descriptorPool, nullptr);
    vkDestroyPipelineLayout(fixture.device, pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(fixture.device, descriptorLayout, nullptr);
    std::printf("M0 native RQ manual wrapper: %u failures\n", failures);
    return failures ? 1 : 0;
}
