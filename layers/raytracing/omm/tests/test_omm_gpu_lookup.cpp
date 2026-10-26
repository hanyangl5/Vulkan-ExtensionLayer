// Copyright 2022-2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0

#include "native_fixture.h"
#include "../omm_opacity.h"
#include "../omm_storage.h"

#include <array>
#include <chrono>
#include <limits>

namespace {

using namespace vel::omm;

struct LookupCase {
    uint64_t blob_offset, blob_size, indices_offset, indices_bytes, stride, geometry_triangle;
    float u, v;
    uint32_t width, base_triangle;
};
struct Result {
    uint32_t status, state, microtriangle, reserved;
};
struct LookupPush {
    uint64_t blob_address, indices_address, indices_backing_bytes, cases_address, results_address;
    uint32_t count;
};
struct BuildPush {
    uint64_t triangles_address, data_address, output_address, status_address;
    uint64_t triangles_bytes, data_bytes, output_bytes, count, stride;
    uint64_t triangles_accessible, data_accessible;
    uint64_t phase;
};
static_assert(sizeof(LookupCase) == 64 && sizeof(Result) == 16 && sizeof(LookupPush) == 48 && sizeof(BuildPush) == 96);

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "OMM GPU lookup: %s\n", message);
        std::exit(1);
    }
}

void write32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
void write64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    write32(bytes, offset, static_cast<uint32_t>(value));
    write32(bytes, offset + 4, static_cast<uint32_t>(value >> 32));
}

void release(Fixture& fixture, Buffer buffer) {
    const auto found = std::find_if(fixture.buffers.begin(), fixture.buffers.end(),
                                    [&](const Buffer& candidate) { return candidate.handle == buffer.handle; });
    require(found != fixture.buffers.end(), "release bookkeeping");
    if (buffer.mapped) vkUnmapMemory(fixture.device, buffer.memory);
    vkDestroyBuffer(fixture.device, buffer.handle, nullptr);
    vkFreeMemory(fixture.device, buffer.memory, nullptr);
    fixture.buffers.erase(found);
}

VkPipeline pipeline(Fixture& fixture, VkPipelineLayout layout, const char* path) {
    std::printf("OMM GPU lookup pipeline: %s\n", path);
    std::fflush(stdout);
    std::ifstream source(path, std::ios::binary | std::ios::ate);
    require(bool(source), "shader input unavailable");
    const auto count = source.tellg();
    require(count > 0 && count % 4 == 0, "shader input size");
    std::vector<uint32_t> words(static_cast<size_t>(count) / 4);
    source.seekg(0);
    require(bool(source.read(reinterpret_cast<char*>(words.data()), count)), "shader input read");
    auto moduleInfo = init<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    moduleInfo.codeSize = static_cast<size_t>(count);
    moduleInfo.pCode = words.data();
    VkShaderModule module;
    CHECK(vkCreateShaderModule(fixture.device, &moduleInfo, nullptr, &module));
    auto info = init<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    info.stage = init<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = layout;
    VkPipeline result;
    CHECK(vkCreateComputePipelines(fixture.device, VK_NULL_HANDLE, 1, &info, nullptr, &result));
    vkDestroyShaderModule(fixture.device, module, nullptr);
    return result;
}

