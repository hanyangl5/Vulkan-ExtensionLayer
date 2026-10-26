// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#include "native_fixture.h"
#include "../omm_storage.h"
#include <chrono>

using namespace vel::omm;

namespace {

struct BuildPush {
    uint64_t triangles, data, output, status;
    uint64_t triangles_size, data_size, output_size, triangle_count, triangle_stride;
    uint64_t triangles_access_size, data_access_size;
    uint64_t phase;
};
static_assert(sizeof(BuildPush) == 96);

struct GeneratePush {
    uint64_t triangles, data, count, stride, seed, triangles_access_size, data_access_size;
    uint64_t max_level;
};
static_assert(sizeof(GeneratePush) == 64);

struct Program {
    Fixture& fixture;
    VkShaderModule module{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};

    Program(Fixture& owner, const char* path, uint32_t push_size) : fixture(owner) {
        std::ifstream source(path, std::ios::binary | std::ios::ate);
        if (!source || source.tellg() <= 0 || source.tellg() % 4) std::exit(1);
        const size_t bytes = static_cast<size_t>(source.tellg());
        std::vector<uint32_t> words(bytes / 4);
        source.seekg(0);
        if (!source.read(reinterpret_cast<char*>(words.data()), bytes)) std::exit(1);
        auto module_info = init<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        module_info.codeSize = bytes;
        module_info.pCode = words.data();
        CHECK(vkCreateShaderModule(fixture.device, &module_info, nullptr, &module));
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size};
        auto layout_info = init<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        CHECK(vkCreatePipelineLayout(fixture.device, &layout_info, nullptr, &layout));
        auto create = init<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
        create.stage = init<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        create.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        create.stage.module = module;
        create.stage.pName = "main";
        create.layout = layout;
        CHECK(vkCreateComputePipelines(fixture.device, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline));
    }

    void dispatch(VkCommandBuffer command, const void* push, uint32_t size, uint32_t groups = 1) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, size, push);
        vkCmdDispatch(command, groups, 1, 1);
    }

    ~Program() {
        vkDestroyPipeline(fixture.device, pipeline, nullptr);
        vkDestroyPipelineLayout(fixture.device, layout, nullptr);
        vkDestroyShaderModule(fixture.device, module, nullptr);
    }
};

uint64_t align4(uint64_t value) { return (value + 3) & ~uint64_t(3); }

void put32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (unsigned lane = 0; lane < 4; ++lane) bytes[offset + lane] = uint8_t(value >> (lane * 8));
}

void release_buffer(Fixture& fixture, const Buffer& buffer) {
    for (auto i = fixture.buffers.begin(); i != fixture.buffers.end(); ++i) {
        if (i->handle != buffer.handle) continue;
        if (i->mapped) vkUnmapMemory(fixture.device, i->memory);
        vkDestroyBuffer(fixture.device, i->handle, nullptr);
        vkFreeMemory(fixture.device, i->memory, nullptr);
        fixture.buffers.erase(i);
        return;
    }
    std::exit(1);
}

enum class Damage {
    None,
    Format,
    Subdivision,
    DataOffset,
    TriangleBounds,
    DataBounds,
    WordBounds,
    OutputSize,
    CountOverflow,
    Stride
};

const char* damage_name(Damage damage) {
    switch (damage) {
        case Damage::None:
            return "valid";
        case Damage::Format:
            return "invalid format";
        case Damage::Subdivision:
            return "invalid subdivision";
        case Damage::DataOffset:
            return "data offset overflow/bounds";
        case Damage::TriangleBounds:
            return "triangle bounds";
        case Damage::DataBounds:
            return "data bounds";
        case Damage::WordBounds:
            return "partial-word backing";
        case Damage::OutputSize:
            return "usage-size mismatch";
        case Damage::CountOverflow:
            return "count overflow";
        case Damage::Stride:
            return "invalid stride";
    }
    return "unknown";
}

