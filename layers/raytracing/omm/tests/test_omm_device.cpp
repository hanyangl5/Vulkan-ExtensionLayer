// Copyright 2026 The Khronos Group Inc.
// SPDX-License-Identifier: Apache-2.0
#define main omm_gpu_build_fixture_main
#include "test_omm_gpu_build.cpp"
#undef main
#include "../omm_device.h"

namespace {
unsigned assertions = 0;
unsigned native_binds = 0, unbound_queries = 0;
VkBuffer unbound_handle{};
PFN_vkCmdBindPipeline native_bind{};
PFN_vkGetBufferDeviceAddress native_address{};

void require_device(bool condition, const char* reason) {
    ++assertions;
    if (!condition) {
        std::fprintf(stderr, "FAIL OMM device: %s\n", reason);
        std::exit(1);
    }
}
VKAPI_ATTR void VKAPI_CALL spy_bind(VkCommandBuffer command, VkPipelineBindPoint point, VkPipeline pipeline) {
    ++native_binds;
    native_bind(command, point, pipeline);
}
VKAPI_ATTR VkDeviceAddress VKAPI_CALL spy_address(VkDevice device, const VkBufferDeviceAddressInfo* info) {
    if (info->buffer == unbound_handle) ++unbound_queries;
    return native_address(device, info);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL native_proc(VkDevice device, const char* name) {
    if (!std::strcmp(name, "vkCmdBindPipeline")) return reinterpret_cast<PFN_vkVoidFunction>(spy_bind);
    if (!std::strcmp(name, "vkGetBufferDeviceAddress")) return reinterpret_cast<PFN_vkVoidFunction>(spy_address);
    return vkGetDeviceProcAddr(device, name);
}
struct CallbackState {
    size_t allocations = 0, frees = 0;
    bool fail = false;
};
VKAPI_ATTR void* VKAPI_CALL object_allocate(void* user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    auto& state = *static_cast<CallbackState*>(user);
    if (state.fail) return nullptr;
    require_device(scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT && alignment <= alignof(std::max_align_t), "object callback scope");
    void* result = std::malloc(size);
    if (result) ++state.allocations;
    return result;
}
VKAPI_ATTR void* VKAPI_CALL object_reallocate(void*, void* memory, size_t size, size_t, VkSystemAllocationScope) {
    return std::realloc(memory, size);
}
VKAPI_ATTR void VKAPI_CALL object_free(void* user, void* memory) {
    if (memory) {
        ++static_cast<CallbackState*>(user)->frees;
        std::free(memory);
    }
}
std::vector<uint32_t> shader_words(const char* path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require_device(bool(input) && input.tellg() > 0 && input.tellg() % 4 == 0, "shader file");
    const auto size = size_t(input.tellg());
    std::vector<uint32_t> words(size / 4);
    input.seekg(0);
    require_device(bool(input.read(reinterpret_cast<char*>(words.data()), size)), "shader read");
    return words;
}
void track(DeviceBackend* backend, const Buffer& buffer, VkDeviceSize logical, VkDeviceSize accessible, VkBufferUsageFlags flags,
           bool bound = true) {
    auto original = init<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    original.size = logical;
    original.usage = flags;
    CHECK(device_track_buffer(backend, buffer.handle, &original, accessible));
    if (bound) device_buffer_bound(backend, buffer.handle);
}
void submit_reusable(Fixture& fixture, VkCommandBuffer command) {
    auto fence_info = init<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    VkFence fence{};
    CHECK(vkCreateFence(fixture.device, &fence_info, nullptr, &fence));
    auto submit = init<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    CHECK(vkQueueSubmit(fixture.queue, 1, &submit, fence));
    CHECK(vkWaitForFences(fixture.device, 1, &fence, VK_TRUE, UINT64_MAX));
    vkDestroyFence(fixture.device, fence, nullptr);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: test_omm_device build.spv copy.spv generator.spv\n");
        return 1;
    }
    Fixture fixture;
    if (!fixture.open()) return 77;
    const auto build_words = shader_words(argv[1]), copy_words = shader_words(argv[2]);
    native_bind = reinterpret_cast<PFN_vkCmdBindPipeline>(vkGetDeviceProcAddr(fixture.device, "vkCmdBindPipeline"));
    native_address =
        reinterpret_cast<PFN_vkGetBufferDeviceAddress>(vkGetDeviceProcAddr(fixture.device, "vkGetBufferDeviceAddress"));
    DeviceBackend* backend = nullptr;
    DeviceBackendCreateInfo backend_info{fixture.physical,   fixture.device,         native_proc,       nullptr,
                                         build_words.data(), build_words.size() * 4, copy_words.data(), copy_words.size() * 4};
    CHECK(device_backend_create(backend_info, &backend));
    constexpr uint32_t count = 17, stride = 13, seed = 1, max_level = 6;
    const uint64_t triangles_size = (count - 1) * stride + 8;
    const uint64_t triangles_accessible = align4(triangles_size);
    constexpr uint64_t data_size = 1041, data_accessible = 1044;
    std::vector<VkMicromapUsageEXT> usages;
    std::vector<const VkMicromapUsageEXT*> usage_pointers;
    std::vector<uint8_t> records(triangles_size, 0xcd), data(data_size);
    for (uint32_t i = 0; i < data_size; ++i) data[i] = uint8_t((i * 37u + seed * 19u + 0xa7u) ^ (i >> 3));
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t level = (i + seed) % (max_level + 1), format = 1 + ((i ^ seed) & 1);
        usages.push_back({1, level, format});
        put32(records, i * stride, (i % 3) * 3 + 7);
        records[i * stride + 4] = uint8_t(level);
        records[i * stride + 5] = 0;
        records[i * stride + 6] = uint8_t(format);
        records[i * stride + 7] = 0;
    }
    for (const auto& usage : usages) usage_pointers.push_back(&usage);
    std::vector<uint8_t> expected;
    require_device(storage_build({records.data(), records.size(), count, stride, data.data(), data.size()}, &expected) ==
                       StorageResult::Success,
                   "independent CPU canonical oracle");
    auto build_info = init<VkMicromapBuildInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT);
    build_info.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
    build_info.flags = VK_BUILD_MICROMAP_ALLOW_COMPACTION_BIT_EXT;
    build_info.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
    build_info.usageCountsCount = count;
    build_info.pUsageCounts = usages.data();
    build_info.triangleArrayStride = stride;
    auto sizes = init<VkMicromapBuildSizesInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT);
    CHECK(device_get_build_sizes(backend, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build_info, &sizes));
    require_device(sizes.micromapSize == expected.size() && sizes.buildScratchSize == 256 && !sizes.discardable, "size contract");
    auto indirect_info = build_info;
    indirect_info.pUsageCounts = nullptr;
    indirect_info.ppUsageCounts = usage_pointers.data();
    auto indirect_sizes = init<VkMicromapBuildSizesInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT);
    CHECK(device_get_build_sizes(backend, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &indirect_info, &indirect_sizes));
    require_device(indirect_sizes.micromapSize == sizes.micromapSize, "pointer usage form");
    const auto saved_sizes = sizes;
    require_device(device_get_build_sizes(backend, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_HOST_KHR, &build_info, &sizes) ==
                           VK_ERROR_FEATURE_NOT_PRESENT &&
                       sizes.micromapSize == saved_sizes.micromapSize,
                   "host path fail closed");

    constexpr auto native_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const auto input_usage = native_usage | VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT;
    const auto output_usage = native_usage | VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT;
    auto triangles = fixture.buffer(triangles_accessible, native_usage, false);
    auto states = fixture.buffer(data_accessible, native_usage, false);
    auto scratch = fixture.buffer(256, native_usage, false);
    auto source = fixture.buffer(256 + expected.size() + 256 + 32, native_usage, true);
    auto cloned = fixture.buffer(expected.size() + 32, native_usage, true);
    auto compacted = fixture.buffer(expected.size() + 32, native_usage, true);
    track(backend, triangles, triangles_size, triangles_accessible, input_usage);
    track(backend, states, data_size, data_accessible, input_usage);
    track(backend, scratch, 256, 256, native_usage);
    track(backend, source, 256 + expected.size() + 256 + 32, 256 + expected.size() + 256 + 32, output_usage, false);
    track(backend, cloned, expected.size() + 32, expected.size() + 32, output_usage);
    track(backend, compacted, expected.size() + 32, expected.size() + 32, output_usage);
    auto unbound_info = init<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    unbound_info.size = 16;
    unbound_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    CHECK(vkCreateBuffer(fixture.device, &unbound_info, nullptr, &unbound_handle));
    CHECK(device_track_buffer(backend, unbound_handle, &unbound_info));

    CallbackState callback_state;
    VkAllocationCallbacks callbacks{};
    callbacks.pUserData = &callback_state;
    callbacks.pfnAllocation = object_allocate;
    callbacks.pfnReallocation = object_reallocate;
    callbacks.pfnFree = object_free;
    auto create = init<VkMicromapCreateInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT);
    create.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
    create.buffer = source.handle;
    create.offset = 256;
    create.size = expected.size() + 256;  // Deliberate capacity > built active span.
    VkMicromapEXT source_map = VK_NULL_HANDLE;
    callback_state.fail = true;
    require_device(device_create_micromap(backend, &create, &callbacks, &source_map) == VK_ERROR_OUT_OF_HOST_MEMORY && !source_map,
                   "object allocation failure is atomic");
    callback_state.fail = false;
    CHECK(device_create_micromap(backend, &create, &callbacks, &source_map));
    MicromapStorage storage{};
    require_device(device_micromap_storage(backend, source_map, &storage) == VK_ERROR_VALIDATION_FAILED_EXT, "unbound BDA avoided");
    device_buffer_bound(backend, source.handle);
    CHECK(device_micromap_storage(backend, source_map, &storage));
    require_device(storage.address == source.address + 256 && storage.capacity == create.size, "original backing offset preserved");
    create.offset = 0;
    create.size = expected.size();
    create.buffer = cloned.handle;
    VkMicromapEXT clone_map{}, compact_map{};
    CHECK(device_create_micromap(backend, &create, nullptr, &clone_map));
    create.buffer = compacted.handle;
    CHECK(device_create_micromap(backend, &create, nullptr, &compact_map));
    create.createFlags = VK_MICROMAP_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT_EXT;
    VkMicromapEXT rejected{};
    require_device(device_create_micromap(backend, &create, nullptr, &rejected) == VK_ERROR_FEATURE_NOT_PRESENT && !rejected,
                   "capture replay fail closed");

    // Record copies before the source's build is recorded or submitted. The
    // copied active span must come from execution-time GPU data, not host history.
    auto copy_command = fixture.begin();
    vkCmdFillBuffer(copy_command, cloned.handle, 0, VK_WHOLE_SIZE, 0xa5a5a5a5u);
    vkCmdFillBuffer(copy_command, compacted.handle, 0, VK_WHOLE_SIZE, 0xa5a5a5a5u);
    Fixture::barrier(copy_command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    auto copy_info = init<VkCopyMicromapInfoEXT>(VK_STRUCTURE_TYPE_COPY_MICROMAP_INFO_EXT);
    copy_info.src = source_map;
    copy_info.dst = clone_map;
    copy_info.mode = VK_COPY_MICROMAP_MODE_CLONE_EXT;
    CHECK(device_cmd_copy(backend, copy_command, &copy_info));
    Fixture::barrier(copy_command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    copy_info.src = clone_map;
    copy_info.dst = compact_map;
    copy_info.mode = VK_COPY_MICROMAP_MODE_COMPACT_EXT;
    CHECK(device_cmd_copy(backend, copy_command, &copy_info));
    Fixture::barrier(copy_command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                     VK_ACCESS_HOST_READ_BIT);

    Program generate(fixture, argv[3], sizeof(GeneratePush));
    GeneratePush generate_push{triangles.address,    states.address,  count,    stride, seed,
                               triangles_accessible, data_accessible, max_level};
    auto build_command = fixture.begin();
    vkCmdFillBuffer(build_command, source.handle, 0, VK_WHOLE_SIZE, 0xa5a5a5a5u);
    Fixture::barrier(build_command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
    generate.dispatch(build_command, &generate_push, sizeof(generate_push), 128);
    Fixture::barrier(build_command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    build_info.dstMicromap = source_map;
    build_info.data.deviceAddress = states.address;
    build_info.triangleArray.deviceAddress = triangles.address;
    build_info.scratchData.deviceAddress = scratch.address;
    VkMicromapBuildInfoEXT batch[] = {build_info, build_info};
    batch[1].pNext = &create;
    const unsigned previous_binds = native_binds;
    require_device(
        device_cmd_build(backend, build_command, 2, batch) == VK_ERROR_FEATURE_NOT_PRESENT && native_binds == previous_binds,
        "invalid batch records no valid prefix");
    CHECK(device_cmd_build(backend, build_command, 1, &build_info));
    Fixture::barrier(build_command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(build_command, triangles.handle, 0, VK_WHOLE_SIZE, 0xeeeeeeeeu);
    vkCmdFillBuffer(build_command, states.handle, 0, VK_WHOLE_SIZE, 0xeeeeeeeeu);
    fixture.submit(build_command);
    device_untrack_buffer(backend, triangles.handle);
    device_untrack_buffer(backend, states.handle);
    release_buffer(fixture, triangles);
    release_buffer(fixture, states);
    CHECK(vkEndCommandBuffer(copy_command));
    submit_reusable(fixture, copy_command);
    const auto* actual = static_cast<const uint8_t*>(source.mapped);
    require_device(!std::memcmp(actual + 256, expected.data(), expected.size()), "device EXT build equals CPU canonical oracle");
    require_device(std::all_of(actual, actual + 256, [](uint8_t value) { return value == 0xa5; }) &&
                       std::all_of(actual + 256 + expected.size(), actual + 256 + expected.size() + 256 + 32,
                                   [](uint8_t value) { return value == 0xa5; }),
                   "backing prefix/capacity/tail canaries");
    require_device(!std::memcmp(cloned.mapped, expected.data(), expected.size()) &&
                       !std::memcmp(compacted.mapped, expected.data(), expected.size()),
                   "GPU active-span clone and compact");
    require_device(storage_validate(static_cast<uint8_t*>(cloned.mapped), expected.size()) == StorageResult::Success &&
                       storage_validate(static_cast<uint8_t*>(compacted.mapped), expected.size()) == StorageResult::Success,
                   "clones remain self contained after input destruction");
    require_device(
        std::all_of(static_cast<uint8_t*>(cloned.mapped) + expected.size(),
                    static_cast<uint8_t*>(cloned.mapped) + expected.size() + 32, [](uint8_t value) { return value == 0xa5; }),
        "copy respects smaller destination capacity");
    auto empty_info = build_info;
    empty_info.usageCountsCount = 0;
    empty_info.pUsageCounts = nullptr;
    empty_info.data.deviceAddress = 0;
    empty_info.triangleArray.deviceAddress = 0;
    empty_info.triangleArrayStride = 0;
    auto empty_sizes = init<VkMicromapBuildSizesInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT);
    CHECK(device_get_build_sizes(backend, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &empty_info, &empty_sizes));
    require_device(empty_sizes.micromapSize == 64, "empty device build size");
    auto empty_command = fixture.begin();
    CHECK(device_cmd_build(backend, empty_command, 1, &empty_info));
    fixture.submit(empty_command);
    // Replay the exact same copy CB after changing source contents and active
    // size. There is no rerecording, host header read, or cached build size.
    submit_reusable(fixture, copy_command);
    require_device(storage_validate(static_cast<uint8_t*>(cloned.mapped), 64) == StorageResult::Success &&
                       storage_validate(static_cast<uint8_t*>(compacted.mapped), 64) == StorageResult::Success,
                   "GPU active size follows empty rebuild on command-buffer replay");
    require_device(
        std::all_of(static_cast<uint8_t*>(cloned.mapped) + 64, static_cast<uint8_t*>(cloned.mapped) + expected.size() + 32,
                    [](uint8_t value) { return value == 0xa5; }),
        "replayed copy touches only new active span");
    vkFreeCommandBuffers(fixture.device, fixture.pool, 1, &copy_command);
    require_device(unbound_queries == 0, "never query unbound buffer BDA");
    uint8_t public_version[32]{};
    auto version_info = init<VkMicromapVersionInfoEXT>(VK_STRUCTURE_TYPE_MICROMAP_VERSION_INFO_EXT);
    version_info.pVersionData = public_version;
    VkAccelerationStructureCompatibilityKHR compatibility{};
    device_get_compatibility(backend, &version_info, &compatibility);
    require_device(compatibility == VK_ACCELERATION_STRUCTURE_COMPATIBILITY_INCOMPATIBLE_KHR,
                   "public serialization not fabricated");
    device_destroy_micromap(backend, source_map, &callbacks);
    device_destroy_micromap(backend, clone_map, nullptr);
    device_destroy_micromap(backend, compact_map, nullptr);
    device_destroy_micromap(backend, VK_NULL_HANDLE, nullptr);
    require_device(callback_state.allocations == 1 && callback_state.frees == 1, "balanced original object callbacks");
    device_untrack_buffer(backend, unbound_handle);
    vkDestroyBuffer(fixture.device, unbound_handle, nullptr);
    device_backend_destroy(backend);
    std::printf("OMM device backend: %u assertions, GPU build/clone/compact and input lifetime PASS\n", assertions);
    return 0;
}
