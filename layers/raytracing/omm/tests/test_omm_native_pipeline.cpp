// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
// M0 explicit native RTP wrapper experiment, not a layer or advertised extension.
#include "native_fixture.h"
#include <cstddef>
#include <string>

struct OmmRecord {
    uint32_t state, associated, instanceFlags, geometryFlags;
};
struct Context {
    uint64_t scene;
    uint32_t flags, mode, action, applicationAnyHit, reserved0, reserved1;
    OmmRecord records[3];
    uint32_t hit, instanceId, anyHitCalls, intersectionCalls;
    uint32_t anyHitFlags, intersectionFlags, closestHitFlags, nativeFlags;
    uint32_t reportAccepted, payloadMarker, wrapperCalls, missCalls;
    float hitT, attributeU, attributeV;
    uint32_t reserved2;
};
static_assert(offsetof(Context, records) == 32 && offsetof(Context, hit) == 80 && sizeof(Context) == 144);

static VkShaderModule module(VkDevice device, const std::string& filename) {
    std::ifstream source(filename, std::ios::binary | std::ios::ate);
    if (!source) {
        std::fprintf(stderr, "Cannot open shader %s\n", filename.c_str());
        std::exit(1);
    }
    const auto byteCount = source.tellg();
    if (byteCount <= 0 || byteCount % 4) std::exit(1);
    source.seekg(0);
    std::vector<uint32_t> words(static_cast<size_t>(byteCount) / 4);
    if (!source.read(reinterpret_cast<char*>(words.data()), byteCount)) std::exit(1);
    auto info = init<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    info.codeSize = static_cast<size_t>(byteCount);
    info.pCode = words.data();
    VkShaderModule result;
    CHECK(vkCreateShaderModule(device, &info, nullptr, &result));
    return result;
}
static VkDeviceSize align(VkDeviceSize value, VkDeviceSize alignment) { return (value + alignment - 1) & ~(alignment - 1); }
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    Fixture fixture;
    if (!fixture.open(true)) {
        std::puts("SKIP: native AS/RTP/BDA/shaderInt64 prerequisites unavailable");
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
        geometries[i].flags = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR | (i == 0 ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0);
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
        instanceData[i].instanceShaderBindingTableRecordOffset = i == 2 ? 1 : 0;
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
                     VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                     VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    fixture.submit(command);

    auto contextBuffer =
        fixture.buffer(sizeof(Context), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    auto* context = static_cast<Context*>(contextBuffer.mapped);
    VkSpecializationMapEntry addressEntry = {0, 0, sizeof(VkDeviceAddress)};
    VkSpecializationInfo specialization = {1, &addressEntry, sizeof(contextBuffer.address), &contextBuffer.address};
    const char* suffixes[] = {"rgen", "rmiss", "rahit", "rchit", "rint"};
    const VkShaderStageFlagBits kinds[] = {VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
                                           VK_SHADER_STAGE_ANY_HIT_BIT_KHR, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
                                           VK_SHADER_STAGE_INTERSECTION_BIT_KHR};
    VkShaderModule modules[5];
    VkPipelineShaderStageCreateInfo stages[5];
    for (unsigned i = 0; i < 5; ++i) {
        modules[i] = module(fixture.device, std::string(argv[1]) + "/native_pipeline." + suffixes[i] + ".spv");
        stages[i] = init<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        stages[i].stage = kinds[i];
        stages[i].module = modules[i];
        stages[i].pName = "main";
        stages[i].pSpecializationInfo = &specialization;
    }
    VkRayTracingShaderGroupCreateInfoKHR groups[5];
    for (auto& group : groups) {
        group = init<VkRayTracingShaderGroupCreateInfoKHR>(VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR);
        group.generalShader = group.closestHitShader = group.anyHitShader = group.intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    groups[0].type = groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;
    groups[1].generalShader = 1;
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].closestHitShader = 3;
    groups[2].anyHitShader = 2;
    groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR;
    groups[3].closestHitShader = 3;
    groups[3].anyHitShader = 2;
    groups[3].intersectionShader = 4;
    // Replacement for a logical zero triangle hit group: filtering only, with no application shaders.
    groups[4].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[4].anyHitShader = 2;
    auto layoutInfo = init<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(fixture.device, &layoutInfo, nullptr, &layout));
    auto pipelineInfo = init<VkRayTracingPipelineCreateInfoKHR>(VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR);
    pipelineInfo.stageCount = 5;
    pipelineInfo.pStages = stages;
    pipelineInfo.groupCount = 5;
    pipelineInfo.pGroups = groups;
    pipelineInfo.maxPipelineRayRecursionDepth = 1;
    pipelineInfo.layout = layout;
    auto createPipeline =
        reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(vkGetDeviceProcAddr(fixture.device, "vkCreateRayTracingPipelinesKHR"));
    auto getHandles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
        vkGetDeviceProcAddr(fixture.device, "vkGetRayTracingShaderGroupHandlesKHR"));
    auto trace = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(vkGetDeviceProcAddr(fixture.device, "vkCmdTraceRaysKHR"));
    if (!createPipeline || !getHandles || !trace) return 1;
    VkPipeline pipeline;
    CHECK(createPipeline(fixture.device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
    auto rtProperties = init<VkPhysicalDeviceRayTracingPipelinePropertiesKHR>(
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR);
    auto properties = init<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
    properties.pNext = &rtProperties;
    vkGetPhysicalDeviceProperties2(fixture.physical, &properties);
    const auto handleSize = rtProperties.shaderGroupHandleSize;
    const auto stride = align(handleSize, rtProperties.shaderGroupHandleAlignment);
    std::vector<uint8_t> handles(5 * handleSize);
    CHECK(getHandles(fixture.device, pipeline, 0, 5, handles.size(), handles.data()));
    auto region = [&](unsigned first, unsigned count) {
        const auto size = stride * count;
        auto data = fixture.buffer(size + rtProperties.shaderGroupBaseAlignment - 1,
                                   VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
        const auto address = align(data.address, rtProperties.shaderGroupBaseAlignment);
        auto* bytes = static_cast<uint8_t*>(data.mapped) + (address - data.address);
        std::memset(bytes, 0, static_cast<size_t>(size));
        for (unsigned i = 0; i < count; ++i) std::memcpy(bytes + stride * i, handles.data() + handleSize * (first + i), handleSize);
        return VkStridedDeviceAddressRegionKHR{address, stride, size};
    };
    auto raygen = region(0, 1), miss = region(1, 1);
    auto hitBuffer = fixture.buffer(stride * 4 + rtProperties.shaderGroupBaseAlignment - 1,
                                    VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
    auto hitAddress = align(hitBuffer.address, rtProperties.shaderGroupBaseAlignment);
    auto* hitBytes = static_cast<uint8_t*>(hitBuffer.mapped) + (hitAddress - hitBuffer.address);
    std::memset(hitBytes, 0, static_cast<size_t>(stride * 4));
    std::memcpy(hitBytes, handles.data() + handleSize * 2, handleSize);
    std::memcpy(hitBytes + stride, handles.data() + handleSize * 3, handleSize);
    // Record2 is the Vulkan-defined all-zero shader group. Record3 injects only the wrapper.
    std::memcpy(hitBytes + stride * 3, handles.data() + handleSize * 4, handleSize);
    VkStridedDeviceAddressRegionKHR hit{hitAddress, stride, stride * 4};
    const VkStridedDeviceAddressRegionKHR callable{};
    struct Case {
        const char* name;
        uint32_t mode, state, associated, geometryFlags, instanceFlags, rayFlags, appAnyHit, action;
        uint32_t hit, instance, anyHits, intersections, accepted;
        float t;
    };
    const Case cases[] = {
        {"transparent skips application AH", 1, 0, 1, 1, 0, 0, 1, 0, 0, UINT32_MAX, 0, 0, 0, -1},
        {"opaque bypasses application AH", 1, 1, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1},
        {"unknown transparent invokes AH", 1, 2, 1, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1},
        {"unknown opaque invokes AH", 1, 3, 1, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1},
        {"force opaque cannot revive transparent", 1, 0, 1, 1, 0, 1, 1, 0, 0, UINT32_MAX, 0, 0, 0, -1},
        {"force opaque unknown bypasses AH", 1, 2, 1, 1, 0, 1, 1, 0, 1, 0, 0, 0, 0, 1},
        {"force nonopaque opaque invokes AH", 1, 1, 1, 1, 0, 2, 1, 0, 1, 0, 1, 0, 0, 1},
        {"force2 unknown transparent ignores", 1, 2, 1, 1, 0, 1024, 1, 0, 0, UINT32_MAX, 0, 0, 0, -1},
        {"force2 unknown opaque bypasses AH", 1, 3, 1, 1, 0, 1024, 1, 0, 1, 0, 0, 0, 0, 1},
        {"instance opaque unknown bypasses AH", 1, 2, 1, 1, 4, 0, 1, 0, 1, 0, 0, 0, 0, 1},
        {"ray noopaque overrides instance opaque", 1, 2, 1, 1, 4, 2, 1, 0, 1, 0, 1, 0, 0, 1},
        {"disable OMM preserves geometry opacity", 1, 0, 1, 1, 32, 0, 1, 0, 1, 0, 0, 0, 0, 1},
        {"ordinary opaque geometry", 1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1},
        {"ordinary nonopaque geometry", 1, 1, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1},
        {"opaque culling after OMM", 1, 1, 1, 1, 0, 64, 1, 0, 0, UINT32_MAX, 0, 0, 0, -1},
        {"nonopaque culling after OMM", 1, 2, 1, 1, 0, 128, 1, 0, 0, UINT32_MAX, 0, 0, 0, -1},
        {"absent application AH gets injected wrapper", 1, 2, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1},
        {"application IgnoreIntersection keeps side effects", 1, 2, 1, 1, 0, 0, 1, 1, 0, UINT32_MAX, 1, 0, 0, -1},
        {"application TerminateRay accepts and keeps payload", 1, 2, 1, 1, 0, 0, 1, 2, 1, 0, 1, 0, 0, 1},
        {"first hit preserves logical original flags", 1, 2, 1, 1, 0, 4, 1, 0, 1, 0, 1, 0, 0, 1},
        {"mixed transparent near and opaque far", 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 0, 2},
        {"procedural nonopaque application AH", 2, 0, 0, 0, 0, 0, 1, 0, 1, 2, 1, 1, 1, 1.25f},
        {"procedural opaque bypasses application AH", 2, 0, 0, 1, 0, 0, 1, 0, 1, 2, 0, 1, 1, 1.25f},
        {"procedural cull opaque guards IS side effect", 2, 0, 0, 1, 0, 64, 1, 0, 0, UINT32_MAX, 0, 0, 0, -1},
        {"procedural cull nonopaque guards IS side effect", 2, 0, 0, 0, 0, 128, 1, 0, 0, UINT32_MAX, 0, 0, 0, -1},
        {"procedural forced nonopaque", 2, 0, 0, 1, 0, 2, 1, 0, 1, 2, 1, 1, 1, 1.25f},
        {"procedural forced opaque", 2, 0, 0, 0, 0, 1, 1, 0, 1, 2, 0, 1, 1, 1.25f},
        {"procedural Ignore reports false after AH", 2, 0, 0, 0, 0, 0, 1, 1, 0, UINT32_MAX, 1, 1, 0, -1},
        {"procedural absent application AH", 2, 0, 0, 0, 0, 0, 0, 0, 1, 2, 0, 1, 1, 1.25f},
    };
    unsigned failures = 0;
    for (const auto& test : cases) {
        *context = {};
        context->scene = tlas.address;
        context->flags = test.rayFlags;
        context->mode = test.mode;
        context->action = test.action;
        context->applicationAnyHit = test.appAnyHit;
        context->records[0] = {test.state, test.associated, test.instanceFlags, test.geometryFlags};
        context->records[1] = {1, 1, 0, 0};
        context->records[2] = {test.state, test.associated, test.instanceFlags, test.geometryFlags};
        context->anyHitFlags = context->intersectionFlags = context->closestHitFlags = context->nativeFlags = UINT32_MAX;
        command = fixture.begin();
        Fixture::barrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);
        trace(command, &raygen, &miss, &hit, &callable, 1, 1, 1);
        Fixture::barrier(command, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        fixture.submit(command);
        const auto& result = *context;
        bool pass = result.hit == test.hit && result.instanceId == test.instance && result.anyHitCalls == test.anyHits &&
                    result.intersectionCalls == test.intersections && result.reportAccepted == test.accepted &&
                    result.payloadMarker == test.anyHits * 7 && result.missCalls == (test.hit ? 0u : 1u) &&
                    std::fabs(result.hitT - test.t) < 0.00001f;
        if (test.anyHits) pass &= result.anyHitFlags == test.rayFlags;
        if (test.intersections) pass &= result.intersectionFlags == test.rayFlags;
        if (test.hit) {
            const float expectedU = test.mode == 2 ? 0.2f : 0.25f;
            const float expectedV = test.mode == 2 ? 0.4f : 0.25f;
            pass &= result.closestHitFlags == test.rayFlags && std::fabs(result.attributeU - expectedU) < 0.00001f &&
                    std::fabs(result.attributeV - expectedV) < 0.00001f;
        }
        if (result.wrapperCalls || test.intersections || test.hit)
            pass &= result.nativeFlags == ((test.rayFlags & ~(1u | 2u | 64u | 128u | 1024u)) | 2u);
        if (!pass) ++failures;
        std::printf("%s: %s (hit=%u instance=%u AH=%u IS=%u report=%u marker=%u miss=%u t=%.2f)\n", pass ? "PASS" : "FAIL",
                    test.name, result.hit, result.instanceId, result.anyHitCalls, result.intersectionCalls, result.reportAccepted,
                    result.payloadMarker, result.missCalls, result.hitT);
    }
    // Exercise a legal all-zero triangle hit group and its AH-only replacement.
    // Both accepted variants keep the original payload unchanged and skip Miss;
    // filtering a transparent candidate must instead invoke Miss. No CH exists.
    unsigned zeroCases = 0;
    for (uint32_t mode : {3u, 4u}) {
        for (uint32_t state = 0; state < 4; ++state) {
            *context = {};
            context->scene = tlas.address;
            context->mode = mode;
            context->records[0] = {state, 1, 0, 1};
            command = fixture.begin();
            Fixture::barrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
                             VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline);
            trace(command, &raygen, &miss, &hit, &callable, 1, 1, 1);
            Fixture::barrier(command, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
            fixture.submit(command);
            const auto expectedMiss = mode == 4 && state == 0 ? 1u : 0u;
            const bool pass = context->hit == 0 && context->instanceId == UINT32_MAX && context->payloadMarker == 0 &&
                              context->anyHitCalls == 0 && context->intersectionCalls == 0 && context->missCalls == expectedMiss &&
                              (mode == 3 ? context->wrapperCalls == 0 : context->wrapperCalls != 0);
            ++zeroCases;
            if (!pass) ++failures;
            std::printf("%s: zero triangle group %s state=%u (wrapper=%u miss=%u payload=%u)\n", pass ? "PASS" : "FAIL",
                        mode == 3 ? "native control" : "AH-only replacement", state, context->wrapperCalls, context->missCalls,
                        context->payloadMarker);
        }
    }
    vkDestroyPipeline(fixture.device, pipeline, nullptr);
    vkDestroyPipelineLayout(fixture.device, layout, nullptr);
    for (auto value : modules) vkDestroyShaderModule(fixture.device, value, nullptr);
    std::printf(
        "Native RTP wrapper proof: %zu semantic + %u zero-group cases, %u failures; general instrumentation and nested/callable "
        "context remain open.\n",
        sizeof(cases) / sizeof(cases[0]), zeroCases, failures);
    return failures ? 1 : 0;
}