bool run_case(Fixture& fixture, Program& generate, Program& build, uint32_t count, uint32_t stride, uint32_t seed, Damage damage,
              uint32_t max_level = 6) {
    const uint64_t triangles_size = count ? uint64_t(count - 1) * stride + 8 : 0;
    const uint64_t triangles_access = align4(triangles_size);
    const uint32_t data_size = ((1u << (2 * max_level)) * 2 + 7) / 8 + 17;
    const uint32_t data_access = uint32_t(align4(data_size));
    std::vector<uint8_t> records(size_t(triangles_size), 0xcd), data(data_size);
    std::vector<StorageUsage> usages;
    for (uint32_t i = 0; i < data_size; ++i) data[i] = uint8_t((i * 37u + seed * 19u + 0xa7u) ^ (i >> 3));
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t level = (i + seed) % (max_level + 1);
        const auto format = StorageFormat(1 + ((i ^ seed) & 1));
        const size_t record = size_t(i) * stride;
        put32(records, record, (i % 3) * 3 + 7);
        records[record + 4] = uint8_t(level);
        records[record + 5] = 0;
        records[record + 6] = uint8_t(format);
        records[record + 7] = 0;
        usages.push_back({1, level, format});
    }
    vel::omm::StorageLayout planned{};
    if (storage_plan(count, count ? usages.data() : nullptr, nullptr, &planned) != StorageResult::Success) return false;
    std::vector<uint8_t> expected;
    if (storage_build({records.data(), records.size(), count, stride, data.data(), data.size()}, &expected) !=
            StorageResult::Success ||
        planned.size != expected.size())
        return false;
    constexpr auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    // Inputs are never mapped or read by the host. A compute dispatch generates
    // both triangle records and overlapping packed data ranges on the device.
    auto triangles = fixture.buffer(std::max<uint64_t>(4, triangles_access), usage, false);
    auto states = fixture.buffer(data_access, usage, false);
    auto output = fixture.buffer(expected.size() + 32, usage, true);
    auto clone = fixture.buffer(expected.size() + 32, usage, true);
    auto status = fixture.buffer(4, usage, true);
    GeneratePush generate_push{triangles.address, states.address, count, stride, seed, triangles_access, data_access, max_level};
    BuildPush push{count ? triangles.address : 0,
                   count ? states.address : 0,
                   output.address,
                   status.address,
                   triangles_size,
                   count ? data_size : 0,
                   planned.size,
                   count,
                   stride,
                   triangles_access,
                   count ? data_access : 0,
                   0};
    uint32_t expected_status = 0;
    uint32_t patched = 0;
    uint64_t patch_offset = 0;
    bool patch = false;
    switch (damage) {
        case Damage::None:
            break;
        case Damage::Format:
            patch = true;
            patch_offset = 4;
            patched = 3u << 16;
            expected_status = 1;
            break;
        case Damage::Subdivision:
            patch = true;
            patch_offset = 4;
            patched = (1u << 16) | 32u;
            expected_status = 1;
            break;
        case Damage::DataOffset:
            patch = true;
            patched = UINT32_MAX;
            expected_status = 1;
            break;
        case Damage::TriangleBounds:
            push.triangles_size = 7;
            expected_status = 1;
            break;
        case Damage::DataBounds:
            push.data_size = 0;
            expected_status = 1;
            break;
        case Damage::WordBounds:
            push.data_access_size = data_size;
            expected_status = 1;
            break;
        case Damage::OutputSize:
            push.output_size += 16;
            expected_status = 3;
            break;
        case Damage::CountOverflow:
            push.triangle_count = UINT64_MAX;
            expected_status = 2;
            break;
        case Damage::Stride:
            push.triangle_stride = 7;
            expected_status = 1;
            break;
    }
    auto command = fixture.begin();
    vkCmdFillBuffer(command, output.handle, 0, VK_WHOLE_SIZE, 0xa5a5a5a5u);
    vkCmdFillBuffer(command, clone.handle, 0, VK_WHOLE_SIZE, 0xa5a5a5a5u);
    vkCmdFillBuffer(command, status.handle, 0, VK_WHOLE_SIZE, UINT32_MAX);
    Fixture::barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_WRITE_BIT);
    generate.dispatch(command, &generate_push, sizeof(generate_push), 128);
    if (patch) {
        Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdUpdateBuffer(command, triangles.handle, patch_offset, 4, &patched);
        Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    } else {
        Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    build.dispatch(command, &push, sizeof(push));
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    push.phase = 1;
    build.dispatch(command, &push, sizeof(push), 128);
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferCopy copy{0, 0, expected.size()};
    vkCmdCopyBuffer(command, output.handle, clone.handle, 1, &copy);
    // Explicit execution dependency from build's reads to legal input reuse.
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(command, triangles.handle, 0, VK_WHOLE_SIZE, 0xeeeeeeeeu);
    vkCmdFillBuffer(command, states.handle, 0, VK_WHOLE_SIZE, 0xeeeeeeeeu);
    Fixture::barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                     VK_ACCESS_HOST_READ_BIT);
    const auto before_submit = std::chrono::steady_clock::now();
    fixture.submit(command);
    const double submit_wait_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before_submit).count();
    // The representation and its GPU clone must remain usable after both source
    // inputs are overwritten and their buffers/memory are destroyed.
    release_buffer(fixture, triangles);
    release_buffer(fixture, states);
    const uint32_t actual_status = *static_cast<uint32_t*>(status.mapped);
    const auto* actual = static_cast<const uint8_t*>(output.mapped);
    const auto* cloned = static_cast<const uint8_t*>(clone.mapped);
    bool pass = actual_status == expected_status;
    if (expected_status == 0) {
        pass &= !std::memcmp(actual, expected.data(), expected.size()) && !std::memcmp(cloned, expected.data(), expected.size()) &&
                storage_validate(actual, expected.size()) == StorageResult::Success &&
                storage_validate(cloned, expected.size()) == StorageResult::Success;
    } else {
        pass &= std::all_of(actual, actual + expected.size() + 32, [](uint8_t byte) { return byte == 0xa5; });
    }
    pass &= std::all_of(actual + expected.size(), actual + expected.size() + 32, [](uint8_t byte) { return byte == 0xa5; }) &&
            std::all_of(cloned + expected.size(), cloned + expected.size() + 32, [](uint8_t byte) { return byte == 0xa5; });
    std::printf("%s GPU build count %u stride %u seed %u max level %u %s: status %u expected %u, bytes %zu, submit/wait %.2f ms\n",
                pass ? "PASS" : "FAIL", count, stride, seed, max_level, damage_name(damage), actual_status, expected_status,
                expected.size(), submit_wait_ms);
    release_buffer(fixture, output);
    release_buffer(fixture, clone);
    release_buffer(fixture, status);
    return pass;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: test_omm_gpu_build build.spv generate.spv\n");
        return 1;
    }
    Fixture fixture;
    if (!fixture.open()) {
        std::puts("SKIP: native fixture AS/RQ/BDA/shaderInt64 prerequisites unavailable");
        return 77;
    }
    Program build(fixture, argv[1], sizeof(BuildPush));
    Program generate(fixture, argv[2], sizeof(GeneratePush));
    unsigned failures = 0, cases = 0;
    for (uint32_t count : {0u, 1u, 3u, 17u, 64u}) {
        for (uint32_t seed : {0u, 1u, 5u}) {
            ++cases;
            failures += !run_case(fixture, generate, build, count, seed & 1 ? 20 : 13, seed, Damage::None);
        }
    }
    for (auto damage : {Damage::Format, Damage::Subdivision, Damage::DataOffset, Damage::TriangleBounds, Damage::DataBounds,
                        Damage::WordBounds, Damage::OutputSize, Damage::CountOverflow, Damage::Stride}) {
        ++cases;
        failures += !run_case(fixture, generate, build, 17, 13, 1, damage);
    }
    ++cases;
    failures += !run_case(fixture, generate, build, 26, 13, 0, Damage::None, 12);
    std::printf("OMM GPU canonical build: %u cases, %u failures\n", cases, failures);
    return failures ? 1 : 0;
}