// Independent index -> interior point oracle adapted from Apache-2.0 Khronos
// CTS calcSubtriangleCentroid, revision d18e4c68543103371715845cd914c71e5859963e,
// ray_tracing/vktRayTracingOpacityMicromapTests.cpp. The GPU implements the
// opposite direction and never receives the expected microtriangle index.
std::array<float, 2> centroid(uint32_t index, uint32_t level) {
    if (!level) return {1.0f / 3.0f, 1.0f / 3.0f};
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
    const uint32_t mask = (1u << level) - 1;
    const uint32_t iu = ((f & ~t) | (d & ~t) | (~d & ~f & t)) & mask;
    const uint32_t iv = ((f >> 16) ^ d) & mask;
    const uint32_t iw = ((~f & ~t) | (d & ~t) | (~d & f & t)) & mask;
    const bool upright = ((iu ^ iv ^ iw) & 1) != 0;
    const float scale = 1.0f / static_cast<float>(1u << level);
    const float fraction = upright ? 1.0f / 3.0f : 2.0f / 3.0f;
    return {(static_cast<float>(iu) + fraction) * scale, (static_cast<float>(iv) + fraction) * scale};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 1;
    Fixture fixture;
    // This shared native-proof fixture currently requires AS/RQ in addition to
    // BDA and shaderInt64. The lookup itself uses compute and BDA only.
    if (!fixture.open()) {
        std::puts("SKIP: shared native AS/RQ/BDA/shaderInt64 fixture prerequisites unavailable");
        return 77;
    }
    std::vector<uint8_t> triangles(26 * 8), source;
    for (uint32_t level = 0; level <= 12; ++level)
        for (uint32_t format = 1; format <= 2; ++format) {
            const uint32_t triangle = level * 2 + format - 1;
            uint64_t bytes;
            require(storage_data_size(static_cast<StorageFormat>(format), level, &bytes) == StorageResult::Success, "data sizing");
            write32(triangles, triangle * 8, static_cast<uint32_t>(source.size()));
            triangles[triangle * 8 + 4] = static_cast<uint8_t>(level);
            triangles[triangle * 8 + 6] = static_cast<uint8_t>(format);
            const size_t start = source.size();
            source.resize(start + static_cast<size_t>(bytes));
            for (size_t i = 0; i < bytes; ++i)
                source[start + i] = static_cast<uint8_t>((i * 197u + (i >> 3) * 43u + triangle * 71u + 0x39u) & 255u);
        }
    std::vector<uint8_t> reference;
    require(storage_build({triangles.data(), triangles.size(), 26, 8, source.data(), source.size()}, &reference) ==
                StorageResult::Success,
            "CPU reference build");
    std::vector<StoredTriangle> records(26);
    for (uint32_t i = 0; i < records.size(); ++i)
        require(storage_triangle(reference.data(), reference.size(), i, &records[i]) == StorageResult::Success,
                "reference triangle");
    std::vector<LookupCase> cases;
    std::vector<Result> expected;
    auto normal = [&](uint32_t triangle, float u, float v, uint32_t knownIndex) {
        const auto& record = records[triangle];
        uint32_t cpuIndex = 0;
        require(barycentric_index(u, v, record.subdivision_level, &cpuIndex) == Status::Success && cpuIndex == knownIndex,
                "CPU mapper versus independent CTS inverse");
        State state;
        require(decode_state(static_cast<Format>(record.format), record.subdivision_level,
                             reference.data() + static_cast<size_t>(record.data_offset), static_cast<size_t>(record.data_size),
                             knownIndex, &state) == Status::Success,
                "CPU packed decode");
        cases.push_back({0, reference.size(), 0, 0, 0, triangle, u, v, 0, 0});
        expected.push_back({0, static_cast<uint32_t>(state), knownIndex, 0});
    };
    uint32_t random = 0xface1234u;
    for (uint32_t level = 0; level <= 12; ++level) {
        const uint32_t count = 1u << (2 * level);
        const uint32_t samples = level <= 9 ? count : 8192;
        for (uint32_t i = 0; i < samples; ++i) {
            random = random * 1664525u + 1013904223u;
            const uint32_t index = level <= 9 ? i : random & (count - 1);
            const auto point = centroid(index, level);
            for (uint32_t format = 1; format <= 2; ++format) normal(level * 2 + format - 1, point[0], point[1], index);
        }
        constexpr float boundary[][2] = {{0, 0}, {1, 0}, {0, 1}, {0.5f, 0.5f}, {0.25f, 0.25f}, {-1, 2}};
        for (const auto& point : boundary) {
            uint32_t index = 0;
            require(barycentric_index(point[0], point[1], level, &index) == Status::Success, "boundary oracle");
            for (uint32_t format = 1; format <= 2; ++format) normal(level * 2 + format - 1, point[0], point[1], index);
        }
    }
    const size_t interiorCases = cases.size();

    std::vector<uint8_t> indices(256, 0xa5);
    auto associationCase = [&](uint32_t width, uint32_t raw, uint32_t base, uint64_t geometry = 0, uint64_t stride = 0) {
        const uint64_t offset = width == 16 ? 2 : 4;
        // Each case needs an immutable independent slot in the shared index buffer.
        const size_t slot = indices.size();
        indices.resize(slot + 8, 0xa5);
        for (uint32_t i = 0; i < width / 8; ++i) indices[slot + (offset & 3u) + i] = static_cast<uint8_t>(raw >> (8 * i));
        TriangleIndex resolved;
        require(decode_index(raw, width, base, &resolved) == Status::Success, "index oracle");
        const auto point = centroid(0, records[5].subdivision_level);
        LookupCase test{0, reference.size(), slot + (offset & 3u), width / 8, stride, geometry, point[0], point[1], width, base};
        Result result{0, 0, 0, 0};
        if (resolved.special) {
            test.blob_offset = UINT64_MAX;  // Converted to an actual null BDA below.
            test.blob_size = 0;
            result.state = static_cast<uint32_t>(resolved.state);
        } else if (resolved.triangle >= records.size()) {
            result = {2, 0xeeeeeeeeu, 0xddddddddu, 0};
        } else {
            const auto& record = records[static_cast<size_t>(resolved.triangle)];
            uint32_t micro = 0;
            require(barycentric_index(test.u, test.v, record.subdivision_level, &micro) == Status::Success,
                    "association bary oracle");
            State state;
            require(decode_state(static_cast<Format>(record.format), record.subdivision_level,
                                 reference.data() + static_cast<size_t>(record.data_offset), static_cast<size_t>(record.data_size),
                                 micro, &state) == Status::Success,
                    "association state oracle");
            result = {0, static_cast<uint32_t>(state), micro, 0};
        }
        cases.push_back(test);
        expected.push_back(result);
    };
    for (uint32_t width : {8u, 16u, 32u}) {
        const uint32_t maximum = width == 32 ? UINT32_MAX : (1u << width) - 1;
        for (uint32_t special = 0; special < 4; ++special) associationCase(width, maximum - special, UINT32_MAX);
        associationCase(width, 2, 3);
        associationCase(width, 1u << (width - 1), UINT32_MAX);
    }
    associationCase(16, 0xffffu, 0, UINT64_MAX, 0);  // Zero stride legitimately repeats the special index.
    for (uint32_t special = 0; special < 4; ++special) {
        cases.push_back({UINT64_MAX, 0, 0, 0, 0, UINT32_MAX - special, 0, 0, 0, UINT32_MAX});
        expected.push_back({0, special, 0, 0});
    }

    auto invalid = [&](LookupCase test, uint32_t status) {
        cases.push_back(test);
        expected.push_back({status, 0xeeeeeeeeu, 0xddddddddu, 0});
    };
    LookupCase valid{0, reference.size(), 0, 0, 0, 0, 0.2f, 0.2f, 0, 0};
    auto test = valid;
    test.geometry_triangle = 26;
    invalid(test, 2);
    test.geometry_triangle = uint64_t{1} << 32;
    invalid(test, 2);
    test = valid;
    test.blob_offset = UINT64_MAX;
    test.blob_size = 0;
    invalid(test, 1);
    test = valid;
    test.blob_offset = 1;
    invalid(test, 1);
    test = valid;
    test.blob_size = 32;
    invalid(test, 2);
    test = valid;
    test.u = std::numeric_limits<float>::quiet_NaN();
    invalid(test, 1);
    test = valid;
    test.v = std::numeric_limits<float>::infinity();
    invalid(test, 1);
    test = valid;
    test.width = 12;
    invalid(test, 1);
    test.width = 16;
    test.stride = 3;
    invalid(test, 1);
    test.stride = 8;
    test.geometry_triangle = UINT64_MAX;
    invalid(test, 3);
    test.geometry_triangle = 0;
    test.indices_bytes = 1;
    invalid(test, 2);
    test.indices_bytes = 8;
    test.indices_offset = indices.size();
    invalid(test, 2);

    std::vector<uint8_t> tiny;
    require(storage_build({triangles.data(), 8, 1, 8, source.data(), source.size()}, &tiny) == StorageResult::Success,
            "tiny reference");
    std::vector<uint8_t> extraBlobs;
    auto corrupted = [&](size_t offset, uint64_t value, bool wide, uint32_t status) {
        auto blob = tiny;
        if (wide)
            write64(blob, offset, value);
        else
            write32(blob, offset, static_cast<uint32_t>(value));
        auto corruptCase = valid;
        corruptCase.blob_offset = reference.size() + extraBlobs.size();
        corruptCase.blob_size = blob.size();
        invalid(corruptCase, status);
        extraBlobs.insert(extraBlobs.end(), blob.begin(), blob.end());
    };
    corrupted(0, 0, false, 4);
    corrupted(8, 2, false, 4);
    corrupted(12, 63, false, 1);
    corrupted(16, tiny.size() - 16, true, 1);
    corrupted(24, UINT64_MAX, true, 2);
    corrupted(32, 60, true, 1);
    corrupted(40, 64, true, 1);
    corrupted(48, 1, true, 1);
    corrupted(64, 3, false, 1);
    corrupted(68, 17, false, 1);
    corrupted(72, 81, true, 1);
    corrupted(72, tiny.size(), true, 2);
    corrupted(80, 2, true, 1);

    const auto bdaUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    auto triangleInput = fixture.buffer(triangles.size(), bdaUsage, true);
    auto dataInput = fixture.buffer((source.size() + 3) & ~size_t{3}, bdaUsage, true);
    std::memcpy(triangleInput.mapped, triangles.data(), triangles.size());
    std::memset(dataInput.mapped, 0, (source.size() + 3) & ~size_t{3});
    std::memcpy(dataInput.mapped, source.data(), source.size());
    auto blob = fixture.buffer(reference.size() + extraBlobs.size(), bdaUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    auto status = fixture.buffer(4, bdaUsage, true);
    *static_cast<uint32_t*>(status.mapped) = UINT32_MAX;
    auto extraInput = fixture.buffer(extraBlobs.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(extraInput.mapped, extraBlobs.data(), extraBlobs.size());
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BuildPush)};
    auto layoutInfo = init<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(fixture.device, &layoutInfo, nullptr, &layout));
    const auto buildPipeline = pipeline(fixture, layout, argv[2]);
    const auto lookupPipeline = pipeline(fixture, layout, argv[1]);
    BuildPush build{triangleInput.address,
                    dataInput.address,
                    blob.address,
                    status.address,
                    triangles.size(),
                    source.size(),
                    reference.size(),
                    26,
                    8,
                    triangles.size(),
                    (source.size() + 3) & ~size_t{3},
                    0};
    const auto buildStart = std::chrono::steady_clock::now();
    auto command = fixture.begin();
    Fixture::barrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, buildPipeline);
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(build), &build);
    vkCmdDispatch(command, 1, 1, 1);
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    build.phase = 1;
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(build), &build);
    vkCmdDispatch(command, static_cast<uint32_t>((reference.size() / 4 + 63) / 64), 1, 1);
    const VkBufferCopy copy{0, reference.size(), extraBlobs.size()};
    vkCmdCopyBuffer(command, extraInput.handle, blob.handle, 1, &copy);
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_HOST_READ_BIT);
    fixture.submit(command);
    std::printf("OMM GPU build plus completion wait: %zu bytes in %.3f ms\n", reference.size(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count());
    require(*static_cast<const uint32_t*>(status.mapped) == 0, "GPU canonical build status");
    // Source buffers are actually destroyed after completion, before their
    // address ranges can be reused for cases/results and before any lookup.
    release(fixture, triangleInput);
    release(fixture, dataInput);
    release(fixture, status);
    release(fixture, extraInput);
    for (auto& lookupCase : cases)
        if (lookupCase.blob_offset == UINT64_MAX) lookupCase.blob_offset = uint64_t{0} - blob.address;
    auto indexInput = fixture.buffer(indices.size(), bdaUsage, true);
    auto caseInput = fixture.buffer(cases.size() * sizeof(LookupCase), bdaUsage, true);
    auto results = fixture.buffer(cases.size() * sizeof(Result), bdaUsage, true);
    std::memcpy(indexInput.mapped, indices.data(), indices.size());
    std::memcpy(caseInput.mapped, cases.data(), cases.size() * sizeof(LookupCase));
    std::memset(results.mapped, 0xcc, cases.size() * sizeof(Result));
    const LookupPush lookup{blob.address,      indexInput.address, indices.size(),
                            caseInput.address, results.address,    static_cast<uint32_t>(cases.size())};
    command = fixture.begin();
    Fixture::barrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, lookupPipeline);
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(lookup), &lookup);
    vkCmdDispatch(command, (lookup.count + 63) / 64, 1, 1);
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                     VK_ACCESS_HOST_READ_BIT);
    fixture.submit(command);
    const auto* actual = static_cast<const Result*>(results.mapped);
    for (size_t i = 0; i < expected.size(); ++i)
        if (actual[i].status != expected[i].status || actual[i].state != expected[i].state ||
            actual[i].microtriangle != expected[i].microtriangle) {
            std::fprintf(stderr, "lookup %zu: got status/state/index %u/%u/%u expected %u/%u/%u\n", i, actual[i].status,
                         actual[i].state, actual[i].microtriangle, expected[i].status, expected[i].state,
                         expected[i].microtriangle);
            return 1;
        }
    std::printf(
        "OMM GPU lookup: %zu cases PASS (%zu interiors/boundaries, levels0..12; GPU-built data survives source destruction)\n",
        cases.size(), interiorCases);
    vkDestroyPipeline(fixture.device, lookupPipeline, nullptr);
    vkDestroyPipeline(fixture.device, buildPipeline, nullptr);
    vkDestroyPipelineLayout(fixture.device, layout, nullptr);
    return 0;
}
